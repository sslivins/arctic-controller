"""Stress the production Home Assistant WSS transport on physical hardware."""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
import os
import socket
import ssl
import statistics
import time
from urllib.parse import urlsplit

import aiohttp
import requests


def summarize(values: list[float]) -> dict:
    if not values:
        return {"count": 0}
    ordered = sorted(values)
    p95 = ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))]
    return {
        "count": len(ordered),
        "median_ms": round(statistics.median(ordered), 1),
        "p95_ms": round(p95, 1),
        "max_ms": round(max(ordered), 1),
    }


def issue_token(http_url: str) -> str:
    response = requests.post(
        http_url + "/api/test/ha-token", timeout=10
    )
    response.raise_for_status()
    return response.json()["token"]


def get_fingerprint(http_url: str) -> bytes:
    response = requests.get(
        http_url + "/api/test/ha-identity", timeout=10
    )
    response.raise_for_status()
    return bytes.fromhex(response.json()["sha256_fingerprint"])


def fetch_info(http_url: str) -> dict:
    response = requests.get(http_url + "/api/info", timeout=10)
    return response.json() if response.ok else {}

def open_stalled_client(
    wss_url: str,
    token: str,
    fingerprint: bytes,
) -> ssl.SSLSocket:
    parts = urlsplit(wss_url)
    raw = socket.create_connection(
        (parts.hostname, parts.port or 8443), timeout=10
    )
    raw.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    tls = context.wrap_socket(raw, server_hostname=parts.hostname)
    actual = hashlib.sha256(tls.getpeercert(binary_form=True)).digest()
    if actual != fingerprint:
        tls.close()
        raise RuntimeError("TLS fingerprint mismatch")

    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET {parts.path} HTTP/1.1\r\n"
        f"Host: {parts.hostname}:{parts.port or 8443}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Authorization: Bearer {token}\r\n\r\n"
    )
    tls.sendall(request.encode("ascii"))

    header = bytearray()
    while not header.endswith(b"\r\n\r\n"):
        chunk = tls.recv(1)
        if not chunk:
            tls.close()
            raise RuntimeError("WSS handshake closed unexpectedly")
        header.extend(chunk)
        if len(header) > 4096:
            tls.close()
            raise RuntimeError("WSS handshake header too large")
    if not header.startswith(b"HTTP/1.1 101"):
        tls.close()
        raise RuntimeError(header.decode(errors="replace"))
    return tls


async def connect_healthy(
    session: aiohttp.ClientSession,
    wss_url: str,
    token: str,
    fingerprint: bytes,
) -> aiohttp.ClientWebSocketResponse:
    websocket = await session.ws_connect(
        wss_url,
        headers={"Authorization": f"Bearer {token}"},
        ssl=aiohttp.Fingerprint(fingerprint),
        autoclose=True,
        autoping=True,
    )
    hello = await websocket.receive_json(timeout=5)
    snapshot = await websocket.receive_json(timeout=5)
    if hello.get("type") != "hello" or snapshot.get("type") != "snapshot":
        await websocket.close()
        raise RuntimeError("Unexpected initial WSS message sequence")
    return websocket


async def receive_snapshots(
    websocket: aiohttp.ClientWebSocketResponse,
    duration: float,
) -> tuple[int, int, list[str], list[float]]:
    deadline = time.monotonic() + duration
    count = 0
    idle_timeouts = 0
    failures: list[str] = []
    latencies: list[float] = []
    last_revision = 0
    while time.monotonic() < deadline:
        started = time.perf_counter()
        try:
            message = await websocket.receive_json(timeout=3)
            if message.get("type") != "snapshot":
                continue
            revision = int(message["snapshot"]["revision"])
            if revision <= last_revision:
                failures.append(
                    f"non-increasing revision {revision} after "
                    f"{last_revision}"
                )
            last_revision = revision
            count += 1
            latencies.append((time.perf_counter() - started) * 1000)
        except asyncio.TimeoutError:
            idle_timeouts += 1
        except Exception as error:
            failures.append(f"{type(error).__name__}: {error}")
            break
    return count, idle_timeouts, failures, latencies


async def toggle_telemetry(
    session: aiohttp.ClientSession,
    http_url: str,
    duration: float,
) -> int:
    deadline = time.monotonic() + duration
    changes = 0
    value = 40
    while time.monotonic() < deadline:
        value = 41 if value == 40 else 40
        async with session.post(
            http_url + "/api/test/set-demo-field",
            json={"water_tank_temp": value},
            timeout=aiohttp.ClientTimeout(total=5),
        ) as response:
            await response.read()
            if response.status != 200:
                raise RuntimeError(
                    f"Demo update failed: {response.status}"
                )
        changes += 1
        await asyncio.sleep(0.08)
    return changes


async def probe_url(
    session: aiohttp.ClientSession,
    url: str,
    duration: float,
    *,
    headers: dict | None = None,
    ssl_value=None,
    interval: float = 0.1,
) -> tuple[list[float], list[str]]:
    deadline = time.monotonic() + duration
    latencies: list[float] = []
    failures: list[str] = []
    while time.monotonic() < deadline:
        started = time.perf_counter()
        try:
            async with session.get(
                url,
                headers=headers,
                ssl=ssl_value,
                timeout=aiohttp.ClientTimeout(total=5),
            ) as response:
                await response.read()
                if response.status != 200:
                    raise RuntimeError(f"HTTP {response.status}")
            latencies.append((time.perf_counter() - started) * 1000)
        except Exception as error:
            failures.append(f"{type(error).__name__}: {error}")
        await asyncio.sleep(interval)
    return latencies, failures


async def run(args: argparse.Namespace) -> dict:
    token = issue_token(args.http)
    fingerprint = get_fingerprint(args.http)
    info_before = fetch_info(args.http)
    stalled = await asyncio.to_thread(
        open_stalled_client,
        args.wss,
        token,
        fingerprint,
    )

    connector = aiohttp.TCPConnector(force_close=True)
    async with aiohttp.ClientSession(connector=connector) as session:
        healthy = [
            await connect_healthy(
                session, args.wss, token, fingerprint
            )
            for _ in range(2)
        ]
        try:
            tasks = [
                asyncio.create_task(
                    receive_snapshots(
                        websocket, args.duration
                    )
                )
                for websocket in healthy
            ]
            producer = asyncio.create_task(
                toggle_telemetry(session, args.http, args.duration)
            )
            health = asyncio.create_task(
                probe_url(
                    session,
                    args.http + "/api/health",
                    args.duration,
                    interval=args.http_interval,
                )
            )
            web_ui = asyncio.create_task(
                probe_url(
                    session,
                    args.http + "/",
                    args.duration,
                    interval=args.http_interval,
                )
            )
            rest = asyncio.create_task(
                probe_url(
                    session,
                    args.https + "/api/v1/state",
                    args.duration,
                    headers={
                        "Authorization": f"Bearer {token}"
                    },
                    ssl_value=aiohttp.Fingerprint(fingerprint),
                    interval=args.rest_interval,
                )
            )

            healthy_results = await asyncio.gather(*tasks)
            changes = await producer
            health_latencies, health_errors = await health
            web_latencies, web_errors = await web_ui
            rest_latencies, rest_errors = await rest

            stalled_disconnected = False
            healthy_still_open = all(
                not websocket.closed for websocket in healthy
            )
            if healthy_still_open:
                try:
                    replacement = await connect_healthy(
                        session, args.wss, token, fingerprint
                    )
                    stalled_disconnected = True
                    await replacement.close()
                except aiohttp.WSServerHandshakeError as error:
                    if error.status != 503:
                        raise
        finally:
            for websocket in healthy:
                await websocket.close()
            stalled.close()
            try:
                async with session.post(
                    args.http + "/api/test/set-demo-field",
                    json={"water_tank_temp": 42},
                    timeout=aiohttp.ClientTimeout(total=5),
                ):
                    pass
            except Exception:
                pass

    await asyncio.sleep(1)
    info_after = fetch_info(args.http)
    result = {
        "duration_seconds": args.duration,
        "telemetry_changes": changes,
        "healthy_clients": [
            {
                "snapshots": count,
                "idle_timeouts": idle_timeouts,
                "errors": errors,
                "receive_wait": summarize(latencies),
            }
            for count, idle_timeouts, errors, latencies in healthy_results
        ],
        "http_health": summarize(health_latencies),
        "http_health_errors": health_errors,
        "web_ui": summarize(web_latencies),
        "web_ui_errors": web_errors,
        "integration_rest": summarize(rest_latencies),
        "integration_rest_errors": rest_errors,
        "healthy_clients_still_open": healthy_still_open,
        "stalled_client_disconnected": stalled_disconnected,
        "free_heap_before": info_before.get("free_heap"),
        "free_heap_after": info_after.get("free_heap"),
        "min_free_heap_after": info_after.get("min_free_heap"),
    }
    heap_before = result["free_heap_before"]
    heap_after = result["free_heap_after"]
    min_heap = result["min_free_heap_after"]
    heap_healthy = (
        isinstance(heap_before, int)
        and isinstance(heap_after, int)
        and isinstance(min_heap, int)
        and heap_after >= heap_before - 64 * 1024
        and min_heap >= heap_before - 2 * 1024 * 1024
    )
    result["passed"] = (
        all(not item["errors"] for item in result["healthy_clients"])
        and all(
            item["snapshots"] >= args.duration * 0.5
            for item in result["healthy_clients"]
        )
        and all(
            item["receive_wait"].get("max_ms", 9999) < 5000
            for item in result["healthy_clients"]
        )
        and not health_errors
        and not web_errors
        and not rest_errors
        and healthy_still_open
        and stalled_disconnected
        and result["http_health"].get("max_ms", 9999) < 1500
        and result["web_ui"].get("max_ms", 9999) < 1500
        and result["integration_rest"].get("max_ms", 9999) < 1500
        and heap_healthy
    )
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--http", default="http://192.168.1.21"
    )
    parser.add_argument(
        "--https", default="https://192.168.1.21:8443"
    )
    parser.add_argument(
        "--wss", default="wss://192.168.1.21:8443/api/v1/events"
    )
    parser.add_argument("--duration", type=float, default=60)
    parser.add_argument("--http-interval", type=float, default=1.0)
    parser.add_argument("--rest-interval", type=float, default=1.0)
    args = parser.parse_args()
    result = asyncio.run(run(args))
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
