#pragma once

#include "config/config_store.h"
#include <ArduinoJson.h>
#include <vector>

void haStateStoreInit();
void haStateStoreClear();

/** Update from HA state object fields. */
void haStateStoreUpdate(const String &entityId, const String &state);
void haStateStoreUpdateFromJson(const char *entityId, JsonObject stateObj);

/** User tapped toggle — trust until HA event or timeout. */
void haStateStoreMarkOptimistic(const String &entityId, bool wantOn, uint32_t holdMs = 8000);
bool haStateStoreIsOptimistic(const String &entityId);

bool haStateStoreHas(const String &entityId);
bool haStateStoreGetIsOn(const String &entityId, bool &isOnOut);
bool haStateStoreGetStateText(const String &entityId, String &stateOut);

/** Domain-aware ON detection (light, switch, cover, lock, climate, …). */
bool haStateStoreComputeIsOn(const String &entityId, const String &state);

/** Per-entity REST GET (safe on ESP32; never downloads all HA states). */
bool haStateStoreRefreshBatch(const HaSettings &s, const std::vector<String> &entityIds);

/** Queue entities for one-at-a-time refresh on the main loop (low RAM). */
void haStateStoreQueueBootstrap(const std::vector<String> &entityIds);
/** Fetch next queued entity if heap allows. Returns false when queue empty. */
bool haStateStoreBootstrapTick(const HaSettings &s);
void haStateStoreAbortBootstrap();

/** Called from HA worker when store changes — UI applies on next loop. */
void haStateStoreSetDirty();
bool haStateStoreConsumeDirty();
