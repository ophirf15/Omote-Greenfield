"""Sensors for Omote Remote."""

from __future__ import annotations

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator = hass.data[DOMAIN][entry.entry_id]["coordinator"]
    async_add_entities([OmoteWifiSensor(coordinator, entry)])


class OmoteWifiSensor(CoordinatorEntity, SensorEntity):
    _attr_has_entity_name = True
    _attr_name = "WiFi RSSI"

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{entry.entry_id}_wifi_rssi"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.entry_id)},
            name="Omote Remote",
            manufacturer="Omote OS",
            model="ESP32",
        )

    @property
    def native_value(self):
        wifi = self.coordinator.data.get("wifi", {})
        return wifi.get("rssi") if wifi.get("connected") else None

    @property
    def native_unit_of_measurement(self):
        return "dBm"
