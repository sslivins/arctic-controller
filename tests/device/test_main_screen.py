"""
Test: Main Screen — Hero Card, Component Pills, Tiles, Fault Banner

Uses the demo mode test API to inject specific heat pump states and verifies
that the main screen UI updates accordingly.

Component/run state is decoded natively by arctic-macon from the real Tuya
registers, so there is no fictional "status1" bitfield. Tests drive named
demo fields instead of raw register bits:
  compressor -> compressor_freq (>0 = running)
  fan bars   -> fan_speed RPM (reg2003 raw ×10; getFanSpeedLevel buckets)
  fan pill   -> fan_on
  pump       -> pump_on
Faults are injected by their Macon code via inject_fault()/clear_all_faults().

The device must be running in demo mode for these tests to work.
"""

import pytest
from device_client import DeviceClient

# ---------------------------------------------------------------------------
# fan_speed is the fan RPM (reg2003 raw ×10). getFanSpeedLevel buckets it as a
# percentage of the library-provided max (fan_speed_max = 1000 RPM):
#   0 -> 0 bars, <33% -> 1 bar, 33..65% -> 2 bars, >=66% -> 3 bars
# ---------------------------------------------------------------------------
FAN_OFF, FAN_LOW, FAN_MED, FAN_HIGH = 0, 200, 450, 700

# Working modes
MODE_COOLING       = 0
MODE_HEATING = 1
MODE_HOT_WATER     = 5

# Unlit component pill / fan bar color (COLOR_TRACK). A lit pill or bar takes
# the hero accent color instead.
COLOR_INACTIVE = "#2b3a5c"

# The hero state line may carry a detail suffix after this separator,
# e.g. "Fault · P02" or "Idle · at setpoint".
STATE_SEP = " · "

# Default demo fault (matches initDemoState()).
DEMO_FAULT = "P02"

# Update interval + margin for UI to refresh after demo field change
UI_SETTLE = 1.5


def _wait(device: DeviceClient, predicate, description: str):
    """Wait for the main screen 1-second timer to render demo-state changes.

    Polls the actual observable (a live widget fetch) instead of sleeping a
    fixed interval, so the test is robust to timer/render jitter. Falls back
    after the timeout (raise_on_timeout=False) to let the following
    authoritative assert report the real rendered value.
    """
    device.wait_until(description, predicate, timeout=5.0,
                      expect_within=UI_SETTLE, raise_on_timeout=False)


def _hero_word(w) -> str:
    """The state word of the hero state line, without any detail suffix."""
    return (w.text_en or w.text or "").split(STATE_SEP)[0]


def _hero_text(device: DeviceClient, expected: str) -> bool:
    w = device.find_widget(tag="hero_state")
    return w is not None and _hero_word(w) == expected


def _text_has(device: DeviceClient, tag: str, needle: str) -> bool:
    w = device.find_widget(tag=tag)
    return w is not None and w.text is not None and needle in w.text


def _lit(device: DeviceClient, tag: str) -> bool:
    w = device.find_widget(tag=tag)
    return w is not None and w.bg_color != COLOR_INACTIVE


def _unlit(device: DeviceClient, tag: str) -> bool:
    w = device.find_widget(tag=tag)
    return w is not None and w.bg_color == COLOR_INACTIVE


def _present(device: DeviceClient, tag: str) -> bool:
    return device.find_widget(tag=tag) is not None


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
        working_mode=MODE_HEATING,
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


# =========================================================================
# Hero Card — State Text
# =========================================================================

class TestHeroState:
    """Verify the hero card shows the correct operating state."""

    def _assert_hero(self, device: DeviceClient, expected: str):
        hero = device.find_widget(tag="hero_state")
        assert hero is not None, "hero_state widget not found"
        assert _hero_word(hero) == expected, \
            f"Expected hero state '{expected}', got '{hero.text}'"
        return hero

    def test_hero_shows_fault_with_error(self, device: DeviceClient):
        """With an active fault, hero should show 'Fault · <code>'."""
        # Default demo state has the P02 fault active, so hero = Fault
        _wait(device, lambda: _hero_text(device, "Fault"),
              "hero_state == Fault")
        hero = self._assert_hero(device, "Fault")
        assert hero.text.endswith(f"{STATE_SEP}{DEMO_FAULT}"), \
            f"Expected the fault code after the state, got '{hero.text}'"

    def test_hero_shows_floor_heat(self, device: DeviceClient):
        """A running heating cycle should show the actual Heating operation."""
        device.clear_all_faults()
        _running(device)
        _wait(device, lambda: _hero_text(device, "Heating"),
              "hero_state == Heating (floor heat)")
        self._assert_hero(device, "Heating")

    def test_hero_shows_cooling(self, device: DeviceClient):
        """Switching to cooling mode with compressor on should show Cooling."""
        device.clear_all_faults()
        # Cooling operation is decoded from the reversing-valve bit (reg2129
        # bit2 = cooling_on), not the selected working_mode, so set both.
        _running(device, working_mode=MODE_COOLING, cooling_on=1)
        _wait(device, lambda: _hero_text(device, "Cooling"),
              "hero_state == Cooling")
        self._assert_hero(device, "Cooling")

    def test_hero_shows_hot_water(self, device: DeviceClient):
        """Hot-water selection still reports the actual heating operation."""
        device.clear_all_faults()
        _running(device, working_mode=MODE_HOT_WATER)
        _wait(device, lambda: _hero_text(device, "Heating"),
              "hero_state == Heating (hot water)")
        self._assert_hero(device, "Heating")

    def test_hero_shows_idle(self, device: DeviceClient):
        """Unit on, no faults, compressor off should show Idle (plus the why)."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=1, compressor_freq=0, fan_on=0,
                               fan_speed=FAN_OFF, pump_on=1)
        _wait(device, lambda: _hero_text(device, "Idle"),
              "hero_state == Idle")
        hero = self._assert_hero(device, "Idle")
        label = hero.text_en or hero.text
        if STATE_SEP in label:
            assert label.split(STATE_SEP, 1)[1] in ("at setpoint", "no demand"), \
                f"Unexpected idle detail in '{label}'"

    def test_hero_shows_standby(self, device: DeviceClient):
        """Unit off should show Standby."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=0, compressor_freq=0, fan_on=0,
                               fan_speed=FAN_OFF, pump_on=0)
        _wait(device, lambda: _hero_text(device, "Standby"),
              "hero_state == Standby")
        self._assert_hero(device, "Standby")


# =========================================================================
# Hero Card — Tank Temperature
# =========================================================================

class TestHeroTankTemp:
    """Verify the hero card displays the tank temperature correctly."""

    def test_tank_temp_displayed(self, device: DeviceClient):
        """Tank temp is the bare number; the unit sits in its own label."""
        device.clear_all_faults()
        device.set_demo_fields(water_tank_temp=50)
        _wait(device, lambda: _text_has(device, "hero_tank_temp", "50"),
              "hero_tank_temp shows 50")
        tank = device.find_widget(tag="hero_tank_temp")
        assert tank is not None, "hero_tank_temp widget not found"
        assert tank.text == "50", f"Expected '50' in tank text, got '{tank.text}'"
        unit = device.find_widget(tag="hero_tank_unit")
        assert unit is not None, "hero_tank_unit widget not found"
        assert unit.text in ("°C", "°F"), f"Unexpected tank unit '{unit.text}'"

    def test_tank_caption_shown(self, device: DeviceClient):
        """The caption under the number names the tank."""
        sub = device.find_widget(tag="hero_sub")
        assert sub is not None, "hero_sub widget not found"
        assert (sub.text_en or sub.text).startswith("Tank"), \
            f"Expected the tank caption, got '{sub.text}'"


# =========================================================================
# Component Pills
# =========================================================================

class TestComponentPills:
    """Verify the component indicator pills reflect run state."""

    def _assert_bars(self, device: DeviceClient, lit: int):
        for i in range(1, 4):
            bar = device.find_widget(tag=f"home_ind_fan_bar_{i}")
            assert bar is not None, f"home_ind_fan_bar_{i} widget not found"
            if i <= lit:
                assert bar.bg_color != COLOR_INACTIVE, \
                    f"Bar {i} should be lit ({lit} bars), got {bar.bg_color}"
            else:
                assert bar.bg_color == COLOR_INACTIVE, \
                    f"Bar {i} should be unlit ({lit} bars), got {bar.bg_color}"

    def _wait_bars(self, device: DeviceClient, lit: int):
        _wait(device, lambda: all(
                  (_lit if i <= lit else _unlit)(device, f"home_ind_fan_bar_{i}")
                  for i in range(1, 4)),
              f"fan bars = {lit} lit")

    def test_compressor_pill_on(self, device: DeviceClient):
        """Compressor running should light the compressor pill."""
        device.clear_all_faults()
        _running(device)
        _wait(device, lambda: _lit(device, "home_ind_compressor"),
              "home_ind_compressor lit")
        pill = device.find_widget(tag="home_ind_compressor")
        assert pill is not None, "home_ind_compressor widget not found"
        assert pill.bg_color != COLOR_INACTIVE, \
            f"Compressor pill should be lit, got bg_color={pill.bg_color}"

    def test_compressor_pill_off(self, device: DeviceClient):
        """Compressor stopped should dim the compressor pill."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=1, compressor_freq=0, fan_on=0,
                               fan_speed=FAN_OFF, pump_on=1)
        _wait(device, lambda: _unlit(device, "home_ind_compressor"),
              "home_ind_compressor unlit")
        pill = device.find_widget(tag="home_ind_compressor")
        assert pill is not None
        assert pill.bg_color == COLOR_INACTIVE, \
            f"Compressor pill should be unlit, got bg_color={pill.bg_color}"

    def test_fan_pill_on(self, device: DeviceClient):
        """Fan running should light the fan pill."""
        device.clear_all_faults()
        _running(device)
        _wait(device, lambda: _lit(device, "home_ind_fan"), "home_ind_fan lit")
        pill = device.find_widget(tag="home_ind_fan")
        assert pill is not None, "home_ind_fan widget not found"
        assert pill.bg_color != COLOR_INACTIVE, \
            f"Fan pill should be lit, got bg_color={pill.bg_color}"

    def test_fan_speed_bars_medium(self, device: DeviceClient):
        """FAN_MED — bars 1 and 2 lit, bar 3 unlit."""
        device.clear_all_faults()
        _running(device, fan_speed=FAN_MED)
        self._wait_bars(device, 2)
        self._assert_bars(device, 2)

    def test_fan_speed_bars_high(self, device: DeviceClient):
        """FAN_HIGH — all 3 bars lit."""
        device.clear_all_faults()
        _running(device, fan_speed=FAN_HIGH)
        self._wait_bars(device, 3)
        self._assert_bars(device, 3)

    def test_fan_speed_bars_low(self, device: DeviceClient):
        """FAN_LOW — only bar 1 lit."""
        device.clear_all_faults()
        _running(device, fan_speed=FAN_LOW)
        self._wait_bars(device, 1)
        self._assert_bars(device, 1)

    def test_fan_speed_bars_off(self, device: DeviceClient):
        """Fan stopped — pill and all 3 bars unlit."""
        device.clear_all_faults()
        _running(device, fan_on=0, fan_speed=FAN_OFF)
        self._wait_bars(device, 0)
        self._assert_bars(device, 0)
        pill = device.find_widget(tag="home_ind_fan")
        assert pill is not None
        assert pill.bg_color == COLOR_INACTIVE, \
            f"Fan pill should be unlit, got bg_color={pill.bg_color}"

    def test_pump_pill_on(self, device: DeviceClient):
        """Water pump running should light the pump pill."""
        device.clear_all_faults()
        _running(device)
        _wait(device, lambda: _lit(device, "home_ind_pump"), "home_ind_pump lit")
        pill = device.find_widget(tag="home_ind_pump")
        assert pill is not None, "home_ind_pump widget not found"
        assert pill.bg_color != COLOR_INACTIVE, \
            f"Pump pill should be lit, got bg_color={pill.bg_color}"

    def test_pump_pill_off(self, device: DeviceClient):
        """Water pump stopped should dim the pump pill."""
        device.clear_all_faults()
        _running(device, pump_on=0)
        _wait(device, lambda: _unlit(device, "home_ind_pump"),
              "home_ind_pump unlit")
        pill = device.find_widget(tag="home_ind_pump")
        assert pill is not None
        assert pill.bg_color == COLOR_INACTIVE, \
            f"Pump pill should be unlit, got bg_color={pill.bg_color}"

    def test_heater_pill_hidden_by_default(self, device: DeviceClient):
        """The aux heater pill only shows while the heater runs (off in demo)."""
        device.clear_all_faults()
        _running(device)
        _wait(device, lambda: _present(device, "home_ind_compressor"),
              "home_indicators present")
        assert device.find_widget(tag="home_ind_heater") is None, \
            "Aux heater pill should be hidden while the heater is off"

    @pytest.mark.skip(reason="Backup/aux heater is not mapped from any Tuya "
                             "register yet; always off. See sun-peaks TODO "
                             "(fan/aux-heater register rework).")
    def test_heater_pill_on(self, device: DeviceClient):
        """Turning on backup heater should show the heater pill — no mapping yet."""


# =========================================================================
# Tiles
# =========================================================================

class TestTiles:
    """Verify the supply/return/power tiles update from demo state."""

    def test_supply_and_return_displayed(self, device: DeviceClient):
        """Supply and return tiles show the outlet/inlet water temperatures."""
        device.clear_all_faults()
        _running(device, outlet_water_temp=47, inlet_water_temp=39)
        _wait(device, lambda: _text_has(device, "home_supply", "47")
              and _text_has(device, "home_return", "39"),
              "home_supply shows 47, home_return shows 39")
        supply = device.find_widget(tag="home_supply")
        ret = device.find_widget(tag="home_return")
        assert supply is not None and "47" in supply.text, \
            f"Expected '47' in supply tile, got '{supply.text if supply else None}'"
        assert ret is not None and "39" in ret.text, \
            f"Expected '39' in return tile, got '{ret.text if ret else None}'"

    def test_power_displayed(self, device: DeviceClient):
        """Power consumption should be displayed when compressor is running."""
        device.clear_all_faults()
        _running(device, ac_voltage=230, ac_current=52)
        _wait(device, lambda: _present(device, "perf_power"),
              "perf_power present")
        power = device.find_widget(tag="perf_power")
        assert power is not None, "perf_power widget not found"


# =========================================================================
# Fault Banner
# =========================================================================

class TestFaultBanner:
    """Verify the fault banner reflects fault state."""

    def test_no_errors_hides_banner(self, device: DeviceClient):
        """With no faults the fault banner is hidden entirely."""
        device.clear_all_faults()
        _wait(device, lambda: not _present(device, "home_fault_banner"),
              "home_fault_banner hidden (no faults)")
        assert device.find_widget(tag="home_fault_banner") is None, \
            "Fault banner should be hidden with no active faults"
        assert device.find_widget(tag="error_label") is None

    def test_error_shows_description(self, device: DeviceClient):
        """With an active fault, the banner should display the error code."""
        device.clear_all_faults()
        device.inject_fault("P02", True)
        _wait(device, lambda: _text_has(device, "error_label", "P02"),
              "error_label shows P02")
        err = device.find_widget(tag="error_label")
        assert err is not None
        # P02 is the high pressure error code
        assert "P02" in err.text, f"Expected 'P02' in error text, got '{err.text}'"