"""
Test: Settings Sub-Screen Navigation

Verifies that each settings menu row navigates to its sub-screen
and the back button returns to the settings menu.
"""

import time
import pytest
from device_client import DeviceClient


SUB_SCREENS = [
    ("settings_wifi", "wifi", "wifi_back"),
    ("settings_firmware", "firmware", "firmware_back"),
    ("settings_time", "time", "time_back"),
    ("settings_language", "language", "language_back"),
    ("settings_display", "display", "display_back"),
    (
        "settings_home_assistant",
        "home_assistant",
        "home_assistant_back",
    ),
]


@pytest.mark.parametrize("row_tag,screen_name,back_tag", SUB_SCREENS,
                         ids=[s[1] for s in SUB_SCREENS])
def test_open_sub_screen(device: DeviceClient, row_tag, screen_name, back_tag):
    """Clicking a settings row should open the corresponding sub-screen."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0), \
        f"Settings did not open — on '{device.screen}'"
    time.sleep(0.5)

    device.click(tag=row_tag)
    assert device.wait_for_screen(screen_name, timeout=5.0), \
        f"Expected '{screen_name}' screen, got '{device.screen}'"


@pytest.mark.parametrize("row_tag,screen_name,back_tag", SUB_SCREENS,
                         ids=[s[1] for s in SUB_SCREENS])
def test_back_from_sub_screen(device: DeviceClient, row_tag, screen_name, back_tag):
    """Pressing back from a sub-screen should return to the settings menu."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    device.click(tag=row_tag)
    assert device.wait_for_screen(screen_name, timeout=5.0)
    time.sleep(0.5)

    device.click(tag=back_tag)
    assert device.wait_for_screen("settings", timeout=5.0), \
        f"Did not return to settings — on '{device.screen}'"
