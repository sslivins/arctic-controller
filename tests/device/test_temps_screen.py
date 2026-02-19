"""
Test: Temperatures Screen

Navigates to the temperatures sub-screen and verifies that all 9 temperature
sensor readings are displayed with correct labels and demo-mode values.

Demo mode uses hardcoded temperature values (not set_demo_fields registers).
"""

import time
import pytest
from device_client import DeviceClient

UI_SETTLE = 1.5

# Demo mode hardcoded temps (from heatpump_temps_screen.cpp update_readings)
DEMO_TEMPS = {
    "Water Tank": 42,
    "Water Outlet": 45,
    "Water Inlet": 38,
    "Outdoor Ambient": 22,
    "Discharge": 85,
    "Suction": 12,
    "Outdoor Coil": 35,
    "Indoor Coil": 40,
    "IPM Module": 55,
}


def _open_temps(device: DeviceClient):
    """Navigate from main to the temperatures screen."""
    device.click(tag="nav_temps")
    assert device.wait_for_screen("temps", timeout=5.0), \
        f"Expected 'temps' screen, got '{device.screen}'"
    time.sleep(0.5)


def _close_temps(device: DeviceClient):
    """Close the temperatures screen back to main."""
    device.click(tag="temps_close")
    assert device.wait_for_screen("main", timeout=5.0), \
        f"Expected 'main' screen after close, got '{device.screen}'"


def _has_text_containing(device: DeviceClient, substring: str) -> bool:
    """Check if any widget's text contains the given substring."""
    sub_lower = substring.lower()
    for w in device.widgets:
        t = w.text_en or w.text
        if t and sub_lower in t.lower():
            return True
    return False


# =========================================================================
# Navigation
# =========================================================================

class TestTempsNavigation:
    """Open/close temperatures screen from the main screen footer."""

    def test_open_temps_screen(self, device: DeviceClient):
        """Clicking the Temps nav button opens the temperatures screen."""
        _open_temps(device)

    def test_close_temps_screen(self, device: DeviceClient):
        """Clicking close returns to the main screen."""
        _open_temps(device)
        _close_temps(device)


# =========================================================================
# Title
# =========================================================================

class TestTempsTitle:
    """Verify the title shows 'Temperatures'."""

    def test_title_text(self, device: DeviceClient):
        """The title should display the Temperatures i18n string."""
        _open_temps(device)
        title = device.find_widget(tag="temps_title")
        assert title is not None, "Temps title widget not found"
        text = title.text_en or title.text
        assert "temperatures" in text.lower(), \
            f"Expected 'Temperatures' in title, got: {text!r}"


# =========================================================================
# Temperature Readings
# =========================================================================

class TestTempsReadings:
    """Verify all 9 temperature labels and values are present in demo mode."""

    @pytest.mark.parametrize("label,expected_value", list(DEMO_TEMPS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_TEMPS])
    def test_temp_label_present(self, device: DeviceClient, label, expected_value):
        """Each temperature sensor label should be visible on screen."""
        _open_temps(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, label), \
            f"Temperature label '{label}' not found on screen"

    @pytest.mark.parametrize("label,expected_value", list(DEMO_TEMPS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_TEMPS])
    def test_temp_value_present(self, device: DeviceClient, label, expected_value):
        """Each demo temperature value should be displayed (e.g. '42 °C')."""
        _open_temps(device)
        time.sleep(UI_SETTLE)

        value_str = f"{expected_value} °C"
        assert _has_text_containing(device, value_str), \
            f"Temperature value '{value_str}' not found on screen"

    def test_all_nine_temps_present(self, device: DeviceClient):
        """All 9 temperature rows should be visible."""
        _open_temps(device)
        time.sleep(UI_SETTLE)

        missing = []
        for label in DEMO_TEMPS:
            if not _has_text_containing(device, label):
                missing.append(label)
        assert not missing, \
            f"Missing temperature labels: {missing}"
