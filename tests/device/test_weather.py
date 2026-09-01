"""
Test: Status-Bar Weather Display

Verifies the top status bar shows the current outside weather next to the time:
a temperature in the user-selected unit (°C/°F) plus a condition icon that maps
from the Open-Meteo WMO weather code.

The weather is fetched from Open-Meteo, so every test installs a canned forecast
via ``device.weather_mock(temp_c, weather_code)`` (which also triggers a refresh)
so the display resolves deterministically without depending on the network.

Temperature is fetched/cached in Celsius and converted to the selected unit at
render time (°F = round(°C × 9/5 + 32)), so a unit toggle updates the status bar
instantly without a refetch.

The weather elements live on the persistent status bar, so they are asserted
from the main screen. Cleanup fixtures clear the mock and restore Celsius.
"""

import pytest
from device_client import DeviceClient

# WMO code -> expected status-bar icon glyph (FontAwesome PUA codepoints,
# must match the mapping in main/weather.cpp / the weather_icons_32 font).
ICON_SUN = "\uf185"          # clear
ICON_CLOUD_SUN = "\uf6c4"    # mainly/partly clear
ICON_CLOUD = "\uf0c2"        # overcast
ICON_FOG = "\uf75f"          # fog
ICON_RAIN = "\uf73d"         # rain / drizzle / showers
ICON_HEAVY_RAIN = "\uf740"   # heavy / freezing rain
ICON_SNOW = "\uf2dc"         # snow
ICON_THUNDER = "\uf0e7"      # thunderstorm


def _c_to_f(celsius: float) -> int:
    return round(celsius * 9 / 5 + 32)


def _weather_value(device: DeviceClient) -> str:
    w = device.find_widget(tag="weather_value")
    return (w.text or "") if w is not None else ""


def _weather_icon(device: DeviceClient) -> str:
    w = device.find_widget(tag="weather_icon")
    return (w.text or "") if w is not None else ""


def _wait_weather_value(device: DeviceClient, substring: str, timeout: float = 8.0):
    """Wait until the status-bar weather value contains ``substring``."""
    device.wait_until(
        f"status-bar weather value contains {substring!r}",
        lambda: substring in _weather_value(device),
        timeout=timeout, raise_on_timeout=False,
    )


def _wait_weather_icon(device: DeviceClient, glyph: str, timeout: float = 8.0):
    device.wait_until(
        f"status-bar weather icon is expected glyph",
        lambda: _weather_icon(device) == glyph,
        timeout=timeout, raise_on_timeout=False,
    )


def _ensure_main(device: DeviceClient):
    if device.screen != "main":
        device.wait_for_screen("main", timeout=5.0)


def _switch_unit(device: DeviceClient, unit: str):
    """Switch the temperature unit ('celsius'/'fahrenheit') via the settings UI."""
    _ensure_main(device)
    device.wait_for_widget(tag="settings", timeout=3.0)
    if device.get_preferences().get("temp_unit") == unit:
        return
    device.click(tag="settings")
    device.wait_for_screen("settings", timeout=5.0)
    device.toggle("temp_unit_switch")
    device.wait_until(
        f"temp_unit == {unit!r}",
        lambda: device.get_preferences().get("temp_unit") == unit,
        timeout=5.0, poll=0.1, raise_on_timeout=False,
    )
    device.click(tag="settings_close")
    device.wait_for_screen("main", timeout=5.0)


@pytest.fixture(autouse=True)
def _weather_cleanup(device: DeviceClient):
    """Clear the weather mock and restore Celsius after each test."""
    yield
    try:
        _switch_unit(device, "celsius")
    except Exception:
        pass
    try:
        device.weather_mock_reset()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Temperature display + units
# ---------------------------------------------------------------------------

def test_weather_shows_temperature_celsius(device: DeviceClient):
    """A mocked forecast renders the temperature in °C with a condition icon."""
    _switch_unit(device, "celsius")
    device.weather_mock(temp_c=-5, weather_code=71)  # snow
    _ensure_main(device)

    _wait_weather_value(device, "-5°C")
    assert "-5°C" in _weather_value(device), \
        f"Expected '-5°C' in status bar, got {_weather_value(device)!r}"
    assert _weather_icon(device) == ICON_SNOW, "Snow code should show the snow glyph"


def test_weather_temperature_fahrenheit(device: DeviceClient):
    """Toggling to °F converts the cached Celsius value instantly."""
    _switch_unit(device, "celsius")
    device.weather_mock(temp_c=-5, weather_code=3)
    _ensure_main(device)
    _wait_weather_value(device, "-5°C")

    _switch_unit(device, "fahrenheit")
    expected = f"{_c_to_f(-5)}°F"  # 23°F
    _wait_weather_value(device, expected)
    assert expected in _weather_value(device), \
        f"Expected {expected!r} after switching to °F, got {_weather_value(device)!r}"


def test_weather_rounds_temperature(device: DeviceClient):
    """A fractional temperature is rounded for display."""
    _switch_unit(device, "celsius")
    device.weather_mock(temp_c=7.4, weather_code=3)
    _ensure_main(device)

    _wait_weather_value(device, "7°C")
    assert "7°C" in _weather_value(device), \
        f"Expected '7°C' for 7.4°C, got {_weather_value(device)!r}"


# ---------------------------------------------------------------------------
# Condition icon mapping (WMO code -> glyph)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("code,glyph", [
    (0, ICON_SUN),
    (2, ICON_CLOUD_SUN),
    (3, ICON_CLOUD),
    (45, ICON_FOG),
    (61, ICON_RAIN),
    (65, ICON_HEAVY_RAIN),
    (71, ICON_SNOW),
    (95, ICON_THUNDER),
])
def test_weather_icon_maps_from_code(device: DeviceClient, code, glyph):
    """Each representative WMO code selects the expected condition glyph."""
    _switch_unit(device, "celsius")
    device.weather_mock(temp_c=10, weather_code=code)
    _ensure_main(device)

    _wait_weather_value(device, "10°C")
    _wait_weather_icon(device, glyph)
    assert _weather_icon(device) == glyph, \
        f"WMO code {code} expected glyph {glyph!r}, got {_weather_icon(device)!r}"
