#include "hal/power_btn_hal.h"
#include "hal/pins.h"

#ifndef OMOTE_POWER_BTN_GPIO
#define OMOTE_POWER_BTN_GPIO PIN_POWER_BTN
#endif

static bool sLastPressed = false;
static unsigned long sDebounceMs = 0;
static unsigned long sPressStartMs = 0;
static bool sLongPressFired = false;
static constexpr unsigned long kLongPressMs = 2800;

void powerBtnInit() {
#if OMOTE_POWER_BTN_GPIO >= 0
  pinMode(OMOTE_POWER_BTN_GPIO, INPUT_PULLUP);
#endif
}

void powerBtnLoop(PowerBtnCallback onChange, PowerBtnLongPressCallback onLongPress) {
#if OMOTE_POWER_BTN_GPIO >= 0
  if (!onChange && !onLongPress) return;
  const bool pressed = digitalRead(OMOTE_POWER_BTN_GPIO) == LOW;
  const unsigned long now = millis();
  if (pressed && sLastPressed && onLongPress && !sLongPressFired &&
      (now - sPressStartMs) >= kLongPressMs) {
    sLongPressFired = true;
    onLongPress();
  }
  if (pressed != sLastPressed && now - sDebounceMs > 40) {
    sDebounceMs = now;
    sLastPressed = pressed;
    if (pressed) {
      sPressStartMs = now;
      sLongPressFired = false;
    }
    if (onChange) onChange(KEY_POWER, pressed);
  }
#endif
}

bool powerBtnLongPressConsumed() { return sLongPressFired; }
