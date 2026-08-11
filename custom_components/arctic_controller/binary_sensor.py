"""Read-only binary sensors for the Arctic Controller."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from arctic_controller import StateSnapshot
from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
    BinarySensorEntityDescription,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .entity import ArcticEntity
from .runtime import ArcticRuntime


@dataclass(frozen=True, kw_only=True)
class ArcticBinarySensorDescription(BinarySensorEntityDescription):
    value_fn: Callable[[StateSnapshot], bool]


DESCRIPTIONS: tuple[ArcticBinarySensorDescription, ...] = (
    ArcticBinarySensorDescription(
        key="heat_pump_connected",
        name="Heat pump connected",
        device_class=BinarySensorDeviceClass.CONNECTIVITY,
        value_fn=lambda value: value.state.connected,
    ),
    ArcticBinarySensorDescription(
        key="unit_power",
        name="Unit power",
        value_fn=lambda value: value.state.unit_on,
    ),
    ArcticBinarySensorDescription(
        key="defrosting",
        name="Defrosting",
        value_fn=lambda value: value.state.defrosting,
    ),
    ArcticBinarySensorDescription(
        key="active_error",
        name="Active error",
        device_class=BinarySensorDeviceClass.PROBLEM,
        value_fn=lambda value: value.state.error.active,
    ),
    ArcticBinarySensorDescription(
        key="compressor",
        name="Compressor",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: value.state.components.compressor,
    ),
    ArcticBinarySensorDescription(
        key="fan",
        name="Fan",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: value.state.components.fan,
    ),
    ArcticBinarySensorDescription(
        key="water_pump",
        name="Water pump",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: value.state.components.water_pump,
    ),
    ArcticBinarySensorDescription(
        key="backup_heater",
        name="Backup heater",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: value.state.components.backup_heater,
    ),
    ArcticBinarySensorDescription(
        key="reversing_valve_request",
        name="Reversing valve request",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: (
            value.state.components.reversing_valve_request
        ),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    runtime: ArcticRuntime = entry.runtime_data
    async_add_entities(
        ArcticBinarySensor(runtime, description)
        for description in DESCRIPTIONS
    )


class ArcticBinarySensor(ArcticEntity, BinarySensorEntity):
    entity_description: ArcticBinarySensorDescription

    def __init__(
        self,
        runtime: ArcticRuntime,
        description: ArcticBinarySensorDescription,
    ) -> None:
        super().__init__(runtime, description.key)
        self.entity_description = description

    @property
    def is_on(self) -> bool | None:
        snapshot = self.runtime.snapshot
        if snapshot is None:
            return None
        return self.entity_description.value_fn(snapshot)
