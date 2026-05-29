#pragma once

#include "config/config_schema.h"
#include <ArduinoJson.h>
#include <map>
#include <vector>

struct HaSettings {
  String url;
  String token;
  bool configured = false;
};

struct ButtonStyle {
  uint32_t bg = 0x3366CC;
  uint32_t fg = 0xFFFFFF;
  uint32_t border = 0x000000;
  uint8_t radius = 8;
  uint8_t borderW = 0;
};

struct ActionDef {
  String type;
  String domain;
  String service;
  String entityId;
  String dataJson;
  String protocol;
  uint64_t irCode = 0;
  uint16_t irBits = 32;
  String bleKey;
  String pageId;
  String macroJson;
  /** Reference to /irlib.json entry (optional). */
  String irId;
};

struct ButtonDef {
  String id;
  String label;
  String widget = WIDGET_PUSH;
  ButtonStyle style;
  /** Place caption under the control (toggle/switch/climate). */
  bool labelBelow = false;
  /** mdi:icon-name from HA (editor + optional device hint). */
  String haIcon;
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 70;
  int16_t h = 40;
  ActionDef action;
};

struct PageDef {
  String id;
  String name;
  std::vector<ButtonDef> buttons;
  std::map<char, ActionDef> keys;
};

struct KeyBinding {
  char key;
  String pageId;
  ActionDef action;
};

struct DeviceSettings {
  uint8_t brightness = 180;
  /** Screen off after this idle period; WiFi stays on. */
  uint32_t displayTimeoutMs = 60000;
  /** esp_deep_sleep after this idle period (min ~15 min recommended). */
  uint32_t deepSleepTimeoutMs = 15UL * 60UL * 1000UL;
  bool motionWake = true;
  bool haPollEnabled = false;
  /**
   * Selects the BLE HID "personality" — controls which Vendor_VVVV_Product_PPPP.kl
   * Android loads, which in turn determines which KEYCODE_* the TV reports.
   * See bleIdentityList() in ble_hal.cpp for the catalogue. Changing this
   * value invalidates existing bonds (Android caches the descriptor per bond),
   * so the user must forget+re-pair on both ends.
   */
  String bleProfile = "generic";
  /** POSIX TZ string, e.g. "PST8PDT,M3.2.0,M11.1.0". */
  String timezone = "UTC0";
  String ntpServer = "pool.ntp.org";
};

struct OmoteConfig {
  int schemaVersion = CONFIG_SCHEMA_VERSION;
  String activePageId;
  std::vector<PageDef> pages;
  std::vector<KeyBinding> keymap;
};

bool configLoad(OmoteConfig &cfg);
bool configLoadFromPath(OmoteConfig &cfg, const char *path);
/** Drop pages/keymap in RAM (disk file unchanged until configSave). */
void configClearInMemory(OmoteConfig &cfg);
/** Commit JSON text to /config.json and reload into RAM (same path as Deploy). */
bool configImportJsonString(const String &json, OmoteConfig &live, String &errorOut);
void configRepairActivePage(OmoteConfig &cfg);
/** Write POST body to disk, validate, commit; reboot to load into RAM. */
bool configApplyPostBody(const String &body, OmoteConfig &live, String &errorOut);
bool configSave(const OmoteConfig &cfg);
bool settingsLoad(HaSettings &s);
bool settingsSave(const HaSettings &s);
bool deviceSettingsLoad(DeviceSettings &ds);
bool deviceSettingsSave(const DeviceSettings &ds);

OmoteConfig configDefault();
PageDef *configFindPage(OmoteConfig &cfg, const String &pageId);
const PageDef *configFindPage(const OmoteConfig &cfg, const String &pageId);
int configPageIndex(const OmoteConfig &cfg, const String &pageId);
bool configValidate(const OmoteConfig &cfg, String &error);

bool actionFromJson(const JsonObject &obj, ActionDef &out);
void actionToJson(const ActionDef &action, JsonObject &obj);
bool pageKeyFromJson(const JsonObject &obj, std::map<char, ActionDef> &keys);
void pageKeysToJson(const std::map<char, ActionDef> &keys, JsonObject &obj);

const ActionDef *pageFindKeyAction(const PageDef &page, char key);
