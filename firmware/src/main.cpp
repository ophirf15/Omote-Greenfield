#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>
#include "actions/action_executor.h"
#include "config/config_store.h"
#include "hal/ble_hal.h"
#include "hal/battery_hal.h"
#include "hal/display_hal.h"
#include "hal/pins.h"
#include "hal/ir_hal.h"
#include "hal/keypad_hal.h"
#include "hal/power_btn_hal.h"
#include "hal/sleep_hal.h"
#include "hal/wifi_hal.h"
#include "net/debug_log.h"
#include "net/http_server.h"
#include "net/net_worker.h"
#include "net/runtime_diag.h"
#include "net/time_sync.h"
#include "ui_runtime/page_engine.h"

static HaSettings gHaSettings;
static OmoteConfig gConfig;
static DeviceSettings gDevSettings;
static bool setupMode = false;
static bool servicesStarted = false;
static bool wifiConnectPending = false;

static void printBootReason() {
  esp_reset_reason_t r = esp_reset_reason();
  const char *reason = "unknown";
  switch (r) {
    case ESP_RST_POWERON:
      reason = "power-on";
      break;
    case ESP_RST_DEEPSLEEP:
      reason = "deep-sleep wake (full reboot)";
      break;
    case ESP_RST_SW:
      reason = "software";
      break;
    case ESP_RST_PANIC:
      reason = "panic";
      break;
    case ESP_RST_BROWNOUT:
      reason = "brownout";
      break;
    default:
      break;
  }
  Serial.printf("Reset reason: %s (%d)\n", reason, (int)r);
}

static bool mountLittleFS() {
  if (LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("LittleFS mounted");
    return true;
  }
  Serial.println("LittleFS mount failed");
  return false;
}

static void checkCrashLoopAndRecover() {
  RTC_DATA_ATTR static uint32_t bootCount = 0;
  RTC_DATA_ATTR static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs > 60000) bootCount = 0;
  lastMs = now;
  bootCount++;
  if (esp_reset_reason() == ESP_RST_PANIC && bootCount >= 4) {
    Serial.println("Crash loop detected — clearing WiFi credentials");
    if (LittleFS.exists("/wifi.json")) LittleFS.remove("/wifi.json");
    bootCount = 0;
  }
}

static void onConfigChanged() {
  gActions.begin(&gHaSettings, &gConfig);
  pageEngineSetHaSettings(&gHaSettings);
  pageEngineRequestReload();
}

static void onKeypad(char key, bool pressed) {
  if (pressed && key) sleepNotifyActivity();
  if (key) httpServerPublishKey(key, pressed);
  pageEngineHandleKey(key, pressed);
}

static void startNetworkServices() {
  if (servicesStarted) return;
  servicesStarted = true;
  delay(300);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  timeStartSync(gDevSettings.timezone, gDevSettings.ntpServer);
  httpServerBegin(gHaSettings, gConfig, gDevSettings, onConfigChanged);
  Serial.println("Ready — http://omote.local or device IP");
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nOmote OS starting...");
  debugLogInit();
  debugLogAppend("Omote OS boot");
  printBootReason();
  checkCrashLoopAndRecover();

  mountLittleFS();
  netWorkerInit();
  sleepInitWakeup();

  pinMode(PIN_USER_LED, OUTPUT);
  digitalWrite(PIN_USER_LED, LOW);

  displayInit();
  batteryInit();
  keypadInit();
  powerBtnInit();
  irInit();
  sleepInitImu();

  settingsLoad(gHaSettings);
  if (!deviceSettingsLoad(gDevSettings)) {
    gDevSettings = DeviceSettings{};
    deviceSettingsSave(gDevSettings);
  }
  sleepSetUserBrightness(gDevSettings.brightness);
  sleepSetDisplayTimeoutMs(gDevSettings.displayTimeoutMs);
  sleepSetDeepSleepTimeoutMs(gDevSettings.deepSleepTimeoutMs);
  sleepSetMotionWake(gDevSettings.motionWake);

  if (!configLoad(gConfig)) {
    Serial.println("config: boot load failed");
    if (!LittleFS.exists("/config.json")) {
      configSave(gConfig);
    }
  }

  gActions.begin(&gHaSettings, &gConfig);
  pageEngineSetDeviceSettings(&gDevSettings);

  WifiCreds wc;
  if (!wifiLoadCreds(wc)) {
    setupMode = true;
    wifiStartPortal("Omote-Setup");
  } else {
    delay(200);
    bleSetProfile(gDevSettings.bleProfile);
    bleInit();
    /* Treat every cold boot (including deep-sleep wake) as a wake event so
     * the bonded TV can re-discover us via open undirected adv. */
    bleOnWake();
    wifiPrepareCoexistence();
    wifiConnectPending = true;
    wifiBeginConnect();
  }

  pageEngineSetHaSettings(&gHaSettings);
  pageEngineInit(&gConfig, &gActions);
}

void loop() {
  diagLoopHeartbeat();
  diagSetStage("loop");
  displayUpdateBacklight();

  // Wake path: scan keys before UI work when the backlight is off.
  if (displayIsOff()) {
    keypadLoop(onKeypad);
    powerBtnLoop(onKeypad);
  }

  if (setupMode) {
    wifiPortalLoop();
    pageEngineLoop();
    bleTaskLoop();
    delay(5);
    return;
  }

  if (wifiConnectPending) {
    if (wifiPollConnect(30000)) {
      wifiConnectPending = false;
      pageEngineReload();
      displayRefreshNow();
    } else if (wifiUiState() == WIFI_UI_FAILED) {
      Serial.println("WiFi failed — opening setup AP");
      wifiConnectPending = false;
      setupMode = true;
      wifiStartPortal("Omote-Setup");
      pageEngineReload();
      delay(5);
      return;
    }
    pageEngineLoop();
    bleTaskLoop();
    delay(50);
    return;
  }

  if (!servicesStarted && WiFi.status() == WL_CONNECTED) {
    static uint32_t connectedSince = 0;
    if (connectedSince == 0) connectedSince = millis();
    if (millis() - connectedSince > 800) startNetworkServices();
  }

  diagSetStage("ui");
  pageEngineLoop();
  if (!displayIsOff()) {
    keypadLoop(onKeypad);
    powerBtnLoop(onKeypad);
  }
  diagSetStage("http");
  httpServerLoop();
  diagSetStage("ha_net");
  pageEngineLoopNetwork();
  diagSetStage("ntp");
  timeSyncLoop();
  bleTaskLoop();
  diagSetStage("loop");
  diagMaybeReportStall();

  static uint32_t activityTimer = 0;
  const uint32_t activityInterval = displayIsOff() ? 50 : 100;
  if (millis() - activityTimer >= activityInterval) {
    activityTimer = millis();
    sleepCheckActivity();
  }

  static uint32_t lastBat = 0;
  if (!displayIsOff() && millis() - lastBat > 30000) {
    lastBat = millis();
    batteryUpdate();
  }
  delay(2);
}
