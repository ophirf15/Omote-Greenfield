#include "net/net_worker.h"
#include "net/net_heap.h"
#include "net/runtime_diag.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t gNetMutex = nullptr;
static volatile uint32_t gLastWebClientMs = 0;
static bool sPreferShortTimeout = false;

static constexpr size_t kMaxGetResponseBytes = 16384;
static constexpr size_t kMaxDrainBytes = 4096;

static String normalizeUrl(const String &url) {
  String u = url;
  u.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  return u;
}

void netWorkerSetPreferShortTimeout(bool preferShort) { sPreferShortTimeout = preferShort; }

static void drainHttpBody(HTTPClient &http) {
  WiFiClient *stream = http.getStreamPtr();
  if (!stream) return;
  uint8_t buf[256];
  size_t drained = 0;
  const uint32_t deadline = millis() + 1500;
  while ((http.connected() || stream->available()) && (int32_t)(millis() - deadline) < 0 &&
         drained < kMaxDrainBytes) {
    const int n = stream->readBytes(buf, sizeof(buf));
    if (n <= 0) {
      if (!stream->available()) break;
      yield();
      continue;
    }
    drained += (size_t)n;
    if ((drained & 0x3FF) == 0) yield();
  }
}

static bool readCappedGetBody(HTTPClient &http, String &response) {
  WiFiClient *stream = http.getStreamPtr();
  if (!stream) return false;
  response = "";
  response.reserve(2048);
  uint8_t buf[512];
  size_t total = 0;
  const uint32_t deadline = millis() + (sPreferShortTimeout ? 2800 : 8000);
  while ((http.connected() || stream->available()) && (int32_t)(millis() - deadline) < 0) {
    const int n = stream->readBytes(buf, sizeof(buf));
    if (n <= 0) {
      if (!stream->available() && !http.connected()) break;
      yield();
      continue;
    }
    total += (size_t)n;
    if (total <= kMaxGetResponseBytes) response.concat((const char *)buf, (unsigned)n);
    if ((total & 0x3FF) == 0) yield();
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
  const bool isPost = method == "POST";
  if (isPost ? !netHeapOkForHaPost() : !netHeapOkForHaGet()) {
    Serial.printf("net: low heap free=%u max=%u skip %s\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap(), method.c_str());
    return false;
  }
  if (!gNetMutex) netWorkerInit();

  const uint32_t mutexMs = sPreferShortTimeout ? 1500 : 12000;
  const uint32_t timeoutMs =
      sPreferShortTimeout ? 2500 : (netWorkerWebUiActive() ? 3500 : 6000);

  if (xSemaphoreTake(gNetMutex, pdMS_TO_TICKS(mutexMs)) != pdTRUE) {
    Serial.println("net: HTTP busy timeout");
    return false;
  }

  HTTPClient http;
  WiFiClient client;
  client.setTimeout(timeoutMs / 1000);
  String url = normalizeUrl(s.url) + path;
  diagHttpBegin(method.c_str(), path.c_str());
  http.setTimeout(timeoutMs);
  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + s.token);
  http.addHeader("Content-Type", "application/json");
  response = "";
  if (method == "GET") {
    httpCode = http.GET();
    if (httpCode > 0) readCappedGetBody(http, response);
  } else if (method == "POST") {
    httpCode = http.POST(body);
    if (httpCode > 0) drainHttpBody(http);
  } else {
    http.end();
    diagHttpEnd();
    xSemaphoreGive(gNetMutex);
    return false;
  }
  http.end();
  diagHttpEnd();
  xSemaphoreGive(gNetMutex);
  return true;
}
