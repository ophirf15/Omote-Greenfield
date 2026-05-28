#include "hal/battery_hal.h"
#include "hal/omote_i2c.h"
#include "hal/pins.h"
#include <Arduino.h>

#if OMOTE_HARDWARE_REV >= 4
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
static SFE_MAX1704X fuelGauge(MAX1704X_MAX17048);
#define PIN_CRG_STAT 21
#endif

static int sPercent = 100;
static bool sCharging = false;
static int sVoltageMv = 4200;
static bool sGaugeOk = false;

static void batteryUpdateAdc() {
  int raw = analogRead(36);
  sVoltageMv = raw * 2 * 3350 / 4095 + 325;
  sPercent = constrain(map(sVoltageMv, 3700, 4200, 0, 100), 0, 100);
  sCharging = false;
}

void batteryInit() {
#if OMOTE_HARDWARE_REV >= 4
  pinMode(PIN_CRG_STAT, INPUT_PULLUP);
  omoteI2cInit();
  if (fuelGauge.begin()) {
    sGaugeOk = true;
    Serial.println("MAX17048 OK");
  } else {
    sGaugeOk = false;
    Serial.println("MAX17048 unavailable — ADC fallback");
    pinMode(36, INPUT);
  }
#else
  pinMode(36, INPUT);
#endif
}

void batteryUpdate() {
#if OMOTE_HARDWARE_REV >= 4
  if (sGaugeOk) {
    sVoltageMv = (int)(fuelGauge.getVoltage() * 1000);
    float soc = fuelGauge.getSOC();
    if (soc > 100.0f) soc = 100.0f;
    if (soc < 0.0f) soc = 0.0f;
    sPercent = (int)soc;
    sCharging = !digitalRead(PIN_CRG_STAT);
    return;
  }
#endif
  batteryUpdateAdc();
}

int batteryPercent() { return sPercent; }
bool batteryCharging() { return sCharging; }
int batteryVoltageMv() { return sVoltageMv; }
