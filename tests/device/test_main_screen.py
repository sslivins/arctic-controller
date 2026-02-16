"""
Test: Main Screen — Hero Card, Component Dots, Performance Strip, Error Card

Uses the demo mode test API (POST /api/test/set-demo-field) to inject specific
heat pump states and verifies that the main screen UI updates accordingly.

The device must be running in demo mode for these tests to work.
"""

import time
import pytest
from device_client import DeviceClient

# ---------------------------------------------------------------------------
# Status register bit masks (must match arctic_registers.h)
# ---------------------------------------------------------------------------
UNIT_ON         = 0x0001
COMPRESSOR      = 0x0002
FAN_HIGH        = 0x0004
FAN_MED         = 0x0008
FAN_LOW         = 0x0010
WATER_PUMP      = 0x0020
BACKUP_HEATER   = 0x0080

# Working modes
MODE_COOLING       = 0
MODE_FLOOR_HEATING = 1
MODE_HOT_WATER     = 5

# Inactive dot color
COLOR_INACTIVE = "#444444"

# Default demo status1 value (for restoration)
DEMO_STATUS1 = UNIT_ON | COMPRESSOR | FAN_MED | WATER_PUMP  # 0x2B
DEMO_ERROR2_HIGH_PRESSURE = 0x0040

# Update interval + margin for UI to refresh after demo field change
UI_SETTLE = 1.5


def _wait_for_update():
    """Wait for the main screen 1-second timer to pick up demo state changes."""
    time.sleep(UI_SETTLE)


# =========================================================================
# Fixtures
# =========================================================================

@pytest.fixture(autouse=True)
def _ensure_demo_defaults(device: DeviceClient):
    """Restore demo state defaults after each test in this module."""
    yield
    # Restore: unit on, compressor+fan+pump running, floor heating, error2 active
    device.set_demo_fields(
        status1=DEMO_STATUS1,
        status2=0,
        error1=0,
        error2=DEMO_ERROR2_HIGH_PRESSURE,
        working_mode=MODE_FLOOR_HEATING,
        unit_on=1,
        water_tank_temp=42,
        fan_speed=850,
        compressor_freq=60,
        ac_voltage=230,
        ac_current=52,
        inlet_water_temp=38,
        outlet_water_temp=45,
    )
    _wait_for_update()


# =========================================================================
# Hero Card — State Text
# =========================================================================

class TestHeroState:
    """Verify the hero card shows the correct operating mode."""

    def test_hero_shows_fault_with_error(self, device: DeviceClient):
        """With an active error, hero should show FAULT."""
        # Default demo state has error2=HIGH_PRESSURE, so hero = FAULT
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None, "hero_state widget not found"
        label = hero.text_en or hero.text
        assert label == "FAULT", f"Expected 'FAULT', got '{label}'"

    def test_hero_shows_floor_heat(self, device: DeviceClient):
        """Clearing errors with floor heating mode should show FLOOR HEAT."""
        device.set_demo_fields(error1=0, error2=0)
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "FLOOR HEAT", f"Expected 'FLOOR HEAT', got '{label}'"

    def test_hero_shows_cooling(self, device: DeviceClient):
        """Switching to cooling mode with compressor on should show COOLING."""
        device.set_demo_fields(error1=0, error2=0, working_mode=MODE_COOLING)
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "COOLING", f"Expected 'COOLING', got '{label}'"

    def test_hero_shows_hot_water(self, device: DeviceClient):
        """Hot water mode with compressor on should show HOT WATER."""
        device.set_demo_fields(error1=0, error2=0, working_mode=MODE_HOT_WATER)
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "HOT WATER", f"Expected 'HOT WATER', got '{label}'"

    def test_hero_shows_idle(self, device: DeviceClient):
        """Unit on, no errors, compressor off should show IDLE."""
        device.set_demo_fields(
            error1=0, error2=0,
            status1=UNIT_ON | WATER_PUMP,  # no compressor
        )
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "IDLE", f"Expected 'IDLE', got '{label}'"

    def test_hero_shows_standby(self, device: DeviceClient):
        """Unit off should show STANDBY."""
        device.set_demo_fields(error1=0, error2=0, unit_on=0, status1=0)
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "STANDBY", f"Expected 'STANDBY', got '{label}'"


# =========================================================================
# Hero Card — Tank Temperature
# =========================================================================

class TestHeroTankTemp:
    """Verify the hero card displays the tank temperature correctly."""

    def test_tank_temp_displayed(self, device: DeviceClient):
        """Tank temp should be shown as a number with unit."""
        device.set_demo_fields(error1=0, error2=0, water_tank_temp=50)
        _wait_for_update()
        tank = device.find_widget(tag="hero_tank_temp")
        assert tank is not None, "hero_tank_temp widget not found"
        # Should contain "50" and the unit symbol
        assert "50" in tank.text, f"Expected '50' in tank text, got '{tank.text}'"


# =========================================================================
# Component Dots — Status Indicators
# =========================================================================

class TestComponentDots:
    """Verify the component indicator dots reflect the status register bits."""

    def test_compressor_dot_on(self, device: DeviceClient):
        """Compressor running should light the compressor dot."""
        device.set_demo_fields(error1=0, error2=0)
        _wait_for_update()
        dot = device.find_widget(tag="comp_dot")
        assert dot is not None, "comp_dot not found"
        assert dot.bg_color != COLOR_INACTIVE, \
            f"Compressor dot should be active, got bg_color={dot.bg_color}"

    def test_compressor_dot_off(self, device: DeviceClient):
        """Compressor stopped should grey out the compressor dot."""
        device.set_demo_fields(
            error1=0, error2=0,
            status1=UNIT_ON | WATER_PUMP,  # no COMPRESSOR bit
        )
        _wait_for_update()
        dot = device.find_widget(tag="comp_dot")
        assert dot is not None
        assert dot.bg_color == COLOR_INACTIVE, \
            f"Compressor dot should be inactive, got bg_color={dot.bg_color}"

    def test_fan_dot_on(self, device: DeviceClient):
        """Fan running should light the fan dot."""
        device.set_demo_fields(error1=0, error2=0)
        _wait_for_update()
        dot = device.find_widget(tag="fan_dot")
        assert dot is not None, "fan_dot not found"
        assert dot.bg_color != COLOR_INACTIVE, \
            f"Fan dot should be active, got bg_color={dot.bg_color}"

    def test_fan_dot_off(self, device: DeviceClient):
        """Fan stopped should grey out the fan dot."""
        device.set_demo_fields(
            error1=0, error2=0,
            status1=UNIT_ON | COMPRESSOR | WATER_PUMP,  # no FAN bits
        )
        _wait_for_update()
        dot = device.find_widget(tag="fan_dot")
        assert dot is not None
        assert dot.bg_color == COLOR_INACTIVE, \
            f"Fan dot should be inactive, got bg_color={dot.bg_color}"

    def test_pump_dot_on(self, device: DeviceClient):
        """Water pump running should light the pump dot."""
        device.set_demo_fields(error1=0, error2=0)
        _wait_for_update()
        dot = device.find_widget(tag="pump_dot")
        assert dot is not None, "pump_dot not found"
        assert dot.bg_color != COLOR_INACTIVE, \
            f"Pump dot should be active, got bg_color={dot.bg_color}"

    def test_pump_dot_off(self, device: DeviceClient):
        """Water pump stopped should grey out the pump dot."""
        device.set_demo_fields(
            error1=0, error2=0,
            status1=UNIT_ON | COMPRESSOR | FAN_MED,  # no WATER_PUMP
        )
        _wait_for_update()
        dot = device.find_widget(tag="pump_dot")
        assert dot is not None
        assert dot.bg_color == COLOR_INACTIVE, \
            f"Pump dot should be inactive, got bg_color={dot.bg_color}"

    def test_heater_dot_off_by_default(self, device: DeviceClient):
        """Aux heater should be off in default demo state."""
        device.set_demo_fields(error1=0, error2=0)
        _wait_for_update()
        dot = device.find_widget(tag="heater_dot")
        assert dot is not None, "heater_dot not found"
        assert dot.bg_color == COLOR_INACTIVE, \
            f"Heater dot should be inactive by default, got bg_color={dot.bg_color}"

    def test_heater_dot_on(self, device: DeviceClient):
        """Turning on backup heater should light the heater dot."""
        device.set_demo_fields(
            error1=0, error2=0,
            status1=DEMO_STATUS1 | BACKUP_HEATER,
        )
        _wait_for_update()
        dot = device.find_widget(tag="heater_dot")
        assert dot is not None
        assert dot.bg_color != COLOR_INACTIVE, \
            f"Heater dot should be active, got bg_color={dot.bg_color}"


# =========================================================================
# Performance Strip — Fan RPM
# =========================================================================

class TestPerformanceStrip:
    """Verify performance strip values update from demo state."""

    def test_fan_rpm_displayed(self, device: DeviceClient):
        """Fan speed should show RPM value when fan is running."""
        device.set_demo_fields(error1=0, error2=0, fan_speed=850)
        _wait_for_update()
        fan = device.find_widget(tag="perf_fan")
        assert fan is not None, "perf_fan widget not found"
        assert "850" in fan.text, f"Expected '850' in fan text, got '{fan.text}'"
        assert "RPM" in fan.text, f"Expected 'RPM' in fan text, got '{fan.text}'"

    def test_fan_rpm_dashes_when_stopped(self, device: DeviceClient):
        """Fan speed should show '--' when fan is not running."""
        device.set_demo_fields(error1=0, error2=0, fan_speed=0)
        _wait_for_update()
        fan = device.find_widget(tag="perf_fan")
        assert fan is not None
        assert fan.text == "--", f"Expected '--' for stopped fan, got '{fan.text}'"

    def test_power_displayed(self, device: DeviceClient):
        """Power consumption should be displayed when compressor is running."""
        # Default: 230V * 52 (tenths of A) / 10 = 1196W → "1.1 kW"
        device.set_demo_fields(error1=0, error2=0, ac_voltage=230, ac_current=52)
        _wait_for_update()
        power = device.find_widget(tag="perf_power")
        assert power is not None, "perf_power widget not found"
        assert power.text != "--", f"Expected power value, got '{power.text}'"
        assert "kW" in power.text or "W" in power.text, \
            f"Expected unit in power text, got '{power.text}'"


# =========================================================================
# Error Card
# =========================================================================

class TestErrorCard:
    """Verify the error card reflects current error state."""

    def test_no_errors_shows_system_ok(self, device: DeviceClient):
        """With no errors, error card should show 'No active errors'."""
        device.set_demo_fields(error1=0, error2=0)
        _wait_for_update()
        err = device.find_widget(tag="error_label")
        assert err is not None, "error_label widget not found"
        label = (err.text_en or err.text).lower()
        assert "error" in label or "ok" in label, \
            f"Expected system ok message, got '{err.text_en or err.text}'"

    def test_error_shows_description(self, device: DeviceClient):
        """With an active error, error card should display the error code."""
        device.set_demo_fields(error1=0, error2=DEMO_ERROR2_HIGH_PRESSURE)
        _wait_for_update()
        err = device.find_widget(tag="error_label")
        assert err is not None
        # P02 is the high pressure error code
        assert "P02" in err.text, f"Expected 'P02' in error text, got '{err.text}'"
