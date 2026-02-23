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


@pytest.mark.modbus
class TestModbusModeChange:
    """Verify mode changes sent by the controller reach the simulator."""

    def test_mode_change_to_cooling(self, device: DeviceClient, simulator: SimulatorClient, modbus_mode):
        """Controller sets mode to cooling → simulator register 2001 updated."""
        # Heating preset is loaded by modbus_mode — verify starting state
        status = device.get_heatpump_status()
        assert status["mode"] == "floor_heating"

        # Send mode change from controller
        device.heatpump_control("mode", value="cooling")
        time.sleep(POLL_SETTLE_S)

        # Verify simulator received the write (reg 2001: 0 = cooling)
        _wait_for(
            lambda: simulator.get_register(2001) == 0,
            timeout=5.0,
            desc="simulator reg 2001 to be 0 (cooling)",
        )

        # Verify controller also reflects the new mode
        status = device.get_heatpump_status()
        assert status["mode"] == "cooling", f"Expected 'cooling', got '{status['mode']}'"

    def test_mode_change_to_hot_water(self, device: DeviceClient, simulator: SimulatorClient, modbus_mode):
        """Controller sets mode to hot_water → simulator register 2001 updated."""
        device.heatpump_control("mode", value="hot_water")
        time.sleep(POLL_SETTLE_S)

        # Verify simulator received the write (reg 2001: 5 = hot water)
        _wait_for(
            lambda: simulator.get_register(2001) == 5,
            timeout=5.0,
            desc="simulator reg 2001 to be 5 (hot_water)",
        )


@pytest.mark.modbus
class TestModbusTemperatureReads:
    """Verify specific temperature values propagate from simulator to controller."""

    def test_temperature_reads(self, device: DeviceClient, simulator: SimulatorClient, modbus_mode):
        """Set known temperatures on the simulator, verify the controller reads them."""
        # Set distinct temperature values on the simulator
        test_temps = {
            2100: 38,   # water tank temp
            2102: 29,   # outlet water temp
            2103: 25,   # inlet water temp
            2110: 12,   # outdoor ambient temp
        }
        simulator.bulk_set(test_temps)
        time.sleep(POLL_SETTLE_S)

        status = device.get_heatpump_status()
        temps = status["temperatures"]
        assert temps["tank"] == 38, f"Tank temp: expected 38, got {temps['tank']}"
        assert temps["outlet"] == 29, f"Outlet temp: expected 29, got {temps['outlet']}"
        assert temps["inlet"] == 25, f"Inlet temp: expected 25, got {temps['inlet']}"
        assert temps["outdoor"] == 12, f"Outdoor temp: expected 12, got {temps['outdoor']}"


@pytest.mark.modbus
class TestModbusErrorHandling:
    """Verify error injection and clearing through the Modbus link."""

    def test_error_injection(self, device: DeviceClient, simulator: SimulatorClient, modbus_mode):
        """Inject E01 error on simulator → controller shows error."""
        # Verify no errors initially (heating preset has none)
        status = device.get_heatpump_status()
        assert not status["has_error"], "Precondition: no errors expected"

        # Inject E01 (discharge temp sensor error) — register 2137 bit 6
        simulator.set_error_bit(2137, 6)
        time.sleep(POLL_SETTLE_S)

        _wait_for(
            lambda: device.get_heatpump_status()["has_error"],
            timeout=5.0,
            desc="controller to detect error",
        )
        status = device.get_heatpump_status()
        assert "E01" in status.get("error", ""), (
            f"Expected 'E01' in error string, got: {status.get('error')}"
        )

    def test_error_clear(self, device: DeviceClient, simulator: SimulatorClient, modbus_mode):
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
