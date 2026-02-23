"""
Test: Demo Mode Toggle & Reboot Confirmation

Verifies that toggling the demo mode switch in the settings menu:
  1. Immediately sets the preference
  2. Shows a reboot confirmation panel with Cancel and Restart buttons
  3. Cancel reverts the preference, switch, and dismisses the panel
  4. The demo mode preference can be restored via the test API

Note: We cannot test the Restart button because it calls esp_restart(),
which would reboot the device and disrupt the test session.

Each test is self-contained — it navigates to settings and toggles the
switch itself, because the ensure_main_screen fixture returns to the
main screen (and dismisses the reboot overlay) between tests.
"""

import time
import pytest
from device_client import DeviceClient


def test_toggle_shows_reboot_panel_with_buttons(device: DeviceClient):
    """Toggling demo mode switch shows a reboot panel with Cancel and Restart."""
    prefs = device.get_preferences()
    initial = prefs["demo_mode"]

    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    result = device.toggle("demo_mode_switch")
    assert result["success"] is True
    expected = not initial
    assert result["checked"] == expected
    time.sleep(0.5)

    # Reboot confirmation panel should appear with overlay and both buttons
    assert device.has_widget(tag="reboot_overlay"), \
        "Reboot overlay should be visible after toggling demo mode"
    assert device.has_widget(tag="reboot_panel"), \
        "Reboot panel should be visible after toggling demo mode"
    assert device.has_widget(tag="reboot_cancel"), \
        "Cancel button should be present in reboot panel"
    assert device.has_widget(tag="reboot_confirm"), \
        "Restart button should be present in reboot panel"

    # Preference should be set immediately (before user confirms)
    prefs = device.get_preferences()
    assert prefs["demo_mode"] == expected


def test_cancel_reverts_and_dismisses(device: DeviceClient):
    """Clicking Cancel reverts the preference, switch, and dismisses the panel."""
    prefs = device.get_preferences()
    initial = prefs["demo_mode"]

    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    # Toggle to bring up the reboot panel
    device.toggle("demo_mode_switch")
    time.sleep(0.5)
    assert device.has_widget(tag="reboot_overlay")

    # Click Cancel
    device.click(tag="reboot_cancel")
    time.sleep(0.5)

    # Panel should be dismissed
    assert not device.has_widget(tag="reboot_overlay"), \
        "Reboot overlay should be dismissed after Cancel"
    assert not device.has_widget(tag="reboot_panel"), \
        "Reboot panel should be dismissed after Cancel"

    # Preference should be reverted to initial value
    prefs = device.get_preferences()
    assert prefs["demo_mode"] == initial

    # Switch should be back to its original position
    sw = device.find_widget(tag="demo_mode_switch")
    assert sw is not None
    assert sw.checked == initial


def test_restore_demo_mode(device: DeviceClient):
    """Restore demo mode to ON via API — other tests depend on demo mode."""
    prefs = device.get_preferences()
    if not prefs["demo_mode"]:
        device.set_preference(demo_mode=True)

    prefs = device.get_preferences()
    assert prefs["demo_mode"] is True
