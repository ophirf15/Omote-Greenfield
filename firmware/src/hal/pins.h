#pragma once

// OMOTE Rev 1-4 pin map (see docs/hardware-rev1-4.md)

#define PIN_I2C_SDA 19
#define PIN_I2C_SCL 22

#define PIN_LCD_BL 4
#define PIN_LCD_EN 10
#define PIN_LCD_CS 5
#define PIN_LCD_DC 9
#define PIN_LCD_MOSI 23
#define PIN_LCD_SCK 18

#define PIN_IR_TX 33
#define PIN_IR_RX 15
#define PIN_IR_RX_PWR 25

#define PIN_KP_OUT_1 32
#define PIN_KP_OUT_2 26
#define PIN_KP_OUT_3 27
#define PIN_KP_OUT_4 14
#define PIN_KP_OUT_5 12
#define PIN_KP_IN_A 37
#define PIN_KP_IN_B 38
#define PIN_KP_IN_C 39
#define PIN_KP_IN_D 34
#define PIN_KP_IN_E 35

#define PIN_USER_LED 2
#define PIN_ACC_INT 13
/** Top power button above LCD (active low). Set to -1 if not wired on your board. */
#define PIN_POWER_BTN 0

/** ext1 deep-sleep wake: keypad row inputs (OMOTE Rev1-4, without ACC INT). */
#define PIN_KEYPAD_WAKE_BITMASK \
  ((1ULL << PIN_KP_IN_A) | (1ULL << PIN_KP_IN_B) | (1ULL << PIN_KP_IN_C) | (1ULL << PIN_KP_IN_D) | \
   (1ULL << PIN_KP_IN_E))

#define SCR_WIDTH 240
#define SCR_HEIGHT 320
