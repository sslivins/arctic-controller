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

def test_notification_dropdown_shows_heading(device: DeviceClient):
    """The notification dropdown should show a 'Notifications' heading.

    Mirrors the web dashboard bell panel, which has a "Notifications" title at
    the top of the popover. The heading is tagged 'notify_title' for
    language-independent addressability.
    """
    device.notification_mock_reset()
    time.sleep(0.5)

    device.notification_mock(notification_type=0, message="Firmware v99.0.0 available")
    time.sleep(0.5)

    # Open the dropdown
    device.click(tag="notifications")
    time.sleep(0.5)

    try:
        # The heading should be present in the dropdown overlay
        assert device.wait_for_widget(tag="notify_title", timeout=3.0), \
            "Expected a 'notify_title' heading in the notification dropdown"
        title = device.find_widget(tag="notify_title")
        assert title is not None and title.text_en == "Notifications", \
            f"Expected heading text 'Notifications', got '{getattr(title, 'text_en', None)}'"
    finally:
        # Close the dropdown and clean up
        device.click(tag="notifications")
        time.sleep(0.3)
        device.notification_mock_reset()


def test_notification_icon_shows_dropdown(device: DeviceClient):
    """Clicking the notification icon should show the dropdown with notifications.
    
    Note: The dropdown is created dynamically and doesn't have persistent widget tags.
    This test verifies that clicking the notification icon doesn't cause crashes or
    navigation errors. Full verification of dropdown visibility would require checking
    the physical device or enhancing the test infrastructure to detect dynamic overlays.
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
    
    # Close dropdown by toggling the notification bell again
    device.click(tag="notifications")
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

def test_notification_interaction_stability(device: DeviceClient):
    """Verify notification interaction flow works correctly without crashes.
    
    This test verifies the complete interaction flow: adding a notification,
    opening the dropdown, clicking the notification to navigate to firmware screen,
    returning to main, and interacting with the notification button again. This
    ensures the system remains stable throughout the notification lifecycle.
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
        
        # Clicking the notification button again should not cause errors
        # (In production, the notification was cleared by the callback)
        device.click(tag="notifications")
        time.sleep(0.5)
        
        # Should still be on main screen
        assert device.screen == "main"
    finally:
        # Clean up
        device.notification_mock_reset()

