#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#define IRLIB_PATH "/irlib.json"

struct IrLibEntry {
  String id;
  String name;
  String protocol;
  uint64_t code = 0;
  uint16_t bits = 32;
};

bool irLibListJson(String &jsonOut, String &errorOut);
bool irLibAdd(const String &name, const String &protocol, uint64_t code, uint16_t bits,
              String &idOut, String &errorOut);
bool irLibDelete(const String &id, String &errorOut);
bool irLibReplaceAll(const JsonArray &entries, String &errorOut);
bool irLibResolve(const String &id, String &protocolOut, uint64_t &codeOut, uint16_t &bitsOut);
