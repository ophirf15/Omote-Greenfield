#pragma once

#include <Arduino.h>

/** Top power button above the LCD (not in the 5×5 matrix). */
#define KEY_POWER 'P'

typedef void (*PowerBtnCallback)(char key, bool pressed);
typedef void (*PowerBtnLongPressCallback)();

void powerBtnInit();
void powerBtnLoop(PowerBtnCallback onChange, PowerBtnLongPressCallback onLongPress = nullptr);
/** After long-press handling, skip the matching short release. */
bool powerBtnLongPressConsumed();
