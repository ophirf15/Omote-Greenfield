#pragma once

#include <LovyanGFX.hpp>
#include <lvgl.h>

class OmoteDisplay : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ILI9341 _panel;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;

 public:
  OmoteDisplay();
};

extern OmoteDisplay tft;
void displayInit();
void displaySetBacklight(uint8_t brightness);
void displayBacklightInit();
void displayResetBacklightFade();
/** Apply dim PWM immediately when pre-sleep window starts. */
void displayEnterPreSleepDim();
void displayPowerOff();
void displayPowerOn();
bool displayIsOff();
/** Fade-in after wake; ramp down during sleepInPreSleepDimPhase(). */
void displayUpdateBacklight();
/** Run LVGL handler and push a full redraw to the panel (call after blocking work). */
void displayPump();
/** Schedule a single lv_refr_now on the next pageEngineLoop tick (safe from LVGL events). */
void displayRequestRefresh();
void displayRefreshNow();
/** Returns true once and clears the pending refresh flag. */
bool displayConsumeRefreshPending();
