"""Hardware tests for the production Home Assistant WSS transport."""

import asyncio
import os
from urllib.parse import urlsplit

import aiohttp
import pytest
import requests


BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
_base_parts = urlsplit(BASE_URL)
HA_URL = os.environ.get(
    "ARCTIC_HA_URL",
    f"https://{_base_parts.hostname}:8443",
)
WS_URL = HA_URL.replace("https://", "wss://") + "/api/v1/events"


def _test_token() -> str:
    response = requests.post(
        f"{BASE_URL}/api/test/ha-token", timeout=10
    )
    if response.status_code == 404:
        pytest.skip("Device firmware does not expose test instrumentation")
    response.raise_for_status()
    return response.json()["token"]


def _fingerprint() -> bytes:
    response = requests.get(
        f"{BASE_URL}/api/test/ha-identity", timeout=10
    )
    response.raise_for_status()
    return bytes.fromhex(response.json()["sha256_fingerprint"])


async def _connect(
    session: aiohttp.ClientSession,
    token: str,
) -> aiohttp.ClientWebSocketResponse:
    return await session.ws_connect(
        WS_URL,
        headers={"Authorization": f"Bearer {token}"},
        ssl=aiohttp.Fingerprint(_fingerprint()),
        timeout=10,
        autoclose=True,
        autoping=True,
    )


async def _connect_with_retry(
    session: aiohttp.ClientSession,
    token: str,
    attempts: int = 5,
    base_delay: float = 0.25,
) -> aiohttp.ClientWebSocketResponse:
    """Connect, tolerating a transient stall on the single-threaded server.

    The device's integration HTTPS server is single-threaded and does not purge
    lingering sockets (``lru_purge_enable=false``), so under a rapid
    connect/disconnect stress pattern a just-closed connection's teardown can
    briefly hog the accept loop and make the *next* handshake miss its 10s
    timeout. Production HA holds a single persistent connection and never does
    this, so a bounded retry with backoff is the correct way to keep the stress
    test from flaking on that transient stall rather than a real fault.
    """
    last_error = None
    for attempt in range(attempts):
        try:
            return await _connect(session, token)
        except (aiohttp.ClientError, asyncio.TimeoutError, OSError) as error:
            last_error = error
            if attempt == attempts - 1:
                break
            await asyncio.sleep(base_delay * (2**attempt))
    raise AssertionError(
        f"WSS connect did not succeed after {attempts} attempts: "
        f"{type(last_error).__name__}: {last_error}"
    ) from last_error


async def _initial_messages(
    websocket: aiohttp.ClientWebSocketResponse,
) -> tuple[dict, dict]:
    hello = await websocket.receive_json(timeout=5)
    message = await websocket.receive_json(timeout=5)
    assert hello["type"] == "hello"
    assert message["type"] == "snapshot"
    return hello, message["snapshot"]


@pytest.mark.asyncio
async def test_websocket_requires_strict_bearer_authentication():
    async with aiohttp.ClientSession() as session:
        with pytest.raises(aiohttp.WSServerHandshakeError) as missing:
            await session.ws_connect(
                WS_URL,
                ssl=aiohttp.Fingerprint(_fingerprint()),
                timeout=10,
            )
        assert missing.value.status == 401

        with pytest.raises(aiohttp.WSServerHandshakeError) as invalid:
            await session.ws_connect(
                WS_URL,
                headers={"Authorization": "Bearer " + ("0" * 64)},
                ssl=aiohttp.Fingerprint(_fingerprint()),
                timeout=10,
            )
        assert invalid.value.status == 401


@pytest.mark.asyncio
async def test_websocket_starts_with_hello_and_coherent_snapshot():
    token = _test_token()
    async with aiohttp.ClientSession() as session:
        websocket = await _connect(session, token)
        try:
            hello, snapshot = await _initial_messages(websocket)
            assert hello["protocol_version"] == 1
            assert hello["device_id"] == snapshot["device_id"]
            assert hello["boot_id"] == snapshot["boot_id"]
            assert hello["revision"] == snapshot["revision"]
        finally:
            await websocket.close()


@pytest.mark.asyncio
async def test_telemetry_only_change_pushes_newer_snapshot():
    token = _test_token()
    async with aiohttp.ClientSession() as session:
        websocket = await _connect(session, token)
        try:
            _, initial = await _initial_messages(websocket)
            original = initial["state"]["temperatures_c"]["tank"]
            changed = 41 if original != 41 else 42
            update = requests.post(
                f"{BASE_URL}/api/test/set-demo-field",
                json={"water_tank_temp": changed},
                timeout=10,
            )
            if update.status_code == 400:
                pytest.skip("Device is not running in demo mode")
            update.raise_for_status()

            pushed = await websocket.receive_json(timeout=5)
            assert pushed["type"] == "snapshot"
            snapshot = pushed["snapshot"]
            assert snapshot["revision"] > initial["revision"]
            assert snapshot["state"]["temperatures_c"]["tank"] == changed
        finally:
            requests.post(
                f"{BASE_URL}/api/test/set-demo-field",
                json={"water_tank_temp": original},
                timeout=10,
            )
            await websocket.close()


@pytest.mark.asyncio
async def test_websocket_survives_heartbeat_interval():
    token = _test_token()
    async with aiohttp.ClientSession() as session:
        websocket = await _connect(session, token)
        await _initial_messages(websocket)
        deadline = asyncio.get_running_loop().time() + 50
        try:
            while asyncio.get_running_loop().time() < deadline:
                try:
                    message = await websocket.receive(timeout=5)
                except asyncio.TimeoutError:
                    continue
                assert message.type not in {
                    aiohttp.WSMsgType.CLOSE,
                    aiohttp.WSMsgType.CLOSED,
                    aiohttp.WSMsgType.ERROR,
                }
            assert not websocket.closed
        finally:
            await websocket.close()


@pytest.mark.asyncio
async def test_token_rotation_disconnects_existing_websocket():
    token = _test_token()
    async with aiohttp.ClientSession() as session:
        websocket = await _connect(session, token)
        await _initial_messages(websocket)
        _test_token()
        message = await websocket.receive(timeout=5)
        assert message.type in {
            aiohttp.WSMsgType.CLOSE,
            aiohttp.WSMsgType.CLOSED,
            aiohttp.WSMsgType.ERROR,
        }


@pytest.mark.asyncio
async def test_websocket_reserves_capacity_for_rest():
    token = _test_token()
    async with aiohttp.ClientSession() as session:
        clients = [await _connect(session, token) for _ in range(3)]
        try:
            for websocket in clients:
                await _initial_messages(websocket)
            # Three persistent WSS clients must still leave the fourth server
            # socket available for REST reconciliation.
            response = requests.get(
                f"{HA_URL}/api/v1/state",
                headers={"Authorization": f"Bearer {token}"},
                verify=False,
                timeout=10,
            )
            response.raise_for_status()
        finally:
            for websocket in clients:
                await websocket.close()


@pytest.mark.asyncio
async def test_repeated_connect_disconnect_cycles():
    token = _test_token()
    async with aiohttp.ClientSession() as session:
        for _ in range(20):
            websocket = await _connect_with_retry(session, token)
            await _initial_messages(websocket)
            await websocket.close()
            assert websocket.close_code == 1000
            # Yield briefly so the single-threaded server can finish tearing
            # down the just-closed socket before the next cycle reconnects.
            await asyncio.sleep(0.05)


@pytest.mark.asyncio
async def test_unexpected_application_frame_disconnects_client():
    token = _test_token()
    async with aiohttp.ClientSession() as session:
        websocket = await _connect(session, token)
        await _initial_messages(websocket)
        await websocket.send_str("unexpected")
        message = await websocket.receive(timeout=5)
        assert message.type in {
            aiohttp.WSMsgType.CLOSE,
            aiohttp.WSMsgType.CLOSED,
            aiohttp.WSMsgType.ERROR,
        }
