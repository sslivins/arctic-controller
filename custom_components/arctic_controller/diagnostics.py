"""Redacted diagnostics for the Arctic Controller integration."""

from __future__ import annotations

from dataclasses import asdict
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant

from .runtime import ArcticRuntime


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: ConfigEntry
) -> dict[str, Any]:
    """Return useful state without host, token, or certificate pin."""
    runtime: ArcticRuntime = entry.runtime_data
    snapshot = runtime.snapshot
    capabilities = runtime.client.capabilities
    return {
        "entry": {
            "device_id": entry.unique_id,
            "port": entry.data["port"],
        },
        "status": {
            "available": runtime.status.available,
            "stream_connected": runtime.status.stream_connected,
            "last_error_type": (
                type(runtime.status.last_error).__name__
                if runtime.status.last_error is not None
                else None
            ),
        },
        "capabilities": (
            asdict(capabilities) if capabilities is not None else None
        ),
        "snapshot": asdict(snapshot) if snapshot is not None else None,
    }
