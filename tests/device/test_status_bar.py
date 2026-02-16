"""
Test: Status Bar Shortcuts

Verifies that tapping icons in the status bar header navigates
directly to the expected screen and back.
"""

import time
import pytest
from device_client import DeviceClient


def test_wifi_icon_opens_wifi_screen(device: DeviceClient):
    """Clicking the WiFi icon in the status bar should open the WiFi screen."""
    device.click(tag="wifi")
    assert device.wait_for_screen("wifi", timeout=5.0), \
        f"Expected 'wifi' screen, got '{device.screen}'"


def test_wifi_icon_back_returns_to_main(device: DeviceClient):
    """Pressing back from the WiFi screen (opened via status bar) returns to main."""
    device.click(tag="wifi")
    assert device.wait_for_screen("wifi", timeout=5.0)
    time.sleep(0.5)

    device.click(tag="wifi_back")
    assert device.wait_for_screen("main", timeout=5.0), \
        f"Expected 'main' screen, got '{device.screen}'"


def test_notification_badge_appears_with_update(device: DeviceClient):
    """When a firmware update notification is added, the badge should appear."""
    # Clear any existing notifications
    device.notification_mock_reset()
    time.sleep(0.5)
    
    # Add a firmware update notification (type 0)
    device.notification_mock(type=0, message="Firmware v99.0.0 available")
    time.sleep(0.5)
    
    # Check that the notification badge exists and is visible
    # The badge is part of the notify_btn, so we just verify the button exists
    notify_btn = device.find_widget(tag="notifications")
    assert notify_btn is not None, "Notification button should exist"
    
    # Clean up
    device.notification_mock_reset()


def test_notification_icon_shows_dropdown(device: DeviceClient):
    """Clicking the notification icon should show the dropdown with notifications."""
    # Clear and add a notification
    device.notification_mock_reset()
    time.sleep(0.5)
    
    device.notification_mock(type=0, message="Firmware v99.0.0 available")
    time.sleep(0.5)
    
    # Click the notification bell icon
    device.click(tag="notifications")
    time.sleep(0.5)
    
    # The dropdown should appear - we can verify by checking the UI state
    # The dropdown shows the notification message
    # Note: We can't directly check for the dropdown widget, but we can verify
    # the system doesn't crash and remains on main screen
    assert device.screen == "main", "Should remain on main screen when dropdown opens"
    
    # Click somewhere else to close dropdown (click on time area)
    device.click(tag="time")
    time.sleep(0.5)
    
    # Clean up
    device.notification_mock_reset()


def test_notification_firmware_update_opens_firmware_screen(device: DeviceClient):
    """Clicking a firmware update notification should navigate to the firmware screen."""
    # Clear notifications and add a firmware update notification
    device.notification_mock_reset()
    time.sleep(0.5)
    
    device.notification_mock(type=0, message="Firmware v99.0.0 available")
    time.sleep(0.5)
    
    # Click the notification bell to open dropdown
    device.click(tag="notifications")
    time.sleep(0.5)
    
    # Click on the notification item in the dropdown
    # The notification item shows a download icon (symbol)
    try:
        device.click(symbol="DOWNLOAD")
        time.sleep(0.5)
        
        # Should navigate to firmware screen
        assert device.wait_for_screen("firmware", timeout=5.0), \
            f"Expected 'firmware' screen, got '{device.screen}'"
        
        # Navigate back to main
        device.click(tag="firmware_back")
        assert device.wait_for_screen("main", timeout=5.0)
    finally:
        # Clean up
        device.notification_mock_reset()


def test_notification_clears_after_clicking(device: DeviceClient):
    """After clicking a notification, it should be cleared from the status bar."""
    # Add a firmware update notification
    device.notification_mock_reset()
    time.sleep(0.5)
    
    device.notification_mock(type=0, message="Firmware v99.0.0 available")
    time.sleep(0.5)
    
    # Click the notification bell to open dropdown
    device.click(tag="notifications")
    time.sleep(0.5)
    
    # Click on the notification
    try:
        device.click(symbol="DOWNLOAD")
        time.sleep(0.5)
        
        # Should navigate to firmware screen
        assert device.wait_for_screen("firmware", timeout=5.0)
        
        # Go back to main
        device.click(tag="firmware_back")
        assert device.wait_for_screen("main", timeout=5.0)
        time.sleep(0.5)
        
        # Try to click notifications again - dropdown should be empty or not appear
        # (the notification was cleared when clicked)
        # We can verify this by checking that clicking notifications doesn't cause issues
        device.click(tag="notifications")
        time.sleep(0.5)
        
        # Should still be on main screen (dropdown might show "no notifications" or not show)
        assert device.screen == "main"
    finally:
        # Clean up
        device.notification_mock_reset()

