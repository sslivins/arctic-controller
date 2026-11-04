"""
Test: Status tab temperature rows

Every temperature reading on the Status tab renders its translated name, the
OEM code pill and the demo value, with the unit in a separate muted label.
The rest of the tab (summary, chips, derived rows, defrost, faults) is
covered by test_system_screen.py; °F conversion by test_fahrenheit.py.
"""

import pytest
from device_client import DeviceClient

# tag -> (name, OEM code, demo °C)
DEMO_TEMPS = {
    "st_tank": ("Tank", "o1", 42),
    "st_outlet": ("Outlet", "o3", 45),
    "st_inlet": ("Inlet", "o2", 38),
    "st_outdoor": ("Outdoor air", "o4", 22),
    "st_discharge": ("Discharge", "A1", 85),
    "st_suction": ("Suction", "A3", 12),
    "st_outdoor_coil": ("Outdoor coil", "A2", 35),
    "st_water_hx": ("Water HX", "A6", 40),
    "st_ipm": ("IPM module", "A8", 55),
}


@pytest.fixture(autouse=True)
def _seed_temps(device: DeviceClient):
    device.set_demo_fields(
        water_tank_temp=42, outlet_water_temp=45, inlet_water_temp=38,
        outdoor_ambient_temp=22, discharge_temp=85, suction_temp=12,
        outdoor_coil_temp=35, indoor_coil_temp=40, ipm_temp=55,
    )


def test_all_temperature_rows(device: DeviceClient):
    device.click(tag="nav_status")
    assert device.wait_for_screen("status", timeout=5.0)

    def _rendered():
        return all(
            (w := device.find_widget(tag=tag)) is not None and w.text == str(c)
            for tag, (_, _, c) in DEMO_TEMPS.items()
        )
    device.wait_until("all temperature rows rendered", _rendered, timeout=5.0)

    widgets = device.widgets
    by_tag = {w.tag: w for w in widgets if w.tag}
    problems = []
    for tag, (name, code, celsius) in DEMO_TEMPS.items():
        val = by_tag[tag]
        pill = by_tag.get(f"{tag}_code")
        if pill is None or pill.text != code:
            problems.append(f"{tag}: pill {pill and pill.text!r} != {code!r}")
        # Name and unit sit on the value's row.
        row = [w for w in widgets if abs((w.y + w.h / 2) - (val.y + val.h / 2)) < 20]
        if not any((w.text_en or w.text) == name for w in row):
            problems.append(f"{tag}: name {name!r} not on its row")
        if not any(w.text == "°C" for w in row):
            problems.append(f"{tag}: °C unit not on its row")
    assert not problems, problems