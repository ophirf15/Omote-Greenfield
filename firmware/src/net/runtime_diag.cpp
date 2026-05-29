#include "net/runtime_diag.h"
#include "net/debug_log.h"
#include "net/ha_websocket.h"
#include <ArduinoJson.h>

static const char *sStage = "boot";
static uint32_t sStageSince = 0;
static uint32_t sLastLoopMs = 0;
static uint32_t sLoopCount = 0;
static bool sInLvglEvent = false;
static const char *sLvglEvent = "";
static bool sHttpBusy = false;
static const char *sHttpMethod = "";
static const char *sHttpPath = "";
static uint32_t sHttpSince = 0;
static bool sFlushPending = false;
static uint32_t sFlushSince = 0;
static bool sTogglePending = false;
static bool sRefreshPending = false;
static uint32_t sLastStallReportMs = 0;

void diagSetStage(const char *stage) {
  if (!stage) stage = "?";
  if (sStage != stage) {
    sStage = stage;
    sStageSince = millis();
  }
}

void diagLoopHeartbeat() {
  sLastLoopMs = millis();
  sLoopCount++;
}

void diagLvglEventEnter(const char *name) {
  sInLvglEvent = true;
  sLvglEvent = name ? name : "?";
}

void diagLvglEventLeave() {
  sInLvglEvent = false;
  sLvglEvent = "";
}

void diagHttpBegin(const char *method, const char *path) {
  sHttpBusy = true;
  sHttpMethod = method ? method : "?";
  sHttpPath = path ? path : "";
  sHttpSince = millis();
}

void diagHttpEnd() {
  sHttpBusy = false;
  sHttpMethod = "";
  sHttpPath = "";
}

void diagFlushBegin() {
  sFlushPending = true;
  sFlushSince = millis();
}

void diagFlushEnd() {
  sFlushPending = false;
}

void diagSetTogglePending(bool pending) { sTogglePending = pending; }
void diagSetDisplayRefreshPending(bool pending) { sRefreshPending = pending; }

bool diagHttpBusy() { return sHttpBusy; }

String diagSnapshotJson() {
  const uint32_t now = millis();
  JsonDocument doc;
  doc["stage"] = sStage;
  doc["stage_ms"] = sStageSince ? (now - sStageSince) : 0;
  doc["last_loop_ms"] = sLastLoopMs ? (now - sLastLoopMs) : 0;
  doc["loop_count"] = sLoopCount;
  doc["in_lvgl_event"] = sInLvglEvent;
  if (sInLvglEvent) doc["lvgl_event"] = sLvglEvent;
  doc["http_busy"] = sHttpBusy;
  if (sHttpBusy) {
    doc["http_method"] = sHttpMethod;
    doc["http_path"] = sHttpPath;
    doc["http_ms"] = now - sHttpSince;
  }
  doc["flush_pending"] = sFlushPending;
  if (sFlushPending) doc["flush_ms"] = now - sFlushSince;
  doc["toggle_pending"] = sTogglePending;
  doc["refresh_pending"] = sRefreshPending;
  doc["ha_ws_connected"] = haWsIsConnected();
  doc["ha_ws_connecting"] = haWsIsConnecting();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["max_alloc_heap"] = ESP.getMaxAllocHeap();
  doc["min_free_heap"] = ESP.getMinFreeHeap();
  String out;
  serializeJson(doc, out);
  return out;
}

void diagMaybeReportStall() {
  const uint32_t now = millis();
  if (sLastLoopMs == 0) return;
  const uint32_t idle = now - sLastLoopMs;
  if (idle < 2500) return;
  if (now - sLastStallReportMs < 5000) return;
  sLastStallReportMs = now;
  char line[160];
  snprintf(line, sizeof(line),
           "STALL stage=%s %lums in_lvgl=%d evt=%s http=%d flush=%d toggle=%d heap=%u",
           sStage, (unsigned long)idle, sInLvglEvent ? 1 : 0,
           sInLvglEvent ? sLvglEvent : "-", sHttpBusy ? 1 : 0, sFlushPending ? 1 : 0,
           sTogglePending ? 1 : 0, (unsigned)ESP.getFreeHeap());
  Serial.println(line);
  debugLogAppend(line);
}
