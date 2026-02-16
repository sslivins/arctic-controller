"""
Test: Temperature Unit Toggle

Verifies that toggling the temperature unit switch in the settings
menu changes between Celsius and Fahrenheit, confirmed by the
preferences API.
"""

import time
from device_client import DeviceClient


def test_toggle_temp_unit(device: DeviceClient):
    """Toggling the temperature unit switch should change the preference."""
    # Read initial state
    prefs = device.get_preferences()
    initial_unit = prefs["temp_unit"]  # "celsius" or "fahrenheit"

    # Open settings
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)

    # Toggle the switch
    result = device.toggle("temp_unit_switch")
    assert result["success"] is True
    time.sleep(0.3)

    # Verify via preferences API
    prefs = device.get_preferences()
    expected = "fahrenheit" if initial_unit == "celsius" else "celsius"
    assert prefs["temp_unit"] == expected, \
        f"Expected temp_unit='{expected}', got '{prefs['temp_unit']}'"

    # Verify switch UI matches
    sw = device.find_widget(tag="temp_unit_switch")
    assert sw is not None, "Could not find temp_unit_switch"
    assert sw.checked == (expected == "fahrenheit"), \
        f"Switch checked state doesn't match expected unit '{expected}'"


def test_restore_temp_unit(device: DeviceClient):
    """Restore temperature unit to Celsius."""
    prefs = device.get_preferences()

    if prefs["temp_unit"] == "fahrenheit":
        # Open settings and toggle back
        device.click(tag="settings")
        assert device.wait_for_screen("settings", timeout=5.0)
        time.sleep(0.5)

        result = device.toggle("temp_unit_switch")
        assert result["success"] is True
        time.sleep(0.3)

    prefs = device.get_preferences()
    assert prefs["temp_unit"] == "celsius", \
        f"Expected temp_unit='celsius', got '{prefs['temp_unit']}'"
