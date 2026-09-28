"""The Home Assistant client library (pymacon) against this firmware.

The other Home Assistant API tests check the wire format by hand. These go
through pymacon itself, the library the Home Assistant integration uses, so
a response it no longer understands fails here instead of in someone's Home
Assistant. pymacon parses health fields leniently (a missing or renamed field
becomes None rather than an error), which is why the fields are checked
explicitly.
"""

import asyncio
import os
from dataclasses import fields
from urllib.parse import urlsplit

import aiohttp
import pytest
import requests
from pymacon import ControllerDiagnostics, MaconClient

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
HOST = urlsplit(BASE_URL).hostname

BUS_ROLES = {"master", "listener", "blocked", "demo", "inactive"}

# Reported whatever the controller is doing.
ALWAYS_REPORTED = (
    "uptime_ms",
    "last_reset_reason",
    "brownout_count",
    "panic_count",
    "watchdog_count",
    "crash_streak",
    "safe_mode",
    "internal_free_bytes",
    "internal_min_free_bytes",
    "internal_largest_free_block_bytes",
    "wifi_connected",
    "wifi_disconnect_count",
    "time_synced",
    "ota_busy",
    "ota_pending_verify",
    "bus_role",
)
# Only while driving the bus (and last_ok only once the heat pump answered).
MASTER_COUNTERS = (
    "bus_polls_ok",
    "bus_polls_no_response",
    "bus_polls_transport_error",
    "bus_checksum_errors",
    "bus_consecutive_failures",
    "bus_writes_ok",
    "bus_writes_failed",
)
LISTENER_COUNTERS = ("bus_frames_ok", "bus_resyncs")


def _credentials() -> tuple[str, str]:
    response = requests.post(f"{BASE_URL}/api/test/ha-token", timeout=10)
    if response.status_code == 404:
        pytest.skip("Device firmware does not expose test instrumentation")
    response.raise_for_status()
    token = response.json()["token"]
    response = requests.get(f"{BASE_URL}/api/test/ha-identity", timeout=10)
    response.raise_for_status()
    return token, response.json()["sha256_fingerprint"]


def _missing(diag: ControllerDiagnostics, names: tuple[str, ...]) -> list[str]:
    return [name for name in names if getattr(diag, name) is None]


@pytest.mark.asyncio
async def test_home_assistant_client_understands_the_controller():
    token, fingerprint = _credentials()
    async with aiohttp.ClientSession() as session:
        client = MaconClient(HOST, token, fingerprint, session=session)
        try:
            snapshot = await client.start()
            capabilities = client.capabilities
            assert capabilities is not None
            assert capabilities.device_id == snapshot.device_id
            assert capabilities.diagnostics, "controller must offer diagnostics"
            assert capabilities.supported_modes, "no supported modes"
            for name in ("cooling_range", "heating_range", "hot_water_range"):
                limits = getattr(capabilities, name)
                assert limits.minimum < limits.maximum, name

            # Live updates reach Home Assistant over the push stream.
            async with asyncio.timeout(10):
                while not client.stream_connected:
                    await asyncio.sleep(0.1)

            diag = await client.async_fetch_diagnostics()
            assert diag.device_id == snapshot.device_id
            assert diag.boot_id == snapshot.boot_id
            assert _missing(diag, ALWAYS_REPORTED) == []
            assert diag.bus_role in BUS_ROLES, diag.bus_role
            if diag.wifi_connected:
                assert _missing(diag, ("wifi_ssid", "wifi_rssi_dbm")) == []
            if diag.bus_role == "master":
                assert _missing(diag, MASTER_COUNTERS) == []
                if diag.bus_polls_ok:
                    assert diag.bus_last_ok_uptime_ms is not None
                    assert diag.bus_last_ok_uptime_ms <= diag.uptime_ms
            if diag.bus_role == "listener":
                assert _missing(diag, LISTENER_COUNTERS) == []

            ota = await client.async_ota_status()
            assert ota.state, "OTA state missing"
        finally:
            await client.stop()


def test_expected_fields_exist_in_pymacon():
    """Guard the lists above against a pymacon rename."""
    names = {field.name for field in fields(ControllerDiagnostics)}
    expected = {*ALWAYS_REPORTED, *MASTER_COUNTERS, *LISTENER_COUNTERS}
    assert expected <= names, expected - names
