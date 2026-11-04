"""
Test: Status tab (device)

The Status tab groups every reading like the web Status page: a summary card
(state, freshness, uptime, component chips, defrost panel), an optional fault
banner, then Water loop / Performance / Refrigerant circuit / Outdoor /
Electrical sections. Each row leads with its OEM display code as a pill (from
the arctic-macon field catalog) and the value is split into a number and a
muted unit label. Widgets carry stable tags: "st_<key>" (value),
"st_<key>_code" (pill), "st_<key>_sub" (sub-line).

Demo-mode values come from the seeded shadow register image (initDemoState()
in main/heatpump_controller.cpp), the same source the home screen renders
from, so the two screens cannot drift apart. Other modules write to the shared
demo image, so this module seeds what it asserts.
"""

import pytest
from device_client import DeviceClient

# tag -> (OEM code, value, unit) for the seeded demo state below. Temperature
# rows are covered by test_temps_screen.py.
DEMO_ROWS = {
    "compressor_freq": ("A14", "60", "Hz"),
    "fan_rpm": ("A10", "400", "RPM"),
    "ac_voltage": ("A13", "230", "V"),
    "ac_current": ("A4", "5", "A"),
    "dc_voltage": ("A7", "380", "V"),
    # EEV openings are shown as a percentage of the library's 500-step travel.
    "primary_eev": ("A5", "74", "%"),
    "aux_eev": ("o6", "58", "%"),
}

SECTION_HEADERS = {
    "st_hdr_water": "Water loop",
    "st_hdr_perf": "Performance",
    "st_hdr_refrig": "Refrigerant circuit",
    "st_hdr_outdoor": "Outdoor",
    "st_hdr_elec": "Electrical",
}

DEMO_FIELD_SEEDS = {
    "unit_on": 1,
    "compressor_freq": 60,
    "fan_on": 1,
    "pump_on": 1,
    "defrost_on": 0,
    "fan_speed": 400,
    "ac_voltage": 230,
    "ac_current": 5,
    "dc_voltage": 380,
    "primary_eev_opening": 370,
    "aux_eev": 290,
    "water_tank_temp": 42,
    "outlet_water_temp": 45,
    "inlet_water_temp": 38,
    "outdoor_ambient_temp": 22,
    "outdoor_coil_temp": 35,
    "indoor_coil_temp": 40,
    "suction_temp": 12,
    "discharge_temp": 85,
    "ipm_temp": 55,
    "heating_setpoint": 45,
}


@pytest.fixture(autouse=True)
def _seed_demo_readings(device: DeviceClient):
    """Seed the asserted demo values before each test, and restore after."""
    device.set_demo_fields(**DEMO_FIELD_SEEDS)
    yield
    device.set_demo_fields(**DEMO_FIELD_SEEDS)


def _open_status(device: DeviceClient):
    device.click(tag="nav_status")
    assert device.wait_for_screen("status", timeout=5.0), \
        f"Expected 'status' screen, got '{device.screen}'"


def _text(device: DeviceClient, tag: str):
    w = device.find_widget(tag=tag)
    return None if w is None else (w.text_en or w.text)


def _wait_text(device: DeviceClient, tag: str, predicate, timeout: float = 5.0):
    device.wait_until(
        f"widget {tag!r} text matches",
        lambda: predicate(_text(device, tag)),
        timeout=timeout,
    )
    return _text(device, tag)


class TestStatusNavigation:
    def test_open_and_close(self, device: DeviceClient):
        _open_status(device)
        device.click(tag="nav_home")
        assert device.wait_for_screen("main", timeout=5.0)


class TestStatusSections:
    @pytest.mark.parametrize("tag,title", list(SECTION_HEADERS.items()))
    def test_section_header(self, device: DeviceClient, tag, title):
        _open_status(device)
        assert _wait_text(device, tag, lambda t: t == title) == title


class TestStatusRows:
    @pytest.mark.parametrize("key", list(DEMO_ROWS))
    def test_row_code_value_unit(self, device: DeviceClient, key):
        code, value, unit = DEMO_ROWS[key]
        _open_status(device)
        _wait_text(device, f"st_{key}", lambda t: t == value)
        assert _text(device, f"st_{key}_code") == code
        assert _text(device, f"st_{key}") == value
        # The unit is the label right of the value: same row, to its right.
        val = device.find_widget(tag=f"st_{key}")
        units = [w for w in device.widgets
                 if (w.text_en or w.text) == unit and abs(w.y + w.h - (val.y + val.h)) < 12
                 and 0 <= w.x - (val.x + val.w) < 30]
        assert units, f"unit {unit!r} not rendered next to st_{key}"

    def test_eev_sub_line_shows_steps(self, device: DeviceClient):
        _open_status(device)
        sub = _wait_text(device, "st_primary_eev_sub", lambda t: t and "370" in t)
        assert sub == "370 of 500 steps"

    def test_no_setpoint_rows_and_target_in_summary(self, device: DeviceClient):
        """Only the active target is shown, inside the summary line."""
        _open_status(device)
        detail = _wait_text(device, "st_now_detail", lambda t: t and "target" in t)
        assert "Heating mode" in detail and "target 45 °C" in detail
        assert _text(device, "st_now") == "Heating"
        texts = [(w.text_en or w.text or "") for w in device.widgets]
        assert not any(t.startswith("Hot Water") for t in texts)

    def test_all_status_text_renders(self, device: DeviceClient):
        """Every label on the tab (incl. Δ and ·) has glyphs in its font."""
        _open_status(device)
        _wait_text(device, "st_water_dt", lambda t: t and t != "--")
        bad = [(w.tag, w.text) for w in device.widgets
               if getattr(w, "missing_glyphs", 0)]
        assert not bad, f"labels with missing glyphs: {bad}"


class TestStatusDerived:
    def test_water_dt_signed_when_pump_running(self, device: DeviceClient):
        _open_status(device)
        assert _wait_text(device, "st_water_dt", lambda t: t == "+7") == "+7"

    def test_water_dt_hidden_when_pump_off(self, device: DeviceClient):
        device.set_demo_fields(pump_on=0)
        _open_status(device)
        assert _wait_text(device, "st_water_dt", lambda t: t == "--") == "--"
        chip = device.find_widget(tag="st_chip_pump")
        assert chip is not None and chip.bg_color == "#555555"

    def test_discharge_warns_near_limit(self, device: DeviceClient):
        device.set_demo_fields(discharge_temp=104)
        _open_status(device)
        sub = _wait_text(device, "st_discharge_sub", lambda t: t == "Near limit")
        assert sub == "Near limit"

    def test_negative_estimate_is_not_shown(self, device: DeviceClient):
        # Seeds put the suction (12) below the outdoor coil (35).
        _open_status(device)
        sub = _wait_text(device, "st_superheat_sub", lambda t: t == "Below zero: sensors disagree")
        assert sub == "Below zero: sensors disagree"
        assert _text(device, "st_superheat") == "--"
        device.set_demo_fields(suction_temp=38)
        assert _wait_text(device, "st_superheat", lambda t: t == "3") == "3"


class TestStatusDefrost:
    def test_defrost_panel_and_paused_performance(self, device: DeviceClient):
        device.set_demo_fields(defrost_on=1, outlet_water_temp=33, inlet_water_temp=38)
        _open_status(device)
        assert _wait_text(device, "st_now", lambda t: t == "Defrosting") == "Defrosting"
        assert device.find_widget(tag="st_defrost") is not None
        assert _text(device, "st_defrost_water") == "33 / 38 °C"
        assert _wait_text(device, "st_defrost_elapsed", lambda t: bool(t))
        assert _text(device, "st_cop") == "--"
        assert _text(device, "st_cop_sub") == "Paused during defrost"
        assert _text(device, "st_superheat") == "--"
        assert _text(device, "st_water_dt") == "-5"
        assert _text(device, "st_water_dt_sub") == "Reversed during defrost"
        chip = device.find_widget(tag="st_chip_defrost")
        assert chip is not None and chip.bg_color == "#3b82f6"

    def test_no_defrost_panel_normally(self, device: DeviceClient):
        _open_status(device)
        _wait_text(device, "st_now", lambda t: t == "Heating")
        assert device.find_widget(tag="st_defrost") is None


class TestStatusFault:
    def test_fault_banner(self, device: DeviceClient):
        device.inject_fault("P02", True)
        try:
            _open_status(device)
            code = _wait_text(device, "st_fault_code", lambda t: t == "P02")
            assert code == "P02"
            assert _text(device, "st_fault_title")
        finally:
            device.inject_fault("P02", False)

    def test_no_fault_banner_without_faults(self, device: DeviceClient):
        _open_status(device)
        _wait_text(device, "st_now", lambda t: t == "Heating")
        assert device.find_widget(tag="st_fault") is None