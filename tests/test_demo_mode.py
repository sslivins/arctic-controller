"""
Test: Demo Mode Toggle

Verifies that toggling the demo mode switch in the settings menu
changes the preference and is reflected back by the preferences API.
"""

import time
from device_client import DeviceClient


def test_toggle_demo_mode_on(device: DeviceClient):
    """Toggling demo mode switch should change the preference."""
    # Read initial state
    prefs = device.get_preferences()
    initial = prefs["demo_mode"]

    # Open settings
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    # Toggle the switch
    result = device.toggle("demo_mode_switch")
    assert result["success"] is True
    expected = not initial
    assert result["checked"] == expected, \
        f"Expected checked={expected}, got {result['checked']}"
    time.sleep(0.3)

    # Verify via preferences API
    prefs = device.get_preferences()
    assert prefs["demo_mode"] == expected, \
        f"Expected demo_mode={expected}, got {prefs['demo_mode']}"


def test_toggle_demo_mode_restore(device: DeviceClient):
    """Toggle demo mode back to its original state."""
    # Read current state
    prefs = device.get_preferences()
    current = prefs["demo_mode"]

    # Open settings
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    # If demo mode is on, toggle it off (restore to off)
    if current:
        result = device.toggle("demo_mode_switch")
        assert result["success"] is True
        assert result["checked"] is False
        time.sleep(0.3)

    prefs = device.get_preferences()
    assert prefs["demo_mode"] is False, \
        f"Expected demo_mode=False, got {prefs['demo_mode']}"
