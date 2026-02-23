"""
Arctic Simulator — HTTP client for the Modbus heat pump simulator.

Wraps the arctic-simulator REST API so end-to-end tests can control the
simulated heat pump registers and verify that the controller reads/writes
correctly over RS-485.

Simulator repo: https://github.com/sslivins/arctic-simulator
"""

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry
from typing import Dict, Optional, Union


class SimulatorError(Exception):
    """Raised when the simulator returns an error response."""
    pass


class SimulatorClient:
    """HTTP client for the Arctic Heat Pump Simulator REST API."""

    def __init__(self, base_url: str = "http://arctic-sim.local", timeout: float = 5.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()

        # Retry transient errors (the Atom S3 can be flaky over WiFi)
        retry_strategy = Retry(
            total=3,
            backoff_factor=0.5,
            allowed_methods=None,
            status_forcelist=[502, 503, 504],
        )
        adapter = HTTPAdapter(max_retries=retry_strategy)
        self.session.mount("http://", adapter)
        self.session.mount("https://", adapter)

    # ------------------------------------------------------------------
    # Status
    # ------------------------------------------------------------------

    def get_status(self) -> dict:
        """GET /api/status — simulator health, Modbus stats, playback state."""
        r = self.session.get(f"{self.base_url}/api/status", timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    # ------------------------------------------------------------------
    # Register access
    # ------------------------------------------------------------------

    def get_registers(self) -> dict:
        """GET /api/registers — all register values as JSON."""
        r = self.session.get(f"{self.base_url}/api/registers", timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def get_register(self, addr: int) -> int:
        """GET /api/registers?addr=XXXX — read a single register value."""
        r = self.session.get(
            f"{self.base_url}/api/registers",
            params={"addr": addr},
            timeout=self.timeout,
        )
        r.raise_for_status()
        data = r.json()
        return data["value"]

    def set_register(self, addr: int, value: int) -> dict:
        """PUT /api/registers?addr=XXXX — set a single register value."""
        r = self.session.put(
            f"{self.base_url}/api/registers",
            params={"addr": addr},
            json={"value": value},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            raise SimulatorError(f"Set register {addr}={value} failed ({r.status_code}): {r.text}")
        return r.json()

    def bulk_set(self, registers: Dict[Union[int, str], int]) -> dict:
        """POST /api/registers/bulk — set multiple registers at once.

        Args:
            registers: Dict mapping register address (int or str) to value.
                       Example: {2100: 350, 2110: 200} or {"2100": 350}
        """
        # Ensure keys are strings (the simulator API expects string keys)
        str_regs = {str(k): v for k, v in registers.items()}
        r = self.session.post(
            f"{self.base_url}/api/registers/bulk",
            json={"registers": str_regs},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            raise SimulatorError(f"Bulk set failed ({r.status_code}): {r.text}")
        return r.json()

    # ------------------------------------------------------------------
    # Presets
    # ------------------------------------------------------------------

    def load_preset(self, name: str) -> dict:
        """POST /api/preset — load a named preset.

        Available presets: idle, heating, cooling, hot_water, defrost,
                          error_e01, error_p01
        """
        r = self.session.post(
            f"{self.base_url}/api/preset",
            json={"name": name},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            raise SimulatorError(f"Load preset '{name}' failed ({r.status_code}): {r.text}")
        return r.json()

    # ------------------------------------------------------------------
    # Error control
    # ------------------------------------------------------------------

    def clear_errors(self) -> dict:
        """POST /api/errors/clear — clear all error flags."""
        r = self.session.post(
            f"{self.base_url}/api/errors/clear", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()

    # ------------------------------------------------------------------
    # Convenience helpers
    # ------------------------------------------------------------------

    def set_temperature(self, register: int, value: int) -> dict:
        """Set a temperature register to a specific value.

        Common registers:
            2100 = water tank temp
            2102 = outlet water temp
            2103 = inlet water temp
            2110 = outdoor ambient temp
        """
        return self.set_register(register, value)

    def set_error_bit(self, error_register: int, bit: int) -> dict:
        """Set a specific error bit in an error register.

        Args:
            error_register: 2134 (error1), 2137 (error2), or 2138 (error3)
            bit: Bit position (0-15)
        """
        current = self.get_register(error_register)
        new_value = current | (1 << bit)
        return self.set_register(error_register, new_value)

    def clear_error_bit(self, error_register: int, bit: int) -> dict:
        """Clear a specific error bit in an error register."""
        current = self.get_register(error_register)
        new_value = current & ~(1 << bit)
        return self.set_register(error_register, new_value)

    def is_reachable(self) -> bool:
        """Quick health check — returns True if the simulator responds."""
        try:
            self.get_status()
            return True
        except Exception:
            return False
