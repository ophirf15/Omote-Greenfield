#pragma once

#include "config/config_store.h"

class ActionExecutor {
 public:
  void begin(HaSettings *settings, OmoteConfig *config);
  bool execute(const ActionDef &action);
  void setActivePage(const String &pageId);
  String activePageId() const { return _activePage; }

 private:
  HaSettings *_settings = nullptr;
  OmoteConfig *_config = nullptr;
  String _activePage;
  bool executeHaService(const ActionDef &a);
  bool executeMacro(const ActionDef &a);
};

extern ActionExecutor gActions;
