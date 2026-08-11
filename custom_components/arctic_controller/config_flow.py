"""Config flow for securely pairing Arctic Controllers."""

from __future__ import annotations

from typing import Any

from arctic_controller import (
    ArcticCertificateError,
    ArcticConnectionError,
    ArcticControllerClient,
    ArcticPairingError,
    ArcticProtocolError,
    PairingResult,
)
from homeassistant import config_entries
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo
import voluptuous as vol

from .const import (
    CONF_DEVICE_ID,
    CONF_FINGERPRINT,
    CONF_TOKEN,
    DEFAULT_PORT,
    DOMAIN,
)

CONF_PAIRING_CODE = "pairing_code"


class ArcticControllerConfigFlow(
    config_entries.ConfigFlow, domain=DOMAIN
):
    """Pair one controller per config entry."""

    VERSION = 1

    def __init__(self) -> None:
        self._discovered_host: str | None = None
        self._discovered_port = DEFAULT_PORT
        self._discovered_device_id: str | None = None

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            result, error = await self._async_pair(user_input)
            if result is not None:
                return result
            errors["base"] = error

        return self.async_show_form(
            step_id="user",
            data_schema=self._schema(
                host=self._discovered_host,
                port=self._discovered_port,
            ),
            errors=errors,
        )

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> config_entries.ConfigFlowResult:
        properties = discovery_info.properties
        if properties.get("device") != "arctic-controller":
            return self.async_abort(reason="not_arctic_controller")
        device_id = properties.get("id")
        if not device_id:
            return self.async_abort(reason="invalid_discovery")

        await self.async_set_unique_id(device_id)
        self._abort_if_unique_id_configured(
            updates={
                CONF_HOST: discovery_info.host,
                CONF_PORT: discovery_info.port or DEFAULT_PORT,
            }
        )
        self._discovered_host = discovery_info.host
        self._discovered_port = discovery_info.port or DEFAULT_PORT
        self._discovered_device_id = device_id
        self.context["title_placeholders"] = {"name": device_id}
        return await self.async_step_confirm()

    async def async_step_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        if self._discovered_host is None:
            return self.async_abort(reason="invalid_discovery")
        errors: dict[str, str] = {}
        if user_input is not None:
            pair_input = {
                CONF_HOST: self._discovered_host,
                CONF_PORT: self._discovered_port,
                **user_input,
            }
            result, error = await self._async_pair(pair_input)
            if result is not None:
                return result
            errors["base"] = error

        return self.async_show_form(
            step_id="confirm",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_FINGERPRINT): str,
                    vol.Required(CONF_PAIRING_CODE): str,
                }
            ),
            errors=errors,
            description_placeholders={
                "host": self._discovered_host,
                "device_id": self._discovered_device_id or "",
            },
        )

    async def async_step_reauth(
        self, entry_data: dict[str, Any]
    ) -> config_entries.ConfigFlowResult:
        self._discovered_host = entry_data[CONF_HOST]
        self._discovered_port = entry_data[CONF_PORT]
        self._discovered_device_id = entry_data[CONF_DEVICE_ID]
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            pair_input = {
                CONF_HOST: user_input[CONF_HOST],
                CONF_PORT: user_input[CONF_PORT],
                CONF_FINGERPRINT: user_input[CONF_FINGERPRINT],
                CONF_PAIRING_CODE: user_input[CONF_PAIRING_CODE],
            }
            pairing, error = await self._async_claim(pair_input)
            if pairing is not None:
                if pairing.device_id != self._discovered_device_id:
                    await self._async_update_claimed_entry(
                        pair_input, pairing
                    )
                    errors["base"] = "wrong_device"
                else:
                    return self.async_update_reload_and_abort(
                        self._get_reauth_entry(),
                        data_updates=self._entry_data(
                            pair_input, pairing
                        ),
                        reason="reauth_successful",
                    )
            else:
                errors["base"] = error

        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=self._schema(
                host=self._discovered_host,
                port=self._discovered_port,
                fingerprint=None,
            ),
            errors=errors,
        )

    async def _async_pair(
        self, user_input: dict[str, Any]
    ) -> tuple[config_entries.ConfigFlowResult | None, str]:
        pairing, error = await self._async_claim(user_input)
        if pairing is None:
            return None, error
        await self.async_set_unique_id(pairing.device_id)
        self._abort_if_unique_id_configured(
            updates=self._entry_data(user_input, pairing)
        )
        title = f"Arctic Controller {pairing.device_id[-6:].upper()}"
        return (
            self.async_create_entry(
                title=title,
                data=self._entry_data(user_input, pairing),
            ),
            "",
        )

    async def _async_claim(
        self, user_input: dict[str, Any]
    ) -> tuple[PairingResult | None, str]:
        try:
            pairing = await ArcticControllerClient.pair(
                user_input[CONF_HOST],
                user_input[CONF_PAIRING_CODE],
                user_input[CONF_FINGERPRINT],
                port=user_input[CONF_PORT],
                session=async_get_clientsession(self.hass),
            )
        except ArcticCertificateError:
            return None, "invalid_fingerprint"
        except ArcticPairingError:
            return None, "invalid_pairing_code"
        except ArcticConnectionError:
            return None, "cannot_connect"
        except (ArcticProtocolError, ValueError):
            return None, "invalid_response"
        return pairing, ""

    async def _async_update_claimed_entry(
        self,
        user_input: dict[str, Any],
        pairing: PairingResult,
    ) -> None:
        """Preserve a rotated credential if this device already exists."""
        entry = self.hass.config_entries.async_entry_for_domain_unique_id(
            DOMAIN, pairing.device_id
        )
        if entry is None:
            return
        changed = self.hass.config_entries.async_update_entry(
            entry,
            data={
                **entry.data,
                **self._entry_data(user_input, pairing),
            },
        )
        if changed and entry.state in {
            config_entries.ConfigEntryState.LOADED,
            config_entries.ConfigEntryState.SETUP_RETRY,
        }:
            await self.hass.config_entries.async_reload(entry.entry_id)

    @staticmethod
    def _entry_data(
        user_input: dict[str, Any], pairing: PairingResult
    ) -> dict[str, Any]:
        return {
            CONF_HOST: user_input[CONF_HOST],
            CONF_PORT: user_input[CONF_PORT],
            CONF_DEVICE_ID: pairing.device_id,
            CONF_FINGERPRINT: pairing.fingerprint,
            CONF_TOKEN: pairing.token,
        }

    @staticmethod
    def _schema(
        *,
        host: str | None,
        port: int,
        fingerprint: str | None = None,
    ) -> vol.Schema:
        fields: dict[Any, Any] = {
            vol.Required(
                CONF_HOST, default=host or "arctic.local"
            ): str,
            vol.Required(CONF_PORT, default=port): int,
            vol.Required(CONF_FINGERPRINT): str,
            vol.Required(CONF_PAIRING_CODE): str,
        }
        if fingerprint is not None:
            fields[vol.Required(
                CONF_FINGERPRINT, default=fingerprint
            )] = fields.pop(vol.Required(CONF_FINGERPRINT))
        return vol.Schema(fields)
