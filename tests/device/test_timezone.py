"""
Test: Timezone Selection

Verifies that changing the timezone via the roller widget:
1. Updates the roller's selected text on the time screen
2. Changes the time preview to reflect the new timezone
3. Is reported correctly in the preferences API
4. Restores the original timezone after tests
"""

import time
import pytest
from device_client import DeviceClient


# Timezone roller entries (must match firmware order in settings_time_screen.cpp)
TIMEZONES = [
    {"index": 0,  "name": "US Eastern (EST/EDT)",      "tz": "EST5EDT,M3.2.0,M11.1.0"},
    {"index": 3,  "name": "US Pacific (PST/PDT)",      "tz": "PST8PDT,M3.2.0,M11.1.0"},
    {"index": 9,  "name": "UK / London (GMT/BST)",      "tz": "GMT0BST,M3.5.0/1,M10.5.0"},
    {"index": 12, "name": "Japan (JST)",                "tz": "JST-9"},
    {"index": 20, "name": "UTC (No offset)",            "tz": "UTC0"},
]

# Capture initial timezone so we can restore it
_initial_timezone = None
_initial_roller_index = None


def _navigate_to_time_screen(device: DeviceClient):
    """Open settings → time sub-screen."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    device.click(tag="settings_time")
    assert device.wait_for_screen("time", timeout=5.0)
    time.sleep(0.5)


def test_roller_shows_current_timezone(device: DeviceClient):
    """The timezone roller should be visible and show the current timezone."""
    global _initial_timezone, _initial_roller_index

    # Capture initial state from API
    prefs = device.get_preferences()
    _initial_timezone = prefs.get("timezone")

    _navigate_to_time_screen(device)

    roller = device.find_widget(tag="timezone_roller")
    assert roller is not None, "Could not find timezone_roller widget"
    assert roller.type == "roller", f"Expected type 'roller', got '{roller.type}'"
    assert roller.selected_text, "Roller should have a selected timezone name"
    assert roller.option_count is not None and roller.option_count > 0, \
        "Roller should have options"

    _initial_roller_index = roller.value


def test_change_timezone_updates_roller(device: DeviceClient):
    """Changing the roller index should update the selected timezone text."""
    _navigate_to_time_screen(device)

    # Pick a timezone different from the current one
    roller = device.find_widget(tag="timezone_roller")
    current_index = roller.value

    # Choose UTC (index 20) unless already there, then choose Eastern (0)
    target = TIMEZONES[-1] if current_index != 20 else TIMEZONES[0]

    result = device.set_roller("timezone_roller", target["index"])
    assert result["success"] is True
    assert result["selected_text"] == target["name"], \
        f"Expected '{target['name']}', got '{result['selected_text']}'"
    time.sleep(0.5)

    # Verify the roller widget shows the new selection
    roller = device.find_widget(tag="timezone_roller")
    assert roller.value == target["index"], \
        f"Expected roller index {target['index']}, got {roller.value}"
    assert roller.selected_text == target["name"]


def test_change_timezone_updates_preview(device: DeviceClient):
    """Changing timezone should update the time preview label."""
    _navigate_to_time_screen(device)

    # Read current preview
    preview_before = device.find_widget(tag="time_preview")
    assert preview_before is not None

    # Switch to a very different timezone to ensure the time changes
    roller = device.find_widget(tag="timezone_roller")
    current_index = roller.value

    # Pick Japan (UTC+9) or US Pacific (UTC-8) — whichever is different
    target = TIMEZONES[3] if current_index != 12 else TIMEZONES[1]

    device.set_roller("timezone_roller", target["index"])
    time.sleep(1)  # Give preview timer a moment

    preview_after = device.find_widget(tag="time_preview")
    assert preview_after is not None

    # The time text should be different (different timezone = different hour)
    assert preview_before.text != preview_after.text, \
        f"Time preview should change after timezone switch, " \
        f"but both show '{preview_before.text}'"


def test_timezone_reflected_in_preferences(device: DeviceClient):
    """The preferences API should report the current timezone string."""
    _navigate_to_time_screen(device)

    # Set to a known timezone
    target = TIMEZONES[4]  # UTC
    device.set_roller("timezone_roller", target["index"])
    time.sleep(0.5)

    prefs = device.get_preferences()
    assert prefs["timezone"] == target["tz"], \
        f"Expected timezone='{target['tz']}', got '{prefs['timezone']}'"


def test_restore_timezone(device: DeviceClient):
    """Restore the original timezone so other tests aren't affected."""
    if _initial_roller_index is None:
        pytest.skip("Initial timezone index not captured")

    _navigate_to_time_screen(device)

    roller = device.find_widget(tag="timezone_roller")
    if roller.value != _initial_roller_index:
        device.set_roller("timezone_roller", _initial_roller_index)
        time.sleep(0.5)

    # Verify restoration
    roller = device.find_widget(tag="timezone_roller")
    assert roller.value == _initial_roller_index, \
        f"Expected roller index {_initial_roller_index}, got {roller.value}"

    if _initial_timezone:
        prefs = device.get_preferences()
        assert prefs["timezone"] == _initial_timezone, \
            f"Expected timezone='{_initial_timezone}' after restore, got '{prefs['timezone']}'"
