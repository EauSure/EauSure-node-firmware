#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "app_state.h"
#include "lora_radio.h"
#include "audio_alert.h"

// =====================================================
// Alert Collection
// =====================================================
void collectAlertFiles(JsonDocument& doc, int rssi);

// =====================================================
// Secure Packet Handler
// =====================================================
void handleSecurePacket(const uint8_t *frame, size_t frameLen, int rssi, float snr);
