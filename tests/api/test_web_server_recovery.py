"""Web/API servers survive a WiFi drop and recover if they ever go down.

Field failure (3.3.0): the controller stopped its servers on every WiFi
disconnect and restarted them on reconnect. When internal RAM was too
fragmented for a server task stack the restart failed and was never retried,
so the controller stayed pingable but unmanageable until it was power-cycled.

These tests drive the real recovery paths through test-firmware hooks:
  - POST /api/test/wifi-bounce drops and re-joins WiFi; the servers must keep
    running across it (no reboot, HTTPS and the HA server reachable).
  - POST /api/test/api-server-kill stops the servers behind the supervisor's
    back, optionally making the next N start attempts fail. The supervisor
    must restart them in place, or reboot when restarts keep failing.

They are destructive (the device is offline for up to a few minutes); the
reboot case is last in this module.
"""

import os
import time
from urllib.parse import urlsplit

import pytest
import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")
HA_URL = os.environ.get(
    "ARCTIC_HA_URL", f"https://{urlsplit(BASE_URL).hostname}:8443"
)

# Supervisor timing (main/wifi_supervisor.cpp): checks every 15 s and reboots
# after 90 s of failed restarts. Generous slack on top.
RESTART_TIMEOUT_S = 90
REBOOT_TIMEOUT_S = 240


def _session(paced: bool) -> requests.Session:
    """A fresh, non-pooled session. ``paced`` retries refused connections with
    backoff, so polling a device whose servers are down waits on the network
    instead of spinning (refused connects return instantly)."""
    s = requests.Session()
    s.verify = False
    s.headers["Connection"] = "close"
    if API_KEY:
        s.headers["X-API-Key"] = API_KEY
    if paced:
        retry = Retry(total=2, connect=2, read=0, status=0, other=0,
                      backoff_factor=1, allowed_methods=None)
        s.mount("http://", HTTPAdapter(max_retries=retry))
        s.mount("https://", HTTPAdapter(max_retries=retry))
    return s


def _events(paced: bool = True, limit: int = 50):
    """The events page, or None while the API is unreachable."""
    try:
        with _session(paced) as s:
            r = s.get(f"{BASE_URL}/api/events?limit={limit}", timeout=5)
        r.raise_for_status()
        return r.json()
    except (requests.RequestException, ValueError):
        return None


def _post(path: str, json=None) -> requests.Response:
    with _session(paced=False) as s:
        return s.post(f"{BASE_URL}{path}", json=json, timeout=10)


def _ha_reachable() -> bool:
    """The HA integration server answers (401 without a token is fine)."""
    try:
        with _session(paced=True) as s:
            r = s.get(f"{HA_URL}/api/v1/capabilities", timeout=5)
        return r.status_code in (200, 401, 403)
    except requests.RequestException:
        return False


def _wait_for(predicate, description: str, timeout: float):
    """Poll until predicate() is truthy. Every probe is a network round trip
    with its own timeout/backoff, which paces the loop."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
    pytest.fail(f"timed out after {timeout}s waiting for {description}")


def _web_server_events(data: dict, boot_id: int, since_uptime_ms: int) -> list:
    return [
        e for e in data["events"]
        if e["type"] in ("web_server_down", "web_server_recovered")
        and e["boot_id"] == boot_id
        and e["uptime_ms"] > since_uptime_ms
    ]


def _snapshot() -> tuple[int, int]:
    """(current boot id, uptime of its newest event)."""
    data = _events()
    assert data, "device API unreachable before the test"
    boot = data["current_boot_id"]
    newest = max((e["uptime_ms"] for e in data["events"] if e["boot_id"] == boot),
                 default=0)
    return boot, newest


@pytest.fixture(scope="module", autouse=True)
def _recovery_hooks():
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    with _session(paced=False) as s:
        probe = s.get(f"{BASE_URL}/api/test/wifi-bounce", timeout=10)
    if probe.status_code == 404:
        pytest.skip("Device firmware does not expose test instrumentation")
    yield
    # Leave the device reachable for whatever runs next, even after a failure.
    _wait_for(_events, "the device to answer after the recovery tests",
              REBOOT_TIMEOUT_S)


def test_servers_survive_a_wifi_bounce():
    boot_id, since = _snapshot()

    r = _post("/api/test/wifi-bounce")
    assert r.status_code == 202, r.text

    # The link really dropped: unpaced probes start timing out.
    _wait_for(lambda: _events(paced=False) is None, "WiFi to drop", 20)
    data = _wait_for(_events, "the API to answer after the bounce", 60)

    assert data["current_boot_id"] == boot_id, "the bounce must not reboot"
    assert _wait_for(_ha_reachable, "the HA server (8443) after the bounce", 30)
    downs = [e for e in _web_server_events(data, boot_id, since)
             if e["type"] == "web_server_down"]
    assert not downs, f"servers went down across a WiFi bounce: {downs}"


def test_killed_servers_are_restarted_in_place():
    boot_id, since = _snapshot()

    # The first restart attempt fails (injected); the next one succeeds.
    r = _post("/api/test/api-server-kill", json={"fail_starts": 1})
    assert r.status_code == 202, r.text
    _wait_for(lambda: _events(paced=False) is None, "the servers to stop", 15)

    def recovered():
        data = _events()
        if not data:
            return None
        evts = _web_server_events(data, boot_id, since)
        if any(e["type"] == "web_server_recovered" for e in evts):
            return data, evts
        return None

    data, evts = _wait_for(recovered, "web_server_recovered", RESTART_TIMEOUT_S)
    assert data["current_boot_id"] == boot_id, "an in-place restart must not reboot"

    down = [e for e in evts if e["type"] == "web_server_down"]
    up = [e for e in evts if e["type"] == "web_server_recovered"]
    assert len(down) == 1, evts
    assert down[0]["category"] == "problems"
    assert down[0]["cause"] == "test"
    assert up[0]["recovery"] == "restarted"
    assert up[0]["uptime_ms"] >= down[0]["uptime_ms"]
    assert _wait_for(_ha_reachable, "the HA server (8443) after the restart", 30)


def test_servers_that_will_not_restart_trigger_a_reboot():
    boot_id, since = _snapshot()

    # Every restart in this boot fails, so only a reboot can recover.
    r = _post("/api/test/api-server-kill", json={"fail_starts": 1000})
    assert r.status_code == 202, r.text

    def new_boot():
        data = _events(limit=200)
        return data if data and data["current_boot_id"] != boot_id else None

    data = _wait_for(new_boot, "the controller to reboot", REBOOT_TIMEOUT_S)

    old = _web_server_events(data, boot_id, since)
    down = [e for e in old if e["type"] == "web_server_down"]
    up = [e for e in old if e["type"] == "web_server_recovered"]
    assert down and down[0]["cause"] == "test", old
    assert up and up[-1]["recovery"] == "reboot", old
    assert _wait_for(_ha_reachable, "the HA server (8443) after the reboot", 60)
