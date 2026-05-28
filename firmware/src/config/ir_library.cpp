#include "config/ir_library.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

static bool readIrLibDoc(JsonDocument &doc) {
  doc.clear();
  if (!LittleFS.exists(IRLIB_PATH)) {
    doc.to<JsonArray>();
    return true;
  }
  File f = LittleFS.open(IRLIB_PATH, "r");
  if (!f) return false;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

static bool writeIrLibDoc(JsonDocument &doc) {
  File f = LittleFS.open(IRLIB_PATH, "w");
  if (!f) return false;
  if (serializeJson(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

static String makeIrId() {
  return String("ir_") + String(millis(), HEX) + String(random(0xFFFF), HEX);
}

bool irLibListJson(String &jsonOut, String &errorOut) {
  static JsonDocument doc;
  if (!readIrLibDoc(doc)) {
    errorOut = "read failed";
    return false;
  }
  serializeJson(doc, jsonOut);
  errorOut = "";
  return true;
}

bool irLibAdd(const String &name, const String &protocol, uint64_t code, uint16_t bits,
              String &idOut, String &errorOut) {
  static JsonDocument doc;
  if (!readIrLibDoc(doc)) {
    errorOut = "read failed";
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) arr = doc.to<JsonArray>();
  if (arr.size() >= 128) {
    errorOut = "library full";
    return false;
  }
  idOut = makeIrId();
  JsonObject e = arr.add<JsonObject>();
  e["id"] = idOut;
  e["name"] = name.length() ? name : idOut;
  e["protocol"] = protocol.length() ? protocol : "NEC";
  e["code"] = String("0x") + String(code, HEX);
  e["bits"] = bits ? bits : 32;
  if (!writeIrLibDoc(doc)) {
    errorOut = "write failed";
    return false;
  }
  errorOut = "";
  return true;
}

bool irLibReplaceAll(const JsonArray &entries, String &errorOut) {
  static JsonDocument doc;
  doc.clear();
  JsonArray arr = doc.to<JsonArray>();
  for (JsonObject o : entries) {
    if (arr.size() >= 128) break;
    JsonObject e = arr.add<JsonObject>();
    String id = o["id"] | "";
    if (id.length() == 0) id = makeIrId();
    e["id"] = id;
    e["name"] = o["name"] | id;
    e["protocol"] = o["protocol"] | "NEC";
    if (o["code"].is<const char *>()) e["code"] = o["code"].as<const char *>();
    else e["code"] = String("0x") + String(o["code"].as<uint64_t>(), HEX);
    e["bits"] = o["bits"] | 32;
  }
  if (!writeIrLibDoc(doc)) {
    errorOut = "write failed";
    return false;
  }
  errorOut = "";
  return true;
}

bool irLibDelete(const String &id, String &errorOut) {
  if (id.length() == 0) {
    errorOut = "missing id";
    return false;
  }
  static JsonDocument doc;
  if (!readIrLibDoc(doc)) {
    errorOut = "read failed";
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    errorOut = "not found";
    return false;
  }
  bool found = false;
  for (size_t i = 0; i < arr.size(); i++) {
    JsonObject o = arr[i].as<JsonObject>();
    if (String(o["id"] | "") == id) {
      arr.remove(i);
      found = true;
      break;
    }
  }
  if (!found) {
    errorOut = "not found";
    return false;
  }
  if (!writeIrLibDoc(doc)) {
    errorOut = "write failed";
    return false;
  }
  errorOut = "";
  return true;
}

bool irLibResolve(const String &id, String &protocolOut, uint64_t &codeOut, uint16_t &bitsOut) {
  if (id.length() == 0) return false;
  static JsonDocument doc;
  if (!readIrLibDoc(doc)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) return false;
  for (JsonObject o : arr) {
    if (String(o["id"] | "") != id) continue;
    protocolOut = o["protocol"] | "NEC";
    const char *codeStr = o["code"];
    if (codeStr) codeOut = strtoull(codeStr, nullptr, 0);
    else codeOut = o["code"].as<uint64_t>();
    bitsOut = o["bits"] | 32;
    return true;
  }
  return false;
}
