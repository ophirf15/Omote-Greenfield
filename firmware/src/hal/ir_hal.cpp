#include "hal/ir_hal.h"
#include "hal/pins.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>

static IRsend irSender(PIN_IR_TX, true);
static IRrecv irReceiver(PIN_IR_RX, 1024, 50, true);
static decode_results lastResult;
static bool learning = false;
static bool irRxEnabled = false;

static decode_type_t protocolFromName(const String &name) {
  if (name.equalsIgnoreCase("NEC")) return NEC;
  if (name.equalsIgnoreCase("SAMSUNG")) return SAMSUNG;
  if (name.equalsIgnoreCase("SONY")) return SONY;
  if (name.equalsIgnoreCase("RC5")) return RC5;
  if (name.equalsIgnoreCase("RC6")) return RC6;
  if (name.equalsIgnoreCase("JVC")) return JVC;
  if (name.equalsIgnoreCase("PANASONIC")) return PANASONIC;
  // Kaseikyo is the Panasonic-family protocol naming used by Flipper and others.
  if (name.equalsIgnoreCase("KASEIKYO")) return PANASONIC;
  if (name.equalsIgnoreCase("DENON")) return DENON;
  if (name.equalsIgnoreCase("SHARP")) return SHARP;
  if (name.equalsIgnoreCase("LG")) return LG;
  Serial.printf("IR send: unknown protocol '%s', fallback NEC\n", name.c_str());
  return NEC;
}

void irInit() {
  pinMode(PIN_IR_TX, OUTPUT);
  digitalWrite(PIN_IR_TX, HIGH);
  pinMode(PIN_IR_RX_PWR, OUTPUT);
  digitalWrite(PIN_IR_RX_PWR, HIGH);
  irSender.begin();
}

void irSend(const String &protocolName, uint64_t code, uint16_t bits) {
  decode_type_t proto = protocolFromName(protocolName);
  if (bits == 0) bits = 32;
  Serial.printf("IR TX mapped proto=%s bits=%u code=0x%llX\n",
                typeToString(proto, false).c_str(), bits, (unsigned long long)code);
  irSender.send(proto, code, bits);
}

bool irLearnStart() {
  if (irRxEnabled) irLearnStop();
  irReceiver.enableIRIn();
  irRxEnabled = true;
  learning = true;
  return true;
}

bool irLearnPoll(String &protocolOut, uint64_t &codeOut, uint16_t &bitsOut) {
  if (!learning) return false;
  if (!irReceiver.decode(&lastResult)) return false;
  if (lastResult.overflow) {
    irReceiver.resume();
    return false;
  }
  protocolOut = String(typeToString(lastResult.decode_type, false).c_str());
  codeOut = lastResult.value;
  bitsOut = lastResult.bits;
  irReceiver.resume();
  return true;
}

void irLearnStop() {
  learning = false;
  if (!irRxEnabled) return;
  irReceiver.disableIRIn();
  irRxEnabled = false;
}
