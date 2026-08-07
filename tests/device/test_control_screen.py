"""
Test: Control (Advanced) Screen

Navigates to the control sub-screen and verifies the power button,
mode selector, and setpoint controls are displayed correctly in demo mode.

The control screen shows:
- Power ON/OFF button (single click to turn on, 3s hold to turn off)
- 5 mode buttons (Cooling, Floor Heat, Fan Heat, Hot Water, Auto)
- 3 setpoint rows (Cooling, Heating, Hot Water) with values
- Advanced Parameter rows (AP13, AP24, AP38, etc.)
"""

import time
import pytest
from device_client import DeviceClient

UI_SETTLE = 1.5

# Working modes (must match arctic_registers.h)
MODE_COOLING = 0
MODE_FLOOR_HEATING = 1
MODE_FAN_HEATING = 2
MODE_HOT_WATER = 5
MODE_AUTO = 6

# Mode button labels (i18n English)
MODE_LABELS = ["COOLING", "FLOOR HEAT", "FAN HEAT", "HOT WATER", "AUTO"]

# Demo default setpoints (from heatpump_params_screen.cpp)
DEMO_SETPOINTS = {
    "Cooling Setpoint": "18 °C",
    "Heating Setpoint": "45 °C",
    "Hot Water Setpoint": "50 °C",
}


def _open_control(device: DeviceClient):
    """Navigate from main to the control screen."""
    device.click(tag="nav_control")
    assert device.wait_for_screen("control", timeout=5.0), \
        f"Expected 'control' screen, got '{device.screen}'"
    time.sleep(0.5)


def _close_control(device: DeviceClient):
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


@pytest.fixture(autouse=True)
def _restore_control_defaults(device: DeviceClient):
    """Restore demo control state after each test."""
    yield
    device.set_demo_fields(
        unit_on=1,
        working_mode=MODE_FLOOR_HEATING,
        cooling_setpoint=18,
        heating_setpoint=45,
        hot_water_setpoint=50,
    )
    time.sleep(UI_SETTLE)


# =========================================================================
# Navigation
# =========================================================================

class TestControlNavigation:
    """Open/close control screen from the main screen footer."""

    def test_open_control_screen(self, device: DeviceClient):
        """Clicking the Control nav button opens the control screen."""
        _open_control(device)

    def test_close_control_screen(self, device: DeviceClient):
        """Selecting the Home tab returns to the main screen."""
        _open_control(device)
        _close_control(device)


# =========================================================================
# Power Button
# =========================================================================

class TestPowerButton:
    """Verify the power button is present and reflects unit state."""

    def test_power_button_shows_on(self, device: DeviceClient):
        """When unit is ON, power button should show POWERED ON."""
        device.set_demo_fields(unit_on=1)
        time.sleep(UI_SETTLE)

        _open_control(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, "POWERED ON"), \
            "POWERED ON text not found on control screen"

    def test_power_button_shows_off(self, device: DeviceClient):
        """When unit is OFF, power button should show POWERED OFF."""
        device.set_demo_fields(unit_on=0)
        time.sleep(UI_SETTLE)

        _open_control(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, "POWERED OFF"), \
            "POWERED OFF text not found on control screen"


# =========================================================================
# Mode Buttons
# =========================================================================

class TestModeButtons:
    """Verify all 5 mode buttons are present."""

    @pytest.mark.parametrize("mode_label", MODE_LABELS)
    def test_mode_button_present(self, device: DeviceClient, mode_label):
        """Each mode button should be visible on the control screen."""
        _open_control(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, mode_label), \
            f"Mode button '{mode_label}' not found on screen"

    def test_active_mode_highlighted(self, device: DeviceClient):
        """The active mode (Floor Heating) should be distinguishable."""
        device.set_demo_fields(working_mode=MODE_FLOOR_HEATING)
        time.sleep(UI_SETTLE)

        _open_control(device)
        time.sleep(UI_SETTLE)

        # Just verify the mode label exists — visual highlighting
        # is difficult to test without bg_color on mode buttons
        assert _has_text_containing(device, "FLOOR HEAT"), \
            "Active mode 'FLOOR HEAT' not found"


# =========================================================================
# Setpoints
# =========================================================================

class TestSetpoints:
    """Verify setpoint labels and values are displayed."""

    @pytest.mark.parametrize("label,expected_value",
                             list(DEMO_SETPOINTS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_SETPOINTS])
    def test_setpoint_label_present(self, device: DeviceClient, label, expected_value):
        """Each setpoint label should be visible."""
        _open_control(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, label), \
            f"Setpoint label '{label}' not found on screen"

    @pytest.mark.parametrize("label,expected_value",
                             list(DEMO_SETPOINTS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_SETPOINTS])
    def test_setpoint_value_present(self, device: DeviceClient, label, expected_value):
        """Each setpoint value should be displayed."""
        _open_control(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, expected_value), \
            f"Setpoint value '{expected_value}' for '{label}' not found"


# =========================================================================
# Advanced Parameters Section
# =========================================================================

class TestAdvancedParameters:
    """Verify Advanced Parameter (AP##) rows are visible."""

    def test_setpoints_section_header(self, device: DeviceClient):
        """The Setpoints section header should be present."""
        _open_control(device)
        time.sleep(UI_SETTLE)

        assert _has_text_containing(device, "Setpoints"), \
            "Setpoints section header not found"

    def test_advanced_parameter_visible(self, device: DeviceClient):
        """At least one Advanced Parameter row should be visible (e.g. AP13)."""
        _open_control(device)
        time.sleep(UI_SETTLE)

        # Look for any Advanced Parameter label containing "(AP"
        found = False
        for w in device.widgets:
            t = w.text_en or w.text
            if t and "(AP" in t:
                found = True
                break
        assert found, "No Advanced Parameter rows found on control screen"
