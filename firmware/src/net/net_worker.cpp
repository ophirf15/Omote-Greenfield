#include "net/net_worker.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t gNetMutex = nullptr;
static volatile uint32_t gLastWebClientMs = 0;

static String normalizeUrl(const String &url) {
  String u = url;
  u.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  return u;
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
  if (method == "GET") {
    httpCode = http.GET();
  } else if (method == "POST") {
    httpCode = http.POST(body);
  } else {
    http.end();
    xSemaphoreGive(gNetMutex);
    return false;
  }
  response = http.getString();
  http.end();
  xSemaphoreGive(gNetMutex);
  return true;
}
