#include "hal/wifi_hal.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>

#define WIFI_SETTINGS "/wifi.json"

static DNSServer dns;
static WebServer portalServer(80);
static bool portalActive = false;
static WifiUiState gUiState = WIFI_UI_CONNECTING;
static bool gConnectStarted = false;
static uint32_t gConnectStartMs = 0;

bool wifiLoadCreds(WifiCreds &c) {
  if (!LittleFS.exists(WIFI_SETTINGS)) return false;
  File f = LittleFS.open(WIFI_SETTINGS, "r");
  if (!f) return false;
  JsonDocument doc;
  if (deserializeJson(doc, f)) {
    f.close();
    return false;
  }
  f.close();
  c.ssid = doc["ssid"] | "";
  c.password = doc["password"] | "";
  return c.ssid.length() > 0;
}

bool wifiSaveCreds(const WifiCreds &c) {
  JsonDocument doc;
  doc["ssid"] = c.ssid;
  doc["password"] = c.password;
  File f = LittleFS.open(WIFI_SETTINGS, "w");
  if (!f) {
    Serial.println("WiFi: failed to open wifi.json for write");
    return false;
  }
  serializeJson(doc, f);
  f.close();
  Serial.printf("WiFi credentials saved for SSID: %s\n", c.ssid.c_str());
  return true;
}

static String portalHtml() {
  return R"raw(<!DOCTYPE html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Omote WiFi Setup</title><style>body{font-family:sans-serif;max-width:400px;margin:2em auto;padding:1em}
input,button{width:100%;padding:12px;margin:8px 0;box-sizing:border-box}button{background:#3366cc;color:#fff;border:0}</style></head>
<body><h1>Omote WiFi</h1><form method=POST action=/save>
<label>SSID</label><input name=ssid required>
<label>Password</label><input name=password type=password>
<button type=submit>Save &amp; Reboot</button></form></body></html>)raw";
}

static void handlePortalRoot() { portalServer.send(200, "text/html", portalHtml()); }

static void handlePortalSave() {
  if (!portalServer.hasArg("ssid")) {
    portalServer.send(400, "text/plain", "Missing ssid");
    return;
  }
  WifiCreds c;
  c.ssid = portalServer.arg("ssid");
  c.password = portalServer.arg("password");
  if (!wifiSaveCreds(c)) {
    portalServer.send(500, "text/html", "<html><body><h2>Save failed</h2></body></html>");
    return;
  }
  portalServer.send(200, "text/html",
                    "<html><body><h2>Saved. Rebooting...</h2></body></html>");
  portalServer.client().flush();
  delay(800);
  ESP.restart();
}

static void handlePortalCaptive() {
  portalServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  portalServer.send(302, "text/plain", "");
}

void wifiStartPortal(const char *apName) {
  gUiState = WIFI_UI_SETUP_AP;
  gConnectStarted = false;
  WiFi.disconnect(true, true);
  delay(150);
  WiFi.mode(WIFI_OFF);
  delay(100);
  if (!WiFi.mode(WIFI_AP)) {
    Serial.println("WiFi: failed to set AP mode");
    return;
  }
  delay(100);
  const bool apOk = WiFi.softAP(apName, nullptr, 6, 0, 8);
  delay(200);
  Serial.printf("WiFi AP '%s' %s — IP %s\n", apName, apOk ? "started" : "FAILED",
                WiFi.softAPIP().toString().c_str());
  if (!apOk) return;
  dns.start(53, "*", WiFi.softAPIP());
  portalServer.on("/", HTTP_GET, handlePortalRoot);
  portalServer.on("/save", HTTP_POST, handlePortalSave);
  portalServer.on("/generate_204", HTTP_GET, handlePortalCaptive);
  portalServer.on("/hotspot-detect.html", HTTP_GET, handlePortalCaptive);
  portalServer.on("/fwlink", HTTP_GET, handlePortalCaptive);
  portalServer.onNotFound(handlePortalCaptive);
  portalServer.begin();
  portalActive = true;
}

void wifiPrepareCoexistence() {
  WiFi.persistent(false);
}

void wifiBeginConnect() {
  WifiCreds c;
  if (!wifiLoadCreds(c)) {
    gUiState = WIFI_UI_FAILED;
    return;
  }
  gUiState = WIFI_UI_CONNECTING;
  gConnectStarted = true;
  gConnectStartMs = millis();
  WiFi.persistent(false);
  WiFi.setSleep(true);
  Serial.printf("WiFi connecting to %s...\n", c.ssid.c_str());
  WiFi.begin(c.ssid.c_str(), c.password.c_str());
}

bool wifiPollConnect(uint32_t timeoutMs) {
  if (!gConnectStarted) return false;
  if (WiFi.status() == WL_CONNECTED) {
    gUiState = WIFI_UI_CONNECTED;
    gConnectStarted = false;
    Serial.print("WiFi OK: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  if (millis() - gConnectStartMs > timeoutMs) {
    gUiState = WIFI_UI_FAILED;
    gConnectStarted = false;
    Serial.println("WiFi connect timeout");
    return false;
  }
  return false;
}

bool wifiConnectStored(uint32_t timeoutMs) {
  wifiBeginConnect();
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (wifiPollConnect(timeoutMs)) return true;
    delay(250);
  }
  return false;
}

WifiUiState wifiUiState() { return gUiState; }

const char *wifiUiStateText() {
  switch (gUiState) {
    case WIFI_UI_SETUP_AP:
      return "Setup: Omote-Setup";
    case WIFI_UI_CONNECTING:
      return "WiFi connecting...";
    case WIFI_UI_CONNECTED:
      return "WiFi connected";
    case WIFI_UI_FAILED:
      return "WiFi failed";
  }
  return "?";
}

bool wifiIsConnected() { return WiFi.status() == WL_CONNECTED; }

String wifiStatusJson() {
  String j = "{\"connected\":";
  j += wifiIsConnected() ? "true" : "false";
  j += ",\"ssid\":\"";
  if (wifiIsConnected()) j += WiFi.SSID();
  j += "\",\"rssi\":";
  j += wifiIsConnected() ? String(WiFi.RSSI()) : "0";
  j += "}";
  return j;
}

int wifiRssi() { return wifiIsConnected() ? WiFi.RSSI() : 0; }

void wifiPortalLoop() {
  if (!portalActive) return;
  dns.processNextRequest();
  portalServer.handleClient();
}
