#pragma once

#include <Arduino.h>

void debugLogInit();
void debugLogAppend(const char *line);
String debugLogTail(size_t maxBytes = 4096);
