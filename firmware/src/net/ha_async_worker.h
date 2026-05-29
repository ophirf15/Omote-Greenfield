#pragma once

#include "config/config_store.h"
#include <Arduino.h>

enum class HaAsyncKind : uint8_t { None, CallService, FetchState, FetchClimateRaw };

/** Background HA HTTP on core 0 so the UI loop (core 1) stays responsive. */
void haAsyncInit();
bool haAsyncBusy();

bool haAsyncStartCallService(const HaSettings &s, const String &domain, const String &service,
                             const String &entityId, const String &dataJson);
bool haAsyncStartFetchState(const HaSettings &s, const String &entityId);
bool haAsyncStartFetchClimateRaw(const HaSettings &s, const String &entityId);

/** Returns true when a job finished; clears the slot. */
bool haAsyncPoll(HaAsyncKind &kindOut, bool &okOut, String &entityIdOut, String &stateOrErrorOut);
