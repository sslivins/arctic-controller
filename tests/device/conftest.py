"""
Pytest fixtures for Arctic Controller device UI tests.

Set ARCTIC_URL env var to override the default device address.
Example: ARCTIC_URL=http://192.168.1.42 pytest
"""

import os
import time
import pytest
from pathlib import Path
from device_client import DeviceClient

# Directory for failure screenshots
SCREENSHOT_DIR = Path(__file__).parent / "screenshots"

# Circuit breaker — abort the entire session if the device stops responding.
# When a test can't reach the device, _consecutive_failures is incremented.
# After MAX_CONSECUTIVE_FAILURES in a row, pytest.exit() is called to avoid
# burning hours on HTTP timeouts for a dead device.
MAX_CONSECUTIVE_FAILURES = 3
_consecutive_failures = 0


def _device_alive(device: DeviceClient) -> bool:
    """Quick health check — returns True if the device responds."""
    try:
        device.screen  # lightweight /api/test/screen call
        return True
    except Exception:
        return False


def _return_to_main(device: DeviceClient):
    """Navigate the device back to the main screen from wherever it is."""
    try:
        current = device.screen
    except Exception:
        time.sleep(2)
        try:
            current = device.screen
        except Exception:
            return

    if current == "main":
        device.wait_for_widget(tag="settings", timeout=3.0)
        return

    # If on a heat pump sub-screen, close it to return to main
    if current in ("temps", "system", "control", "errors", "event_log"):
        try:
            device.click(tag=f"{current}_close")
        except Exception:
            try:
                device.click(symbol="CLOSE")
            except Exception:
                pass
        device.wait_for_screen("main", timeout=5.0)
        device.wait_for_widget(tag="settings", timeout=3.0)
        return

    # If on a sub-screen, go back to settings first
    if current in ("display", "wifi", "firmware", "time", "language"):
        if current == "wifi":
            try:
                device.wifi_mock_reset()
            except Exception:
                pass
        if current == "firmware":
            try:
                device.firmware_mock_reset()
            except Exception:
                pass
        try:
            device.click(tag=f"{current}_back")
        except Exception:
            try:
                device.click(symbol="LEFT")
            except Exception:
                pass
        device.wait_for_screen("settings", timeout=5.0)
        time.sleep(0.5)

    # Close settings to return to main
    try:
        device.click(tag="settings_close")
    except Exception:
        try:
            device.click(symbol="CLOSE")
        except Exception:
            pass
    device.wait_for_screen("main", timeout=5.0)
    device.wait_for_widget(tag="settings", timeout=5.0)


@pytest.fixture(scope="session")
def device() -> DeviceClient:
    """Shared device client for the entire test session."""
    url = os.environ.get("ARCTIC_URL", "http://arctic.local")
    client = DeviceClient(base_url=url)
    # Sanity check — can we reach the device?
    try:
        client.get_ui_state()
    except Exception as e:
        pytest.skip(f"Device not reachable at {url}: {e}")

    # Acquire exclusive device lock (prevents concurrent test sessions)
    try:
        client.lock(ttl_seconds=900)  # 15 minute TTL
    except Exception as e:
        pytest.exit(f"Cannot acquire device lock: {e}", returncode=1)

    # Ensure demo mode is enabled (many tests depend on set_demo_fields)
    try:
        prefs = client.get_preferences()
        if not prefs.get("demo_mode"):
            client.click(tag="settings")
            client.wait_for_screen("settings", timeout=5.0)
            time.sleep(0.5)
            client.toggle("demo_mode_switch")
            time.sleep(0.3)
            _return_to_main(client)
        # Verify demo mode is actually on
        prefs = client.get_preferences()
        if not prefs.get("demo_mode"):
            pytest.exit("Demo mode could not be enabled — aborting session", returncode=1)
    except SystemExit:
        raise  # Let pytest.exit() propagate
    except Exception as e:
        pytest.exit(f"Failed to enable demo mode: {e}", returncode=1)

    yield client

    # After all tests complete, leave the device on the main screen
    _return_to_main(client)

    # Release device lock
    try:
        client.unlock(force=True)
    except Exception:
        pass  # Best effort — lock will expire via TTL


@pytest.fixture(autouse=True)
def ensure_main_screen(device: DeviceClient):
    """Before each test, make sure we're on the main screen.
    
    If the device is unreachable, skip immediately — the circuit breaker
    (pytest_runtest_makereport) will abort the session after repeated failures.
    """
    global _consecutive_failures
    if _consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
        pytest.exit(
            f"Device unreachable — {_consecutive_failures} consecutive failures, aborting session",
            returncode=1,
        )
    if not _device_alive(device):
        pytest.fail("Device unreachable — cannot navigate to main screen")
    _return_to_main(device)


@pytest.fixture(autouse=True)
def screenshot_on_failure(request, device: DeviceClient):
    """Capture a screenshot when a test fails, for visual debugging.
    
    Skips the screenshot if the device is unreachable (avoids a 30s timeout).
    """
    yield
    rep = getattr(request.node, "rep_call", None)
    if rep and rep.failed and _consecutive_failures < MAX_CONSECUTIVE_FAILURES:
        SCREENSHOT_DIR.mkdir(exist_ok=True)
        name = request.node.name.replace("/", "_").replace("::", "_")
        path = SCREENSHOT_DIR / f"{name}.png"
        try:
            device.screenshot(str(path))
            print(f"\n📸 Failure screenshot saved: {path}")
        except Exception as e:
            print(f"\n⚠️ Failed to capture screenshot: {e}")


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    """Stash test result on the item and track consecutive failures.

    After MAX_CONSECUTIVE_FAILURES tests fail in a row (any phase),
    the session is aborted via ensure_main_screen to avoid spending
    hours on HTTP timeouts for a dead device.
    """
    global _consecutive_failures
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)

    # Track consecutive failures (setup or call phase)
    if rep.when in ("setup", "call"):
        if rep.failed:
            _consecutive_failures += 1
        elif rep.when == "call" and rep.passed:
            _consecutive_failures = 0
