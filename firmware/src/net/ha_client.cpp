#include "net/ha_client.h"
#include "net/net_worker.h"

static bool httpRequest(const HaSettings &s, const String &method, const String &path,
                        const String &body, int &httpCode, String &response) {
  return netWorkerHttpRequest(s, method, path, body, httpCode, response);
}

bool haTestConnection(const HaSettings &s, String &errorOut) {
  int code = 0;
  String resp;
  if (!httpRequest(s, "GET", "/api/", "", code, resp)) {
    errorOut = "HTTP request failed";
    return false;
  }
  if (code == 401) {
    errorOut = "Unauthorized - check token";
    return false;
  }
  if (code < 200 || code >= 300) {
    errorOut = "HTTP " + String(code);
    return false;
  }
  errorOut = "";
  return true;
}

void haResolveHaCall(String &domain, String &service, const String &entityId) {
  String dom = domain;
  if (entityId.length()) {
    int dot = entityId.indexOf('.');
    const String entityDom = dot > 0 ? entityId.substring(0, dot) : "";
    if (dom.length() == 0 || dom == "homeassistant") dom = entityDom;
  }
  if (service.length() == 0) service = "toggle";
  if (service != "toggle") {
    if (domain.length() == 0 && dom.length()) domain = dom;
    return;
  }
  if (dom == "scene") {
    domain = "scene";
    service = "turn_on";
    return;
  }
  if (dom == "script") {
    domain = "script";
    service = "turn_on";
    return;
  }
  if (dom == "button") {
    domain = "button";
    service = "press";
    return;
  }
  domain = "homeassistant";
  service = "toggle";
}

bool haCallService(const HaSettings &s, const String &domain, const String &service,
                   const String &entityId, const String &dataJson) {
  String path = "/api/services/" + domain + "/" + service;
  String body = "{}";
  if (entityId.length() || dataJson.length()) {
    static JsonDocument doc;
    doc.clear();
    if (entityId.length()) {
      if (entityId.indexOf(',') >= 0) {
        JsonArray arr = doc["entity_id"].to<JsonArray>();
        int start = 0;
        while (start < (int)entityId.length()) {
          int comma = entityId.indexOf(',', start);
          String part = comma < 0 ? entityId.substring(start) : entityId.substring(start, comma);
          part.trim();
          if (part.length()) arr.add(part);
          if (comma < 0) break;
          start = comma + 1;
        }
      } else {
        doc["entity_id"] = entityId;
      }
    }
    if (dataJson.length()) {
      static JsonDocument extra;
      extra.clear();
      if (!deserializeJson(extra, dataJson)) {
        for (JsonPair kv : extra.as<JsonObject>()) {
          doc[kv.key().c_str()] = kv.value();
        }
      }
    }
    serializeJson(doc, body);
  }
  int code = 0;
  String resp;
  if (!httpRequest(s, "POST", path, body, code, resp)) {
    Serial.println("HA: HTTP request failed");
    return false;
  }
  if (code < 200 || code >= 300) {
    Serial.printf("HA: HTTP %d body: %s\n", code, resp.substring(0, 160).c_str());
    return false;
  }
  return true;
}

bool haStateIsOn(const String &state) {
  String s = state;
  s.toLowerCase();
  if (s == "on" || s == "open" || s == "opening" || s == "playing" || s == "home" ||
      s == "heat" || s == "cool" || s == "auto" || s == "unlocked" || s == "active") {
    return true;
  }
  return false;
}

bool haFetchEntityRaw(const HaSettings &s, const String &entityId, String &jsonOut,
                      String &errorOut) {
  if (!s.configured || entityId.length() == 0) {
    errorOut = "HA not configured";
    return false;
  }
  int code = 0;
  String path = "/api/states/" + entityId;
  if (!httpRequest(s, "GET", path, "", code, jsonOut)) {
    errorOut = "Request failed";
    return false;
  }
  if (code < 200 || code >= 300) {
    errorOut = "HTTP " + String(code);
    return false;
  }
  return true;
}

bool haFetchEntityState(const HaSettings &s, const String &entityId, String &stateOut,
                        String &errorOut) {
  if (!s.configured || entityId.length() == 0) {
    errorOut = "HA not configured";
    return false;
  }
  int code = 0;
  String resp;
  String path = "/api/states/" + entityId;
  if (!httpRequest(s, "GET", path, "", code, resp)) {
    errorOut = "Request failed";
    return false;
  }
  if (code < 200 || code >= 300) {
    errorOut = "HTTP " + String(code);
    return false;
  }
  static JsonDocument doc;
  doc.clear();
  if (deserializeJson(doc, resp)) {
    errorOut = "Invalid JSON";
    return false;
  }
  stateOut = doc["state"].as<String>();
  return true;
}

bool haFetchStates(const HaSettings &s, String &jsonOut, String &errorOut) {
  int code = 0;
  if (!httpRequest(s, "GET", "/api/states", "", code, jsonOut)) {
    errorOut = "Request failed";
    return false;
  }
  if (code < 200 || code >= 300) {
    errorOut = "HTTP " + String(code);
    return false;
  }
  return true;
}

bool haFetchEntitiesFiltered(const HaSettings &s, const String &domain, const String &search,
                             String &jsonOut, String &errorOut) {
  if (!s.configured) {
    errorOut = "HA not configured";
    return false;
  }
  String dom = domain;
  dom.trim();
  String q = search;
  q.trim();
  q.toLowerCase();

  String tpl = "{% for s in states %}{% if s.entity_id.split('.')[0] == '";
  tpl += dom.length() ? dom : "light";
  tpl += "' %}{{ s.entity_id }}|{{ s.state }}|{{ s.attributes.friendly_name | default('') }}|{{ s.attributes.icon | default('') }}\n{% endif %}{% endfor %}";

  static JsonDocument req;
  req.clear();
  req["template"] = tpl;
  String body;
  serializeJson(req, body);

  int code = 0;
  String resp;
  if (!httpRequest(s, "POST", "/api/template", body, code, resp)) {
    errorOut = "Template request failed";
    return false;
  }
  if (code < 200 || code >= 300) {
    errorOut = "HTTP " + String(code) + ": " + resp.substring(0, 120);
    return false;
  }

  static JsonDocument out;
  out.clear();
  JsonArray arr = out["entities"].to<JsonArray>();
  int start = 0;
  int count = 0;
  while (start < (int)resp.length() && count < 100) {
    int nl = resp.indexOf('\n', start);
    if (nl < 0) nl = resp.length();
    String line = resp.substring(start, nl);
    start = nl + 1;
    line.trim();
    if (line.length() < 3) continue;
    int p1 = line.indexOf('|');
    if (p1 < 0) continue;
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = p2 > 0 ? line.indexOf('|', p2 + 1) : -1;
    String eid = line.substring(0, p1);
    String state = p2 > 0 ? line.substring(p1 + 1, p2) : line.substring(p1 + 1);
    String fname = (p2 > 0 && p3 > 0) ? line.substring(p2 + 1, p3) : (p2 > 0 ? line.substring(p2 + 1) : "");
    String icon = p3 > 0 ? line.substring(p3 + 1) : "";
    eid.trim();
    if (eid.indexOf('.') < 0) continue;
    if (q.length()) {
      String low = eid;
      low.toLowerCase();
      String fl = fname;
      fl.toLowerCase();
      if (low.indexOf(q) < 0 && fl.indexOf(q) < 0) continue;
    }
    JsonObject e = arr.add<JsonObject>();
    e["entity_id"] = eid;
    e["state"] = state;
    e["friendly_name"] = fname;
    if (icon.length()) e["icon"] = icon;
    e["domain"] = eid.substring(0, eid.indexOf('.'));
    count++;
  }
  serializeJson(out, jsonOut);
  errorOut = "";
  return true;
}

bool haWsProxy(const HaSettings &s, const String &clientMessage, String &responseOut,
               String &errorOut) {
  static JsonDocument req;
  req.clear();
  if (deserializeJson(req, clientMessage)) {
    errorOut = "Invalid JSON";
    return false;
  }
  const char *type = req["type"] | "";
  if (strcmp(type, "get_states") == 0) {
    return haFetchStates(s, responseOut, errorOut);
  }
  if (strcmp(type, "get_entities") == 0) {
    String domain = req["domain"] | "light";
    String search = req["search"] | "";
    return haFetchEntitiesFiltered(s, domain, search, responseOut, errorOut);
  }
  if (strcmp(type, "call_service") == 0) {
    String domain = req["domain"] | "";
    String service = req["service"] | "";
    String entity = req["entity_id"] | "";
    String data;
    if (!req["data"].isNull()) serializeJson(req["data"], data);
    bool ok = haCallService(s, domain, service, entity, data);
    responseOut = ok ? "{\"ok\":true}" : "{\"ok\":false}";
    return ok;
  }
  errorOut = "Unsupported ws message type";
  return false;
}
