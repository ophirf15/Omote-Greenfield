#pragma once

#include <Arduino.h>

/** Mark the current main-loop phase (short literal string, no heap). */
void diagSetStage(const char *stage);

/** Call once per main-loop iteration when the loop is healthy. */
void diagLoopHeartbeat();

/** True while an LVGL input/event callback is running. */
void diagLvglEventEnter(const char *name);
void diagLvglEventLeave();

/** Home Assistant HTTP in flight (net_worker mutex held for HA). */
void diagHttpBegin(const char *method, const char *path);
void diagHttpEnd();

/** Display flush callback started / finished. */
void diagFlushBegin();
void diagFlushEnd();

void diagSetTogglePending(bool pending);
void diagSetDisplayRefreshPending(bool pending);

/** True while net_worker is inside an HA HTTP call. */
bool diagHttpBusy();

/** JSON snapshot for GET /api/device/diag — safe to call anytime. */
String diagSnapshotJson();

/** If UI loop hasn't heartbeated, emit one line to Serial + debug log. */
void diagMaybeReportStall();
