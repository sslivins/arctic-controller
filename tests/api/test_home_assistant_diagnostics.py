"""Home Assistant controller diagnostics and restart (/api/v1/diagnostics,
/api/v1/control/restart) on the integration server."""

import gc
import os
import time
import uuid
from urllib.parse import urlsplit

import pytest
import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
_base_parts = urlsplit(BASE_URL)
HA_URL = os.environ.get(
    "ARCTIC_HA_URL",
    f"https://{_base_parts.hostname}:8443",
)
_session = requests.Session()
_session.headers["Connection"] = "close"
_session.mount("http://", HTTPAdapter(max_retries=Retry(total=3, backoff_factor=1)))
_session.mount("https://", HTTPAdapter(max_retries=Retry(total=3, backoff_factor=1)))
_session.verify = False

RESET_REASONS = {
    "power_on", "external", "software", "panic", "interrupt_wdt", "task_wdt",
    "other_wdt", "deep_sleep", "brownout", "sdio", "usb", "jtag",
    "efuse_error", "power_glitch", "cpu_lockup", "unknown",
}
MASTER_COUNTERS = {
    "polls_ok", "polls_no_response", "polls_transport_error",
    "checksum_errors", "consecutive_failures", "writes_ok", "writes_failed",
}
LISTENER_COUNTERS = {"frames_ok", "checksum_errors", "resyncs"}


@pytest.fixture(autouse=True, scope="module")
def _release_pooled_connections():
    # esp_http_server keeps an idle keep-alive socket open for as long as the
    # client holds it, and the integration server has only 5 slots with no
    # LRU eviction. After the restart test, urllib3 pools orphaned by the
    # reboot stay reachable from response/exception reference cycles, so their
    # sockets survive session.close() until the cyclic GC runs - and starved
    # test_websocket_reserves_capacity_for_rest (run 36206822808).
    yield
    _session.close()
    gc.collect()


def _issue_test_token() -> str:
    response = _session.post(f"{BASE_URL}/api/test/ha-token", timeout=10)
    if response.status_code == 404:
        pytest.skip("Device firmware does not expose test instrumentation")
    response.raise_for_status()
    return response.json()["token"]


def _headers(token: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


def _diagnostics(token: str) -> dict:
    response = _session.get(
        f"{HA_URL}/api/v1/diagnostics", headers=_headers(token), timeout=10
    )
    response.raise_for_status()
    return response.json()


def _restart(token: str, boot_id, **extra) -> requests.Response:
    body = {"command_id": uuid.uuid4().hex, "boot_id": boot_id, **extra}
    return _session.post(
        f"{HA_URL}/api/v1/control/restart",
        headers=_headers(token),
        json=body,
        timeout=10,
    )


def _is_count(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def test_capabilities_advertise_diagnostics_and_restart():
    token = _issue_test_token()
    response = _session.get(
        f"{HA_URL}/api/v1/capabilities", headers=_headers(token), timeout=10
    )
    response.raise_for_status()
    capabilities = response.json()["capabilities"]
    assert capabilities["diagnostics"] is True
    assert capabilities["restart"] is True


def test_diagnostics_require_the_integration_token():
    response = _session.get(f"{HA_URL}/api/v1/diagnostics", timeout=10)
    assert response.status_code == 401


def test_diagnostics_report_controller_health():
    token = _issue_test_token()
    data = _diagnostics(token)
    state = _session.get(
        f"{HA_URL}/api/v1/state", headers=_headers(token), timeout=10
    ).json()

    assert data["protocol_version"] == 1
    assert data["device_id"] == state["device_id"]
    assert data["boot_id"] == state["boot_id"]
    diag = data["diagnostics"]

    assert _is_count(diag["uptime_ms"]) and diag["uptime_ms"] > 0

    system = diag["system"]
    assert system["last_reset_reason"] in RESET_REASONS
    for key in ("brownout_count", "panic_count", "watchdog_count", "crash_streak"):
        assert _is_count(system[key]), key
    assert isinstance(system["safe_mode"], bool)

    memory = diag["memory"]
    assert 0 < memory["internal_min_free_bytes"] <= memory["internal_free_bytes"]
    assert 0 < memory["internal_largest_free_block_bytes"] <= memory["internal_free_bytes"]

    wifi = diag["wifi"]
    # The test reached us over WiFi, so the link is up right now.
    assert wifi["connected"] is True
    assert isinstance(wifi["ssid"], str) and wifi["ssid"]
    assert isinstance(wifi["rssi_dbm"], int) and -127 <= wifi["rssi_dbm"] < 0
    assert _is_count(wifi["disconnect_count"])
    assert wifi["last_disconnect_reason"] is None or _is_count(
        wifi["last_disconnect_reason"]
    )

    assert isinstance(diag["time"]["synced"], bool)
    assert isinstance(diag["ota"]["busy"], bool)
    assert isinstance(diag["ota"]["pending_verify"], bool)

    rs485 = diag["rs485"]
    role = rs485["role"]
    assert role in {"master", "listener", "blocked", "demo", "inactive"}
    if role == "master":
        expected = MASTER_COUNTERS
    elif role == "listener":
        expected = LISTENER_COUNTERS
    else:
        expected = set()
    assert set(rs485) - {"role", "last_ok_uptime_ms"} == expected
    for key in expected:
        assert _is_count(rs485[key]), key
    if role in ("master", "listener"):
        last_ok = rs485["last_ok_uptime_ms"]
        assert last_ok is None or 0 <= last_ok <= diag["uptime_ms"]


def test_diagnostics_uptime_is_monotonic_within_a_boot():
    token = _issue_test_token()
    first = _diagnostics(token)
    second = _diagnostics(token)
    assert first["boot_id"] == second["boot_id"]
    assert second["diagnostics"]["uptime_ms"] >= first["diagnostics"]["uptime_ms"]


def test_state_snapshot_excludes_diagnostics():
    """Diagnostics must stay out of the revisioned state snapshot, or every
    uptime/heap tick would push a new snapshot to Home Assistant."""
    token = _issue_test_token()
    snapshot = _session.get(
        f"{HA_URL}/api/v1/state", headers=_headers(token), timeout=10
    ).json()
    assert "diagnostics" not in snapshot
    assert "uptime_ms" not in snapshot["state"]


def test_restart_requires_the_integration_token():
    response = _session.post(
        f"{HA_URL}/api/v1/control/restart",
        json={"command_id": "x", "boot_id": "y"},
        timeout=10,
    )
    assert response.status_code == 401


@pytest.mark.parametrize(
    "body",
    [
        {},
        {"command_id": "abc"},
        {"boot_id": "abc"},
        {"command_id": "abc", "boot_id": 7},
        {"command_id": "", "boot_id": "abc"},
        {"command_id": "abc", "boot_id": "abc", "force": True},
    ],
)
def test_restart_rejects_malformed_bodies(body):
    token = _issue_test_token()
    response = _session.post(
        f"{HA_URL}/api/v1/control/restart",
        headers=_headers(token),
        json=body,
        timeout=10,
    )
    assert response.status_code == 422
    assert response.headers["Content-Type"].startswith("application/json")


def test_restart_rejects_a_stale_boot_id():
    token = _issue_test_token()
    boot_id = _diagnostics(token)["boot_id"]
    response = _restart(token, "not-" + boot_id)
    assert response.status_code == 409
    # Nothing restarted.
    assert _diagnostics(token)["boot_id"] == boot_id


def _wait_for_new_boot(old_boot_id: str, timeout: float) -> dict:
    """Wait for the controller to come back on a different boot. Each attempt
    is a real HTTPS round trip (connect timeouts pace it while the device is
    down), so no fixed sleep is needed."""
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            with requests.Session() as probe:
                probe.verify = False
                token = probe.post(
                    f"{BASE_URL}/api/test/ha-token", timeout=5
                ).json()["token"]
                response = probe.get(
                    f"{HA_URL}/api/v1/diagnostics",
                    headers=_headers(token),
                    timeout=5,
                )
                response.raise_for_status()
                data = response.json()
            if data["boot_id"] != old_boot_id:
                return data
        except (requests.RequestException, ValueError, KeyError) as exc:
            # Keep only the text: the exception's traceback pins the failed
            # attempt's socket open until the cyclic GC happens to run.
            last_error = repr(exc)
    pytest.fail(
        f"controller did not come back on a new boot within {timeout}s "
        f"(last error: {last_error})"
    )


def test_restart_reboots_into_a_new_boot():
    token = _issue_test_token()
    before = _diagnostics(token)
    if before["diagnostics"]["ota"]["busy"] or before["diagnostics"]["ota"]["pending_verify"]:
        pytest.fail("restart precondition: OTA must be idle and committed")

    response = _restart(token, before["boot_id"])
    assert response.status_code == 202
    body = response.json()
    assert body["accepted"] is True
    assert body["status"] == "restarting"

    after = _wait_for_new_boot(before["boot_id"], timeout=120)
    assert after["device_id"] == before["device_id"]
    assert after["diagnostics"]["system"]["last_reset_reason"] == "software"
    # uptime restarted from zero (bounded by the wait plus request slack).
    assert after["diagnostics"]["uptime_ms"] < 150_000
    # The reboot killed every pooled keep-alive socket, and urllib3 never
    # retries a POST on a dead one, so reconnect before the retry.
    _session.close()
    # A retry of the same request after the reboot must not restart again.
    retry = _restart(_issue_test_token(), before["boot_id"])
    assert retry.status_code == 409
