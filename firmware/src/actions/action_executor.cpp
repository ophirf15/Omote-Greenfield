#include "actions/action_executor.h"
#include "config/config_schema.h"
#include "config/ir_library.h"
#include "hal/ble_hal.h"
#include "hal/display_hal.h"
#include "hal/ir_hal.h"
#include "net/ha_client.h"

ActionExecutor gActions;

void ActionExecutor::begin(HaSettings *settings, OmoteConfig *config) {
  _settings = settings;
  _config = config;
  if (config && config->activePageId.length()) {
    _activePage = config->activePageId;
  }
}

void ActionExecutor::setActivePage(const String &pageId) {
  _activePage = pageId;
  if (_config) _config->activePageId = pageId;
}

bool ActionExecutor::executeHaService(const ActionDef &a) {
  if (!_settings || !_settings->configured) {
    Serial.println("HA: not configured");
    return false;
  }
  if (a.entityId.length() == 0) {
    Serial.println("HA: missing entity_id");
    return false;
  }
  String domain = a.domain;
  String service = a.service.length() ? a.service : "toggle";
  haResolveHaCall(domain, service, a.entityId);
  Serial.printf("HA: %s.%s -> %s\n", domain.c_str(), service.c_str(), a.entityId.c_str());
  return haCallService(*_settings, domain, service, a.entityId, a.dataJson);
}

bool ActionExecutor::executeMacro(const ActionDef &a) {
  if (a.macroJson.length() == 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, a.macroJson)) return false;
  JsonArray steps = doc["steps"].as<JsonArray>();
  if (steps.isNull()) steps = doc.as<JsonArray>();
  for (JsonObject step : steps) {
    ActionDef sub;
    if (actionFromJson(step, sub)) execute(sub);
    delay(80);
  }
  return true;
}

bool ActionExecutor::execute(const ActionDef &action) {
  bool ok = false;
  if (action.type == ACTION_HA_SERVICE) ok = executeHaService(action);
  else if (action.type == ACTION_IR) {
    String proto = action.protocol.length() ? action.protocol : "NEC";
    uint64_t code = action.irCode;
    uint16_t bits = action.irBits ? action.irBits : 32;
    bool resolved = false;
    if (action.irId.length()) {
      resolved = irLibResolve(action.irId, proto, code, bits);
    }
    Serial.printf("IR send: id=%s resolved=%d proto=%s code=0x%llX bits=%u\n", action.irId.c_str(),
                  resolved ? 1 : 0, proto.c_str(), (unsigned long long)code, bits);
    irSend(proto, code, bits);
    ok = true;
  } else if (action.type == ACTION_BLE_KEY) {
    bleSendKey(action.bleKey);
    ok = true;
  } else if (action.type == ACTION_BLE_MEDIA) {
    bleSendMediaKey(action.bleKey);
    ok = true;
  } else if (action.type == ACTION_NAVIGATE_PAGE) {
    setActivePage(action.pageId);
    ok = true;
  } else if (action.type == ACTION_MACRO) ok = executeMacro(action);
  return ok;
}
