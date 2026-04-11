#pragma once
#include <Arduino.h>

bool loraInit();
bool loraSendText(const String& payload);   // keep if you want debug plaintext tests
bool secureSendJson(const String& json);
void sendJsonData();
bool secureSendControl(const String& json);
void pollControlFrames(uint32_t timeoutMs);
