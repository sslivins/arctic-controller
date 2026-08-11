"""Base entity for one Arctic Controller config entry."""

from __future__ import annotations

from homeassistant.helpers.entity import Entity

from .runtime import ArcticRuntime


class ArcticEntity(Entity):
    """Base class updated directly by the local-push runtime."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, runtime: ArcticRuntime, key: str) -> None:
        self.runtime = runtime
        device_id = runtime.entry.data["device_id"]
        self._attr_unique_id = f"{device_id}_{key}"
        self._attr_device_info = runtime.device_info

    @property
    def available(self) -> bool:
        return self.runtime.available

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(
            self.runtime.async_add_listener(
                self._async_runtime_updated
            )
        )

    def _async_runtime_updated(self) -> None:
        self.async_write_ha_state()
