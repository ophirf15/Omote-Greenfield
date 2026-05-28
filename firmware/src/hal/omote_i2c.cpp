#include "hal/omote_i2c.h"
#include "hal/pins.h"
#include <Wire.h>

static bool sReady = false;

void omoteI2cInit() {
  if (sReady) return;
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  sReady = true;
}

void omoteI2cSetClock(uint32_t hz) {
  omoteI2cInit();
  Wire.setClock(hz);
}
