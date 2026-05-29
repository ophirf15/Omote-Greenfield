#include "net/http_server.h"
#include "config/config_schema.h"
#include "config/config_store.h"
#include "config/ir_library.h"
#include "hal/battery_hal.h"
#include "hal/ble_hal.h"
#include "hal/display_hal.h"
#include "hal/ir_hal.h"
#include "hal/sleep_hal.h"
#include "hal/wifi_hal.h"
#include "net/debug_log.h"
#include "net/ha_client.h"
#include "net/net_worker.h"
#include "net/time_sync.h"
#include "ui_runtime/page_engine.h"
#include <ESP.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESPmDNS.h>

static WebServer server(80);
static HaSettings *gSettings = nullptr;
static OmoteConfig *gConfig = nullptr;
static DeviceSettings *gDevSettings = nullptr;
static ConfigChangedCallback gOnChange;
static bool gServerUp = false;

static void httpCloseClient() {
  WiFiClient client = server.client();
  if (client && client.connected()) client.stop();
}

static void sendJson(int code, const String &body) {
  netWorkerTouchWebClient();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");
  if (body.length() == 0) {
    server.send(code, "application/json", "{}");
  } else {
    server.send(code, "application/json", body);
  }
  httpCloseClient();
}

static String gLastEventJson = "{}";
static String gLastKeyJson = "{\"key\":\"\",\"pressed\":false}";

void httpServerPublishEvent(const char *buttonId, const char *pageId, const char *actionType) {
  JsonDocument doc;
  doc["button_id"] = buttonId;
  doc["page_id"] = pageId;
  doc["action_type"] = actionType;
  doc["ts"] = millis();
  serializeJson(doc, gLastEventJson);
}

String httpServerLastEventJson() { return gLastEventJson; }

void httpServerPublishKey(char key, bool pressed) {
  JsonDocument doc;
  doc["key"] = String(key);
  doc["pressed"] = pressed;
  doc["ts"] = millis();
  serializeJson(doc, gLastKeyJson);
}

String httpServerLastKeyJson() { return gLastKeyJson; }

static String mimeForPath(const String &path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  return "text/plain";
}

static void handleStatic() {
  netWorkerTouchWebClient();
  String path = server.uri();
  if (path == "/") path = "/index.html";
  if (!LittleFS.exists(path)) {
    if (path == "/index.html" && LittleFS.exists("/setup.html")) {
      path = "/setup.html";
    } else {
      server.send(404, "text/plain", "Not found");
      return;
    }
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    server.send(500, "text/plain", "open failed");
    return;
  }
  const String mime = mimeForPath(path);
  if (path.endsWith(".html") || path.endsWith(".js") || path.endsWith(".css")) {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");
  server.sendHeader("Content-Type", mime);
  server.setContentLength(f.size());
  server.send(200);
  uint8_t buf[1024];
  WiFiClient client = server.client();
  while (f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    size_t sent = 0;
    while (sent < n) {
      const size_t w = client.write(buf + sent, n - sent);
      if (w == 0) break;
      sent += w;
    }
    yield();
  }
  f.close();
  httpCloseClient();
}

static void serializeConfig(JsonDocument &doc, const OmoteConfig &cfg) {
  doc["schema_version"] = cfg.schemaVersion;
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
      b["color"] = btn.style.bg;
      JsonObject st = b["style"].to<JsonObject>();
      st["bg"] = btn.style.bg;
      st["radius"] = btn.style.radius;
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
}

static void handleApiStatus() {
  String j = "{\"wifi\":";
  j += wifiStatusJson();
  j += ",\"ble\":";
  j += bleStatusJson();
  j += ",\"ha_configured\":";
  j += (gSettings && gSettings->configured) ? "true" : "false";
  j += ",\"battery_percent\":";
  j += batteryPercent();
  j += ",\"battery_charging\":";
  j += batteryCharging() ? "true" : "false";
  if (gConfig) {
    size_t buttons = 0;
    for (const auto &p : gConfig->pages) buttons += p.buttons.size();
    j += ",\"config_pages\":";
    j += gConfig->pages.size();
    j += ",\"config_buttons\":";
    j += buttons;
    j += ",\"config_active\":\"";
    j += gConfig->activePageId;
    j += "\"";
  }
  j += "}";
  sendJson(200, j);
}

static void handleSettingsGet() {
  JsonDocument doc;
  doc["ha_url"] = gSettings ? gSettings->url : "";
  doc["ha_token_set"] = (gSettings && gSettings->token.length() > 0);
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

static void handleSettingsPost() {
  if (!server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"missing body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJson(400, "{\"error\":\"invalid json\"}");
    return;
  }
  if (gSettings) {
    if (!doc["ha_url"].isNull()) gSettings->url = doc["ha_url"].as<String>();
    if (!doc["ha_token"].isNull()) gSettings->token = doc["ha_token"].as<String>();
    gSettings->configured = gSettings->url.length() && gSettings->token.length();
    settingsSave(*gSettings);
  }
  sendJson(200, "{\"ok\":true}");
}

static void handleSettingsTest() {
  String err;
  bool ok = gSettings && haTestConnection(*gSettings, err);
  if (ok) sendJson(200, "{\"ok\":true}");
  else sendJson(400, String("{\"ok\":false,\"error\":\"") + err + "\"}");
}

/**
 * Stream the backup directly from LittleFS instead of building a huge JsonDocument
 * in heap. Avoids OOM "failed to fetch" with large configs / IR libraries.
 */
static void handleBackupExport() {
  netWorkerTouchWebClient();

  /* Small metadata header: omote_backup_version, exported_at, ha, wifi, device_settings.
   * config + ir_library are streamed verbatim from disk below. */
  JsonDocument hdr;
  hdr["omote_backup_version"] = 1;
  hdr["exported_at"] = millis();
  if (gSettings) {
    JsonObject ha = hdr["ha"].to<JsonObject>();
    ha["ha_url"] = gSettings->url;
    ha["ha_token"] = gSettings->token;
  }
  WifiCreds wc;
  if (wifiLoadCreds(wc)) {
    JsonObject w = hdr["wifi"].to<JsonObject>();
    w["ssid"] = wc.ssid;
    w["password"] = wc.password;
  }
  if (gDevSettings) {
    JsonObject ds = hdr["device_settings"].to<JsonObject>();
    ds["brightness"] = gDevSettings->brightness;
    ds["display_timeout_ms"] = gDevSettings->displayTimeoutMs;
    ds["deep_sleep_timeout_ms"] = gDevSettings->deepSleepTimeoutMs;
    ds["sleep_timeout_ms"] = gDevSettings->displayTimeoutMs;
    ds["motion_wake_enabled"] = gDevSettings->motionWake;
    ds["ha_poll_enabled"] = gDevSettings->haPollEnabled;
  }
  String headerJson;
  serializeJson(hdr, headerJson);
  /* Strip trailing '}' — we'll append more keys + close at the end. */
  if (headerJson.length() && headerJson[headerJson.length() - 1] == '}') {
    headerJson.remove(headerJson.length() - 1);
  }

  /* Probe sizes so we can set an accurate Content-Length and avoid chunked encoding. */
  size_t cfgSize = 0;
  if (LittleFS.exists(CONFIG_PATH)) {
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (f) {
      cfgSize = f.size();
      f.close();
    }
  }
  size_t irSize = 0;
  if (LittleFS.exists(IRLIB_PATH)) {
    File f = LittleFS.open(IRLIB_PATH, "r");
    if (f) {
      irSize = f.size();
      f.close();
    }
  }

  static const char kCfgKey[] = ",\"config\":";
  static const char kIrKey[] = ",\"ir_library\":";
  static const char kEmptyArr[] = "[]";

  size_t total = headerJson.length();
  if (cfgSize > 0) total += sizeof(kCfgKey) - 1 + cfgSize;
  total += sizeof(kIrKey) - 1 + (irSize > 0 ? irSize : sizeof(kEmptyArr) - 1);
  total += 1;  /* closing '}' */

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Content-Type", "application/json");
  server.setContentLength(total);
  server.send(200);
  WiFiClient client = server.client();

  auto sendStr = [&](const char *s, size_t n) {
    if (!n) n = strlen(s);
    size_t sent = 0;
    while (sent < n) {
      const size_t w = client.write((const uint8_t *)s + sent, n - sent);
      if (w == 0) break;
      sent += w;
    }
  };

  sendStr(headerJson.c_str(), headerJson.length());

  if (cfgSize > 0) {
    sendStr(kCfgKey, sizeof(kCfgKey) - 1);
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (f) {
      uint8_t buf[512];
      while (f.available()) {
        const size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        sendStr((const char *)buf, n);
        yield();
      }
      f.close();
    }
  }

  sendStr(kIrKey, sizeof(kIrKey) - 1);
  if (irSize > 0) {
    File f = LittleFS.open(IRLIB_PATH, "r");
    if (f) {
      uint8_t buf[512];
      while (f.available()) {
        const size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        sendStr((const char *)buf, n);
        yield();
      }
      f.close();
    }
  } else {
    sendStr(kEmptyArr, sizeof(kEmptyArr) - 1);
  }
  sendStr("}", 1);
}

static void handleBackupImport() {
  if (!server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"missing body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJson(400, "{\"error\":\"invalid json\"}");
    return;
  }
  if (doc["ha"].is<JsonObject>() && gSettings) {
    JsonObject ha = doc["ha"].as<JsonObject>();
    if (!ha["ha_url"].isNull()) gSettings->url = ha["ha_url"].as<String>();
    if (!ha["ha_token"].isNull()) gSettings->token = ha["ha_token"].as<String>();
    gSettings->configured = gSettings->url.length() && gSettings->token.length();
    settingsSave(*gSettings);
  }
  if (doc["wifi"].is<JsonObject>()) {
    JsonObject w = doc["wifi"].as<JsonObject>();
    String ssid = w["ssid"] | "";
    if (ssid.length()) {
      WifiCreds wc;
      wc.ssid = ssid;
      wc.password = w["password"] | "";
      wifiSaveCreds(wc);
    }
  }
  if (doc["device_settings"].is<JsonObject>() && gDevSettings) {
    JsonObject ds = doc["device_settings"].as<JsonObject>();
    if (!ds["brightness"].isNull()) {
      gDevSettings->brightness = ds["brightness"] | 180;
      sleepSetUserBrightness(gDevSettings->brightness);
      if (!displayIsOff()) displaySetBacklight(gDevSettings->brightness);
    }
    if (!ds["display_timeout_ms"].isNull()) {
      gDevSettings->displayTimeoutMs = ds["display_timeout_ms"] | 60000;
    } else if (!ds["sleep_timeout_ms"].isNull()) {
      gDevSettings->displayTimeoutMs = ds["sleep_timeout_ms"] | 60000;
    }
    if (!ds["deep_sleep_timeout_ms"].isNull()) {
      gDevSettings->deepSleepTimeoutMs = ds["deep_sleep_timeout_ms"] | (15UL * 60UL * 1000UL);
    }
    if (!ds["display_timeout_ms"].isNull() || !ds["sleep_timeout_ms"].isNull() ||
        !ds["deep_sleep_timeout_ms"].isNull()) {
      sleepSetDisplayTimeoutMs(gDevSettings->displayTimeoutMs);
      sleepSetDeepSleepTimeoutMs(gDevSettings->deepSleepTimeoutMs);
    }
    if (!ds["motion_wake_enabled"].isNull()) {
      gDevSettings->motionWake = ds["motion_wake_enabled"] | true;
      sleepSetMotionWake(gDevSettings->motionWake);
    }
    if (!ds["ha_poll_enabled"].isNull()) gDevSettings->haPollEnabled = ds["ha_poll_enabled"] | false;
    deviceSettingsSave(*gDevSettings);
  }
  if (doc["config"].is<JsonObject>() && gConfig) {
    String cfgJson;
    serializeJson(doc["config"], cfgJson);
    doc.remove("config");
    yield();
    String err;
    if (!configImportJsonString(cfgJson, *gConfig, err)) {
      sendJson(400, String("{\"error\":\"") + err + "\"}");
      return;
    }
  }
  if (doc["ir_library"].is<JsonArray>()) {
    String err;
    if (!irLibReplaceAll(doc["ir_library"].as<JsonArray>(), err)) {
      sendJson(400, String("{\"error\":\"ir_library: ") + err + "\"}");
      return;
    }
  }
  sendJson(200, "{\"ok\":true}");
  // Response must be flushed before heavy UI reload (import + immediate GET /api/config).
  if (server.client()) server.client().flush();
  if (gOnChange) gOnChange();
}

static void handleConfigGet() {
  if (!LittleFS.exists(CONFIG_PATH)) {
    sendJson(404, "{\"error\":\"no config file\"}");
    return;
  }
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    sendJson(500, "{\"error\":\"open failed\"}");
    return;
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Content-Type", "application/json");
  server.setContentLength(f.size());
  server.send(200);
  uint8_t buf[512];
  WiFiClient client = server.client();
  while (f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    client.write(buf, n);
    yield();
  }
  f.close();
  httpCloseClient();
}

static bool gConfigDeployBusy = false;

static void handleConfigPost() {
  if (!gConfig) {
    sendJson(500, "{\"error\":\"no config\"}");
    return;
  }
  if (gConfigDeployBusy) {
    sendJson(503, "{\"error\":\"deploy in progress\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"missing body\"}");
    return;
  }
  const String &body = server.arg("plain");
  if (body.length() < 4) {
    sendJson(400, "{\"error\":\"empty body\"}");
    return;
  }
  gConfigDeployBusy = true;
  const DeviceMemInfo mem = deviceMemSnapshot();
  Serial.printf("deploy heap free=%u max=%u min=%u\n", mem.freeHeap, mem.maxAllocHeap,
                mem.minFreeHeap);
  String err;
  if (!configApplyPostBody(body, *gConfig, err)) {
    gConfigDeployBusy = false;
    sendJson(400, String("{\"error\":\"") + err + "\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"restart\":true}");
  delay(250);
  ESP.restart();
}

static void handleHaEntities() {
  String domain = server.hasArg("domain") ? server.arg("domain") : "light";
  String search = server.hasArg("search") ? server.arg("search") : "";
  String json, err;
  if (!gSettings || !haFetchEntitiesFiltered(*gSettings, domain, search, json, err)) {
    sendJson(400, String("{\"error\":\"") + err + "\"}");
    return;
  }
  sendJson(200, json);
}

static void handleHaStates() {
  String json, err;
  if (!gSettings || !haFetchStates(*gSettings, json, err)) {
    sendJson(400, String("{\"error\":\"") + err + "\"}");
    return;
  }
  sendJson(200, json);
}

static void handleHaEntityState() {
  if (!server.hasArg("entity_id")) {
    sendJson(400, "{\"error\":\"entity_id required\"}");
    return;
  }
  String resp, err;
  if (!gSettings || !haFetchEntityRaw(*gSettings, server.arg("entity_id"), resp, err)) {
    sendJson(400, String("{\"error\":\"") + err + "\"}");
    return;
  }
  JsonDocument raw;
  if (deserializeJson(raw, resp)) {
    sendJson(400, "{\"error\":\"invalid json\"}");
    return;
  }
  JsonDocument doc;
  doc["entity_id"] = server.arg("entity_id");
  String state = raw["state"].as<String>();
  doc["state"] = state;
  doc["on"] = haStateIsOn(state);
  JsonObject attr = raw["attributes"];
  if (!attr.isNull()) {
    doc["friendly_name"] = attr["friendly_name"] | "";
    doc["icon"] = attr["icon"] | "";
    if (!attr["current_temperature"].isNull())
      doc["current_temperature"] = attr["current_temperature"];
    if (!attr["temperature"].isNull()) doc["temperature"] = attr["temperature"];
    if (!attr["hvac_mode"].isNull()) doc["hvac_mode"] = attr["hvac_mode"];
    if (!attr["unit_of_measurement"].isNull())
      doc["unit_of_measurement"] = attr["unit_of_measurement"];
  }
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

static void handleHaWs() {
  if (!server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"missing body\"}");
    return;
  }
  String resp, err;
  bool ok = gSettings && haWsProxy(*gSettings, server.arg("plain"), resp, err);
  if (ok) sendJson(200, resp);
  else sendJson(400, String("{\"error\":\"") + err + "\"}");
}

static void handleDeviceSettingsGet() {
  JsonDocument doc;
  if (gDevSettings) {
    doc["brightness"] = gDevSettings->brightness;
    doc["display_timeout_ms"] = gDevSettings->displayTimeoutMs;
    doc["deep_sleep_timeout_ms"] = gDevSettings->deepSleepTimeoutMs;
    doc["sleep_timeout_ms"] = gDevSettings->displayTimeoutMs;
    doc["motion_wake_enabled"] = gDevSettings->motionWake;
    doc["timezone"] = gDevSettings->timezone;
    doc["ntp_server"] = gDevSettings->ntpServer;
    doc["display_off"] = displayIsOff();
  }
  doc["time_synced"] = timeIsSynced();
  char iso[24];
  if (timeGetLocalIso(iso, sizeof(iso))) doc["device_time"] = iso;
  doc["battery_percent"] = batteryPercent();
  doc["battery_charging"] = batteryCharging();
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["wifi_ssid"] = WiFi.SSID();
  doc["ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

static void handleDeviceSettingsPost() {
  if (!gDevSettings || !server.hasArg("plain")) {
    sendJson(400, "{\"error\":\"missing\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJson(400, "{\"error\":\"invalid json\"}");
    return;
  }
    if (!doc["brightness"].isNull()) {
      gDevSettings->brightness = doc["brightness"] | 180;
      sleepSetUserBrightness(gDevSettings->brightness);
      if (!displayIsOff()) displaySetBacklight(gDevSettings->brightness);
    }
    if (!doc["display_timeout_ms"].isNull()) {
      gDevSettings->displayTimeoutMs = doc["display_timeout_ms"] | 60000;
    } else if (!doc["sleep_timeout_ms"].isNull()) {
      gDevSettings->displayTimeoutMs = doc["sleep_timeout_ms"] | 60000;
    }
    if (!doc["deep_sleep_timeout_ms"].isNull()) {
      gDevSettings->deepSleepTimeoutMs = doc["deep_sleep_timeout_ms"] | (15UL * 60UL * 1000UL);
    }
    if (!doc["display_timeout_ms"].isNull() || !doc["sleep_timeout_ms"].isNull() ||
        !doc["deep_sleep_timeout_ms"].isNull()) {
      sleepSetDisplayTimeoutMs(gDevSettings->displayTimeoutMs);
      sleepSetDeepSleepTimeoutMs(gDevSettings->deepSleepTimeoutMs);
    }
    if (!doc["motion_wake_enabled"].isNull()) {
      gDevSettings->motionWake = doc["motion_wake_enabled"] | true;
      sleepSetMotionWake(gDevSettings->motionWake);
    }
    bool clockChanged = false;
    if (!doc["timezone"].isNull()) {
      gDevSettings->timezone = doc["timezone"].as<String>();
      if (!gDevSettings->timezone.length()) gDevSettings->timezone = "UTC0";
      clockChanged = true;
    }
    if (!doc["ntp_server"].isNull()) {
      gDevSettings->ntpServer = doc["ntp_server"].as<String>();
      if (!gDevSettings->ntpServer.length()) gDevSettings->ntpServer = "pool.ntp.org";
      clockChanged = true;
    }
    if (clockChanged) {
      timeSetTimezone(gDevSettings->timezone);
      timeForceResync();
    }
  deviceSettingsSave(*gDevSettings);
  sendJson(200, "{\"ok\":true}");
}

void httpServerBegin(HaSettings &settings, OmoteConfig &config, DeviceSettings &devSettings,
                     ConfigChangedCallback onConfigChanged) {
  if (gServerUp) return;
  gSettings = &settings;
  gConfig = &config;
  gDevSettings = &devSettings;
  gOnChange = onConfigChanged;

  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsPost);
  server.on("/api/settings/test", HTTP_POST, handleSettingsTest);
  server.on("/api/backup/export", HTTP_GET, handleBackupExport);
  server.on("/api/backup/import", HTTP_POST, handleBackupImport);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/ha/entities", HTTP_GET, handleHaEntities);
  server.on("/api/ha/states", HTTP_GET, handleHaStates);
  server.on("/api/ha/entity", HTTP_GET, handleHaEntityState);
  server.on("/api/ha/ws", HTTP_POST, handleHaWs);
  server.on("/api/device/settings", HTTP_GET, handleDeviceSettingsGet);
  server.on("/api/device/settings", HTTP_POST, handleDeviceSettingsPost);
  server.on("/api/device/reboot", HTTP_POST, []() {
    sendJson(200, "{\"ok\":true}");
    delay(200);
    ESP.restart();
  });
  server.on("/api/device/display-off", HTTP_POST, []() {
    sleepDisplayOff();
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/device/sleep", HTTP_POST, []() {
    sendJson(200, "{\"ok\":true}");
    delay(100);
    sleepEnterDeep();
  });
  server.on("/api/device/mem", HTTP_GET, []() {
    const DeviceMemInfo m = deviceMemSnapshot();
    JsonDocument doc;
    doc["free_heap"] = m.freeHeap;
    doc["max_alloc_heap"] = m.maxAllocHeap;
    doc["min_free_heap"] = m.minFreeHeap;
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
  });
  server.on("/api/device/log", HTTP_GET, []() {
    JsonDocument doc;
    doc["log"] = debugLogTail(6000);
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
  });
  server.on("/api/ir/learn/start", HTTP_POST, []() {
    irLearnStart();
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/ir/learn/poll", HTTP_GET, []() {
    String proto;
    uint64_t code = 0;
    uint16_t bits = 0;
    if (irLearnPoll(proto, code, bits)) {
      JsonDocument doc;
      doc["ok"] = true;
      doc["protocol"] = proto;
      doc["code"] = String("0x") + String(code, HEX);
      doc["bits"] = bits;
      String out;
      serializeJson(doc, out);
      sendJson(200, out);
    } else {
      sendJson(200, "{\"ok\":false}");
    }
  });
  server.on("/api/ir/learn/stop", HTTP_POST, []() {
    irLearnStop();
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/ir/library", HTTP_GET, []() {
    String json, err;
    if (!irLibListJson(json, err)) {
      sendJson(500, String("{\"error\":\"") + err + "\"}");
      return;
    }
    sendJson(200, json);
  });
  server.on("/api/ir/library", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      sendJson(400, "{\"error\":\"missing body\"}");
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      sendJson(400, "{\"error\":\"invalid json\"}");
      return;
    }
    String name = doc["name"] | "";
    String protocol = doc["protocol"] | "NEC";
    uint64_t code = 0;
    if (doc["code"].is<const char *>()) code = strtoull(doc["code"], nullptr, 0);
    else code = doc["code"].as<uint64_t>();
    uint16_t bits = doc["bits"] | 32;
    String id, err;
    if (!irLibAdd(name, protocol, code, bits, id, err)) {
      sendJson(400, String("{\"error\":\"") + err + "\"}");
      return;
    }
    sendJson(200, String("{\"ok\":true,\"id\":\"") + id + "\"}");
  });
  server.on("/api/ir/library/delete", HTTP_POST, []() {
    String id = server.hasArg("id") ? server.arg("id") : "";
    if (!id.length() && server.hasArg("plain")) {
      JsonDocument doc;
      if (!deserializeJson(doc, server.arg("plain"))) id = doc["id"] | "";
    }
    String err;
    if (!irLibDelete(id, err)) {
      sendJson(400, String("{\"error\":\"") + err + "\"}");
      return;
    }
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/ir/library/import", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      sendJson(400, "{\"error\":\"missing body\"}");
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      sendJson(400, "{\"error\":\"invalid json\"}");
      return;
    }
    JsonArray arr;
    if (doc.is<JsonArray>()) arr = doc.as<JsonArray>();
    else if (doc["entries"].is<JsonArray>()) arr = doc["entries"].as<JsonArray>();
    else {
      sendJson(400, "{\"error\":\"expected array or entries[]\"}");
      return;
    }
    String err;
    if (!irLibReplaceAll(arr, err)) {
      sendJson(400, String("{\"error\":\"") + err + "\"}");
      return;
    }
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/ble/status", HTTP_GET, []() { sendJson(200, bleStatusJson()); });
  server.on("/api/ble/start", HTTP_POST, []() {
    bleStartPairingMode();
    sendJson(200, bleStatusJson());
  });
  server.on("/api/ble/stop", HTTP_POST, []() {
    bleStopPairingMode();
    sendJson(200, bleStatusJson());
  });
  server.on("/api/ble/disconnect", HTTP_POST, []() {
    if (!bleIsInitialized()) bleInit();
    bleDisconnectClients();
    sendJson(200, bleStatusJson());
  });
  server.on("/api/ble/forget", HTTP_POST, []() {
    bleForgetBonds();
    sendJson(200, bleStatusJson());
  });
  server.on("/api/ble/identities", HTTP_GET, []() { sendJson(200, bleIdentityListJson()); });
  /* Debug: send a raw HID code without going through normalizeBleKeyName().
   * Body: { "usage": "0x02A2" }   → 16-bit Consumer-Page usage
   *       { "button": 3 }         → 16-button report (BUTTON_1..16) */
  server.on("/api/ble/test", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      sendJson(400, "{\"error\":\"missing body\"}");
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      sendJson(400, "{\"error\":\"invalid json\"}");
      return;
    }
    if (!bleIsConnected()) {
      sendJson(409, "{\"error\":\"BLE not connected\"}");
      return;
    }
    if (!doc["button"].isNull()) {
      const int btn = doc["button"] | 0;
      if (!bleSendRawButton((uint8_t)btn)) {
        sendJson(400, "{\"error\":\"button must be 1..16\"}");
        return;
      }
      sendJson(200, String("{\"ok\":true,\"sent\":\"BUTTON_") + btn + "\"}");
      return;
    }
    uint32_t usage = 0;
    if (doc["usage"].is<const char *>()) {
      usage = (uint32_t)strtoul(doc["usage"], nullptr, 0);
    } else {
      usage = doc["usage"] | 0u;
    }
    if (usage == 0 || usage > 0xFFFF) {
      sendJson(400, "{\"error\":\"usage must be 0x0001..0xFFFF\"}");
      return;
    }
    bleSendRawConsumerUsage((uint16_t)usage);
    String out = "{\"ok\":true,\"sent\":\"0x";
    char buf[8];
    snprintf(buf, sizeof(buf), "%04X", (unsigned)usage);
    out += buf;
    out += "\"}";
    sendJson(200, out);
  });
  server.on("/api/ble/identity", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      sendJson(400, "{\"error\":\"missing body\"}");
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      sendJson(400, "{\"error\":\"invalid json\"}");
      return;
    }
    const String profile = doc["profile"] | "";
    if (!profile.length()) {
      sendJson(400, "{\"error\":\"profile required\"}");
      return;
    }
    if (!bleSetProfile(profile)) {
      sendJson(400, "{\"error\":\"unknown profile\"}");
      return;
    }
    if (gDevSettings) {
      gDevSettings->bleProfile = bleCurrentProfile();
      deviceSettingsSave(*gDevSettings);
    }
    /* Wipe bonds so the host re-discovers the new HID descriptor cleanly,
     * and re-init BLE with the new VID/PID. The user must also forget the
     * old bond on the TV side and re-pair. */
    bleForgetBonds();
    sendJson(200, bleIdentityListJson());
  });
  server.on("/api/event/last", HTTP_GET, []() { sendJson(200, httpServerLastEventJson()); });
  server.on("/api/key/last", HTTP_GET, []() { sendJson(200, httpServerLastKeyJson()); });

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Connection", "close");
      server.send(204, "text/plain", "");
      httpCloseClient();
      return;
    }
    if (server.uri().startsWith("/api/")) {
      Serial.printf("HTTP 404 api: %s\n", server.uri().c_str());
      sendJson(404, "{\"error\":\"not found\"}");
      return;
    }
    if (server.uri() == "/favicon.ico") {
      server.sendHeader("Connection", "close");
      server.send(204, "text/plain", "");
      httpCloseClient();
      return;
    }
    handleStatic();
  });

  server.enableDelay(false);
  server.begin();
  gServerUp = true;
  Serial.println("HTTP server started on :80");

  if (MDNS.begin("omote")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: omote.local");
  }
}

void httpServerLoop() {
  if (!gServerUp) return;
  server.handleClient();
}
