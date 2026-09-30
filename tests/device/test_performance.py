"""
Test: Heat output & COP from external Modbus TCP sensors

Covers the Performance settings end to end against a real controller:
  * the /api/performance/config contract (shape, validation, partial updates),
  * the one-shot /api/performance/test read against a fake Thermux,
  * the background worker picking up configured sensors, learning the Thermux
    sensor ID and falling back when the sensor goes away,
  * the device screen and its sensor editor.

The controller has to reach a Modbus TCP server, so the module starts
fake_modbus_server.py on this machine (the self-hosted runner in CI) on a free
port and points the sensors at this machine's LAN address. Set
ARCTIC_FAKE_MODBUS_HOST to override the address the controller should use.

The saved performance settings are restored when the module finishes.
"""

import os
import re
import socket
import subprocess
import sys
from pathlib import Path
from typing import Optional
from urllib.parse import urlparse

import pytest

from device_client import DeviceClient

FAKE_SERVER = Path(__file__).with_name("fake_modbus_server.py")

# What the fake Thermux serves (see fake_modbus_server.py).
SUPPLY_REG, SUPPLY_C, SUPPLY_ROM = 104, 22.69, "28FF9A2B0F1C0412"
RETURN_REG, RETURN_C = 103, 21.25
FLOAT_REG, FLOAT_C = 500, 45.5
READ_ERROR_REG = 106      # channel 6 reports a read error
UNMAPPED_INPUT_REG = 50   # not served: Modbus exception 2

# The worker polls every 10 s and each step is bounded to 1.5 s.
POLL_TIMEOUT = 30.0

PERSISTED_SENSOR_FIELDS = (
    "source", "host", "port", "unit_id", "register", "register_type",
    "value_type", "scale", "no_reading",
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _url(device: DeviceClient, path: str) -> str:
    return f"{device.base_url}{path}"


def _get_config(device: DeviceClient) -> dict:
    r = device.session.get(_url(device, "/api/performance/config"), timeout=device.timeout)
    r.raise_for_status()
    return r.json()


def _put_config(device: DeviceClient, body: dict):
    return device.session.put(_url(device, "/api/performance/config"), json=body,
                              timeout=device.timeout)


def _test_read(device: DeviceClient, sensor: dict, slot: Optional[str] = None):
    body = {"sensor": sensor}
    if slot:
        body["slot"] = slot
    # The device bounds a test to ~8 s; leave headroom over DeviceClient's default.
    return device.session.post(_url(device, "/api/performance/test"), json=body, timeout=15)


def _saved_settings(cfg: dict) -> dict:
    """The part of a GET response that PUT accepts back."""
    return {
        "flow_lpm": cfg["flow_lpm"],
        "fluid": cfg["fluid"],
        "glycol_pct": cfg["glycol_pct"],
        "sensors": {
            slot: {k: s[k] for k in PERSISTED_SENSOR_FIELDS}
            for slot, s in cfg["sensors"].items()
        },
    }


def _modbus(fake, register: int, **overrides) -> dict:
    host, port = fake
    sensor = {
        "source": "modbus_tcp", "host": host, "port": port, "unit_id": 1,
        "register": register, "register_type": "input", "value_type": "int16",
        "scale": 0.01, "no_reading": "0x8000",
    }
    sensor.update(overrides)
    return sensor


def _heat_pump_sensors() -> dict:
    return {"supply": {"source": "heat_pump"}, "return": {"source": "heat_pump"}}


def _local_ip_towards(device_host: str) -> str:
    """This machine's address on the route to the device (no packets sent)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((socket.gethostbyname(device_host), 9))
        return s.getsockname()[0]


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("", 0))
        return s.getsockname()[1]


class FakeThermux:
    """fake_modbus_server.py in a subprocess that tests can stop and restart."""

    def __init__(self, advertise_host: str):
        self.host = advertise_host
        self.port = _free_port()
        self.proc = None

    @property
    def addr(self):
        return self.host, self.port

    def start(self):
        self.proc = subprocess.Popen(
            [sys.executable, "-u", str(FAKE_SERVER), "--port", str(self.port)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        # The server prints one line once it is listening (or a traceback).
        line = self.proc.stdout.readline()
        if "fake Thermux Modbus TCP" not in line:
            self.stop()
            pytest.fail(f"fake Modbus server did not start on port {self.port}: {line!r}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def saved_perf(device: DeviceClient):
    """Restores the performance settings the device had before this module."""
    original = _saved_settings(_get_config(device))
    yield original
    r = _put_config(device, original)
    assert r.status_code == 200, f"could not restore performance settings: {r.text}"


@pytest.fixture(scope="module")
def thermux(device: DeviceClient, saved_perf):
    host = os.environ.get("ARCTIC_FAKE_MODBUS_HOST") or _local_ip_towards(
        urlparse(device.base_url).hostname)
    server = FakeThermux(host)
    server.start()
    yield server
    # Stop polling the server before it goes away.
    _put_config(device, {"sensors": _heat_pump_sensors()})
    server.stop()


@pytest.fixture
def fake(thermux):
    """(host, port) of a running fake Thermux."""
    if thermux.proc is None:
        thermux.start()
    return thermux.addr


def _open_perf_screen(device: DeviceClient):
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    assert device.wait_for_widget(tag="settings_perf", timeout=5.0)
    device.click(tag="settings_perf")
    assert device.wait_for_screen("performance", timeout=5.0), \
        f"Performance screen did not open — still on '{device.screen}'"


def _number(text: str) -> float:
    m = re.search(r"-?\d+(?:\.\d+)?", text or "")
    assert m, f"no number in {text!r}"
    return float(m.group())


def _display_temp(device: DeviceClient, celsius: float) -> float:
    if device.get_preferences().get("temp_unit") == "fahrenheit":
        return celsius * 9 / 5 + 32
    return celsius


# ---------------------------------------------------------------------------
# API contract
# ---------------------------------------------------------------------------

class TestConfigApi:

    def test_config_shape(self, device: DeviceClient, saved_perf):
        cfg = _get_config(device)
        assert set(cfg) >= {"flow_lpm", "fluid", "glycol_pct", "sensors", "limits", "status"}
        assert set(cfg["sensors"]) == {"supply", "return"}
        for s in cfg["sensors"].values():
            assert set(s) >= set(PERSISTED_SENSOR_FIELDS) | {"rom_id"}
        assert cfg["limits"] == {
            "flow_min_lpm": 1, "flow_max_lpm": 300,
            "glycol_max_pct": cfg["limits"]["glycol_max_pct"], "glycol_step_pct": 5,
        }
        st = cfg["status"]
        assert st["source"] in ("heat_pump", "external")
        assert set(st["sensors"]) == {"supply", "return"}

    @pytest.mark.parametrize("body,field", [
        ({"flow_lpm": 0.5}, "flow_lpm"),
        ({"flow_lpm": 301}, "flow_lpm"),
        ({"flow_lpm": "40"}, "flow_lpm"),
        ({"fluid": "brine"}, "fluid"),
        ({"glycol_pct": 32}, "glycol_pct"),
        ({"glycol_pct": 12.5}, "glycol_pct"),
        ({"sensors": {"supply": {"port": 70000}}}, "port"),
        ({"sensors": {"supply": {"unit_id": 256}}}, "unit_id"),
        ({"sensors": {"supply": {"register_type": "coil"}}}, "register_type"),
        ({"sensors": {"supply": {"value_type": "int32"}}}, "value_type"),
        ({"sensors": {"supply": {"scale": 0.5}}}, "scale"),
        ({"sensors": {"supply": {"no_reading": "0x1234"}}}, "no_reading"),
        ({"sensors": {"supply": {"source": "modbus_tcp", "host": ""}}}, "host"),
    ])
    def test_invalid_values_are_rejected(self, device: DeviceClient, saved_perf, body, field):
        before = _saved_settings(_get_config(device))
        r = _put_config(device, body)
        assert r.status_code == 400, r.text
        assert r.json() == {"error": "Invalid value", "field": field}
        assert _saved_settings(_get_config(device)) == before, "a rejected update changed settings"

    def test_empty_update_is_rejected(self, device: DeviceClient, saved_perf):
        r = _put_config(device, {"unrelated": 1})
        assert r.status_code == 400
        assert "error" in r.json()

    def test_partial_update_keeps_other_settings(self, device: DeviceClient, saved_perf):
        before = _get_config(device)
        r = _put_config(device, {"flow_lpm": 42.5})
        assert r.status_code == 200, r.text
        after = r.json()
        assert after["flow_lpm"] == 42.5
        assert after["fluid"] == before["fluid"]
        assert after["glycol_pct"] == before["glycol_pct"]
        assert after["sensors"] == before["sensors"]
        assert _get_config(device)["flow_lpm"] == 42.5

    def test_fluid_and_glycol_round_trip(self, device: DeviceClient, saved_perf):
        r = _put_config(device, {"fluid": "propylene_glycol", "glycol_pct": 35})
        assert r.status_code == 200, r.text
        cfg = _get_config(device)
        assert (cfg["fluid"], cfg["glycol_pct"]) == ("propylene_glycol", 35)

        r = _put_config(device, {"fluid": "water"})
        assert r.status_code == 200, r.text
        assert _get_config(device)["fluid"] == "water"


# ---------------------------------------------------------------------------
# One-shot sensor test against the fake Thermux
# ---------------------------------------------------------------------------

class TestSensorTest:

    def test_thermux_channel(self, device: DeviceClient, fake):
        r = _test_read(device, _modbus(fake, SUPPLY_REG))
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] is True, body
        assert body["error"] == "none"
        assert body["celsius"] == pytest.approx(SUPPLY_C, abs=0.01)
        assert body["thermux"] == {"channel": 4, "status": "ok", "age_s": 7, "rom_id": SUPPLY_ROM}

    def test_float_holding_register(self, device: DeviceClient, fake):
        r = _test_read(device, _modbus(fake, FLOAT_REG, register_type="holding",
                                       value_type="float32", scale=1, no_reading="none"))
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] is True, body
        assert body["celsius"] == pytest.approx(FLOAT_C, abs=0.01)
        assert body["thermux"] is None

    def test_input_falls_back_to_holding(self, device: DeviceClient, fake):
        body = _test_read(device, _modbus(fake, FLOAT_REG, value_type="float32", scale=1,
                                          no_reading="none")).json()
        assert body["ok"] is True, body
        assert body["register_type"] == "holding"
        assert body["celsius"] == pytest.approx(FLOAT_C, abs=0.01)

    def test_holding_falls_back_to_input(self, device: DeviceClient, fake):
        body = _test_read(device, _modbus(fake, SUPPLY_REG, register_type="holding")).json()
        assert body["ok"] is True, body
        assert body["register_type"] == "input"
        assert body["thermux"]["channel"] == 4

    def test_register_type_kept_when_it_works(self, device: DeviceClient, fake):
        assert _test_read(device, _modbus(fake, SUPPLY_REG)).json()["register_type"] == "input"

    def test_channel_without_reading(self, device: DeviceClient, fake):
        body = _test_read(device, _modbus(fake, READ_ERROR_REG)).json()
        assert body["ok"] is False
        assert body["error"] == "no_reading"
        assert body["celsius"] is None
        assert body["thermux"]["status"] == "read_error"

    def test_modbus_exception_is_reported(self, device: DeviceClient, fake):
        body = _test_read(device, _modbus(fake, UNMAPPED_INPUT_REG)).json()
        assert body["ok"] is False
        assert body["error"] == "exception"
        assert body["exception"] == 2

    def test_nothing_listening(self, device: DeviceClient, fake):
        host, _ = fake
        body = _test_read(device, _modbus((host, _free_port()), SUPPLY_REG)).json()
        assert body["ok"] is False
        assert body["error"] in ("connect", "timeout")

    def test_invalid_sensor_is_rejected(self, device: DeviceClient, fake):
        r = _test_read(device, _modbus(fake, SUPPLY_REG, value_type="int32"))
        assert r.status_code == 400
        assert r.json()["field"] == "value_type"

    def test_does_not_save(self, device: DeviceClient, fake):
        before = _saved_settings(_get_config(device))
        assert _test_read(device, _modbus(fake, SUPPLY_REG)).json()["ok"] is True
        assert _saved_settings(_get_config(device)) == before


# ---------------------------------------------------------------------------
# Background polling
# ---------------------------------------------------------------------------

def _configure_both(device: DeviceClient, fake):
    r = _put_config(device, {"sensors": {
        "supply": _modbus(fake, SUPPLY_REG),
        "return": _modbus(fake, RETURN_REG),
    }})
    assert r.status_code == 200, r.text
    return r.json()


def _wait_readings(device: DeviceClient, timeout: float = POLL_TIMEOUT) -> dict:
    def both_read():
        s = _get_config(device)["status"]["sensors"]
        return all(v["error"] == "none" and v["celsius"] is not None for v in s.values())
    device.wait_until("both external sensors read", both_read, timeout=timeout, poll=1.0)
    return _get_config(device)


# Clean readings for kRecoverMs (60 s), plus up to kSettleMs (180 s) if demo
# mode starts the compressor or changes mode meanwhile (main/perf_source.h).
SWITCH_TIMEOUT = 300.0


def _wait_source(device: DeviceClient, source: str, desc: str) -> dict:
    device.wait_until(desc, lambda: _get_config(device)["status"]["source"] == source,
                      timeout=SWITCH_TIMEOUT, poll=2.0)
    return _get_config(device)["status"]


class TestPolling:

    def test_configured_sensors_are_read(self, device: DeviceClient, fake):
        cfg = _configure_both(device, fake)
        # Editing a sensor forgets its learned Thermux sensor ID.
        assert cfg["sensors"]["supply"]["rom_id"] is None
        assert cfg["status"]["sensors"]["supply"]["configured"] is True

        cfg = _wait_readings(device)
        s = cfg["status"]["sensors"]
        assert s["supply"]["celsius"] == pytest.approx(SUPPLY_C, abs=0.01)
        assert s["return"]["celsius"] == pytest.approx(RETURN_C, abs=0.01)
        assert s["supply"]["age_s"] < POLL_TIMEOUT
        # External readings have to prove themselves before the estimate uses them.
        st = cfg["status"]
        assert st["pending"] or st["source"] == "external", st

    def test_thermux_sensor_id_is_learned(self, device: DeviceClient, fake):
        _configure_both(device, fake)
        device.wait_until(
            "supply sensor ID learned",
            lambda: _get_config(device)["sensors"]["supply"]["rom_id"] == SUPPLY_ROM,
            timeout=POLL_TIMEOUT, poll=1.0)

    def test_losing_the_sensor_falls_back_and_recovers(self, device: DeviceClient, thermux, fake):
        """External -> sensor drops -> heat pump -> sensor back -> external."""
        _configure_both(device, fake)
        _wait_source(device, "external", "estimate to switch to the external sensors")
        thermux.stop()
        try:
            device.wait_until(
                "sensors report the lost connection",
                lambda: all(v["error"] in ("connect", "timeout") for v in
                            _get_config(device)["status"]["sensors"].values()),
                timeout=POLL_TIMEOUT, poll=1.0)
            st = _get_config(device)["status"]
            # The last reading stays visible (with its age), but the estimate
            # must not be using it.
            assert st["source"] == "heat_pump", st
            assert st["fallback"] is True, st
        finally:
            thermux.start()
        _wait_readings(device)
        st = _wait_source(device, "external", "estimate to switch back to the external sensors")
        assert st["fallback"] is False, st
        # Demo mode can start a settle hold, which blanks the estimate.
        if not st["settling"]:
            assert st["cop"] is not None, st


# ---------------------------------------------------------------------------
# Device screen
# ---------------------------------------------------------------------------

class TestScreen:

    def test_screen_shows_settings(self, device: DeviceClient, saved_perf):
        assert _put_config(device, {"flow_lpm": 37.5, "fluid": "propylene_glycol",
                                    "glycol_pct": 30}).status_code == 200
        _open_perf_screen(device)
        for tag in ("perf_source", "perf_output", "perf_cop_value", "perf_flow_value",
                    "perf_fluid", "perf_glycol", "perf_sensor_supply", "perf_sensor_return"):
            assert device.wait_for_widget(tag=tag, timeout=5.0), f"{tag} missing"
        assert device.find_widget(tag="perf_flow_value").text == "37.5 L/min"
        assert "30" in device.find_widget(tag="perf_glycol_value").text

    def test_flow_buttons_save(self, device: DeviceClient, saved_perf):
        assert _put_config(device, {"flow_lpm": 40}).status_code == 200
        _open_perf_screen(device)
        device.click(tag="perf_flow_plus")
        device.wait_until("flow label updated",
                          lambda: device.find_widget(tag="perf_flow_value").text == "41 L/min",
                          timeout=3.0)
        device.wait_until("flow saved",
                          lambda: _get_config(device)["flow_lpm"] == 41, timeout=5.0, poll=0.5)

    def test_menu_row_marks_external_sensors(self, device: DeviceClient, fake):
        _configure_both(device, fake)
        device.click(tag="settings")
        assert device.wait_for_screen("settings", timeout=5.0)
        assert device.wait_for_widget(tag="perf_row_value", timeout=5.0), \
            "settings row does not show that external sensors are in use"

    def test_sensor_rows_show_external_readings(self, device: DeviceClient, fake):
        _configure_both(device, fake)
        _wait_readings(device)
        _open_perf_screen(device)
        want = _display_temp(device, SUPPLY_C)
        device.wait_until(
            "supply row shows the external reading",
            lambda: abs(_number(device.find_widget(tag="perf_sensor_supply_value").text)
                        - want) < 0.02,
            timeout=5.0)

    def test_editor_test_button(self, device: DeviceClient, fake):
        _configure_both(device, fake)
        before = _saved_settings(_get_config(device))
        _open_perf_screen(device)
        device.click(tag="perf_sensor_supply")
        assert device.wait_for_widget(tag="perf_editor", timeout=5.0)
        assert device.wait_for_widget(tag="perf_test", timeout=5.0)

        device.click(tag="perf_test")
        assert device.wait_for_widget(tag="perf_test_ok", timeout=12.0), \
            "test result did not report success"
        msg = device.find_widget(tag="perf_test_message").text
        assert "4" in msg and SUPPLY_ROM in msg, f"Thermux channel/ID missing from {msg!r}"

        device.click(tag="perf_editor_cancel")
        device.wait_until("editor closed",
                          lambda: not device.has_widget(tag="perf_editor"), timeout=3.0)
        assert _saved_settings(_get_config(device)) == before, "cancel saved the sensor"

    def test_editor_test_picks_register_type(self, device: DeviceClient, fake):
        r = _put_config(device, {"sensors": {
            "supply": _modbus(fake, SUPPLY_REG, register_type="holding"),
            "return": {"source": "heat_pump"}}})
        assert r.status_code == 200, r.text
        _open_perf_screen(device)
        device.click(tag="perf_sensor_supply")
        assert device.wait_for_widget(tag="perf_test", timeout=5.0)

        device.click(tag="perf_test")
        assert device.wait_for_widget(tag="perf_test_ok", timeout=12.0), \
            "test result did not report success"
        msg = device.find_widget(tag="perf_test_message").text
        assert "Input" in msg, f"register type change not mentioned in {msg!r}"

        device.click(tag="perf_editor_save")
        device.wait_until("editor closed",
                          lambda: not device.has_widget(tag="perf_editor"), timeout=5.0)
        assert _get_config(device)["sensors"]["supply"]["register_type"] == "input"

    def test_editor_switches_back_to_heat_pump(self, device: DeviceClient, fake):
        _configure_both(device, fake)
        _open_perf_screen(device)
        device.click(tag="perf_sensor_return")
        assert device.wait_for_widget(tag="perf_editor", timeout=5.0)
        device.click(tag="perf_src_heat_pump")
        device.wait_until("Modbus fields hidden",
                          lambda: not device.has_widget(tag="perf_host"), timeout=3.0)
        device.click(tag="perf_editor_save")
        device.wait_until("editor closed",
                          lambda: not device.has_widget(tag="perf_editor"), timeout=5.0)
        cfg = _get_config(device)
        assert cfg["sensors"]["return"]["source"] == "heat_pump"
        assert cfg["sensors"]["supply"]["source"] == "modbus_tcp"
