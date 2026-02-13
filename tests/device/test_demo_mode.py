"""
Test: Demo Mode Toggle

Verifies that toggling the demo mode switch in the settings menu
changes the preference and dynamically shows/hides the demo mode
banner on the main screen without requiring a reboot.
"""

import time
import pytest
from device_client import DeviceClient

# Module-level storage for the initial demo mode state
_initial_demo_mode: bool | None = None


def test_toggle_demo_mode(device: DeviceClient):
    """Toggling demo mode switch should change the preference."""
    global _initial_demo_mode

    # Read and remember the initial state
    prefs = device.get_preferences()
    _initial_demo_mode = prefs["demo_mode"]

    # Open settings
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    # Toggle the switch
    result = device.toggle("demo_mode_switch")
    assert result["success"] is True
    expected = not _initial_demo_mode
    assert result["checked"] == expected, \
        f"Expected checked={expected}, got {result['checked']}"
    time.sleep(0.3)

    # Verify via preferences API
    prefs = device.get_preferences()
    assert prefs["demo_mode"] == expected, \
        f"Expected demo_mode={expected}, got {prefs['demo_mode']}"


def test_demo_banner_shown_when_enabled(device: DeviceClient):
    """When demo mode is ON, the main screen should show the demo banner."""
    # Ensure demo mode is ON
    prefs = device.get_preferences()
    if not prefs["demo_mode"]:
        device.click(tag="settings")
        assert device.wait_for_screen("settings", timeout=5.0)
        time.sleep(0.5)
        device.toggle("demo_mode_switch")
        time.sleep(0.3)

    # Go to main screen (close settings if open)
    if device.has_widget(tag="settings_close"):
        device.click(tag="settings_close")
        time.sleep(1.0)

    # Verify the demo banner is visible
    banner = device.wait_for_widget(tag="demo_banner", timeout=3.0)
    assert banner is not None, "Demo banner should be visible when demo mode is ON"


def test_demo_banner_hidden_when_disabled(device: DeviceClient):
    """When demo mode is OFF, the main screen should not show the demo banner."""
    # Ensure demo mode is OFF
    prefs = device.get_preferences()
    if prefs["demo_mode"]:
        device.click(tag="settings")
        assert device.wait_for_screen("settings", timeout=5.0)
        time.sleep(0.5)
        device.toggle("demo_mode_switch")
        time.sleep(0.3)

    # Go to main screen (close settings if open)
    if device.has_widget(tag="settings_close"):
        device.click(tag="settings_close")
        time.sleep(1.0)

    # Verify the demo banner is NOT visible
    assert not device.has_widget(tag="demo_banner"), \
        "Demo banner should NOT be visible when demo mode is OFF"


def test_restore_demo_mode(device: DeviceClient):
    """Restore demo mode to whatever it was before the test."""
    global _initial_demo_mode
    assert _initial_demo_mode is not None, "test_toggle_demo_mode must run first"

    prefs = device.get_preferences()
    current = prefs["demo_mode"]

    # Toggle back only if the current state differs from the initial state
    if current != _initial_demo_mode:
        device.click(tag="settings")
        assert device.wait_for_screen("settings", timeout=5.0)
        time.sleep(0.5)

        result = device.toggle("demo_mode_switch")
        assert result["success"] is True
        assert result["checked"] == _initial_demo_mode
        time.sleep(0.3)

    prefs = device.get_preferences()
    assert prefs["demo_mode"] == _initial_demo_mode, \
        f"Expected demo_mode={_initial_demo_mode}, got {prefs['demo_mode']}"
