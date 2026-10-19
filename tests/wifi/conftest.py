"""
Fixtures for the Wi-Fi recovery suite: the bench controller is knocked off
Wi-Fi by the UniFi controller (kick / block), as a real access point would,
and must recover on its own without rebooting.

These tests take the device offline, so the suite runs on its own after every
other suite and holds the device test lock throughout.

Env:
  ARCTIC_URL        controller base URL (default http://arctic.local)
  ARCTIC_API_KEY    controller API key
  UNIFI_URL         UniFi console (default https://192.168.1.1)
  UNIFI_API_KEY     UniFi Network API key
  UNIFI_SITE        UniFi site name (default "default")
  UNIFI_DEVICE_MAC  controller Wi-Fi MAC; looked up in UniFi by IP when unset
  CI                when set, missing UniFi access FAILS the run instead of
                    skipping it (a silent skip would hide a dead rig)
"""

import os
import socket
import sys
import time
from pathlib import Path
from urllib.parse import urlparse

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from device_client import DeviceClient  # noqa: E402
from unifi_client import UnifiClient  # noqa: E402

IN_CI = bool(os.environ.get("CI"))


def _unavailable(msg: str):
    if IN_CI:
        pytest.exit(msg, returncode=1)
    pytest.skip(msg)


def wait_until(fn, timeout: float, poll: float = 0.5, desc: str = "condition"):
    """Poll fn() until it returns a truthy value and return it; fail on timeout."""
    deadline = time.monotonic() + timeout
    last = None
    while True:
        try:
            last = fn()
            if last:
                return last
        except Exception as e:  # device unreachable mid-outage - keep polling
            last = e
        if time.monotonic() >= deadline:
            pytest.fail(f"Timed out after {timeout:.0f}s waiting for {desc} (last: {last!r})")
        time.sleep(poll)


def status(dev: DeviceClient, timeout: float = 3.0) -> dict:
    r = dev.session.get(f"{dev.base_url}/api/status", timeout=timeout)
    r.raise_for_status()
    return r.json()


def events_since(dev: DeviceClient, boot_id: int, uptime_ms: int) -> list[dict]:
    """Events logged in this boot at or after uptime_ms, oldest first."""
    r = dev.session.get(f"{dev.base_url}/api/events", params={"limit": 50}, timeout=dev.timeout)
    r.raise_for_status()
    evs = [e for e in r.json().get("events", [])
           if e.get("boot_id") == boot_id and e.get("uptime_ms", 0) >= uptime_ms]
    return sorted(evs, key=lambda e: e["uptime_ms"])


@pytest.fixture(scope="session")
def unifi() -> UnifiClient:
    key = os.environ.get("UNIFI_API_KEY", "").strip()
    if not key:
        _unavailable("UNIFI_API_KEY not set")
    client = UnifiClient(os.environ.get("UNIFI_URL", "https://192.168.1.1"), key,
                         site=os.environ.get("UNIFI_SITE", "default"))
    try:
        client.clients()
    except Exception as e:
        _unavailable(f"UniFi controller not reachable: {e}")
    return client


@pytest.fixture(scope="session")
def device() -> DeviceClient:
    url = os.environ.get("ARCTIC_URL", "http://arctic.local")
    client = DeviceClient(base_url=url)
    try:
        st = status(client, timeout=10.0)
    except Exception as e:
        _unavailable(f"Device not reachable at {url}: {e}")
    if (st.get("wifi") or {}).get("state") != "connected":
        _unavailable("WiFi not connected")
    try:
        client.lock(ttl_seconds=900)
    except Exception as e:
        pytest.exit(f"Cannot acquire device lock: {e}", returncode=1)
    yield client
    try:
        client.unlock(force=True)
    except Exception:
        pass


@pytest.fixture(scope="session")
def device_mac(unifi: UnifiClient, device: DeviceClient) -> str:
    mac = os.environ.get("UNIFI_DEVICE_MAC", "").strip()
    if mac:
        return mac.lower()
    ip = status(device)["wifi"].get("ip") or socket.gethostbyname(urlparse(device.base_url).hostname)
    mac = unifi.find_mac_by_ip(ip)
    if not mac:
        _unavailable(f"Device not reachable through UniFi: no associated client has IP {ip}")
    return mac.lower()
