#include "hal/keypad_hal.h"

#include "hal/display_hal.h"

#include "hal/pins.h"

#include "hal/sleep_hal.h"



const char KEYPAD_MAP[KEYPAD_ROWS][KEYPAD_COLS] = {

    {'s', '^', '-', 'm', 'e'},

    {'i', 'r', '+', 'k', 'd'},

    {'4', 'v', '1', '3', '2'},

    {'>', 'o', 'b', 'u', 'l'},

    {'?', 'p', 'c', '<', '='},

};



static const uint8_t COL_PINS[] = {PIN_KP_OUT_1, PIN_KP_OUT_2, PIN_KP_OUT_3, PIN_KP_OUT_4,

                                   PIN_KP_OUT_5};

static const uint8_t ROW_PINS[] = {PIN_KP_IN_A, PIN_KP_IN_B, PIN_KP_IN_C, PIN_KP_IN_D,

                                   PIN_KP_IN_E};



static char lastKey = 0;

static bool lastPressed = false;

static unsigned long lastDebounce = 0;



void keypadInit() {

  for (uint8_t c = 0; c < KEYPAD_COLS; c++) {

    pinMode(COL_PINS[c], OUTPUT);

    digitalWrite(COL_PINS[c], HIGH);

  }

  for (uint8_t r = 0; r < KEYPAD_ROWS; r++) {

    pinMode(ROW_PINS[r], INPUT);

  }

}



/** OMOTE Rev1-4 inverted matrix: strobe column HIGH, key closed = row HIGH. */

static char scanKeypadOmote() {

  for (uint8_t c = 0; c < KEYPAD_COLS; c++) {

    for (uint8_t oc = 0; oc < KEYPAD_COLS; oc++) {

      pinMode(COL_PINS[oc], OUTPUT);

      digitalWrite(COL_PINS[oc], oc == c ? HIGH : LOW);

    }

    delayMicroseconds(50);

    for (uint8_t r = 0; r < KEYPAD_ROWS; r++) {

      if (digitalRead(ROW_PINS[r]) == HIGH) {

        for (uint8_t oc = 0; oc < KEYPAD_COLS; oc++) {

          pinMode(COL_PINS[oc], OUTPUT);

          digitalWrite(COL_PINS[oc], HIGH);

        }

        return KEYPAD_MAP[r][c];

      }

    }

    pinMode(COL_PINS[c], INPUT);

  }

  for (uint8_t oc = 0; oc < KEYPAD_COLS; oc++) {

    pinMode(COL_PINS[oc], OUTPUT);

    digitalWrite(COL_PINS[oc], HIGH);

  }

  return 0;

}



static char scanKeypadStable() {

  char a = scanKeypadOmote();

  if (!a) return 0;

  char b = scanKeypadOmote();

  return (a == b) ? a : 0;

}



void keypadLoop(KeypadCallback onKey) {

  if (displayIsOff()) {

    const char k = scanKeypadStable();

    if (k) {

      sleepNotifyActivity();

      Serial.printf("Key wake while off: '%c'\n", k);

    }

    return;

  }



  char k = scanKeypadOmote();

  bool pressed = k != 0;

  unsigned long now = millis();

  if (pressed != lastPressed || (pressed && k != lastKey)) {

    if (now - lastDebounce > 40) {

      lastDebounce = now;

      if (onKey) onKey(k ? k : lastKey, pressed);

      lastKey = k;

      lastPressed = pressed;

    }

  } else if (!pressed) {

    lastKey = 0;

    lastPressed = false;

  }

}


