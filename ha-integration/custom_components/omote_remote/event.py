"""Button press events from Omote Remote."""

from __future__ import annotations

from homeassistant.components.event import EventEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.event import async_track_time_interval
from datetime import timedelta

from .const import DOMAIN


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    api = hass.data[DOMAIN][entry.entry_id]["api"]
    async_add_entities([OmoteButtonEvent(api, entry)])


class OmoteButtonEvent(EventEntity):
    _attr_has_entity_name = True
    _attr_name = "Button press"
    _attr_event_types = ["button_press"]

    def __init__(self, api, entry: ConfigEntry) -> None:
        self._api = api
        self._attr_unique_id = f"{entry.entry_id}_button_event"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.entry_id)},
            name="Omote Remote",
            manufacturer="Omote OS",
            model="ESP32",
        )
        self._last_ts = 0

    async def async_added_to_hass(self) -> None:
        @callback
        def _poll(_):
            self.hass.async_create_task(self._async_poll())

        self.async_on_remove(
            async_track_time_interval(self.hass, _poll, timedelta(seconds=2))
        )

    async def _async_poll(self) -> None:
        try:
            data = await self._api.get_last_event()
        except Exception:
            return
        ts = data.get("ts", 0)
        if ts and ts != self._last_ts and data.get("button_id"):
            self._last_ts = ts
            self._trigger_event(
                "button_press",
                {
                    "button_id": data.get("button_id"),
                    "page_id": data.get("page_id"),
                    "action_type": data.get("action_type"),
                },
            )
            self.async_write_ha_state()
