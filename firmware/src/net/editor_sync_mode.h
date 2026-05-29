#pragma once

#include <Arduino.h>

/** True while the device is paused for PC layout editing (BLE/HA/input off). */
bool editorSyncModeActive();

/** Pause BLE, HA bootstrap, and remote input; show on-device overlay. Idempotent. */
bool editorSyncModeEnter();

/** Leave sync mode; default reboot restores BLE/HA. */
void editorSyncModeExit(bool reboot = true);
