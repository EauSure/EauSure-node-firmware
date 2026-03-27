#pragma once
#include <Arduino.h>

bool loraInit();
bool loraSendText(const String& payload);   // keep if you want debug plaintext tests
bool secureSendJson(const String& json);
void sendJsonData();