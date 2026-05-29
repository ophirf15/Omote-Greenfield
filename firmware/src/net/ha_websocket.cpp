#include "net/ha_websocket.h"
#include "net/ha_client.h"
#include "net/ha_state_store.h"
#include "net/net_worker.h"
#include "net/net_heap.h"
#include <ArduinoJson.h>
#include <string.h>

#ifdef OMOTE_HA_REST_ONLY

static HaSettings gSettings;

void haWsInit() { haStateStoreInit(); }

void haWsStartTask() {
  haWsInit();
  Serial.println("ha: REST-only mode (no WebSocket task — saves ~8KB RAM)");
}

void haWsOnWifiUp() {}
void haWsOnWifiDown() {}

void haWsSetSettings(const HaSettings *settings) {
  if (settings) gSettings = *settings;
}

bool haWsIsConnected() { return false; }
bool haWsIsConnecting() { return false; }

void haWsSubscribeEntities(const std::vector<String> &entityIds) {
  if (!netHeapOkForHaGet()) return;
  haStateStoreQueueBootstrap(entityIds);
}

bool haWsCallService(const String &domain, const String &service, const String &entityId,
                     const String &dataJson, String &errorOut) {
  if (!gSettings.configured) {
    errorOut = "HA not configured";
    return false;
  }
  if (!netHeapOkForHaPost()) {
    errorOut = "low memory";
    return false;
  }
  netWorkerSetPreferShortTimeout(true);
  const bool ok = haCallService(gSettings, domain, service, entityId, dataJson);
  netWorkerSetPreferShortTimeout(false);
  if (!ok) errorOut = "HA call failed";
  return ok;
}

void haWsTick() {}

void haWsReleasePressure() { haStateStoreAbortBootstrap(); }

#else /* full WebSocket path */

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#undef WEBSOCKETS_MAX_DATA_SIZE
#define WEBSOCKETS_MAX_DATA_SIZE 2048
#include <WebSocketsClient.h>

static HaSettings gSettings;
static WebSocketsClient gWs;
static bool gAuthOk = false;
static bool gConnecting = false;
static bool gWantConnect = false;
static uint32_t gLastConnectAttemptMs = 0;
static uint32_t gNetworkReadyMs = 0;
static uint32_t gNextMsgId = 1;
static String gResolvedHost;
static std::vector<String> gSubscribed;
static QueueHandle_t gCmdQueue = nullptr;
static TaskHandle_t gTaskHandle = nullptr;
static SemaphoreHandle_t gSubMx = nullptr;

static constexpr uint32_t kReconnectMs = 30000;
static constexpr uint32_t kConnectCooldownMs = 5000;

enum CmdType : uint8_t {
  CMD_SUBSCRIBE = 1,
  CMD_CALL_SERVICE = 2,
};

struct WsCommand {
  CmdType type = CMD_SUBSCRIBE;
  char domain[20] = {};
  char service[24] = {};
  char entityId[96] = {};
  char dataJson[96] = {};
};

static bool parseHaBaseUrl(const String &urlIn, String &host, uint16_t &port, bool &useSsl) {
  String url = urlIn;
  url.trim();
  useSsl = url.startsWith("https://");
  if (url.startsWith("http://")) url.remove(0, 7);
  else if (url.startsWith("https://")) url.remove(0, 8);
  while (url.endsWith("/")) url.remove(url.length() - 1);
  int colon = url.indexOf(':');
  if (colon > 0) {
    host = url.substring(0, colon);
    port = (uint16_t)url.substring(colon + 1).toInt();
    if (port == 0) port = useSsl ? 443 : 8123;
  } else {
    host = url;
    port = useSsl ? 443 : 8123;
  }
  return host.length() > 0;
}

static bool heapOkForWs() { return netHeapComfortable(); }

static bool networkSettled() {
  return gNetworkReadyMs != 0 && (millis() - gNetworkReadyMs) >= 4000;
}

static uint32_t nextMsgId() { return gNextMsgId++; }

static void copySubscribeList(const std::vector<String> &entities) {
  if (!gSubMx) gSubMx = xSemaphoreCreateMutex();
  if (xSemaphoreTake(gSubMx, pdMS_TO_TICKS(100)) != pdTRUE) return;
  gSubscribed = entities;
  xSemaphoreGive(gSubMx);
}

static bool entityIsSubscribed(const char *entityId) {
  if (!entityId || !entityId[0]) return false;
  if (xSemaphoreTake(gSubMx, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  bool found = false;
  for (const auto &e : gSubscribed) {
    if (e == entityId) {
      found = true;
      break;
    }
  }
  xSemaphoreGive(gSubMx);
  return found;
}

static bool resolveHostToIp(const String &host, String &ipOut) {
  if (host.indexOf('.') >= 0 && host != "homeassistant.local") {
    IPAddress addr;
    if (addr.fromString(host)) {
      ipOut = host;
      return true;
    }
  }
  IPAddress ip;
  if (WiFi.hostByName(host.c_str(), ip) != 1) return false;
  ipOut = ip.toString();
  return ipOut.length() > 0;
}

static void wsSendAuth() {
  if (!gSettings.token.length()) return;
  JsonDocument doc;
  doc["type"] = "auth";
  doc["access_token"] = gSettings.token;
  String out;
  serializeJson(doc, out);
  gWs.sendTXT(out);
}

static void wsSendSubscribeEntities() {
  if (!gAuthOk) return;
  if (xSemaphoreTake(gSubMx, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (gSubscribed.empty()) {
    xSemaphoreGive(gSubMx);
    return;
  }
  JsonDocument doc;
  doc["id"] = nextMsgId();
  doc["type"] = "subscribe_entities";
  JsonArray arr = doc["entity_ids"].to<JsonArray>();
  for (const auto &e : gSubscribed) arr.add(e);
  const size_t nEnt = gSubscribed.size();
  String out;
  serializeJson(doc, out);
  xSemaphoreGive(gSubMx);
  gWs.sendTXT(out);
  Serial.printf("ha_ws: subscribe_entities (%u)\n", (unsigned)nEnt);
}

static void handleStateChanged(JsonObject eventData) {
  const char *eid = eventData["entity_id"] | "";
  if (!eid[0] || !entityIsSubscribed(eid)) return;
  JsonObject newState = eventData["new_state"];
  if (newState.isNull()) return;
  haStateStoreUpdateFromJson(eid, newState);
}

static void handleWsText(const char *payload) {
  static JsonDocument doc;
  doc.clear();
  if (deserializeJson(doc, payload)) return;
  const char *type = doc["type"] | "";
  if (strcmp(type, "auth_required") == 0) {
    wsSendAuth();
    return;
  }
  if (strcmp(type, "auth_ok") == 0) {
    gAuthOk = true;
    gConnecting = false;
    Serial.println("ha_ws: authenticated");
    wsSendSubscribeEntities();
    return;
  }
  if (strcmp(type, "auth_invalid") == 0) {
    Serial.println("ha_ws: auth invalid");
    gAuthOk = false;
    gWs.disconnect();
    return;
  }
  if (strcmp(type, "event") == 0) {
    JsonObject event = doc["event"];
    const char *eventType = event["event_type"] | "";
    if (strcmp(eventType, "state_changed") == 0) {
      handleStateChanged(event["data"]);
      return;
    }
    JsonObject a = event["a"];
    if (!a.isNull()) {
      for (JsonPair kv : a) {
        const char *eid = kv.key().c_str();
        if (!entityIsSubscribed(eid)) continue;
        JsonObject ent = kv.value();
        const char *st = ent["s"] | ent["state"] | "";
        if (st[0]) haStateStoreUpdate(String(eid), String(st));
      }
    }
  }
}

static void wsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      gAuthOk = false;
      gConnecting = false;
      break;
    case WStype_CONNECTED:
      gConnecting = false;
      Serial.println("ha_ws: connected");
      break;
    case WStype_TEXT:
      if (payload && length) {
        static char buf[2048];
        const size_t n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
        memcpy(buf, payload, n);
        buf[n] = '\0';
        handleWsText(buf);
      }
      break;
    default:
      break;
  }
}

static bool wsConnect() {
  if (!gSettings.configured || WiFi.status() != WL_CONNECTED) return false;
  if (!networkSettled() || !heapOkForWs()) return false;
  const uint32_t now = millis();
  if (now - gLastConnectAttemptMs < kConnectCooldownMs) return false;
  gLastConnectAttemptMs = now;
  String host;
  uint16_t port = 8123;
  bool ssl = false;
  if (!parseHaBaseUrl(gSettings.url, host, port, ssl)) return false;
  if (!resolveHostToIp(host, gResolvedHost)) return false;
  gAuthOk = false;
  gConnecting = true;
  gWs.disconnect();
  vTaskDelay(pdMS_TO_TICKS(1));
  gWs.onEvent(wsEvent);
  gWs.setReconnectInterval(0);
  if (ssl)
    gWs.beginSSL(gResolvedHost.c_str(), port, "/api/websocket");
  else
    gWs.begin(gResolvedHost.c_str(), port, "/api/websocket");
  Serial.printf("ha_ws: connecting %s (%s):%u heap=%u\n", host.c_str(), gResolvedHost.c_str(), port,
                ESP.getFreeHeap());
  return true;
}

static void processCommand(const WsCommand &cmd) {
  switch (cmd.type) {
    case CMD_SUBSCRIBE:
      gWantConnect = true;
      if (gAuthOk)
        wsSendSubscribeEntities();
      else if (!gConnecting)
        wsConnect();
      break;
    case CMD_CALL_SERVICE:
      if (gAuthOk) {
        JsonDocument doc;
        doc["id"] = nextMsgId();
        doc["type"] = "call_service";
        doc["domain"] = cmd.domain;
        doc["service"] = cmd.service;
        JsonObject target = doc["target"].to<JsonObject>();
        target["entity_id"] = cmd.entityId;
        String out;
        serializeJson(doc, out);
        gWs.sendTXT(out);
      } else if (netHeapOkForHa()) {
        haCallService(gSettings, cmd.domain, cmd.service, cmd.entityId, cmd.dataJson);
      }
      break;
    default:
      break;
  }
}

static void haWsTask(void *param) {
  (void)param;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && gSettings.configured && heapOkForWs()) {
      gWs.loop();
      if (gWantConnect && !gAuthOk && !gConnecting && networkSettled() &&
          millis() - gLastConnectAttemptMs >= kConnectCooldownMs) {
        wsConnect();
      }
    } else if (gAuthOk || gConnecting) {
      gWs.disconnect();
      gAuthOk = false;
      gConnecting = false;
    }
    WsCommand cmd;
    while (xQueueReceive(gCmdQueue, &cmd, 0) == pdTRUE) processCommand(cmd);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void haWsInit() {
  haStateStoreInit();
  if (!gCmdQueue) gCmdQueue = xQueueCreate(4, sizeof(WsCommand));
  if (!gSubMx) gSubMx = xSemaphoreCreateMutex();
}

void haWsStartTask() {
  haWsInit();
  if (gTaskHandle) return;
  xTaskCreatePinnedToCore(haWsTask, "ha_ws", 8192, nullptr, 1, &gTaskHandle, 1);
  Serial.println("ha_ws: worker task started");
}

void haWsOnWifiUp() { gNetworkReadyMs = millis(); }
void haWsOnWifiDown() {
  gWs.disconnect();
  gAuthOk = false;
  gConnecting = false;
  gWantConnect = false;
  gNetworkReadyMs = 0;
}

void haWsSetSettings(const HaSettings *settings) {
  if (settings) gSettings = *settings;
}

bool haWsIsConnected() { return gAuthOk; }
bool haWsIsConnecting() { return gConnecting; }

void haWsSubscribeEntities(const std::vector<String> &entityIds) {
  if (!netHeapOkForHa()) return;
  copySubscribeList(entityIds);
  haStateStoreQueueBootstrap(entityIds);
  gWantConnect = true;
  WsCommand cmd;
  cmd.type = CMD_SUBSCRIBE;
  xQueueSend(gCmdQueue, &cmd, 0);
}

bool haWsCallService(const String &domain, const String &service, const String &entityId,
                     const String &dataJson, String &errorOut) {
  if (!gSettings.configured) {
    errorOut = "HA not configured";
    return false;
  }
  if (haWsIsConnected()) {
    WsCommand cmd;
    cmd.type = CMD_CALL_SERVICE;
    strncpy(cmd.domain, domain.c_str(), sizeof(cmd.domain) - 1);
    strncpy(cmd.service, service.c_str(), sizeof(cmd.service) - 1);
    strncpy(cmd.entityId, entityId.c_str(), sizeof(cmd.entityId) - 1);
    strncpy(cmd.dataJson, dataJson.c_str(), sizeof(cmd.dataJson) - 1);
    if (xQueueSend(gCmdQueue, &cmd, 0) == pdTRUE) return true;
  }
  if (!netHeapOkForHaPost()) {
    errorOut = "low memory";
    return false;
  }
  netWorkerSetPreferShortTimeout(true);
  const bool ok = haCallService(gSettings, domain, service, entityId, dataJson);
  netWorkerSetPreferShortTimeout(false);
  if (!ok) errorOut = "HA call failed";
  return ok;
}

void haWsTick() {}

void haWsReleasePressure() {
  haStateStoreAbortBootstrap();
  if (gAuthOk || gConnecting) {
    gWs.disconnect();
    gAuthOk = false;
    gConnecting = false;
  }
}

#endif /* OMOTE_HA_REST_ONLY */
