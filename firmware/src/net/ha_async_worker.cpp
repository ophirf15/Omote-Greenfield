#include "net/ha_async_worker.h"
#include "net/ha_client.h"
#include "net/net_worker.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static SemaphoreHandle_t gMx = nullptr;
static TaskHandle_t gTask = nullptr;
static volatile bool gDone = false;
static HaAsyncKind gKind = HaAsyncKind::None;
static HaSettings gSettings;
static String gDomain;
static String gService;
static String gEntityId;
static String gDataJson;
static String gStateOut;
static String gErrorOut;
static bool gOk = false;

static void haAsyncTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!gMx) continue;

    xSemaphoreTake(gMx, portMAX_DELAY);
    const HaAsyncKind kind = gKind;
    const HaSettings settings = gSettings;
    const String domain = gDomain;
    const String service = gService;
    const String entityId = gEntityId;
    const String dataJson = gDataJson;
    xSemaphoreGive(gMx);

    bool ok = false;
    String err;
    String state;
    netWorkerSetPreferShortTimeout(true);
    if (kind == HaAsyncKind::CallService) {
      ok = haCallService(settings, domain, service, entityId, dataJson);
      if (!ok) err = "HA call failed";
    } else if (kind == HaAsyncKind::FetchState) {
      ok = haFetchEntityState(settings, entityId, state, err);
      if (ok && (state.length() == 0 || state == "unavailable" || state == "unknown")) {
        ok = false;
        err = "no state";
      }
    } else if (kind == HaAsyncKind::FetchClimateRaw) {
      ok = haFetchEntityRaw(settings, entityId, state, err);
    }
    netWorkerSetPreferShortTimeout(false);

    xSemaphoreTake(gMx, portMAX_DELAY);
    gOk = ok;
    gErrorOut = err;
    gStateOut = state;
    gDone = true;
    xSemaphoreGive(gMx);
  }
}

void haAsyncInit() {
  if (gMx) return;
  gMx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(haAsyncTask, "ha_async", 5120, nullptr, 1, &gTask, 0);
}

bool haAsyncBusy() {
  if (!gMx) return false;
  xSemaphoreTake(gMx, portMAX_DELAY);
  const bool busy = gKind != HaAsyncKind::None && !gDone;
  xSemaphoreGive(gMx);
  return busy;
}

static bool haAsyncStart(HaAsyncKind kind, const HaSettings &s, const String &domain,
                         const String &service, const String &entityId, const String &dataJson) {
  if (!gMx || !gTask || !s.configured) return false;
  xSemaphoreTake(gMx, portMAX_DELAY);
  if (gKind != HaAsyncKind::None && !gDone) {
    xSemaphoreGive(gMx);
    return false;
  }
  gKind = kind;
  gSettings = s;
  gDomain = domain;
  gService = service;
  gEntityId = entityId;
  gDataJson = dataJson;
  gStateOut = "";
  gErrorOut = "";
  gOk = false;
  gDone = false;
  xSemaphoreGive(gMx);
  xTaskNotifyGive(gTask);
  return true;
}

bool haAsyncStartCallService(const HaSettings &s, const String &domain, const String &service,
                             const String &entityId, const String &dataJson) {
  return haAsyncStart(HaAsyncKind::CallService, s, domain, service, entityId, dataJson);
}

bool haAsyncStartFetchState(const HaSettings &s, const String &entityId) {
  return haAsyncStart(HaAsyncKind::FetchState, s, "", "", entityId, "");
}

bool haAsyncStartFetchClimateRaw(const HaSettings &s, const String &entityId) {
  return haAsyncStart(HaAsyncKind::FetchClimateRaw, s, "", "", entityId, "");
}

bool haAsyncPoll(HaAsyncKind &kindOut, bool &okOut, String &entityIdOut,
                 String &stateOrErrorOut) {
  if (!gMx) return false;
  xSemaphoreTake(gMx, portMAX_DELAY);
  if (!gDone) {
    xSemaphoreGive(gMx);
    return false;
  }
  kindOut = gKind;
  okOut = gOk;
  entityIdOut = gEntityId;
  stateOrErrorOut =
      gOk && (gKind == HaAsyncKind::FetchState || gKind == HaAsyncKind::FetchClimateRaw) ? gStateOut
                                                                                         : gErrorOut;
  gKind = HaAsyncKind::None;
  gDone = false;
  xSemaphoreGive(gMx);
  return true;
}
