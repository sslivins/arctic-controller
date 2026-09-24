"""
Fixtures for the RS-485 end-to-end suite: controller (active Macon bus master)
wired to the arctic-simulator (Macon slave).

Every expectation is expressed by *meaning*. The simulator is driven through
its semantic API (named fields, fault codes / sites) and the fault catalog is
read from the simulator at collection time, so this suite carries no register
map and no fault table of its own: the arctic-macon library, built into both
firmwares, is the single source of truth. A session gate refuses to run if the
two firmwares were built against different library layouts / catalogs.

Unlike tests/device, this suite needs demo mode OFF so the controller actually
drives the bus. The session fixture turns it off, and always turns it back on
at teardown so the next suite (and the bench baseline) sees a demo device.

Env:
  ARCTIC_URL       controller base URL (default http://arctic.local)
  ARCTIC_API_KEY   controller API key
  ARCTIC_SIM_URL   simulator base URL (default http://arctic-sim.local)
  CI               when set, an unreachable simulator/controller FAILS the run
                   instead of skipping it (a silent skip would hide a dead rig)
"""

import os
import sys
import time
import uuid
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from device_client import DeviceClient  # noqa: E402
from simulator_client import ArcticSimClient  # noqa: E402

# The master polls every ~800 ms; allow a few cycles plus HTTP latency.
BUS_TIMEOUT = 10.0

SIM_URL = os.environ.get("ARCTIC_SIM_URL", "http://arctic-sim.local")
IN_CI = bool(os.environ.get("CI"))
LEASE_OWNER = f"rs485-{os.environ.get('GITHUB_RUN_ID', 'local')}-{uuid.uuid4().hex[:6]}"


def _unavailable(msg: str):
    if IN_CI:
        pytest.exit(msg, returncode=1)
    pytest.skip(msg)


def wait_until(fn, timeout: float = BUS_TIMEOUT, poll: float = 0.4, desc: str = "condition"):
    """Poll fn() until it returns a truthy value; return it, or fail with the last value."""
    deadline = time.monotonic() + timeout
    last = None
    while True:
        try:
            last = fn()
            if last:
                return last
        except Exception as e:  # transient HTTP hiccup - keep polling
            last = e
        if time.monotonic() >= deadline:
            pytest.fail(f"Timed out after {timeout:.0f}s waiting for {desc} (last: {last!r})")
        time.sleep(poll)


# ---------------------------------------------------------------------------
# Catalog-driven parametrization
# ---------------------------------------------------------------------------

_catalog_cache = None


def _catalog():
    """Fault catalog from the simulator (None if it can't be reached)."""
    global _catalog_cache
    if _catalog_cache is None:
        try:
            _catalog_cache = ArcticSimClient(base_url=SIM_URL).fault_catalog()
        except Exception:
            _catalog_cache = []
    return _catalog_cache


def pytest_generate_tests(metafunc):
    if "fault_entry" in metafunc.fixturenames:
        cat = _catalog()
        metafunc.parametrize("fault_entry", cat or [None],
                             ids=[f["code"] for f in cat] if cat else ["no-catalog"])
    if "fault_site" in metafunc.fixturenames:
        sites = [dict(s, code=f["code"]) for f in _catalog() for s in f["sites"]]
        metafunc.parametrize("fault_site", sites or [None],
                             ids=[f"{s['code']}-{s['site']}" for s in sites] if sites else ["no-catalog"])


# ---------------------------------------------------------------------------
# Session fixtures
# ---------------------------------------------------------------------------

def _set_demo_mode(dev: DeviceClient, enabled: bool) -> None:
    if bool(dev.get_preferences().get("demo_mode")) == enabled:
        return
    dev.set_preference(demo_mode=enabled)
    dev.reboot()
    time.sleep(5)  # let it actually go down before polling for it
    if not dev.wait_for_device(timeout=60.0):
        raise RuntimeError(f"Device did not come back after setting demo_mode={enabled}")
    if bool(dev.get_preferences().get("demo_mode")) != enabled:
        raise RuntimeError(f"demo_mode did not stick at {enabled}")


@pytest.fixture(scope="session")
def sim() -> ArcticSimClient:
    client = ArcticSimClient(base_url=SIM_URL)
    if not client.is_reachable():
        _unavailable(f"Simulator not reachable at {SIM_URL}")
    try:
        client.acquire_lease(LEASE_OWNER, ttl_s=1800)
    except Exception as e:
        pytest.exit(f"Simulator bench is leased by someone else: {e}", returncode=1)
    client.load_preset("idle")
    yield client
    try:
        client.load_preset("idle")
        client.release_lease(LEASE_OWNER)
    except Exception as e:
        print(f"\n⚠️ Could not release the simulator lease: {e}")


@pytest.fixture(scope="session")
def device(sim: ArcticSimClient) -> DeviceClient:
    url = os.environ.get("ARCTIC_URL", "http://arctic.local")
    client = DeviceClient(base_url=url)
    try:
        client.get_preferences()
    except Exception as e:
        _unavailable(f"Device not reachable at {url}: {e}")

    try:
        client.lock(ttl_seconds=1800)
    except Exception as e:
        pytest.exit(f"Cannot acquire device lock: {e}", returncode=1)

    mismatch = _library_mismatch(client, sim)
    if mismatch:
        pytest.exit(mismatch, returncode=1)

    try:
        _set_demo_mode(client, False)
        client.lock(ttl_seconds=1800)  # a reboot clears the in-memory lock
        wait_until(
            lambda: (s := client.get_heatpump_status()).get("connected")
            and not s.get("demo_mode"),
            timeout=30.0, desc="controller connected to the simulator over RS-485",
        )
    except BaseException as e:
        _restore(client, sim)
        if isinstance(e, pytest.fail.Exception):
            pytest.exit(f"Controller never connected to the simulator: {e}", returncode=1)
        pytest.exit(f"Failed to switch the controller onto the RS-485 bus: {e}", returncode=1)

    yield client

    _restore(client, sim)


def controller_macon_identity(dev: DeviceClient) -> dict:
    r = dev.session.get(f"{dev.base_url}/api/status", timeout=dev.timeout)
    r.raise_for_status()
    return r.json().get("macon") or {}


def _library_mismatch(dev: DeviceClient, sim: ArcticSimClient):
    """Refuse to compare firmwares built against different arctic-macon builds."""
    ctrl, simm = controller_macon_identity(dev), sim.macon_identity()
    keys = ("api_version", "layout_fingerprint", "catalog_fingerprint")
    if not ctrl or not simm or any(ctrl.get(k) != simm.get(k) for k in keys):
        return ("arctic-macon library mismatch between controller and simulator "
                f"(controller={ctrl or 'n/a'}, simulator={simm or 'n/a'}); bump the "
                "arctic-macon submodule in both repos before running this suite")
    return None


def _restore(dev: DeviceClient, sim: ArcticSimClient) -> None:
    """Leave the simulator idle and the controller back in demo mode (best-effort)."""
    try:
        sim.load_preset("idle")
    except Exception as e:
        print(f"\n⚠️ Could not reset simulator: {e}")
    try:
        _set_demo_mode(dev, True)
    except Exception as e:
        print(f"\n⚠️ Could not restore demo mode: {e}")
    try:
        dev.unlock(force=True)
    except Exception:
        pass


@pytest.fixture(autouse=True)
def fresh_bus_state(sim: ArcticSimClient, device: DeviceClient):
    """Every test starts from the simulator's idle preset with no faults."""
    sim.load_preset("idle")
    wait_until(lambda: device.get_heatpump_errors().get("error_count") == 0,
               desc="controller to report no active errors after reset")
    yield
