#pragma once

#include <Arduino.h>

void initOtaaManager();
void otaaTick();
bool handleOtaaControlFrame(const uint8_t *frame, size_t frameLen, int rssi, float snr);

void requestNodeActive();
void requestNodeSleep(uint32_t sleepSeconds);
