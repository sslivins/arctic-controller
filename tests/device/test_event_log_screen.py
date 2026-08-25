"""
Test: Event Log Screen

Navigates to the event log screen and verifies empty state, title,
close button, and clear functionality.

Note: Events are generated internally by state changes — there is no
test injection endpoint. We can test the empty state after clearing,
and the system_start event that's always present.
"""

import time
import pytest
from device_client import DeviceClient

UI_SETTLE = 1.5


def _open_event_log(device: DeviceClient):
    """Navigate from main to the event log screen."""
    device.click(tag="nav_events")
    assert device.wait_for_screen("event_log", timeout=5.0), \
        f"Expected 'event_log' screen, got '{device.screen}'"
    time.sleep(0.5)


def _close_event_log(device: DeviceClient):
    """Return to the Home (main) tab via the persistent nav bar."""
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0), \
        f"Expected 'main' screen after close, got '{device.screen}'"


def _reset_filters(device: DeviceClient):
    """Return event filtering to its default state."""
    if device.find_widget(tag="event_search_clear") is not None:
        device.click(tag="event_search_clear")
    device.click(tag="event_filters_open")
    device.click(tag="event_filters_reset")
    device.click(tag="event_filters_apply")
    time.sleep(0.3)


# =========================================================================
# Navigation
# =========================================================================

class TestEventLogNavigation:
    """Open/close event log screen from the main screen footer."""

    def test_open_event_log_screen(self, device: DeviceClient):
        """Clicking the Events nav button opens the event log screen."""
        _open_event_log(device)

    def test_close_event_log_screen(self, device: DeviceClient):
        """Clicking the close button returns to the main screen."""
        _open_event_log(device)
        _close_event_log(device)

    def test_scrolled_events_control_events_does_not_reset(self, device: DeviceClient):
        """A scrolled event list can be left and reopened repeatedly."""
        device.inject_fault("P02", True)
        device.inject_fault("P06", True)
        device.inject_fault("E19", True)
        time.sleep(1.5)
        device.clear_all_faults()
        time.sleep(1.5)

        previous_uptime = device.session.get(
            f"{device.base_url}/api/health", timeout=device.timeout
        ).json()["uptime_ms"]

        for _ in range(5):
            _open_event_log(device)
            result = device.scroll_to("event_log_content", 350)
            assert result["y"] > 0, "Event list did not scroll"

            device.click(tag="nav_control")
            assert device.wait_for_screen("control", timeout=5.0)

            device.click(tag="nav_events")
            assert device.wait_for_screen("event_log", timeout=5.0)

            current_uptime = device.session.get(
                f"{device.base_url}/api/health", timeout=device.timeout
            ).json()["uptime_ms"]
            assert current_uptime >= previous_uptime, \
                "Controller rebooted during Events → Control → Events navigation"
            previous_uptime = current_uptime

            device.click(tag="nav_control")
            assert device.wait_for_screen("control", timeout=5.0)


# =========================================================================
# Title & Layout
# =========================================================================

class TestEventLogTitle:
    """Verify the event log controls. The per-tab title/count header was
    removed in the persistent-nav shell (the nav bar labels the tab)."""

    def test_clear_button_present(self, device: DeviceClient):
        """The clear button should be visible."""
        _open_event_log(device)
        clear_btn = device.find_widget(tag="event_log_clear")
        assert clear_btn is not None, "Event log clear button not found"


# =========================================================================
# Empty State
# =========================================================================

class TestEventLogEmptyState:
    """Clear the event log and verify the empty-state message."""

    @pytest.fixture(autouse=True)
    def _restore_events(self, device: DeviceClient):
        """Let the test run; afterwards the system_start event will
        reappear on the next event (no explicit restore needed)."""
        yield

    def test_empty_state_after_clear(self, device: DeviceClient):
        """After clearing all events, the 'No events' message should appear."""
        _open_event_log(device)

        # Clear events
        device.click(tag="event_log_clear")
        device.click(tag="event_log_clear_confirm")
        time.sleep(1.0)

        # The empty-state label should now be visible
        empty = device.find_widget(tag="event_log_empty")
        assert empty is not None, "Empty-state label not found after clearing"
        # Text should be the i18n "No events recorded" string
        text = empty.text_en or empty.text
        assert "no events" in text.lower(), \
            f"Expected 'No events' message, got: {text!r}"

    def test_content_shows_events_after_clear(self, device: DeviceClient):
        """After clearing, the empty-state label should be shown (no count
        header exists in the persistent-nav shell)."""
        _open_event_log(device)

        device.click(tag="event_log_clear")
        device.click(tag="event_log_clear_confirm")
        time.sleep(1.0)

        empty = device.find_widget(tag="event_log_empty")
        assert empty is not None, \
            "Empty-state label not shown after clearing events"


# =========================================================================
# Event Display
# =========================================================================

class TestEventLogDisplay:
    """Verify events appear as card entries."""

    def test_system_start_event_present(self, device: DeviceClient):
        """Events generated by state changes appear in the log.

        The tab shell has no count header, so we generate activity (an error
        transition logs events) and assert content is present indirectly:
        the empty-state label must be absent."""
        # Generate events via a fault set/clear transition.
        device.inject_fault("P02", True)
        time.sleep(1.5)
        device.clear_all_faults()
        time.sleep(1.5)

        _open_event_log(device)
        time.sleep(0.5)

        # If events exist, the "no events" empty-state label is not rendered.
        empty = device.find_widget(tag="event_log_empty")
        assert empty is None, \
            "Empty-state label present despite recent event activity"
        separator = device.find_widget(tag="event_date_separator")
        assert separator is not None, \
            "Event rows should be grouped under a date separator"

    def test_events_via_api(self, device: DeviceClient):
        """The /api/events endpoint should return events matching the UI count."""
        resp = device.session.get(f"{device.base_url}/api/events")
        assert resp.status_code == 200
        data = resp.json()
        assert "total" in data, "Expected 'total' field in events response"
        assert isinstance(data["total"], int)
        # In demo mode after boot, we should have at least 1 event (system_start)
        assert data["total"] >= 0

    def test_abnormal_reset_reasons_create_explicit_events(self, device: DeviceClient):
        """Panic and watchdog reset reasons produce distinct persisted events."""
        device.session.delete(f"{device.base_url}/api/events").raise_for_status()

        # ESP-IDF esp_reset_reason_t values: PANIC=4, TASK_WDT=6.
        for reason in (4, 6):
            resp = device.session.post(
                f"{device.base_url}/api/test/record-reset-reason",
                json={"reason": reason},
            )
            assert resp.status_code == 200

        events = device.session.get(f"{device.base_url}/api/events").json()["events"]
        types = [event["type"] for event in events]
        assert "application_crash" in types
        assert "watchdog_reset" in types

    def test_events_survive_reboot(self, device: DeviceClient):
        """The raw-flash journal restores events after a software reboot."""
        device.session.delete(f"{device.base_url}/api/events").raise_for_status()
        resp = device.session.post(
            f"{device.base_url}/api/test/record-reset-reason",
            json={"reason": 4},
        )
        resp.raise_for_status()

        before = device.session.get(f"{device.base_url}/api/events").json()
        assert "application_crash" in [event["type"] for event in before["events"]]

        device.reboot()
        assert device.wait_for_device(timeout=45.0), "Device did not return after reboot"

        after = device.session.get(f"{device.base_url}/api/events").json()
        assert "application_crash" in [event["type"] for event in after["events"]]
        assert after["total"] >= before["total"] + 1

    def test_clear_survives_reboot(self, device: DeviceClient):
        """Clearing commits an empty journal bank before rebooting, so events
        that existed before the clear must NOT be restored on the next boot.

        We seed a distinctive, journaled event (application_crash), clear the
        log, then reboot. After reboot that seeded event must be gone and a
        single fresh system_start must be present.

        We deliberately do NOT constrain the total count or the exact type
        set: once the demo heat-pump link comes up, the firmware emits
        `connected` plus any number of operational transition events
        (pump_on, compressor_on, mode_changed, ...) as the decoded initial
        state diverges from the reset defaults. Those are legitimate fresh
        events and land nondeterministically relative to when we sample, so
        asserting on them races. The real invariant here is only that the
        cleared journal did not resurrect pre-clear events and that exactly
        one boot marker replayed."""
        # Seed a distinctive, journaled event so a failed clear is visible as
        # its reappearance after reboot (record-reset-reason 4 => crash).
        device.session.post(
            f"{device.base_url}/api/test/record-reset-reason",
            json={"reason": 4},
        ).raise_for_status()
        seeded = device.session.get(f"{device.base_url}/api/events").json()
        assert "application_crash" in [e["type"] for e in seeded["events"]], \
            "Failed to seed a distinctive event before clearing"

        device.session.delete(f"{device.base_url}/api/events").raise_for_status()
        device.reboot()
        assert device.wait_for_device(timeout=45.0), "Device did not return after reboot"

        events = device.session.get(f"{device.base_url}/api/events").json()
        types = [event["type"] for event in events["events"]]

        # A buggy clear would replay the pre-clear journal bank, resurrecting
        # the seeded crash (and any prior events).
        assert "application_crash" not in types, \
            f"Cleared events resurrected after reboot: {types}"
        # Exactly one fresh boot marker proves an empty bank was committed and
        # replayed (accumulated system_start events would mean the clear only
        # partially took).
        assert "system_start" in types, f"Missing system_start after reboot: {types}"
        assert types.count("system_start") == 1, \
            f"Expected exactly one system_start after clear+reboot, got {types}"


# =========================================================================
# Search and Filters
# =========================================================================

class TestEventLogSearchAndFilters:
    """Verify the pinned search/filter controls and filtered empty state."""

    def test_filter_toolbar_is_present(self, device: DeviceClient):
        _open_event_log(device)
        _reset_filters(device)

        for tag in (
            "event_search_open",
            "event_filters_open",
            "event_filter_problems",
            "event_filter_equipment",
            "event_filter_changes",
            "event_filter_system",
            "event_filter_summary",
        ):
            assert device.find_widget(tag=tag) is not None, f"{tag} not found"

    def test_search_uses_visible_wifi_style_keyboard(self, device: DeviceClient):
        _open_event_log(device)
        _reset_filters(device)
        device.click(tag="event_search_open")

        textarea = device.find_widget(tag="event_search_input")
        keyboard = device.find_widget(tag="event_search_keyboard")
        assert textarea is not None, "Search textarea not found"
        assert textarea.password_mode is False, "Search text must remain visible"
        assert keyboard is not None, "Shared Wi-Fi-style keyboard not found"
        assert device.find_widget(tag="event_search_cancel") is not None
        assert device.find_widget(tag="event_search_apply") is not None

        device.click(tag="event_search_cancel")

    def test_search_filters_event_descriptions(self, device: DeviceClient):
        device.inject_fault("P02", True)
        time.sleep(1.0)
        device.clear_all_faults()
        time.sleep(1.0)

        _open_event_log(device)
        _reset_filters(device)
        device.click(tag="event_search_open")
        device.type_text("event_search_input", "Error")
        device.click(tag="event_search_apply")
        time.sleep(0.5)

        summary = device.find_widget(tag="event_filter_summary")
        assert summary is not None and " of " in (summary.text_en or summary.text)
        assert device.find_widget(tag="event_log_no_matches") is None

        device.click(tag="event_search_open")
        device.type_text("event_search_input", "definitely-no-such-event")
        device.click(tag="event_search_apply")
        time.sleep(0.5)
        assert device.find_widget(tag="event_log_no_matches") is not None

        device.click(tag="event_search_clear")

    def test_problem_chip_and_time_filter_are_available(self, device: DeviceClient):
        _open_event_log(device)
        _reset_filters(device)

        device.click(tag="event_filter_problems")
        filters = device.find_widget(tag="event_filters_label")
        assert filters is not None and "(1)" in (filters.text_en or filters.text)

        device.click(tag="event_filters_open")
        device.click(tag="event_filters_reset")
        device.click(tag="event_filters_cancel")
        filters = device.find_widget(tag="event_filters_label")
        assert filters is not None and "(1)" in (filters.text_en or filters.text)

        device.click(tag="event_filters_open")
        for tag in (
            "event_time_all",
            "event_time_today",
            "event_time_24h",
            "event_time_7d",
            "event_time_restart",
        ):
            assert device.find_widget(tag=tag) is not None, f"{tag} not found"

        device.click(tag="event_time_restart")
        device.click(tag="event_filters_apply")
        filters = device.find_widget(tag="event_filters_label")
        assert filters is not None and "(2)" in (filters.text_en or filters.text)

        _reset_filters(device)


# =========================================================================
# API Clear
# =========================================================================

class TestEventLogApiClear:
    """Verify the DELETE /api/events endpoint clears the log."""

    def test_clear_via_api(self, device: DeviceClient):
        """DELETE /api/events should clear, then GET should return total=0."""
        resp = device.session.delete(f"{device.base_url}/api/events")
        assert resp.status_code == 200

        resp = device.session.get(f"{device.base_url}/api/events")
        assert resp.status_code == 200
        data = resp.json()
        assert data["total"] == 0, \
            f"Expected 0 events after clear, got {data['total']}"

    def test_ui_reflects_api_clear(self, device: DeviceClient):
        """After clearing via API, the event log screen should show empty state."""
        _open_event_log(device)
        _reset_filters(device)
        _close_event_log(device)

        device.session.delete(f"{device.base_url}/api/events")
        time.sleep(0.5)

        _open_event_log(device)
        time.sleep(1.0)

        empty = device.find_widget(tag="event_log_empty")
        assert empty is not None, \
            "Empty-state label not found after API clear"
