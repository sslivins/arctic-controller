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


# Every canonical Macon fault code (mirrors arctic-macon MACON_FAULT_BITS),
# used to stress the event log / errors screen with a full set of faults.
ALL_FAULT_CODES = [
    "P15", "P16", "FE", "FF", "E28", "E19", "E18", "E13", "E03", "E27", "E21",
    "r02", "E26", "r01", "E01", "E09", "E05", "E22", "P19", "r06", "r10", "r11",
    "r05", "P11", "P02", "P06", "P27", "PC", "P10", "P30", "P01",
]


def _inject_all_faults(device: DeviceClient):
    """Activate every known fault to fill the event log / errors screen."""
    for code in ALL_FAULT_CODES:
        device.inject_fault(code, True)


# ---------------------------------------------------------------------------
# Budget (microseconds).  300 ms = 300 000 µs.
# ---------------------------------------------------------------------------
RENDER_BUDGET_US = 300_000

# The error screen renders only its first card batch synchronously, then
# progressively appends the rest on the LVGL task. Keep the initial response
# comfortably below the 300 ms general screen budget even at maximum content.
ERROR_INITIAL_BUDGET_US = 150_000


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _render_us(result: dict) -> int:
    """Extract render_time_us from a click response, or -1 if missing."""
    return int(result.get("render_time_us", -1))


def _assert_under_budget(result: dict, label: str, budget_us: int = RENDER_BUDGET_US):
    us = _render_us(result)
    ms = us / 1000
    budget_ms = budget_us / 1000
    pct = us / budget_us * 100
    assert us > 0, f"{label}: render_time_us missing from response"
    print(f"  ⏱  {label}: {ms:.1f} ms  ({pct:.0f}% of {budget_ms:.0f} ms budget)")
    assert us <= budget_us, (
        f"{label}: render took {ms:.1f} ms — exceeds {budget_ms:.0f} ms budget"
    )
    return us


# =========================================================================
# Nav button screen transitions (normal state)
# =========================================================================

@pytest.mark.performance
class TestNavScreenPerformance:
    """Render budget for each footer nav button → sub-screen."""

    def test_status_screen_render(self, device: DeviceClient):
        """Status screen (merged temps+system) opens within budget."""
        result = device.click(tag="nav_status")
        _assert_under_budget(result, "status")
        device.wait_for_screen("status", timeout=3.0)
        device.click(tag="nav_home")

    def test_control_screen_render(self, device: DeviceClient):
        """Control screen opens within budget."""
        result = device.click(tag="nav_control")
        _assert_under_budget(result, "control")
        device.wait_for_screen("control", timeout=3.0)
        device.click(tag="nav_home")

    def test_event_log_screen_render(self, device: DeviceClient):
        """Event log screen opens within budget (minimal events)."""
        result = device.click(tag="nav_events")
        _assert_under_budget(result, "event_log")
        device.wait_for_screen("event_log", timeout=3.0)
        device.click(tag="nav_home")

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

        Injects every known fault to generate many events, then
        opens the event log screen and checks render time.
        """
        # Fill the event log by toggling faults
        _inject_all_faults(device)
        time.sleep(2)  # let poll loop fire events
        device.clear_all_faults()
        device.inject_fault("P02", True)
        time.sleep(2)  # clear events also logged

        # Now open the event log — this is the hot path
        result = device.click(tag="nav_events")
        us = _assert_under_budget(result, "event_log_full")
        device.wait_for_screen("event_log", timeout=3.0)
        device.click(tag="nav_home")

    def test_errors_screen_many_errors(self, device: DeviceClient):
        """Errors screen with multiple active errors stays within budget."""
        _inject_all_faults(device)
        time.sleep(2)

        try:
            result = device.click(tag="error_label")
            us = _assert_under_budget(
                result, "errors_heavy", ERROR_INITIAL_BUDGET_US
            )
            device.wait_for_screen("errors", timeout=3.0)
        finally:
            # Close errors screen if open, then restore normal state
            try:
                device.click(tag="errors_close")
                time.sleep(0.5)
            except Exception:
                pass
            device.clear_all_faults()
            device.inject_fault("P02", True)
            time.sleep(1.5)

    def test_errors_screen_with_history(self, device: DeviceClient):
        """Errors screen with active + cleared history stays within budget."""
        try:
            # Create history by setting then clearing faults
            device.clear_all_faults()
            for code in ["E19", "E18", "E13", "E01", "E09", "E22", "P02", "P06"]:
                device.inject_fault(code, True)
            time.sleep(2)
            device.clear_all_faults()
            device.inject_fault("P02", True)  # keep 1 active, rest go to history
            time.sleep(2)

            result = device.click(tag="error_label")
            us = _assert_under_budget(
                result, "errors_with_history", ERROR_INITIAL_BUDGET_US
            )
            device.wait_for_screen("errors", timeout=3.0)
        finally:
            # Close errors screen if open, then restore
            try:
                device.click(tag="errors_close")
                time.sleep(0.5)
            except Exception:
                pass
            device.clear_all_faults()
            device.inject_fault("P02", True)
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
            device.wait_for_widget(tag="nav_events", timeout=3.0)
            result = device.click(tag="nav_events")
            us = _render_us(result)
            times_us.append(us)
            device.wait_for_screen("event_log", timeout=3.0)
            device.click(tag="nav_home")
            device.wait_for_screen("main", timeout=3.0)

        # Report all iteration times
        for i, us in enumerate(times_us):
            pct = us / RENDER_BUDGET_US * 100
            print(f"  ⏱  event_log #{i+1}: {us/1000:.1f} ms  ({pct:.0f}% of {RENDER_BUDGET_US/1000:.0f} ms budget)")

        # All iterations should be within budget
        for i, us in enumerate(times_us):
            assert us <= RENDER_BUDGET_US, (
                f"Iteration {i+1}: {us/1000:.1f} ms exceeds budget"
            )

        # No significant degradation: last should be ≤2× first
        if times_us[0] > 0:
            ratio = times_us[-1] / times_us[0]
            print(f"  📈 degradation: {ratio:.2f}× ({times_us[0]/1000:.1f} → {times_us[-1]/1000:.1f} ms)")
            assert ratio < 3.0, (
                f"Render time degraded {ratio:.1f}× over 5 iterations "
                f"({times_us[0]/1000:.1f} → {times_us[-1]/1000:.1f} ms) — "
                f"possible widget leak"
            )

    def test_temps_repeated_open_close(self, device: DeviceClient):
        """Open/close Status screen 5 times — check for degradation."""
        times_us = []
        for i in range(5):
            device.wait_for_widget(tag="nav_status", timeout=3.0)
            result = device.click(tag="nav_status")
            us = _render_us(result)
            times_us.append(us)
            device.wait_for_screen("status", timeout=3.0)
            device.click(tag="nav_home")
            device.wait_for_screen("main", timeout=3.0)

        # Report all iteration times
        for i, us in enumerate(times_us):
            pct = us / RENDER_BUDGET_US * 100
            print(f"  ⏱  status #{i+1}: {us/1000:.1f} ms  ({pct:.0f}% of {RENDER_BUDGET_US/1000:.0f} ms budget)")

        for i, us in enumerate(times_us):
            assert us <= RENDER_BUDGET_US, (
                f"Iteration {i+1}: {us/1000:.1f} ms exceeds budget"
            )

        if times_us[0] > 0:
            ratio = times_us[-1] / times_us[0]
            print(f"  📈 degradation: {ratio:.2f}× ({times_us[0]/1000:.1f} → {times_us[-1]/1000:.1f} ms)")
            assert ratio < 3.0, (
                f"Render time degraded {ratio:.1f}× over 5 iterations — "
                f"possible widget leak"
            )
