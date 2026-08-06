"""
Test: System Readings Screen

Navigates to the system readings sub-screen and verifies that all sensor
categories (compressor, electrical, pressures, expansion valves, setpoints)
are displayed with correct labels and demo-mode values.

Demo mode uses hardcoded values (not set_demo_fields registers).
"""

import time
import pytest
from device_client import DeviceClient

UI_SETTLE = 1.5

# Demo mode hardcoded system readings (from heatpump_system_screen.cpp)
DEMO_READINGS = {
    # Compressor section
    "Frequency": "60 Hz",
    "Fan Speed": "850 RPM",
    # Electrical section
    "AC Voltage": "230 V",
    "AC Current": "5 A",
    "DC Voltage": "380.0 V",
    "DC Current": "4 A",
    # Expansion valves section
    "Primary EEV": "350 steps",
    "Secondary EEV": "200 steps",
}

DEMO_SETPOINTS = {
    "Cooling": "20 °C",
    "Heating": "45 °C",
    "Hot Water": "50 °C",
}

# Section headers present on the merged Status screen (i18n English labels).
# The old sub-section headers (Compressor / Electrical / Expansion Valves /
# Setpoints) were removed when the System screen was merged into Status;
# readings now live under a single "System" section header.
SECTION_HEADERS = [
    "System",
]


def _open_system(device: DeviceClient):
    """Navigate from main to the Status screen (System section)."""
    device.click(tag="nav_status")
    assert device.wait_for_screen("status", timeout=5.0), \
        f"Expected 'status' screen, got '{device.screen}'"
    time.sleep(0.5)


def _close_system(device: DeviceClient):
    """Close the Status screen back to main."""
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

class TestSystemNavigation:
    """Open/close system readings screen from the main screen footer."""

    def test_open_system_screen(self, device: DeviceClient):
        """Clicking the System nav button opens the system screen."""
        _open_system(device)

    def test_close_system_screen(self, device: DeviceClient):
        """Clicking close returns to the main screen."""
        _open_system(device)
        _close_system(device)


# =========================================================================
# Title
# =========================================================================

class TestSystemTitle:
    """Verify the title shows 'Status' (the merged temps+system screen)."""

    def test_title_text(self, device: DeviceClient):
        """The title should display the Status i18n string."""
        _open_system(device)
        title = device.find_widget(tag="temps_title")
        assert title is not None, "Status title widget not found"
        text = title.text_en or title.text
        assert "status" in text.lower(), \
            f"Expected 'Status' in title, got: {text!r}"


# =========================================================================
# Section Headers
# =========================================================================

class TestSystemSections:
    """Verify section headers are present."""

    @pytest.mark.parametrize("header", SECTION_HEADERS)
    def test_section_header_present(self, device: DeviceClient, header):
        """Each section header should be visible on the system screen."""
        _open_system(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, header), \
            f"Section header '{header}' not found on screen"


# =========================================================================
# Readings
# =========================================================================

class TestSystemReadings:
    """Verify all reading labels and demo values are present."""

    @pytest.mark.parametrize("label,expected_value",
                             list(DEMO_READINGS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_READINGS])
    def test_reading_label_present(self, device: DeviceClient, label, expected_value):
        """Each reading label should be visible."""
        _open_system(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, label), \
            f"Reading label '{label}' not found on screen"

    @pytest.mark.parametrize("label,expected_value",
                             list(DEMO_READINGS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_READINGS])
    def test_reading_value_present(self, device: DeviceClient, label, expected_value):
        """Each demo reading value should be displayed."""
        _open_system(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, expected_value), \
            f"Reading value '{expected_value}' for '{label}' not found on screen"


# =========================================================================
# Setpoints
# =========================================================================

class TestSystemSetpoints:
    """Verify setpoint values are displayed in the system screen."""

    @pytest.mark.parametrize("label,expected_value",
                             list(DEMO_SETPOINTS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_SETPOINTS])
    def test_setpoint_present(self, device: DeviceClient, label, expected_value):
        """Each setpoint label should be visible."""
        _open_system(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, label), \
            f"Setpoint label '{label}' not found on screen"
