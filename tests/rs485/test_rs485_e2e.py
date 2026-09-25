"""
RS-485 end-to-end: the controller, as Macon bus master, must decode exactly
what the simulator (slave) serves, and its writes must land on the simulator
with the meaning the library defines.

The suite has no register map and no fault table. Inputs are named fields
(simulator GET /api/fields) and the fault catalog (GET /api/faults/catalog),
both produced by the arctic-macon library the controller is also built on.
The only mapping here is between the two firmwares' *public API names* (e.g.
the simulator's `outlet_water_temp` is the controller's
`temperatures.outlet`), which is API naming, not bus knowledge.
"""


import pytest

from conftest import BUS_TIMEOUT, LEASE_OWNER, controller_macon_identity, wait_until

# simulator field -> path in the controller's GET /api/heatpump/status
NUMERIC_FIELDS = {
    "water_tank_temp":      ("temperatures", "tank"),
    "outlet_water_temp":    ("temperatures", "outlet"),
    "inlet_water_temp":     ("temperatures", "inlet"),
    "outdoor_ambient_temp": ("temperatures", "outdoor"),
    "discharge_temp":       ("temperatures", "discharge"),
    "suction_temp":         ("temperatures", "suction"),
    "outdoor_coil_temp":    ("temperatures", "outdoor_coil"),
    "indoor_coil_temp":     ("temperatures", "indoor_coil"),
    "ipm_temp":             ("temperatures", "ipm"),
    "compressor_freq":      ("readings", "compressor_freq"),
    "fan_speed":            ("readings", "fan_rpm"),
    "ac_voltage":           ("readings", "ac_voltage"),
    "ac_current":           ("readings", "ac_current"),
    "dc_voltage":           ("readings", "dc_voltage"),
    "primary_eev":          ("readings", "primary_eev"),
    "realtime_power":       ("readings", "power_consumption"),
    "cooling_setpoint":     ("setpoints", "cooling"),
    "hot_water_setpoint":   ("setpoints", "hot_water"),
}

FLAG_FIELDS = {
    "pump_on":    "pump",
    "fan_on":     "fans",
    "defrost_on": "defrosting",
    "unit_on":    "unit_on",
}


def _get(d, path):
    for k in path:
        d = d[k]
    return d


def _active(device):
    return device.get_heatpump_errors().get("active") or []


# ---------------------------------------------------------------------------
# Identity + link
# ---------------------------------------------------------------------------

def test_library_identity_matches(sim, device):
    """Both firmwares were built against the same arctic-macon layout + catalog."""
    ctrl = controller_macon_identity(device)
    assert ctrl, "controller /api/status has no macon identity"
    assert ctrl == sim.macon_identity()


def test_controller_is_polling_the_simulator(sim, device):
    before = sim.bus_stats()
    wait_until(lambda: sim.bus_stats()["responses_sent"] >= before["responses_sent"] + 3,
               desc="simulator to answer controller polls")
    after = sim.bus_stats()
    assert after["parse_errors"] == before["parse_errors"]
    assert after["unknown_windows"] == before["unknown_windows"]
    assert device.get_heatpump_status()["connected"] is True


# ---------------------------------------------------------------------------
# Telemetry: every named numeric field, in its own units
# ---------------------------------------------------------------------------

def _probe_value(desc: dict, bias: int) -> int:
    """A value inside the field's range, on its step, distinct from idle."""
    lo, hi, step = desc["min"], desc["max"], desc.get("step") or 1
    if desc.get("unit") == "C" and lo < 0:
        v = 30 + bias  # plausible temperature; negatives covered separately
    else:
        v = min(hi, max(lo, (hi // 4) // step * step + bias * step))
    return v


@pytest.mark.parametrize("field", sorted(NUMERIC_FIELDS))
def test_numeric_field_reaches_controller(sim, device, field):
    v = _probe_value(sim.field(field), bias=3)
    sim.set(**{field: v})
    path = NUMERIC_FIELDS[field]
    wait_until(lambda: _get(device.get_heatpump_status(), path) == v,
               desc=f"controller {'.'.join(path)} == {v} (sim {field})")


@pytest.mark.parametrize("field", [
    "outdoor_ambient_temp", "outdoor_coil_temp", "suction_temp", "outlet_water_temp",
])
@pytest.mark.parametrize("value", [-1, -15, -30])
def test_negative_temperatures(sim, device, field, value):
    sim.set(**{field: value})
    path = NUMERIC_FIELDS[field]
    wait_until(lambda: _get(device.get_heatpump_status(), path) == value,
               desc=f"controller {'.'.join(path)} == {value}")


def test_many_fields_in_one_atomic_update(sim, device):
    target = {"outlet_water_temp": 44, "inlet_water_temp": 37,
              "compressor_freq": 52, "fan_speed": 650, "outdoor_ambient_temp": -4}
    sim.set(**target)

    def all_match():
        s = device.get_heatpump_status()
        return all(_get(s, NUMERIC_FIELDS[k]) == v for k, v in target.items())
    wait_until(all_match, desc="every field of one PATCH to reach the controller")


# ---------------------------------------------------------------------------
# Flags, compressor and derived operation
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("field", sorted(FLAG_FIELDS))
def test_flag_toggles(sim, device, field):
    key = FLAG_FIELDS[field]
    start = bool(sim.state()["fields"][field])
    for want in (not start, start):
        sim.set(**{field: want})
        wait_until(lambda: device.get_heatpump_status()[key] is want,
                   desc=f"controller {key} is {want}")


def test_compressor_follows_frequency(sim, device):
    sim.set(compressor_freq=45, compressor_icon=True)
    wait_until(lambda: device.get_heatpump_status()["compressor"] is True,
               desc="compressor running at 45 Hz")
    sim.set(compressor_freq=0, compressor_icon=False)
    wait_until(lambda: device.get_heatpump_status()["compressor"] is False,
               desc="compressor stopped at 0 Hz")


@pytest.mark.parametrize("preset", ["idle", "heating", "cooling", "hot_water", "defrost"])
def test_operation_matches_library_decode(sim, device, preset):
    sim.load_preset(preset)
    want = sim.state()["operation"].lower()
    wait_until(lambda: device.get_heatpump_status()["operation"] == want,
               desc=f"controller operation == {want!r} for preset {preset}")


def test_working_mode_reported(sim, device):
    for key in sim.field("working_mode")["options"]:
        sim.set(working_mode=key)
        wait_until(lambda: device.get_heatpump_status()["mode"] == key,
                   desc=f"controller mode == {key!r}")


# ---------------------------------------------------------------------------
# Faults: every code and every site in the library catalog
# ---------------------------------------------------------------------------

def test_fault_code_round_trip(sim, device, fault_entry):
    """Lighting a code on the sim shows it on the controller with the library's
    name + severity; clearing it moves it to history with a cleared time."""
    assert fault_entry, "simulator fault catalog unavailable"
    code = fault_entry["code"]
    sim.set_fault(code, True)

    def shown():
        mine = [e for e in _active(device) if e["code"] == code]
        return mine if len(mine) == len(fault_entry["sites"]) else None
    entries = wait_until(shown, desc=f"controller to show all {len(fault_entry['sites'])} site(s) of {code}")
    want = {(s["label"], s["severity"]) for s in fault_entry["sites"]}
    assert {(e["name"], e["severity"]) for e in entries} == want

    sim.set_fault(code, False)
    wait_until(lambda: not any(e["code"] == code for e in _active(device)),
               desc=f"{code} to clear on the controller")
    hist = [h for h in device.get_heatpump_errors().get("history") or [] if h["code"] == code]
    assert hist and all(h.get("cleared") for h in hist[:len(fault_entry["sites"])]), hist


def test_fault_site_round_trip(sim, device, fault_site):
    """One specific bit: the controller reports exactly that one entry."""
    assert fault_site, "simulator fault catalog unavailable"
    sim.set_fault_site(fault_site["site"], True)
    entries = wait_until(
        lambda: [e for e in _active(device) if e["code"] == fault_site["code"]] or None,
        desc=f"controller to show site {fault_site['site']} ({fault_site['code']})")
    assert len(entries) == 1
    assert entries[0]["name"] == fault_site["label"]
    assert entries[0]["severity"] == fault_site["severity"]
    assert device.get_heatpump_status()["has_error"] is True


def test_multiple_faults_highest_severity(sim, device):
    cat = sim.fault_catalog()
    by_sev = {}
    for f in cat:
        by_sev.setdefault(f["severity"], f["code"])
    codes = [c for c in by_sev.values()]
    for c in codes:
        sim.set_fault(c, True)
    def all_shown():
        e = device.get_heatpump_errors()
        return e if {x["code"] for x in e.get("active") or []} >= set(codes) else None
    errs = wait_until(all_shown, desc=f"controller to show {codes}")
    order = ["info", "warning", "error", "critical"]
    assert errs["highest_severity"] == max(by_sev, key=order.index)


def test_clear_all_faults(sim, device):
    sim.load_preset("fault_p01")
    wait_until(lambda: any(e["code"] == "P01" for e in _active(device)), desc="P01 active")
    sim.clear_faults()
    wait_until(lambda: device.get_heatpump_errors()["error_count"] == 0,
               desc="all faults cleared on the controller")


# ---------------------------------------------------------------------------
# Controller -> unit writes (fc=0x06), verified by meaning on the simulator
# ---------------------------------------------------------------------------

def _put(device, path, body):
    return device.session.put(f"{device.base_url}{path}", json=body, timeout=device.timeout)


@pytest.mark.parametrize("kind,field,value", [
    ("cooling", "cooling_setpoint", 18),
    ("cooling", "cooling_setpoint", 7),
    ("hot_water", "hot_water_setpoint", 48),
    ("hot_water", "hot_water_setpoint", 38),
])
def test_setpoint_write_lands_on_unit(sim, device, kind, field, value):
    before = sim.commands()["total"]
    r = _put(device, "/api/heatpump/setpoints", {kind: value})
    assert r.status_code == 200, r.text
    wait_until(lambda: sim.state()["fields"][field] == value,
               desc=f"simulator {field} == {value}")
    cmds = sim.commands_since(before)
    assert cmds and all(c["applied"] for c in cmds), cmds
    wait_until(lambda: device.get_heatpump_status()["setpoints"][kind] == value,
               desc=f"controller reads back {kind} == {value}")


def test_working_mode_write_lands_on_unit(sim, device):
    for key in sim.field("working_mode")["options"]:
        before = sim.commands()["total"]
        r = _put(device, "/api/heatpump/mode", {"mode": key})
        assert r.status_code == 200, r.text
        wait_until(lambda: sim.state()["fields"]["working_mode"] == key,
                   desc=f"simulator working_mode == {key!r}")
        assert all(c["applied"] for c in sim.commands_since(before))
        wait_until(lambda: device.get_heatpump_status()["mode"] == key,
                   desc=f"controller reads back mode {key!r}")


@pytest.mark.parametrize("path,body", [
    ("/api/heatpump/setpoints", {"heating": 45}),
    ("/api/heatpump/power", {"on": False}),
])
def test_unverified_writes_are_refused(sim, device, path, body):
    """Writes the library has no verified wire mapping for must fail loudly,
    not send a guess onto the bus."""
    before = sim.commands()["total"]
    r = _put(device, path, body)
    refused = r.status_code >= 400 or r.json().get("success") is False
    assert refused, r.text
    assert sim.commands()["total"] == before


# ---------------------------------------------------------------------------
# Resilience
# ---------------------------------------------------------------------------

def test_recovers_after_simulator_reboot(sim, device):
    sim.reboot()
    # The lease is RAM-only, so seeing it gone proves the sim really rebooted.
    wait_until(lambda: sim.lease().get("held") is False, timeout=60.0,
               desc="simulator back on WiFi after a reboot")
    sim.acquire_lease(LEASE_OWNER, ttl_s=1800)  # the lease lives in RAM
    sim.set(outlet_water_temp=41)
    wait_until(lambda: (s := device.get_heatpump_status())["connected"]
               and s["temperatures"]["outlet"] == 41,
               timeout=BUS_TIMEOUT * 3, desc="controller reconnected and decoding again")
