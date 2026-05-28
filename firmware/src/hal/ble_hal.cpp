#include "hal/ble_hal.h"

#include "hal/battery_hal.h"
#include <BleKeyboard.h>
#include <NimBLEDevice.h>

static BleKeyboard bleKeyboard("Omote Remote", "Omote OS", 100);
static bool sBleStarted = false;
static bool sPairingMode = false;
static uint32_t sLastAdvCheckMs = 0;
static uint32_t sLastBondCount = 0;
/* If non-zero, the supervisor uses undirected open adv until this deadline.
 * Set by bleOnWake() so a TV that closed the link during sleep can re-find us. */
static uint32_t sOpenAdvUntilMs = 0;

/**
 * BLE HID "personality" catalogue. The VID/PID determines which
 *   /system/usr/keylayout/Vendor_VVVV_Product_PPPP.kl
 * file Android loads for this device, which in turn determines which
 * KEYCODE_* values the TV emits for our HID reports.
 *
 * Android's EventHub falls back to Generic.kl when no Vendor file matches.
 * Generic.kl on stock Google TV / Onn is the richest layout — it covers
 * full QWERTY plus NOTIFICATION (204), PROFILE_SWITCH (HID 0x019C),
 * CAPTIONS (370), PROG_RED/GREEN/BLUE/YELLOW (398..401), BUTTON_3/4/6/7
 * (258/259/261/262), GUIDE (362), TV (377), ASSIST (583), CHANNEL_UP/DOWN
 * (402/403), POWER (116/152), HOME (172), BACK (158), and every standard
 * media key — so it's the recommended profile.
 *
 * The other profiles are kept as escape hatches in case a particular TV's
 * Generic.kl is stripped down (rare) or the user wants to mimic a specific
 * vendor remote.
 */
struct BleIdentity {
  const char *key;
  const char *name;
  uint16_t vid;
  uint16_t pid;
  const char *description;
  bool recommended;
};

static const BleIdentity kBleIdentities[] = {
    {"generic",
     "Generic (recommended)",
     /* VID/PID chosen to NOT collide with any Vendor_*.kl on stock Google TV,
      * so Android falls back to Generic.kl. "OM"/"OT" in ASCII (Omote). */
     0x4F4D,
     0x4F54,
     "Falls back to /system/usr/keylayout/Generic.kl — the widest mapping. "
     "Recovers NOTIFICATION, PROFILE_SWITCH, CAPTIONS, the four colour keys "
     "(red/green/blue/yellow), and the streaming-app shortcuts.",
     true},
    {"onn-full-keyboard",
     "Onn Full Keyboard + Remote (Vendor 0x0484 / 0x5738)",
     0x0484,
     0x5738,
     "Loads Vendor_0484_Product_5738.kl on Google TV. Full QWERTY plus the "
     "standard remote keys. Useful if Generic.kl is stripped down on a "
     "particular TV. Missing colour keys, NOTIFICATION, PROFILE_SWITCH.",
     false},
    {"google-reference-rcu",
     "Google Reference RCU (Vendor 0x0957 / 0x0001)",
     0x0957,
     0x0001,
     "Loads Vendor_0957_Product_0001.kl. Has colour keys and NOTIFICATION / "
     "PROFILE_SWITCH but no QWERTY letters (only digits via numpad).",
     false},
    {"apple-keyboard",
     "Apple Bluetooth Keyboard (0x05AC / 0x820A)",
     0x05AC,
     0x820A,
     "Original BleKeyboard defaults — iOS / macOS / iPadOS. Use if pairing "
     "the Omote with an Apple device.",
     false},
};

static const BleIdentity *findIdentity(const String &key) {
  for (const auto &id : kBleIdentities) {
    if (key.equals(id.key)) return &id;
  }
  return nullptr;
}

static String sBleProfileKey = "generic";

static const BleIdentity &activeIdentity() {
  const BleIdentity *id = findIdentity(sBleProfileKey);
  return id ? *id : kBleIdentities[0];
}

bool bleSetProfile(const String &profileKey) {
  const BleIdentity *id = findIdentity(profileKey);
  if (!id) {
    Serial.printf("BLE: unknown profile '%s' — keeping '%s'\n", profileKey.c_str(),
                  sBleProfileKey.c_str());
    return false;
  }
  if (sBleProfileKey == id->key) return true;
  sBleProfileKey = id->key;
  Serial.printf("BLE: profile → %s (VID 0x%04X / PID 0x%04X)\n", id->key, id->vid, id->pid);
  /* If BLE is already running, the host has cached the old descriptor; we
   * need to shut down and let the caller rebond before bringing it back. */
  if (sBleStarted) bleShutdown();
  return true;
}

String bleCurrentProfile() { return sBleProfileKey; }

String bleIdentityListJson() {
  String s = "{\"current\":\"";
  s += sBleProfileKey;
  s += "\",\"identities\":[";
  bool first = true;
  for (const auto &id : kBleIdentities) {
    if (!first) s += ',';
    first = false;
    s += "{\"key\":\"";
    s += id.key;
    s += "\",\"name\":\"";
    s += id.name;
    s += "\",\"vid\":";
    s += String(id.vid);
    s += ",\"pid\":";
    s += String(id.pid);
    s += ",\"recommended\":";
    s += id.recommended ? "true" : "false";
    s += ",\"description\":\"";
    /* Description is constant ASCII with no embedded quotes — safe to inline. */
    s += id.description;
    s += "\"}";
  }
  s += "]}";
  return s;
}

/* HID Consumer Page (0x0C) usages.
 *
 * Two things must agree for one of these to reach Android as a real KEYCODE_*:
 *
 *   1) The Linux HID kernel layer (drivers/hid/hid-input.c) translates the
 *      raw HID Consumer usage to a Linux keycode (e.g. 0x0E9 → KEY_VOLUMEUP).
 *      If the kernel has no entry, you get KEY_UNKNOWN (240) and it dies.
 *   2) Android's keylayout (.kl) translates that Linux keycode to a
 *      KEYCODE_*. If the .kl has no entry, the app sees KEYCODE_UNKNOWN.
 *
 *   *Exception*: `.kl` files can also carry `key usage 0x0cXXXX KEY_NAME`
 *   entries that bypass the kernel step entirely. Generic.kl uses this for
 *   PROFILE_SWITCH (0x019C), ALL_APPS (0x01A2), MEDIA_AUDIO_TRACK (0x0173).
 *
 * Every constant below is annotated with the kernel mapping + the
 * Generic.kl entry it relies on. Keys that can't reach Android via Generic.kl
 * are still defined but only emitted when the active profile is a richer .kl.
 */
static constexpr uint16_t HID_CC_POWER             = 0x0030;  /* → KEY_POWER (116) → POWER */
static constexpr uint16_t HID_CC_SLEEP             = 0x0032;  /* → KEY_SLEEP (142) */
static constexpr uint16_t HID_CC_MENU_PICK         = 0x0041;  /* → KEY_SELECT (353) → DPAD_CENTER */
static constexpr uint16_t HID_CC_CAPTIONS          = 0x0061;  /* → KEY_SUBTITLE (370) → CAPTIONS */
static constexpr uint16_t HID_CC_PROG_RED          = 0x0069;  /* → KEY_RED (398) → PROG_RED */
static constexpr uint16_t HID_CC_PROG_GREEN        = 0x006A;  /* → KEY_GREEN (399) → PROG_GREEN */
static constexpr uint16_t HID_CC_PROG_BLUE         = 0x006B;  /* → KEY_BLUE (401) → PROG_BLUE */
static constexpr uint16_t HID_CC_PROG_YELLOW       = 0x006C;  /* → KEY_YELLOW (400) → PROG_YELLOW */
static constexpr uint16_t HID_CC_TV                = 0x0089;  /* → KEY_TV (377) → TV */
static constexpr uint16_t HID_CC_GUIDE             = 0x008D;  /* → KEY_PROGRAM (362) → GUIDE */
static constexpr uint16_t HID_CC_CHANNEL_UP        = 0x009C;  /* → KEY_CHANNELUP (402) → CHANNEL_UP */
static constexpr uint16_t HID_CC_CHANNEL_DOWN      = 0x009D;  /* → KEY_CHANNELDOWN (403) → CHANNEL_DOWN */
static constexpr uint16_t HID_CC_RECORD            = 0x00B2;  /* → KEY_RECORD (167) → MEDIA_RECORD */
static constexpr uint16_t HID_CC_SKIP_FORWARD      = 0x00B3;  /* → KEY_FASTFORWARD (208) → MEDIA_FAST_FORWARD */
static constexpr uint16_t HID_CC_SKIP_BACKWARD     = 0x00B4;  /* → KEY_REWIND (168) → MEDIA_REWIND */
static constexpr uint16_t HID_CC_NEXT_TRACK        = 0x00B5;  /* → KEY_NEXTSONG (163) → MEDIA_NEXT */
static constexpr uint16_t HID_CC_PREV_TRACK        = 0x00B6;  /* → KEY_PREVIOUSSONG (165) → MEDIA_PREVIOUS */
static constexpr uint16_t HID_CC_STOP              = 0x00B7;  /* → KEY_STOPCD (166) → MEDIA_STOP */
static constexpr uint16_t HID_CC_PLAY_PAUSE        = 0x00CD;  /* → KEY_PLAYPAUSE (164) → MEDIA_PLAY_PAUSE */
static constexpr uint16_t HID_CC_MUTE              = 0x00E2;  /* → KEY_MUTE (113) → VOLUME_MUTE */
static constexpr uint16_t HID_CC_VOLUME_UP         = 0x00E9;  /* → KEY_VOLUMEUP (115) → VOLUME_UP */
static constexpr uint16_t HID_CC_VOLUME_DOWN       = 0x00EA;  /* → KEY_VOLUMEDOWN (114) → VOLUME_DOWN */
static constexpr uint16_t HID_CC_PROFILE_SWITCH    = 0x019C;  /* Generic.kl override → PROFILE_SWITCH */
static constexpr uint16_t HID_CC_ALL_APPS          = 0x01A2;  /* Generic.kl override → ALL_APPS */
static constexpr uint16_t HID_CC_ASSIST            = 0x01CB;  /* → KEY_ASSISTANT (583) → ASSIST */
static constexpr uint16_t HID_CC_AC_SEARCH         = 0x0221;  /* → KEY_SEARCH (217) → SEARCH */
static constexpr uint16_t HID_CC_AC_HOME           = 0x0223;  /* → KEY_HOMEPAGE (172) → HOME */
static constexpr uint16_t HID_CC_AC_BACK           = 0x0224;  /* → KEY_BACK (158) → BACK */
/* NOTIFICATION needs a code the kernel routes to KEY_ALL_APPLICATIONS (204).
 * 0x009F looks tempting from the HUT spec but the kernel has NO mapping for
 * it — sending 0x9F lands as KEY_UNKNOWN (240) and Generic.kl drops it.
 * 0x02A2 (App Launch Manager) is the correct code: kernel maps it to
 * KEY_ALL_APPLICATIONS (204), which Generic.kl maps to NOTIFICATION. */
static constexpr uint16_t HID_CC_NOTIFICATION      = 0x02A2;  /* → KEY_ALL_APPLICATIONS (204) → NOTIFICATION */
/* The following arrive at Generic.kl as KEY_* values it doesn't map, so they
 * silently turn into KEYCODE_UNKNOWN on stock Google TV. Kept here so they
 * still work on richer .kl files (e.g. when the user picks the
 * google-reference-rcu profile). */
static constexpr uint16_t HID_CC_INFO              = 0x01BD;  /* → KEY_INFO (358); Generic.kl: not mapped */
static constexpr uint16_t HID_CC_TV_TELETEXT       = 0x0185;  /* kernel: not mapped; .kl-dependent */

static void startBondedAdvertising() {
  if (!sBleStarted) return;
  const int bonds = NimBLEDevice::getNumBonds();
  if (bonds == 0) return;
  /* Directed advertising to the most recent bond is invisible to other phones,
   * so the TV can reconnect after sleep without the phone hijacking. */
  NimBLEAddress addr = NimBLEDevice::getBondedAddress(bonds - 1);
  bool randomAddr = addr.getType() != BLE_ADDR_PUBLIC;
  bleKeyboard.startAdvertisingDirected(addr.toString(), randomAddr);
  Serial.printf("BLE: directed advertising → %s (bond %d/%d)\n", addr.toString().c_str(),
                bonds, bonds);
}

void bleInit() {
  if (sBleStarted) return;
  batteryUpdate();
  bleKeyboard.setBatteryLevel((uint8_t)batteryPercent());
  const BleIdentity &id = activeIdentity();
  Serial.printf("BLE: identity '%s' → VID 0x%04X / PID 0x%04X\n", id.key, id.vid, id.pid);
  bleKeyboard.set_vendor_id(id.vid);
  bleKeyboard.set_product_id(id.pid);
  bleKeyboard.begin();
  sBleStarted = true;
  const int bonds = NimBLEDevice::getNumBonds();
  sLastBondCount = bonds;
  if (bonds == 0) {
    sPairingMode = true;
    bleKeyboard.startAdvertisingForAll();
    Serial.println("BLE: no bonds — advertising for pairing");
  } else {
    sPairingMode = false;
    startBondedAdvertising();
  }
  Serial.println("BLE HID started (NimBLE)");
}

void bleRequestInit() { bleInit(); }

void bleShutdown() {
  if (!sBleStarted) return;
  bleKeyboard.end();
  sBleStarted = false;
  sPairingMode = false;
}

bool bleIsInitialized() { return sBleStarted; }

void bleStartAdvertising() {
  if (!sBleStarted) bleInit();
  if (NimBLEDevice::getNumBonds() > 0)
    startBondedAdvertising();
  else
    bleKeyboard.startAdvertisingForAll();
}

void bleOnWake(uint32_t windowMs) {
  if (!sBleStarted) bleInit();
  if (bleKeyboard.isConnected()) return;
  if (windowMs == 0) windowMs = 60000;
  /* Track an absolute deadline; bleTaskLoop() keeps undirected adv up until
   * we cross it, then resumes the normal directed/bonded strategy. */
  sOpenAdvUntilMs = millis() + windowMs;
  if (sOpenAdvUntilMs == 0) sOpenAdvUntilMs = 1;  /* avoid the "disabled" sentinel */
  bleKeyboard.stopAdvertising();
  bleKeyboard.startAdvertisingForAll();
  Serial.printf("BLE: wake — open advertising for %lus\n",
                (unsigned long)(windowMs / 1000));
}

void bleStopAdvertising() {
  if (!sBleStarted) return;
  bleKeyboard.stopAdvertising();
  sPairingMode = false;
}

void bleStartPairingMode() {
  if (!sBleStarted) bleInit();
  sPairingMode = true;
  /* Pairing mode = open advertising so a new device can discover Omote. */
  bleKeyboard.stopAdvertising();
  bleKeyboard.startAdvertisingForAll();
  Serial.println("BLE: pairing mode on — discoverable as Omote Remote");
}

void bleStopPairingMode() {
  sPairingMode = false;
  bleStopAdvertising();
  /* Resume directed/whitelist adv so the bonded TV can still reconnect. */
  if (sBleStarted && NimBLEDevice::getNumBonds() > 0) startBondedAdvertising();
}

bool bleIsPairingMode() { return sPairingMode; }

bool bleIsAdvertising() { return sBleStarted && bleKeyboard.isAdvertising(); }

void bleDisconnectClients() {
  if (!sBleStarted) return;
  bleKeyboard.disconnectAllClients();
  Serial.println("BLE: disconnected active clients");
}

void bleForgetBonds() {
  if (!sBleStarted) bleInit();
  bleKeyboard.stopAdvertising();
  bleKeyboard.deleteBonds();
  sLastBondCount = 0;
  sPairingMode = true;
  bleKeyboard.startAdvertisingForAll();
  Serial.println("BLE: bonds cleared — advertising for new pairing");
}

bool bleIsConnected() { return sBleStarted && bleKeyboard.isConnected(); }

/**
 * Periodic supervisor: keep advertising up so the TV can reconnect after the
 * Omote sleeps/wakes or moves out and back into range. BleKeyboard hardcodes
 * advertiseOnDisconnect(false), so we restart adv ourselves.
 */
void bleTaskLoop() {
  if (!sBleStarted) return;
  const uint32_t now = millis();
  if (now - sLastAdvCheckMs < 1500) return;
  sLastAdvCheckMs = now;
  if (bleKeyboard.isConnected()) {
    /* Got the connection; drop the wake-window override so we don't stay
     * over-discoverable longer than needed. */
    sOpenAdvUntilMs = 0;
    return;
  }
  /* Newly paired bond can change the strategy (directed ↔ whitelist). */
  const int bonds = NimBLEDevice::getNumBonds();
  if (bonds != (int)sLastBondCount) {
    sLastBondCount = bonds;
    Serial.printf("BLE: bond count changed → %d\n", bonds);
  }
  /* Wake window: keep undirected open adv up for the full window so the TV
   * can find us even if directed adv missed its scan. */
  if (sOpenAdvUntilMs && (int32_t)(now - sOpenAdvUntilMs) < 0) {
    if (!bleKeyboard.isAdvertising()) bleKeyboard.startAdvertisingForAll();
    return;
  }
  if (sOpenAdvUntilMs && (int32_t)(now - sOpenAdvUntilMs) >= 0) {
    sOpenAdvUntilMs = 0;
    /* Drop open adv so the bonded strategy can take over cleanly. */
    bleKeyboard.stopAdvertising();
    Serial.println("BLE: wake window elapsed — resuming bonded supervision");
  }
  if (bleKeyboard.isAdvertising()) return;
  /* Don't second-guess explicit pairing-mode adv. */
  if (sPairingMode) {
    bleKeyboard.startAdvertisingForAll();
    return;
  }
  if (bonds > 0) {
    startBondedAdvertising();
  } else {
    /* No bonds + not in pairing mode = idle. Wait for user. */
  }
}

/** Map Android KeyEvent-style names to internal BLE key ids. */
static String normalizeBleKeyName(String k) {
  k.trim();
  k.toUpperCase();
  if (k.startsWith("KEYCODE_")) k = k.substring(8);
  if (k.startsWith("KEY_")) k = k.substring(4);
  if (k == "DPAD_UP" || k == "UP_ARROW") return "UP";
  if (k == "DPAD_DOWN" || k == "DOWN_ARROW") return "DOWN";
  if (k == "DPAD_LEFT" || k == "LEFT_ARROW") return "LEFT";
  if (k == "DPAD_RIGHT" || k == "RIGHT_ARROW") return "RIGHT";
  if (k == "DPAD_CENTER" || k == "CENTER" || k == "OK" || k == "SELECT" ||
      k == "ENTER" || k == "KPENTER" || k == "NUMPAD_ENTER")
    return "DPAD_CENTER";
  if (k == "RETURN") return "ENTER_TEXT";  /* generic newline */
  if (k == "VOLUME_MUTE" || k == "MUTE") return "MUTE";
  if (k == "VOLUME_UP" || k == "VOL_UP" || k == "VOLUP") return "VOLUME_UP";
  if (k == "VOLUME_DOWN" || k == "VOL_DOWN" || k == "VOLDN") return "VOLUME_DOWN";
  if (k == "MEDIA_PLAY_PAUSE" || k == "PLAY_PAUSE") return "PLAY_PAUSE";
  if (k == "MEDIA_PLAY") return "PLAY";
  if (k == "MEDIA_PAUSE") return "PAUSE";
  if (k == "MEDIA_STOP") return "STOP";
  if (k == "MEDIA_NEXT" || k == "NEXT_TRACK") return "NEXT";
  if (k == "MEDIA_PREVIOUS" || k == "MEDIA_PREV" || k == "PREV_TRACK") return "PREVIOUS";
  if (k == "MEDIA_FAST_FORWARD" || k == "FAST_FORWARD" || k == "MEDIA_SKIP_FORWARD")
    return "FORWARD";
  if (k == "MEDIA_REWIND" || k == "MEDIA_SKIP_BACKWARD") return "REWIND";
  if (k == "CHANNEL_UP" || k == "CH_UP" || k == "CHUP") return "CHANNEL_UP";
  if (k == "CHANNEL_DOWN" || k == "CH_DOWN" || k == "CHDN") return "CHANNEL_DOWN";
  if (k == "CAPTIONS" || k == "CLOSED_CAPTION" || k == "CC") return "CAPTIONS";
  if (k == "EPG" || k == "PROGRAM_GUIDE" || k == "TV_GUIDE") return "GUIDE";
  if (k == "PROFILE_SWITCH" || k == "SWITCH_PROFILE" || k == "PROFILES") return "PROFILE_SWITCH";
  if (k == "NOTIFICATION" || k == "NOTIFICATIONS" || k == "NOTIFY") return "NOTIFICATION";
  if (k == "LIVE" || k == "LIVE_TV" || k == "GO_TO_LIVE_TV") return "LIVE_TV";
  if (k == "NAVBAR") return "SEARCH";  /* old GTV protocol: NAVBAR == search */
  if (k == "EXPLORER" || k == "BROWSER") return "WWW_HOME";
  /* Android KEYCODE_DEL is the backspace key (deletes char before cursor).
   * KEYCODE_FORWARD_DEL is the actual forward-delete. The legacy GTV protocol
   * sent KEYCODE_DEL for "backspace", so DEL must map to BACKSPACE, not DELETE. */
  if (k == "DEL") return "BACKSPACE";
  if (k == "FORWARD_DEL" || k == "FORWARD_DELETE") return "DELETE";
  if (k == "BACK") return "BACK";
  if (k == "HOME") return "HOME";
  if (k == "MENU") return "MENU";
  if (k == "SEARCH") return "SEARCH";
  if (k == "ASSIST" || k == "VOICE_ASSIST") return "ASSIST";
  if (k == "APP_SWITCH" || k == "ALL_APPS") return "APP_SWITCH";
  if (k == "POWER" || k == "SLEEP") return "POWER";
  if (k == "TV_POWER") return "TV_POWER";
  /* Google TV launcher convention (matches HID Consumer Page 0x77/78/79/7A
   * → KEYCODE_BUTTON_3/4/6/7 — see Vendor_0484_Product_5738.kl): */
  if (k == "YOUTUBE")                       return "BUTTON_3";
  if (k == "NETFLIX")                       return "BUTTON_4";
  if (k == "PRIME_VIDEO" || k == "PRIME")   return "BUTTON_6";
  if (k == "DISNEY_PLUS" || k == "DISNEY")  return "BUTTON_7";
  /* Convention for additional app shortcuts (no fixed standard — launchers
   * vary). Users can override by assigning BUTTON_<N> directly in the editor. */
  if (k == "SPOTIFY")                       return "BUTTON_8";
  if (k == "HBO_MAX" || k == "MAX")         return "BUTTON_9";
  if (k == "APPLE_TV")                      return "BUTTON_10";
  if (k == "JELLYFIN")                      return "BUTTON_11";
  if (k.startsWith("VIDEO_APP_")) {
    int n = k.substring(10).toInt();
    if (n >= 1 && n <= 16) {
      k = "BUTTON_";
      k += String(n);
      return k;
    }
  }
  return k;
}

/**
 * Map a normalized key name to the HID Consumer Page usage that
 * Vendor_0957_Product_0001.kl recognises. Returns 0 if unmapped.
 */
static uint16_t consumerUsageFor(const String &k) {
  if (k == "DPAD_CENTER")          return HID_CC_MENU_PICK;
  if (k == "BACK" || k == "ESC")   return HID_CC_AC_BACK;
  if (k == "HOME")                 return HID_CC_AC_HOME;
  if (k == "SEARCH")               return HID_CC_AC_SEARCH;
  if (k == "VOLUME_UP")            return HID_CC_VOLUME_UP;
  if (k == "VOLUME_DOWN")          return HID_CC_VOLUME_DOWN;
  if (k == "MUTE")                 return HID_CC_MUTE;
  if (k == "PLAY_PAUSE" || k == "PLAY" || k == "PAUSE") return HID_CC_PLAY_PAUSE;
  if (k == "STOP")                 return HID_CC_STOP;
  if (k == "NEXT")                 return HID_CC_NEXT_TRACK;
  if (k == "PREVIOUS")             return HID_CC_PREV_TRACK;
  if (k == "FORWARD")              return HID_CC_SKIP_FORWARD;
  if (k == "REWIND")               return HID_CC_SKIP_BACKWARD;
  if (k == "RECORD" || k == "MEDIA_RECORD") return HID_CC_RECORD;
  if (k == "GUIDE")                return HID_CC_GUIDE;
  if (k == "CHANNEL_UP")           return HID_CC_CHANNEL_UP;
  if (k == "CHANNEL_DOWN")         return HID_CC_CHANNEL_DOWN;
  if (k == "TV" || k == "LIVE_TV") return HID_CC_TV;
  if (k == "INFO")                 return HID_CC_INFO;       /* not on Generic.kl */
  if (k == "CAPTIONS")             return HID_CC_CAPTIONS;
  if (k == "NOTIFICATION")         return HID_CC_NOTIFICATION;
  if (k == "PROFILE_SWITCH")       return HID_CC_PROFILE_SWITCH;
  if (k == "ALL_APPS" || k == "APP_SWITCH") return HID_CC_ALL_APPS;
  if (k == "ASSIST" || k == "VOICE_ASSIST") return HID_CC_ASSIST;
  if (k == "TV_TELETEXT")          return HID_CC_TV_TELETEXT; /* not on Generic.kl */
  if (k == "PROG_RED")             return HID_CC_PROG_RED;
  if (k == "PROG_GREEN")           return HID_CC_PROG_GREEN;
  if (k == "PROG_BLUE")            return HID_CC_PROG_BLUE;
  if (k == "PROG_YELLOW")          return HID_CC_PROG_YELLOW;
  if (k == "POWER" || k == "TV_POWER") return HID_CC_POWER;
  if (k == "SLEEP")                return HID_CC_SLEEP;
  return 0;
}

/**
 * Parse `BUTTON_<N>` (1..16) into a button index. Returns 0 if not a button
 * name. Streaming-app shortcuts (NETFLIX, YOUTUBE, …) are normalized to
 * BUTTON_N upstream in normalizeBleKeyName().
 */
static uint8_t gamepadButtonFor(const String &k) {
  if (!k.startsWith("BUTTON_")) return 0;
  const long n = k.substring(7).toInt();
  if (n < 1 || n > 16) return 0;
  return (uint8_t)n;
}

void bleSendMediaKey(const String &keyName) {
  /* Same code path as bleSendKey now — kept for backwards compatibility with
   * existing config files that still use type=media_key. */
  bleSendKey(keyName);
}

void bleSendKey(const String &keyName) {
  if (!sBleStarted) {
    Serial.println("BLE: not ready (key ignored)");
    return;
  }
  if (!bleKeyboard.isConnected()) {
    Serial.printf("BLE: not connected ('%s' ignored)\n", keyName.c_str());
    return;
  }
  String k = normalizeBleKeyName(keyName);

  /* 1) DPAD arrows — keyboard-page HID keys map to Linux KEY_{UP,…} which
   *    every Android keylayout maps to DPAD_*. */
  if (k == "UP")    { bleKeyboard.write(KEY_UP_ARROW); return; }
  if (k == "DOWN")  { bleKeyboard.write(KEY_DOWN_ARROW); return; }
  if (k == "LEFT")  { bleKeyboard.write(KEY_LEFT_ARROW); return; }
  if (k == "RIGHT") { bleKeyboard.write(KEY_RIGHT_ARROW); return; }

  /* 2) Editing keys that belong on the keyboard page. */
  if (k == "ENTER_TEXT") { bleKeyboard.write(KEY_RETURN); return; }
  if (k == "BACKSPACE")  { bleKeyboard.write(KEY_BACKSPACE); return; }
  if (k == "DELETE")     { bleKeyboard.write(KEY_DELETE); return; }
  if (k == "TAB")        { bleKeyboard.write(KEY_TAB); return; }
  if (k == "PAGE_UP")    { bleKeyboard.write(KEY_PAGE_UP); return; }
  if (k == "PAGE_DOWN")  { bleKeyboard.write(KEY_PAGE_DOWN); return; }
  if (k == "END")        { bleKeyboard.write(KEY_END); return; }
  if (k == "INSERT")     { bleKeyboard.write(KEY_INSERT); return; }

  /* 3) App-launcher buttons (Netflix / YouTube / Prime / Disney+, etc.).
   *    Both Generic.kl and Vendor_0484_Product_5738.kl map Linux BTN_0..BTN_15
   *    (256..271) to BUTTON_1..BUTTON_16, which the launcher binds to
   *    streaming apps (BUTTON_3 → YouTube, BUTTON_4 → Netflix, …). The 16-
   *    button HID report is declared as a Vendor-defined application (not
   *    Game Pad) so the kernel emits BTN_0..BTN_15 instead of BTN_A..BTN_R3. */
  uint8_t btn = gamepadButtonFor(k);
  if (btn != 0) {
    bleKeyboard.writeGamepadButton(btn);
    return;
  }

  /* 4) TV-oriented keys — send the real HID Consumer Page usages and let the
   *    TV's keylayout translate them into the proper KEYCODE_*
   *    (DPAD_CENTER, VOLUME_UP, GUIDE, CHANNEL_UP, …). */
  uint16_t usage = consumerUsageFor(k);
  if (usage != 0) {
    bleKeyboard.writeConsumerUsage(usage);
    return;
  }

  /* 5) Plain ASCII fallback for letters / digits / punctuation. */
  if (k.length() == 1) { bleKeyboard.write(k[0]); return; }

  Serial.printf("BLE: unknown key '%s' (after normalize: '%s')\n", keyName.c_str(), k.c_str());
}

void bleSendText(const String &text) {
  if (!sBleStarted) {
    Serial.println("BLE: not ready (text ignored)");
    return;
  }
  if (!bleKeyboard.isConnected()) return;
  for (unsigned i = 0; i < text.length(); i++) {
    const char c = text[i];
    if (c == '\n')
      bleKeyboard.write(KEY_RETURN);
    else if (c == '\t')
      bleKeyboard.write(KEY_TAB);
    else if (c >= 32 && c <= 126)
      bleKeyboard.write((uint8_t)c);
  }
}

bool bleSendRawConsumerUsage(uint16_t usage) {
  if (!sBleStarted || !bleKeyboard.isConnected()) return false;
  Serial.printf("BLE: raw consumer usage 0x%04X\n", usage);
  bleKeyboard.writeConsumerUsage(usage);
  return true;
}

bool bleSendRawButton(uint8_t button1to16) {
  if (!sBleStarted || !bleKeyboard.isConnected()) return false;
  if (button1to16 < 1 || button1to16 > 16) return false;
  Serial.printf("BLE: raw button %u\n", button1to16);
  bleKeyboard.writeGamepadButton(button1to16);
  return true;
}

String bleStatusJson() {
  String s = "{\"connected\":";
  s += bleIsConnected() ? "true" : "false";
  s += ",\"initialized\":";
  s += sBleStarted ? "true" : "false";
  s += ",\"advertising\":";
  s += bleIsAdvertising() ? "true" : "false";
  s += ",\"pairing_mode\":";
  s += sPairingMode ? "true" : "false";
  const int n = sBleStarted ? NimBLEDevice::getNumBonds() : 0;
  s += ",\"bond_count\":";
  s += String(n);
  s += ",\"bonds\":[";
  for (int i = 0; i < n; i++) {
    if (i) s += ',';
    s += '"';
    s += NimBLEDevice::getBondedAddress(i).toString().c_str();
    s += '"';
  }
  s += "]}";
  return s;
}
