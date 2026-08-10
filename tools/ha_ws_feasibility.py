"""Exercise the controller WebSocket socket/latency feasibility gate."""

from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import time
import urllib.request

import websockets


def fetch_json(url: str) -> tuple[dict, float]:
    started = time.perf_counter()
    with urllib.request.urlopen(url, timeout=3) as response:
        payload = json.load(response)
    return payload, (time.perf_counter() - started) * 1000


async def probe_rest(url: str, duration: float) -> tuple[list[float], int]:
    latencies: list[float] = []
    failures = 0
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        try:
            _, latency = await asyncio.to_thread(fetch_json, url)
            latencies.append(latency)
        except Exception:
            failures += 1
        await asyncio.sleep(0.05)
    return latencies, failures


async def verify_client(client: websockets.ClientConnection, size: int = 4096) -> None:
    await client.send(f"payload:{size}")
    payload = await asyncio.wait_for(client.recv(), timeout=3)
    if not isinstance(payload, bytes) or len(payload) != size:
        raise RuntimeError(f"Unexpected WebSocket payload: {type(payload)} {len(payload)}")


async def flood_without_reading(
    client: websockets.ClientConnection, duration: float
) -> int:
    sent = 0
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        try:
            await asyncio.wait_for(client.send("payload:8192"), timeout=2)
            sent += 1
        except Exception:
            break
    return sent


def summarize(latencies: list[float]) -> dict:
    if not latencies:
        return {"count": 0}
    ordered = sorted(latencies)
    p95_index = min(len(ordered) - 1, int(len(ordered) * 0.95))
    return {
        "count": len(ordered),
        "median_ms": round(statistics.median(ordered), 1),
        "p95_ms": round(ordered[p95_index], 1),
        "max_ms": round(max(ordered), 1),
    }


async def run(args: argparse.Namespace) -> dict:
    info_before, _ = await asyncio.to_thread(fetch_json, f"{args.http}/api/info")
    healthy_clients = [
        await websockets.connect(
            args.ws,
            open_timeout=5,
            close_timeout=1,
            ping_interval=None,
            max_size=16384,
        )
        for _ in range(2)
    ]

    try:
        for client in healthy_clients:
            await verify_client(client)

        baseline_latencies, baseline_failures = await probe_rest(
            f"{args.http}/api/health", args.baseline_seconds
        )

        slow_client = await websockets.connect(
            args.ws,
            open_timeout=5,
            close_timeout=1,
            ping_interval=None,
            max_size=16384,
        )
        try:
            flood_task = asyncio.create_task(
                flood_without_reading(slow_client, args.stress_seconds)
            )
            stress_latencies, stress_failures = await probe_rest(
                f"{args.http}/api/health", args.stress_seconds
            )
            flood_sent = await flood_task
        finally:
            await slow_client.close()

        for client in healthy_clients:
            await verify_client(client)

        info_after, _ = await asyncio.to_thread(fetch_json, f"{args.http}/api/info")
    finally:
        for client in healthy_clients:
            await client.close()

    result = {
        "baseline_rest": summarize(baseline_latencies),
        "baseline_failures": baseline_failures,
        "stalled_client_rest": summarize(stress_latencies),
        "stalled_client_failures": stress_failures,
        "stalled_client_commands_sent": flood_sent,
        "free_heap_before": info_before.get("free_heap"),
        "free_heap_after": info_after.get("free_heap"),
        "min_free_heap_after": info_after.get("min_free_heap"),
    }
    result["passed"] = (
        baseline_failures == 0
        and stress_failures == 0
        and result["stalled_client_rest"].get("max_ms", 9999) < 1500
        and result["stalled_client_rest"].get("p95_ms", 9999) < 500
    )
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--http", default="http://192.168.1.21")
    parser.add_argument(
        "--ws", default="ws://192.168.1.21/api/test/ws-feasibility"
    )
    parser.add_argument("--baseline-seconds", type=float, default=5)
    parser.add_argument("--stress-seconds", type=float, default=15)
    args = parser.parse_args()
    result = asyncio.run(run(args))
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
