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

# Default demo state
DEMO_STATUS1 = 0x002B
DEMO_ERROR2 = 0x0040  # HIGH_PRESSURE (P02)
ERROR_HOLD_SECONDS = 3


@pytest.fixture(autouse=True)
def _restore_demo_state(device: DeviceClient):
    """Restore default demo state after each test."""
    yield
    device.set_demo_fields(
        error1=0,
        error2=DEMO_ERROR2,
        status1=DEMO_STATUS1,
        unit_on=1,
    )
    time.sleep(UI_SETTLE)


class TestErrorHistoryDuration:
    """Verify the duration format for cleared errors in the history."""

    def test_cleared_error_shows_seconds_format(self, device: DeviceClient):
        """A cleared error held for ~3s should show duration like '3s' or '4s'."""
        # 1. Clear all errors and history
        device.set_demo_fields(error1=0, error2=0)
        time.sleep(UI_SETTLE)
        device.clear_error_history()

        # 2. Set P02 error
        device.set_demo_fields(error2=DEMO_ERROR2)
        time.sleep(UI_SETTLE)

        # 3. Hold for a known duration
        time.sleep(ERROR_HOLD_SECONDS)

        # 4. Clear the error — this creates a history entry
        device.set_demo_fields(error2=0)
        time.sleep(UI_SETTLE)

        # 5. Open the error panel
        device.click(tag="error_label")
        time.sleep(1.0)  # panel animation

        # 6. Find duration text in the widget tree
        widgets = device.widgets
        duration_texts = [
            w.text for w in widgets
            if w.text and "Duration:" in w.text
        ]

        assert len(duration_texts) > 0, \
            "No 'Duration:' label found — error history may be empty"

        # 7. Verify the duration contains a proper seconds format
        #    Should match something like "Duration: 3s" or "Duration: 4s"
        #    Bug produces "Duration: lds" or "Duration: 3lds"
        duration_line = duration_texts[0]
        match = re.search(r'Duration:\s*(\S+)', duration_line)
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
        time.sleep(0.5)

        # 9. Clean up error history
        device.clear_error_history()
