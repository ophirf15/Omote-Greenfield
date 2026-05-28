/**
 * Display off after displayTimeout (WiFi stays on). Deep sleep after deepSleepTimeout.
 */
#include "hal/sleep_hal.h"
#include "hal/display_hal.h"
#include "hal/omote_i2c.h"
// Touch + fuel gauge share this bus; avoid hammering the IMU while the screen is off.
#include "hal/pins.h"
#include <SparkFunLIS3DH.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

static const int kDefaultMotionThreshold = 35;
/** Screen-off wake only — small bump filter; does not affect "stay awake while holding". */
static const int kScreenOffMotionDeltaMin = 55;
static const uint32_t kDefaultDisplayTimeoutMs = 60000;
static const uint32_t kDefaultDeepSleepTimeoutMs = 15UL * 60UL * 1000UL;
static const uint32_t kDefaultDimLeadMs = 2000;
static const uint32_t kMinDeepAfterDisplayMs = 60000;

static LIS3DH sImu(I2C_MODE, 0x19);
static uint32_t sLastActivityMs = 0;
static uint32_t sDisplayTimeoutMs = kDefaultDisplayTimeoutMs;
static uint32_t sDeepSleepTimeoutMs = kDefaultDeepSleepTimeoutMs;
static uint32_t sDimLeadMs = kDefaultDimLeadMs;
static bool sWakeupByImu = true;
static uint8_t sMotionThreshold = kDefaultMotionThreshold;
static uint8_t sBacklightBrightness = 180;
static bool sImuOk = false;
static bool sLoggedDim = false;
static bool sMotionBaselineReset = false;
static uint32_t sLastScreenOffMotionWakeMs = 0;
static uint32_t sDisplayOffSinceMs = 0;
static uint32_t sLastOffImuPollMs = 0;
static bool sScreenOffIntReady = false;
static int sOffAccX = 0, sOffAccY = 0, sOffAccZ = 0;
static const uint32_t kScreenOffMotionGraceMs = 600;
static const uint32_t kScreenOffImuPollMs = 150;

static void configImuInterruptsBeforeSleep();

static void clampDeepSleepTimeout() {
  const uint32_t minDeep = sDisplayTimeoutMs + kMinDeepAfterDisplayMs;
  if (sDeepSleepTimeoutMs < minDeep) sDeepSleepTimeoutMs = minDeep;
}

static void setLastActivity() {
  sLastActivityMs = millis();
  sLoggedDim = false;
  if (displayIsOff()) {
    displayPowerOn();
    Serial.println("Screen on (activity)");
  }
}

static void activityDetection() {
  if (!sImuOk || !sWakeupByImu) return;
  if (sleepInPreSleepDimPhase()) return;
  static int accXold = 0, accYold = 0, accZold = 0;
  static uint32_t lastMotionLog = 0;

  if (displayIsOff()) {
    const uint32_t now = millis();
    if (now - sDisplayOffSinceMs < kScreenOffMotionGraceMs) return;
    if (now - sLastOffImuPollMs < kScreenOffImuPollMs) return;
    sLastOffImuPollMs = now;
    omoteI2cSetClock(100000);
    uint8_t int1src = 0;
    sImu.readRegister(&int1src, LIS3DH_INT1_SRC);
    if ((int1src & 0x40) == 0) {
      sScreenOffIntReady = true;
      return;
    }
    if (!sScreenOffIntReady || now - sLastScreenOffMotionWakeMs <= 400) return;

    int accX = (int)(sImu.readFloatAccelX() * 1000);
    int accY = (int)(sImu.readFloatAccelY() * 1000);
    int accZ = (int)(sImu.readFloatAccelZ() * 1000);
    const int delta =
        abs(sOffAccX - accX) + abs(sOffAccY - accY) + abs(sOffAccZ - accZ);
    if (delta < kScreenOffMotionDeltaMin) return;

    sLastScreenOffMotionWakeMs = now;
    sScreenOffIntReady = false;
    setLastActivity();
    Serial.printf("Motion wake while off: INT1=0x%02X delta=%d\n", int1src, delta);
    return;
  }

  omoteI2cSetClock(400000);
  int accX = (int)(sImu.readFloatAccelX() * 1000);
  int accY = (int)(sImu.readFloatAccelY() * 1000);
  int accZ = (int)(sImu.readFloatAccelZ() * 1000);

  if (sMotionBaselineReset) {
    accXold = accX;
    accYold = accY;
    accZold = accZ;
    sMotionBaselineReset = false;
    return;
  }

  int motion = abs(accXold - accX) + abs(accYold - accY) + abs(accZold - accZ);
  accXold = accX;
  accYold = accY;
  accZold = accZ;
  if (motion > sMotionThreshold) {
    setLastActivity();
    uint32_t now = millis();
    if (now - lastMotionLog > 1500) {
      lastMotionLog = now;
      Serial.printf("Motion activity: %d (xyz %d,%d,%d)\n", motion, accX, accY, accZ);
    }
  }
}

void sleepOnDisplayPoweredOff() {
  sMotionBaselineReset = true;
  sLastScreenOffMotionWakeMs = 0;
  sDisplayOffSinceMs = millis();
  sScreenOffIntReady = false;
  if (sImuOk) {
    omoteI2cSetClock(100000);
    uint8_t junk = 0;
    for (int i = 0; i < 4; i++) {
      sImu.readRegister(&junk, LIS3DH_INT1_SRC);
      if ((junk & 0x40) == 0) break;
      delay(5);
    }
    sOffAccX = (int)(sImu.readFloatAccelX() * 1000);
    sOffAccY = (int)(sImu.readFloatAccelY() * 1000);
    sOffAccZ = (int)(sImu.readFloatAccelZ() * 1000);
    omoteI2cSetClock(400000);
    Serial.printf("Screen off: INT1_SRC=0x%02X baseline xyz %d,%d,%d\n", junk, sOffAccX,
                  sOffAccY, sOffAccZ);
  }
}

static void configImuInterruptsBeforeSleep() {
  if (!sImuOk) return;

  if (sWakeupByImu) {
    sImu.writeRegister(LIS3DH_INT1_CFG, 0b00101010);
  } else {
    sImu.writeRegister(LIS3DH_INT1_CFG, 0b00000000);
    return;
  }

  sImu.writeRegister(LIS3DH_INT1_THS, 0x45);
  sImu.writeRegister(LIS3DH_INT1_DURATION, 0x00);

  uint8_t dataToWrite = 0;
  sImu.readRegister(&dataToWrite, LIS3DH_CTRL_REG5);
  dataToWrite &= 0xF3;
  dataToWrite |= 0x08;
  sImu.writeRegister(LIS3DH_CTRL_REG5, dataToWrite);
  sImu.writeRegister(LIS3DH_CTRL_REG6, 0x00);
  sImu.writeRegister(LIS3DH_CTRL_REG3, 0x40);
}

static bool armImuForSleepWake() {
  if (!sImuOk || !sWakeupByImu) return false;

  Wire.setClock(100000);
  delay(2);

  uint8_t junk = 0;
  sImu.readRegister(&junk, LIS3DH_INT1_SRC);
  configImuInterruptsBeforeSleep();
  sImu.readRegister(&junk, LIS3DH_INT1_SRC);

  uint8_t int1cfg = 0;
  sImu.readRegister(&int1cfg, LIS3DH_INT1_CFG);
  uint8_t int1src = 0;
  sImu.readRegister(&int1src, LIS3DH_INT1_SRC);

  pinMode(PIN_ACC_INT, INPUT);
  Serial.printf("LIS3DH armed: INT1_CFG=0x%02X INT1_SRC=0x%02X GPIO%d=%d\n", int1cfg, int1src,
                PIN_ACC_INT, digitalRead(PIN_ACC_INT));

  if (int1cfg != 0x2A) {
    Serial.println("LIS3DH sleep arm FAILED (INT1_CFG mismatch)");
    return false;
  }
  return true;
}

static void prepareAccIntExt0Wake() {
  rtc_gpio_init((gpio_num_t)PIN_ACC_INT);
  rtc_gpio_set_direction((gpio_num_t)PIN_ACC_INT, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_ACC_INT);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_ACC_INT);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_ACC_INT, 1);
}

static void enterDeepSleep() {
  Serial.println("Entering deep sleep (WiFi off). Goodbye.");

  const bool imuArmed = armImuForSleepWake();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  displayPowerOff();
  pinMode(PIN_LCD_EN, OUTPUT);
  digitalWrite(PIN_LCD_EN, HIGH);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  digitalWrite(PIN_LCD_DC, LOW);
  digitalWrite(PIN_LCD_CS, LOW);
  digitalWrite(PIN_LCD_MOSI, LOW);
  digitalWrite(PIN_LCD_SCK, LOW);

  pinMode(PIN_IR_RX_PWR, OUTPUT);
  digitalWrite(PIN_IR_RX_PWR, LOW);

  pinMode(PIN_KP_OUT_1, OUTPUT);
  pinMode(PIN_KP_OUT_2, OUTPUT);
  pinMode(PIN_KP_OUT_3, OUTPUT);
  pinMode(PIN_KP_OUT_4, OUTPUT);
  pinMode(PIN_KP_OUT_5, OUTPUT);
  digitalWrite(PIN_KP_OUT_1, HIGH);
  digitalWrite(PIN_KP_OUT_2, HIGH);
  digitalWrite(PIN_KP_OUT_3, HIGH);
  digitalWrite(PIN_KP_OUT_4, HIGH);
  digitalWrite(PIN_KP_OUT_5, HIGH);
  gpio_hold_en((gpio_num_t)PIN_KP_OUT_1);
  gpio_hold_en((gpio_num_t)PIN_KP_OUT_2);
  gpio_hold_en((gpio_num_t)PIN_KP_OUT_3);
  gpio_hold_en((gpio_num_t)PIN_KP_OUT_4);
  gpio_hold_en((gpio_num_t)PIN_KP_OUT_5);

  pinMode(PIN_LCD_BL, INPUT);
  pinMode(PIN_LCD_EN, INPUT);
  gpio_hold_en((gpio_num_t)PIN_LCD_BL);
  gpio_hold_en((gpio_num_t)PIN_LCD_EN);
  gpio_deep_sleep_hold_en();

  esp_sleep_enable_ext1_wakeup(PIN_KEYPAD_WAKE_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);

  if (imuArmed) {
    prepareAccIntExt0Wake();
    Serial.println("Sleep: ext1 keypad + ext0 motion (GPIO13)");
  } else {
    Serial.println("Sleep: ext1 keypad only");
  }

  delay(100);
  esp_deep_sleep_start();
}

void sleepInitWakeup() {
  if (sDisplayTimeoutMs == 0) sDisplayTimeoutMs = kDefaultDisplayTimeoutMs;
  if (sDeepSleepTimeoutMs == 0) sDeepSleepTimeoutMs = kDefaultDeepSleepTimeoutMs;
  if (sMotionThreshold == 0) sMotionThreshold = kDefaultMotionThreshold;
  clampDeepSleepTimeout();

  displayResetBacklightFade();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Wake: motion (IMU / GPIO13 ext0)");
  } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    const uint64_t mask = esp_sleep_get_ext1_wakeup_status();
    if (mask == (1ULL << PIN_KP_IN_B)) {
      Serial.println("Wake: keypad row B (lift/vibration — not IMU)");
    } else {
      Serial.printf("Wake: keypad (ext1 mask=0x%llx)\n", (unsigned long long)mask);
    }
  } else if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.printf("Wake: cause %d\n", (int)cause);
  }

  pinMode(PIN_ACC_INT, INPUT);
  gpio_hold_dis((gpio_num_t)PIN_KP_OUT_1);
  gpio_hold_dis((gpio_num_t)PIN_KP_OUT_2);
  gpio_hold_dis((gpio_num_t)PIN_KP_OUT_3);
  gpio_hold_dis((gpio_num_t)PIN_KP_OUT_4);
  gpio_hold_dis((gpio_num_t)PIN_KP_OUT_5);
  gpio_hold_dis((gpio_num_t)PIN_LCD_EN);
  gpio_hold_dis((gpio_num_t)PIN_LCD_BL);
  gpio_deep_sleep_hold_dis();

  setLastActivity();
}

void sleepInitImu() {
  omoteI2cInit();
  sImu.settings.accelSampleRate = 50;
  sImu.settings.accelRange = 2;
  sImu.settings.adcEnabled = 0;
  sImu.settings.tempEnabled = 0;
  sImu.settings.xAccelEnabled = 1;
  sImu.settings.yAccelEnabled = 1;
  sImu.settings.zAccelEnabled = 1;
  if (sImu.begin()) {
    sImuOk = true;
    uint8_t junk = 0;
    sImu.readRegister(&junk, LIS3DH_INT1_SRC);
    // Keep ACC INT configured while awake so motion can wake from "screen off" mode.
    configImuInterruptsBeforeSleep();
    Serial.println("LIS3DH OK");
  } else {
    sImuOk = false;
    Serial.println("LIS3DH unavailable");
  }
}

void sleepNotifyActivity() { setLastActivity(); }

void sleepCheckActivity() {
  activityDetection();

  const uint32_t idle = millis() - sLastActivityMs;
  static uint32_t lastStateLog = 0;
  const uint32_t now = millis();
  if (now - lastStateLog > 5000) {
    lastStateLog = now;
    Serial.printf("Sleep state: idle=%lus disp_to=%lus deep_to=%lus display_off=%d\n",
                  (unsigned long)(idle / 1000), (unsigned long)(sDisplayTimeoutMs / 1000),
                  (unsigned long)(sDeepSleepTimeoutMs / 1000), displayIsOff() ? 1 : 0);
  }

  if (!displayIsOff()) {
    if (sDisplayTimeoutMs > sDimLeadMs && idle >= sDisplayTimeoutMs - sDimLeadMs &&
        idle < sDisplayTimeoutMs && !sLoggedDim) {
      sLoggedDim = true;
      displayEnterPreSleepDim();
      const uint32_t secLeft = (sDisplayTimeoutMs - idle + 999) / 1000;
      Serial.printf("Dimming (%lu s until screen off)\n", (unsigned long)secLeft);
    }
    if (idle >= sDisplayTimeoutMs) {
      displayPowerOff();
      Serial.printf("Screen off — WiFi stays on (EN=%d BL=%d)\n", digitalRead(PIN_LCD_EN),
                    digitalRead(PIN_LCD_BL));
      sLoggedDim = false;
    }
  }

  if (idle >= sDeepSleepTimeoutMs) {
    enterDeepSleep();
  }
}

void sleepSetDisplayTimeoutMs(uint32_t ms) {
  sDisplayTimeoutMs = ms;
  if (sDisplayTimeoutMs == 0) sDisplayTimeoutMs = kDefaultDisplayTimeoutMs;
  clampDeepSleepTimeout();
}

void sleepSetDeepSleepTimeoutMs(uint32_t ms) {
  sDeepSleepTimeoutMs = ms;
  if (sDeepSleepTimeoutMs == 0) sDeepSleepTimeoutMs = kDefaultDeepSleepTimeoutMs;
  clampDeepSleepTimeout();
}

void sleepSetTimeoutMs(uint32_t ms) { sleepSetDisplayTimeoutMs(ms); }

void sleepSetMotionWake(bool enabled) { sWakeupByImu = enabled; }

void sleepSetUserBrightness(uint8_t brightness) {
  if (brightness < 10) brightness = 10;
  sBacklightBrightness = brightness;
  Serial.printf("Brightness set: %u\n", (unsigned)sBacklightBrightness);
  if (!displayIsOff()) displaySetBacklight(brightness);
}

void sleepEnterDeep() { enterDeepSleep(); }
void sleepDisplayOff() { displayPowerOff(); }

uint32_t sleepGetLastActivityMs() { return sLastActivityMs; }

bool sleepInPreSleepDimPhase() {
  if (displayIsOff()) return false;
  const uint32_t idle = millis() - sLastActivityMs;
  return sDisplayTimeoutMs > sDimLeadMs && idle >= sDisplayTimeoutMs - sDimLeadMs &&
         idle < sDisplayTimeoutMs;
}

uint32_t sleepGetTimeoutMs() { return sDisplayTimeoutMs; }
uint32_t sleepGetDisplayTimeoutMs() { return sDisplayTimeoutMs; }
uint32_t sleepGetDeepSleepTimeoutMs() { return sDeepSleepTimeoutMs; }
uint32_t sleepGetDimLeadMs() { return sDimLeadMs; }
uint8_t sleepGetBacklightBrightness() { return sBacklightBrightness; }
bool sleepMotionWakeEnabled() { return sWakeupByImu; }
