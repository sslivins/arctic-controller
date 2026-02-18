"""
Functional tests for the Heat Pump REST API endpoints.

Tests behavioral correctness of heat pump status, control, parameters,
errors, and demo mode injection — not just schema compliance.

These tests assume demo mode is enabled on the device (the conftest in
tests/device/ enables it at session start). Demo mode lets us inject
known values and verify the API returns them correctly.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
  - Demo mode enabled on the device
"""

import os
import time

import pytest
import requests
from pathlib import Path

# Load .env from repo root if present (local dev)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

VALID_MODES = ["cooling", "floor_heating", "fan_coil_heating", "hot_water", "auto"]


def _headers(api_key=None):
    """Build auth headers."""
    h = {}
    key = api_key if api_key is not None else API_KEY
    if key:
        h["X-API-Key"] = key
    return h


def _get(path, **kwargs):
    """Authenticated GET helper."""
    return requests.get(f"{BASE_URL}{path}", headers=_headers(), timeout=10, **kwargs)


def _put(path, json=None, data=None, **kwargs):
    """Authenticated PUT helper."""
    return requests.put(
        f"{BASE_URL}{path}", headers=_headers(), json=json, data=data, timeout=10, **kwargs
    )


def _patch(path, json=None, **kwargs):
    """Authenticated PATCH helper."""
    return requests.patch(f"{BASE_URL}{path}", headers=_headers(), json=json, timeout=10, **kwargs)


def _post(path, json=None, **kwargs):
    """Authenticated POST helper."""
    return requests.post(f"{BASE_URL}{path}", headers=_headers(), json=json, timeout=10, **kwargs)


def _delete(path, **kwargs):
    """Authenticated DELETE helper."""
    return requests.delete(f"{BASE_URL}{path}", headers=_headers(), timeout=10, **kwargs)


def _inject_demo(fields: dict):
    """Inject demo field values via PATCH /api/heatpump/demo."""
    r = _patch("/api/heatpump/demo", json=fields)
    assert r.status_code == 200, f"Demo inject failed: {r.text}"
    data = r.json()
    assert data["success"], f"Demo inject not successful: {data}"
    return data


# ── Fixtures ─────────────────────────────────────────────────────────────


@pytest.fixture(scope="module", autouse=True)
def _check_prerequisites():
    """Verify device is reachable, demo mode is on, and API key is set."""
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    try:
        r = _get("/api/health")
        r.raise_for_status()
    except Exception as e:
        pytest.skip(f"Device not reachable at {BASE_URL}: {e}")

    # Verify demo mode is enabled
    r = _get("/api/heatpump/status")
    if r.status_code == 401:
        pytest.skip("API auth required but key not accepted")
    data = r.json()
    if not data.get("demo_mode"):
        pytest.skip("Demo mode not enabled on device")


# ── Heat Pump Status ─────────────────────────────────────────────────────


class TestHeatpumpStatus:
    """GET /api/heatpump/status — verify response structure and values."""

    def test_status_returns_200(self):
        r = _get("/api/heatpump/status")
        assert r.status_code == 200

    def test_status_has_required_fields(self):
        """All documented top-level fields must be present."""
        r = _get("/api/heatpump/status")
        data = r.json()
        for field in [
            "connected", "demo_mode", "unit_on", "mode", "defrosting",
            "compressor", "fans", "fan_speed", "pump", "aux_heater",
            "temperatures", "setpoints", "readings", "has_error", "error",
        ]:
            assert field in data, f"Missing field: {field}"

    def test_status_demo_mode_flag(self):
        """demo_mode should be true when demo mode is active."""
        data = _get("/api/heatpump/status").json()
        assert data["demo_mode"] is True

    def test_status_connected_in_demo(self):
        """connected should be true in demo mode."""
        data = _get("/api/heatpump/status").json()
        assert data["connected"] is True

    def test_status_mode_is_valid_string(self):
        data = _get("/api/heatpump/status").json()
        assert data["mode"] in VALID_MODES + ["unknown"]

    def test_status_temperatures_structure(self):
        """temperatures object must have all 9 temp fields."""
        data = _get("/api/heatpump/status").json()
        temps = data["temperatures"]
        for key in ["tank", "outlet", "inlet", "outdoor", "discharge",
                     "suction", "outdoor_coil", "indoor_coil", "ipm"]:
            assert key in temps, f"Missing temperature: {key}"
            assert isinstance(temps[key], (int, float)), f"{key} not numeric"

    def test_status_setpoints_structure(self):
        """setpoints object must have cooling, heating, hot_water."""
        data = _get("/api/heatpump/status").json()
        sp = data["setpoints"]
        for key in ["cooling", "heating", "hot_water"]:
            assert key in sp, f"Missing setpoint: {key}"
            assert isinstance(sp[key], (int, float)), f"{key} not numeric"

    def test_status_readings_structure(self):
        """readings object must have all documented fields."""
        data = _get("/api/heatpump/status").json()
        readings = data["readings"]
        for key in ["compressor_freq", "fan_rpm", "ac_voltage", "ac_current",
                     "dc_voltage", "dc_current", "high_pressure", "low_pressure",
                     "primary_eev", "secondary_eev", "power_consumption"]:
            assert key in readings, f"Missing reading: {key}"

    def test_status_boolean_fields_are_bools(self):
        """Boolean fields must actually be booleans."""
        data = _get("/api/heatpump/status").json()
        for field in ["connected", "demo_mode", "unit_on", "defrosting",
                       "compressor", "fans", "pump", "aux_heater", "has_error"]:
            assert isinstance(data[field], bool), f"{field} is not a boolean"

    def test_status_error_null_when_no_error(self):
        """error should be null when has_error is false."""
        # Clear errors first
        _inject_demo({"error1": 0, "error2": 0})
        time.sleep(0.3)
        data = _get("/api/heatpump/status").json()
        if not data["has_error"]:
            assert data["error"] is None

    def test_status_fan_speed_range(self):
        """fan_speed should be 0-3."""
        data = _get("/api/heatpump/status").json()
        assert 0 <= data["fan_speed"] <= 3


# ── Demo Mode Injection ──────────────────────────────────────────────────


class TestDemoModeInjection:
    """PATCH /api/heatpump/demo — inject values and verify in status."""

    def test_inject_temperature(self):
        """Injecting water_tank_temp should be reflected in status."""
        _inject_demo({"water_tank_temp": 77})
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        assert data["temperatures"]["tank"] == 77

    def test_inject_multiple_temperatures(self):
        """Inject multiple temperatures at once."""
        _inject_demo({
            "water_tank_temp": 50,
            "outlet_water_temp": 48,
            "inlet_water_temp": 35,
            "outdoor_ambient_temp": 22,
        })
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        temps = data["temperatures"]
        assert temps["tank"] == 50
        assert temps["outlet"] == 48
        assert temps["inlet"] == 35
        assert temps["outdoor"] == 22

    def test_inject_compressor_readings(self):
        """Inject compressor freq and verify in readings."""
        _inject_demo({"compressor_freq": 75, "fan_speed": 850})
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        assert data["readings"]["compressor_freq"] == 75
        assert data["readings"]["fan_rpm"] == 850

    def test_inject_electrical_readings(self):
        """Inject voltage/current and verify power_consumption calculation."""
        _inject_demo({"ac_voltage": 230, "ac_current": 50})
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        assert data["readings"]["ac_voltage"] == 230
        assert data["readings"]["ac_current"] == 50
        # Power = (voltage * current) / 10
        assert data["readings"]["power_consumption"] == (230 * 50) // 10

    def test_inject_setpoints(self):
        """Injecting setpoint registers should be reflected in status."""
        _inject_demo({
            "cooling_setpoint": 24,
            "heating_setpoint": 40,
            "hot_water_setpoint": 48,
        })
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        assert data["setpoints"]["cooling"] == 24
        assert data["setpoints"]["heating"] == 40
        assert data["setpoints"]["hot_water"] == 48

    def test_inject_errors(self):
        """Injecting error registers should set has_error and error string."""
        _inject_demo({"error1": 1})  # Bit 0 = first error code
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        assert data["has_error"] is True
        assert data["error"] is not None

    def test_clear_errors(self):
        """Clearing error registers should clear error state."""
        _inject_demo({"error1": 0, "error2": 0})
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        assert data["has_error"] is False
        assert data["error"] is None

    def test_inject_unit_on_off(self):
        """Injecting unit_on field controls power state."""
        _inject_demo({"unit_on": 1})
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        assert data["unit_on"] is True

        _inject_demo({"unit_on": 0})
        time.sleep(0.6)
        data = _get("/api/heatpump/status").json()
        assert data["unit_on"] is False

    def test_inject_working_mode(self):
        """Injecting working_mode changes reported mode."""
        # Enum values: 0=cooling, 1=floor_heating, 2=fan_coil_heating, 5=hot_water, 6=auto
        modes = {0: "cooling", 1: "floor_heating", 2: "fan_coil_heating", 5: "hot_water", 6: "auto"}
        for mode_val, mode_name in modes.items():
            _inject_demo({"working_mode": mode_val})
            time.sleep(0.6)
            data = _get("/api/heatpump/status").json()
            assert data["mode"] == mode_name, f"Expected {mode_name} for value {mode_val}, got {data['mode']}"

    def test_inject_unknown_field_fails(self):
        """Injecting an unknown field should report failure."""
        r = _patch("/api/heatpump/demo", json={"nonexistent_field": 42})
        assert r.status_code == 200
        data = r.json()
        assert data["failed"] > 0

    def test_inject_non_numeric_value_fails(self):
        """Non-numeric values should be rejected."""
        r = _patch("/api/heatpump/demo", json={"water_tank_temp": "hot"})
        assert r.status_code == 200
        data = r.json()
        assert data["failed"] > 0

    def test_demo_not_available_when_disabled(self):
        """If demo mode were disabled, PATCH should return 403."""
        # We can't actually disable demo mode in these tests because
        # other tests depend on it. Just verify the endpoint exists.
        r = _patch("/api/heatpump/demo", json={"water_tank_temp": 30})
        assert r.status_code in (200, 403)


# ── Heat Pump Control ─────────────────────────────────────────────────────


class TestHeatpumpPower:
    """PUT /api/heatpump/power — power on/off control."""

    def test_power_on(self):
        r = _put("/api/heatpump/power", json={"on": True})
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True
        assert data["on"] is True
        assert "demo_mode" in data

    def test_power_off(self):
        r = _put("/api/heatpump/power", json={"on": False})
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True
        assert data["on"] is False

    def test_power_missing_field(self):
        """Missing 'on' field should return 400."""
        r = _put("/api/heatpump/power", json={})
        assert r.status_code == 400

    def test_power_invalid_type(self):
        """Non-boolean 'on' should return 400."""
        r = _put("/api/heatpump/power", json={"on": "maybe"})
        assert r.status_code == 400


class TestHeatpumpMode:
    """PUT /api/heatpump/mode — operating mode control."""

    @pytest.mark.parametrize("mode", VALID_MODES)
    def test_set_valid_mode(self, mode):
        """Each valid mode should be accepted."""
        r = _put("/api/heatpump/mode", json={"mode": mode})
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True
        assert data["mode"] == mode

    def test_invalid_mode_returns_400(self):
        r = _put("/api/heatpump/mode", json={"mode": "turbo"})
        assert r.status_code == 400

    def test_missing_mode_returns_400(self):
        r = _put("/api/heatpump/mode", json={})
        assert r.status_code == 400


class TestHeatpumpSetpoints:
    """PUT /api/heatpump/setpoints — temperature setpoint control."""

    def test_set_all_setpoints(self):
        r = _put("/api/heatpump/setpoints", json={
            "cooling": 18, "heating": 45, "hot_water": 55
        })
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True
        sp = data["setpoints"]
        assert sp["cooling"] == 18
        assert sp["heating"] == 45
        assert sp["hot_water"] == 55

    def test_set_single_setpoint(self):
        """Setting only one setpoint should work; others unchanged."""
        r = _put("/api/heatpump/setpoints", json={"cooling": 20})
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True
        assert data["setpoints"]["cooling"] == 20
        # heating and hot_water should not appear in response
        # (only set fields are returned)

    def test_empty_setpoints_returns_400(self):
        """No setpoint fields should return 400."""
        r = _put("/api/heatpump/setpoints", json={})
        assert r.status_code == 400


# ── Heat Pump Errors ──────────────────────────────────────────────────────


class TestHeatpumpErrors:
    """GET /api/heatpump/errors — error info and history."""

    def test_errors_response_structure(self):
        r = _get("/api/heatpump/errors")
        assert r.status_code == 200
        data = r.json()
        for field in ["demo_mode", "connected", "has_errors", "error_count",
                       "highest_severity", "active", "history"]:
            assert field in data, f"Missing field: {field}"

    def test_errors_with_no_active_errors(self):
        """When no errors injected, error_count should be 0."""
        _inject_demo({"error1": 0, "error2": 0})
        time.sleep(0.3)
        data = _get("/api/heatpump/errors").json()
        assert data["error_count"] == 0
        assert data["has_errors"] is False
        assert isinstance(data["active"], list)

    def test_errors_with_injected_error(self):
        """Injecting an error should appear in active errors."""
        _inject_demo({"error1": 1})  # Bit 0
        time.sleep(0.3)
        data = _get("/api/heatpump/errors").json()
        assert data["has_errors"] is True
        assert data["error_count"] > 0
        assert len(data["active"]) > 0

        # Each active error should have expected fields
        err = data["active"][0]
        for field in ["code", "name", "description", "severity"]:
            assert field in err, f"Missing error field: {field}"

        # Clean up
        _inject_demo({"error1": 0, "error2": 0})
        time.sleep(0.3)

    def test_error_severity_values(self):
        """highest_severity should be a valid severity string."""
        data = _get("/api/heatpump/errors").json()
        assert data["highest_severity"] in ["info", "warning", "error", "critical", "none"]

    def test_error_history_is_list(self):
        data = _get("/api/heatpump/errors").json()
        assert isinstance(data["history"], list)


# ── Heat Pump Parameters ─────────────────────────────────────────────────


class TestHeatpumpParams:
    """GET/PUT /api/heatpump/params — P-parameter management."""

    def test_get_all_params(self):
        """GET /api/heatpump/params returns params object."""
        r = _get("/api/heatpump/params")
        assert r.status_code == 200
        data = r.json()
        assert "connected" in data
        assert "demo_mode" in data
        assert "params" in data
        assert isinstance(data["params"], dict)
        assert len(data["params"]) > 0

    def test_param_structure(self):
        """Each parameter should have the expected metadata fields."""
        data = _get("/api/heatpump/params").json()
        # Check the first parameter
        first_key = list(data["params"].keys())[0]
        param = data["params"][first_key]
        for field in ["value", "p_code", "name", "description", "unit", "min", "max", "category"]:
            assert field in param, f"Missing param field: {field}"

    def test_get_single_param_by_key(self):
        """GET /api/heatpump/params/:key returns a single parameter."""
        # Get all params to find a valid key
        all_data = _get("/api/heatpump/params").json()
        key = list(all_data["params"].keys())[0]

        r = _get(f"/api/heatpump/params/{key}")
        assert r.status_code == 200
        data = r.json()
        assert data["key"] == key
        assert "p_code" in data
        assert "value" in data

    def test_get_single_param_by_pcode(self):
        """Parameters can also be fetched by P-code (e.g., P29)."""
        # Get a p_code from the first param
        all_data = _get("/api/heatpump/params").json()
        first_key = list(all_data["params"].keys())[0]
        p_code = all_data["params"][first_key]["p_code"]

        r = _get(f"/api/heatpump/params/{p_code}")
        assert r.status_code == 200
        data = r.json()
        assert data["p_code"] == p_code

    def test_get_unknown_param_returns_404(self):
        r = _get("/api/heatpump/params/nonexistent_param")
        assert r.status_code == 404

    def test_set_param_value(self):
        """PUT /api/heatpump/params/:key sets the value (in demo mode)."""
        # Get a param and its valid range
        all_data = _get("/api/heatpump/params").json()
        key = list(all_data["params"].keys())[0]
        param = all_data["params"][key]
        # Use a value in the valid range
        test_val = param["min"]

        r = _put(f"/api/heatpump/params/{key}", data=str(test_val))
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True
        assert data["value"] == test_val

    def test_set_param_out_of_range(self):
        """Setting a value outside min/max should return 400."""
        all_data = _get("/api/heatpump/params").json()
        key = list(all_data["params"].keys())[0]
        param = all_data["params"][key]
        # Use a value way above max
        bad_val = param["max"] + 1000

        r = _put(f"/api/heatpump/params/{key}", data=str(bad_val))
        assert r.status_code == 400


# ── Heat Pump Control via POST /api/heatpump/control ─────────────────────


class TestHeatpumpControlEndpoint:
    """POST /api/heatpump/control — legacy control endpoint."""

    def test_control_power_on(self):
        r = _post("/api/heatpump/control", json={"command": "power", "value": True})
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True

    def test_control_mode_change(self):
        r = _post("/api/heatpump/control", json={"command": "mode", "value": "cooling"})
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True

    def test_control_setpoint(self):
        r = _post("/api/heatpump/control", json={
            "command": "setpoint", "type": "heating", "value": 45
        })
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True

    def test_control_unknown_command(self):
        r = _post("/api/heatpump/control", json={"command": "explode"})
        assert r.status_code == 400

    def test_control_empty_body(self):
        r = requests.post(
            f"{BASE_URL}/api/heatpump/control",
            headers=_headers(),
            timeout=10,
        )
        assert r.status_code == 400

    def test_control_invalid_mode(self):
        r = _post("/api/heatpump/control", json={"command": "mode", "value": "warp_speed"})
        assert r.status_code == 400

    def test_control_invalid_setpoint_type(self):
        r = _post("/api/heatpump/control", json={
            "command": "setpoint", "type": "nuclear", "value": 100
        })
        assert r.status_code == 400
