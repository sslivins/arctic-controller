"""Exact Arctic working-mode selection."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.components.select import SelectEntity

from .entity import ArcticEntity
from .runtime import ArcticRuntime


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    async_add_entities([ArcticModeSelect(entry.runtime_data)])


class ArcticModeSelect(ArcticEntity, SelectEntity):
    """Expose the exact server-advertised Arctic mode allowlist."""

    _attr_name = "Selected mode"

    def __init__(self, runtime: ArcticRuntime) -> None:
        super().__init__(runtime, "mode")

    @property
    def options(self) -> list[str]:
        capabilities = self.runtime.client.capabilities
        return [] if capabilities is None else list(capabilities.supported_modes)

    @property
    def current_option(self) -> str | None:
        snapshot = self.runtime.snapshot
        if snapshot is None or snapshot.state.mode not in self.options:
            return None
        return snapshot.state.mode

    @property
    def available(self) -> bool:
        capabilities = self.runtime.client.capabilities
        return (
            super().available
            and capabilities is not None
            and capabilities.control_mode
            and bool(capabilities.supported_modes)
        )

    async def async_select_option(self, option: str) -> None:
        capabilities = self.runtime.client.capabilities
        if (
            capabilities is None
            or not capabilities.control_mode
            or option not in capabilities.supported_modes
        ):
            raise HomeAssistantError(
                "The selected Arctic mode is not currently supported"
            )
        await self.runtime.client.async_set_mode(option)
