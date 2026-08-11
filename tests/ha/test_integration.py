"""Test the read-only Home Assistant integration."""

from __future__ import annotations

from unittest.mock import AsyncMock, MagicMock, patch

from arctic_controller import (
    ArcticAuthenticationError,
    ArcticCertificateError,
    ArcticConnectionError,
    ClientStatus,
)
from homeassistant.components.climate import DOMAIN as CLIMATE_DOMAIN
from homeassistant.components.sensor import DOMAIN as SENSOR_DOMAIN
from homeassistant.config_entries import ConfigEntryState
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.arctic_controller.const import (
    CONF_DEVICE_ID,
    CONF_FINGERPRINT,
    CONF_TOKEN,
    DEFAULT_PORT,
    DOMAIN,
)
from custom_components.arctic_controller.diagnostics import (
    async_get_config_entry_diagnostics,
)

from .conftest import make_snapshot

FINGERPRINT = "AA" * 32
TOKEN = "ab" * 32


def make_entry(device_id: str, host: str) -> MockConfigEntry:
    return MockConfigEntry(
        domain=DOMAIN,
        title=f"Arctic Controller {device_id}",
        unique_id=device_id,
        data={
            CONF_HOST: host,
            CONF_PORT: DEFAULT_PORT,
            CONF_DEVICE_ID: device_id,
            CONF_FINGERPRINT: FINGERPRINT,
            CONF_TOKEN: TOKEN,
        },
    )


async def setup_entry(
    hass: HomeAssistant, device_id: str, host: str
) -> MockConfigEntry:
    entry = make_entry(device_id, host)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry


def entity_id(
    hass: HomeAssistant, platform: str, unique_id: str
) -> str:
    result = er.async_get(hass).async_get_entity_id(
        platform, DOMAIN, unique_id
    )
    assert result is not None
    return result


async def test_two_entries_are_independent_and_push_updates_entities(
    hass: HomeAssistant, mock_clients: dict[str, MagicMock]
) -> None:
    first = await setup_entry(hass, "arctic-001", "controller-1.local")
    second = await setup_entry(hass, "arctic-002", "controller-2.local")

    first_sensor = entity_id(
        hass, SENSOR_DOMAIN, "arctic-001_tank_temperature"
    )
    second_sensor = entity_id(
        hass, SENSOR_DOMAIN, "arctic-002_tank_temperature"
    )
    assert hass.states.get(first_sensor).state == "42.0"
    assert hass.states.get(second_sensor).state == "42.0"

    pushed = make_snapshot(
        "arctic-001",
        revision=2,
        mode="hot_water",
        operation="idle",
    )
    mock_clients["controller-1.local"].snapshot_callback(pushed)
    await hass.async_block_till_done()

    climate = entity_id(
        hass, CLIMATE_DOMAIN, "arctic-001_climate"
    )
    assert hass.states.get(climate).state == "heat"
    assert hass.states.get(climate).attributes["hvac_action"] == "idle"
    assert hass.states.get(second_sensor).state == "42.0"
    assert first.runtime_data is not second.runtime_data


async def test_availability_and_unload_cleanup(
    hass: HomeAssistant, mock_clients: dict[str, MagicMock]
) -> None:
    entry = await setup_entry(hass, "arctic-001", "controller.local")
    client = mock_clients["controller.local"]
    sensor = entity_id(
        hass, SENSOR_DOMAIN, "arctic-001_tank_temperature"
    )

    client.status_callback(ClientStatus(False, False, OSError("offline")))
    await hass.async_block_till_done()
    assert hass.states.get(sensor).state == "unavailable"

    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.NOT_LOADED
    client.unsubscribe_snapshot.assert_called_once()
    client.unsubscribe_status.assert_called_once()
    client.stop.assert_awaited_once()


async def test_diagnostics_redact_connection_secrets(
    hass: HomeAssistant, mock_clients: dict[str, MagicMock]
) -> None:
    entry = await setup_entry(hass, "arctic-001", "controller.local")

    diagnostics = await async_get_config_entry_diagnostics(hass, entry)
    rendered = repr(diagnostics)

    assert diagnostics["entry"]["device_id"] == "arctic-001"
    assert "controller.local" not in rendered
    assert TOKEN not in rendered
    assert FINGERPRINT not in rendered


async def test_runtime_auth_failure_starts_reauthentication_once(
    hass: HomeAssistant, mock_clients: dict[str, MagicMock]
) -> None:
    entry = await setup_entry(hass, "arctic-001", "controller.local")
    client = mock_clients["controller.local"]
    error = ArcticAuthenticationError("token rejected")

    with patch.object(entry, "async_start_reauth") as start_reauth:
        client.status_callback(ClientStatus(False, False, error))
        client.status_callback(ClientStatus(False, False, error))

    start_reauth.assert_called_once_with(hass)


async def test_runtime_certificate_change_starts_reauthentication(
    hass: HomeAssistant, mock_clients: dict[str, MagicMock]
) -> None:
    entry = await setup_entry(hass, "arctic-001", "controller.local")
    client = mock_clients["controller.local"]

    with patch.object(entry, "async_start_reauth") as start_reauth:
        client.status_callback(
            ClientStatus(
                False,
                False,
                ArcticCertificateError("certificate changed"),
            )
        )

    start_reauth.assert_called_once_with(hass)


async def test_setup_auth_failure_cleans_up_client(
    hass: HomeAssistant,
) -> None:
    client = MagicMock()
    client.subscribe.return_value = MagicMock()
    client.subscribe_status.return_value = MagicMock()
    client.start = AsyncMock(
        side_effect=ArcticAuthenticationError("token rejected")
    )
    client.stop = AsyncMock()
    entry = make_entry("arctic-001", "controller.local")
    entry.add_to_hass(hass)

    with patch(
        "custom_components.arctic_controller.ArcticControllerClient",
        return_value=client,
    ):
        assert not await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    assert entry.state is ConfigEntryState.SETUP_ERROR
    client.stop.assert_awaited_once()


async def test_setup_connection_failure_retries_and_cleans_up(
    hass: HomeAssistant,
) -> None:
    client = MagicMock()
    client.subscribe.return_value = MagicMock()
    client.subscribe_status.return_value = MagicMock()
    client.start = AsyncMock(
        side_effect=ArcticConnectionError("offline")
    )
    client.stop = AsyncMock()
    entry = make_entry("arctic-001", "controller.local")
    entry.add_to_hass(hass)

    with patch(
        "custom_components.arctic_controller.ArcticControllerClient",
        return_value=client,
    ):
        assert not await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    assert entry.state is ConfigEntryState.SETUP_RETRY
    client.stop.assert_awaited_once()


async def test_platform_setup_failure_cleans_up_running_client(
    hass: HomeAssistant, mock_clients: dict[str, MagicMock]
) -> None:
    entry = make_entry("arctic-001", "controller.local")
    entry.add_to_hass(hass)

    with patch.object(
        hass.config_entries,
        "async_forward_entry_setups",
        AsyncMock(side_effect=RuntimeError("platform failed")),
    ):
        assert not await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    mock_clients["controller.local"].stop.assert_awaited_once()
