#include "net/time_sync.h"

#include <stdio.h>
#include <WiFi.h>
#include <WiFiUdp.h>

/**
 * Hand-rolled minimal NTP client + integer-only local-time arithmetic.
 *
 * We avoid newlib's setenv/tzset/localtime_r/strftime chain — pulling those
 * symbols costs ~940 bytes of IRAM on this build and overflows iram0_0_seg.
 */

static String sActiveServer;
static String sActiveTz;
static bool sSynced = false;
static uint32_t sLastSyncMs = 0;
static uint32_t sSyncedUtcAtBoot = 0;
static uint32_t sSyncedMillisAtBoot = 0;

static int32_t sStdOffsetSec = 0;
static int32_t sDstOffsetSec = 0;
static bool sHasDst = false;

static constexpr uint32_t kNtpUnixOffset = 2208988800UL;
static constexpr uint32_t kResyncIntervalMs = 60UL * 60UL * 1000UL;

static int32_t parseOffset(const char *&p) {
  int sign = 1;
  if (*p == '+') p++;
  else if (*p == '-') {
    sign = -1;
    p++;
  }
  int hours = 0;
  while (*p >= '0' && *p <= '9') {
    hours = hours * 10 + (*p - '0');
    p++;
  }
  int minutes = 0;
  if (*p == ':') {
    p++;
    while (*p >= '0' && *p <= '9') {
      minutes = minutes * 10 + (*p - '0');
      p++;
    }
  }
  return -sign * (hours * 3600 + minutes * 60);
}

static void parseTz(const String &tz) {
  sStdOffsetSec = 0;
  sDstOffsetSec = 0;
  sHasDst = false;
  if (!tz.length()) return;
  const char *p = tz.c_str();
  if (*p == '<') {
    while (*p && *p != '>') p++;
    if (*p) p++;
  } else {
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++;
  }
  sStdOffsetSec = parseOffset(p);
  if (!*p || *p == ',') return;
  if (*p == '<') {
    while (*p && *p != '>') p++;
    if (*p) p++;
  } else {
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++;
  }
  sHasDst = true;
  if (*p && *p != ',') {
    sDstOffsetSec = parseOffset(p);
  } else {
    sDstOffsetSec = sStdOffsetSec + 3600;
  }
}

struct CivilDate {
  int year;
  unsigned month;
  unsigned day;
};

static CivilDate epochToCivil(uint32_t epochSec, uint32_t &todSec) {
  uint32_t days = epochSec / 86400;
  todSec = epochSec % 86400;
  int32_t z = (int32_t)days + 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t y = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  unsigned d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
  unsigned m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
  if (m <= 2) y += 1;
  return CivilDate{(int)y, m, d};
}

static unsigned nthDow(int year, unsigned month, unsigned n, unsigned wantedDow) {
  int q = 1, m = (int)month, y = year;
  if (m < 3) {
    m += 12;
    y -= 1;
  }
  int K = y % 100;
  int J = y / 100;
  int h = (q + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  unsigned dowFirst = (unsigned)((h + 6) % 7);
  unsigned offset = (wantedDow + 7 - dowFirst) % 7;
  return 1 + offset + (n - 1) * 7;
}

static bool inUsDst(uint32_t epochSec) {
  uint32_t tod;
  CivilDate c = epochToCivil(epochSec, tod);
  if (c.month < 3 || c.month > 11) return false;
  if (c.month > 3 && c.month < 11) return true;
  if (c.month == 3) {
    unsigned start = nthDow(c.year, 3, 2, 0);
    return c.day > start || (c.day == start && tod >= 7200);
  }
  unsigned end = nthDow(c.year, 11, 1, 0);
  return c.day < end || (c.day == end && tod < 7200);
}

static int32_t currentOffsetSec(uint32_t utcSec) {
  if (!sHasDst) return sStdOffsetSec;
  return inUsDst(utcSec + sStdOffsetSec) ? sDstOffsetSec : sStdOffsetSec;
}

static uint32_t currentUtc() {
  if (!sSynced) return 0;
  return sSyncedUtcAtBoot + (millis() - sSyncedMillisAtBoot) / 1000;
}

static bool ntpQuery(const char *host, uint32_t &outUtc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiUDP udp;
  if (!udp.begin(0)) return false;

  uint8_t pkt[48] = {0};
  pkt[0] = 0x1B;
  if (!udp.beginPacket(host, 123)) {
    udp.stop();
    return false;
  }
  udp.write(pkt, sizeof(pkt));
  if (!udp.endPacket()) {
    udp.stop();
    return false;
  }

  const uint32_t deadline = millis() + 1500;
  while ((int32_t)(millis() - deadline) < 0) {
    if (udp.parsePacket() >= (int)sizeof(pkt)) {
      udp.read(pkt, sizeof(pkt));
      udp.stop();
      const uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                            ((uint32_t)pkt[42] << 8) | (uint32_t)pkt[43];
      if (secs < kNtpUnixOffset) return false;
      outUtc = secs - kNtpUnixOffset;
      return true;
    }
    delay(20);
  }
  udp.stop();
  return false;
}

static bool ntpSyncOnce() {
  const char *servers[3] = {
      sActiveServer.length() ? sActiveServer.c_str() : "pool.ntp.org",
      "time.google.com",
      "time.cloudflare.com",
  };
  for (const char *host : servers) {
    uint32_t utc = 0;
    if (ntpQuery(host, utc)) {
      sSyncedUtcAtBoot = utc;
      sSyncedMillisAtBoot = millis();
      sSynced = true;
      sLastSyncMs = millis();
      Serial.printf("NTP: synced via %s (epoch=%lu)\n", host, (unsigned long)utc);
      return true;
    }
  }
  Serial.println("NTP: all servers failed");
  return false;
}

void timeStartSync(const String &posixTz, const String &ntpServer) {
  sActiveTz = posixTz.length() ? posixTz : String("UTC0");
  parseTz(sActiveTz);
  sActiveServer = ntpServer.length() ? ntpServer : String("pool.ntp.org");
  ntpSyncOnce();
}

void timeSetTimezone(const String &posixTz) {
  sActiveTz = posixTz.length() ? posixTz : String("UTC0");
  parseTz(sActiveTz);
}

void timeForceResync() { ntpSyncOnce(); }

void timeSyncLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if (sLastSyncMs && now - sLastSyncMs < kResyncIntervalMs) return;
  ntpSyncOnce();
}

bool timeIsSynced() { return sSynced; }

bool timeGetLocalHm(char *buf, size_t cap) {
  if (!sSynced || !buf || cap < 6) return false;
  uint32_t utc = currentUtc();
  uint32_t local = utc + currentOffsetSec(utc);
  uint32_t tod = local % 86400;
  unsigned h = tod / 3600;
  unsigned m = (tod % 3600) / 60;
  snprintf(buf, cap, "%02u:%02u", h, m);
  return true;
}

bool timeGetLocalIso(char *buf, size_t cap) {
  if (!sSynced || !buf || cap < 20) return false;
  uint32_t utc = currentUtc();
  uint32_t local = utc + currentOffsetSec(utc);
  uint32_t tod;
  CivilDate c = epochToCivil(local, tod);
  unsigned hh = tod / 3600;
  unsigned mm = (tod % 3600) / 60;
  unsigned ss = tod % 60;
  snprintf(buf, cap, "%04d-%02u-%02uT%02u:%02u:%02u", c.year, c.month, c.day, hh, mm, ss);
  return true;
}

uint32_t timeEpochSeconds() { return currentUtc(); }
