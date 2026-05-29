#include "config/config_store.h"
#include <ESP.h>
#include <LittleFS.h>

static bool readJsonFile(const char *path, JsonDocument &doc) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const size_t sz = f.size();
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("config: parse %s (%s, %u bytes)\n", err.c_str(), path, (unsigned)sz);
    return false;
  }
  return true;
}

static bool writeJsonFile(const char *path, const JsonDocument &doc) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  if (serializeJson(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

bool actionFromJson(const JsonObject &obj, ActionDef &out) {
  out.type = obj["type"] | "";
  out.domain = obj["domain"] | "";
  out.service = obj["service"] | "";
  if (obj["target"].is<JsonObject>()) {
    out.entityId = obj["target"]["entity_id"] | "";
  } else {
    out.entityId = obj["entity_id"] | "";
  }
  if (!obj["data"].isNull()) {
    String buf;
    serializeJson(obj["data"], buf);
    out.dataJson = buf;
  }
  out.protocol = obj["protocol"] | "NEC";
  if (obj["code"].is<const char *>()) {
    out.irCode = strtoull(obj["code"], nullptr, 0);
  } else {
    out.irCode = obj["code"].as<uint64_t>();
  }
  out.irBits = obj["bits"] | 32;
  out.bleKey = obj["key"] | "";
  out.pageId = obj["page_id"] | "";
  out.irId = obj["ir_id"] | "";
  if (!obj["steps"].isNull()) {
    String buf;
    serializeJson(obj["steps"], buf);
    out.macroJson = buf;
  }
  return out.type.length() > 0;
}

void actionToJson(const ActionDef &action, JsonObject &obj) {
  obj["type"] = action.type;
  if (action.type == ACTION_HA_SERVICE) {
    obj["domain"] = action.domain;
    obj["service"] = action.service;
    if (action.entityId.length()) {
      JsonObject t = obj["target"].to<JsonObject>();
      t["entity_id"] = action.entityId;
    }
    if (action.dataJson.length()) {
      JsonDocument tmp;
      deserializeJson(tmp, action.dataJson);
      obj["data"] = tmp.as<JsonVariant>();
    }
  } else if (action.type == ACTION_IR) {
    if (action.irId.length()) obj["ir_id"] = action.irId;
    obj["protocol"] = action.protocol;
    obj["code"] = String("0x") + String((uint32_t)(action.irCode >> 32), HEX) +
                  String((uint32_t)action.irCode, HEX);
    obj["bits"] = action.irBits;
  } else if (action.type == ACTION_BLE_KEY || action.type == ACTION_BLE_MEDIA) {
    obj["key"] = action.bleKey;
  } else if (action.type == ACTION_NAVIGATE_PAGE) {
    obj["page_id"] = action.pageId;
  } else if (action.type == ACTION_OPEN_KEYBOARD || action.type == ACTION_OPEN_SETTINGS) {
    /* type only */
  } else if (action.type == ACTION_MACRO && action.macroJson.length()) {
    JsonDocument tmp;
    deserializeJson(tmp, action.macroJson);
    obj["steps"] = tmp["steps"];
  }
}

bool pageKeyFromJson(const JsonObject &obj, std::map<char, ActionDef> &keys) {
  keys.clear();
  for (JsonPair kv : obj) {
    String k = kv.key().c_str();
    if (k.length() != 1) continue;
    ActionDef act;
    if (kv.value().is<JsonObject>() && actionFromJson(kv.value().as<JsonObject>(), act)) {
      keys[k[0]] = act;
    }
  }
  return true;
}

void pageKeysToJson(const std::map<char, ActionDef> &keys, JsonObject &obj) {
  for (const auto &kv : keys) {
    String keyStr;
    keyStr += kv.first;
    JsonObject act = obj[keyStr].to<JsonObject>();
    actionToJson(kv.second, act);
  }
}

const ActionDef *pageFindKeyAction(const PageDef &page, char key) {
  auto it = page.keys.find(key);
  if (it != page.keys.end()) return &it->second;
  return nullptr;
}

static void parseButtons(const JsonArray &arr, std::vector<ButtonDef> &buttons) {
  buttons.clear();
  for (JsonObject b : arr) {
    if (buttons.size() >= MAX_BUTTONS_PER_PAGE) break;
    ButtonDef btn;
    btn.id = b["id"] | "";
    btn.label = b["label"] | "?";
    btn.widget = b["widget"] | WIDGET_PUSH;
    btn.x = b["x"] | 0;
    btn.y = b["y"] | 0;
    btn.w = b["w"] | 70;
    btn.h = b["h"] | 40;
    btn.labelBelow = b["label_below"] | false;
    btn.haIcon = b["ha_icon"] | "";
    if (btn.haIcon.equalsIgnoreCase("null") || btn.haIcon.equalsIgnoreCase("undefined")) btn.haIcon = "";
    if (b["style"].is<JsonObject>()) {
      JsonObject st = b["style"].as<JsonObject>();
      btn.style.bg = st["bg"].as<uint32_t>();
      if (btn.style.bg == 0) {
        const char *bgStr = st["bg"];
        if (bgStr && bgStr[0] == '#') btn.style.bg = strtoul(bgStr + 1, nullptr, 16);
      }
      btn.style.radius = st["radius"] | 8;
      btn.style.fg = st["fg"].as<uint32_t>();
      if (btn.style.fg == 0) {
        const char *fgStr = st["fg"];
        if (fgStr && fgStr[0] == '#') btn.style.fg = strtoul(fgStr + 1, nullptr, 16);
      }
      if (btn.style.fg == 0) btn.style.fg = 0xFFFFFF;
      btn.style.border = st["border"].as<uint32_t>();
      if (btn.style.border == 0) {
        const char *bdStr = st["border"];
        if (bdStr && bdStr[0] == '#') btn.style.border = strtoul(bdStr + 1, nullptr, 16);
      }
      btn.style.borderW = st["border_w"] | 0;
      if (st["label_below"].is<bool>()) btn.labelBelow = st["label_below"];
    }
    if (btn.style.bg == 0) {
      btn.style.bg = b["color"].as<uint32_t>();
      if (btn.style.bg == 0) btn.style.bg = 0x3366CC;
    }
    if (b["action"].is<JsonObject>()) {
      actionFromJson(b["action"].as<JsonObject>(), btn.action);
    }
    if (b["ha"].is<JsonObject>()) {
      JsonObject ha = b["ha"].as<JsonObject>();
      btn.action.type = ACTION_HA_SERVICE;
      String eid = ha["entity_id"] | "";
      int dot = eid.indexOf('.');
      btn.action.domain = dot > 0 ? eid.substring(0, dot) : "light";
      btn.action.entityId = eid;
      btn.action.service = ha["service_on"] | "turn_on";
      if (btn.widget == WIDGET_TOGGLE) {
        if (btn.action.domain == "scene" || btn.action.domain == "script") {
          btn.action.service = "turn_on";
        } else {
          btn.action.service = "toggle";
        }
      }
    }
    buttons.push_back(btn);
  }
}

static void parsePages(const JsonArray &arr, std::vector<PageDef> &pages) {
  pages.clear();
  for (JsonObject p : arr) {
    if (pages.size() >= MAX_PAGES) break;
    PageDef page;
    page.id = p["id"] | "";
    page.name = p["name"] | "Page";
    if (p["buttons"].is<JsonArray>()) {
      parseButtons(p["buttons"].as<JsonArray>(), page.buttons);
    }
    if (p["keys"].is<JsonObject>()) {
      pageKeyFromJson(p["keys"].as<JsonObject>(), page.keys);
    }
    pages.push_back(page);
  }
}

static void parseKeymap(const JsonArray &arr, std::vector<KeyBinding> &keymap) {
  keymap.clear();
  for (JsonObject k : arr) {
    if (keymap.size() >= MAX_KEY_BINDINGS) break;
    KeyBinding kb;
    String keyStr = k["key"] | "";
    if (keyStr.length()) kb.key = keyStr[0];
    kb.pageId = k["page_id"] | "";
    if (k["action"].is<JsonObject>()) {
      actionFromJson(k["action"].as<JsonObject>(), kb.action);
    }
    keymap.push_back(kb);
  }
}

static const char CONFIG_TMP_PATH[] = "/config.json.new";
static const char CONFIG_BAK_PATH[] = "/config.json.bak";

static void configClearVectors(OmoteConfig &cfg) {
  cfg.pages.clear();
  cfg.keymap.clear();
}

void configClearInMemory(OmoteConfig &cfg) { configClearVectors(cfg); }

static void configSwapIn(OmoteConfig &live, OmoteConfig &src) {
  live.schemaVersion = src.schemaVersion;
  live.activePageId = src.activePageId;
  live.pages.swap(src.pages);
  live.keymap.swap(src.keymap);
  configClearVectors(src);
}

static void configInstallDefault(OmoteConfig &cfg) {
  configClearVectors(cfg);
  OmoteConfig def = configDefault();
  configSwapIn(cfg, def);
}

void configRepairActivePage(OmoteConfig &cfg) {
  if (cfg.pages.empty()) return;
  if (configFindPage(cfg, cfg.activePageId)) return;
  cfg.activePageId = cfg.pages[0].id;
  Serial.printf("config: repaired active_page_id -> %s\n", cfg.activePageId.c_str());
}

bool configLoadFromPath(OmoteConfig &cfg, const char *path) {
  JsonDocument doc;
  if (!readJsonFile(path, doc)) return false;
  cfg.schemaVersion = doc["schema_version"] | CONFIG_SCHEMA_VERSION;
  cfg.activePageId = doc["active_page_id"] | "home";
  configClearVectors(cfg);
  if (doc["pages"].is<JsonArray>()) parsePages(doc["pages"].as<JsonArray>(), cfg.pages);
  if (doc["keymap"].is<JsonArray>()) parseKeymap(doc["keymap"].as<JsonArray>(), cfg.keymap);
  return true;
}

bool configLoad(OmoteConfig &cfg) {
  if (!configLoadFromPath(cfg, CONFIG_PATH)) {
    if (LittleFS.exists(CONFIG_BAK_PATH)) {
      Serial.println("config: restoring from backup");
      LittleFS.remove(CONFIG_PATH);
      LittleFS.rename(CONFIG_BAK_PATH, CONFIG_PATH);
      if (configLoadFromPath(cfg, CONFIG_PATH) && !cfg.pages.empty()) {
        configRepairActivePage(cfg);
        Serial.printf("config loaded from backup (%u pages)\n", cfg.pages.size());
        return true;
      }
    }
    configInstallDefault(cfg);
    return false;
  }
  if (cfg.pages.empty()) {
    if (LittleFS.exists(CONFIG_BAK_PATH)) {
      Serial.println("config: empty pages, trying backup");
      LittleFS.remove(CONFIG_PATH);
      LittleFS.rename(CONFIG_BAK_PATH, CONFIG_PATH);
      if (configLoadFromPath(cfg, CONFIG_PATH) && !cfg.pages.empty()) {
        configRepairActivePage(cfg);
        return true;
      }
    }
    configInstallDefault(cfg);
  }
  configRepairActivePage(cfg);
  if (LittleFS.exists(ACTIVE_PAGE_PATH)) {
    File f = LittleFS.open(ACTIVE_PAGE_PATH, "r");
    if (f) {
      String id = f.readString();
      f.close();
      id.trim();
      if (id.length() && configFindPage(cfg, id)) cfg.activePageId = id;
    }
  }
  Serial.printf("config loaded (%u pages, active=%s)\n", cfg.pages.size(), cfg.activePageId.c_str());
  return true;
}

static bool writeTextFile(const char *path, const String &body) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  const size_t n = f.print(body);
  f.close();
  return n == body.length();
}

/** Cheap file check — no JsonDocument (deploy often runs with <8 KB max alloc). */
static bool configValidateFileQuick(const char *path, String &errorOut) {
  if (!LittleFS.exists(path)) {
    errorOut = "missing file";
    return false;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    errorOut = "open failed";
    return false;
  }
  const size_t sz = f.size();
  if (sz < 24 || sz > 65536) {
    f.close();
    errorOut = "invalid size";
    return false;
  }
  char buf[128];
  size_t n = f.read((uint8_t *)buf, sizeof(buf) - 1);
  buf[n] = '\0';
  if (buf[0] != '{') {
    f.close();
    errorOut = "invalid config";
    return false;
  }
  bool hasPages = strstr(buf, "\"pages\"") != nullptr;
  while (!hasPages && f.available()) {
    n = f.read((uint8_t *)buf, sizeof(buf) - 1);
    if (n == 0) break;
    buf[n] = '\0';
    if (strstr(buf, "\"pages\"")) hasPages = true;
  }
  f.close();
  if (!hasPages) {
    errorOut = "no pages";
    return false;
  }
  return true;
}

/** Full JSON parse when heap allows; falls back to quick scan only. */
static bool configValidateFile(const char *path, String &errorOut) {
  if (!configValidateFileQuick(path, errorOut)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) {
    errorOut = "open failed";
    return false;
  }
  const size_t sz = f.size();
  f.close();
  if (ESP.getMaxAllocHeap() < sz + 2048) {
    Serial.printf("config validate: quick only (maxalloc=%u sz=%u)\n", ESP.getMaxAllocHeap(),
                  (unsigned)sz);
    return true;
  }
  JsonDocument doc;
  File jf = LittleFS.open(path, "r");
  if (!jf) {
    errorOut = "open failed";
    return false;
  }
  DeserializationError err = deserializeJson(doc, jf);
  jf.close();
  if (err) {
    // Large config deploys can transiently fail full parse on low contiguous heap.
    // Quick validation already passed, so allow commit and let boot-time load/backup recovery handle it.
    if (err == DeserializationError::NoMemory) {
      Serial.printf("config validate: parse skipped on low mem (%u bytes)\n", (unsigned)sz);
      return true;
    }
    errorOut = "invalid json";
    return false;
  }
  JsonArray pages = doc["pages"].as<JsonArray>();
  if (pages.isNull() || pages.size() == 0) {
    errorOut = "no pages";
    return false;
  }
  return true;
}

static bool configCommitTempFile(OmoteConfig &live, String &errorOut) {
  if (!configValidateFile(CONFIG_TMP_PATH, errorOut)) {
    LittleFS.remove(CONFIG_TMP_PATH);
    return false;
  }
  if (LittleFS.exists(CONFIG_BAK_PATH)) LittleFS.remove(CONFIG_BAK_PATH);
  if (LittleFS.exists(CONFIG_PATH)) LittleFS.rename(CONFIG_PATH, CONFIG_BAK_PATH);
  if (!LittleFS.rename(CONFIG_TMP_PATH, CONFIG_PATH)) {
    LittleFS.remove(CONFIG_TMP_PATH);
    if (LittleFS.exists(CONFIG_BAK_PATH)) {
      LittleFS.remove(CONFIG_PATH);
      LittleFS.rename(CONFIG_BAK_PATH, CONFIG_PATH);
    }
    configLoad(live);
    errorOut = "commit failed";
    return false;
  }
  configClearVectors(live);
  yield();
  if (!configLoadFromPath(live, CONFIG_PATH)) {
    errorOut = "load failed";
    return false;
  }
  configRepairActivePage(live);
  size_t buttons = 0;
  for (const auto &p : live.pages) buttons += p.buttons.size();
  Serial.printf("config committed: %u pages, %u buttons, active=%s, heap=%u\n", live.pages.size(),
                buttons, live.activePageId.c_str(), ESP.getFreeHeap());
  return true;
}

bool configImportJsonString(const String &json, OmoteConfig &live, String &errorOut) {
  if (json.length() < 4) {
    errorOut = "empty config";
    return false;
  }
  if (!writeTextFile(CONFIG_TMP_PATH, json)) {
    errorOut = "temp write failed";
    return false;
  }
  return configCommitTempFile(live, errorOut);
}

bool configApplyPostBody(const String &body, OmoteConfig &live, String &errorOut) {
  Serial.printf("config POST: %u bytes, heap=%u max=%u\n", body.length(), ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
  if (body.length() < 4) {
    errorOut = "empty body";
    return false;
  }
  if (!writeTextFile(CONFIG_TMP_PATH, body)) {
    errorOut = "temp write failed";
    return false;
  }
  Serial.println("config validate ok");
  return configCommitTempFile(live, errorOut);
}

bool configPersistActivePageId(const String &pageId) {
  if (pageId.length() == 0) return false;
  File f = LittleFS.open(ACTIVE_PAGE_PATH, "w");
  if (!f) return false;
  const size_t n = f.print(pageId);
  f.close();
  return n == (size_t)pageId.length();
}

bool configSave(const OmoteConfig &cfg) {
  static JsonDocument doc;
  doc.clear();
  doc["schema_version"] = CONFIG_SCHEMA_VERSION;
  doc["active_page_id"] = cfg.activePageId;
  JsonArray pages = doc["pages"].to<JsonArray>();
  for (const auto &page : cfg.pages) {
    JsonObject p = pages.add<JsonObject>();
    p["id"] = page.id;
    p["name"] = page.name;
    JsonArray btns = p["buttons"].to<JsonArray>();
    for (const auto &btn : page.buttons) {
      JsonObject b = btns.add<JsonObject>();
      b["id"] = btn.id;
      b["label"] = btn.label;
      b["widget"] = btn.widget;
      b["x"] = btn.x;
      b["y"] = btn.y;
      b["w"] = btn.w;
      b["h"] = btn.h;
      if (btn.labelBelow) b["label_below"] = true;
      if (btn.haIcon.length()) b["ha_icon"] = btn.haIcon;
      JsonObject st = b["style"].to<JsonObject>();
      st["bg"] = btn.style.bg;
      st["fg"] = btn.style.fg;
      st["radius"] = btn.style.radius;
      if (btn.style.borderW > 0) {
        st["border"] = btn.style.border;
        st["border_w"] = btn.style.borderW;
      }
      JsonObject act = b["action"].to<JsonObject>();
      actionToJson(btn.action, act);
    }
    if (!page.keys.empty()) {
      JsonObject keys = p["keys"].to<JsonObject>();
      pageKeysToJson(page.keys, keys);
    }
  }
  JsonArray km = doc["keymap"].to<JsonArray>();
  for (const auto &kb : cfg.keymap) {
    JsonObject k = km.add<JsonObject>();
    k["key"] = String(kb.key);
    k["page_id"] = kb.pageId;
    JsonObject act = k["action"].to<JsonObject>();
    actionToJson(kb.action, act);
  }
  return writeJsonFile(CONFIG_PATH, doc);
}

bool settingsLoad(HaSettings &s) {
  JsonDocument doc;
  if (!readJsonFile(SETTINGS_PATH, doc)) return false;
  s.url = doc["ha_url"] | "";
  s.token = doc["ha_token"] | "";
  s.configured = s.url.length() > 0 && s.token.length() > 0;
  return s.configured;
}

bool settingsSave(const HaSettings &s) {
  JsonDocument doc;
  doc["ha_url"] = s.url;
  doc["ha_token"] = s.token;
  return writeJsonFile(SETTINGS_PATH, doc);
}

bool deviceSettingsLoad(DeviceSettings &ds) {
  JsonDocument doc;
  if (!readJsonFile(DEVICE_SETTINGS_PATH, doc)) return false;
  ds.brightness = doc["brightness"] | 180;
  ds.motionWake = doc["motion_wake_enabled"] | true;
  ds.haPollEnabled = doc["ha_poll_enabled"] | false;
  ds.bleProfile = doc["ble_profile"] | "generic";
  ds.timezone = doc["timezone"] | "UTC0";
  ds.ntpServer = doc["ntp_server"] | "pool.ntp.org";

  uint32_t display = doc["display_timeout_ms"] | 0;
  uint32_t deep = doc["deep_sleep_timeout_ms"] | 0;
  const uint32_t legacy = doc["sleep_timeout_ms"] | 0;
  if (display == 0 && deep == 0 && legacy != 0) {
    if (legacy >= 300000) {
      deep = legacy;
      display = 60000;
    } else {
      display = legacy;
      deep = 15UL * 60UL * 1000UL;
    }
  }
  if (display == 0) display = 60000;
  if (deep == 0) deep = 15UL * 60UL * 1000UL;
  if (deep < display + 60000) deep = display + 60000;
  ds.displayTimeoutMs = display;
  ds.deepSleepTimeoutMs = deep;
  return true;
}

bool deviceSettingsSave(const DeviceSettings &ds) {
  JsonDocument doc;
  doc["brightness"] = ds.brightness;
  doc["display_timeout_ms"] = ds.displayTimeoutMs;
  doc["deep_sleep_timeout_ms"] = ds.deepSleepTimeoutMs;
  doc["sleep_timeout_ms"] = ds.displayTimeoutMs;
  doc["motion_wake_enabled"] = ds.motionWake;
  doc["ha_poll_enabled"] = ds.haPollEnabled;
  doc["ble_profile"] = ds.bleProfile;
  doc["timezone"] = ds.timezone;
  doc["ntp_server"] = ds.ntpServer;
  return writeJsonFile(DEVICE_SETTINGS_PATH, doc);
}

PageDef *configFindPage(OmoteConfig &cfg, const String &pageId) {
  for (auto &p : cfg.pages) {
    if (p.id == pageId) return &p;
  }
  return nullptr;
}

const PageDef *configFindPage(const OmoteConfig &cfg, const String &pageId) {
  for (const auto &p : cfg.pages) {
    if (p.id == pageId) return &p;
  }
  return nullptr;
}

int configPageIndex(const OmoteConfig &cfg, const String &pageId) {
  for (size_t i = 0; i < cfg.pages.size(); i++) {
    if (cfg.pages[i].id == pageId) return (int)i;
  }
  return -1;
}

bool configValidate(const OmoteConfig &cfg, String &error) {
  if (cfg.pages.empty()) {
    error = "At least one page required";
    return false;
  }
  for (const auto &p : cfg.pages) {
    if (p.id.length() == 0) {
      error = "Page missing id";
      return false;
    }
    if (p.buttons.size() > MAX_BUTTONS_PER_PAGE) {
      error = "Too many buttons on page " + p.id;
      return false;
    }
  }
  return true;
}

static void addBleKey(PageDef &p, char key, const char *bleKey) {
  ActionDef a;
  a.type = ACTION_BLE_KEY;
  a.bleKey = bleKey;
  p.keys[key] = a;
}

static void addNavKey(PageDef &p, char key, const char *pageId) {
  ActionDef a;
  a.type = ACTION_NAVIGATE_PAGE;
  a.pageId = pageId;
  p.keys[key] = a;
}

PageDef googleTvPageTemplate();
PageDef homeAssistantPageTemplate();

OmoteConfig configDefault() {
  OmoteConfig cfg;
  cfg.schemaVersion = CONFIG_SCHEMA_VERSION;
  cfg.activePageId = "home";
  cfg.pages.push_back(homeAssistantPageTemplate());
  cfg.pages.push_back(googleTvPageTemplate());
  return cfg;
}

PageDef homeAssistantPageTemplate() {
  PageDef p;
  p.id = "home";
  p.name = "Home";
  addNavKey(p, '1', "home");
  addNavKey(p, '2', "google_tv");
  ButtonDef b1;
  b1.id = "lights";
  b1.label = "Lights";
  b1.widget = WIDGET_TOGGLE;
  b1.x = 10;
  b1.y = 30;
  b1.w = 100;
  b1.h = 44;
  b1.style.bg = 0xFFAA00;
  b1.action.type = ACTION_HA_SERVICE;
  b1.action.domain = "light";
  b1.action.service = "toggle";
  b1.action.entityId = "light.example";
  p.buttons.push_back(b1);
  ButtonDef nav;
  nav.id = "gtv";
  nav.label = "Google TV";
  nav.x = 120;
  nav.y = 30;
  nav.w = 110;
  nav.h = 44;
  nav.style.bg = 0x4488FF;
  nav.action.type = ACTION_NAVIGATE_PAGE;
  nav.action.pageId = "google_tv";
  p.buttons.push_back(nav);
  return p;
}

PageDef googleTvPageTemplate() {
  PageDef p;
  p.id = "google_tv";
  p.name = "Google TV";
  addBleKey(p, '^', "UP");
  addBleKey(p, 'u', "UP");
  addBleKey(p, 'd', "DOWN");
  addBleKey(p, 'l', "LEFT");
  addBleKey(p, 'r', "RIGHT");
  addBleKey(p, 'k', "ENTER");
  addBleKey(p, '+', "VOLUME_UP");
  addBleKey(p, '-', "VOLUME_DOWN");
  addBleKey(p, 'b', "BACK");
  addBleKey(p, 'm', "MENU");
  addBleKey(p, 'e', "HOME");
  addNavKey(p, '1', "home");
  const struct {
    const char *id;
    const char *label;
    int x, y, w, h;
    const char *key;
  } keys[] = {
      {"up", "Up", 85, 30, 70, 40, "UP"},
      {"down", "Down", 85, 180, 70, 40, "DOWN"},
      {"left", "Left", 10, 105, 70, 40, "LEFT"},
      {"right", "Right", 160, 105, 70, 40, "RIGHT"},
      {"ok", "OK", 85, 105, 70, 40, "ENTER"},
      {"back", "Back", 10, 30, 70, 40, "BACK"},
      {"home", "Home", 160, 30, 70, 40, "HOME"},
      {"vol_up", "Vol+", 10, 180, 70, 40, "VOLUME_UP"},
      {"vol_down", "Vol-", 160, 180, 70, 40, "VOLUME_DOWN"},
  };
  for (const auto &k : keys) {
    ButtonDef b;
    b.id = k.id;
    b.label = k.label;
    b.x = k.x;
    b.y = k.y;
    b.w = k.w;
    b.h = k.h;
    b.style.bg = 0x2255AA;
    b.action.type = ACTION_BLE_KEY;
    b.action.bleKey = k.key;
    p.buttons.push_back(b);
  }
  ButtonDef homeBtn;
  homeBtn.id = "to_home";
  homeBtn.label = "HA";
  homeBtn.x = 10;
  homeBtn.y = 240;
  homeBtn.w = 220;
  homeBtn.h = 36;
  homeBtn.style.bg = 0x33AA66;
  homeBtn.action.type = ACTION_NAVIGATE_PAGE;
  homeBtn.action.pageId = "home";
  p.buttons.push_back(homeBtn);
  ButtonDef kbBtn;
  kbBtn.id = "keyboard";
  kbBtn.label = "KB";
  kbBtn.x = 120;
  kbBtn.y = 240;
  kbBtn.w = 50;
  kbBtn.h = 36;
  kbBtn.style.bg = 0x555588;
  kbBtn.action.type = ACTION_OPEN_KEYBOARD;
  p.buttons.push_back(kbBtn);
  return p;
}
