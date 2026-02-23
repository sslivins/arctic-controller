"""
Test: Demo Mode Toggle & Reboot Confirmation

Verifies that toggling the demo mode switch in the settings menu:
  1. Immediately sets the preference
  2. Shows a reboot confirmation panel with Cancel and Restart buttons
  3. Cancel reverts the preference and dismisses the panel
  4. The demo mode preference can be restored via the test API

Note: We cannot test the Restart button because it calls esp_restart(),
which would reboot the device and disrupt the test session.
"""

import time
import pytest
from device_client import DeviceClient


def test_toggle_shows_reboot_panel(device: DeviceClient):
    """Toggling demo mode switch should show the reboot confirmation panel."""
    # Read the initial state
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
    time.sleep(0.5)

    # The reboot confirmation panel should appear
    assert device.has_widget(tag="reboot_overlay"), \
        "Reboot overlay should be visible after toggling demo mode"
    assert device.has_widget(tag="reboot_panel"), \
        "Reboot panel should be visible after toggling demo mode"

    # Preference should be set immediately (before user confirms)
    prefs = device.get_preferences()
    assert prefs["demo_mode"] == expected, \
        f"Preference should be set immediately: expected {expected}, got {prefs['demo_mode']}"


def test_reboot_panel_has_buttons(device: DeviceClient):
    """The reboot confirmation panel should have Cancel and Restart buttons."""
    # Panel should still be up from previous test
    assert device.has_widget(tag="reboot_panel"), \
        "Reboot panel should still be visible"
    assert device.has_widget(tag="reboot_cancel"), \
        "Cancel button should be present in reboot panel"
    assert device.has_widget(tag="reboot_confirm"), \
        "Restart button should be present in reboot panel"


def test_cancel_reverts_preference(device: DeviceClient):
    """Clicking Cancel should revert the preference and dismiss the panel."""
    # Read current preference (was toggled in test_toggle_shows_reboot_panel)
    prefs = device.get_preferences()
    toggled = prefs["demo_mode"]

    # Click Cancel
    device.click(tag="reboot_cancel")
    time.sleep(0.5)

    # Panel should be dismissed
    assert not device.has_widget(tag="reboot_overlay"), \
        "Reboot overlay should be dismissed after Cancel"
    assert not device.has_widget(tag="reboot_panel"), \
        "Reboot panel should be dismissed after Cancel"

    # Preference should be reverted
    prefs = device.get_preferences()
    expected = not toggled
    assert prefs["demo_mode"] == expected, \
        f"Preference should be reverted: expected {expected}, got {prefs['demo_mode']}"


def test_cancel_reverts_switch_state(device: DeviceClient):
    """After Cancel, the switch should be back to its original position."""
    # The switch should reflect the reverted state
    sw = device.find_widget(tag="demo_mode_switch")
    assert sw is not None, "Demo mode switch should exist on settings screen"

    prefs = device.get_preferences()
    expected_checked = prefs["demo_mode"]
    assert sw.checked == expected_checked, \
        f"Switch checked={sw.checked} should match preference {expected_checked}"


def test_restore_demo_mode(device: DeviceClient):
    """Restore demo mode to ON via API — other tests depend on demo mode."""
    prefs = device.get_preferences()
    if not prefs["demo_mode"]:
        device.set_preference(demo_mode=True)

    prefs = device.get_preferences()
    assert prefs["demo_mode"] is True, \
        f"Expected demo_mode=True, got {prefs['demo_mode']}"
