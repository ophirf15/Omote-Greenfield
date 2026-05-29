#include "net/ha_state_store.h"
#include "net/ha_client.h"
#include "net/ha_async_worker.h"
#include "net/net_worker.h"
#include "net/net_heap.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

struct StoreEntry {
  String entityId;
  String state;
  uint32_t updatedMs = 0;
  uint32_t optimisticUntil = 0;
  bool optimisticOn = false;
};

static std::vector<StoreEntry> gEntries;
static std::vector<String> gBootstrapQueue;
static SemaphoreHandle_t gMx = nullptr;
static volatile bool gDirty = false;
static constexpr size_t kMaxEntries = 32;

static bool takeMx(uint32_t ms = 50) {
  if (!gMx) haStateStoreInit();
  return xSemaphoreTake(gMx, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void giveMx() {
  if (gMx) xSemaphoreGive(gMx);
}

static StoreEntry *findEntry(const String &entityId) {
  for (auto &e : gEntries) {
    if (e.entityId == entityId) return &e;
  }
  return nullptr;
}

static String domainOf(const String &entityId) {
  const int dot = entityId.indexOf('.');
  return dot > 0 ? entityId.substring(0, dot) : String();
}

void haStateStoreInit() {
  if (!gMx) gMx = xSemaphoreCreateMutex();
}

void haStateStoreClear() {
  if (!takeMx()) return;
  gEntries.clear();
  gDirty = false;
  giveMx();
}

void haStateStoreSetDirty() { gDirty = true; }

bool haStateStoreConsumeDirty() {
  if (!gDirty) return false;
  gDirty = false;
  return true;
}

bool haStateStoreComputeIsOn(const String &entityId, const String &state) {
  String s = state;
  s.trim();
  s.toLowerCase();
  if (s == "unavailable" || s == "unknown") return false;

  const String dom = domainOf(entityId);
  if (dom == "cover") return s == "open" || s == "opening";
  if (dom == "lock") return s == "unlocked";
  if (dom == "climate") return s != "off";
  if (dom == "valve") return s == "open";
  if (dom == "binary_sensor") return s == "on";
  return haStateIsOn(state);
}

void haStateStoreUpdate(const String &entityId, const String &state) {
  if (entityId.length() == 0) return;
  if (!takeMx()) return;

  StoreEntry *e = findEntry(entityId);
  if (!e) {
    if (gEntries.size() >= kMaxEntries) gEntries.erase(gEntries.begin());
    StoreEntry ent;
    ent.entityId = entityId;
    ent.state = state;
    ent.updatedMs = millis();
    gEntries.push_back(ent);
  } else {
    e->state = state;
    e->updatedMs = millis();
    if (millis() >= e->optimisticUntil) e->optimisticUntil = 0;
  }
  gDirty = true;
  giveMx();
}

void haStateStoreUpdateFromJson(const char *entityId, JsonObject stateObj) {
  if (!entityId || !entityId[0]) return;
  const String state = stateObj["state"].as<String>();
  haStateStoreUpdate(String(entityId), state);
}

void haStateStoreMarkOptimistic(const String &entityId, bool wantOn, uint32_t holdMs) {
  if (entityId.length() == 0) return;
  if (!takeMx()) return;

  StoreEntry *e = findEntry(entityId);
  if (!e) {
    if (gEntries.size() >= kMaxEntries) gEntries.erase(gEntries.begin());
    StoreEntry ent;
    ent.entityId = entityId;
    ent.state = wantOn ? "on" : "off";
    ent.updatedMs = millis();
    ent.optimisticUntil = millis() + holdMs;
    ent.optimisticOn = wantOn;
    gEntries.push_back(ent);
  } else {
    e->optimisticUntil = millis() + holdMs;
    e->optimisticOn = wantOn;
    e->state = wantOn ? "on" : "off";
  }
  gDirty = true;
  giveMx();
}

bool haStateStoreIsOptimistic(const String &entityId) {
  if (!takeMx()) return false;
  const StoreEntry *e = findEntry(entityId);
  const bool opt = e && millis() < e->optimisticUntil;
  giveMx();
  return opt;
}

bool haStateStoreHas(const String &entityId) {
  if (!takeMx()) return false;
  const bool has = findEntry(entityId) != nullptr;
  giveMx();
  return has;
}

bool haStateStoreGetIsOn(const String &entityId, bool &isOnOut) {
  if (!takeMx()) return false;
  const StoreEntry *e = findEntry(entityId);
  if (!e) {
    giveMx();
    return false;
  }
  if (millis() < e->optimisticUntil) {
    isOnOut = e->optimisticOn;
    giveMx();
    return true;
  }
  isOnOut = haStateStoreComputeIsOn(entityId, e->state);
  giveMx();
  return true;
}

bool haStateStoreGetStateText(const String &entityId, String &stateOut) {
  if (!takeMx()) return false;
  const StoreEntry *e = findEntry(entityId);
  if (!e) {
    giveMx();
    return false;
  }
  stateOut = e->state;
  giveMx();
  return true;
}

bool haStateStoreRefreshBatch(const HaSettings &s, const std::vector<String> &entityIds) {
  if (!s.configured || entityIds.empty()) return false;

  /* Never GET /api/states (entire HA) on ESP32 — it can be MB and trips the WDT. */
  netWorkerSetPreferShortTimeout(true);
  size_t updated = 0;
  for (const auto &eid : entityIds) {
    if (eid.length() == 0) continue;
    String state, err;
    if (haFetchEntityState(s, eid, state, err) && state.length() && state != "unavailable" &&
        state != "unknown") {
      haStateStoreUpdate(eid, state);
      updated++;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    yield();
  }
  netWorkerSetPreferShortTimeout(false);
  Serial.printf("ha_store: refreshed %u/%u entities\n", (unsigned)updated,
                (unsigned)entityIds.size());
  return updated > 0;
}

static bool entityListsEqual(const std::vector<String> &a, const std::vector<String> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

void haStateStoreQueueBootstrap(const std::vector<String> &entityIds) {
  if (entityListsEqual(entityIds, gBootstrapQueue) && !gBootstrapQueue.empty()) return;
  gBootstrapQueue = entityIds;
}

void haStateStoreAbortBootstrap() { gBootstrapQueue.clear(); }

bool haStateStoreBootstrapTick(const HaSettings &s) {
  if (gBootstrapQueue.empty()) return false;
  if (!netHeapOkForHaGet()) return true;
  if (netWorkerWebUiActive(8000)) return true;
  if (haAsyncBusy()) return true;

  const String eid = gBootstrapQueue.front();
  gBootstrapQueue.erase(gBootstrapQueue.begin());
  if (!haAsyncStartFetchState(s, eid)) {
    gBootstrapQueue.insert(gBootstrapQueue.begin(), eid);
    return true;
  }
  return !gBootstrapQueue.empty();
}
