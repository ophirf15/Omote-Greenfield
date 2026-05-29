#include "ui_runtime/page_engine.h"
#include <ESP.h>
#include <stdlib.h>
#include "config/config_schema.h"
#include "hal/battery_hal.h"
#include "hal/display_hal.h"
#include "hal/sleep_hal.h"
#include "hal/wifi_hal.h"
#include "hal/ble_hal.h"
#include <NimBLEDevice.h>
#include "net/http_server.h"
#include "net/ha_client.h"
#include "hal/ha_icon_hal.h"
#include "net/net_worker.h"
#include "net/runtime_diag.h"
#include "net/time_sync.h"
#include "hal/pins.h"
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include <vector>
static OmoteConfig *gCfg = nullptr;
static ActionExecutor *gExec = nullptr;
static DeviceSettings *gDevSettings = nullptr;
static HaSettings *gHaSettings = nullptr;
static lv_obj_t *gScreen = nullptr;
static lv_obj_t *gStatusBar = nullptr;
static lv_obj_t *gBatLbl = nullptr;
static lv_obj_t *gBatIconLbl = nullptr;
static lv_obj_t *gClockLbl = nullptr;
static lv_obj_t *gTitleLbl = nullptr;
static lv_obj_t *gContent = nullptr;
static lv_obj_t *gSettingsPanel = nullptr;
static std::vector<lv_obj_t *> gWidgets;
static String gCachedPageId;
static std::vector<ButtonDef> gCachedButtons;
static lv_obj_t *gOverlayMsg = nullptr;
static bool gShowingSettings = false;
static bool gShowingKeyboard = false;
static lv_obj_t *gKeyboardPanel = nullptr;
static lv_obj_t *gKbMatrix = nullptr;
static lv_obj_t *gBleStatusLbl = nullptr;
static uint8_t gKbLayer = 0;
static bool gKbShift = false;
static uint32_t sLastHaPoll = 0;
static uint32_t sHaPollNotBeforeMs = 0;
static uint32_t sPageEngineBootMs = 0;
static constexpr uint32_t kHaBootGraceMs = 25000;
static constexpr uint32_t kHaPollIntervalMs = 8000;
static constexpr uint32_t kEntityOptimisticMs = 8000;
static uint32_t sPersistActivePageDeadline = 0;
static uint32_t sUiBusyUntilMs = 0;
static constexpr uint32_t kUiBusyAfterSwipeMs = 5000;
static uint32_t sLastCycleMs = 0;
static bool sClimateResyncPending = false;
static uint32_t sHaUiQuietUntil = 0;
static bool sPageSwitchPending = false;
static uint32_t sClimateSyncNotBeforeMs = 0;
static uint32_t sClimateLastOkMs = 0;
static constexpr uint32_t kClimateRefreshMs = 45000;
static constexpr uint32_t kClimateRetryMs = 2500;

struct HaUiJob {
  bool pending = false;
  ActionDef action;
  size_t btnIndex = SIZE_MAX;
  bool isToggle = false;
  bool wantOn = false;
};
static HaUiJob sHaUiJob;
static bool sHaSyncInProgress = false;
static volatile bool sReloadPending = false;
static size_t sSkipToggleSyncIdx = SIZE_MAX;
static uint32_t sSkipToggleSyncUntil = 0;
struct ToggleUi {
  lv_obj_t *sw = nullptr;
  size_t btnIndex = 0;
};
static std::vector<ToggleUi> gToggles;
struct HaLabelUi {
  lv_obj_t *valueLbl = nullptr;
  size_t btnIndex = 0;
};
struct HaClimateUi {
  lv_obj_t *tempLbl = nullptr;
  lv_obj_t *modeLbl = nullptr;
  size_t btnIndex = 0;
};
struct ClimateThermostatUi {
  lv_obj_t *panel = nullptr;
  lv_obj_t *arc = nullptr;
  lv_obj_t *heatArc = nullptr;
  lv_obj_t *coolArc = nullptr;
  lv_obj_t *modeLbl = nullptr;
  lv_obj_t *lowLbl = nullptr;
  lv_obj_t *highLbl = nullptr;
  lv_obj_t *roomLbl = nullptr;
  lv_obj_t *heatTick = nullptr;
  lv_obj_t *coolTick = nullptr;
  lv_obj_t *roomTick = nullptr;
  lv_obj_t *modeMenu = nullptr;
  size_t btnIndex = 0;
  bool menuOpen = false;
  bool dualSetpoint = false;
  float arcMin = 50;
  float arcMax = 90;
};
static std::vector<HaLabelUi> gHaLabels;
static std::vector<HaClimateUi> gHaClimates;
static ClimateThermostatUi gThermostat;

struct EntityStateCacheEntry {
  String entityId;
  bool isOn = false;
  uint32_t optimisticUntil = 0;
};
static std::vector<EntityStateCacheEntry> sEntityStateCache;
static constexpr size_t kEntityCacheMax = 28;

static EntityStateCacheEntry *entityCacheFind(const String &entityId) {
  if (entityId.length() == 0) return nullptr;
  for (auto &e : sEntityStateCache) {
    if (e.entityId == entityId) return &e;
  }
  return nullptr;
}

static bool entityCacheGet(const String &entityId, bool &isOn) {
  const EntityStateCacheEntry *e = entityCacheFind(entityId);
  if (!e) return false;
  isOn = e->isOn;
  return true;
}

static void entityCachePut(const String &entityId, bool isOn) {
  if (entityId.length() == 0) return;
  if (EntityStateCacheEntry *e = entityCacheFind(entityId)) {
    e->isOn = isOn;
    return;
  }
  if (sEntityStateCache.size() >= kEntityCacheMax) {
    sEntityStateCache.erase(sEntityStateCache.begin());
  }
  EntityStateCacheEntry ent;
  ent.entityId = entityId;
  ent.isOn = isOn;
  sEntityStateCache.push_back(ent);
}

static void entityCacheMarkOptimistic(const String &entityId, bool isOn) {
  if (entityId.length() == 0) return;
  entityCachePut(entityId, isOn);
  if (EntityStateCacheEntry *e = entityCacheFind(entityId)) {
    const uint32_t until = millis() + kEntityOptimisticMs;
    if (until > e->optimisticUntil) e->optimisticUntil = until;
  }
}

static bool entityCacheIsOptimistic(const String &entityId) {
  const EntityStateCacheEntry *e = entityCacheFind(entityId);
  return e && millis() < e->optimisticUntil;
}

static lv_color_t hexToColor(uint32_t hex) { return lv_color_hex(hex & 0xFFFFFF); }
static void stripContainerStyle(lv_obj_t *obj);
static void cyclePage(int dir);
static void updateStatusBarTitle();
static void buildPageWidgets();
static void pageEngineSwitchPageLight();
static void makeTransparentContainer(lv_obj_t *obj) {
  stripContainerStyle(obj);
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}
static float readClimateTempNative(const String &rawJson) {
  static JsonDocument doc;
  doc.clear();
  if (deserializeJson(doc, rawJson)) return 0;
  JsonObject attr = doc["attributes"];
  if (attr.isNull()) return 0;
  if (!attr["temperature"].isNull()) return attr["temperature"].as<float>();
  if (!attr["current_temperature"].isNull()) return attr["current_temperature"].as<float>();
  return 0;
}
struct ClimateAttrs {
  float current = 0;
  float target = 0;
  float low = 0;
  float high = 0;
  float minT = 50;
  float maxT = 90;
  bool hasRange = false;
  String mode;
};
struct ClimateCacheEntry {
  String entityId;
  ClimateAttrs attrs;
};
static ClimateCacheEntry sClimateCache;
static bool parseClimateAttrs(const String &rawJson, ClimateAttrs &out) {
  static JsonDocument doc;
  doc.clear();
  const DeserializationError err = deserializeJson(doc, rawJson);
  if (err) {
    Serial.printf("climate: JSON parse %s (%u bytes)\n", err.c_str(), rawJson.length());
    return false;
  }
  JsonObject attr = doc["attributes"];
  if (attr.isNull()) return false;
  out.mode = attr["hvac_mode"] | doc["state"].as<String>();
  out.current = attr["current_temperature"] | 0.0f;
  out.target = attr["temperature"] | 0.0f;
  if (!attr["target_temp_low"].isNull() && !attr["target_temp_high"].isNull()) {
    out.low = attr["target_temp_low"].as<float>();
    out.high = attr["target_temp_high"].as<float>();
    out.hasRange = true;
  } else {
    out.low = out.target;
    out.high = out.target;
  }
  String unit = attr["temperature_unit"] | attr["unit_of_measurement"] | "°F";
  const bool useF = unit.indexOf('F') >= 0 || unit.indexOf('f') >= 0;
  out.minT = attr["min_temp"] | (useF ? 50.0f : 10.0f);
  out.maxT = attr["max_temp"] | (useF ? 90.0f : 32.0f);
  if (out.maxT <= out.minT) {
    out.minT = useF ? 50.0f : 10.0f;
    out.maxT = useF ? 90.0f : 32.0f;
  }
  return true;
}
static void formatTemp(char *buf, size_t len, float temp) {
  snprintf(buf, len, "%.0f°", temp);
}
static String capitalizeMode(const String &m) {
  if (m.length() == 0) return String("—");
  String s = m;
  s.replace("_", " ");
  s.setCharAt(0, (char)toupper(s.charAt(0)));
  return s;
}
static uint16_t tempToArcAngle(float temp, float minT, float maxT) {
  const uint16_t rot = 135;
  const uint16_t span = 270;
  if (maxT <= minT) return rot;
  float t = (temp - minT) / (maxT - minT);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return rot + (uint16_t)(t * span);
}
static void positionArcTick(lv_obj_t *tick, float temp, float minT, float maxT) {
  if (!tick) return;
  const int cx = SCR_WIDTH / 2;
  const int cy = 6 + 95;
  // Match LVGL arc centerline radius: size/2 - arc_width/2  (190/2 - 12/2 = 89)
  const int r = 89;
  if (maxT <= minT) return;
  float t = (temp - minT) / (maxT - minT);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  const float angleDeg = 135.0f + t * 270.0f;
  const float rad = angleDeg * 3.14159265f / 180.0f;
  const int x = cx + (int)(r * cosf(rad)) - 7;
  const int y = cy + (int)(r * sinf(rad)) - 7;
  lv_obj_set_pos(tick, x, y);
}
static lv_obj_t *makeArcTick(lv_obj_t *panel, lv_color_t color) {
  lv_obj_t *tick = lv_obj_create(panel);
  lv_obj_set_size(tick, 14, 14);
  lv_obj_set_style_radius(tick, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(tick, color, 0);
  lv_obj_set_style_border_width(tick, 2, 0);
  lv_obj_set_style_border_color(tick, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_pad_all(tick, 0, 0);
  lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tick, LV_OBJ_FLAG_GESTURE_BUBBLE);
  return tick;
}
static void buildClimateThermostat(const ButtonDef &btn, size_t idx);
static void onThermoAdjustBtn(lv_event_t *e);
static void onClimateTempBtn(lv_event_t *e);
static void haCallClimateMode(size_t btnIndex, const char *mode);
static void closeThermoModeMenu() {
  if (!gThermostat.modeMenu) return;
  lv_obj_add_flag(gThermostat.modeMenu, LV_OBJ_FLAG_HIDDEN);
  gThermostat.menuOpen = false;
}
static void applyClimateAttrsToUi(const ClimateAttrs &ca) {
  if (!gThermostat.panel) return;
  gThermostat.dualSetpoint = ca.hasRange;
  gThermostat.arcMin = ca.minT;
  gThermostat.arcMax = ca.maxT;
  char buf[20];
  if (gThermostat.modeLbl) lv_label_set_text(gThermostat.modeLbl, capitalizeMode(ca.mode).c_str());
  if (gThermostat.lowLbl) {
    formatTemp(buf, sizeof(buf), ca.low);
    lv_label_set_text(gThermostat.lowLbl, buf);
  }
  if (gThermostat.highLbl) {
    formatTemp(buf, sizeof(buf), ca.high);
    lv_label_set_text(gThermostat.highLbl, buf);
  }
  if (gThermostat.roomLbl) {
    snprintf(buf, sizeof(buf), "Room %.0f°", ca.current);
    lv_label_set_text(gThermostat.roomLbl, buf);
  }
  if (gThermostat.arc) {
    if (ca.hasRange) {
      uint16_t aLow = tempToArcAngle(ca.low, ca.minT, ca.maxT);
      uint16_t aHigh = tempToArcAngle(ca.high, ca.minT, ca.maxT);
      if (aHigh < aLow) {
        const uint16_t t = aLow;
        aLow = aHigh;
        aHigh = t;
      }
      if (gThermostat.heatArc) {
        lv_obj_clear_flag(gThermostat.heatArc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_angles(gThermostat.heatArc, 135, aLow);
      }
      if (gThermostat.coolArc) {
        lv_obj_clear_flag(gThermostat.coolArc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_angles(gThermostat.coolArc, aHigh, 405);
      }
    } else {
      const float t = ca.target > 0 ? ca.target : ca.current;
      const uint16_t a = tempToArcAngle(t, ca.minT, ca.maxT);
      if (gThermostat.heatArc) {
        lv_obj_clear_flag(gThermostat.heatArc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_angles(gThermostat.heatArc, 135, a);
      }
      if (gThermostat.coolArc) {
        lv_obj_add_flag(gThermostat.coolArc, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
  if (gThermostat.heatTick) {
    if (ca.hasRange) {
      lv_obj_clear_flag(gThermostat.heatTick, LV_OBJ_FLAG_HIDDEN);
      positionArcTick(gThermostat.heatTick, ca.low, ca.minT, ca.maxT);
    } else {
      lv_obj_add_flag(gThermostat.heatTick, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (gThermostat.coolTick) {
    if (ca.hasRange) {
      lv_obj_clear_flag(gThermostat.coolTick, LV_OBJ_FLAG_HIDDEN);
      positionArcTick(gThermostat.coolTick, ca.high, ca.minT, ca.maxT);
    } else {
      lv_obj_add_flag(gThermostat.coolTick, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (gThermostat.roomTick) {
    if (ca.current > 0) {
      lv_obj_clear_flag(gThermostat.roomTick, LV_OBJ_FLAG_HIDDEN);
      positionArcTick(gThermostat.roomTick, ca.current, ca.minT, ca.maxT);
    } else {
      lv_obj_add_flag(gThermostat.roomTick, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static bool climateCacheGet(const String &entityId, ClimateAttrs &out) {
  if (entityId.length() == 0 || sClimateCache.entityId != entityId) return false;
  out = sClimateCache.attrs;
  return true;
}

static void climateCachePut(const String &entityId, const ClimateAttrs &ca) {
  if (entityId.length() == 0) return;
  sClimateCache.entityId = entityId;
  sClimateCache.attrs = ca;
}

static String climateEntityForButton(const ButtonDef &btn) {
  if (btn.action.entityId.length()) return btn.action.entityId;
  if (btn.label.length() && btn.label.indexOf('.') > 0) return btn.label;
  return String();
}

static void climateShowStatus(const char *msg) {
  if (gThermostat.modeLbl) lv_label_set_text(gThermostat.modeLbl, msg);
}

static bool syncClimateThermostat() {
  if (!gThermostat.panel || gThermostat.btnIndex >= gCachedButtons.size() || !gHaSettings) return false;
  if (!gHaSettings->configured) {
    climateShowStatus("HA not set");
    return false;
  }
  if (!wifiIsConnected()) {
    climateShowStatus("No WiFi");
    return false;
  }
  const ButtonDef &btn = gCachedButtons[gThermostat.btnIndex];
  const String entityId = climateEntityForButton(btn);
  if (entityId.length() == 0) {
    climateShowStatus("No entity");
    return false;
  }
  String raw, err;
  netWorkerSetPreferShortTimeout(true);
  const bool ok = haFetchEntityRaw(*gHaSettings, entityId, raw, err);
  netWorkerSetPreferShortTimeout(false);
  if (ok) {
    ClimateAttrs ca;
    if (parseClimateAttrs(raw, ca)) {
      climateCachePut(entityId, ca);
      applyClimateAttrsToUi(ca);
      sClimateLastOkMs = millis();
      return true;
    }
    Serial.printf("climate: attrs parse failed %s\n", entityId.c_str());
    climateShowStatus("Parse err");
  } else {
    Serial.printf("climate: fetch %s failed: %s\n", entityId.c_str(), err.c_str());
    ClimateAttrs ca;
    if (climateCacheGet(entityId, ca)) {
      applyClimateAttrsToUi(ca);
      climateShowStatus("Cached");
      return true;
    }
    climateShowStatus("HA err");
  }
  return false;
}

static void scheduleClimateResync(uint32_t delayMs) {
  sClimateResyncPending = true;
  const uint32_t when = millis() + delayMs;
  if (when > sClimateSyncNotBeforeMs) sClimateSyncNotBeforeMs = when;
}

static bool climateNeedsService() {
  if (!gThermostat.panel) return false;
  if (millis() < sClimateSyncNotBeforeMs) return false;
  if (sClimateResyncPending || sClimateLastOkMs == 0) return true;
  return millis() - sClimateLastOkMs >= kClimateRefreshMs;
}
static void buildClimateThermostat(const ButtonDef &btn, size_t idx) {
  (void)btn;
  const int ch = SCR_HEIGHT - STATUS_BAR_H;
  lv_obj_t *panel = lv_obj_create(gContent);
  lv_obj_set_pos(panel, 0, 0);
  lv_obj_set_size(panel, SCR_WIDTH, ch);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x2a1a3a), 0);
  stripContainerStyle(panel);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_t *arc = lv_arc_create(panel);
  lv_obj_set_size(arc, 190, 190);
  lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 6);
  lv_arc_set_rotation(arc, 0);
  lv_arc_set_bg_angles(arc, 135, 405);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_angles(arc, 135, 270);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 0, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x443322), LV_PART_MAIN);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  auto mkIndicatorArc = [&](lv_color_t color) {
    lv_obj_t *a = lv_arc_create(panel);
    lv_obj_set_size(a, 190, 190);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 6);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, 135, 405);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_angles(a, 135, 135);
    lv_obj_set_style_arc_width(a, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, color, LV_PART_INDICATOR);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
  };
  lv_obj_t *heatArc = mkIndicatorArc(lv_color_hex(0xff8844));
  lv_obj_t *coolArc = mkIndicatorArc(lv_color_hex(0x66aaff));
  lv_obj_t *heatTick = makeArcTick(panel, lv_color_hex(0xff8844));
  lv_obj_t *coolTick = makeArcTick(panel, lv_color_hex(0x4488ff));
  lv_obj_t *roomTick = makeArcTick(panel, lv_color_hex(0xe6e6e6));
  lv_obj_move_foreground(heatArc);
  lv_obj_move_foreground(coolArc);
  lv_obj_move_foreground(heatTick);
  lv_obj_move_foreground(coolTick);
  lv_obj_move_foreground(roomTick);
  lv_obj_t *modeLbl = lv_label_create(panel);
  lv_label_set_text(modeLbl, "…");
  lv_obj_align(modeLbl, LV_ALIGN_TOP_MID, 0, 68);
  lv_obj_set_style_text_font(modeLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(modeLbl, lv_color_hex(0xdddddd), 0);
  lv_obj_add_flag(modeLbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      modeLbl,
      [](lv_event_t *e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        if (!gThermostat.modeMenu) return;
        if (gThermostat.menuOpen)
          closeThermoModeMenu();
        else {
          lv_obj_clear_flag(gThermostat.modeMenu, LV_OBJ_FLAG_HIDDEN);
          gThermostat.menuOpen = true;
        }
      },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t *lowLbl = lv_label_create(panel);
  lv_obj_t *highLbl = lv_label_create(panel);
  lv_label_set_text(lowLbl, "--°");
  lv_label_set_text(highLbl, "--°");
  lv_obj_set_style_text_font(lowLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_font(highLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lowLbl, lv_color_hex(0xffaa66), 0);
  lv_obj_set_style_text_color(highLbl, lv_color_hex(0x66aaff), 0);
  lv_obj_align(lowLbl, LV_ALIGN_TOP_MID, -54, 92);
  lv_obj_align(highLbl, LV_ALIGN_TOP_MID, 54, 92);
  lv_obj_t *roomLbl = lv_label_create(panel);
  lv_label_set_text(roomLbl, "Room --°");
  lv_obj_align(roomLbl, LV_ALIGN_TOP_MID, 0, 118);
  lv_obj_set_style_text_color(roomLbl, lv_color_hex(0xbbbbbb), 0);
  lv_obj_set_style_text_font(roomLbl, &lv_font_montserrat_12, 0);
  auto mkRoundBtn = [&](const char *sym, int xofs, intptr_t ud) {
    lv_obj_t *b = lv_btn_create(panel);
    lv_obj_set_size(b, 40, 40);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, xofs, -10);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0xff8844), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3a2848), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onThermoAdjustBtn, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
  };
  mkRoundBtn("-", -88, (intptr_t)((idx << 2) | 0));
  mkRoundBtn("+", -44, (intptr_t)((idx << 2) | 1));
  mkRoundBtn("-", 44, (intptr_t)((idx << 2) | 2));
  mkRoundBtn("+", 88, (intptr_t)((idx << 2) | 3));
  lv_obj_t *menu = lv_obj_create(panel);
  lv_obj_set_size(menu, SCR_WIDTH - 20, 36);
  lv_obj_align(menu, LV_ALIGN_BOTTOM_MID, 0, -58);
  lv_obj_set_style_bg_color(menu, lv_color_hex(0x1a1228), 0);
  lv_obj_set_style_border_color(menu, lv_color_hex(0x555555), 0);
  lv_obj_set_style_border_width(menu, 1, 0);
  lv_obj_set_style_radius(menu, 8, 0);
  lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(menu, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  stripContainerStyle(menu);
  lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
  const char *labels[] = {"Off", "Heat", "Cool", "Auto"};
  for (int m = 0; m < 4; m++) {
    lv_obj_t *b = lv_btn_create(menu);
    lv_obj_set_size(b, 50, 28);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, labels[m]);
    lv_obj_center(l);
    lv_obj_add_event_cb(
        b,
        [](lv_event_t *e) {
          if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
          const intptr_t packed = (intptr_t)lv_event_get_user_data(e);
          const size_t idx2 = (size_t)(packed >> 3);
          const int modeIdx = (int)(packed & 7);
          static const char *modes[] = {"off", "heat", "cool", "auto"};
          if (modeIdx >= 0 && modeIdx <= 3) haCallClimateMode(idx2, modes[modeIdx]);
          closeThermoModeMenu();
        },
        LV_EVENT_CLICKED, (void *)(intptr_t)((idx << 3) | m));
    lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
  }
  gWidgets.push_back(panel);
  gThermostat.panel = panel;
  gThermostat.arc = arc;
  gThermostat.heatArc = heatArc;
  gThermostat.coolArc = coolArc;
  gThermostat.modeLbl = modeLbl;
  gThermostat.lowLbl = lowLbl;
  gThermostat.highLbl = highLbl;
  gThermostat.roomLbl = roomLbl;
  gThermostat.heatTick = heatTick;
  gThermostat.coolTick = coolTick;
  gThermostat.roomTick = roomTick;
  gThermostat.modeMenu = menu;
  gThermostat.btnIndex = idx;
  gThermostat.menuOpen = false;
  const String entityId = climateEntityForButton(btn);
  if (entityId.length()) {
    ClimateAttrs ca;
    if (climateCacheGet(entityId, ca)) {
      applyClimateAttrsToUi(ca);
      sClimateLastOkMs = millis();
    } else {
      scheduleClimateResync(300);
    }
  } else {
    climateShowStatus("No entity");
  }
}
static void haCallClimateTemp(size_t btnIndex, float target) {
  if (!gExec || btnIndex >= gCachedButtons.size() || !gHaSettings) return;
  const ButtonDef &btn = gCachedButtons[btnIndex];
  ActionDef act = btn.action;
  act.type = ACTION_HA_SERVICE;
  act.domain = "climate";
  act.service = "set_temperature";
  JsonDocument dj;
  dj["temperature"] = target;
  serializeJson(dj, act.dataJson);
  gExec->execute(act);
  sClimateResyncPending = true;
}
static void haCallClimateRange(size_t btnIndex, float low, float high) {
  if (!gExec || btnIndex >= gCachedButtons.size() || !gHaSettings) return;
  const ButtonDef &btn = gCachedButtons[btnIndex];
  ActionDef act = btn.action;
  act.type = ACTION_HA_SERVICE;
  act.domain = "climate";
  act.service = "set_temperature";
  JsonDocument dj;
  dj["target_temp_low"] = low;
  dj["target_temp_high"] = high;
  serializeJson(dj, act.dataJson);
  gExec->execute(act);
  sClimateResyncPending = true;
}
static void haCallClimateMode(size_t btnIndex, const char *mode) {
  if (!gExec || btnIndex >= gCachedButtons.size() || !gHaSettings) return;
  const ButtonDef &btn = gCachedButtons[btnIndex];
  ActionDef act = btn.action;
  act.type = ACTION_HA_SERVICE;
  act.domain = "climate";
  act.service = "set_hvac_mode";
  JsonDocument dj;
  dj["hvac_mode"] = mode;
  serializeJson(dj, act.dataJson);
  gExec->execute(act);
  sClimateResyncPending = true;
}
static void onThermoAdjustBtn(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const intptr_t packed = (intptr_t)lv_event_get_user_data(e);
  const size_t idx = (size_t)(packed >> 2);
  const int which = (int)(packed & 3);  // 0 heat-, 1 heat+, 2 cool-, 3 cool+
  if (idx >= gCachedButtons.size()) return;
  String raw, err;
  netWorkerSetPreferShortTimeout(true);
  const bool got = haFetchEntityRaw(*gHaSettings, gCachedButtons[idx].action.entityId, raw, err);
  netWorkerSetPreferShortTimeout(false);
  if (!got) return;
  ClimateAttrs ca;
  if (!parseClimateAttrs(raw, ca)) return;
  if (ca.hasRange) {
    float low = ca.low;
    float high = ca.high;
    if (which == 0) low -= 1.0f;
    else if (which == 1) low += 1.0f;
    else if (which == 2) high -= 1.0f;
    else high += 1.0f;
    if (low < ca.minT) low = ca.minT;
    if (high > ca.maxT) high = ca.maxT;
    if (low > high - 1.0f) low = high - 1.0f;
    if (high < low + 1.0f) high = low + 1.0f;
    haCallClimateRange(idx, low, high);
    return;
  }
  float t = ca.target > 0 ? ca.target : readClimateTempNative(raw);
  if (t < ca.minT) t = ca.minT;
  if (which == 0 || which == 2) t -= 1.0f;
  else t += 1.0f;
  if (t < ca.minT) t = ca.minT;
  if (t > ca.maxT) t = ca.maxT;
  haCallClimateTemp(idx, t);
}
static void onClimateTempBtn(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const intptr_t packed = (intptr_t)lv_event_get_user_data(e);
  const size_t idx = (size_t)(packed >> 1);
  const int delta = (packed & 1) ? 1 : -1;
  if (idx >= gCachedButtons.size()) return;
  String raw, err;
  netWorkerSetPreferShortTimeout(true);
  const bool got = haFetchEntityRaw(*gHaSettings, gCachedButtons[idx].action.entityId, raw, err);
  netWorkerSetPreferShortTimeout(false);
  if (!got) return;
  ClimateAttrs ca;
  if (!parseClimateAttrs(raw, ca)) return;
  float t = ca.target > 0 ? ca.target : readClimateTempNative(raw);
  if (t < ca.minT) t = ca.minT + 1.0f;
  t += (float)delta;
  if (t < ca.minT) t = ca.minT;
  if (t > ca.maxT) t = ca.maxT;
  haCallClimateTemp(idx, t);
}
static void onClimateModeBtn(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const intptr_t packed = (intptr_t)lv_event_get_user_data(e);
  const size_t idx = (size_t)(packed >> 3);
  const int modeIdx = (int)(packed & 7);
  static const char *modes[] = {"off", "heat", "cool", "auto"};
  if (modeIdx < 0 || modeIdx > 3) return;
  haCallClimateMode(idx, modes[modeIdx]);
}
static void applyButtonStyle(lv_obj_t *btn, const ButtonDef &def) {
  lv_obj_set_style_bg_color(btn, hexToColor(def.style.bg), 0);
  lv_obj_set_style_radius(btn, def.style.radius, 0);
  if (def.style.borderW > 0) {
    lv_obj_set_style_border_width(btn, def.style.borderW, 0);
    lv_obj_set_style_border_color(btn, hexToColor(def.style.border), 0);
  }
}
static void applyLabelStyle(lv_obj_t *lbl, const ButtonDef &def) {
  lv_obj_set_style_text_color(lbl, hexToColor(def.style.fg), 0);
}
static void setSwitchChecked(lv_obj_t *sw, bool on, bool sendEvents) {
  if (!sw) return;
  if (sendEvents) {
    if (on)
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    else
      lv_obj_clear_state(sw, LV_STATE_CHECKED);
    return;
  }
  sHaSyncInProgress = true;
  if (on)
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  else
    lv_obj_clear_state(sw, LV_STATE_CHECKED);
  sHaSyncInProgress = false;
}
static size_t sHaSyncCursor = 0;

static void scheduleHaPollAfter(uint32_t delayMs) {
  const uint32_t when = millis() + delayMs;
  if (when > sHaPollNotBeforeMs) sHaPollNotBeforeMs = when;
}

/** Block HA HTTP + climate sync while LVGL rebuilds after a swipe. */
static void markUiBusy(uint32_t durationMs) {
  const uint32_t until = millis() + durationMs;
  if (until > sUiBusyUntilMs) sUiBusyUntilMs = until;
  if (until > sHaUiQuietUntil) sHaUiQuietUntil = until;
  if (until > sHaPollNotBeforeMs) sHaPollNotBeforeMs = until;
  if (until > sClimateSyncNotBeforeMs) sClimateSyncNotBeforeMs = until;
}

static void pageEngineReloadDone() {
  sHaSyncCursor = 0;
  scheduleHaPollAfter(1500);
  displayRequestRefresh();
  diagSetStage("ui_loop");
}

static void pageEngineSyncHaBatch(int maxItems) {
  if (!gHaSettings || !gHaSettings->configured || !wifiIsConnected() || gShowingSettings) return;
  if (ESP.getFreeHeap() < 18000) return;
  /* Climate state is fetched only when sClimateResyncPending — not every poll tick. */
  if (gThermostat.panel) return;
  const size_t nT = gToggles.size();
  const size_t nL = gHaLabels.size();
  const size_t nC = gHaClimates.size();
  const size_t total = nT + nL + nC;
  if (total == 0) return;

  for (int n = 0; n < maxItems; n++) {
    const size_t pass = sHaSyncCursor % total;
    sHaSyncCursor = (sHaSyncCursor + 1) % total;
    if (pass < nT) {
      const auto &t = gToggles[pass];
      if (t.btnIndex >= gCachedButtons.size() || !t.sw) continue;
      if (t.btnIndex == sSkipToggleSyncIdx && millis() < sSkipToggleSyncUntil) continue;
      const ButtonDef &btn = gCachedButtons[t.btnIndex];
      if (btn.action.entityId.length() == 0) continue;
      if (entityCacheIsOptimistic(btn.action.entityId)) continue;
      String state, err;
      if (!haFetchEntityState(*gHaSettings, btn.action.entityId, state, err)) continue;
      if (state == "unavailable" || state == "unknown") continue;
      const bool on = haStateIsOn(state);
      entityCachePut(btn.action.entityId, on);
      setSwitchChecked(t.sw, on, false);
    } else if (pass < nT + nL) {
      const auto &h = gHaLabels[pass - nT];
      if (h.btnIndex >= gCachedButtons.size() || !h.valueLbl) continue;
      const ButtonDef &btn = gCachedButtons[h.btnIndex];
      if (btn.action.entityId.length() == 0) continue;
      String state, err;
      if (!haFetchEntityState(*gHaSettings, btn.action.entityId, state, err)) continue;
      if (state.length() > 28) state = state.substring(0, 26) + "..";
      lv_label_set_text(h.valueLbl, state.c_str());
    } else {
      const auto &c = gHaClimates[pass - nT - nL];
      if (c.btnIndex >= gCachedButtons.size()) continue;
      const ButtonDef &btn = gCachedButtons[c.btnIndex];
      if (btn.action.entityId.length() == 0) continue;
      String raw, err;
      if (!haFetchEntityRaw(*gHaSettings, btn.action.entityId, raw, err)) continue;
      static JsonDocument doc;
      doc.clear();
      if (deserializeJson(doc, raw)) continue;
      float t = readClimateTempNative(raw);
      String mode = doc["attributes"]["hvac_mode"] | doc["state"].as<String>();
      if (c.tempLbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.0f°", t);
        lv_label_set_text(c.tempLbl, buf);
      }
      if (c.modeLbl) lv_label_set_text(c.modeLbl, mode.c_str());
    }
  }
}

void pageEngineSyncHaToggles() { pageEngineSyncHaBatch(1); }
void pageEngineOnSwipeDrag(int dx, int dy) {
  if (gShowingSettings || gShowingKeyboard) return;
  if (dy > 55 && abs(dy) > abs(dx) * 2) {
    gShowingSettings = true;
    pageEngineReload();
    return;
  }
  if (abs(dx) < 45 || abs(dx) < abs(dy) * 2) return;
  if (dx < 0) cyclePage(1);
  else cyclePage(-1);
}
static void saveActivePage() {
  if (gCfg && gExec) {
    gCfg->activePageId = gExec->activePageId();
    /* Tiny /active_page.txt write when idle — never configSave() on swipe (multi-second flash). */
    sPersistActivePageDeadline = millis() + 15000;
  }
}
static void cyclePage(int dir) {
  if (!gCfg || !gExec || gCfg->pages.size() < 2) return;
  const uint32_t now = millis();
  if (now - sLastCycleMs < 250) return;
  sLastCycleMs = now;
  int idx = configPageIndex(*gCfg, gExec->activePageId());
  if (idx < 0) idx = 0;
  idx = (idx + dir + (int)gCfg->pages.size()) % (int)gCfg->pages.size();
  gExec->setActivePage(gCfg->pages[idx].id);
  saveActivePage();
  markUiBusy(kUiBusyAfterSwipeMs);
  sleepNotifyActivity();
  sPageSwitchPending = true;
}
static bool runButtonAction(const ButtonDef &btn) {
  if (!gExec) return false;
  if (btn.action.type == ACTION_OPEN_KEYBOARD) {
    gShowingKeyboard = true;
    pageEngineRequestReload();
    return true;
  }
  if (btn.action.type == ACTION_HA_SERVICE) {
    sHaUiJob.action = btn.action;
    sHaUiJob.isToggle = false;
    sHaUiJob.btnIndex = SIZE_MAX;
    sHaUiJob.pending = true;
    diagSetTogglePending(true);
    return true;
  }
  String prevPage = gExec->activePageId();
  bool ok = gExec->execute(btn.action);
  httpServerPublishEvent(btn.id.c_str(), gCachedPageId.c_str(), btn.action.type.c_str());
  bool pageChanged = (gExec->activePageId() != prevPage);
  pageEngineAfterAction(pageChanged);
  return ok;
}

static void applyToggleHaService(ActionDef &act, bool wantOn) {
  String svc = act.service;
  svc.toLowerCase();
  if (svc.length() == 0 || svc == "toggle") {
    act.service = wantOn ? "turn_on" : "turn_off";
  }
  String dom = act.domain;
  haResolveHaCall(dom, act.service, act.entityId);
  act.domain = dom;
}

static void processHaUiJob() {
  if (!sHaUiJob.pending) return;
  if (millis() < sUiBusyUntilMs) return;

  HaUiJob job = sHaUiJob;
  sHaUiJob.pending = false;
  diagSetTogglePending(false);

  if (job.isToggle) applyToggleHaService(job.action, job.wantOn);

  netWorkerSetPreferShortTimeout(true);
  diagSetStage("ha_ui");

  String prevPage = gExec ? gExec->activePageId() : "";
  bool ok = gExec ? gExec->execute(job.action) : false;

  netWorkerSetPreferShortTimeout(false);

  if (job.isToggle && job.btnIndex < gCachedButtons.size()) {
    const ButtonDef &btn = gCachedButtons[job.btnIndex];
    Serial.printf("HA %s %s: %s\n", job.action.service.c_str(), btn.action.entityId.c_str(),
                  ok ? "ok" : "fail");
    lv_obj_t *sw = nullptr;
    for (const auto &t : gToggles) {
      if (t.btnIndex == job.btnIndex) {
        sw = t.sw;
        break;
      }
    }
    if (ok) {
      sSkipToggleSyncIdx = job.btnIndex;
      sSkipToggleSyncUntil = millis() + kEntityOptimisticMs;
      entityCacheMarkOptimistic(btn.action.entityId, job.wantOn);
      if (sw) setSwitchChecked(sw, job.wantOn, false);
    } else if (sw) {
      entityCachePut(btn.action.entityId, !job.wantOn);
      if (EntityStateCacheEntry *ent = entityCacheFind(btn.action.entityId)) ent->optimisticUntil = 0;
      setSwitchChecked(sw, !job.wantOn, false);
    }
  } else if (ok && gExec && gExec->activePageId() != prevPage) {
    pageEngineAfterAction(true);
  }

  sHaUiQuietUntil = millis() + 1500;
  scheduleHaPollAfter(kEntityOptimisticMs);
  diagSetStage("ui_loop");
  displayRequestRefresh();
  sleepNotifyActivity();
}
static void onBtnEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  size_t idx = (size_t)lv_event_get_user_data(e);
  if (!gExec || idx >= gCachedButtons.size()) return;
  const ButtonDef &btn = gCachedButtons[idx];
  lv_obj_t *target = lv_event_get_target(e);
  if (btn.widget == WIDGET_MOMENTARY) {
    if (code == LV_EVENT_PRESSED) {
      diagLvglEventEnter("btn_press");
      lv_obj_add_state(target, LV_STATE_PRESSED);
      displayRequestRefresh();
      diagLvglEventLeave();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
      diagLvglEventEnter("btn_release");
      lv_obj_clear_state(target, LV_STATE_PRESSED);
      if (code == LV_EVENT_RELEASED) runButtonAction(btn);
      else displayRequestRefresh();
      diagLvglEventLeave();
    }
    sleepNotifyActivity();
    return;
  }
  if (code != LV_EVENT_CLICKED) return;
  diagLvglEventEnter("btn_click");
  runButtonAction(btn);
  diagLvglEventLeave();
  sleepNotifyActivity();
}
static void onToggleChanged(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  if (sHaSyncInProgress) return;
  diagLvglEventEnter("toggle");
  const size_t idx = (size_t)lv_event_get_user_data(e);
  if (idx >= gCachedButtons.size()) {
    diagLvglEventLeave();
    return;
  }
  lv_obj_t *sw = lv_event_get_target(e);
  const ButtonDef &btn = gCachedButtons[idx];
  const bool wantOn = sw && lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (btn.action.entityId.length()) entityCacheMarkOptimistic(btn.action.entityId, wantOn);
  sHaUiJob.action = btn.action;
  sHaUiJob.btnIndex = idx;
  sHaUiJob.isToggle = true;
  sHaUiJob.wantOn = wantOn;
  sHaUiJob.pending = true;
  diagSetTogglePending(true);
  diagLvglEventLeave();
  sleepNotifyActivity();
}
static void clearWidgets() {
  sClimateResyncPending = false;
  sClimateLastOkMs = 0;
  for (auto *w : gWidgets) {
    if (w) lv_obj_del(w);
  }
  gWidgets.clear();
  gToggles.clear();
  gHaLabels.clear();
  gHaClimates.clear();
  gThermostat = ClimateThermostatUi{};
  haIconClearAll();
  gCachedButtons.clear();
  if (gOverlayMsg) {
    lv_obj_del(gOverlayMsg);
    gOverlayMsg = nullptr;
  }
  if (gKeyboardPanel) {
    lv_obj_del(gKeyboardPanel);
    gKeyboardPanel = nullptr;
    gKbMatrix = nullptr;
  }
}

static void refreshBleSettingsLabel() {
  if (!gBleStatusLbl) return;
  String t = "BLE: ";
  t += bleIsConnected() ? "connected" : "off";
  if (bleIsPairingMode()) t += " (pairing)";
  if (bleIsAdvertising()) t += " adv";
  t += " bonds:";
  t += bleIsInitialized() ? String(NimBLEDevice::getNumBonds()) : "0";
  lv_label_set_text(gBleStatusLbl, t.c_str());
}

static lv_obj_t *mkSettingsBtn(lv_obj_t *parent, const char *label, int x, int y, int w, int h,
                               lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, label);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

static const char *kbMapLower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n", "a", "s", "d", "f", "g", "h", "j", "k",
    "l", "\n", "Sh", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n", "123", "Space",
    LV_SYMBOL_OK, LV_SYMBOL_CLOSE, ""};
static const char *kbMapUpper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n", "A", "S", "D", "F", "G", "H", "J", "K",
    "L", "\n", "Sh", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n", "123", "Space",
    LV_SYMBOL_OK, LV_SYMBOL_CLOSE, ""};
static const char *kbMapNum[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n", "-", "/", ":", ";", "(", ")", "$", "&",
    "@", "\n", "#+=", ".", ",", "?", "!", "'", "\n", "ABC", "Space", LV_SYMBOL_OK, LV_SYMBOL_CLOSE,
    ""};
static const char *kbMapSym[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n", "_", "\\", "|", "~", "<", ">", "`",
    "\"", "\n", "123", ".", ",", "?", "!", "'", "\n", "ABC", "Space", LV_SYMBOL_OK, LV_SYMBOL_CLOSE,
    ""};

static void applyKbMap() {
  if (!gKbMatrix) return;
  if (gKbLayer == 1)
    lv_btnmatrix_set_map(gKbMatrix, kbMapNum);
  else if (gKbLayer == 2)
    lv_btnmatrix_set_map(gKbMatrix, kbMapSym);
  else
    lv_btnmatrix_set_map(gKbMatrix, gKbShift ? kbMapUpper : kbMapLower);
}

static void onKeyboardBtn(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *m = lv_event_get_target(e);
  uint32_t id = lv_btnmatrix_get_selected_btn(m);
  const char *txt = lv_btnmatrix_get_btn_text(m, id);
  if (!txt || !txt[0]) return;
  sleepNotifyActivity();
  if (!strcmp(txt, LV_SYMBOL_CLOSE)) {
    gShowingKeyboard = false;
    pageEngineReload();
    return;
  }
  if (!strcmp(txt, "123")) {
    gKbLayer = 1;
    applyKbMap();
    return;
  }
  if (!strcmp(txt, "ABC")) {
    gKbLayer = 0;
    applyKbMap();
    return;
  }
  if (!strcmp(txt, "#+=")) {
    gKbLayer = 2;
    applyKbMap();
    return;
  }
  if (!strcmp(txt, "Sh")) {
    gKbShift = !gKbShift;
    applyKbMap();
    return;
  }
  if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) {
    bleSendKey("BACKSPACE");
    return;
  }
  if (!strcmp(txt, LV_SYMBOL_OK)) {
    bleSendKey("ENTER");
    return;
  }
  if (!strcmp(txt, "Space")) {
    bleSendText(" ");
    return;
  }
  bleSendText(txt);
}

static lv_style_t sKbItemStyle;
static lv_style_t sKbItemPressedStyle;
static bool sKbStylesInit = false;

static void ensureKbStyles() {
  if (sKbStylesInit) return;
  sKbStylesInit = true;
  lv_style_init(&sKbItemStyle);
  lv_style_set_bg_color(&sKbItemStyle, lv_color_hex(0x2a2a2a));
  lv_style_set_bg_opa(&sKbItemStyle, LV_OPA_COVER);
  lv_style_set_text_color(&sKbItemStyle, lv_color_hex(0xffffff));
  lv_style_set_radius(&sKbItemStyle, 6);
  lv_style_set_border_width(&sKbItemStyle, 0);
  lv_style_init(&sKbItemPressedStyle);
  /* Vivid orange so taps are clearly visible on the small screen. */
  lv_style_set_bg_color(&sKbItemPressedStyle, lv_color_hex(0xff7a18));
  lv_style_set_bg_opa(&sKbItemPressedStyle, LV_OPA_COVER);
  lv_style_set_text_color(&sKbItemPressedStyle, lv_color_hex(0x000000));
  lv_style_set_border_color(&sKbItemPressedStyle, lv_color_hex(0xffffff));
  lv_style_set_border_width(&sKbItemPressedStyle, 2);
}

static void buildKeyboardPanel() {
  ensureKbStyles();
  if (gKeyboardPanel) lv_obj_del(gKeyboardPanel);
  gKeyboardPanel = lv_obj_create(gScreen);
  lv_obj_set_size(gKeyboardPanel, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_set_pos(gKeyboardPanel, 0, 0);
  lv_obj_set_style_bg_color(gKeyboardPanel, lv_color_hex(0x111111), 0);
  stripContainerStyle(gKeyboardPanel);
  lv_obj_clear_flag(gKeyboardPanel, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_t *title = lv_label_create(gKeyboardPanel);
  lv_label_set_text(title, "Keyboard (BLE)");
  lv_obj_set_pos(title, 8, 2);
  gKbMatrix = lv_btnmatrix_create(gKeyboardPanel);
  lv_obj_set_size(gKbMatrix, SCR_WIDTH - 8, SCR_HEIGHT - STATUS_BAR_H - 8);
  lv_obj_set_pos(gKbMatrix, 4, STATUS_BAR_H + 2);
  lv_obj_set_style_text_font(gKbMatrix, &lv_font_montserrat_14, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(gKbMatrix, lv_color_hex(0x111111), LV_PART_MAIN);
  lv_obj_set_style_border_width(gKbMatrix, 0, LV_PART_MAIN);
  lv_obj_add_style(gKbMatrix, &sKbItemStyle, LV_PART_ITEMS);
  lv_obj_add_style(gKbMatrix, &sKbItemPressedStyle, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_btnmatrix_set_btn_width(gKbMatrix, 9, 2);
  lv_btnmatrix_set_btn_width(gKbMatrix, 19, 2);
  applyKbMap();
  lv_obj_add_event_cb(gKbMatrix, onKeyboardBtn, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_clear_flag(gKbMatrix, LV_OBJ_FLAG_GESTURE_BUBBLE);
}
static void stripContainerStyle(lv_obj_t *obj) {
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}
static void stripWidgetStyle(lv_obj_t *obj) {
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}
static void tuneTouchGestures() {
  lv_indev_t *indev = lv_indev_get_next(nullptr);
  while (indev) {
    if (indev->driver->type == LV_INDEV_TYPE_POINTER) {
      indev->driver->gesture_limit = 28;
      indev->driver->gesture_min_velocity = 3;
    }
    indev = lv_indev_get_next(indev);
  }
}
/** Pick a battery glyph for the current percentage (LV_SYMBOL_BATTERY_*). */
static const char *batterySymbolFor(uint8_t percent, bool charging) {
  if (charging) return LV_SYMBOL_CHARGE;
  if (percent >= 88) return LV_SYMBOL_BATTERY_FULL;
  if (percent >= 63) return LV_SYMBOL_BATTERY_3;
  if (percent >= 38) return LV_SYMBOL_BATTERY_2;
  if (percent >= 13) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

/** Tint the battery glyph red when critically low or amber when charging. */
static uint32_t batteryColorFor(uint8_t percent, bool charging) {
  if (charging) return 0x80FF80;       /* green-ish while charging */
  if (percent <= 12) return 0xFF5050;  /* red — get to a charger */
  if (percent <= 25) return 0xFFB050;  /* amber */
  return 0xFFFFFF;
}

static void buildStatusBar() {
  if (!gStatusBar) {
    gStatusBar = lv_obj_create(gScreen);
    lv_obj_set_size(gStatusBar, SCR_WIDTH, STATUS_BAR_H);
    lv_obj_set_pos(gStatusBar, 0, 0);
    lv_obj_set_style_bg_color(gStatusBar, lv_color_hex(0x1a1a1a), 0);
    stripContainerStyle(gStatusBar);
    lv_obj_set_style_pad_left(gStatusBar, 2, 0);
    lv_obj_set_style_pad_right(gStatusBar, 2, 0);
    gBatIconLbl = lv_label_create(gStatusBar);
    lv_obj_align(gBatIconLbl, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_text_font(gBatIconLbl, &lv_font_montserrat_12, 0);
    gBatLbl = lv_label_create(gStatusBar);
    /* Percentage sits just right of the icon. */
    lv_obj_align(gBatLbl, LV_ALIGN_LEFT_MID, 22, 0);
    lv_obj_set_style_text_font(gBatLbl, &lv_font_montserrat_12, 0);
    gTitleLbl = lv_label_create(gStatusBar);
    lv_obj_align(gTitleLbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(gTitleLbl, &lv_font_montserrat_12, 0);
    gClockLbl = lv_label_create(gStatusBar);
    lv_obj_align(gClockLbl, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_text_font(gClockLbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(gClockLbl, "");
    lv_obj_add_flag(gStatusBar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gStatusBar, [](lv_event_t *e) {
      if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        gShowingSettings = true;
        pageEngineReload();
      }
    }, LV_EVENT_LONG_PRESSED, nullptr);
  }
  static uint32_t sLastBatUi = 0;
  if (millis() - sLastBatUi > 10000) {
    sLastBatUi = millis();
    batteryUpdate();
  }
  const uint8_t pct = (uint8_t)batteryPercent();
  const bool charging = batteryCharging();
  lv_label_set_text(gBatIconLbl, batterySymbolFor(pct, charging));
  lv_obj_set_style_text_color(gBatIconLbl, lv_color_hex(batteryColorFor(pct, charging)), 0);
  lv_label_set_text(gBatLbl, (String(pct) + "%").c_str());
  const PageDef *p = gCfg ? configFindPage(*gCfg, gExec ? gExec->activePageId() : "") : nullptr;
  lv_label_set_text(gTitleLbl, p ? p->name.c_str() : "Omote");
  if (gClockLbl) {
    char hm[8];
    if (timeGetLocalHm(hm, sizeof(hm))) {
      lv_label_set_text(gClockLbl, hm);
    } else {
      lv_label_set_text(gClockLbl, "");
    }
  }
}
static void buildSettingsPanel() {
  if (gSettingsPanel) lv_obj_del(gSettingsPanel);
  gSettingsPanel = lv_obj_create(gScreen);
  lv_obj_set_size(gSettingsPanel, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_set_style_bg_color(gSettingsPanel, lv_color_hex(0x111111), 0);
  lv_obj_set_pos(gSettingsPanel, 0, 0);
  stripContainerStyle(gSettingsPanel);
  lv_obj_clear_flag(gSettingsPanel, LV_OBJ_FLAG_GESTURE_BUBBLE);
  int y = 8;
  auto row = [&](const char *txt) {
    lv_obj_t *l = lv_label_create(gSettingsPanel);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, 8, y);
    y += 22;
  };
  row("Settings");
  row((String("WiFi: ") + wifiUiStateText()).c_str());
  row((String("IP: ") + (WiFi.localIP().toString().c_str())).c_str());
  row((String("Battery: ") + batteryPercent() + "%").c_str());
  lv_obj_t *brightLbl = lv_label_create(gSettingsPanel);
  lv_label_set_text(brightLbl, "Brightness");
  lv_obj_set_pos(brightLbl, 8, y);
  y += 18;
  lv_obj_t *slider = lv_slider_create(gSettingsPanel);
  lv_obj_set_width(slider, SCR_WIDTH - 24);
  lv_obj_set_pos(slider, 8, y);
  lv_slider_set_range(slider, 10, 255);
  lv_slider_set_value(slider, gDevSettings ? gDevSettings->brightness : 180, LV_ANIM_OFF);
  stripContainerStyle(slider);
  lv_obj_add_flag(slider, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(slider, [](lv_event_t *e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (!displayIsOff()) displaySetBacklight((uint8_t)v);
    if (gDevSettings) {
      gDevSettings->brightness = (uint8_t)v;
      sleepSetUserBrightness(gDevSettings->brightness);
      deviceSettingsSave(*gDevSettings);
    }
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  y += 36;
  gBleStatusLbl = lv_label_create(gSettingsPanel);
  lv_label_set_text(gBleStatusLbl, "BLE: …");
  lv_obj_set_pos(gBleStatusLbl, 8, y);
  y += 18;
  mkSettingsBtn(gSettingsPanel, "Pair", 8, y, 108, 28, [](lv_event_t *) {
    if (!bleIsInitialized()) bleInit();
    bleStartPairingMode();
    refreshBleSettingsLabel();
  });
  mkSettingsBtn(gSettingsPanel, "Disc", 124, y, 108, 28, [](lv_event_t *) {
    if (!bleIsInitialized()) bleInit();
    bleDisconnectClients();
    refreshBleSettingsLabel();
  });
  y += 32;
  mkSettingsBtn(gSettingsPanel, "Forget", 8, y, 108, 28, [](lv_event_t *) {
    bleForgetBonds();
    refreshBleSettingsLabel();
  });
  mkSettingsBtn(gSettingsPanel, "Refresh", 124, y, 108, 28,
                [](lv_event_t *) { refreshBleSettingsLabel(); });
  refreshBleSettingsLabel();
  y += 36;
  lv_obj_t *btnBack = lv_btn_create(gSettingsPanel);
  lv_obj_set_pos(btnBack, 8, SCR_HEIGHT - 44);
  lv_obj_set_size(btnBack, 100, 36);
  lv_obj_t *lbl = lv_label_create(btnBack);
  lv_label_set_text(lbl, "Back");
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btnBack, [](lv_event_t *) {
    gShowingSettings = false;
    pageEngineReload();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *btnReboot = lv_btn_create(gSettingsPanel);
  lv_obj_set_pos(btnReboot, 120, SCR_HEIGHT - 44);
  lv_obj_set_size(btnReboot, 100, 36);
  lbl = lv_label_create(btnReboot);
  lv_label_set_text(lbl, "Reboot");
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btnReboot, [](lv_event_t *) { ESP.restart(); }, LV_EVENT_CLICKED, nullptr);
}
void pageEngineSetDeviceSettings(DeviceSettings *ds) { gDevSettings = ds; }
void pageEngineSetHaSettings(HaSettings *ha) { gHaSettings = ha; }
void pageEngineInit(OmoteConfig *config, ActionExecutor *executor) {
  gCfg = config;
  gExec = executor;
  sPageEngineBootMs = millis();
  sHaPollNotBeforeMs = sPageEngineBootMs + kHaBootGraceMs;
  gScreen = lv_scr_act();
  lv_obj_set_size(gScreen, SCR_WIDTH, SCR_HEIGHT);
  lv_obj_set_style_bg_color(gScreen, lv_color_hex(0x111111), 0);
  stripContainerStyle(gScreen);
  lv_obj_add_flag(gScreen, LV_OBJ_FLAG_GESTURE_BUBBLE);
  gContent = lv_obj_create(gScreen);
  lv_obj_set_size(gContent, SCR_WIDTH, SCR_HEIGHT - STATUS_BAR_H);
  lv_obj_set_pos(gContent, 0, STATUS_BAR_H);
  lv_obj_set_style_bg_opa(gContent, LV_OPA_TRANSP, 0);
  stripContainerStyle(gContent);
  tuneTouchGestures();
  pageEngineReload();
  displayRefreshNow();
}
void pageEngineRequestReload() { sReloadPending = true; }

void pageEngineUnloadUi() {
  if (!gScreen) return;
  clearWidgets();
  haIconClearQueue();
  if (gSettingsPanel) {
    lv_obj_del(gSettingsPanel);
    gSettingsPanel = nullptr;
  }
  if (gStatusBar) {
    lv_obj_del(gStatusBar);
    gStatusBar = nullptr;
    gBatLbl = nullptr;
    gTitleLbl = nullptr;
  }
  if (gContent) {
    lv_obj_del(gContent);
    gContent = nullptr;
  }
  gShowingSettings = false;
  gShowingKeyboard = false;
  gCachedPageId = "";
}

void pageEngineReload() {
  if (!gCfg || !gExec) return;
  diagSetStage("ui_reload");
  clearWidgets();
  if (gSettingsPanel) {
    lv_obj_del(gSettingsPanel);
    gSettingsPanel = nullptr;
  }
  buildStatusBar();
  if (gShowingSettings) {
    buildSettingsPanel();
    pageEngineReloadDone();
    return;
  }
  if (gShowingKeyboard) {
    buildKeyboardPanel();
    pageEngineReloadDone();
    return;
  }
  if (!wifiIsConnected() &&
      (wifiUiState() == WIFI_UI_CONNECTING || wifiUiState() == WIFI_UI_SETUP_AP ||
       wifiUiState() == WIFI_UI_FAILED)) {
    gOverlayMsg = lv_label_create(gScreen);
    lv_label_set_text(gOverlayMsg, wifiUiStateText());
    lv_obj_center(gOverlayMsg);
    pageEngineReloadDone();
    return;
  }
  String pageId = gExec->activePageId();
  const PageDef *page = configFindPage(*gCfg, pageId);
  if (!page && !gCfg->pages.empty()) {
    pageId = gCfg->pages[0].id;
    gExec->setActivePage(pageId);
    gCfg->activePageId = pageId;
    page = &gCfg->pages[0];
    Serial.printf("page_engine: active page repaired -> %s\n", pageId.c_str());
  }
  if (!page) {
    gOverlayMsg = lv_label_create(gScreen);
    lv_label_set_text(gOverlayMsg, "No pages — deploy layout from web UI");
    lv_obj_center(gOverlayMsg);
    pageEngineReloadDone();
    return;
  }
  gCachedPageId = pageId;
  gCachedButtons = page->buttons;
  sHaSyncCursor = 0;
  for (size_t ti = 0; ti < gCachedButtons.size(); ti++) {
    if (gCachedButtons[ti].widget == WIDGET_CLIMATE_THERMOSTAT) {
      buildClimateThermostat(gCachedButtons[ti], ti);
      scheduleClimateResync(400);
      pageEngineReloadDone();
      return;
    }
  }
  buildPageWidgets();
  pageEngineReloadDone();
}

static void updateStatusBarTitle() {
  if (!gTitleLbl || !gCfg || !gExec) return;
  const PageDef *p = configFindPage(*gCfg, gExec->activePageId());
  lv_label_set_text(gTitleLbl, p ? p->name.c_str() : "Omote");
}

static void pageEngineSwitchPageLight() {
  if (!gCfg || !gExec || !gContent || gShowingSettings || gShowingKeyboard) {
    pageEngineRequestReload();
    return;
  }
  markUiBusy(kUiBusyAfterSwipeMs);
  diagSetStage("ui_switch");
  clearWidgets();
  updateStatusBarTitle();

  const PageDef *page = configFindPage(*gCfg, gExec->activePageId());
  if (!page) {
    pageEngineRequestReload();
    return;
  }
  gCachedPageId = gExec->activePageId();
  gCachedButtons = page->buttons;
  sHaSyncCursor = 0;

  for (size_t ti = 0; ti < gCachedButtons.size(); ti++) {
    if (gCachedButtons[ti].widget == WIDGET_CLIMATE_THERMOSTAT) {
      buildClimateThermostat(gCachedButtons[ti], ti);
      scheduleClimateResync(kUiBusyAfterSwipeMs);
      diagSetStage("ui_loop");
      return;
    }
  }
  buildPageWidgets();
  diagSetStage("ui_loop");
}

static void buildPageWidgets() {
  for (size_t i = 0; i < gCachedButtons.size(); i++) {
    const auto &btn = gCachedButtons[i];
    lv_obj_t *w = nullptr;
    if (btn.widget == WIDGET_TOGGLE) {
      lv_obj_t *row = lv_obj_create(gContent);
      makeTransparentContainer(row);
      lv_obj_set_pos(row, btn.x, btn.y);
      lv_obj_set_size(row, btn.w > 0 ? btn.w : 120, btn.h > 0 ? btn.h : (btn.labelBelow ? 44 : 32));
      lv_obj_set_flex_flow(row, btn.labelBelow ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(row, 2, 0);
      lv_obj_set_style_pad_column(row, 6, 0);
      const String ent = btn.action.entityId.length() ? btn.action.entityId : btn.label;
      haIconAttach(row, ent, btn.haIcon);
      w = lv_switch_create(row);
      lv_obj_set_size(w, 44, 22);
      lv_obj_clear_flag(w, LV_OBJ_FLAG_GESTURE_BUBBLE);
      if (btn.label.length()) {
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, btn.label.c_str());
        applyLabelStyle(lbl, btn);
        lv_obj_set_width(lbl, btn.w > 0 ? btn.w - 4 : 116);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
      }
      lv_obj_add_event_cb(w, onToggleChanged, LV_EVENT_VALUE_CHANGED, (void *)i);
      bool cachedOn = false;
      if (btn.action.entityId.length() && entityCacheGet(btn.action.entityId, cachedOn)) {
        setSwitchChecked(w, cachedOn, false);
      }
      gWidgets.push_back(row);
      ToggleUi tu;
      tu.sw = w;
      tu.btnIndex = i;
      gToggles.push_back(tu);
      continue;
    }
    if (btn.widget == WIDGET_LABEL) {
      lv_obj_t *box = lv_obj_create(gContent);
      stripContainerStyle(box);
      applyButtonStyle(box, btn);
      lv_obj_add_flag(box, LV_OBJ_FLAG_GESTURE_BUBBLE);
      lv_obj_set_pos(box, btn.x, btn.y);
      lv_obj_set_size(box, btn.w > 0 ? btn.w : 100, btn.h > 0 ? btn.h : 40);
      lv_obj_set_flex_flow(box, btn.labelBelow ? LV_FLEX_FLOW_COLUMN_REVERSE : LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(box, 2, 0);
      lv_obj_set_style_pad_all(box, 4, 0);
      if (btn.label.length() && !btn.labelBelow) {
        lv_obj_t *title = lv_label_create(box);
        lv_label_set_text(title, btn.label.c_str());
        applyLabelStyle(title, btn);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
        lv_obj_set_width(title, btn.w > 4 ? btn.w - 8 : 92);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
      }
      lv_obj_t *val = lv_label_create(box);
      lv_label_set_text(val, "…");
      applyLabelStyle(val, btn);
      lv_obj_set_style_text_font(val, &lv_font_montserrat_12, 0);
      lv_obj_set_width(val, btn.w > 4 ? btn.w - 8 : 92);
      lv_label_set_long_mode(val, LV_LABEL_LONG_SCROLL_CIRCULAR);
      if (btn.labelBelow && btn.label.length()) {
        lv_obj_t *cap = lv_label_create(box);
        lv_label_set_text(cap, btn.label.c_str());
        applyLabelStyle(cap, btn);
        lv_obj_set_style_text_color(cap, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
        lv_obj_set_width(cap, btn.w > 4 ? btn.w - 8 : 92);
        lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
      }
      gWidgets.push_back(box);
      HaLabelUi hu;
      hu.valueLbl = val;
      hu.btnIndex = i;
      gHaLabels.push_back(hu);
      continue;
    }
    if (btn.widget == WIDGET_CLIMATE) {
      lv_obj_t *box = lv_obj_create(gContent);
      makeTransparentContainer(box);
      lv_obj_set_pos(box, btn.x, btn.y);
      lv_obj_set_size(box, btn.w > 0 ? btn.w : 220, btn.h > 0 ? btn.h : 88);
      lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(box, 2, 0);
      if (btn.label.length()) {
        lv_obj_t *title = lv_label_create(box);
        lv_label_set_text(title, btn.label.c_str());
        applyLabelStyle(title, btn);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
      }
      lv_obj_t *tempLbl = lv_label_create(box);
      lv_label_set_text(tempLbl, "--°");
      applyLabelStyle(tempLbl, btn);
      lv_obj_t *row = lv_obj_create(box);
      makeTransparentContainer(row);
      lv_obj_set_size(row, LV_PCT(100), 28);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      auto mkBtn = [&](const char *txt, intptr_t ud) {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_size(b, 36, 26);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, onClimateTempBtn, LV_EVENT_CLICKED, (void *)ud);
        lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
      };
      mkBtn("-", (intptr_t)((i << 1) | 0));
      mkBtn("+", (intptr_t)((i << 1) | 1));
      lv_obj_t *modeLbl = lv_label_create(box);
      lv_label_set_text(modeLbl, "…");
      lv_obj_set_style_text_color(modeLbl, lv_color_hex(0xaaaaaa), 0);
      lv_obj_t *modes = lv_obj_create(box);
      makeTransparentContainer(modes);
      lv_obj_set_size(modes, LV_PCT(100), 26);
      lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(modes, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      const char *labels[] = {"Off", "Heat", "Cool", "Auto"};
      for (int m = 0; m < 4; m++) {
        lv_obj_t *b = lv_btn_create(modes);
        lv_obj_set_size(b, 48, 24);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[m]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, onClimateModeBtn, LV_EVENT_CLICKED, (void *)(intptr_t)((i << 3) | m));
        lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
      }
      gWidgets.push_back(box);
      HaClimateUi cu;
      cu.tempLbl = tempLbl;
      cu.modeLbl = modeLbl;
      cu.btnIndex = i;
      gHaClimates.push_back(cu);
      continue;
    }
    {
      w = lv_btn_create(gContent);
      lv_obj_set_pos(w, btn.x, btn.y);
      lv_obj_set_size(w, btn.w, btn.h);
      stripWidgetStyle(w);
      applyButtonStyle(w, btn);
      lv_obj_t *lbl = lv_label_create(w);
      lv_label_set_text(lbl, btn.label.c_str());
      applyLabelStyle(lbl, btn);
      lv_obj_set_width(lbl, btn.w - 8);
      lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
      lv_obj_center(lbl);
      lv_obj_add_event_cb(w, onBtnEvent, LV_EVENT_ALL, (void *)i);
      gWidgets.push_back(w);
    }
  }
}

void pageEngineAfterAction(bool pageChanged) {
  if (pageChanged) {
    markUiBusy(kUiBusyAfterSwipeMs);
    sPageSwitchPending = true;
  } else {
    buildStatusBar();
    displayRequestRefresh();
  }
}
void pageEngineLoop() {
  if (displayIsOff()) return;

  /* Touch + draw + page changes only — no blocking HA in this path. */
  diagSetStage("lvgl_timer");
  lv_timer_handler();
  if (displayConsumeRefreshPending()) displayRefreshNow();
  diagSetStage("ui_loop");

  if (sPageSwitchPending) {
    sPageSwitchPending = false;
    pageEngineSwitchPageLight();
    displayRequestRefresh();
  }

  if (sReloadPending) {
    sReloadPending = false;
    pageEngineReload();
    displayRequestRefresh();
  }

  static uint32_t lastBar = 0;
  static uint32_t lastClock = 0;
  const uint32_t now = millis();
  if (now - lastBar > 5000 && millis() >= sUiBusyUntilMs) {
    lastBar = now;
    buildStatusBar();
    displayRequestRefresh();
  } else if (gClockLbl && now - lastClock > 1000) {
    lastClock = now;
    char hm[8];
    if (timeGetLocalHm(hm, sizeof(hm))) {
      lv_label_set_text(gClockLbl, hm);
      displayRequestRefresh();
    }
  }

  if (displayConsumeRefreshPending()) displayRefreshNow();
}

void pageEngineLoopNetwork() {
  if (displayIsOff()) return;
  if (diagHttpBusy()) return;

  /* HVAC data: highest priority while thermostat is visible (retry until success). */
  if (climateNeedsService()) {
    const bool uiBusy = millis() < sUiBusyUntilMs;
    if (!uiBusy || sClimateLastOkMs == 0) {
      diagSetStage("ha_climate_sync");
      if (syncClimateThermostat()) {
        sClimateResyncPending = false;
      } else {
        scheduleClimateResync(kClimateRetryMs);
      }
      displayRequestRefresh();
      diagSetStage("ui_loop");
      return;
    }
  }

  if (millis() < sUiBusyUntilMs || netWorkerWebUiActive()) return;

  if (sPersistActivePageDeadline && millis() >= sPersistActivePageDeadline) {
    sPersistActivePageDeadline = 0;
    if (gExec) configPersistActivePageId(gExec->activePageId());
    return;
  }

  if (sHaUiJob.pending) {
    processHaUiJob();
    return;
  }

  const uint32_t now = millis();
  const bool haPollDue =
      (now >= sHaPollNotBeforeMs) && (now - sPageEngineBootMs >= kHaBootGraceMs) &&
      (sLastHaPoll == 0 || now - sLastHaPoll >= kHaPollIntervalMs);
  if (gHaSettings && gHaSettings->configured && wifiIsConnected() && millis() >= sHaUiQuietUntil &&
      haPollDue && (!gToggles.empty() || !gHaLabels.empty() || !gHaClimates.empty())) {
    sLastHaPoll = now;
    diagSetStage("ha_poll");
    pageEngineSyncHaToggles();
    displayRequestRefresh();
    diagSetStage("ui_loop");
  }
}
void pageEngineHandleKey(char key, bool pressed) {
  if (!pressed || !gCfg || !gExec) return;
  Serial.printf("KEY pressed: '%c' active_page=%s\n", key, gExec->activePageId().c_str());
  sleepNotifyActivity();
  String pageId = gExec->activePageId();
  const PageDef *page = configFindPage(*gCfg, pageId);
  if (page) {
    const ActionDef *act = pageFindKeyAction(*page, key);
    if (act) {
      Serial.printf("KEY action(page): '%c' type=%s\n", key, act->type.c_str());
      String prev = gExec->activePageId();
      gExec->execute(*act);
      pageEngineAfterAction(gExec->activePageId() != prev);
      return;
    }
  }
  for (const auto &kb : gCfg->keymap) {
    if (kb.key == key && (kb.pageId.length() == 0 || kb.pageId == pageId)) {
      Serial.printf("KEY action(global): '%c' type=%s page_scope=%s\n", key, kb.action.type.c_str(),
                    kb.pageId.c_str());
      String prev = gExec->activePageId();
      gExec->execute(kb.action);
      pageEngineAfterAction(gExec->activePageId() != prev);
      return;
    }
  }
  Serial.printf("KEY no action: '%c'\n", key);
  if (key == '1' && configFindPage(*gCfg, "home")) {
    gExec->setActivePage("home");
    saveActivePage();
    pageEngineAfterAction(true);
  } else if (key == '2' && configFindPage(*gCfg, "google_tv")) {
    gExec->setActivePage("google_tv");
    saveActivePage();
    pageEngineAfterAction(true);
  }
}
