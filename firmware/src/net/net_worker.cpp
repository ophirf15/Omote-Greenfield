#include "net/net_worker.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t gNetMutex = nullptr;
static volatile uint32_t gLastWebClientMs = 0;

static constexpr size_t kMaxGetResponseBytes = 16384;

static String normalizeUrl(const String &url) {
  String u = url;
  u.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  return u;
}

static void drainHttpBody(HTTPClient &http) {
  WiFiClient *stream = http.getStreamPtr();
  if (!stream) return;
  uint8_t buf[256];
  const uint32_t deadline = millis() + 2000;
  while ((http.connected() || stream->available()) && (int32_t)(millis() - deadline) < 0) {
    const int n = stream->readBytes(buf, sizeof(buf));
    if (n <= 0) {
      if (!stream->available()) break;
      delay(1);
      continue;
    }
  }
}

static bool readCappedGetBody(HTTPClient &http, String &response) {
  WiFiClient *stream = http.getStreamPtr();
  if (!stream) return false;
  response = "";
  response.reserve(2048);
  uint8_t buf[512];
  size_t total = 0;
  const uint32_t deadline = millis() + 8000;
  while ((http.connected() || stream->available()) && (int32_t)(millis() - deadline) < 0) {
    const int n = stream->readBytes(buf, sizeof(buf));
    if (n <= 0) {
      if (!stream->available() && !http.connected()) break;
      delay(1);
      continue;
    }
    total += (size_t)n;
    if (total <= kMaxGetResponseBytes) response.concat((const char *)buf, (unsigned)n);
  }
  return total > 0;
}

void netWorkerInit() {
  if (!gNetMutex) gNetMutex = xSemaphoreCreateMutex();
}

void netWorkerTouchWebClient() { gLastWebClientMs = millis(); }

bool netWorkerWebUiActive(uint32_t windowMs) {
  if (gLastWebClientMs == 0) return false;
  return (millis() - gLastWebClientMs) < windowMs;
}

bool netWorkerLock(uint32_t timeoutMs) {
  if (!gNetMutex) netWorkerInit();
  return xSemaphoreTake(gNetMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void netWorkerUnlock() {
  if (gNetMutex) xSemaphoreGive(gNetMutex);
}

DeviceMemInfo deviceMemSnapshot() {
  DeviceMemInfo m;
  m.freeHeap = ESP.getFreeHeap();
  m.maxAllocHeap = ESP.getMaxAllocHeap();
  m.minFreeHeap = ESP.getMinFreeHeap();
  return m;
}

bool netWorkerHttpRequest(const HaSettings &s, const String &method, const String &path,
                          const String &body, int &httpCode, String &response) {
  if (!s.configured || !WiFi.isConnected()) return false;
  if (!gNetMutex) netWorkerInit();
  if (xSemaphoreTake(gNetMutex, pdMS_TO_TICKS(12000)) != pdTRUE) {
    Serial.println("net: HTTP busy timeout");
    return false;
  }

  HTTPClient http;
  WiFiClient client;
  String url = normalizeUrl(s.url) + path;
  const uint32_t timeoutMs = netWorkerWebUiActive() ? 3500 : 6000;
  http.setTimeout(timeoutMs);
  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + s.token);
  http.addHeader("Content-Type", "application/json");
  response = "";
  if (method == "GET") {
    httpCode = http.GET();
    if (httpCode > 0) {
      if (!readCappedGetBody(http, response) && httpCode >= 200 && httpCode < 300) {
        response = http.getString();
      }
    }
  } else if (method == "POST") {
    httpCode = http.POST(body);
    /* homeassistant.toggle dumps the full entity state — never buffer it. */
    if (httpCode > 0) drainHttpBody(http);
  } else {
    http.end();
    xSemaphoreGive(gNetMutex);
    return false;
  }
  http.end();
  xSemaphoreGive(gNetMutex);
  return true;
}
