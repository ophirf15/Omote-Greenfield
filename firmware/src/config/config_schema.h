#pragma once

#include <Arduino.h>

#define CONFIG_PATH "/config.json"
#define SETTINGS_PATH "/settings.json"
#define DEVICE_SETTINGS_PATH "/device_settings.json"
#define CONFIG_SCHEMA_VERSION 2

#define ACTION_HA_SERVICE "ha_service"
#define ACTION_IR "ir"
#define ACTION_BLE_KEY "ble_key"
#define ACTION_BLE_MEDIA "ble_media"
#define ACTION_NAVIGATE_PAGE "navigate_page"
#define ACTION_MACRO "macro"
#define ACTION_OPEN_SETTINGS "open_settings"
#define ACTION_OPEN_KEYBOARD "open_keyboard"

#define WIDGET_PUSH "push"
#define WIDGET_TOGGLE "toggle"
#define WIDGET_MOMENTARY "momentary"
#define WIDGET_LABEL "label"
#define WIDGET_CLIMATE "climate"
#define WIDGET_CLIMATE_THERMOSTAT "climate_thermostat"

#define MAX_PAGES 8
#define MAX_BUTTONS_PER_PAGE 24
#define MAX_KEY_BINDINGS 40
#define MAX_MACRO_STEPS 8

#define STATUS_BAR_H 22
#define CONTENT_Y_OFFSET 22
