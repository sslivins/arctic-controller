"""Read-only sensor entities for the Arctic Controller."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from arctic_controller import StateSnapshot
from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import (
    EntityCategory,
    UnitOfFrequency,
    UnitOfPower,
    UnitOfTemperature,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .entity import ArcticEntity
from .runtime import ArcticRuntime


@dataclass(frozen=True, kw_only=True)
class ArcticSensorDescription(SensorEntityDescription):
    value_fn: Callable[[StateSnapshot], str | int | float | None]


TEMPERATURES: tuple[ArcticSensorDescription, ...] = (
    ArcticSensorDescription(
        key="tank_temperature",
        name="Tank temperature",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda value: value.state.temperatures_c.tank,
    ),
    ArcticSensorDescription(
        key="outlet_temperature",
        name="Outlet temperature",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda value: value.state.temperatures_c.outlet,
    ),
    ArcticSensorDescription(
        key="inlet_temperature",
        name="Inlet temperature",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda value: value.state.temperatures_c.inlet,
    ),
    ArcticSensorDescription(
        key="outdoor_temperature",
        name="Outdoor temperature",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda value: value.state.temperatures_c.outdoor_ambient,
    ),
)

SETPOINTS: tuple[ArcticSensorDescription, ...] = (
    ArcticSensorDescription(
        key="cooling_setpoint",
        name="Cooling setpoint",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        value_fn=lambda value: value.state.setpoints_c.cooling,
    ),
    ArcticSensorDescription(
        key="heating_setpoint",
        name="Heating setpoint",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        value_fn=lambda value: value.state.setpoints_c.heating,
    ),
    ArcticSensorDescription(
        key="hot_water_setpoint",
        name="Hot water setpoint",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        value_fn=lambda value: value.state.setpoints_c.hot_water,
    ),
)

DIAGNOSTICS: tuple[ArcticSensorDescription, ...] = (
    ArcticSensorDescription(
        key="working_mode",
        name="Working mode",
        device_class=SensorDeviceClass.ENUM,
        options=[
            "cooling",
            "floor_heating",
            "fan_coil_heating",
            "heating",
            "hot_water",
            "auto",
            "unknown",
        ],
        value_fn=lambda value: value.state.mode,
    ),
    ArcticSensorDescription(
        key="operation",
        name="Operation",
        device_class=SensorDeviceClass.ENUM,
        options=[
            "off",
            "idle",
            "heating",
            "cooling",
            "defrost",
            "fault",
            "unknown",
        ],
        value_fn=lambda value: value.state.operation,
    ),
    ArcticSensorDescription(
        key="compressor_frequency",
        name="Compressor frequency",
        device_class=SensorDeviceClass.FREQUENCY,
        native_unit_of_measurement=UnitOfFrequency.HERTZ,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: (
            value.state.readings.compressor_frequency_hz
        ),
    ),
    ArcticSensorDescription(
        key="fan_speed",
        name="Fan speed",
        native_unit_of_measurement="rpm",
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: value.state.readings.fan_rpm,
    ),
    ArcticSensorDescription(
        key="power",
        name="Power",
        device_class=SensorDeviceClass.POWER,
        native_unit_of_measurement=UnitOfPower.WATT,
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda value: value.state.readings.power_w,
    ),
    ArcticSensorDescription(
        key="thermal_output",
        name="Thermal output",
        device_class=SensorDeviceClass.POWER,
        native_unit_of_measurement=UnitOfPower.WATT,
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda value: value.state.readings.thermal_w,
    ),
    ArcticSensorDescription(
        key="cop",
        name="Coefficient of performance",
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda value: value.state.readings.cop,
    ),
    ArcticSensorDescription(
        key="fan_level",
        name="Fan level",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: value.state.components.fan_level,
    ),
    ArcticSensorDescription(
        key="error_description",
        name="Error description",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda value: value.state.error.description,
    ),
)

EXTRA_TEMPERATURES: tuple[ArcticSensorDescription, ...] = tuple(
    ArcticSensorDescription(
        key=key,
        name=name,
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=value_fn,
    )
    for key, name, value_fn in (
        (
            "discharge_temperature",
            "Discharge temperature",
            lambda value: value.state.temperatures_c.discharge,
        ),
        (
            "suction_temperature",
            "Suction temperature",
            lambda value: value.state.temperatures_c.suction,
        ),
        (
            "outdoor_coil_temperature",
            "Outdoor coil temperature",
            lambda value: value.state.temperatures_c.outdoor_coil,
        ),
        (
            "indoor_coil_temperature",
            "Indoor coil temperature",
            lambda value: value.state.temperatures_c.indoor_coil,
        ),
        (
            "ipm_temperature",
            "IPM temperature",
            lambda value: value.state.temperatures_c.ipm,
        ),
    )
)

DESCRIPTIONS = TEMPERATURES + SETPOINTS + DIAGNOSTICS + EXTRA_TEMPERATURES


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    runtime: ArcticRuntime = entry.runtime_data
    async_add_entities(
        ArcticSensor(runtime, description)
        for description in DESCRIPTIONS
    )


class ArcticSensor(ArcticEntity, SensorEntity):
    entity_description: ArcticSensorDescription

    def __init__(
        self,
        runtime: ArcticRuntime,
        description: ArcticSensorDescription,
    ) -> None:
        super().__init__(runtime, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> str | int | float | None:
        snapshot = self.runtime.snapshot
        if snapshot is None:
            return None
        return self.entity_description.value_fn(snapshot)
