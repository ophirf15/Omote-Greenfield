#pragma once

/** Set to 1 to download MDI icons from Iconify over HTTPS (uses RAM + blocks net mutex). */
#ifndef OMOTE_ICON_FETCH_ENABLED
#define OMOTE_ICON_FETCH_ENABLED 0
#endif

#include <lvgl.h>
#include <Arduino.h>

/** Attach 20x20 HA/domain icon to parent (left side). Returns image obj or nullptr. */
lv_obj_t *haIconAttach(lv_obj_t *parent, const String &entityId, const String &mdiIcon);

/** Queue MDI icon download to LittleFS (call from main loop). */
void haIconQueueFetch(const String &mdiIcon);
void haIconLoop();

/** Invalidate cache after config change. */
void haIconClearQueue();
void haIconClearAll();
