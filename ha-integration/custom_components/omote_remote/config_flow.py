"""Config flow for Omote Remote."""

from __future__ import annotations

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.const import CONF_HOST
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import DOMAIN
from .omote_api import OmoteApi

DATA_SCHEMA = vol.Schema({CONF_HOST: str})


class OmoteConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            return await self._create_entry(user_input[CONF_HOST], errors)
        return self.async_show_form(step_id="user", data_schema=DATA_SCHEMA, errors=errors)

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo):
        await self.async_set_unique_id(discovery_info.hostname.split(".")[0])
        self._abort_if_unique_id_configured()
        host = f"http://{discovery_info.host}:{discovery_info.port or 80}"
        self.context["discovered_host"] = host
        self.context["title_placeholders"] = {"name": discovery_info.name or "Omote"}
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(self, user_input=None):
        host = self.context.get("discovered_host", "")
        if user_input is not None:
            host = user_input.get(CONF_HOST, host)
        if user_input is None:
            return self.async_show_form(
                step_id="zeroconf_confirm",
                data_schema=vol.Schema({vol.Required(CONF_HOST, default=host): str}),
                description_placeholders=self.context.get("title_placeholders", {}),
            )
        return await self._create_entry(host, {})

    async def _create_entry(self, host: str, errors: dict):
        session = async_get_clientsession(self.hass)
        api = OmoteApi(host, session)
        try:
            await api.get_status()
        except Exception:
            errors["base"] = "cannot_connect"
            return self.async_show_form(step_id="user", data_schema=DATA_SCHEMA, errors=errors)

        await self.async_set_unique_id(host)
        self._abort_if_unique_id_configured()
        return self.async_create_entry(title="Omote Remote", data={CONF_HOST: host})
