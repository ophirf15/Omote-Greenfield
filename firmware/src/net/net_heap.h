#pragma once

#include <Arduino.h>

/** Typical free heap with WiFi + BLE + LVGL is ~8–12 KB — gates must match reality. */
static constexpr uint32_t kNetMinFreeHeap = 6500;
static constexpr uint32_t kNetMinMaxAllocGet = 3200;
static constexpr uint32_t kNetMinMaxAllocPost = 2048;
static constexpr uint32_t kNetComfortableHeap = 10500;

inline bool netHeapOkForHaGet() {
  return ESP.getFreeHeap() >= kNetMinFreeHeap &&
         ESP.getMaxAllocHeap() >= kNetMinMaxAllocGet;
}

/** Small HA POST (service calls) — fragmented heap often has <3KB max block. */
inline bool netHeapOkForHaPost() {
  return ESP.getFreeHeap() >= 6000 && ESP.getMaxAllocHeap() >= kNetMinMaxAllocPost;
}

inline bool netHeapOkForHa() { return netHeapOkForHaGet(); }

inline bool netHeapComfortable() { return ESP.getFreeHeap() >= kNetComfortableHeap; }
