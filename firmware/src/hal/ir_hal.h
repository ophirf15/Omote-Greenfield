#pragma once

#include <Arduino.h>

void irInit();
void irSend(const String &protocolName, uint64_t code, uint16_t bits);
bool irLearnStart();
bool irLearnPoll(String &protocolOut, uint64_t &codeOut, uint16_t &bitsOut);
void irLearnStop();
