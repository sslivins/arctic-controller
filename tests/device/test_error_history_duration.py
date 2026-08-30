"""
Test: Error History Duration Format

Verifies that the duration shown for cleared errors in the error history
uses the correct format (e.g., "3s", "5s") rather than a malformed string.

Steps:
  1. Clear error history
  2. Set P02 error, wait ~3 seconds, then clear it
  3. Open the error panel
  4. Find the duration label for the cleared error
  5. Verify the duration text contains a proper seconds format like "3s" or "4s"
"""

import re
import time
import pytest
from device_client import DeviceClient

UI_SETTLE = 1.5  # Wait for 1s main screen timer

# Default demo fault (matches initDemoState()).
DEMO_FAULT = "P02"
ERROR_HOLD_SECONDS = 3

# Duration labels render as "<label> | <sep> <n><unit>" e.g. "Duration: | 3s".
_DURATION_RE = re.compile(r'\|\s*\S+\s+\d+[smhd]')


def _errors_json(device: DeviceClient) -> dict:
    """GET /api/heatpump/errors as JSON."""
    r = device.session.get(f"{device.base_url}/api/heatpump/errors",
                           timeout=device.timeout)
    r.raise_for_status()
    return r.json()


def _active_codes(device: DeviceClient) -> list:
    """Active error codes from /api/heatpump/errors."""
    data = _errors_json(device)
    active = data.get("active", data.get("errors", []))
    return [e.get("code", "") for e in active if isinstance(e, dict)]


def _has_duration_label(device: DeviceClient) -> bool:
    """True once a cleared-error duration label has rendered on the panel."""
    return any(w.text and _DURATION_RE.search(w.text) for w in device.widgets)


@pytest.fixture(autouse=True)
def _restore_demo_state(device: DeviceClient):
    """Restore default demo state after each test."""
    yield
    device.clear_all_faults()
    device.inject_fault(DEMO_FAULT, True)
    device.set_demo_fields(unit_on=1)


class TestErrorHistoryDuration:
    """Verify the duration format for cleared errors in the history."""

    def test_cleared_error_shows_seconds_format(self, device: DeviceClient):
        """A cleared error held for ~3s should show duration like '3s' or '4s'."""
        # 1. Clear all faults and history
        device.clear_all_faults()
        device.wait_until(
            "all faults cleared",
            lambda: not _active_codes(device),
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )
        device.clear_error_history()

        # 2. Set P02 fault
        device.inject_fault("P02", True)
        device.wait_until(
            "P02 active",
            lambda: "P02" in _active_codes(device),
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )

        # 3. Hold for a known duration. This sleep is INTENTIONAL: the test
        #    measures how long the fault was active, so it must stay active for
        #    a real, known wall-clock interval. It is not a UI settle.
        time.sleep(ERROR_HOLD_SECONDS)

        # 4. Clear the fault — this creates a history entry
        device.inject_fault("P02", False)
        device.wait_until(
            "P02 cleared",
            lambda: "P02" not in _active_codes(device),
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )

        # 5. Open the error panel
        device.click(tag="error_label")
        assert device.wait_for_screen("errors", timeout=5.0), \
            f"Expected 'errors' screen, got '{device.screen}'"

        # 6. Find duration text in the widget tree
        #    The duration label is language-dependent (Duration:/Duración:/Durée :)
        #    but the time format is always like "3s", "5m 30s", etc.
        #    Look for labels containing a time duration pattern after a pipe separator.
        #    The card renders asynchronously after the screen settles, so poll for
        #    it before the authoritative assert below.
        device.wait_until(
            "cleared-error duration label present",
            lambda: _has_duration_label(device),
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )
        widgets = device.widgets
        duration_texts = [
            w.text for w in widgets
            if w.text and _DURATION_RE.search(w.text)
        ]

        assert len(duration_texts) > 0, \
            "No duration label found — error history may be empty"

        # 7. Verify the duration contains a proper seconds format
        #    Should match something like "3s" or "4s"
        #    Bug produces "lds" or "3lds"
        duration_line = duration_texts[0]
        match = re.search(r'\|\s*\S+\s+(\d+[smhd])', duration_line)
        assert match, f"Could not parse duration from: '{duration_line}'"
        duration_value = match.group(1)

        # Duration should be a number followed by 's' (e.g., "3s", "4s", "5s")
        assert re.match(r'^\d+s$', duration_value), \
            f"Expected duration like '3s', got '{duration_value}'"

        # Sanity: should be roughly ERROR_HOLD_SECONDS ± 2
        seconds = int(duration_value[:-1])
        assert ERROR_HOLD_SECONDS - 1 <= seconds <= ERROR_HOLD_SECONDS + 3, \
            f"Duration {seconds}s outside expected range ({ERROR_HOLD_SECONDS}±2s)"

        # 8. Close panel
        device.click(symbol="CLOSE")
        device.wait_for_screen("main", timeout=5.0, raise_on_timeout=False)

        # 9. Clean up error history
        device.clear_error_history()
