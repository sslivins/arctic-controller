"""Test secure Arctic Controller config flows."""

from __future__ import annotations

from ipaddress import ip_address
from unittest.mock import AsyncMock, patch

from arctic_controller import (
    ArcticCertificateError,
    PairingResult,
)
from homeassistant.config_entries import (
    SOURCE_REAUTH,
    SOURCE_USER,
    SOURCE_ZEROCONF,
)
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.arctic_controller.config_flow import (
    CONF_PAIRING_CODE,
)
from custom_components.arctic_controller.const import (
    CONF_DEVICE_ID,
    CONF_FINGERPRINT,
    CONF_TOKEN,
    DEFAULT_PORT,
    DOMAIN,
)

DEVICE_ID = "arctic-001"
FINGERPRINT = "AA" * 32
TOKEN = "ab" * 32


def pairing(device_id: str = DEVICE_ID) -> PairingResult:
    return PairingResult(1, device_id, FINGERPRINT, TOKEN)


def user_data() -> dict[str, object]:
    return {
        CONF_HOST: "192.168.1.21",
        CONF_PORT: DEFAULT_PORT,
        CONF_FINGERPRINT: FINGERPRINT,
        CONF_PAIRING_CODE: "123456",
    }


async def test_user_flow_success(
    hass: HomeAssistant, mock_clients: dict[str, object]
) -> None:
    with patch(
        "custom_components.arctic_controller.config_flow."
        "ArcticControllerClient.pair",
        AsyncMock(return_value=pairing()),
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN, context={"source": SOURCE_USER}
        )
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], user_data()
        )

    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["result"].unique_id == DEVICE_ID
    assert result["data"][CONF_TOKEN] == TOKEN
    assert result["data"][CONF_FINGERPRINT] == FINGERPRINT


async def test_user_flow_rejects_bad_fingerprint(
    hass: HomeAssistant,
) -> None:
    with patch(
        "custom_components.arctic_controller.config_flow."
        "ArcticControllerClient.pair",
        AsyncMock(side_effect=ArcticCertificateError("wrong pin")),
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN, context={"source": SOURCE_USER}
        )
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], user_data()
        )

    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "invalid_fingerprint"}


async def test_duplicate_manual_pairing_preserves_rotated_token(
    hass: HomeAssistant,
) -> None:
    existing = MockConfigEntry(
        domain=DOMAIN,
        unique_id=DEVICE_ID,
        data={
            CONF_HOST: "old.local",
            CONF_PORT: DEFAULT_PORT,
            CONF_DEVICE_ID: DEVICE_ID,
            CONF_FINGERPRINT: "BB" * 32,
            CONF_TOKEN: "cd" * 32,
        },
    )
    existing.add_to_hass(hass)
    with patch(
        "custom_components.arctic_controller.config_flow."
        "ArcticControllerClient.pair",
        AsyncMock(return_value=pairing()),
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN, context={"source": SOURCE_USER}
        )
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], user_data()
        )

    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"
    assert existing.data[CONF_HOST] == "192.168.1.21"
    assert existing.data[CONF_FINGERPRINT] == FINGERPRINT
    assert existing.data[CONF_TOKEN] == TOKEN


async def test_zeroconf_uses_default_port_and_prevents_duplicates(
    hass: HomeAssistant,
) -> None:
    existing = MockConfigEntry(
        domain=DOMAIN,
        unique_id=DEVICE_ID,
        data={
            CONF_HOST: "old.local",
            CONF_PORT: DEFAULT_PORT,
            CONF_DEVICE_ID: DEVICE_ID,
            CONF_FINGERPRINT: FINGERPRINT,
            CONF_TOKEN: TOKEN,
        },
    )
    existing.add_to_hass(hass)
    discovery = ZeroconfServiceInfo(
        ip_address=ip_address("192.168.1.21"),
        ip_addresses=[ip_address("192.168.1.21")],
        port=None,
        hostname="arctic.local.",
        type="_arctic._tcp.local.",
        name="Arctic._arctic._tcp.local.",
        properties={"device": "arctic-controller", "id": DEVICE_ID},
    )

    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": SOURCE_ZEROCONF},
        data=discovery,
    )

    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"
    assert existing.data[CONF_HOST] == "192.168.1.21"
    assert existing.data[CONF_PORT] == DEFAULT_PORT


async def test_reauth_rejects_different_controller(
    hass: HomeAssistant,
) -> None:
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=DEVICE_ID,
        data={
            CONF_HOST: "arctic.local",
            CONF_PORT: DEFAULT_PORT,
            CONF_DEVICE_ID: DEVICE_ID,
            CONF_FINGERPRINT: FINGERPRINT,
            CONF_TOKEN: TOKEN,
        },
    )
    entry.add_to_hass(hass)
    other = MockConfigEntry(
        domain=DOMAIN,
        unique_id="arctic-002",
        data={
            CONF_HOST: "other-old.local",
            CONF_PORT: DEFAULT_PORT,
            CONF_DEVICE_ID: "arctic-002",
            CONF_FINGERPRINT: "CC" * 32,
            CONF_TOKEN: "ef" * 32,
        },
    )
    other.add_to_hass(hass)
    with patch(
        "custom_components.arctic_controller.config_flow."
        "ArcticControllerClient.pair",
        AsyncMock(return_value=pairing("arctic-002")),
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN,
            context={
                "source": SOURCE_REAUTH,
                "entry_id": entry.entry_id,
            },
            data=entry.data,
        )
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], user_data()
        )

    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "wrong_device"}
    assert other.data[CONF_HOST] == "192.168.1.21"
    assert other.data[CONF_FINGERPRINT] == FINGERPRINT
    assert other.data[CONF_TOKEN] == TOKEN


async def test_reauth_updates_credentials(
    hass: HomeAssistant, mock_clients: dict[str, object]
) -> None:
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=DEVICE_ID,
        data={
            CONF_HOST: "old.local",
            CONF_PORT: DEFAULT_PORT,
            CONF_DEVICE_ID: DEVICE_ID,
            CONF_FINGERPRINT: "BB" * 32,
            CONF_TOKEN: "cd" * 32,
        },
    )
    entry.add_to_hass(hass)
    with patch(
        "custom_components.arctic_controller.config_flow."
        "ArcticControllerClient.pair",
        AsyncMock(return_value=pairing()),
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN,
            context={
                "source": SOURCE_REAUTH,
                "entry_id": entry.entry_id,
            },
            data=entry.data,
        )
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], user_data()
        )
        await hass.async_block_till_done()

    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "reauth_successful"
    assert entry.data[CONF_HOST] == "192.168.1.21"
    assert entry.data[CONF_FINGERPRINT] == FINGERPRINT
    assert entry.data[CONF_TOKEN] == TOKEN
