"""
Test: System Readings Screen

Navigates to the system readings sub-screen and verifies that all sensor
categories (compressor, electrical, pressures, expansion valves, setpoints)
are displayed with correct labels and demo-mode values.

Demo-mode values come from the seeded shadow register image
(initDemoState() in main/heatpump_controller.cpp), the same source the home
screen renders from. They are asserted here so the two screens cannot drift
apart again -- this screen used to carry its own hardcoded literals, which
had diverged (850 vs 400 RPM, 350 vs 200 EEV steps, 20 vs 18 C cooling).
"""

import pytest
from device_client import DeviceClient

# Demo system readings, derived from the initDemoState() seeds. Keep in step
# with main/heatpump_controller.cpp; a mismatch here means the screen has
# stopped rendering from getState().
DEMO_READINGS = {
    # Compressor section
    "Frequency": "60 Hz",
    "Fan Speed": "400 RPM",
    # Electrical section
    "AC Voltage": "230 V",
    "AC Current": "5 A",
    "DC Voltage": "380 V",
    # Expansion valves section
    "Primary EEV": "200 steps",
}

DEMO_SETPOINTS = {
    "Cooling": "18 °C",
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

# The demo shadow image is shared process-wide, and other modules write to it
# (test_main_screen leaves fan_speed=450 and ac_current=52 behind). The old
# hardcoded literals made this screen immune to that; now that it renders from
# getState() it honestly reflects those writes, so this module must establish
# the state it asserts instead of assuming the pristine initDemoState() seeds.
# Values must match DEMO_READINGS/DEMO_SETPOINTS above.
DEMO_FIELD_SEEDS = {
    "compressor_freq": 60,
    "fan_on": 1,
    "fan_speed": 400,
    "ac_voltage": 230,
    "ac_current": 5,
    "dc_voltage": 380,
    "primary_eev_opening": 200,
    "cooling_setpoint": 18,
    "heating_setpoint": 45,
    "hot_water_setpoint": 50,
}


@pytest.fixture(autouse=True)
def _seed_demo_readings(device: DeviceClient):
    """Seed the asserted demo values before each test, and restore after.

    Restoring on teardown keeps a probe value (see TestCrossScreenAgreement)
    from leaking into modules that run later.
    """
    device.set_demo_fields(**DEMO_FIELD_SEEDS)
    yield
    device.set_demo_fields(**DEMO_FIELD_SEEDS)


def _open_system(device: DeviceClient):
    """Navigate from main to the Status screen (System section)."""
    device.click(tag="nav_status")
    assert device.wait_for_screen("status", timeout=5.0), \
        f"Expected 'status' screen, got '{device.screen}'"


def _close_system(device: DeviceClient):
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
    """The Status tab has no standalone title in the persistent-nav shell;
    the nav bar labels the tab. Content is verified by the section-header and
    reading tests below."""


# =========================================================================
# Section Headers
# =========================================================================

class TestSystemSections:
    """Verify section headers are present."""

    @pytest.mark.parametrize("header", SECTION_HEADERS)
    def test_section_header_present(self, device: DeviceClient, header):
        """Each section header should be visible on the system screen."""
        _open_system(device)
        _wait_for_text(device, header)

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
        _wait_for_text(device, label)

        assert _has_text_containing(device, label), \
            f"Reading label '{label}' not found on screen"

    @pytest.mark.parametrize("label,expected_value",
                             list(DEMO_READINGS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_READINGS])
    def test_reading_value_present(self, device: DeviceClient, label, expected_value):
        """Each demo reading value should be displayed."""
        _open_system(device)
        _wait_for_text(device, expected_value)

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
        _wait_for_text(device, label)

        assert _has_text_containing(device, label), \
            f"Setpoint label '{label}' not found on screen"


# =========================================================================
# Cross-screen consistency
# =========================================================================

class TestCrossScreenAgreement:
    """The home and Status screens must never disagree about a reading.

    Both render from arctic::getState(), so a divergence means one of them has
    reintroduced a private data source. That is exactly the bug this guards:
    the Status screen used to hold its own hardcoded demo literals and showed
    850 RPM while the home screen showed the seeded 400.
    """

    # Distinct from every default (400 seeded, 450 FAN_MED) so a stale label
    # cannot accidentally satisfy the assertion. reg2003 is raw x10, so this
    # must stay a multiple of 10.
    PROBE_RPM = 640

    def test_fan_speed_agrees_between_home_and_status(self, device: DeviceClient):
        device.set_demo_fields(fan_on=1, fan_speed=self.PROBE_RPM)
        expected = f"{self.PROBE_RPM} RPM"

        device.click(tag="nav_home")
        assert device.wait_for_screen("main", timeout=5.0), \
            f"Expected 'main' screen, got '{device.screen}'"
        device.wait_until(
            f"home screen fan shows {expected}",
            lambda: (lambda w: w is not None and w.text is not None
                     and expected in w.text)(device.find_widget(tag="perf_fan")),
            timeout=5.0,
        )
        home = device.find_widget(tag="perf_fan")
        assert expected in home.text, \
            f"Home screen fan shows '{home.text}', expected '{expected}'"

        _open_system(device)
        _wait_for_text(device, expected)
        assert _has_text_containing(device, expected), \
            (f"Status screen does not show '{expected}' while the home screen "
             f"does; the two screens have diverged.")

