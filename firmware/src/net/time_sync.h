#pragma once

#include <Arduino.h>

/**
 * Lightweight wrapper around the ESP-IDF SNTP client. Stores the user's
 * preferred timezone (POSIX TZ string, e.g. "PST8PDT,M3.2.0,M11.1.0") and
 * NTP server in DeviceSettings; the SNTP daemon then re-syncs in the
 * background at the IDF's default cadence (one hour).
 */

/** Configure SNTP + apply timezone. Safe to call multiple times. */
void timeStartSync(const String &posixTz, const String &ntpServer);

/** Update only the timezone — for use when the user changes it in WebUI. */
void timeSetTimezone(const String &posixTz);

/** Force a fresh SNTP query without changing timezone or server. */
void timeForceResync();

/** True once at least one SNTP packet has landed. */
bool timeIsSynced();

/** "HH:MM" (24h) — returns false if not yet synced. */
bool timeGetLocalHm(char *buf, size_t cap);

/** "YYYY-MM-DDTHH:MM:SS" local time — for status / API responses. */
bool timeGetLocalIso(char *buf, size_t cap);

/** UNIX epoch (UTC seconds) — 0 if not yet synced. */
uint32_t timeEpochSeconds();
