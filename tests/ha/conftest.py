"""Fixtures for the Home Assistant integration tests."""

from __future__ import annotations

from collections.abc import Generator
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from arctic_controller import (
    ClientStatus,
    ControllerCapabilities,
    StateSnapshot,
)


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    """Enable custom integration loading."""
    yield


def make_snapshot(
    device_id: str = "arctic-001",
    *,
    revision: int = 1,
    mode: str = "heating",
    operation: str = "heating",
    available: bool = True,
) -> StateSnapshot:
    """Build a representative controller snapshot."""
    return StateSnapshot.from_dict(
        {
            "protocol_version": 1,
            "device_id": device_id,
            "boot_id": "boot-1",
            "revision": revision,
            "captured_at_ms": revision * 1000,
            "state": {
                "connected": available,
                "unit_on": True,
                "mode": mode,
                "operation": operation,
                "defrosting": operation == "defrost",
                "components": {
                    "compressor": True,
                    "fan": True,
                    "fan_level": 3,
                    "water_pump": True,
                    "backup_heater": False,
                    "reversing_valve_request": mode == "cooling",
                },
                "temperatures_c": {
                    "tank": 42,
                    "outlet": 39,
                    "inlet": 34,
                    "outdoor_ambient": 7,
                    "discharge": 65,
                    "suction": 4,
                    "outdoor_coil": 2,
                    "indoor_coil": 36,
                    "ipm": 41,
                },
                "setpoints_c": {
                    "cooling": 12,
                    "heating": 38,
                    "hot_water": 48,
                },
                "readings": {
                    "compressor_frequency_hz": 45,
                    "fan_rpm": 720,
                    "power_w": 1600,
                    "thermal_w": 4800,
                    "cop": 3.0,
                },
                "error": {
                    "active": False,
                    "description": None,
                },
            },
        }
    )


def make_capabilities(device_id: str) -> ControllerCapabilities:
    """Build representative controller capabilities."""
    return ControllerCapabilities.from_dict(
        {
            "protocol_version": 1,
            "device_id": device_id,
            "model": "Arctic Controller",
            "firmware_version": "1.2.3",
            "transports": {"rest": True, "websocket": True},
            "capabilities": {
                "read_state": True,
                "control_power": True,
                "control_mode": True,
                "control_setpoints": True,
                "supported_modes": [
                    "cooling",
                    "floor_heating",
                    "fan_coil_heating",
                    "hot_water",
                    "auto",
                ],
                "setpoint_controls": {
                    "cooling": True,
                    "heating": True,
                    "hot_water": True,
                },
            },
            "setpoint_limits_c": {
                "cooling": {"min": 5, "max": 25},
                "heating": {"min": 20, "max": 55},
                "hot_water": {"min": 30, "max": 60},
            },
        }
    )


@pytest.fixture
def mock_clients() -> Generator[dict[str, MagicMock]]:
    """Create one isolated fake client for every configured host."""
    clients: dict[str, MagicMock] = {}

    def create_client(host, token, fingerprint, **kwargs):
        device_id = kwargs["device_id"]
        client = MagicMock(name=f"ArcticControllerClient({device_id})")
        client.capabilities = make_capabilities(device_id)
        client.status = ClientStatus(True, True, None)
        client.start = AsyncMock(return_value=make_snapshot(device_id))
        client.stop = AsyncMock()
        client.async_set_power = AsyncMock()
        client.async_set_mode = AsyncMock()
        client.async_set_cooling_setpoint = AsyncMock()
        client.async_set_heating_setpoint = AsyncMock()
        client.async_set_hot_water_setpoint = AsyncMock()

        def subscribe(callback):
            client.snapshot_callback = callback
            return client.unsubscribe_snapshot

        def subscribe_status(callback):
            client.status_callback = callback
            return client.unsubscribe_status

        def subscribe_capabilities(callback):
            client.capabilities_callback = callback
            return client.unsubscribe_capabilities

        client.unsubscribe_snapshot = MagicMock()
        client.unsubscribe_status = MagicMock()
        client.unsubscribe_capabilities = MagicMock()
        client.subscribe.side_effect = subscribe
        client.subscribe_status.side_effect = subscribe_status
        client.subscribe_capabilities.side_effect = subscribe_capabilities
        clients[host] = client
        return client

    with patch(
        "custom_components.arctic_controller.ArcticControllerClient",
        side_effect=create_client,
    ):
        yield clients
