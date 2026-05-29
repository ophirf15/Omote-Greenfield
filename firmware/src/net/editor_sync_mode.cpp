#include "net/editor_sync_mode.h"
#include "hal/ble_hal.h"
#include "hal/sleep_hal.h"
#include "net/ha_state_store.h"
#include "net/ha_websocket.h"
#include "ui_runtime/page_engine.h"
#include <ESP.h>
#include <WiFi.h>
#include <ESPmDNS.h>

static bool sActive = false;
static uint32_t sSavedDisplayTimeoutMs = 0;
static uint32_t sSavedDeepSleepTimeoutMs = 0;

bool editorSyncModeActive() { return sActive; }

bool editorSyncModeEnter() {
  if (sActive) return true;
  Serial.println("editor sync: enter (BLE/HA/input paused)");
  sActive = true;
  haStateStoreAbortBootstrap();
  haWsReleasePressure();
  if (bleIsInitialized()) {
    bleDisconnectClients();
    bleStopAdvertising();
    bleShutdown();
  }
  sSavedDisplayTimeoutMs = sleepGetDisplayTimeoutMs();
  sSavedDeepSleepTimeoutMs = sleepGetDeepSleepTimeoutMs();
  sleepSetDisplayTimeoutMs(30UL * 60UL * 1000UL);
  sleepSetDeepSleepTimeoutMs(0);
  sleepNotifyActivity();
  /* Keep WiFi modem sleep enabled — ESP-IDF aborts on setSleep(false) while the BT
   * controller is still registered for coexistence (even after bleShutdown()). */
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("omote")) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("editor sync: mDNS omote.local (%s)\n", WiFi.localIP().toString().c_str());
    }
  }
  pageEngineEnterEditorSync();
  return true;
}

void editorSyncModeExit(bool reboot) {
  if (!sActive) {
    if (reboot) ESP.restart();
    return;
  }
  Serial.println(reboot ? "editor sync: exit (reboot)" : "editor sync: exit");
  sActive = false;
  sleepSetDisplayTimeoutMs(sSavedDisplayTimeoutMs ? sSavedDisplayTimeoutMs : 60000);
  sleepSetDeepSleepTimeoutMs(sSavedDeepSleepTimeoutMs ? sSavedDeepSleepTimeoutMs : (15UL * 60UL * 1000UL));
  pageEngineExitEditorSync();
  if (reboot) {
    delay(80);
    ESP.restart();
  }
}
