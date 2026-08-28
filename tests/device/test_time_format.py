"""
Test: 12/24-Hour Time Format Toggle

Verifies that toggling the time format switch changes:
1. The time preview on the time settings screen
2. The preferences API response
3. The time display on the main screen status bar

Time format is global device state; every test that toggles it requests the
``time_format_restore`` fixture, which captures the initial ``format_24h``
value from the preferences API and restores it on teardown (F-10).
"""

import re
import pytest
from device_client import DeviceClient
from conftest import _return_to_main


# Patterns for detecting 12h vs 24h format
_12H_PATTERN = re.compile(r"\d{1,2}:\d{2}(:\d{2})?\s*(AM|PM)", re.IGNORECASE)
_24H_PATTERN = re.compile(r"^\d{1,2}:\d{2}(:\d{2})?$")


def _is_12h(text: str) -> bool:
    """Return True if the time string is in 12-hour format (contains AM/PM)."""
    return bool(_12H_PATTERN.search(text))


def _is_24h(text: str) -> bool:
    """Return True if the time string is in 24-hour format (no AM/PM)."""
    return bool(_24H_PATTERN.match(text.strip()))


def _navigate_to_time_screen(device: DeviceClient):
    """Open settings → time sub-screen."""
    device.click(tag="settings")
    device.wait_for_screen("settings", timeout=5.0)

    device.click(tag="settings_time")
    device.wait_for_screen("time", timeout=5.0)


def _switch_checked(device: DeviceClient):
    sw = device.find_widget(tag="time_format_switch")
    return sw.checked if sw is not None else None


def _set_format_24h(device: DeviceClient, want_24h: bool):
    """Toggle the time-format switch until it matches ``want_24h`` (on the time screen)."""
    if _switch_checked(device) != want_24h:
        device.toggle("time_format_switch")
        device.wait_until(
            f"time-format switch is {'24h' if want_24h else '12h'}",
            lambda: _switch_checked(device) == want_24h,
            timeout=5.0,
        )


def _get_main_screen_time_text(device: DeviceClient) -> str:
    """Get the time text displayed in the main screen status bar.

    The time label is a child of the 'time' button — it's the first
    label widget right after the time button in the widget list.
    """
    widgets = device.widgets
    for i, w in enumerate(widgets):
        if w.tag == "time" and i + 1 < len(widgets):
            next_w = widgets[i + 1]
            if next_w.type == "label" and next_w.text:
                return next_w.text.strip()
    return ""


@pytest.fixture
def time_format_restore(device: DeviceClient):
    """Capture the initial 12/24h preference and restore it on teardown."""
    initial_24h = device.get_preferences().get("format_24h")

    yield

    if initial_24h is None:
        return
    if device.get_preferences().get("format_24h") != initial_24h:
        _return_to_main(device)
        _navigate_to_time_screen(device)
        _set_format_24h(device, initial_24h)
        device.wait_until(
            f"time format restored to {'24h' if initial_24h else '12h'}",
            lambda: device.get_preferences().get("format_24h") == initial_24h,
            timeout=5.0, raise_on_timeout=False,
        )


def test_toggle_to_24h_format(device: DeviceClient, time_format_restore):
    """Toggling the switch to 24h should change the time preview format."""
    _navigate_to_time_screen(device)

    sw = device.find_widget(tag="time_format_switch")
    assert sw is not None, "Could not find time_format_switch"

    # Start from 12h so we exercise the 12h → 24h toggle.
    _set_format_24h(device, False)
    _set_format_24h(device, True)

    # Verify the time preview label shows 24h format (no AM/PM)
    device.wait_until(
        "time preview shows 24h format",
        lambda: _is_24h((device.find_widget(tag="time_preview") or sw).text or ""),
        timeout=5.0,
    )
    preview = device.find_widget(tag="time_preview")
    assert preview is not None and _is_24h(preview.text), \
        f"Expected 24h format (no AM/PM), got '{getattr(preview, 'text', None)}'"

    # Verify via preferences API
    prefs = device.get_preferences()
    assert prefs["format_24h"] is True, \
        f"Expected format_24h=True, got {prefs['format_24h']}"


def test_toggle_to_12h_format(device: DeviceClient, time_format_restore):
    """Toggling the switch to 12h should show AM/PM in the time preview."""
    _navigate_to_time_screen(device)

    sw = device.find_widget(tag="time_format_switch")
    assert sw is not None, "Could not find time_format_switch"

    # Start from 24h so we exercise the 24h → 12h toggle.
    _set_format_24h(device, True)
    _set_format_24h(device, False)

    # Verify the time preview label shows 12h format (has AM/PM)
    device.wait_until(
        "time preview shows 12h format",
        lambda: _is_12h((device.find_widget(tag="time_preview") or sw).text or ""),
        timeout=5.0,
    )
    preview = device.find_widget(tag="time_preview")
    assert preview is not None and _is_12h(preview.text), \
        f"Expected 12h format (with AM/PM), got '{getattr(preview, 'text', None)}'"

    # Verify via preferences API
    prefs = device.get_preferences()
    assert prefs["format_24h"] is False, \
        f"Expected format_24h=False, got {prefs['format_24h']}"


def test_main_screen_shows_24h(device: DeviceClient, time_format_restore):
    """After switching to 24h, the main screen status bar should show 24h format."""
    _navigate_to_time_screen(device)
    _set_format_24h(device, True)

    # Go back to main screen
    device.click(tag="time_back")
    device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_close")
    device.wait_for_screen("main", timeout=5.0)
    device.wait_for_widget(tag="settings", timeout=5.0)

    # Wait until the status bar timer refreshes the time display in 24h format.
    device.wait_until(
        "main status bar shows 24h time",
        lambda: _is_24h(_get_main_screen_time_text(device)),
        timeout=15.0, poll=0.5,
    )


def test_main_screen_shows_12h(device: DeviceClient, time_format_restore):
    """After switching to 12h, the main screen status bar should show AM/PM."""
    _navigate_to_time_screen(device)
    _set_format_24h(device, False)

    # Go back to main screen
    device.click(tag="time_back")
    device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_close")
    device.wait_for_screen("main", timeout=5.0)
    device.wait_for_widget(tag="settings", timeout=5.0)

    # Wait until the status bar timer refreshes the time display in 12h format.
    device.wait_until(
        "main status bar shows 12h time",
        lambda: _is_12h(_get_main_screen_time_text(device)),
        timeout=15.0, poll=0.5,
    )
