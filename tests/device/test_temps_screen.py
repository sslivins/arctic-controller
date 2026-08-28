"""
Test: Temperatures Screen

Navigates to the temperatures sub-screen and verifies that all 9 temperature
sensor readings are displayed with correct labels and demo-mode values.

Demo mode uses hardcoded temperature values (not set_demo_fields registers).
"""

import pytest
from device_client import DeviceClient

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
    device.click(tag="nav_status")
    assert device.wait_for_screen("status", timeout=5.0), \
        f"Expected 'status' screen, got '{device.screen}'"


def _close_temps(device: DeviceClient):
    """Return to the Home (main) tab via the persistent nav bar."""
    device.click(tag="nav_home")
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


def _wait_for_text(device: DeviceClient, substring: str, timeout: float = 5.0):
    """Wait until some widget's text contains ``substring``."""
    device.wait_until(
        f"text containing '{substring}' visible",
        lambda: _has_text_containing(device, substring),
        timeout=timeout,
    )


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
    """The Status tab has no standalone title in the persistent-nav shell;
    the nav bar labels the tab. Content is verified by the section-header and
    reading tests below."""


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
        _wait_for_text(device, label)

        assert _has_text_containing(device, label), \
            f"Temperature label '{label}' not found on screen"

    @pytest.mark.parametrize("label,expected_value", list(DEMO_TEMPS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_TEMPS])
    def test_temp_value_present(self, device: DeviceClient, label, expected_value):
        """Each demo temperature value should be displayed (e.g. '42 °C')."""
        _open_temps(device)
        value_str = f"{expected_value} °C"
        _wait_for_text(device, value_str)

        assert _has_text_containing(device, value_str), \
            f"Temperature value '{value_str}' not found on screen"

    def test_all_nine_temps_present(self, device: DeviceClient):
        """All 9 temperature rows should be visible."""
        _open_temps(device)
        # Wait until the tree reports all nine demo temperature labels.
        def _all_present() -> bool:
            texts = [(w.text_en or w.text or "").lower() for w in device.widgets]
            return all(any(label.lower() in t for t in texts) for label in DEMO_TEMPS)

        device.wait_until("all nine temperature labels visible", _all_present, timeout=5.0)

        # Fetch widget tree once instead of 9 separate HTTP calls
        widgets = device.widgets
        texts = [
            (w.text_en or w.text or "").lower() for w in widgets
        ]
        missing = []
        for label in DEMO_TEMPS:
            if not any(label.lower() in t for t in texts):
                missing.append(label)
        assert not missing, \
            f"Missing temperature labels: {missing}"
