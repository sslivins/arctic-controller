"""
Pytest fixtures for Arctic Controller device UI tests.
Set ARCTIC_URL env var to override the default device address.
Example: ARCTIC_URL=http://192.168.1.42 pytest
"""

import os
import time
import json
import pytest
from pathlib import Path
from device_client import DeviceClient

# Directory for failure screenshots
SCREENSHOT_DIR = Path(__file__).parent / "screenshots"

# Where wait-latency samples are flushed at session end. The api-suite gate
# (tests/api/test_latency_budget.py) reads this to enforce the p95 budget.
# Overridable so a run can redirect it; defaults under tests/.
LATENCY_SAMPLES_PATH = Path(
    os.environ.get("ARCTIC_LATENCY_LOG",
                   str(Path(__file__).resolve().parents[1] / ".latency-samples.json"))
)

# Where per-task stack high-water marks are flushed at session end. The api-suite
# gate (tests/api/test_stack_watermark_budget.py) reads this to enforce a minimum
# free-stack floor per task, turning a shrinking-headroom regression into a CI
# failure before it becomes a stack-overflow crash. Overridable; defaults under tests/.
STACK_WATERMARKS_PATH = Path(
    os.environ.get("ARCTIC_STACK_WATERMARKS_LOG",
                   str(Path(__file__).resolve().parents[1] / ".stack-watermarks.json"))
)


def _flush_latency_samples(device: DeviceClient) -> None:
    """Merge this session's wait-latency samples into the shared samples file.

    Merge (not overwrite) so multiple pytest processes sharing the device in a
    single CI run each contribute. Best-effort — never fail teardown over
    telemetry.
    """
    try:
        samples = device.latency_samples()
        if not samples:
            return
        existing = []
        if LATENCY_SAMPLES_PATH.exists():
            try:
                existing = json.loads(LATENCY_SAMPLES_PATH.read_text(encoding="utf-8"))
            except Exception:
                existing = []
        existing.extend(samples)
        LATENCY_SAMPLES_PATH.write_text(
            json.dumps(existing), encoding="utf-8"
        )
    except Exception as e:
        print(f"\n⚠️ Failed to flush latency samples: {e}")

def _flush_stack_watermarks(device: DeviceClient) -> None:
    """Capture per-task stack high-water marks and merge into the shared file.

    The watermark is monotonic (smallest-ever free stack per task), so capturing
    once at session end reflects the worst case across everything that ran.
    Merge across pytest processes by keeping the SMALLEST free value seen per
    task (the true worst case). Best-effort — never fail teardown over telemetry.
    """
    try:
        marks = device.get_stack_watermarks()
        if not marks:
            return
        existing = {}
        if STACK_WATERMARKS_PATH.exists():
            try:
                existing = json.loads(STACK_WATERMARKS_PATH.read_text(encoding="utf-8"))
            except Exception:
                existing = {}
        for task, free_bytes in marks.items():
            prev = existing.get(task)
            existing[task] = free_bytes if prev is None else min(prev, free_bytes)
        STACK_WATERMARKS_PATH.write_text(
            json.dumps(existing), encoding="utf-8"
        )
    except Exception as e:
        print(f"\n⚠️ Failed to flush stack watermarks: {e}")

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
        # Long-running suites can legitimately cross the configured inactivity
        # timeout. Start each test awake so its first automation click is not
        # consumed as the physical wake-only tap.
        device.set_display_idle("wake")
    except Exception:
        pass

    try:
        current = device.screen
    except Exception:
        time.sleep(2)
        try:
            current = device.screen
        except Exception:
            return

    if current == "main":
        device.try_wait_widget(tag="settings", timeout=3.0)
        return

    # Dismiss reboot confirmation overlay if present (absorbs all taps)
    if device.has_widget(tag="reboot_overlay"):
        try:
            device.click(tag="reboot_cancel")
            time.sleep(0.5)
        except Exception:
            pass

    # Status / Control / Events are persistent panels in the tab shell — return
    # to the Home tab via the nav bar (there is no per-panel close button).
    if current in ("status", "control", "event_log"):
        try:
            device.click(tag="nav_home")
        except Exception:
            pass
        device.try_wait_screen("main", timeout=5.0)
        device.try_wait_widget(tag="settings", timeout=3.0)
        return

    # The errors screen is still a standalone overlay opened from the Home tab.
    if current == "errors":
        try:
            device.click(tag="errors_close")
        except Exception:
            try:
                device.click(symbol="CLOSE")
            except Exception:
                pass
        device.try_wait_screen("main", timeout=5.0)
        device.try_wait_widget(tag="settings", timeout=3.0)
        return

    # If on a sub-screen, go back to settings first
    if current in (
        "display",
        "wifi",
        "firmware",
        "time",
        "language",
        "home_assistant",
        "security",
        "web",
    ):
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
        device.try_wait_screen("settings", timeout=5.0)
        time.sleep(0.5)

    # Close settings to return to main
    try:
        device.click(tag="settings_close")
    except Exception:
        try:
            device.click(symbol="CLOSE")
        except Exception:
            pass
    device.try_wait_screen("main", timeout=5.0)
    device.try_wait_widget(tag="settings", timeout=5.0)


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

    # Ensure demo mode is enabled (many tests depend on set_demo_fields).
    # Use the API endpoint directly — toggling the UI switch now triggers
    # a reboot confirmation panel that can't be dismissed without side effects.
    try:
        prefs = client.get_preferences()
        if not prefs.get("demo_mode"):
            client.set_preference(demo_mode=True)
            client.reboot()
            if not client.wait_for_device(timeout=45.0):
                pytest.exit(
                    "Device did not return after enabling demo mode",
                    returncode=1,
                )
            # A reboot clears the in-memory test lock.
            client.lock(ttl_seconds=900)
        # Verify demo mode is actually on
        prefs = client.get_preferences()
        if not prefs.get("demo_mode"):
            pytest.exit("Demo mode could not be enabled — aborting session", returncode=1)
    except SystemExit:
        raise  # Let pytest.exit() propagate
    except Exception as e:
        pytest.exit(f"Failed to enable demo mode: {e}", returncode=1)

    # Suppress the firmware's automatic (boot + periodic) GitHub update check for
    # the whole session. Otherwise the background check's callback can clear a
    # test-mocked firmware notification mid-test, flaking the status-bar and
    # firmware-badge tests (issue #164, F-07). This only gates the background
    # path; the firmware-screen-open check (test_firmware) is unaffected. The
    # per-test _suppress_update_check fixture re-applies this after reboot tests
    # reset the in-memory flag.
    try:
        client.set_update_check_suppressed(True)
    except Exception as e:
        print(f"\n⚠️ Could not suppress automatic update check: {e}")

    yield client

    # After all tests complete, leave the device on the main screen
    _return_to_main(client)

    # Flush wait-latency telemetry for the budget gate (best-effort).
    _flush_latency_samples(client)

    # Flush per-task stack high-water marks for the watermark gate (best-effort).
    _flush_stack_watermarks(client)

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
def _suppress_update_check(device: DeviceClient):
    """Keep the automatic firmware update check suppressed for every test.

    The flag lives in device RAM, so any test that reboots the device (e.g.
    test_https, test_ota_api) resets it. Re-applying before each test ensures a
    later test (e.g. test_status_bar) can't be flaked by a background update
    check clearing its mocked notification (issue #164, F-07). Best-effort —
    never fail a test over telemetry/setup; the session fixture already set it
    once, and only reboot tests can clear it.
    """
    try:
        device.set_update_check_suppressed(True)
    except Exception:
        pass


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
