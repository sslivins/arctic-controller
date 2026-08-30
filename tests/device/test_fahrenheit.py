"""
Test: Fahrenheit / Celsius Unit Conversion

Verifies that switching the temperature unit preference to Fahrenheit causes
all temperature displays (main screen hero card, temps sub-screen, expandable
panels) to show correctly converted values with the °F symbol.

Conversion formula: °F = round(°C × 9/5 + 32)

Demo mode register defaults (integer °C, *not* tenths):
  water_tank_temp = 42  → 108°F
  outlet_water_temp = 45 → 113°F
  inlet_water_temp = 38 → 100°F
  outdoor_ambient_temp = 22 → 72°F
  discharge_temp = 85 → 185°F
  suction_temp = 12 → 54°F
  outdoor_coil_temp = 35 → 95°F
  indoor_coil_temp = 40 → 104°F
  ipm_temp = 55 → 131°F

Important: The main screen and API read from s_state (populated via
set_demo_fields / pollTemperatures). The temps sub-screen hardcodes
its own values in demo mode — same numbers, independent code path.

Cleanup: An autouse fixture restores Celsius after every test.
The conftest ensure_main_screen fixture returns to main between tests.
"""

import pytest
from device_client import DeviceClient

UI_SETTLE = 1.5

# Demo mode temps (integer °C — set via set_demo_fields)
DEMO_TEMPS_C = {
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


def _c_to_f(celsius: int) -> int:
    """Convert Celsius to Fahrenheit using the same formula as the firmware."""
    return round(celsius * 9 / 5 + 32)


def _has_text_containing(device: DeviceClient, substring: str) -> bool:
    """Check if any widget's text contains the given substring."""
    sub_lower = substring.lower()
    for w in device.widgets:
        t = w.text_en or w.text
        if t and sub_lower in t.lower():
            return True
    return False


def _wait_widget_contains(device: DeviceClient, tag: str, substring: str,
                          timeout: float = 5.0):
    """Wait until widget ``tag`` renders text containing ``substring``.

    Temperature displays re-render asynchronously after a unit toggle or a
    demo-field update, so poll for the expected value instead of a fixed
    UI_SETTLE guess. Best-effort - the caller's assert is authoritative.
    """
    def _ready():
        w = device.find_widget(tag=tag)
        return w is not None and w.text is not None and substring in w.text
    device.wait_until(
        f"widget {tag!r} to contain {substring!r}", _ready,
        timeout=timeout, expect_within=UI_SETTLE, raise_on_timeout=False,
    )


def _wait_screen_text(device: DeviceClient, substring: str, timeout: float = 5.0):
    """Wait until any widget on the current screen contains ``substring``."""
    device.wait_until(
        f"screen text containing {substring!r}",
        lambda: _has_text_containing(device, substring),
        timeout=timeout, expect_within=UI_SETTLE, raise_on_timeout=False,
    )


def _wait_temp_unit(device: DeviceClient, unit: str, timeout: float = 5.0):
    """Wait until the preferences API reports ``temp_unit == unit``."""
    device.wait_until(
        f"temp_unit == {unit!r}",
        lambda: device.get_preferences().get("temp_unit") == unit,
        timeout=timeout, poll=0.1, raise_on_timeout=False,
    )


# =========================================================================
# Fixtures
# =========================================================================

@pytest.fixture(autouse=True)
def _restore_celsius(device: DeviceClient):
    """Restore Celsius after every test in this module.

    The test may leave the device on any screen (e.g. temps sub-screen).
    We navigate back to main first, then toggle the temp unit if needed.
    """
    yield
    # Navigate back to main if we're on a sub-screen
    try:
        current = device.screen
        if current != "main":
            if current in ("status", "control", "event_log"):
                device.click(tag="nav_home")
                device.wait_for_screen("main", timeout=5.0)
            elif current == "errors":
                device.click(tag="errors_close")
                device.wait_for_screen("main", timeout=5.0)
            elif current == "settings":
                device.click(tag="settings_close")
                device.wait_for_screen("main", timeout=5.0)
    except Exception:
        pass  # Best effort — conftest ensure_main_screen will clean up next
    # Now restore celsius from main screen
    try:
        prefs = device.get_preferences()
        if prefs["temp_unit"] == "fahrenheit":
            device.click(tag="settings")
            device.wait_for_screen("settings", timeout=5.0)
            device.toggle("temp_unit_switch")
            _wait_temp_unit(device, "celsius")
            device.click(tag="settings_close")
            device.wait_for_screen("main", timeout=5.0)
    except Exception:
        pass  # Best effort


def _reset_demo_temps(device: DeviceClient):
    """Reset demo temperatures to known defaults."""
    device.set_demo_fields(
        water_tank_temp=42, outlet_water_temp=45, inlet_water_temp=38,
        outdoor_ambient_temp=22, discharge_temp=85, suction_temp=12,
        outdoor_coil_temp=35, indoor_coil_temp=40, ipm_temp=55,
    )


def _switch_to_celsius(device: DeviceClient):
    """Switch to Celsius from main screen."""
    if device.screen != "main":
        device.wait_for_screen("main", timeout=5.0)
    device.wait_for_widget(tag="settings", timeout=3.0)
    prefs = device.get_preferences()
    if prefs["temp_unit"] == "fahrenheit":
        device.click(tag="settings")
        device.wait_for_screen("settings", timeout=5.0)
        device.toggle("temp_unit_switch")
        _wait_temp_unit(device, "celsius")
        device.click(tag="settings_close")
        device.wait_for_screen("main", timeout=5.0)
        device.wait_for_widget(tag="settings", timeout=3.0)


def _switch_to_fahrenheit(device: DeviceClient):
    """Switch to Fahrenheit from main screen. Asserts success."""
    # Ensure we're on main screen first
    if device.screen != "main":
        device.wait_for_screen("main", timeout=5.0)
    device.wait_for_widget(tag="settings", timeout=3.0)
    prefs = device.get_preferences()
    if prefs["temp_unit"] == "celsius":
        device.click(tag="settings")
        assert device.wait_for_screen("settings", timeout=5.0)
        device.toggle("temp_unit_switch")
        _wait_temp_unit(device, "fahrenheit")
        assert device.get_preferences()["temp_unit"] == "fahrenheit"
        device.click(tag="settings_close")
        device.wait_for_screen("main", timeout=5.0)
        device.wait_for_widget(tag="settings", timeout=3.0)


# =========================================================================
# Main Screen — Hero Tank Temperature in °F
# =========================================================================

class TestHeroTankFahrenheit:
    """Hero card tank temperature displays correctly in Fahrenheit."""

    def test_hero_tank_shows_fahrenheit_value(self, device: DeviceClient):
        """After switching to °F, hero card shows converted temperature."""
        _reset_demo_temps(device)
        _switch_to_fahrenheit(device)
        _wait_widget_contains(device, "hero_tank_temp", str(_c_to_f(42)))

        tank = device.find_widget(tag="hero_tank_temp")
        assert tank is not None, "hero_tank_temp widget not found"
        expected_f = _c_to_f(42)  # 108
        assert str(expected_f) in tank.text, \
            f"Expected '{expected_f}' in hero tank, got '{tank.text}'"

    def test_hero_tank_shows_f_unit(self, device: DeviceClient):
        """Hero card should show °F unit symbol."""
        _switch_to_fahrenheit(device)
        _wait_widget_contains(device, "hero_tank_temp", "°F")

        tank = device.find_widget(tag="hero_tank_temp")
        assert tank is not None, "hero_tank_temp widget not found"
        assert "°F" in tank.text, \
            f"Expected '°F' in hero tank, got '{tank.text}'"

    def test_hero_tank_celsius_after_restore(self, device: DeviceClient):
        """After toggling back to °C, hero card shows Celsius value."""
        _reset_demo_temps(device)
        _switch_to_fahrenheit(device)
        _switch_to_celsius(device)
        _wait_widget_contains(device, "hero_tank_temp", "°C")

        tank = device.find_widget(tag="hero_tank_temp")
        assert tank is not None, "hero_tank_temp widget not found"
        assert "42" in tank.text, \
            f"Expected '42' in hero tank after restore, got '{tank.text}'"
        assert "°C" in tank.text, \
            f"Expected '°C' in hero tank after restore, got '{tank.text}'"


# =========================================================================
# Temperatures Screen — All 9 Readings in °F
# =========================================================================

class TestTempsScreenFahrenheit:
    """Temperatures sub-screen displays all values in Fahrenheit.

    The temps sub-screen hardcodes its own demo values (same numbers as
    the demo register defaults). Each test opens the screen fresh — the
    conftest ensure_main_screen + _restore_celsius handle cleanup.
    """

    @pytest.mark.parametrize("label,celsius", list(DEMO_TEMPS_C.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_TEMPS_C])
    def test_temp_value_in_fahrenheit(self, device: DeviceClient, label, celsius):
        """Each temperature should display the correct °F value."""
        _switch_to_fahrenheit(device)
        device.wait_for_widget(tag="nav_status", timeout=3.0)
        device.click(tag="nav_status")
        assert device.wait_for_screen("status", timeout=5.0)

        expected_f = _c_to_f(celsius)
        _wait_screen_text(device, f"{expected_f} °F")
        # Temps screen uses "value °F" format (with space)
        assert _has_text_containing(device, f"{expected_f} °F"), \
            f"Expected '{expected_f} °F' for {label}, not found on screen"

    def test_no_celsius_symbol_in_f_mode(self, device: DeviceClient):
        """When in °F mode, no widget should contain '°C'."""
        _switch_to_fahrenheit(device)
        device.wait_for_widget(tag="nav_status", timeout=3.0)
        device.click(tag="nav_status")
        assert device.wait_for_screen("status", timeout=5.0)
        # Wait for the temps to render in °F before asserting no °C remains
        _wait_screen_text(device, "°F")

        for w in device.widgets:
            t = w.text_en or w.text
            if t and "°C" in t:
                pytest.fail(f"Found '°C' in widget while in °F mode: '{t}'")


# =========================================================================
# Preferences API — Unit Reported Correctly
# =========================================================================

class TestPreferencesUnitAPI:
    """The preferences API reports the correct temp_unit after toggle."""

    def test_api_reports_fahrenheit(self, device: DeviceClient):
        """After switching to °F, API should report 'fahrenheit'."""
        _switch_to_fahrenheit(device)
        prefs = device.get_preferences()
        assert prefs["temp_unit"] == "fahrenheit"

    def test_api_reports_celsius_after_restore(self, device: DeviceClient):
        """After toggling back, API should report 'celsius'."""
        _switch_to_fahrenheit(device)
        _switch_to_celsius(device)

        prefs = device.get_preferences()
        assert prefs["temp_unit"] == "celsius"


# =========================================================================
# Conversion Math Verification
# =========================================================================

class TestConversionMath:
    """Verify key conversion values match firmware formula."""

    @pytest.mark.parametrize("celsius,expected_f", [
        (0, 32),
        (100, 212),
        (42, 108),
        (-10, 14),
        (22, 72),
        (85, 185),
        (12, 54),
    ])
    def test_c_to_f_formula(self, celsius, expected_f):
        """Python conversion formula matches expected values."""
        assert _c_to_f(celsius) == expected_f

    def test_round_trip_preserves_value(self):
        """C→F→C should return the original value (within rounding)."""
        for c in [0, 20, 42, 85, -10]:
            f = _c_to_f(c)
            back = round((f - 32) * 5 / 9)
            assert back == c, f"Round-trip failed: {c}°C → {f}°F → {back}°C"


# =========================================================================
# Restore — Always run last
# =========================================================================

class TestRestoreCelsius:
    """Final cleanup: ensure device is back to Celsius."""

    def test_final_restore(self, device: DeviceClient):
        """Verify Celsius is active (autouse fixture handles the toggle)."""
        # The _restore_celsius fixture runs after each test, but this explicit
        # test ensures we leave the module in a known state.
        prefs = device.get_preferences()
        assert prefs["temp_unit"] == "celsius"
