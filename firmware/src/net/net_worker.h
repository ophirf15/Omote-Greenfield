#pragma once

#include "config/config_store.h"
#include <Arduino.h>

struct DeviceMemInfo {
  uint32_t freeHeap = 0;
  uint32_t maxAllocHeap = 0;
  uint32_t minFreeHeap = 0;
};

void netWorkerInit();
void netWorkerTouchWebClient();
bool netWorkerWebUiActive(uint32_t windowMs = 30000);

/** Shorter HA timeouts for on-device button/toggle actions (call around execute). */
void netWorkerSetPreferShortTimeout(bool preferShort);

/** Single-flight HTTP to Home Assistant (mutex). */
bool netWorkerHttpRequest(const HaSettings &s, const String &method, const String &path,
                          const String &body, int &httpCode, String &response);

DeviceMemInfo deviceMemSnapshot();

bool netWorkerLock(uint32_t timeoutMs = 12000);
void netWorkerUnlock();
