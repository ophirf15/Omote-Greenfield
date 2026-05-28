#pragma once

#include <Arduino.h>

void bleInit();
void bleShutdown();
void bleRequestInit();
bool bleIsInitialized();
bool bleIsConnected();
void bleStartAdvertising();
void bleStopAdvertising();
/**
 * Force open (undirected, connectable, discoverable) advertising for a window
 * after a wake event. Directed advertising — what BleKeyboard uses by default
 * once bonded — is only visible to a peer that is actively scanning at that
 * exact moment, and lasts ~1.28s per spec. After deep-sleep wake or
 * screen-off→on transition, the TV may have closed the link and isn't
 * scanning; this opens us up for ~`windowMs` so it can re-discover and
 * reconnect, then the supervisor falls back to bonded/directed adv.
 */
void bleOnWake(uint32_t windowMs = 60000);
void bleStartPairingMode();
void bleStopPairingMode();
bool bleIsPairingMode();
bool bleIsAdvertising();
void bleDisconnectClients();
void bleForgetBonds();
void bleSendKey(const String &keyName);
void bleSendMediaKey(const String &keyName);
/** Type literal text via BLE HID keyboard (a-z, 0-9, symbols). */
void bleSendText(const String &text);
/**
 * Debug helper — emit a raw 16-bit HID Consumer-Page usage (e.g. 0x02A2),
 * a 16-button report (1..16), or a "raw:0xKK" keyboard-page scancode.
 * Returns false if BLE is not connected. Useful for empirically discovering
 * which HID codes a particular TV's keylayout responds to.
 */
bool bleSendRawConsumerUsage(uint16_t usage);
bool bleSendRawButton(uint8_t button1to16);
void bleTaskLoop();
String bleStatusJson();

/**
 * Select which BLE HID "personality" the Omote presents. The key must match
 * one of the `key` fields in the table in ble_hal.cpp ("generic",
 * "onn-full-keyboard", "google-reference-rcu", …). Returns true if the key
 * was recognised. Call this BEFORE bleInit(). To change at runtime, callers
 * should also re-bond on both ends.
 */
bool bleSetProfile(const String &profileKey);
/** Active profile key (one of the identity keys); never empty. */
String bleCurrentProfile();
/** JSON `{ current, identities:[{key,name,vid,pid,description,recommended}] }`. */
String bleIdentityListJson();
