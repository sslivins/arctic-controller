"""
Test: 12/24-Hour Time Format Toggle

Verifies that toggling the time format switch changes:
1. The time preview on the time settings screen
2. The preferences API response
3. The time display on the main screen status bar
"""

import re
import time
import pytest
from device_client import DeviceClient


# Patterns for detecting 12h vs 24h format
_12H_PATTERN = re.compile(r"\d{1,2}:\d{2}(:\d{2})?\s*(AM|PM)", re.IGNORECASE)
_24H_PATTERN = re.compile(r"^\d{1,2}:\d{2}(:\d{2})?$")


def _is_12h(text: str) -> bool:
    """Return True if the time string is in 12-hour format (contains AM/PM)."""
    return bool(_12H_PATTERN.search(text))


def _is_24h(text: str) -> bool:
    """Return True if the time string is in 24-hour format (no AM/PM)."""
    return bool(_24H_PATTERN.match(text.strip()))


# Capture the initial time format so we can restore it after tests
_initial_format_24h = None


def _navigate_to_time_screen(device: DeviceClient):
    """Open settings → time sub-screen."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    device.click(tag="settings_time")
    assert device.wait_for_screen("time", timeout=5.0)
    time.sleep(0.5)


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


def test_toggle_to_24h_format(device: DeviceClient):
    """Toggling the switch to 24h should change the time preview format."""
    global _initial_format_24h
    if _initial_format_24h is None:
        _initial_format_24h = device.get_preferences()["format_24h"]

    _navigate_to_time_screen(device)

    # Read current switch state
    sw = device.find_widget(tag="time_format_switch")
    assert sw is not None, "Could not find time_format_switch"

    # If already in 24h mode, switch to 12h first so we can test the toggle
    if sw.checked:
        device.toggle("time_format_switch")
        time.sleep(0.5)

    # Now switch to 24h
    device.toggle("time_format_switch")
    time.sleep(0.5)

    # Verify switch is checked (24h)
    sw = device.find_widget(tag="time_format_switch")
    assert sw.checked is True, "Switch should be checked for 24h mode"

    # Verify the time preview label shows 24h format (no AM/PM)
    preview = device.find_widget(tag="time_preview")
    assert preview is not None, "Could not find time_preview label"
    assert _is_24h(preview.text), \
        f"Expected 24h format (no AM/PM), got '{preview.text}'"

    # Verify via preferences API
    prefs = device.get_preferences()
    assert prefs["format_24h"] is True, \
        f"Expected format_24h=True, got {prefs['format_24h']}"


def test_toggle_to_12h_format(device: DeviceClient):
    """Toggling the switch to 12h should show AM/PM in the time preview."""
    _navigate_to_time_screen(device)

    # Read current switch state
    sw = device.find_widget(tag="time_format_switch")
    assert sw is not None, "Could not find time_format_switch"

    # If already in 12h mode, switch to 24h first so we can test the toggle
    if not sw.checked:
        device.toggle("time_format_switch")
        time.sleep(0.5)

    # Now switch to 12h
    device.toggle("time_format_switch")
    time.sleep(0.5)

    # Verify switch is unchecked (12h)
    sw = device.find_widget(tag="time_format_switch")
    assert sw.checked is False, "Switch should be unchecked for 12h mode"

    # Verify the time preview label shows 12h format (has AM/PM)
    preview = device.find_widget(tag="time_preview")
    assert preview is not None, "Could not find time_preview label"
    assert _is_12h(preview.text), \
        f"Expected 12h format (with AM/PM), got '{preview.text}'"

    # Verify via preferences API
    prefs = device.get_preferences()
    assert prefs["format_24h"] is False, \
        f"Expected format_24h=False, got {prefs['format_24h']}"


def test_main_screen_shows_24h(device: DeviceClient):
    """After switching to 24h, the main screen status bar should show 24h format."""
    # First ensure we're in 24h mode
    _navigate_to_time_screen(device)

    sw = device.find_widget(tag="time_format_switch")
    if not sw.checked:
        device.toggle("time_format_switch")
        time.sleep(0.5)

    # Go back to main screen
    device.click(tag="time_back")
    device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.3)
    device.click(tag="settings_close")
    device.wait_for_screen("main", timeout=5.0)
    device.wait_for_widget(tag="settings", timeout=5.0)

    # Poll until the status bar timer refreshes the time display (up to 12s)
    deadline = time.time() + 12
    time_text = ""
    while time.time() < deadline:
        time_text = _get_main_screen_time_text(device)
        if time_text and _is_24h(time_text):
            break
        time.sleep(1)

    assert _is_24h(time_text), \
        f"Main screen should show 24h format, got '{time_text}'"


def test_main_screen_shows_12h(device: DeviceClient):
    """After switching to 12h, the main screen status bar should show AM/PM."""
    # First ensure we're in 12h mode
    _navigate_to_time_screen(device)

    sw = device.find_widget(tag="time_format_switch")
    if sw.checked:
        device.toggle("time_format_switch")
        time.sleep(0.5)

    # Go back to main screen
    device.click(tag="time_back")
    device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.3)
    device.click(tag="settings_close")
    device.wait_for_screen("main", timeout=5.0)
    device.wait_for_widget(tag="settings", timeout=5.0)

    # Poll until the status bar timer refreshes the time display (up to 12s)
    deadline = time.time() + 12
    time_text = ""
    while time.time() < deadline:
        time_text = _get_main_screen_time_text(device)
        if time_text and _is_12h(time_text):
            break
        time.sleep(1)

    assert _is_12h(time_text), \
        f"Main screen should show 12h format (with AM/PM), got '{time_text}'"


def test_restore_time_format(device: DeviceClient):
    """Restore time format to whatever it was before the tests ran."""
    target = _initial_format_24h if _initial_format_24h is not None else False
    prefs = device.get_preferences()

    if prefs["format_24h"] != target:
        _navigate_to_time_screen(device)
        device.toggle("time_format_switch")
        time.sleep(0.5)

    prefs = device.get_preferences()
    assert prefs["format_24h"] == target, \
        f"Expected format_24h={target} after restore, got {prefs['format_24h']}"
