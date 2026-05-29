#pragma once

#include "config/config_store.h"
#include <vector>

void haWsInit();
void haWsStartTask();
/** Call when HTTP/mDNS services are up — delays WS connect until heap settles. */
void haWsOnWifiUp();
void haWsOnWifiDown();

void haWsSetSettings(const HaSettings *settings);

bool haWsIsConnected();
bool haWsIsConnecting();

/** Replace subscription set and request fresh state (WS + REST bootstrap). */
void haWsSubscribeEntities(const std::vector<String> &entityIds);

/** Queue call_service (WebSocket if connected, else REST on worker). */
bool haWsCallService(const String &domain, const String &service, const String &entityId,
                     const String &dataJson, String &errorOut);

/** Pump reconnect / queued work from main loop when task is not running WS loop. */
void haWsTick();
/** Drop WS + pending work when heap is critically low (WebUI needs RAM). */
void haWsReleasePressure();
