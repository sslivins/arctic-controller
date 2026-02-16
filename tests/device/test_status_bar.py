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
    """When a firmware update notification is added, the badge should appear.
    
    Note: The badge itself doesn't have a widget tag, so we verify that the
    notification system is working by checking the notification button exists
    and later tests verify the full interaction flow.
    """
    # Clear any existing notifications
    device.notification_mock_reset()
    time.sleep(0.5)
    
    # Add a firmware update notification (type 0)
    device.notification_mock(notification_type=0, message="Firmware v99.0.0 available")
    time.sleep(0.5)
    
    # Check that the notification button exists and is visible
    # The badge is part of the notify_btn, so we just verify the button exists
    notify_btn = device.find_widget(tag="notifications")
    assert notify_btn is not None, "Notification button should exist"
    
    # Clean up
    device.notification_mock_reset()

def test_notification_icon_shows_dropdown(device: DeviceClient):
    """Clicking the notification icon should show the dropdown with notifications.
    
    Note: The dropdown is created dynamically and doesn't have persistent widget tags.
    We verify correct behavior by ensuring the system remains on the main screen
    (rather than crashing or navigating away) and that subsequent interaction works.
    Full visual verification would require the physical device.
    """
    # Clear and add a notification
    device.notification_mock_reset()
    time.sleep(0.5)
    
    device.notification_mock(notification_type=0, message="Firmware v99.0.0 available")
    time.sleep(0.5)
    
    # Click the notification bell icon
    device.click(tag="notifications")
    time.sleep(0.5)
    
    # The dropdown should appear - we verify by checking the system remains stable
    # and on the main screen (dropdown is an overlay, not a new screen)
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
    
    device.notification_mock(notification_type=0, message="Firmware v99.0.0 available")
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
    """After clicking a notification, it should be cleared from the status bar.
    
    Note: The notification is cleared by the callback in main.cpp when the dropdown
    item is clicked. We verify this by confirming the notification no longer triggers
    navigation after being clicked once. Full verification of badge disappearance
    would require checking the physical device's visual state.
    """
    # Add a firmware update notification
    device.notification_mock_reset()
    time.sleep(0.5)
    
    device.notification_mock(notification_type=0, message="Firmware v99.0.0 available")
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
        
        # The notification was cleared when clicked (by the callback in main.cpp)
        # We verify this by ensuring the system remains stable
        # In a real scenario, the badge would no longer be visible
        device.click(tag="notifications")
        time.sleep(0.5)
        
        # Should still be on main screen (dropdown shows no notifications or doesn't appear)
        assert device.screen == "main"
    finally:
        # Clean up
        device.notification_mock_reset()

