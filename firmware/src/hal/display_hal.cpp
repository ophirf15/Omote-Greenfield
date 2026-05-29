#include "hal/display_hal.h"
#include "net/runtime_diag.h"
#include "hal/ble_hal.h"
#include "hal/omote_i2c.h"
#include "hal/pins.h"
#include "hal/sleep_hal.h"
#include "ui_runtime/page_engine.h"
#include "driver/ledc.h"
#include <stdlib.h>

#define LEDC_SPEED_MODE LEDC_HIGH_SPEED_MODE

OmoteDisplay::OmoteDisplay() {
  {
    auto cfg = _bus.config();
    cfg.spi_host = SPI2_HOST;
    cfg.spi_mode = 0;
    cfg.freq_write = 40000000;
    cfg.freq_read = 16000000;
    cfg.pin_sclk = PIN_LCD_SCK;
    cfg.pin_mosi = PIN_LCD_MOSI;
    cfg.pin_miso = -1;
    cfg.pin_dc = PIN_LCD_DC;
    _bus.config(cfg);
    _panel.setBus(&_bus);
  }
  {
    auto cfg = _panel.config();
    cfg.pin_cs = PIN_LCD_CS;
    cfg.pin_rst = -1;
    cfg.pin_busy = -1;
    cfg.memory_width = SCR_WIDTH;
    cfg.memory_height = SCR_HEIGHT;
    cfg.panel_width = SCR_WIDTH;
    cfg.panel_height = SCR_HEIGHT;
    cfg.offset_rotation = 2;
    _panel.config(cfg);
  }
  {
    auto cfg = _touch.config();
    cfg.i2c_addr = 0x38;
    cfg.i2c_port = 0;
    cfg.pin_sda = PIN_I2C_SDA;
    cfg.pin_scl = PIN_I2C_SCL;
    cfg.freq = 400000;
    cfg.x_min = 0;
    cfg.x_max = SCR_WIDTH - 1;
    cfg.y_min = 0;
    cfg.y_max = SCR_HEIGHT - 1;
    _touch.config(cfg);
    _panel.setTouch(&_touch);
  }
  setPanel(&_panel);
}

OmoteDisplay tft;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

static void lvglFlush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  diagFlushBegin();
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushPixelsDMA(reinterpret_cast<uint16_t *>(&color_p->full), w * h);
  tft.endWrite();
  diagFlushEnd();
  lv_disp_flush_ready(disp);
}

static lv_point_t sTouchStart;
static uint16_t sTouchLastX = 0;
static uint16_t sTouchLastY = 0;
static bool sTouchDown = false;

static void lvglTouchRead(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  if (displayIsOff()) {
    data->state = LV_INDEV_STATE_REL;
    sTouchDown = false;
    return;
  }
  uint16_t x, y;
  if (tft.getTouch(&x, &y)) {
    if (!sTouchDown) {
      sTouchDown = true;
      sTouchStart.x = x;
      sTouchStart.y = y;
    }
    sTouchLastX = x;
    sTouchLastY = y;
    sleepNotifyActivity();
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    if (sTouchDown) {
      const int dx = (int)sTouchLastX - (int)sTouchStart.x;
      const int dy = (int)sTouchLastY - (int)sTouchStart.y;
      pageEngineOnSwipeDrag(dx, dy);
      sTouchDown = false;
    }
    data->state = LV_INDEV_STATE_REL;
  }
}

static bool sLedcActive = false;
static uint32_t sFadeInStart = 0;
static bool sDisplayOff = false;
static bool sRefreshPending = false;

static void backlightEnsurePwm() {
  if (!sLedcActive) displayBacklightInit();
}

void displayBacklightInit() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = LEDC_TIMER_1;
  timer.freq_hz = 640;
  timer.clk_cfg = LEDC_USE_APB_CLK;
  ledc_timer_config(&timer);

  ledc_channel_config_t ch = {};
  ch.gpio_num = (gpio_num_t)PIN_LCD_BL;
  ch.speed_mode = LEDC_SPEED_MODE;
  ch.channel = LEDC_CHANNEL_5;
  ch.timer_sel = LEDC_TIMER_1;
  ch.duty = 0;
  ch.hpoint = 0;
  ch.flags.output_invert = 1;
  ledc_channel_config(&ch);
  sLedcActive = true;
}

void displaySetBacklight(uint8_t brightness) {
  if (sDisplayOff) return;
  pinMode(PIN_LCD_EN, OUTPUT);
  digitalWrite(PIN_LCD_EN, LOW);
  backlightEnsurePwm();
  ledcWrite(LEDC_CHANNEL_5, brightness);
  Serial.printf("Backlight PWM duty=%u\n", (unsigned)brightness);
}

void displayResetBacklightFade() {
  sLedcActive = false;
  sFadeInStart = 0;
}

void displayEnterPreSleepDim() {
  backlightEnsurePwm();
  const uint8_t b = sleepGetBacklightBrightness();
  const uint32_t duty = (uint32_t)(b * 0.3f);
  ledcWrite(LEDC_CHANNEL_5, duty < 4 ? 4 : duty);
}

void displayPowerOff() {
  if (sDisplayOff) return;
  sDisplayOff = true;
  // Hard-cut backlight for reliable "screen off".
  if (sLedcActive) {
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL_5, 255);
    sLedcActive = false;
  }
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH); // OMOTE: HIGH = backlight off
  sleepOnDisplayPoweredOff();
}

void displayPowerOn() {
  if (!sDisplayOff) return;
  sDisplayOff = false;
  omoteI2cInit();
  pinMode(PIN_LCD_EN, OUTPUT);
  digitalWrite(PIN_LCD_EN, LOW);
  // Immediate visible wake fallback: active-low backlight ON.
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
  delay(2);
  // Re-attach PWM after hard-cut.
  displayBacklightInit();
  ledcWrite(LEDC_CHANNEL_5, sleepGetBacklightBrightness());
  displayPump();
  Serial.printf("Display wake: EN=%d BL=%d brightness=%u\n", digitalRead(PIN_LCD_EN),
                digitalRead(PIN_LCD_BL), (unsigned)sleepGetBacklightBrightness());
  // Open BLE undirected adv briefly so a bonded TV can reconnect after the
  // screen-off period (where the TV may have closed the link).
  bleOnWake();
}

bool displayIsOff() { return sDisplayOff; }

void displayUpdateBacklight() {
  if (sDisplayOff) return;
  if (sFadeInStart == 0) sFadeInStart = millis();

  const uint8_t brightness = sleepGetBacklightBrightness();
  const uint32_t fadeMs = brightness;

  if (millis() < sFadeInStart + fadeMs) {
    backlightEnsurePwm();
    ledcWrite(LEDC_CHANNEL_5, (uint32_t)(millis() - sFadeInStart));
    return;
  }

  if (sleepInPreSleepDimPhase()) {
    backlightEnsurePwm();
    const uint32_t timeout = sleepGetTimeoutMs();
    const uint32_t dimLead = sleepGetDimLeadMs();
    const uint32_t idle = millis() - sleepGetLastActivityMs();
    const uint32_t elapsed = idle - (timeout - dimLead);
    const uint32_t startDuty = brightness;
    const uint32_t endDuty = (uint32_t)(brightness * 0.12f);
    uint32_t duty = startDuty;
    if (dimLead > 0 && elapsed < dimLead) {
      duty = startDuty - (startDuty - endDuty) * elapsed / dimLead;
    } else {
      duty = endDuty;
    }
    if (duty < 4) duty = 4;
    ledcWrite(LEDC_CHANNEL_5, duty);
    return;
  }

  if (brightness < 255) {
    backlightEnsurePwm();
    ledcWrite(LEDC_CHANNEL_5, brightness);
  } else if (sLedcActive) {
    ledc_stop(LEDC_SPEED_MODE, LEDC_CHANNEL_5, 255);
    sLedcActive = false;
  }
}

void displayRequestRefresh() {
  sRefreshPending = true;
  diagSetDisplayRefreshPending(true);
}

bool displayConsumeRefreshPending() {
  if (!sRefreshPending) return false;
  sRefreshPending = false;
  diagSetDisplayRefreshPending(false);
  return true;
}

void displayRefreshNow() {
  lv_disp_t *disp = lv_disp_get_default();
  if (disp) lv_refr_now(disp);
}

void displayPump() {
  for (int i = 0; i < 8; i++) {
    lv_timer_handler();
  }
  displayRefreshNow();
}

void displayInit() {
  pinMode(PIN_LCD_EN, OUTPUT);
  digitalWrite(PIN_LCD_EN, LOW);

  displayBacklightInit();
  delay(5);
  omoteI2cInit();
  tft.init();
  tft.initDMA();
  tft.fillScreen(0x0000);
  tft.setSwapBytes(true);
  for (int b = 0; b <= 180; b += 6) {
    displaySetBacklight((uint8_t)b);
    delay(8);
  }

  lv_init();
  size_t buf_pixels = SCR_WIDTH * SCR_HEIGHT / 12;
  buf1 = (lv_color_t *)malloc(sizeof(lv_color_t) * buf_pixels);
  buf2 = nullptr;
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCR_WIDTH;
  disp_drv.ver_res = SCR_HEIGHT;
  disp_drv.flush_cb = lvglFlush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lvglTouchRead;
  lv_indev_drv_register(&indev_drv);
}
