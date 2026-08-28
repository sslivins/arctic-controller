"""
Test: Display Brightness Control

Navigates to Settings → Display, changes the brightness slider,
and verifies the brightness value label updates correctly.
"""

from device_client import DeviceClient


def test_navigate_to_display_screen(device: DeviceClient):
    """Clicking Display in settings should open the display screen."""
    assert device.screen == "main", "Expected to start on main screen"

    # Open settings
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0), \
        f"Settings screen did not open — still on '{device.screen}'"

    # Click the Display row
    device.click(tag="settings_display")
    assert device.wait_for_screen("display", timeout=5.0), \
        f"Display screen did not open — still on '{device.screen}'"


def test_change_brightness(device: DeviceClient):
    """Changing the brightness slider should update the value label."""
    # Navigate to display screen
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)

    device.click(tag="settings_display")
    assert device.wait_for_screen("display", timeout=5.0)

    # Read the current brightness slider value
    slider = device.find_widget(tag="brightness_slider")
    assert slider is not None, "Could not find brightness slider"
    original_value = slider.value
    assert original_value is not None, "Slider has no value"

    # Pick a new brightness value (different from current)
    new_value = 30 if original_value != 30 else 70

    # Set the slider to the new value
    result = device.set_slider("brightness_slider", new_value)
    assert result["success"] is True
    assert result["value"] == new_value

    # Wait for the value label to reflect the new brightness
    device.wait_until(
        f"brightness label shows {new_value}%",
        lambda: getattr(device.find_widget(tag="brightness_value"), "text", None)
        == f"{new_value}%",
        timeout=5.0,
    )
    value_label = device.find_widget(tag="brightness_value")
    assert value_label is not None, "Could not find brightness value label"
    assert value_label.text == f"{new_value}%", \
        f"Expected brightness label '{new_value}%', got '{value_label.text}'"

    # Also verify the slider widget itself reports the new value
    slider = device.find_widget(tag="brightness_slider")
    assert slider is not None
    assert slider.value == new_value, \
        f"Expected slider value {new_value}, got {slider.value}"

    # Verify the actual display brightness via the permanent API
    actual = device.get_brightness()
    assert actual == new_value, \
        f"Expected device brightness {new_value}%, got {actual}%"


def test_restore_brightness(device: DeviceClient):
    """Restore brightness to 80% (default) after test."""
    # Navigate to display screen
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)

    device.click(tag="settings_display")
    assert device.wait_for_screen("display", timeout=5.0)

    # Set back to default
    result = device.set_slider("brightness_slider", 80)
    assert result["success"] is True
    assert result["value"] == 80

    # Verify brightness was actually applied
    actual = device.get_brightness()
    assert actual == 80, f"Expected device brightness 80%, got {actual}%"

    device.wait_until(
        "brightness label shows 80%",
        lambda: getattr(device.find_widget(tag="brightness_value"), "text", None)
        == "80%",
        timeout=5.0,
    )
    value_label = device.find_widget(tag="brightness_value")
    assert value_label is not None
    assert value_label.text == "80%", \
        f"Expected brightness label '80%', got '{value_label.text}'"
