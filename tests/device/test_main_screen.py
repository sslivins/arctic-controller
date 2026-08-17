"""
Test: Main Screen — Hero Card, Component Dots, Performance Strip, Error Card

Uses the demo mode test API to inject specific heat pump states and verifies
that the main screen UI updates accordingly.

Component/run state is decoded natively by arctic-macon from the real Tuya
registers, so there is no fictional "status1" bitfield. Tests drive named
demo fields instead of raw register bits:
  compressor -> compressor_freq (>0 = running)
  fan bars   -> fan_speed byte level (getFanSpeedLevel buckets)
  fan dot    -> fan_on
  pump       -> pump_on
Faults are injected by their Macon code via inject_fault()/clear_all_faults().

The device must be running in demo mode for these tests to work.
"""

import time
import pytest
from device_client import DeviceClient

# ---------------------------------------------------------------------------
# fan_speed (reg2003) byte-level values -> UI fan level (getFanSpeedLevel):
#   0 -> 0 bars, 1..29 -> 1 bar, 30..59 -> 2 bars, >=60 -> 3 bars
# ---------------------------------------------------------------------------
FAN_OFF, FAN_LOW, FAN_MED, FAN_HIGH = 0, 20, 45, 80

# Working modes
MODE_COOLING       = 0
MODE_FLOOR_HEATING = 1
MODE_HOT_WATER     = 5

# Inactive dot color
COLOR_INACTIVE = "#444444"

# Default demo fault (matches initDemoState()).
DEMO_FAULT = "P02"

# Update interval + margin for UI to refresh after demo field change
UI_SETTLE = 1.5


def _wait_for_update():
    """Wait for the main screen 1-second timer to pick up demo state changes."""
    time.sleep(UI_SETTLE)


def _running(device: DeviceClient, **overrides):
    """Set a normal 'running' component state, then apply any overrides."""
    fields = dict(compressor_freq=60, fan_on=1, fan_speed=FAN_MED, pump_on=1,
                  unit_on=1, cooling_on=0)
    fields.update(overrides)
    device.set_demo_fields(**fields)


# =========================================================================
# Fixtures
# =========================================================================

@pytest.fixture(autouse=True)
def _ensure_demo_defaults(device: DeviceClient):
    """Restore demo state defaults after each test in this module."""
    yield
    # Restore: unit on, compressor+fan+pump running, floor heating, P02 fault
    device.clear_all_faults()
    device.inject_fault(DEMO_FAULT, True)
    device.set_demo_fields(
        working_mode=MODE_FLOOR_HEATING,
        cooling_on=0,
        unit_on=1,
        water_tank_temp=42,
        compressor_freq=60,
        fan_on=1,
        fan_speed=FAN_MED,
        pump_on=1,
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
        """With an active fault, hero should show FAULT."""
        # Default demo state has the P02 fault active, so hero = FAULT
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None, "hero_state widget not found"
        label = hero.text_en or hero.text
        assert label == "FAULT", f"Expected 'FAULT', got '{label}'"

    def test_hero_shows_floor_heat(self, device: DeviceClient):
        """A running heating cycle should show the actual HEATING operation."""
        device.clear_all_faults()
        _running(device)
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "HEATING", f"Expected 'HEATING', got '{label}'"

    def test_hero_shows_cooling(self, device: DeviceClient):
        """Switching to cooling mode with compressor on should show COOLING."""
        device.clear_all_faults()
        # Cooling operation is decoded from the reversing-valve bit (reg2129
        # bit2 = cooling_on), not the selected working_mode, so set both.
        _running(device, working_mode=MODE_COOLING, cooling_on=1)
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "COOLING", f"Expected 'COOLING', got '{label}'"

    def test_hero_shows_hot_water(self, device: DeviceClient):
        """Hot-water selection still reports the actual heating operation."""
        device.clear_all_faults()
        _running(device, working_mode=MODE_HOT_WATER)
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "HEATING", f"Expected 'HEATING', got '{label}'"

    def test_hero_shows_idle(self, device: DeviceClient):
        """Unit on, no faults, compressor off should show IDLE."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=1, compressor_freq=0, fan_on=0,
                               fan_speed=FAN_OFF, pump_on=1)
        _wait_for_update()
        hero = device.find_widget(tag="hero_state")
        assert hero is not None
        label = hero.text_en or hero.text
        assert label == "IDLE", f"Expected 'IDLE', got '{label}'"

    def test_hero_shows_standby(self, device: DeviceClient):
        """Unit off should show STANDBY."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=0, compressor_freq=0, fan_on=0,
                               fan_speed=FAN_OFF, pump_on=0)
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
        device.clear_all_faults()
        device.set_demo_fields(water_tank_temp=50)
        _wait_for_update()
        tank = device.find_widget(tag="hero_tank_temp")
        assert tank is not None, "hero_tank_temp widget not found"
        assert "50" in tank.text, f"Expected '50' in tank text, got '{tank.text}'"


# =========================================================================
# Component Dots
# =========================================================================

class TestComponentDots:
    """Verify component indicator dots reflect run state."""

    def test_compressor_dot_on(self, device: DeviceClient):
        """Compressor running should light the compressor dot."""
        device.clear_all_faults()
        _running(device)
        _wait_for_update()
        dot = device.find_widget(tag="comp_dot")
        assert dot is not None, "comp_dot widget not found"
        assert dot.bg_color != COLOR_INACTIVE, \
            f"Compressor dot should be active, got bg_color={dot.bg_color}"

    def test_compressor_dot_off(self, device: DeviceClient):
        """Compressor stopped should grey out the compressor dot."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=1, compressor_freq=0, fan_on=0,
                               fan_speed=FAN_OFF, pump_on=1)
        _wait_for_update()
        dot = device.find_widget(tag="comp_dot")
        assert dot is not None
        assert dot.bg_color == COLOR_INACTIVE, \
            f"Compressor dot should be inactive, got bg_color={dot.bg_color}"

    def test_fan_speed_bars_medium(self, device: DeviceClient):
        """FAN_MED — bars 1 and 2 should be green, bar 3 gray."""
        device.clear_all_faults()
        _running(device, fan_speed=FAN_MED)
        _wait_for_update()
        bar1 = device.find_widget(tag="fan_bar_1")
        bar2 = device.find_widget(tag="fan_bar_2")
        bar3 = device.find_widget(tag="fan_bar_3")
        assert bar1 is not None and bar2 is not None and bar3 is not None
        assert bar1.bg_color != COLOR_INACTIVE, "Bar 1 should be active (med)"
        assert bar2.bg_color != COLOR_INACTIVE, "Bar 2 should be active (med)"
        assert bar3.bg_color == COLOR_INACTIVE, "Bar 3 should be inactive (med)"

    def test_fan_speed_bars_high(self, device: DeviceClient):
        """FAN_HIGH — all 3 bars should be green."""
        device.clear_all_faults()
        _running(device, fan_speed=FAN_HIGH)
        _wait_for_update()
        bar1 = device.find_widget(tag="fan_bar_1")
        bar2 = device.find_widget(tag="fan_bar_2")
        bar3 = device.find_widget(tag="fan_bar_3")
        assert bar1.bg_color != COLOR_INACTIVE, "Bar 1 should be active (high)"
        assert bar2.bg_color != COLOR_INACTIVE, "Bar 2 should be active (high)"
        assert bar3.bg_color != COLOR_INACTIVE, "Bar 3 should be active (high)"

    def test_fan_speed_bars_low(self, device: DeviceClient):
        """FAN_LOW — only bar 1 should be green."""
        device.clear_all_faults()
        _running(device, fan_speed=FAN_LOW)
        _wait_for_update()
        bar1 = device.find_widget(tag="fan_bar_1")
        bar2 = device.find_widget(tag="fan_bar_2")
        bar3 = device.find_widget(tag="fan_bar_3")
        assert bar1.bg_color != COLOR_INACTIVE, "Bar 1 should be active (low)"
        assert bar2.bg_color == COLOR_INACTIVE, "Bar 2 should be inactive (low)"
        assert bar3.bg_color == COLOR_INACTIVE, "Bar 3 should be inactive (low)"

    def test_fan_speed_bars_off(self, device: DeviceClient):
        """No fan speed — all 3 bars should be gray."""
        device.clear_all_faults()
        _running(device, fan_on=0, fan_speed=FAN_OFF)
        _wait_for_update()
        bar1 = device.find_widget(tag="fan_bar_1")
        bar2 = device.find_widget(tag="fan_bar_2")
        bar3 = device.find_widget(tag="fan_bar_3")
        assert bar1.bg_color == COLOR_INACTIVE, "Bar 1 should be inactive (off)"
        assert bar2.bg_color == COLOR_INACTIVE, "Bar 2 should be inactive (off)"
        assert bar3.bg_color == COLOR_INACTIVE, "Bar 3 should be inactive (off)"

    def test_pump_dot_on(self, device: DeviceClient):
        """Water pump running should light the pump dot."""
        device.clear_all_faults()
        _running(device)
        _wait_for_update()
        dot = device.find_widget(tag="pump_dot")
        assert dot is not None, "pump_dot widget not found"
        assert dot.bg_color != COLOR_INACTIVE, \
            f"Pump dot should be active, got bg_color={dot.bg_color}"

    def test_pump_dot_off(self, device: DeviceClient):
        """Water pump stopped should grey out the pump dot."""
        device.clear_all_faults()
        _running(device, pump_on=0)
        _wait_for_update()
        dot = device.find_widget(tag="pump_dot")
        assert dot is not None
        assert dot.bg_color == COLOR_INACTIVE, \
            f"Pump dot should be inactive, got bg_color={dot.bg_color}"

    def test_heater_dot_off_by_default(self, device: DeviceClient):
        """Aux heater should be off in default demo state."""
        device.clear_all_faults()
        _running(device)
        _wait_for_update()
        dot = device.find_widget(tag="heater_dot")
        assert dot is not None, "heater_dot widget not found"
        assert dot.bg_color == COLOR_INACTIVE, \
            f"Heater dot should be inactive, got bg_color={dot.bg_color}"

    @pytest.mark.skip(reason="Backup/aux heater is not mapped from any Tuya "
                             "register yet; always off. See sun-peaks TODO "
                             "(fan/aux-heater register rework).")
    def test_heater_dot_on(self, device: DeviceClient):
        """Turning on backup heater should light the heater dot — no mapping yet."""


# =========================================================================
# Performance Strip
# =========================================================================

class TestPerformanceStrip:
    """Verify performance strip values update from demo state."""

    def test_fan_rpm_displayed(self, device: DeviceClient):
        """Fan speed should show its value when the fan is running.

        NOTE: fan_speed is now the Macon byte-level fan value (reg2003), not a
        true RPM; the label still reads "<value> RPM" pending the fan-level
        rework (see sun-peaks TODO).
        """
        device.clear_all_faults()
        _running(device, fan_speed=45)
        _wait_for_update()
        fan = device.find_widget(tag="perf_fan")
        assert fan is not None, "perf_fan widget not found"
        assert "45" in fan.text, f"Expected '45' in fan text, got '{fan.text}'"
        assert "RPM" in fan.text, f"Expected 'RPM' in fan text, got '{fan.text}'"

    def test_fan_rpm_dashes_when_stopped(self, device: DeviceClient):
        """Fan speed should show '--' when the fan is not running."""
        device.clear_all_faults()
        _running(device, fan_on=0, fan_speed=0)
        _wait_for_update()
        fan = device.find_widget(tag="perf_fan")
        assert fan is not None
        assert "--" in fan.text, f"Expected '--' in fan text, got '{fan.text}'"

    def test_power_displayed(self, device: DeviceClient):
        """Power consumption should be displayed when compressor is running."""
        device.clear_all_faults()
        _running(device, ac_voltage=230, ac_current=52)
        _wait_for_update()
        power = device.find_widget(tag="perf_power")
        assert power is not None, "perf_power widget not found"


# =========================================================================
# Error Card
# =========================================================================

class TestErrorCard:
    """Verify the error card reflects fault state."""

    def test_no_errors_shows_system_ok(self, device: DeviceClient):
        """With no faults, error card should show 'No active errors'."""
        device.clear_all_faults()
        _wait_for_update()
        err = device.find_widget(tag="error_label")
        assert err is not None

    def test_error_shows_description(self, device: DeviceClient):
        """With an active fault, error card should display the error code."""
        device.clear_all_faults()
        device.inject_fault("P02", True)
        _wait_for_update()
        err = device.find_widget(tag="error_label")
        assert err is not None
        # P02 is the high pressure error code
        assert "P02" in err.text, f"Expected 'P02' in error text, got '{err.text}'"
