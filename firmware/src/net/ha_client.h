#pragma once

#include "config/config_store.h"

bool haTestConnection(const HaSettings &s, String &errorOut);
bool haCallService(const HaSettings &s, const String &domain, const String &service,
                   const String &entityId, const String &dataJson = "");
/** Map toggle + entity domain to a valid HA call (e.g. scene + toggle -> scene.turn_on). */
void haResolveHaCall(String &domain, String &service, const String &entityId);
bool haFetchStates(const HaSettings &s, String &jsonOut, String &errorOut);
bool haFetchEntityState(const HaSettings &s, const String &entityId, String &stateOut,
                        String &errorOut);
/** Full /api/states/<entity> JSON object as string. */
bool haFetchEntityRaw(const HaSettings &s, const String &entityId, String &jsonOut,
                      String &errorOut);
/** True when HA state should show a toggle as ON (light/switch on, cover open, etc.). */
bool haStateIsOn(const String &state);
bool haFetchEntitiesFiltered(const HaSettings &s, const String &domain, const String &search,
                             String &jsonOut, String &errorOut);
bool haWsProxy(const HaSettings &s, const String &clientMessage, String &responseOut,
               String &errorOut);
