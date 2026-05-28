#pragma once

#include <Arduino.h>

void sleepInitWakeup();
void sleepInitImu();
void sleepNotifyActivity();
void sleepOnDisplayPoweredOff();
void sleepCheckActivity();

/** Screen off after this idle time (WiFi stays connected). */
void sleepSetDisplayTimeoutMs(uint32_t ms);
/** esp_deep_sleep after this idle time (WiFi disconnects). */
void sleepSetDeepSleepTimeoutMs(uint32_t ms);
/** Alias for sleepSetDisplayTimeoutMs (legacy API). */
void sleepSetTimeoutMs(uint32_t ms);

void sleepSetMotionWake(bool enabled);
void sleepSetUserBrightness(uint8_t brightness);
void sleepEnterDeep();
void sleepDisplayOff();

uint32_t sleepGetLastActivityMs();
bool sleepInPreSleepDimPhase();
uint32_t sleepGetTimeoutMs();
uint32_t sleepGetDisplayTimeoutMs();
uint32_t sleepGetDeepSleepTimeoutMs();
uint32_t sleepGetDimLeadMs();
uint8_t sleepGetBacklightBrightness();
bool sleepMotionWakeEnabled();
