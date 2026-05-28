#pragma once

#include "config/config_store.h"
#include <functional>

typedef std::function<void()> ConfigChangedCallback;

void httpServerBegin(HaSettings &settings, OmoteConfig &config, DeviceSettings &devSettings,
                     ConfigChangedCallback onConfigChanged);
void httpServerLoop();
void httpServerPublishEvent(const char *buttonId, const char *pageId, const char *actionType);
void httpServerPublishKey(char key, bool pressed);
String httpServerLastEventJson();
String httpServerLastKeyJson();
