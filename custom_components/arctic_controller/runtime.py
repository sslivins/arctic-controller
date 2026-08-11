"""Per-config-entry runtime for one Arctic Controller."""

from __future__ import annotations

from collections.abc import Callable

from arctic_controller import (
    ArcticAuthenticationError,
    ArcticCertificateError,
    ArcticControllerClient,
    ClientStatus,
    ControllerCapabilities,
    StateSnapshot,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo

from .const import DOMAIN


class ArcticRuntime:
    """Own one controller client and notify its Home Assistant entities."""

    def __init__(
        self,
        hass: HomeAssistant,
        entry: ConfigEntry,
        client: ArcticControllerClient,
    ) -> None:
        self.hass = hass
        self.entry = entry
        self.client = client
        self.snapshot: StateSnapshot | None = None
        self.status = ClientStatus(False, False, None)
        self._listeners: set[Callable[[], None]] = set()
        self._unsubscribe_snapshot: Callable[[], None] | None = None
        self._unsubscribe_status: Callable[[], None] | None = None
        self._unsubscribe_capabilities: Callable[[], None] | None = None
        self._reauth_started = False

    @property
    def available(self) -> bool:
        return self.status.available and self.snapshot is not None

    @property
    def device_info(self) -> DeviceInfo:
        capabilities = self.client.capabilities
        return DeviceInfo(
            identifiers={(DOMAIN, self.entry.data["device_id"])},
            name=self.entry.title,
            manufacturer="Arctic",
            model=(
                capabilities.model
                if capabilities is not None
                else "Heat Pump Controller"
            ),
            sw_version=(
                capabilities.firmware_version
                if capabilities is not None
                else None
            ),
        )

    async def async_setup(self) -> None:
        self._unsubscribe_snapshot = self.client.subscribe(
            self._async_snapshot_received
        )
        self._unsubscribe_status = self.client.subscribe_status(
            self._async_status_received
        )
        self._unsubscribe_capabilities = self.client.subscribe_capabilities(
            self._async_capabilities_received
        )
        self.snapshot = await self.client.start()
        self.status = self.client.status

    async def async_shutdown(self) -> None:
        if self._unsubscribe_snapshot is not None:
            self._unsubscribe_snapshot()
            self._unsubscribe_snapshot = None
        if self._unsubscribe_status is not None:
            self._unsubscribe_status()
            self._unsubscribe_status = None
        if self._unsubscribe_capabilities is not None:
            self._unsubscribe_capabilities()
            self._unsubscribe_capabilities = None
        await self.client.stop()
        self._listeners.clear()

    @callback
    def async_add_listener(
        self, listener: Callable[[], None]
    ) -> Callable[[], None]:
        self._listeners.add(listener)

        @callback
        def unsubscribe() -> None:
            self._listeners.discard(listener)

        return unsubscribe

    @callback
    def _async_snapshot_received(self, snapshot: StateSnapshot) -> None:
        self.snapshot = snapshot
        self._async_notify_listeners()

    @callback
    def _async_status_received(self, status: ClientStatus) -> None:
        self.status = status
        if (
            isinstance(
                status.last_error,
                (ArcticAuthenticationError, ArcticCertificateError),
            )
            and not self._reauth_started
        ):
            self._reauth_started = True
            self.entry.async_start_reauth(self.hass)
        self._async_notify_listeners()

    @callback
    def _async_capabilities_received(
        self, capabilities: ControllerCapabilities
    ) -> None:
        self._async_notify_listeners()

    @callback
    def _async_notify_listeners(self) -> None:
        for listener in tuple(self._listeners):
            listener()
