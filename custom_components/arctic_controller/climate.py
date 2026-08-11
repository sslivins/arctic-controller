"""Read-only climate entity for selected mode and actual operation."""

from __future__ import annotations

from arctic_controller import ControllerState
from homeassistant.components.climate import ClimateEntity
from homeassistant.components.climate.const import HVACAction, HVACMode
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .entity import ArcticEntity
from .runtime import ArcticRuntime

MODE_MAP = {
    "cooling": HVACMode.COOL,
    "floor_heating": HVACMode.HEAT,
    "fan_coil_heating": HVACMode.HEAT,
    "heating": HVACMode.HEAT,
    "hot_water": HVACMode.HEAT,
    "auto": HVACMode.HEAT_COOL,
}

ACTION_MAP = {
    "off": HVACAction.OFF,
    "idle": HVACAction.IDLE,
    "heating": HVACAction.HEATING,
    "cooling": HVACAction.COOLING,
    "defrost": HVACAction.DEFROSTING,
    "fault": HVACAction.IDLE,
}


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    async_add_entities([ArcticClimate(entry.runtime_data)])


class ArcticClimate(ArcticEntity, ClimateEntity):
    """Expose requested mode separately from actual heat-pump operation."""

    _attr_name = None
    _attr_hvac_modes = [
        HVACMode.OFF,
        HVACMode.HEAT,
        HVACMode.COOL,
        HVACMode.HEAT_COOL,
    ]
    _attr_temperature_unit = UnitOfTemperature.CELSIUS

    def __init__(self, runtime: ArcticRuntime) -> None:
        super().__init__(runtime, "climate")

    @property
    def current_temperature(self) -> float | None:
        state = self._state
        return None if state is None else state.temperatures_c.tank

    @property
    def target_temperature(self) -> float | None:
        state = self._state
        if state is None:
            return None
        if state.mode == "cooling":
            return state.setpoints_c.cooling
        if state.mode == "hot_water":
            return state.setpoints_c.hot_water
        return state.setpoints_c.heating

    @property
    def hvac_mode(self) -> HVACMode | None:
        state = self._state
        if state is None:
            return None
        if not state.unit_on:
            return HVACMode.OFF
        return MODE_MAP.get(state.mode, HVACMode.OFF)

    @property
    def hvac_action(self) -> HVACAction | None:
        state = self._state
        if state is None or not state.connected:
            return None
        return ACTION_MAP.get(state.operation, HVACAction.IDLE)

    async def async_set_hvac_mode(self, hvac_mode: HVACMode) -> None:
        raise HomeAssistantError(
            "Arctic Controller controls are not enabled yet"
        )

    @property
    def _state(self) -> ControllerState | None:
        snapshot = self.runtime.snapshot
        return None if snapshot is None else snapshot.state
