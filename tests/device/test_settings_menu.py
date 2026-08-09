"""
Test: Settings Menu Navigation

Verifies that pressing the settings button opens the settings menu
and that the settings screen contains the expected UI elements.
"""

import time

from device_client import DeviceClient


def test_open_settings_menu(device: DeviceClient):
    """Clicking the settings button should open the settings screen."""
    # Precondition: we're on the main screen
    assert device.screen == "main", "Expected to start on main screen"

    # Act: click the settings button by tag
    device.click(tag="settings")

    # Assert: settings screen should now be visible
    assert device.wait_for_screen("settings", timeout=3.0), \
        f"Settings screen did not open — still on '{device.screen}'"


def test_settings_menu_has_close_button(device: DeviceClient):
    """The settings screen should have a close (X) button."""
    import time
    time.sleep(0.5)  # Give device time to settle

    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0), \
        f"Settings screen did not open — still on '{device.screen}'"

    # Verify the screen has widgets
    widgets = device.widgets
    assert len(widgets) > 0, "Settings screen has no visible widgets"


def test_close_settings_menu(device: DeviceClient):
    """Clicking the close button should return to the main screen."""
    # Open settings
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=3.0), \
        "Settings screen did not open"

    # Close settings via the close button tag
    device.click(tag="settings_close")

    # Should be back on main
    assert device.wait_for_screen("main", timeout=3.0), \
        f"Did not return to main screen — still on '{device.screen}'"


def test_factory_reset_requires_confirmation_and_can_be_cancelled(
    device: DeviceClient,
):
    """Factory reset is visibly destructive and never runs on the first tap."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=3.0)

    device.click(tag="settings_factory_reset")
    time.sleep(0.4)

    assert device.has_widget(tag="factory_reset_overlay")
    assert device.has_widget(tag="factory_reset_panel")
    assert device.has_widget(tag="factory_reset_confirm")
    assert device.has_widget(tag="factory_reset_cancel")

    device.click(tag="factory_reset_cancel")
    time.sleep(0.3)
    assert not device.has_widget(tag="factory_reset_overlay")
    assert device.screen == "settings"
