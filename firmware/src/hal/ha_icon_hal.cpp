#include "hal/ha_icon_hal.h"
#include "net/net_worker.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <PNGdec.h>
#include <vector>

static const uint16_t kIconSize = 20;
static const char kIconMagic[] = "ICN1";
static String sFetchQueue;
static bool sFetchBusy = false;
static TaskHandle_t sIconTask = nullptr;

struct IconInstance {
  std::vector<uint16_t> pixels;
  lv_img_dsc_t dsc{};
};
static std::vector<IconInstance> gIcons;

static bool isValidMdiIcon(const String &mdi) {
  if (mdi.length() < 5) return false;
  if (!mdi.startsWith("mdi:")) return false;
  if (mdi.equalsIgnoreCase("mdi:null") || mdi.equalsIgnoreCase("null")) return false;
  return true;
}

static String iconCachePath(const String &mdi) {
  String name = mdi;
  name.replace("mdi:", "");
  name.replace("/", "_");
  if (name.length() == 0 || name.equalsIgnoreCase("null")) return "";
  return String("/icons/") + name + ".rgb";
}

static lv_color_t domainColor(const String &entityId) {
  int dot = entityId.indexOf('.');
  String dom = dot > 0 ? entityId.substring(0, dot) : entityId;
  dom.toLowerCase();
  if (dom == "light") return lv_color_hex(0xFFE066);
  if (dom == "climate") return lv_color_hex(0x66AAFF);
  if (dom == "switch") return lv_color_hex(0x88CC88);
  if (dom == "fan") return lv_color_hex(0x88DDFF);
  if (dom == "cover") return lv_color_hex(0xCCAA88);
  if (dom == "scene") return lv_color_hex(0xCC88FF);
  return lv_color_hex(0x888888);
}

static char domainLetter(const String &entityId) {
  int dot = entityId.indexOf('.');
  String dom = dot > 0 ? entityId.substring(0, dot) : entityId;
  if (dom.length() == 0) return '?';
  return (char)toupper(dom.charAt(0));
}

static lv_obj_t *makeDomainBadge(lv_obj_t *parent, const String &entityId) {
  lv_obj_t *badge = lv_obj_create(parent);
  lv_obj_set_size(badge, 18, 18);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(badge, domainColor(entityId), 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_pad_all(badge, 0, 0);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(badge, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_t *l = lv_label_create(badge);
  char ch[2] = {domainLetter(entityId), 0};
  lv_label_set_text(l, ch);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0x111111), 0);
  lv_obj_center(l);
  return badge;
}

static bool loadCachedRgb(const String &path, std::vector<uint16_t> &pixels) {
  if (path.length() == 0 || !LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f || f.size() < 8) return false;
  char magic[4];
  if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, kIconMagic, 4) != 0) {
    f.close();
    return false;
  }
  uint16_t w = 0, h = 0;
  f.read((uint8_t *)&w, 2);
  f.read((uint8_t *)&h, 2);
  if (w != kIconSize || h != kIconSize) {
    f.close();
    return false;
  }
  pixels.resize(w * h);
  f.read((uint8_t *)pixels.data(), pixels.size() * 2);
  f.close();
  return true;
}

static bool saveCachedRgb(const String &path, const std::vector<uint16_t> &px) {
  if (path.length() == 0) return false;
  if (!LittleFS.exists("/icons")) LittleFS.mkdir("/icons");
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  f.write((const uint8_t *)kIconMagic, 4);
  uint16_t w = kIconSize, h = kIconSize;
  f.write((uint8_t *)&w, 2);
  f.write((uint8_t *)&h, 2);
  f.write((const uint8_t *)px.data(), px.size() * 2);
  f.close();
  return true;
}

static std::vector<uint16_t> sDecodeBuf;
static int pngDrawLine(PNGDRAW *pDraw) {
  uint16_t *src = (uint16_t *)pDraw->pPixels;
  if (pDraw->y >= (int)kIconSize) return 1;
  for (int x = 0; x < pDraw->iWidth && x < (int)kIconSize; x++) {
    uint16_t c = src[x];
    if (c != 0) sDecodeBuf[pDraw->y * kIconSize + x] = c;
  }
  return 1;
}

static bool fetchMdiPng(const String &mdi) {
#if !OMOTE_ICON_FETCH_ENABLED
  (void)mdi;
  return false;
#else
  if (!WiFi.isConnected() || !isValidMdiIcon(mdi)) return false;
  if (!netWorkerLock(8000)) return false;
  const String path = iconCachePath(mdi);
  if (path.length() == 0) {
    netWorkerUnlock();
    return false;
  }
  if (LittleFS.exists(path)) {
    netWorkerUnlock();
    return true;
  }

  String name = mdi.substring(4);
  String url = "https://api.iconify.design/mdi/" + name + ".png?width=20&height=20";
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(url)) {
    netWorkerUnlock();
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    http.end();
    netWorkerUnlock();
    return false;
  }
  const int len = http.getSize();
  if (len <= 0 || len > 8000) {
    http.end();
    netWorkerUnlock();
    return false;
  }
  std::vector<uint8_t> pngData((size_t)len);
  WiFiClient *stream = http.getStreamPtr();
  int pos = 0;
  while (http.connected() && pos < len) {
    const int n = stream->read(pngData.data() + pos, len - pos);
    if (n <= 0) break;
    pos += n;
  }
  http.end();
  if (pos < 32) {
    netWorkerUnlock();
    return false;
  }

  sDecodeBuf.assign(kIconSize * kIconSize, 0);
  PNG png;
  int rc = png.openRAM(pngData.data(), pos, pngDrawLine);
  if (rc != PNG_SUCCESS) {
    netWorkerUnlock();
    return false;
  }
  rc = png.decode(nullptr, 0);
  png.close();
  if (rc != PNG_SUCCESS) {
    netWorkerUnlock();
    return false;
  }
  const bool ok = saveCachedRgb(path, sDecodeBuf);
  netWorkerUnlock();
  return ok;
#endif
}

static void iconWorkerTask(void *) {
#if !OMOTE_ICON_FETCH_ENABLED
  for (;;) vTaskDelay(portMAX_DELAY);
#else
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (sFetchQueue.length() > 0 && WiFi.isConnected()) {
      const int comma = sFetchQueue.indexOf(',');
      String mdi = comma >= 0 ? sFetchQueue.substring(0, comma) : sFetchQueue;
      if (comma >= 0)
        sFetchQueue = sFetchQueue.substring(comma + 1);
      else
        sFetchQueue = "";
      if (!isValidMdiIcon(mdi)) continue;
      sFetchBusy = true;
      fetchMdiPng(mdi);
      sFetchBusy = false;
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
#endif
}

static void ensureIconWorker() {
  if (sIconTask) return;
  xTaskCreatePinnedToCore(iconWorkerTask, "haicon", 12288, nullptr, 1, &sIconTask, 0);
}

void haIconQueueFetch(const String &mdiIcon) {
#if !OMOTE_ICON_FETCH_ENABLED
  (void)mdiIcon;
  return;
#endif
  if (!isValidMdiIcon(mdiIcon)) return;
  if (sFetchQueue.indexOf(mdiIcon) >= 0) return;
  if (sFetchQueue.length()) sFetchQueue += ",";
  sFetchQueue += mdiIcon;
  ensureIconWorker();
  if (sIconTask) xTaskNotifyGive(sIconTask);
}

void haIconClearQueue() { sFetchQueue = ""; }

void haIconClearAll() {
  std::vector<IconInstance> freed;
  gIcons.swap(freed);
}

void haIconLoop() {
  /* Downloads run on dedicated task — never call HTTP/PNG from loopTask. */
}

lv_obj_t *haIconAttach(lv_obj_t *parent, const String &entityId, const String &mdiIcon) {
  std::vector<uint16_t> px;
  bool haveImg = false;
#if OMOTE_ICON_FETCH_ENABLED
  if (isValidMdiIcon(mdiIcon)) {
    const String path = iconCachePath(mdiIcon);
    haveImg = loadCachedRgb(path, px);
    if (!haveImg) haIconQueueFetch(mdiIcon);
  }
#else
  (void)mdiIcon;
#endif
  if (!haveImg) return makeDomainBadge(parent, entityId);

  IconInstance inst;
  inst.pixels = px;
  inst.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  inst.dsc.header.w = kIconSize;
  inst.dsc.header.h = kIconSize;
  inst.dsc.data_size = inst.pixels.size() * 2;
  inst.dsc.data = (const uint8_t *)inst.pixels.data();
  gIcons.push_back(inst);

  lv_obj_t *img = lv_img_create(parent);
  lv_img_set_src(img, &gIcons.back().dsc);
  lv_obj_set_size(img, kIconSize, kIconSize);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(img, LV_OBJ_FLAG_GESTURE_BUBBLE);
  return img;
}
