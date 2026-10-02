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
FAKE_BACNET_SERVER = Path(__file__).with_name("fake_bacnet_server.py")

# What the fake Thermux serves (see fake_modbus_server.py).
SUPPLY_REG, SUPPLY_C, SUPPLY_ROM = 104, 22.69, "28FF9A2B0F1C0412"
RETURN_REG, RETURN_C = 103, 21.25
FLOAT_REG, FLOAT_C = 500, 45.5
READ_ERROR_REG = 106      # channel 6 reports a read error
UNMAPPED_INPUT_REG = 50   # not served: Modbus exception 2

# The worker polls every 15 s and each step is bounded to 1.5 s; allow two
# poll intervals plus HTTP/device scheduling margin.
POLL_TIMEOUT = 45.0

PERSISTED_SENSOR_FIELDS = (
    "source", "host", "port", "unit_id", "register", "register_type",
    "object_type", "object_instance", "value_type", "scale", "no_reading",
    "device_instance", "device_name", "object_name", "rom_id",
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


def _bacnet(fake, instance: int = 3, **overrides) -> dict:
    host, port = fake
    sensor = {
        "source": "bacnet_ip", "host": host, "port": port,
        "object_type": "analog_input", "object_instance": instance,
    }
    sensor.update(overrides)
    return sensor


def _heat_pump_sensors() -> dict:
    return {"supply": {"source": "heat_pump"}, "return": {"source": "heat_pump"}}


def _heat_pump_sensors_with_stale_network() -> dict:
    sensors = _heat_pump_sensors()
    sensors["supply"] = {
        "source": "heat_pump",
        "host": "192.168.9.3",
        "port": 502,
        "object_instance": 7,
        "device_name": "Old device",
        "object_name": "Old sensor",
    }
    return sensors


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


class FakeBacnet:
    """fake_bacnet_server.py in a subprocess that tests can stop and restart."""

    def __init__(self, advertise_host: str, huge_object_count: int = 0,
                 rpm_unsupported: bool = False, wrong_device_instance: bool = False,
                 rom_change: bool = False):
        self.host = advertise_host
        self.port = _free_port()
        self.proc = None
        self.huge_object_count = huge_object_count
        self.rpm_unsupported = rpm_unsupported
        self.wrong_device_instance = wrong_device_instance
        self.rom_change = rom_change

    @property
    def addr(self):
        return self.host, self.port

    def restart(self, **kwargs):
        for key, value in kwargs.items():
            setattr(self, key, value)
        self.stop()
        self.start()

    def start(self):
        args = [sys.executable, "-u", str(FAKE_BACNET_SERVER), "--port", str(self.port)]
        if self.huge_object_count:
            args += ["--huge-object-count", str(self.huge_object_count)]
        if self.rpm_unsupported:
            args += ["--rpm-unsupported"]
        if self.wrong_device_instance:
            args += ["--wrong-device-instance"]
        if self.rom_change:
            args += ["--rom-change"]
        self.proc = subprocess.Popen(
            args,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        line = self.proc.stdout.readline()
        if "fake BACnet server" not in line:
            self.stop()
            pytest.fail(f"fake BACnet server did not start on port {self.port}: {line!r}")

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


@pytest.fixture(scope="module")
def bacnet_server(device: DeviceClient, saved_perf):
    host = os.environ.get("ARCTIC_FAKE_BACNET_HOST") or os.environ.get("ARCTIC_FAKE_MODBUS_HOST") \
        or _local_ip_towards(urlparse(device.base_url).hostname)
    server = FakeBacnet(host)
    server.start()
    yield server
    _put_config(device, {"sensors": _heat_pump_sensors()})
    server.stop()


@pytest.fixture
def bacnet_fake(bacnet_server):
    if bacnet_server.proc is None:
        bacnet_server.start()
    return bacnet_server.addr


@pytest.fixture
def huge_bacnet_fake(device: DeviceClient, saved_perf):
    host = os.environ.get("ARCTIC_FAKE_BACNET_HOST") or os.environ.get("ARCTIC_FAKE_MODBUS_HOST") \
        or _local_ip_towards(urlparse(device.base_url).hostname)
    server = FakeBacnet(host, huge_object_count=1_000_000)
    server.start()
    yield server.addr
    server.stop()


@pytest.fixture
def rpm_unsupported_bacnet_fake(device: DeviceClient, saved_perf):
    host = os.environ.get("ARCTIC_FAKE_BACNET_HOST") or os.environ.get("ARCTIC_FAKE_MODBUS_HOST") \
        or _local_ip_towards(urlparse(device.base_url).hostname)
    server = FakeBacnet(host, rpm_unsupported=True)
    server.start()
    yield server.addr
    server.stop()


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


def _screen_text_contains(device: DeviceClient, text: str) -> bool:
    for w in device.widgets:
        if text in (w.text or "") or text in (w.text_en or ""):
            return True
    return False


def _set_perf_entry(device: DeviceClient, field_tag: str, value: str) -> None:
    device.click(tag=field_tag)
    assert device.wait_for_widget(tag="perf_entry_input", timeout=5.0)
    device.type_text("perf_entry_input", value)
    device.click(tag="perf_entry_save")
    device.wait_until("performance editor text entry closed",
                      lambda: not device.has_widget(tag="perf_entry"), timeout=5.0)


def _pick_perf_source(device: DeviceClient, source_tag: str) -> None:
    device.click(tag="perf_source_row")
    assert device.wait_for_widget(tag=source_tag, timeout=5.0)
    device.click(tag=source_tag)


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
        ({"sensors": {"supply": {"object_type": "binary_input"}}}, "object_type"),
        ({"sensors": {"supply": {"object_instance": 4194303}}}, "object_instance"),
        ({"sensors": {"supply": {"scale": 0.5}}}, "scale"),
        ({"sensors": {"supply": {"no_reading": "0x1234"}}}, "no_reading"),
        ({"sensors": {"supply": {"source": "modbus_tcp", "host": ""}}}, "host"),
        ({"sensors": {"supply": {"source": "bacnet_ip", "host": "thermux.local",
                                  "object_instance": None}}}, "object_instance"),
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

    def test_source_change_resets_protocol_port(self, device: DeviceClient, saved_perf):
        r = _put_config(device, {"sensors": {"supply": {
            "source": "modbus_tcp", "host": "thermux.local", "port": 1502,
            "unit_id": 1, "register": 103, "register_type": "input",
            "value_type": "int16", "scale": 0.01, "no_reading": "0x8000",
        }}})
        assert r.status_code == 200, r.text
        r = _put_config(device, {"sensors": {"supply": {
            "source": "bacnet_ip", "host": "thermux.local", "object_instance": 3,
        }}})
        assert r.status_code == 200, r.text
        assert r.json()["sensors"]["supply"]["port"] == 47808
        r = _put_config(device, {"sensors": {"supply": {
            "source": "modbus_tcp", "host": "thermux.local", "register": 103,
        }}})
        assert r.status_code == 200, r.text
        assert r.json()["sensors"]["supply"]["port"] == 502

    def test_bacnet_save_requires_object_instance(self, device: DeviceClient, bacnet_fake):
        host, port = bacnet_fake
        r = _put_config(device, {"sensors": {"supply": {
            "source": "bacnet_ip", "host": host, "port": port, "object_instance": None,
        }}})
        assert r.status_code == 400, r.text
        assert r.json() == {"error": "Invalid value", "field": "object_instance"}


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

    def test_bacnet_round_trip_and_test(self, device: DeviceClient, bacnet_fake):
        sensor = _bacnet(bacnet_fake, 3)
        r = _put_config(device, {"sensors": {"supply": sensor}})
        assert r.status_code == 200, r.text
        saved = _get_config(device)["sensors"]["supply"]
        assert saved["source"] == "bacnet_ip"
        assert saved["object_type"] == "analog_input"
        assert saved["object_instance"] == 3

        r = _test_read(device, sensor)
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] is True, body
        assert body["error"] == "none"
        assert body["celsius"] == pytest.approx(21.4, abs=0.05)
        assert body["object_name"] == "Supply tank"
        assert body["units"] == 62
        assert body["reliability"] == 0
        assert body["device_instance"] == 1234
        assert body["rom_id"] == "28FF6491631603A2"

    def test_bacnet_device_name_round_trips(self, device: DeviceClient, bacnet_fake):
        sensor = _bacnet(bacnet_fake, 3, device_name="Thermux Test")
        r = _put_config(device, {"sensors": {"supply": sensor, "return": {"source": "heat_pump"}}})
        assert r.status_code == 200, r.text
        saved = _get_config(device)["sensors"]["supply"]
        assert saved["device_name"] == "Thermux Test"

    def test_bacnet_rpm_unsupported_falls_back_to_read_property(self, device: DeviceClient,
                                                                 rpm_unsupported_bacnet_fake):
        body = _test_read(device, _bacnet(rpm_unsupported_bacnet_fake, 3)).json()
        assert body["ok"] is True, body
        assert body["celsius"] == pytest.approx(21.4, abs=0.05)
        assert body["object_name"] == "Supply tank"

    def test_bacnet_wrong_device_instance_errors(self, device: DeviceClient, bacnet_fake):
        body = _test_read(device, _bacnet(bacnet_fake, 3, device_instance=9999)).json()
        assert body["ok"] is False, body
        assert body["error"] == "wrong_device"

    def test_bacnet_browse(self, device: DeviceClient, bacnet_fake):
        host, port = bacnet_fake
        r = device.session.post(_url(device, "/api/performance/bacnet/browse"),
                                json={"host": host, "port": port}, timeout=15)
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] is True, body
        assert body["device_name"] == "Thermux Test"
        names = {s["object_name"]: s for s in body["sensors"]}
        assert set(names) >= {"Supply tank", "Return tank", "Faulted sensor"}
        assert names["Supply tank"]["object_instance"] == 3
        assert names["Return tank"]["celsius"] == pytest.approx(20.0, abs=0.05)
        assert names["Faulted sensor"]["available"] is False
        assert names["Faulted sensor"]["celsius"] is None
        assert names["Faulted sensor"]["reliability"] == 1
        assert body["total_objects"] == 4
        assert body["truncated"] is False

    def test_bacnet_discover_finds_fake_device(self, device: DeviceClient, bacnet_fake):
        _host, port = bacnet_fake
        r = device.session.post(_url(device, "/api/performance/bacnet/discover"),
                                json={}, timeout=15)
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] is True, body
        matches = [
            d for d in body["devices"]
            if d.get("device_instance") == 1234 and int(d.get("port", 0)) == port
        ]
        assert matches, body
        assert matches[0]["device_name"] == "Thermux Test"

    def test_bacnet_browse_huge_object_list_is_bounded(self, device: DeviceClient,
                                                       huge_bacnet_fake):
        status_before = device.session.get(_url(device, "/api/info"),
                                           timeout=device.timeout).json()
        host, port = huge_bacnet_fake
        r = device.session.post(_url(device, "/api/performance/bacnet/browse"),
                                json={"host": host, "port": port}, timeout=15)
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] is True, body
        assert body["total_objects"] == 1_000_000
        assert body["truncated"] is True
        assert body["scanned"] <= 512
        assert len(body["sensors"]) <= 64
        assert device.session.get(_url(device, "/api/health"),
                                  timeout=device.timeout).status_code == 200
        status_after = device.session.get(_url(device, "/api/info"),
                                          timeout=device.timeout).json()
        assert status_after["free_heap"] > 100_000
        assert status_after["free_heap"] > status_before["free_heap"] - 80_000

    def test_bacnet_units_fault_and_bad_object(self, device: DeviceClient, bacnet_fake):
        good_f = _test_read(device, _bacnet(bacnet_fake, 4)).json()
        assert good_f["ok"] is True, good_f
        assert good_f["celsius"] == pytest.approx(20.0, abs=0.05)

        fault = _test_read(device, _bacnet(bacnet_fake, 5)).json()
        assert fault["ok"] is False, fault
        assert fault["error"] == "no_reading"

        bad = _test_read(device, _bacnet(bacnet_fake, 99)).json()
        assert bad["ok"] is False, bad
        assert bad["error"] == "rejected"

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


def _configure_both_bacnet(device: DeviceClient, fake):
    r = _put_config(device, {"sensors": {
        "supply": _bacnet(fake, 3),
        "return": _bacnet(fake, 4),
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


def _wait_fallback(device: DeviceClient) -> dict:
    # Wait on the fallback flag itself: before the sensors' first minute of
    # good readings the source is already "heat_pump" (pending), so waiting on
    # the source alone can read a status from before the error was seen.
    device.wait_until("estimate to fall back to the heat pump",
                      lambda: (lambda st: st["source"] == "heat_pump" and st["fallback"])(
                          _get_config(device)["status"]),
                      timeout=30.0, poll=1.0)
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

    def test_bacnet_configured_sensors_are_read(self, device: DeviceClient, bacnet_fake):
        cfg = _configure_both_bacnet(device, bacnet_fake)
        assert cfg["sensors"]["supply"]["rom_id"] is None
        assert cfg["status"]["sensors"]["supply"]["configured"] is True

        cfg = _wait_readings(device)
        s = cfg["status"]["sensors"]
        assert s["supply"]["celsius"] == pytest.approx(21.4, abs=0.05)
        assert s["return"]["celsius"] == pytest.approx(20.0, abs=0.05)
        st = _wait_source(device, "external", "BACnet estimate to switch to external sensors")
        assert st["fallback"] is False, st

    def test_bacnet_browse_does_not_starve_configured_polling(self, device: DeviceClient,
                                                              bacnet_fake):
        host, port = bacnet_fake
        _configure_both_bacnet(device, bacnet_fake)

        for _ in range(2):
            r = device.session.post(_url(device, "/api/performance/bacnet/browse"),
                                    json={"host": host, "port": port}, timeout=12)
            assert r.status_code == 200, r.text
            assert r.json()["ok"] is True

        cfg = _wait_readings(device)
        s = cfg["status"]["sensors"]
        assert s["supply"]["error"] == "none", s["supply"]
        assert s["return"]["error"] == "none", s["return"]
        assert s["supply"]["celsius"] == pytest.approx(21.4, abs=0.05)
        assert s["return"]["celsius"] == pytest.approx(20.0, abs=0.05)

    def test_thermux_sensor_id_is_learned(self, device: DeviceClient, fake):
        _configure_both(device, fake)
        device.wait_until(
            "supply sensor ID learned",
            lambda: _get_config(device)["sensors"]["supply"]["rom_id"] == SUPPLY_ROM,
            timeout=POLL_TIMEOUT, poll=1.0)

    def test_bacnet_polling_does_not_persist_ram_learned_rom(self, device: DeviceClient, bacnet_fake):
        r = _put_config(device, {"sensors": {
            "supply": _bacnet(bacnet_fake, 3),
            "return": {"source": "heat_pump"},
        }})
        assert r.status_code == 200, r.text
        device.wait_until(
            "BACnet supply sensor read without persisted identity",
            lambda: _get_config(device)["status"]["sensors"]["supply"]["error"] == "none",
            timeout=POLL_TIMEOUT, poll=1.0)
        saved = _get_config(device)["sensors"]["supply"]
        assert saved["source"] == "bacnet_ip"
        assert saved["rom_id"] is None
        assert saved["device_instance"] is None

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
            # The last reading stays visible (with its age), but the estimate
            # must stop using it. The source is re-chosen on each heat pump
            # poll, so allow a few cycles after the sensor errors show.
            st = _wait_fallback(device)
            assert st["fallback"] is True, st
        finally:
            thermux.start()
        _wait_readings(device)
        st = _wait_source(device, "external", "estimate to switch back to the external sensors")
        assert st["fallback"] is False, st
        # Demo mode can start a settle hold, which blanks the estimate.
        if not st["settling"]:
            assert st["cop"] is not None, st

    def test_bacnet_dropout_falls_back_and_recovers(self, device: DeviceClient, bacnet_server,
                                                    bacnet_fake):
        _configure_both_bacnet(device, bacnet_fake)
        _wait_source(device, "external", "estimate to switch to BACnet sensors")
        bacnet_server.stop()
        try:
            device.wait_until(
                "BACnet sensors report the lost connection",
                lambda: all(v["error"] in ("connect", "timeout") for v in
                            _get_config(device)["status"]["sensors"].values()),
                timeout=POLL_TIMEOUT, poll=1.0)
            st = _wait_fallback(device)
            assert st["fallback"] is True, st
        finally:
            bacnet_server.start()
        _wait_readings(device)
        st = _wait_source(device, "external", "estimate to switch back to BACnet sensors")
        assert st["fallback"] is False, st

    def test_bacnet_sensor_changed_falls_back(self, device: DeviceClient, bacnet_server,
                                              bacnet_fake):
        identity = _test_read(device, _bacnet(bacnet_fake, 3)).json()
        assert identity["ok"] is True, identity
        supply = _bacnet(
            bacnet_fake, 3,
            device_instance=identity["device_instance"],
            object_name=identity["object_name"],
            rom_id=identity["rom_id"],
        )
        r = _put_config(device, {"sensors": {
            "supply": supply,
            "return": _bacnet(bacnet_fake, 4),
        }})
        assert r.status_code == 200, r.text
        _wait_readings(device)

        bacnet_server.restart(rom_change=True)
        try:
            device.wait_until(
                "BACnet sensor-changed error reported",
                lambda: _get_config(device)["status"]["sensors"]["supply"]["error"] ==
                "sensor_changed",
                timeout=POLL_TIMEOUT, poll=1.0)
            st = _wait_fallback(device)
            assert st["fallback"] is True, st
        finally:
            bacnet_server.restart(rom_change=False)


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

    def test_menu_row_shows_external_sensor_sources(
        self, device: DeviceClient, fake, bacnet_fake
    ):
        original = _saved_settings(_get_config(device))

        def assert_perf_row_value(expected: Optional[str]) -> None:
            device.click(tag="settings")
            assert device.wait_for_screen("settings", timeout=5.0)
            assert device.wait_for_widget(tag="settings_perf", timeout=5.0)
            if expected is None:
                assert device.find_widget(tag="perf_row_value") is None
                device.click(tag="settings_close")
                assert device.wait_for_widget(tag="settings", timeout=5.0)
                return
            assert device.wait_for_widget(tag="perf_row_value", timeout=5.0)
            value = device.find_widget(tag="perf_row_value")
            assert value is not None
            assert value.text == expected
            device.click(tag="settings_close")
            assert device.wait_for_widget(tag="settings", timeout=5.0)

        try:
            r = _put_config(device, {"sensors": {
                "supply": _bacnet(bacnet_fake, 3, device_name="Thermux Test"),
                "return": _bacnet(bacnet_fake, 4, device_name="Thermux Test"),
            }})
            assert r.status_code == 200, r.text
            assert_perf_row_value("BACnet")

            r = _put_config(device, {"sensors": {
                "supply": _modbus(fake, SUPPLY_REG),
                "return": _modbus(fake, RETURN_REG),
            }})
            assert r.status_code == 200, r.text
            assert_perf_row_value("Modbus")

            r = _put_config(device, {"sensors": {
                "supply": _modbus(fake, SUPPLY_REG),
                "return": _bacnet(bacnet_fake, 4, device_name="Thermux Test"),
            }})
            assert r.status_code == 200, r.text
            assert_perf_row_value("Modbus + BACnet")

            r = _put_config(device, {"sensors": _heat_pump_sensors()})
            assert r.status_code == 200, r.text
            assert_perf_row_value(None)
        finally:
            r = _put_config(device, original)
            assert r.status_code == 200, f"could not restore performance settings: {r.text}"

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

    def test_editor_saved_bacnet_opens_summary_and_changes(self, device: DeviceClient, bacnet_fake):
        r = _put_config(device, {"sensors": {
            "supply": _bacnet(bacnet_fake, 3, device_instance=1234, device_name="Thermux Test",
                                object_name="Supply tank"),
            "return": {"source": "heat_pump"},
        }})
        assert r.status_code == 200, r.text
        _open_perf_screen(device)
        device.click(tag="perf_sensor_supply")
        assert device.wait_for_widget(tag="perf_bacnet_device_summary", timeout=5.0)
        assert device.wait_for_widget(tag="perf_bacnet_sensor_summary", timeout=5.0)
        assert not device.has_widget(tag="perf_host")
        assert not device.has_widget(tag="perf_test")
        device.click(tag="perf_bacnet_sensor_change")
        assert device.wait_until("sensor browse starts",
                                 lambda: _screen_text_contains(device, "Finding sensors") or
                                 _screen_text_contains(device, "Supply tank"),
                                 timeout=20.0, poll=0.5)
        device.click(tag="perf_editor_cancel")

    def test_editor_bacnet_browse_pick_saves_identity(self, device: DeviceClient, bacnet_fake):
        r = _put_config(device, {"sensors": _heat_pump_sensors()})
        assert r.status_code == 200, r.text
        host, port = bacnet_fake
        _open_perf_screen(device)
        device.click(tag="perf_sensor_supply")
        assert device.wait_for_widget(tag="perf_editor", timeout=5.0)

        _pick_perf_source(device, "perf_src_bacnet")
        # Picking BACnet starts a device search; skip it and type the address.
        assert device.wait_for_widget(tag="perf_bacnet_manual", timeout=20.0)
        device.click(tag="perf_bacnet_manual")
        assert device.wait_for_widget(tag="perf_bacnet_browse", timeout=5.0)
        _set_perf_entry(device, "perf_host", host)
        _set_perf_entry(device, "perf_port", str(port))

        device.click(tag="perf_bacnet_browse")
        assert device.wait_until("BACnet browse list shows Supply tank",
                                 lambda: _screen_text_contains(device, "Supply tank"),
                                 timeout=30.0, poll=0.5)
        device.click(label_contains="Supply tank")
        # The pick already read the sensor: its reading replaces Test sensor.
        assert device.wait_for_widget(tag="perf_bacnet_sensor_summary", timeout=5.0)
        assert device.wait_until("sensor summary shows the picked reading",
                                 lambda: _screen_text_contains(device, "Supply tank \u00b7"),
                                 timeout=5.0)
        assert not device.has_widget(tag="perf_test")
        device.click(tag="perf_editor_save")
        device.wait_until("editor closed",
                          lambda: not device.has_widget(tag="perf_editor"), timeout=5.0)

        saved = _get_config(device)["sensors"]["supply"]
        assert saved["source"] == "bacnet_ip"
        assert saved["device_name"] in (None, "Thermux Test")
        assert saved["object_instance"] == 3
        assert saved["device_instance"] == 1234
        assert saved["rom_id"] == "28FF6491631603A2"
        device.wait_until("main BACnet card row shows the picked object name",
                          lambda: _screen_text_contains(device, "Supply tank"),
                          timeout=5.0)

    def test_editor_bacnet_discover_pick_device_then_sensor(self, device: DeviceClient, bacnet_fake):
        r = _put_config(device, {"sensors": _heat_pump_sensors()})
        assert r.status_code == 200, r.text
        host, port = bacnet_fake
        _open_perf_screen(device)
        device.click(tag="perf_sensor_supply")
        assert device.wait_for_widget(tag="perf_editor", timeout=5.0)

        _pick_perf_source(device, "perf_src_bacnet")
        assert device.wait_until("BACnet discovery finds fake",
                                 lambda: _screen_text_contains(device, "Thermux Test"),
                                 timeout=20.0, poll=0.5)
        device.click(label_contains="Thermux Test")
        assert device.wait_until("BACnet sensor list shows Supply tank",
                                 lambda: _screen_text_contains(device, "Supply tank"),
                                 timeout=20.0, poll=0.5)
        device.click(label_contains="Supply tank")
        device.click(tag="perf_editor_save")
        device.wait_until("editor closed after BACnet discovery save",
                          lambda: not device.has_widget(tag="perf_editor"), timeout=5.0)

        saved = _get_config(device)["sensors"]["supply"]
        assert saved["source"] == "bacnet_ip"
        assert saved["device_name"] == "Thermux Test"
        assert saved["host"] == host
        assert saved["port"] == port
        assert saved["object_instance"] == 3
        assert saved["device_instance"] == 1234

    def test_editor_bacnet_sensor_step_keeps_device_summary_and_hides_test(
        self, device: DeviceClient, bacnet_fake
    ):
        original = _saved_settings(_get_config(device))
        try:
            r = _put_config(device, {"sensors": _heat_pump_sensors_with_stale_network()})
            assert r.status_code == 200, r.text
            _open_perf_screen(device)
            device.click(tag="perf_sensor_supply")
            assert device.wait_for_widget(tag="perf_editor", timeout=5.0)

            _pick_perf_source(device, "perf_src_bacnet")
            assert not device.has_widget(tag="perf_bacnet_device_summary")
            assert not device.has_widget(tag="perf_test")
            if not device.wait_until("BACnet discovery finds a device",
                                     lambda: _screen_text_contains(device, "Thermux Test"),
                                     timeout=20.0, poll=0.5, raise_on_timeout=False):
                pytest.skip("No BACnet device discoverable on the CI network")
            assert not device.has_widget(tag="perf_bacnet_device_summary")
            assert not device.has_widget(tag="perf_test")
            # Refresh sits in the list header, manual entry as a link under the list.
            refresh = device.find_widget(tag="perf_bacnet_search_again")
            first_device = device.find_widget(tag="perf_bacnet_device_0")
            manual = device.find_widget(tag="perf_bacnet_manual")
            assert refresh.y + refresh.h <= first_device.y
            assert refresh.w < first_device.w / 3
            assert manual.y > first_device.y + first_device.h
            device.click(label_contains="Thermux Test")

            assert device.wait_for_widget(tag="perf_bacnet_device_summary", timeout=5.0)
            assert device.wait_until("Sensor heading is shown while browsing",
                                     lambda: _screen_text_contains(device, "Sensor"),
                                     timeout=20.0, poll=0.5)
            assert not device.has_widget(tag="perf_bacnet_sensor_summary")
            assert not device.has_widget(tag="perf_test")
            assert device.wait_until("BACnet sensor list shows Supply tank",
                                     lambda: _screen_text_contains(device, "Supply tank"),
                                     timeout=20.0, poll=0.5)
            first_sensor = device.find_widget(tag="perf_bacnet_sensor")
            manual_object = device.find_widget(tag="perf_bacnet_manual_object")
            assert manual_object.y > first_sensor.y + first_sensor.h
            assert _screen_text_contains(device, "Not listed?")

            device.click(tag="perf_bacnet_manual_object")
            assert device.wait_for_widget(tag="perf_bacnet_device_summary", timeout=5.0)
            assert device.wait_for_widget(tag="perf_bacnet_obj_ai", timeout=5.0)
            assert device.wait_for_widget(tag="perf_register", timeout=5.0)
            summary = device.find_widget(tag="perf_bacnet_device_summary")
            obj_type = device.find_widget(tag="perf_bacnet_obj_ai")
            obj_instance = device.find_widget(tag="perf_register")
            assert summary.y < obj_type.y < obj_instance.y
            assert obj_instance.w >= summary.w - 40
            assert not device.has_widget(tag="perf_bacnet_list")
            assert not device.has_widget(tag="perf_reg_input")
            assert not device.has_widget(tag="perf_value_type")
            assert not device.has_widget(tag="perf_test")

            _set_perf_entry(device, "perf_register", "3")
            assert device.wait_for_widget(tag="perf_test", timeout=5.0)
        finally:
            r = _put_config(device, original)
            assert r.status_code == 200, f"could not restore performance settings: {r.text}"

    def test_editor_bacnet_manual_address_can_search_again(
        self, device: DeviceClient, bacnet_fake
    ):
        original = _saved_settings(_get_config(device))
        try:
            r = _put_config(device, {"sensors": _heat_pump_sensors_with_stale_network()})
            assert r.status_code == 200, r.text
            _open_perf_screen(device)
            device.click(tag="perf_sensor_supply")
            assert device.wait_for_widget(tag="perf_editor", timeout=5.0)

            _pick_perf_source(device, "perf_src_bacnet")
            assert not device.has_widget(tag="perf_bacnet_device_summary")
            if not device.wait_for_widget(tag="perf_bacnet_manual", timeout=20.0,
                                          raise_on_timeout=False):
                pytest.skip("No BACnet discovery/manual control available")
            assert not device.has_widget(tag="perf_bacnet_device_summary")
            device.click(tag="perf_bacnet_manual")
            assert device.wait_for_widget(tag="perf_host", timeout=5.0)
            assert device.wait_for_widget(tag="perf_bacnet_browse", timeout=5.0)
            assert device.wait_for_widget(tag="perf_bacnet_search_again", timeout=5.0)
            assert not device.has_widget(tag="perf_bacnet_device_summary")
            assert not device.has_widget(tag="perf_test")
            # Going back to the search is a link below Find sensors.
            back = device.find_widget(tag="perf_bacnet_search_again")
            find = device.find_widget(tag="perf_bacnet_browse")
            assert back.y > find.y + find.h
            assert _screen_text_contains(device, "Back to search")

            device.click(tag="perf_bacnet_search_again")
            assert device.wait_until("BACnet discovery restarted from manual address",
                                     lambda: _screen_text_contains(device, "Searching") or
                                     _screen_text_contains(device, "Thermux Test"),
                                     timeout=5.0, poll=0.5)
        finally:
            r = _put_config(device, original)
            assert r.status_code == 200, f"could not restore performance settings: {r.text}"

    def test_editor_bacnet_manual_mode(self, device: DeviceClient, bacnet_fake):
        original = _saved_settings(_get_config(device))
        try:
            r = _put_config(device, {"sensors": _heat_pump_sensors()})
            assert r.status_code == 200, r.text
            _open_perf_screen(device)
            device.click(tag="perf_sensor_supply")
            assert device.wait_for_widget(tag="perf_editor", timeout=5.0)
            _pick_perf_source(device, "perf_src_bacnet")
            if not device.wait_for_widget(tag="perf_bacnet_manual", timeout=20.0,
                                          raise_on_timeout=False):
                pytest.skip("No BACnet discovery/manual control available")
            device.click(tag="perf_bacnet_manual")
            assert device.wait_for_widget(tag="perf_host", timeout=5.0)
            device.click(tag="perf_editor_cancel")
        finally:
            r = _put_config(device, original)
            assert r.status_code == 200, f"could not restore performance settings: {r.text}"

    def test_editor_switches_back_to_heat_pump(self, device: DeviceClient, fake):
        _configure_both(device, fake)
        _open_perf_screen(device)
        device.click(tag="perf_sensor_return")
        assert device.wait_for_widget(tag="perf_editor", timeout=5.0)
        _pick_perf_source(device, "perf_src_heat_pump")
        device.wait_until("Modbus fields hidden",
                          lambda: not device.has_widget(tag="perf_host"), timeout=3.0)
        device.click(tag="perf_editor_save")
        device.wait_until("editor closed",
                          lambda: not device.has_widget(tag="perf_editor"), timeout=5.0)
        cfg = _get_config(device)
        assert cfg["sensors"]["return"]["source"] == "heat_pump"
        assert cfg["sensors"]["supply"]["source"] == "modbus_tcp"
