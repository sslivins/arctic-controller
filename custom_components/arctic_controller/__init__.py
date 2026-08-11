"""Arctic Heat Pump Controller integration."""

from __future__ import annotations

from arctic_controller import (
    ArcticAuthenticationError,
    ArcticCertificateError,
    ArcticConnectionError,
    ArcticControllerClient,
    ArcticProtocolError,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed, ConfigEntryNotReady
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import (
    CONF_DEVICE_ID,
    CONF_FINGERPRINT,
    CONF_TOKEN,
    PLATFORMS,
)
from .runtime import ArcticRuntime


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry
) -> bool:
    """Set up one independently paired Arctic Controller."""
    client = ArcticControllerClient(
        entry.data[CONF_HOST],
        entry.data[CONF_TOKEN],
        entry.data[CONF_FINGERPRINT],
        device_id=entry.data[CONF_DEVICE_ID],
        port=entry.data[CONF_PORT],
        session=async_get_clientsession(hass),
    )
    runtime = ArcticRuntime(hass, entry, client)
    try:
        await runtime.async_setup()
    except (
        ArcticAuthenticationError,
        ArcticCertificateError,
    ) as error:
        await runtime.async_shutdown()
        raise ConfigEntryAuthFailed from error
    except (ArcticConnectionError, ArcticProtocolError) as error:
        await runtime.async_shutdown()
        raise ConfigEntryNotReady from error

    entry.runtime_data = runtime
    setup_complete = False
    try:
        await hass.config_entries.async_forward_entry_setups(
            entry, PLATFORMS
        )
        setup_complete = True
    finally:
        if not setup_complete:
            await runtime.async_shutdown()
    return True


async def async_unload_entry(
    hass: HomeAssistant, entry: ConfigEntry
) -> bool:
    """Unload one controller without affecting other config entries."""
    unloaded = await hass.config_entries.async_unload_platforms(
        entry, PLATFORMS
    )
    if unloaded:
        await entry.runtime_data.async_shutdown()
    return unloaded
