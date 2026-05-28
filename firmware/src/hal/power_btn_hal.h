#pragma once

#include <Arduino.h>

/** Top power button above the LCD (not in the 5×5 matrix). */
#define KEY_POWER 'P'

typedef void (*PowerBtnCallback)(char key, bool pressed);

void powerBtnInit();
void powerBtnLoop(PowerBtnCallback onChange);
