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
    must restart them in place, or reboot when restarts keep failing. The
    same hook can make the supervisor's loopback /api/health self-probe fail
    (a server that is running but not answering) or make only the Home
    Assistant server (8443) fail to start, which must be repaired without
    touching the working web UI.
  - A screenshot streams off the server thread, and POST
    /api/test/httpd-stall holds the thread so the stall detector's report
    can be checked.

They are destructive (the device is offline for up to a few minutes); the
reboot case is last in this module.
"""

import os
import socket
import threading
import time
from urllib.parse import urlsplit

import pytest
import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

from tls_handshake_retry import no_handshake_retry

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
# Self-probe every 60 s; three failures 15 s apart before the restart.
UNRESPONSIVE_TIMEOUT_S = 150
# 8443 is retried every 60 s once the web UI is back.
SECONDARY_REPAIR_TIMEOUT_S = 150


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


def _unreachable() -> bool:
    """One unpaced probe fails. A handshake retry would hide the outage."""
    with no_handshake_retry():
        return _events(paced=False) is None


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
    _wait_for(_unreachable, "WiFi to drop", 20)
    data = _wait_for(_events, "the API to answer after the bounce", 60)

    assert data["current_boot_id"] == boot_id, "the bounce must not reboot"
    assert _wait_for(_ha_reachable, "the HA server (8443) after the bounce", 30)
    downs = [e for e in _web_server_events(data, boot_id, since)
             if e["type"] == "web_server_down"]
    assert not downs, f"servers went down across a WiFi bounce: {downs}"


def test_screenshot_does_not_block_the_web_server():
    """Screenshots stream for seconds; they run off the server thread
    (main/slow_request.cpp), so other requests -- and the supervisor's
    self-probe -- keep being answered meanwhile."""
    done = threading.Event()
    result = {}

    def shoot():
        with _session(paced=False) as s:
            r = s.get(f"{BASE_URL}/api/screenshot", timeout=60)
        result["status"], result["size"] = r.status_code, len(r.content)
        done.set()

    shooter = threading.Thread(target=shoot, daemon=True)
    shooter.start()
    latencies = []
    with _session(paced=False) as s:
        while not done.is_set():
            start = time.monotonic()
            s.get(f"{BASE_URL}/api/health", timeout=10).raise_for_status()
            latencies.append(time.monotonic() - start)
    shooter.join()
    assert result["status"] == 200 and result["size"] > 100_000, result
    assert len(latencies) >= 3, "the screenshot finished too fast to overlap"
    assert max(latencies) < 2, f"/api/health stalled behind a screenshot: {latencies}"


@pytest.mark.parametrize("which", ["ha", "web"])
def test_idle_connection_does_not_block_a_tls_server(which):
    """Each TLS server handshakes on its one thread. A client that connects
    and sends nothing (a browser preconnect) used to hold that thread for the
    10 s receive timeout -- 20 s with the handshake retry -- so every other
    request waited, and a web dashboard load missed its deadline (PR #411).
    The handshake wait is now bounded at ~2 s (main/api_server.cpp).

    The bound applies to every client, so a real request whose handshake is
    held up on the WiFi can be dropped too; it is retried like a handshake
    timeout (tests/tls_handshake_retry.py). The old stall was 10-20 s."""
    url = HA_URL if which == "ha" else BASE_URL
    if not url.startswith("https://"):
        pytest.skip("the web UI is plain HTTP (no administrator certificate)")
    parts = urlsplit(url)
    idle = socket.create_connection((parts.hostname, parts.port or 443), timeout=10)
    try:
        start = time.monotonic()
        with _session(paced=False) as s:
            s.get(f"{url}/api/health", timeout=30)
        waited = time.monotonic() - start
        # The server gave up on the silent socket rather than waiting it out.
        idle.settimeout(10)
        try:
            closed = idle.recv(1) == b""
        except ConnectionResetError:
            closed = True
    finally:
        idle.close()
    assert waited < 8, f"a request waited {waited:.1f} s behind an idle connection"
    assert closed, "the server kept the silent connection open"


def _log_entries_after(seq: int) -> list:
    with _session(paced=True) as s:
        r = s.get(f"{BASE_URL}/api/logs", timeout=10)
    r.raise_for_status()
    return [e for e in r.json()["entries"] if e["seq"] > seq]


def test_stall_detector_reports_a_stuck_server():
    """main/httpd_watch.cpp logs what was running when the server thread
    stops answering, and when it answers again."""
    since = max((e["seq"] for e in _log_entries_after(0)), default=0)

    r = _post("/api/test/httpd-stall?ms=6000")
    assert r.status_code == 200, r.text

    def reported():
        msgs = [e["message"] for e in _log_entries_after(since)
                if e["tag"] == "httpd_watch"]
        return msgs if any("answering again" in m for m in msgs) else None

    msgs = _wait_for(reported, "the stall detector's report", 20)
    stuck = [m for m in msgs if m.startswith("Web server thread stuck for")]
    assert stuck, msgs
    assert any("<- web server" in m for m in msgs), msgs


def test_killed_servers_are_restarted_in_place():
    boot_id, since = _snapshot()

    # The first restart attempt fails (injected); the next one succeeds.
    r = _post("/api/test/api-server-kill", json={"fail_starts": 1})
    assert r.status_code == 202, r.text
    _wait_for(_unreachable, "the servers to stop", 15)

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


def _wait_for_recovered(boot_id: int, since: int, timeout: float):
    def recovered():
        data = _events()
        if not data:
            return None
        evts = _web_server_events(data, boot_id, since)
        if any(e["type"] == "web_server_recovered" for e in evts):
            return data, evts
        return None
    return _wait_for(recovered, "web_server_recovered", timeout)


def test_unresponsive_servers_are_restarted():
    """Running but not answering its own /api/health -> restarted in place."""
    boot_id, since = _snapshot()

    r = _post("/api/test/api-server-kill", json={"fail_probes": 3, "stop": False})
    assert r.status_code == 202, r.text

    data, evts = _wait_for_recovered(boot_id, since, UNRESPONSIVE_TIMEOUT_S)
    assert data["current_boot_id"] == boot_id, "an in-place restart must not reboot"
    down = [e for e in evts if e["type"] == "web_server_down"]
    up = [e for e in evts if e["type"] == "web_server_recovered"]
    assert len(down) == 1, evts
    assert down[0]["cause"] == "unresponsive"
    assert up[0]["recovery"] == "restarted"
    assert _wait_for(_ha_reachable, "the HA server (8443) after the restart", 30)


def test_ha_server_failure_does_not_disturb_the_web_ui():
    """8443 failing to start is repaired on its own, without another web UI
    restart (or a reboot)."""
    boot_id, since = _snapshot()

    # Kill everything; the restart brings the web UI back but 8443 fails, and
    # so does the first separate repair of 8443.
    r = _post("/api/test/api-server-kill", json={"fail_integration_starts": 2})
    assert r.status_code == 202, r.text
    _wait_for(_unreachable, "the servers to stop", 15)
    _wait_for_recovered(boot_id, since, RESTART_TIMEOUT_S)

    assert _wait_for(_ha_reachable, "the HA server (8443) to be repaired",
                     SECONDARY_REPAIR_TIMEOUT_S)
    data = _events()
    assert data and data["current_boot_id"] == boot_id, "must not reboot"
    evts = _web_server_events(data, boot_id, since)
    assert [e["type"] for e in evts].count("web_server_down") == 1, evts
    assert [e["type"] for e in evts].count("web_server_recovered") == 1, evts


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
