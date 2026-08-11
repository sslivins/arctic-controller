"""Climate entity for power, coarse HVAC display, and advertised setpoints."""

from __future__ import annotations

from arctic_controller import ControllerState
from homeassistant.components.climate import ClimateEntity
from homeassistant.components.climate.const import (
    HVACAction,
    HVACMode,
    ClimateEntityFeature,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import ATTR_TEMPERATURE, UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .entity import ArcticEntity
from .runtime import ArcticRuntime

MODE_MAP = {
    "cooling": HVACMode.COOL,
    "floor_heating": HVACMode.HEAT,
    "fan_coil_heating": HVACMode.HEAT,
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
    def min_temp(self) -> float:
        capabilities = self.runtime.client.capabilities
        state = self._state
        if capabilities is None or state is None:
            return 0
        if state.mode == "cooling":
            return capabilities.cooling_range.minimum
        if state.mode == "hot_water":
            return capabilities.hot_water_range.minimum
        return capabilities.heating_range.minimum

    @property
    def max_temp(self) -> float:
        capabilities = self.runtime.client.capabilities
        state = self._state
        if capabilities is None or state is None:
            return 0
        if state.mode == "cooling":
            return capabilities.cooling_range.maximum
        if state.mode == "hot_water":
            return capabilities.hot_water_range.maximum
        return capabilities.heating_range.maximum

    @property
    def hvac_modes(self) -> list[HVACMode]:
        capabilities = self.runtime.client.capabilities
        modes = [HVACMode.OFF]
        state = self._state
        if capabilities is not None and capabilities.control_power:
            if "cooling" in capabilities.supported_modes:
                modes.append(HVACMode.COOL)
            if "auto" in capabilities.supported_modes:
                modes.append(HVACMode.HEAT_COOL)
            if any(
                mode in capabilities.supported_modes
                for mode in (
                    "floor_heating",
                    "fan_coil_heating",
                    "hot_water",
                )
            ):
                modes.append(HVACMode.HEAT)
        if state is not None:
            current_mode = MODE_MAP.get(state.mode)
            if current_mode is not None and current_mode not in modes:
                modes.append(current_mode)
        return modes

    @property
    def supported_features(self) -> int:
        state = self._state
        capabilities = self.runtime.client.capabilities
        if state is None or capabilities is None:
            return 0
        supported = 0
        if state.mode == "cooling" and capabilities.setpoint_controls.cooling:
            supported |= ClimateEntityFeature.TARGET_TEMPERATURE
        elif (
            state.mode == "hot_water"
            and capabilities.setpoint_controls.hot_water
        ):
            supported |= ClimateEntityFeature.TARGET_TEMPERATURE
        elif capabilities.setpoint_controls.heating:
            supported |= ClimateEntityFeature.TARGET_TEMPERATURE
        return supported

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
        capabilities = self.runtime.client.capabilities
        if capabilities is None or not capabilities.control_power:
            raise HomeAssistantError("Power control is unavailable")
        if hvac_mode == HVACMode.OFF:
            await self.runtime.client.async_set_power(False)
            return

        if hvac_mode == HVACMode.HEAT:
            state = self._state
            if state is not None and state.mode in {
                "floor_heating",
                "fan_coil_heating",
                "hot_water",
            }:
                await self.runtime.client.async_set_power(True)
                return
            raise HomeAssistantError(
                "Select an exact Arctic heating mode before turning heat on"
            )

        exact_mode = {
            HVACMode.COOL: "cooling",
            HVACMode.HEAT_COOL: "auto",
        }.get(hvac_mode)
        if exact_mode is None:
            raise HomeAssistantError(
                "Select an exact Arctic mode from the mode entity"
            )
        if (
            not capabilities.control_mode
            or exact_mode not in capabilities.supported_modes
        ):
            raise HomeAssistantError("Selected-mode control is unavailable")
        await self.runtime.client.async_set_mode(exact_mode)
        await self.runtime.client.async_set_power(True)

    async def async_set_temperature(self, **kwargs: float) -> None:
        value = kwargs.get(ATTR_TEMPERATURE)
        if value is None or isinstance(value, bool) or int(value) != value:
            raise HomeAssistantError("Arctic setpoints require whole degrees C")
        state = self._state
        capabilities = self.runtime.client.capabilities
        if state is None or capabilities is None:
            raise HomeAssistantError("Setpoint control is unavailable")
        if state.mode == "cooling":
            if not capabilities.setpoint_controls.cooling:
                raise HomeAssistantError("Cooling setpoint control is unavailable")
            await self.runtime.client.async_set_cooling_setpoint(int(value))
        elif state.mode == "hot_water":
            if not capabilities.setpoint_controls.hot_water:
                raise HomeAssistantError(
                    "Hot-water setpoint control is unavailable"
                )
            await self.runtime.client.async_set_hot_water_setpoint(int(value))
        else:
            if not capabilities.setpoint_controls.heating:
                raise HomeAssistantError(
                    "Heating setpoint control is unavailable"
                )
            await self.runtime.client.async_set_heating_setpoint(int(value))

    @property
    def _state(self) -> ControllerState | None:
        snapshot = self.runtime.snapshot
        return None if snapshot is None else snapshot.state
