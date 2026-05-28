#pragma once

#include <Arduino.h>
#include <WiFi.h>

struct WifiCreds {
  String ssid;
  String password;
};

enum WifiUiState {
  WIFI_UI_SETUP_AP,
  WIFI_UI_CONNECTING,
  WIFI_UI_CONNECTED,
  WIFI_UI_FAILED,
};

bool wifiLoadCreds(WifiCreds &c);
bool wifiSaveCreds(const WifiCreds &c);
void wifiStartPortal(const char *apName = "Omote-Setup");
/** Init WiFi STA driver before BLE so both can coexist (do not call WiFi.mode after bleInit). */
void wifiPrepareCoexistence();
bool wifiConnectStored(uint32_t timeoutMs = 20000);
void wifiBeginConnect();
bool wifiPollConnect(uint32_t timeoutMs = 25000);
WifiUiState wifiUiState();
const char *wifiUiStateText();
bool wifiIsConnected();
String wifiStatusJson();
int wifiRssi();
void wifiPortalLoop();
