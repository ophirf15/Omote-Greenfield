#include "hal/power_btn_hal.h"
#include "hal/pins.h"

#ifndef OMOTE_POWER_BTN_GPIO
#define OMOTE_POWER_BTN_GPIO PIN_POWER_BTN
#endif

static bool sLastPressed = false;
static unsigned long sDebounceMs = 0;

void powerBtnInit() {
#if OMOTE_POWER_BTN_GPIO >= 0
  pinMode(OMOTE_POWER_BTN_GPIO, INPUT_PULLUP);
#endif
}

void powerBtnLoop(PowerBtnCallback onChange) {
#if OMOTE_POWER_BTN_GPIO >= 0
  if (!onChange) return;
  const bool pressed = digitalRead(OMOTE_POWER_BTN_GPIO) == LOW;
  const unsigned long now = millis();
  if (pressed != sLastPressed && now - sDebounceMs > 40) {
    sDebounceMs = now;
    sLastPressed = pressed;
    onChange(KEY_POWER, pressed);
  }
#endif
}
