#pragma once

#include <Arduino.h>

#define KEYPAD_ROWS 5
#define KEYPAD_COLS 5

// OMOTE rev1-4 key layout (row = input A-E, col = output 1-5)
extern const char KEYPAD_MAP[KEYPAD_ROWS][KEYPAD_COLS];

typedef void (*KeypadCallback)(char key, bool pressed);

void keypadInit();
void keypadLoop(KeypadCallback onKey);
