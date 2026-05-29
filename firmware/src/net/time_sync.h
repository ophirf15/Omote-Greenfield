#pragma once

#include <Arduino.h>

/**
 * Lightweight UDP NTP client with integer-only local-time math (no newlib TZ).
 * Stores the user's preferred timezone (POSIX TZ string) and NTP server in
 * DeviceSettings; re-syncs hourly while WiFi is up.
 */

/** Configure NTP + apply timezone. Safe to call multiple times. */
void timeStartSync(const String &posixTz, const String &ntpServer);

/** Update only the timezone — for use when the user changes it in WebUI. */
void timeSetTimezone(const String &posixTz);

/** Force a fresh NTP query without changing timezone or server. */
void timeForceResync();

/** Call from main loop — hourly re-sync while WiFi is connected. */
void timeSyncLoop();

/** True once at least one NTP packet has landed. */
bool timeIsSynced();

/** "HH:MM" (24h) — returns false if not yet synced. */
bool timeGetLocalHm(char *buf, size_t cap);

/** "YYYY-MM-DDTHH:MM:SS" local time — for status / API responses. */
bool timeGetLocalIso(char *buf, size_t cap);

/** UNIX epoch (UTC seconds) — 0 if not yet synced. */
uint32_t timeEpochSeconds();
