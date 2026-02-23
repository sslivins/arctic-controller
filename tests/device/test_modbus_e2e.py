"""
End-to-end Modbus integration tests.

These tests drive both the Arctic Simulator (via REST API) and the
controller (via its HTTP API) to verify real RS-485 Modbus communication.

Requirements:
    - Simulator running at SIMULATOR_URL (default: http://arctic-sim.local)
    - Controller running at ARCTIC_URL (default: http://arctic.local)
    - RS-485 cable connecting the two devices

The ``modbus_mode`` fixture (conftest.py) handles switching the controller
out of demo mode, loading the simulator's "heating" preset, and waiting
for the controller to establish a Modbus connection.  It restores demo
mode on teardown.
"""

import time
import pytest

from device_client import DeviceClient
from simulator_client import SimulatorClient

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Polling interval for the controller's Modbus cycle.
# 4 reads × 500 ms = 2 s per full cycle.  3 s gives ample margin.
POLL_SETTLE_S = 3.0


@pytest.fixture(autouse=True)
def _reset_modbus_state(simulator: SimulatorClient, modbus_mode):
    """Reload the heating preset before each test to ensure clean state.

    This is cheap (single HTTP call to the simulator) compared to the
    reboot cycle that modbus_mode uses for demo mode toggling.
    """
    simulator.load_preset("heating")
    simulator.clear_errors()
    time.sleep(POLL_SETTLE_S)


def _wait_for(fn, *, timeout: float = 5.0, poll: float = 0.5, desc: str = "condition"):
    """Poll *fn* until it returns True, or raise after *timeout* seconds."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return
        time.sleep(poll)
    raise AssertionError(f"Timed out after {timeout}s waiting for {desc}")


# =========================================================================
# Heating preset values (from arctic-simulator register_map.cpp)
# Used to verify the controller reads the correct data.
# =========================================================================
HEATING_PRESET = {
    "unit_on": True,
    "mode": "floor_heating",
    "temperatures": {
        "outdoor": 5,
        "inlet": 35,
        "outlet": 42,
        "discharge": 75,
        "suction": 3,
        "outdoor_coil": 2,
        "ipm": 45,
    },
    "setpoints": {
        "heating": 45,
    },
    "readings": {
        "compressor_freq": 55,
        "fan_rpm": 600,
    },
}


# =========================================================================
# Complete error code definitions — matches heatpump_errors.cpp exactly.
# Each entry: (register, bit, code, severity)
# =========================================================================

# Error register 1 (2137) — 16 error definitions
ERROR_REG1_DEFS = [
    (2137, 0,  "E27", "error"),     # Indoor EEPROM error
    (2137, 1,  "E28", "error"),     # Outdoor EEPROM fault
    (2137, 2,  "E19", "error"),     # Inlet water temp sensor fault
    (2137, 3,  "E18", "error"),     # Outlet water temp sensor fault
    (2137, 4,  "E13", "error"),     # Cooling coil temp sensor fault
    (2137, 5,  "E05", "error"),     # Heat pump coil temp sensor fault
    (2137, 6,  "E01", "error"),     # Compressor discharge temp sensor fault
    (2137, 7,  "E09", "error"),     # Compressor suction temp sensor fault
    (2137, 8,  "E22", "error"),     # Outdoor ambient temp sensor fault
    (2137, 9,  "E10", "critical"),  # Communication error drive/main board
    (2137, 10, "E21", "warning"),   # Wired controller communication fault
    (2137, 11, "r02", "critical"),  # Compressor start fault
    (2137, 12, "E12", "critical"),  # Communication error indoor/outdoor unit
    (2137, 13, "r01", "critical"),  # IPM module fault
    (2137, 14, "PA",  "critical"),  # Tank temperature protection
    (2137, 15, "r10", "error"),     # AC voltage protection
]

# Error register 2 (2138) — 16 error definitions
ERROR_REG2_DEFS = [
    (2138, 0,  "P19", "error"),     # AC current protection
    (2138, 1,  "r06", "critical"),  # Compressor phase current protection
    (2138, 2,  "FA",  "error"),     # DC fan motor protection
    (2138, 3,  "r11", "critical"),  # DC bus voltage protection
    (2138, 4,  "r05", "critical"),  # IPM module temperature too high
    (2138, 5,  "P11", "critical"),  # Compressor discharge temp too high
    (2138, 6,  "P02", "critical"),  # High pressure protection
    (2138, 7,  "P06", "error"),     # Low pressure protection
    (2138, 8,  "P01", "error"),     # Water flow switch protection
    (2138, 9,  "P27", "warning"),   # Cooling coil temp overheating
    (2138, 10, "E26", "warning"),   # Low ambient temperature
    (2138, 11, "EC",  "error"),     # EEV circuit low pressure
    (2138, 12, "ED",  "error"),     # Low pressure (pressure sensor)
    (2138, 13, "P15", "warning"),   # Inlet/outlet temp difference too large
    (2138, 14, "P16", "warning"),   # Outlet water temp too low
    (2138, 15, "r20", "error"),     # Compressor protection
]

ALL_ERROR_DEFS = ERROR_REG1_DEFS + ERROR_REG2_DEFS


# Operating mode name → register value mapping
MODE_MAP = {
    "cooling": 0,
    "floor_heating": 1,
    "fan_coil_heating": 2,
    "hot_water": 5,
    "auto": 6,
}


# =========================================================================
# Temperature register → API field mapping
# =========================================================================

TEMP_REGISTER_MAP = {
    2100: "tank",            # Water tank temperature
    2102: "outlet",          # Outlet water temperature
    2103: "inlet",           # Inlet water temperature
    2104: "discharge",       # Compressor discharge temperature
    2105: "suction",         # Compressor suction temperature
    2107: "outdoor_coil",    # Outdoor coil temperature
    2108: "indoor_coil",     # Indoor coil temperature
    2110: "outdoor",         # Outdoor ambient temperature
    2114: "ipm",             # IPM module temperature
}


# =========================================================================
# Tests
# =========================================================================


@pytest.mark.modbus
class TestModbusConnection:
    """Verify basic Modbus connectivity and data reads."""

    def test_connection_and_data_read(self, device: DeviceClient, modbus_mode):
        """Controller connects to the simulator and reads the heating preset data."""
        status = device.get_heatpump_status()

        # Connection
        assert status["connected"], "Controller should be connected"
        assert not status["demo_mode"], "Demo mode should be off"

        # Unit state
        assert status["unit_on"] == HEATING_PRESET["unit_on"]
        assert status["mode"] == HEATING_PRESET["mode"]

        # Temperatures — the heating preset sets specific values
        for key, expected in HEATING_PRESET["temperatures"].items():
            actual = status["temperatures"][key]
            assert actual == expected, (
                f"Temperature '{key}': expected {expected}, got {actual}"
            )

        # Setpoints
        assert status["setpoints"]["heating"] == HEATING_PRESET["setpoints"]["heating"]

        # System readings
        assert status["readings"]["compressor_freq"] == HEATING_PRESET["readings"]["compressor_freq"]
        assert status["readings"]["fan_rpm"] == HEATING_PRESET["readings"]["fan_rpm"]

    def test_no_errors_on_heating_preset(self, device: DeviceClient, modbus_mode):
        """Heating preset should have no error flags set."""
        status = device.get_heatpump_status()
        assert not status["has_error"], f"Expected no errors, got: {status.get('error')}"


@pytest.mark.modbus
class TestModbusPowerControl:
    """Verify power on/off commands write through to the simulator."""

    def test_power_off(self, device: DeviceClient, simulator: SimulatorClient, modbus_mode):
        """Controller sends power OFF → simulator register 2000 = 0."""
        # Heating preset starts with unit ON
        status = device.get_heatpump_status()
        assert status["unit_on"], "Precondition: unit should be ON"

        # Send power off
        device.heatpump_control("power", value=False)
        time.sleep(POLL_SETTLE_S)

        # Verify simulator received the write
        reg_value = simulator.get_register(2000)
        assert reg_value == 0, f"Simulator reg 2000 should be 0 (OFF), got {reg_value}"

        # Verify controller reflects the new state (simulator auto-acks via STATUS_2)
        _wait_for(
            lambda: not device.get_heatpump_status()["unit_on"],
            timeout=5.0,
            desc="controller to show unit OFF",
        )

    def test_power_on(self, device: DeviceClient, simulator: SimulatorClient, modbus_mode):
        """Controller sends power ON → simulator auto-acks via STATUS_2 bit 0.

        This verifies the key behaviour: when the controller writes
        UNIT_ON_OFF (reg 2000) = 1, the simulator's ``simulation::updateStatus()``
        automatically sets STATUS_2 (reg 2135) bit 0 (STS2_UNIT_ON) plus
        other status bits based on WORKING_MODE (reg 2001).
        """
        # First turn the unit OFF in the simulator
        simulator.set_register(2000, 0)
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert not status["unit_on"], "Precondition: unit should be OFF"

        # Now send power ON from the controller
        device.heatpump_control("power", value=True)
        time.sleep(POLL_SETTLE_S)

        # Verify simulator received the write
        reg_value = simulator.get_register(2000)
        assert reg_value == 1, f"Simulator reg 2000 should be 1 (ON), got {reg_value}"

        # Verify auto-ack: STATUS_2 should have STS2_UNIT_ON set
        sts2 = simulator.get_register(2135)
        assert sts2 & 0x0001, f"STATUS_2 bit 0 (STS2_UNIT_ON) not set: 0x{sts2:04X}"

        # Verify controller sees unit ON
        _wait_for(
            lambda: device.get_heatpump_status()["unit_on"],
            timeout=5.0,
            desc="controller to show unit ON",
        )


# =========================================================================
# All Operating Modes
# =========================================================================


@pytest.mark.modbus
class TestModbusAllModes:
    """Verify every operating mode can be set from the controller."""

    @pytest.mark.parametrize("mode,reg_value", list(MODE_MAP.items()))
    def test_mode_change(self, device: DeviceClient, simulator: SimulatorClient,
                         modbus_mode, mode: str, reg_value: int):
        """Controller sets each mode → simulator register 2001 matches expected value."""
        device.heatpump_control("mode", value=mode)
        time.sleep(POLL_SETTLE_S)

        # Verify simulator received the write
        _wait_for(
            lambda: simulator.get_register(2001) == reg_value,
            timeout=5.0,
            desc=f"simulator reg 2001 to be {reg_value} ({mode})",
        )

        # Verify controller reflects the new mode
        status = device.get_heatpump_status()
        assert status["mode"] == mode, (
            f"Expected mode '{mode}', got '{status['mode']}'"
        )


# =========================================================================
# Comprehensive Temperature Tests
# =========================================================================


@pytest.mark.modbus
class TestModbusTemperatures:
    """Verify temperature values propagate correctly from simulator to controller."""

    def test_heating_preset_temperatures(self, device: DeviceClient, modbus_mode):
        """Verify temperatures from the heating preset are read correctly."""
        status = device.get_heatpump_status()
        temps = status["temperatures"]

        for key, expected in HEATING_PRESET["temperatures"].items():
            actual = temps[key]
            assert actual == expected, (
                f"Temperature '{key}': expected {expected}, got {actual}"
            )

    @pytest.mark.parametrize("register,api_field", list(TEMP_REGISTER_MAP.items()))
    def test_individual_temperature_register(self, device: DeviceClient,
                                              simulator: SimulatorClient,
                                              modbus_mode, register: int,
                                              api_field: str):
        """Set each temperature register individually, verify controller reads it."""
        # Use a unique value per register to avoid false positives
        test_value = 10 + (register % 100)

        simulator.set_register(register, test_value)
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        actual = status["temperatures"][api_field]
        assert actual == test_value, (
            f"Reg {register} → '{api_field}': expected {test_value}, got {actual}"
        )

    def test_negative_temperatures(self, device: DeviceClient,
                                   simulator: SimulatorClient, modbus_mode):
        """Verify negative temperature values (signed int16) are read correctly.

        Modbus registers use unsigned 16-bit words.  Negative Celsius values
        are stored as two's-complement: e.g. -5 °C = 0xFFFB (65531).
        """
        # Outdoor temp = -5 °C (two's-complement: 0xFFFB = 65531)
        simulator.set_register(2110, 65531)
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert status["temperatures"]["outdoor"] == -5, (
            f"Expected outdoor -5, got {status['temperatures']['outdoor']}"
        )

    def test_zero_temperature(self, device: DeviceClient,
                              simulator: SimulatorClient, modbus_mode):
        """Verify 0 °C is read correctly."""
        simulator.set_register(2110, 0)
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert status["temperatures"]["outdoor"] == 0

    def test_high_temperatures(self, device: DeviceClient,
                               simulator: SimulatorClient, modbus_mode):
        """Verify high temperature values (e.g. discharge temp of 120 °C)."""
        simulator.set_register(2104, 120)  # Discharge temp
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert status["temperatures"]["discharge"] == 120

    def test_multiple_temperatures_simultaneous(self, device: DeviceClient,
                                                simulator: SimulatorClient,
                                                modbus_mode):
        """Set all temperature registers at once, verify all are read correctly."""
        test_temps = {
            2100: 50,   # tank
            2102: 47,   # outlet
            2103: 40,   # inlet
            2104: 90,   # discharge
            2105: 8,    # suction
            2107: 15,   # outdoor coil
            2108: 35,   # indoor coil
            2110: -3 & 0xFFFF,  # outdoor (-3 °C as uint16)
            2114: 60,   # ipm
        }
        simulator.bulk_set(test_temps)
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        temps = status["temperatures"]

        assert temps["tank"] == 50
        assert temps["outlet"] == 47
        assert temps["inlet"] == 40
        assert temps["discharge"] == 90
        assert temps["suction"] == 8
        assert temps["outdoor_coil"] == 15
        assert temps["indoor_coil"] == 35
        assert temps["outdoor"] == -3
        assert temps["ipm"] == 60


# =========================================================================
# Fan Speed Tests
# =========================================================================


@pytest.mark.modbus
class TestModbusFanSpeed:
    """Verify fan speed level is correctly derived from STATUS_1 register bits.

    STATUS_1 (register 2135) bits:
        Bit 2 = FAN_HIGH (0x0004)
        Bit 3 = FAN_MED  (0x0008)
        Bit 4 = FAN_LOW  (0x0010)

    Controller's getFanSpeedLevel():
        FAN_HIGH → 3, FAN_MED → 2, FAN_LOW → 1, none → 0
    """

    def _set_status1_fan_bits(self, simulator: SimulatorClient, fan_bits: int):
        """Set fan bits in STATUS_1 while keeping UNIT_ON."""
        # Read current STATUS_1, clear fan bits, set new ones
        current = simulator.get_register(2135)
        # Clear bits 2, 3, 4 (fan bits)
        cleared = current & ~(0x0004 | 0x0008 | 0x0010)
        new_value = cleared | fan_bits
        simulator.set_register(2135, new_value)

    def test_fan_speed_off(self, device: DeviceClient, simulator: SimulatorClient,
                           modbus_mode):
        """No fan bits set → fan_speed = 0."""
        self._set_status1_fan_bits(simulator, 0x0000)
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert status["fan_speed"] == 0, (
            f"Expected fan_speed 0, got {status['fan_speed']}"
        )

    def test_fan_speed_low(self, device: DeviceClient, simulator: SimulatorClient,
                           modbus_mode):
        """FAN_LOW bit set → fan_speed = 1."""
        self._set_status1_fan_bits(simulator, 0x0010)  # Bit 4
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert status["fan_speed"] == 1, (
            f"Expected fan_speed 1, got {status['fan_speed']}"
        )

    def test_fan_speed_med(self, device: DeviceClient, simulator: SimulatorClient,
                           modbus_mode):
        """FAN_MED bit set → fan_speed = 2."""
        self._set_status1_fan_bits(simulator, 0x0008)  # Bit 3
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert status["fan_speed"] == 2, (
            f"Expected fan_speed 2, got {status['fan_speed']}"
        )

    def test_fan_speed_high(self, device: DeviceClient, simulator: SimulatorClient,
                            modbus_mode):
        """FAN_HIGH bit set → fan_speed = 3."""
        self._set_status1_fan_bits(simulator, 0x0004)  # Bit 2
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert status["fan_speed"] == 3, (
            f"Expected fan_speed 3, got {status['fan_speed']}"
        )

    def test_fan_speed_priority(self, device: DeviceClient, simulator: SimulatorClient,
                                modbus_mode):
        """When multiple fan bits are set, highest wins (FAN_HIGH > FAN_MED > FAN_LOW)."""
        # Set both FAN_HIGH and FAN_LOW — should report 3 (high)
        self._set_status1_fan_bits(simulator, 0x0004 | 0x0010)
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        assert status["fan_speed"] == 3, (
            f"Expected fan_speed 3 (HIGH wins), got {status['fan_speed']}"
        )


# =========================================================================
# Comprehensive Error Code Tests — Every Single Error
# =========================================================================


@pytest.mark.modbus
class TestModbusAllErrors:
    """Verify every single error code is detected via the /api/heatpump/errors endpoint.

    Tests all 32 errors: 16 in register 2137 (error1) + 16 in register 2138 (error2).
    Each test injects a single error bit on the simulator, waits for the controller
    to poll it, then verifies the error code, severity, and active status.
    """

    @pytest.mark.parametrize(
        "register,bit,code,severity",
        ALL_ERROR_DEFS,
        ids=[f"{d[2]}_reg{d[0]}_bit{d[1]}" for d in ALL_ERROR_DEFS],
    )
    def test_individual_error(self, device: DeviceClient, simulator: SimulatorClient,
                              modbus_mode, register: int, bit: int, code: str,
                              severity: str):
        """Inject a single error bit → controller detects the error with correct code and severity."""
        # _reset_modbus_state fixture already cleared errors and loaded heating preset

        # Inject the error
        simulator.set_error_bit(register, bit)
        time.sleep(POLL_SETTLE_S)

        # Wait for controller to detect it
        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc=f"controller to detect error {code}",
        )

        # Verify via /api/heatpump/errors
        errors = device.get_heatpump_errors()
        assert errors["has_errors"], f"Expected has_errors=True for {code}"
        assert errors["error_count"] >= 1, f"Expected error_count >= 1 for {code}"

        # Find the specific error code in the active list
        active_codes = [e["code"] for e in errors["active"]]
        assert code in active_codes, (
            f"Error code '{code}' not found in active errors: {active_codes}"
        )

        # Verify the error details
        error_entry = next(e for e in errors["active"] if e["code"] == code)
        assert error_entry["severity"] == severity, (
            f"Error {code}: expected severity '{severity}', got '{error_entry['severity']}'"
        )
        assert error_entry["active"] is True
        assert error_entry["name"], f"Error {code} should have a non-empty name"
        assert error_entry["description"], f"Error {code} should have a description"

        # Also verify via /api/heatpump/status
        status = device.get_heatpump_status()
        assert code in status.get("error", ""), (
            f"Error {code} not in status error string: {status.get('error')}"
        )


@pytest.mark.modbus
class TestModbusMultipleErrors:
    """Verify multiple simultaneous errors are detected correctly."""

    def test_two_errors_same_register(self, device: DeviceClient,
                                      simulator: SimulatorClient, modbus_mode):
        """Inject two errors in register 2137 simultaneously."""
        # Set E01 (bit 6) and E05 (bit 5) in register 2137
        simulator.set_register(2137, (1 << 6) | (1 << 5))
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect errors",
        )

        errors = device.get_heatpump_errors()
        active_codes = {e["code"] for e in errors["active"]}
        assert "E01" in active_codes, f"E01 not found in {active_codes}"
        assert "E05" in active_codes, f"E05 not found in {active_codes}"
        assert errors["error_count"] >= 2

    def test_two_errors_different_registers(self, device: DeviceClient,
                                            simulator: SimulatorClient, modbus_mode):
        """Inject errors in both register 2137 and 2138 simultaneously."""
        # E01 in reg 2137 (bit 6) + P02 in reg 2138 (bit 6)
        simulator.set_error_bit(2137, 6)
        simulator.set_error_bit(2138, 6)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect errors",
        )

        errors = device.get_heatpump_errors()
        active_codes = {e["code"] for e in errors["active"]}
        assert "E01" in active_codes, f"E01 not found in {active_codes}"
        assert "P02" in active_codes, f"P02 not found in {active_codes}"
        assert errors["error_count"] >= 2

    def test_all_errors_register1(self, device: DeviceClient,
                                  simulator: SimulatorClient, modbus_mode):
        """Set all 16 error bits in register 2137 — verify all 16 codes are reported."""
        # Set all 16 bits
        simulator.set_register(2137, 0xFFFF)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect errors",
        )

        errors = device.get_heatpump_errors()
        active_codes = {e["code"] for e in errors["active"]}
        expected_codes = {d[2] for d in ERROR_REG1_DEFS}
        missing = expected_codes - active_codes
        assert not missing, (
            f"Missing error codes from register 2137: {missing}. "
            f"Got: {active_codes}"
        )

    def test_all_errors_register2(self, device: DeviceClient,
                                  simulator: SimulatorClient, modbus_mode):
        """Set all 16 error bits in register 2138 — verify all 16 codes are reported."""
        # Set all 16 bits
        simulator.set_register(2138, 0xFFFF)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect errors",
        )

        errors = device.get_heatpump_errors()
        active_codes = {e["code"] for e in errors["active"]}
        expected_codes = {d[2] for d in ERROR_REG2_DEFS}
        missing = expected_codes - active_codes
        assert not missing, (
            f"Missing error codes from register 2138: {missing}. "
            f"Got: {active_codes}"
        )

    def test_highest_severity_critical(self, device: DeviceClient,
                                       simulator: SimulatorClient, modbus_mode):
        """When a critical error is present, highest_severity should be 'critical'."""
        # r01 (bit 13, reg 2137) is critical
        simulator.set_error_bit(2137, 13)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect error",
        )

        errors = device.get_heatpump_errors()
        assert errors["highest_severity"] == "critical"

    def test_highest_severity_warning(self, device: DeviceClient,
                                      simulator: SimulatorClient, modbus_mode):
        """When only a warning error is active, highest_severity should be 'warning'."""
        # E21 (bit 10, reg 2137) is warning severity
        simulator.set_error_bit(2137, 10)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect error",
        )

        errors = device.get_heatpump_errors()
        assert errors["highest_severity"] == "warning"


@pytest.mark.modbus
class TestModbusErrorClearing:
    """Verify error clearing works correctly."""

    def test_error_inject_and_clear(self, device: DeviceClient,
                                    simulator: SimulatorClient, modbus_mode):
        """Inject error → clear it → controller shows no errors."""
        # Inject E01
        simulator.set_error_bit(2137, 6)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect error",
        )

        # Clear all errors
        simulator.clear_errors()
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: not device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to clear error",
        )

        errors = device.get_heatpump_errors()
        assert not errors["has_errors"]
        assert errors["error_count"] == 0

    def test_clear_single_bit(self, device: DeviceClient,
                              simulator: SimulatorClient, modbus_mode):
        """Set two error bits, clear one — only the remaining error should be active."""
        # Set E01 (bit 6) and E05 (bit 5) in register 2137
        simulator.set_register(2137, (1 << 6) | (1 << 5))
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_errors()["error_count"] >= 2,
            timeout=5.0,
            desc="controller to detect both errors",
        )

        # Clear only E01 (bit 6), keep E05 (bit 5)
        simulator.clear_error_bit(2137, 6)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_errors()["error_count"] == 1,
            timeout=8.0,
            desc="controller to show only one error",
        )

        errors = device.get_heatpump_errors()
        active_codes = [e["code"] for e in errors["active"]]
        assert "E05" in active_codes, f"E05 should still be active, got {active_codes}"
        assert "E01" not in active_codes, f"E01 should be cleared, got {active_codes}"

    def test_error_history_populated(self, device: DeviceClient,
                                     simulator: SimulatorClient, modbus_mode):
        """After injecting and clearing an error, it should appear in error history."""
        device.clear_error_history()

        # Inject E01, wait for detection, then clear
        simulator.set_error_bit(2137, 6)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect E01",
        )

        simulator.clear_errors()
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: not device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to clear E01",
        )

        # Check history
        errors = device.get_heatpump_errors()
        history = errors.get("history", [])
        history_codes = [h["code"] for h in history]
        assert "E01" in history_codes, (
            f"E01 should be in error history, got: {history_codes}"
        )

        # The history entry should be cleared (not active)
        e01_history = [h for h in history if h["code"] == "E01"]
        assert any(not h.get("active", True) for h in e01_history), (
            "E01 history entry should have active=false"
        )


# =========================================================================
# Setpoint Write Tests
# =========================================================================


@pytest.mark.modbus
class TestModbusSetpointWrite:
    """Verify setpoint changes from the controller reach the simulator."""

    def test_setpoint_write_through(self, device: DeviceClient, simulator: SimulatorClient, modbus_mode):
        """Controller changes heating setpoint → simulator register updated."""
        # Read initial setpoint from simulator
        initial = simulator.get_register(2003)  # heating setpoint register
        assert initial == 45, f"Expected initial setpoint 45, got {initial}"

        # Change setpoint via controller API
        new_setpoint = 50
        device.heatpump_control("setpoint", type="heating", value=new_setpoint)
        time.sleep(1.0)  # Single write completes quickly

        # Verify simulator register was updated
        _wait_for(
            lambda: simulator.get_register(2003) == new_setpoint,
            timeout=5.0,
            desc=f"simulator reg 2003 to be {new_setpoint}",
        )

        # Verify controller also reflects the new setpoint
        status = device.get_heatpump_status()
        assert status["setpoints"]["heating"] == new_setpoint, (
            f"Controller setpoint: expected {new_setpoint}, got {status['setpoints']['heating']}"
        )
