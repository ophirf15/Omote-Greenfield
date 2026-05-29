#pragma once

#include "config/config_store.h"
#include "actions/action_executor.h"
#include <lvgl.h>

void pageEngineSetDeviceSettings(DeviceSettings *ds);
void pageEngineSetHaSettings(HaSettings *ha);
void pageEngineInit(OmoteConfig *config, ActionExecutor *executor);
void pageEngineSyncHaToggles();
void pageEngineOnSwipeDrag(int dx, int dy);
void pageEngineReload();
/** Tear down page widgets to free RAM before heavy config writes. */
void pageEngineUnloadUi();
/** Apply config changes on next loop tick (safe during HTTP handlers). */
void pageEngineRequestReload();
void pageEngineAfterAction(bool pageChanged);
void pageEngineLoop();
/** Blocking HA work — call after pageEngineLoop() so swipes/touch stay responsive. */
void pageEngineLoopNetwork();
void pageEngineHandleKey(char key, bool pressed);
