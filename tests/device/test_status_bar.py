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
