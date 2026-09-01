"""
Test: Timezone Selection

Verifies that changing the timezone via the roller widget:
1. Updates the roller's selected text on the time screen
2. Changes the time preview to reflect the new timezone
3. Is reported correctly in the preferences API

Timezone is global device state, so every test that mutates it requests the
``timezone_restore`` fixture, which captures the roller index up front and
restores it on teardown (F-10). There is no ``set-preference`` API for the
timezone — the firmware only exposes it through the roller — so restoration
navigates the UI and re-sets the roller to the captured index.
"""

import pytest
from device_client import DeviceClient
from conftest import _return_to_main


# Timezone roller entries (must match firmware order in settings_time_screen.cpp)
TIMEZONES = [
    {"index": 0,  "name": "US Eastern (EST/EDT)",      "tz": "EST5EDT,M3.2.0,M11.1.0"},
    {"index": 3,  "name": "US Pacific (PST/PDT)",      "tz": "PST8PDT,M3.2.0,M11.1.0"},
    {"index": 9,  "name": "UK / London (GMT/BST)",      "tz": "GMT0BST,M3.5.0/1,M10.5.0"},
    {"index": 12, "name": "Japan (JST)",                "tz": "JST-9"},
    {"index": 20, "name": "UTC (No offset)",            "tz": "UTC0"},
]


def _navigate_to_time_screen(device: DeviceClient):
    """Open settings → time sub-screen (from the main screen)."""
    device.click(tag="settings")
    device.wait_for_screen("settings", timeout=5.0)

    device.click(tag="settings_time")
    device.wait_for_screen("time", timeout=5.0)


def _tz_auto_checked(device: DeviceClient):
    """Return the automatic-timezone switch state (True/False), or None if absent."""
    sw = device.find_widget(tag="tz_auto_switch")
    return sw.checked if sw is not None else None


def _ensure_manual_tz(device: DeviceClient):
    """Switch the timezone card into manual mode so the roller is visible.

    Timezone is derived automatically from the selected location by default
    (``tz_auto`` on), which hides the manual roller. Every roller-based test
    must turn automatic mode off first.
    """
    if _tz_auto_checked(device):
        device.toggle("tz_auto_switch")
        device.wait_until(
            "manual timezone roller is visible",
            lambda: device.find_widget(tag="timezone_roller") is not None
            and _tz_auto_checked(device) is False,
            timeout=5.0,
        )


def _roller_index(device: DeviceClient):
    roller = device.find_widget(tag="timezone_roller")
    return roller.value if roller is not None else None


@pytest.fixture
def timezone_restore(device: DeviceClient):
    """Capture the timezone state (auto flag + roller index) and restore it.

    Timezone is global device state. Tests here force manual mode and set an
    explicit zone, so teardown restores both the original automatic-vs-manual
    choice and, when originally manual, the roller index (F-10).
    """
    _navigate_to_time_screen(device)
    initial_auto = _tz_auto_checked(device)
    _ensure_manual_tz(device)
    initial_index = _roller_index(device)
    _return_to_main(device)

    yield

    _return_to_main(device)
    _navigate_to_time_screen(device)
    if initial_auto:
        # Originally automatic — turning it back on re-derives the zone.
        if _tz_auto_checked(device) is False:
            device.toggle("tz_auto_switch")
            device.wait_until(
                "automatic timezone restored",
                lambda: _tz_auto_checked(device) is True,
                timeout=5.0, raise_on_timeout=False,
            )
    else:
        _ensure_manual_tz(device)
        if initial_index is not None and _roller_index(device) != initial_index:
            device.set_roller("timezone_roller", initial_index)
            device.wait_until(
                f"timezone roller restored to index {initial_index}",
                lambda: _roller_index(device) == initial_index,
                timeout=5.0, raise_on_timeout=False,
            )


def test_roller_shows_current_timezone(device: DeviceClient):
    """The timezone roller should be visible and show the current timezone."""
    _navigate_to_time_screen(device)
    _ensure_manual_tz(device)

    roller = device.find_widget(tag="timezone_roller")
    assert roller is not None, "Could not find timezone_roller widget"
    assert roller.type == "roller", f"Expected type 'roller', got '{roller.type}'"
    assert roller.selected_text, "Roller should have a selected timezone name"
    assert roller.option_count is not None and roller.option_count > 0, \
        "Roller should have options"


def test_change_timezone_updates_roller(device: DeviceClient, timezone_restore):
    """Changing the roller index should update the selected timezone text."""
    _navigate_to_time_screen(device)
    _ensure_manual_tz(device)

    # Pick a timezone different from the current one
    current_index = _roller_index(device)

    # Choose UTC (index 20) unless already there, then choose Eastern (0)
    target = TIMEZONES[-1] if current_index != 20 else TIMEZONES[0]

    result = device.set_roller("timezone_roller", target["index"])
    assert result["success"] is True
    assert result["selected_text"] == target["name"], \
        f"Expected '{target['name']}', got '{result['selected_text']}'"

    # Verify the roller widget shows the new selection
    device.wait_until(
        f"roller settled on index {target['index']}",
        lambda: _roller_index(device) == target["index"],
        timeout=5.0,
    )
    roller = device.find_widget(tag="timezone_roller")
    assert roller.selected_text == target["name"]


def test_change_timezone_updates_preview(device: DeviceClient, timezone_restore):
    """Changing timezone should update the time preview label."""
    _navigate_to_time_screen(device)
    _ensure_manual_tz(device)

    # Read current preview
    preview_before = device.find_widget(tag="time_preview")
    assert preview_before is not None
    text_before = preview_before.text

    # Switch to a very different timezone to ensure the time changes
    current_index = _roller_index(device)

    # Pick Japan (UTC+9) or US Pacific (UTC-8) — whichever is different
    target = TIMEZONES[3] if current_index != 12 else TIMEZONES[1]

    device.set_roller("timezone_roller", target["index"])

    # The preview updates on the next preview-timer tick — wait for it to change.
    device.wait_until(
        "time preview reflects new timezone",
        lambda: (device.find_widget(tag="time_preview") or preview_before).text != text_before,
        timeout=5.0,
    )
    preview_after = device.find_widget(tag="time_preview")
    assert preview_after is not None
    assert text_before != preview_after.text, \
        f"Time preview should change after timezone switch, but both show '{text_before}'"


def test_timezone_reflected_in_preferences(device: DeviceClient, timezone_restore):
    """The preferences API should report the current timezone string."""
    _navigate_to_time_screen(device)
    _ensure_manual_tz(device)

    # Set to a known timezone
    target = TIMEZONES[4]  # UTC
    device.set_roller("timezone_roller", target["index"])

    device.wait_until(
        f"preferences report timezone '{target['tz']}'",
        lambda: device.get_preferences().get("timezone") == target["tz"],
        timeout=5.0,
    )
