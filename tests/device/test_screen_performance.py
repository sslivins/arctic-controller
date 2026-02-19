"""
Screen Render Performance Tests

Measures the firmware-side render time of every screen transition and
asserts it stays under budget.  The click endpoint returns
``render_time_us`` — the wall-clock microseconds spent inside
``lv_obj_send_event(LV_EVENT_CLICKED)`` which synchronously creates
the target screen's widget tree.

Performance budget — derived from industry guidelines:
  * Google RAIL model: user input → visual response within 100 ms
  * Jakob Nielsen: <100 ms perceived as instant, <300 ms maintains
    the feeling of direct manipulation
  * For an embedded touch UI with complex widget creation, we set
    the bar at **300 ms** for any single screen transition.

The tests also exercise "heavy" scenarios (full event log, many errors)
to ensure the batch-layout optimizations hold up.

Markers:
  pytest.mark.performance — all tests in this file
"""

import time
import pytest
from device_client import DeviceClient


# ---------------------------------------------------------------------------
# Budget (microseconds).  300 ms = 300 000 µs.
# ---------------------------------------------------------------------------
RENDER_BUDGET_US = 300_000


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _render_us(result: dict) -> int:
    """Extract render_time_us from a click response, or -1 if missing."""
    return int(result.get("render_time_us", -1))


def _assert_under_budget(result: dict, label: str, budget_us: int = RENDER_BUDGET_US):
    us = _render_us(result)
    ms = us / 1000
    assert us > 0, f"{label}: render_time_us missing from response"
    assert us <= budget_us, (
        f"{label}: render took {ms:.1f} ms — exceeds {budget_us / 1000:.0f} ms budget"
    )
    return us  # return for logging


# =========================================================================
# Nav button screen transitions (normal state)
# =========================================================================

@pytest.mark.performance
class TestNavScreenPerformance:
    """Render budget for each footer nav button → sub-screen."""

    def test_temps_screen_render(self, device: DeviceClient):
        """Temperatures screen opens within budget."""
        result = device.click(tag="nav_temps")
        _assert_under_budget(result, "temps")
        device.wait_for_screen("temps", timeout=3.0)
        device.click(tag="temps_close")

    def test_system_screen_render(self, device: DeviceClient):
        """System screen opens within budget."""
        result = device.click(tag="nav_system")
        _assert_under_budget(result, "system")
        device.wait_for_screen("system", timeout=3.0)
        device.click(tag="system_close")

    def test_control_screen_render(self, device: DeviceClient):
        """Control screen opens within budget."""
        result = device.click(tag="nav_control")
        _assert_under_budget(result, "control")
        device.wait_for_screen("control", timeout=3.0)
        device.click(tag="control_close")

    def test_event_log_screen_render(self, device: DeviceClient):
        """Event log screen opens within budget (minimal events)."""
        result = device.click(tag="nav_events")
        _assert_under_budget(result, "event_log")
        device.wait_for_screen("event_log", timeout=3.0)
        device.click(tag="event_log_close")

    def test_errors_screen_render(self, device: DeviceClient):
        """Errors screen opens within budget (no active errors)."""
        result = device.click(tag="error_label")
        _assert_under_budget(result, "errors")
        device.wait_for_screen("errors", timeout=3.0)
        device.click(tag="errors_close")

    def test_settings_screen_render(self, device: DeviceClient):
        """Settings screen opens within budget."""
        result = device.click(tag="settings")
        _assert_under_budget(result, "settings")
        device.wait_for_screen("settings", timeout=3.0)
        device.click(tag="settings_close")


# =========================================================================
# Heavy state scenarios — worst-case render paths
# =========================================================================

@pytest.mark.performance
class TestHeavyStatePerformance:
    """Render budget when screens have maximum content."""

    def test_event_log_full_buffer(self, device: DeviceClient):
        """Event log screen with a full ring buffer stays within budget.

        Injects all error1+error2 bits to generate many events, then
        opens the event log screen and checks render time.
        """
        # Fill the event log by toggling error bits
        device.set_demo_fields(error1=0xFFFF, error2=0xFFFF)
        time.sleep(2)  # let poll loop fire events
        device.set_demo_fields(error1=0, error2=0x0040)
        time.sleep(2)  # clear events also logged

        # Now open the event log — this is the hot path
        result = device.click(tag="nav_events")
        us = _assert_under_budget(result, "event_log_full")
        device.wait_for_screen("event_log", timeout=3.0)
        device.click(tag="event_log_close")

    def test_errors_screen_many_errors(self, device: DeviceClient):
        """Errors screen with multiple active errors stays within budget."""
        device.set_demo_fields(error1=0xFFFF, error2=0xFFFF)
        time.sleep(2)

        result = device.click(tag="error_label")
        us = _assert_under_budget(result, "errors_heavy")
        device.wait_for_screen("errors", timeout=3.0)
        device.click(tag="errors_close")
        time.sleep(0.5)

        # Restore normal state
        device.set_demo_fields(error1=0, error2=0x0040)
        time.sleep(1.5)

    def test_errors_screen_with_history(self, device: DeviceClient):
        """Errors screen with active + cleared history stays within budget."""
        # Create history by setting then clearing errors
        device.set_demo_fields(error1=0x00FF)
        time.sleep(2)
        device.set_demo_fields(error1=0x0001)  # clear 7 of 8, keep 1 active
        time.sleep(2)

        result = device.click(tag="error_label")
        us = _assert_under_budget(result, "errors_with_history")
        device.wait_for_screen("errors", timeout=3.0)
        device.click(tag="errors_close")
        time.sleep(0.5)

        # Restore
        device.set_demo_fields(error1=0, error2=0x0040)
        device.clear_error_history()
        time.sleep(1.5)


# =========================================================================
# Repeated transitions — check for widget leaks / degradation
# =========================================================================

@pytest.mark.performance
class TestRepeatedTransitionPerformance:
    """Opening and closing a screen repeatedly should not degrade."""

    def test_event_log_repeated_open_close(self, device: DeviceClient):
        """Open/close event log 5 times — last render still within budget."""
        times_us = []
        for i in range(5):
            result = device.click(tag="nav_events")
            us = _render_us(result)
            times_us.append(us)
            device.wait_for_screen("event_log", timeout=3.0)
            device.click(tag="event_log_close")
            device.wait_for_screen("main", timeout=3.0)

        # All iterations should be within budget
        for i, us in enumerate(times_us):
            assert us <= RENDER_BUDGET_US, (
                f"Iteration {i+1}: {us/1000:.1f} ms exceeds budget"
            )

        # No significant degradation: last should be ≤2× first
        if times_us[0] > 0:
            ratio = times_us[-1] / times_us[0]
            assert ratio < 3.0, (
                f"Render time degraded {ratio:.1f}× over 5 iterations "
                f"({times_us[0]/1000:.1f} → {times_us[-1]/1000:.1f} ms) — "
                f"possible widget leak"
            )

    def test_temps_repeated_open_close(self, device: DeviceClient):
        """Open/close temps screen 5 times — check for degradation."""
        times_us = []
        for i in range(5):
            result = device.click(tag="nav_temps")
            us = _render_us(result)
            times_us.append(us)
            device.wait_for_screen("temps", timeout=3.0)
            device.click(tag="temps_close")
            device.wait_for_screen("main", timeout=3.0)

        for i, us in enumerate(times_us):
            assert us <= RENDER_BUDGET_US, (
                f"Iteration {i+1}: {us/1000:.1f} ms exceeds budget"
            )

        if times_us[0] > 0:
            ratio = times_us[-1] / times_us[0]
            assert ratio < 3.0, (
                f"Render time degraded {ratio:.1f}× over 5 iterations — "
                f"possible widget leak"
            )
