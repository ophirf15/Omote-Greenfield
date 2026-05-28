#include "net/debug_log.h"

#define LOG_CAP 8192
static char sBuf[LOG_CAP];
static size_t sLen = 0;

void debugLogInit() { sLen = 0; sBuf[0] = 0; }

void debugLogAppend(const char *line) {
  if (!line) return;
  size_t n = strlen(line);
  if (n >= LOG_CAP) return;
  if (sLen + n + 2 > LOG_CAP) {
    size_t drop = (sLen + n + 2) - LOG_CAP + 256;
    memmove(sBuf, sBuf + drop, sLen - drop);
    sLen -= drop;
    sBuf[sLen] = 0;
  }
  memcpy(sBuf + sLen, line, n);
  sLen += n;
  sBuf[sLen++] = '\n';
  sBuf[sLen] = 0;
}

String debugLogTail(size_t maxBytes) {
  if (sLen <= maxBytes) return String(sBuf);
  return String(sBuf + sLen - maxBytes);
}
