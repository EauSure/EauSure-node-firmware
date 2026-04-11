#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "app_state.h"
#include "lora_radio.h"
#include "audio_alert.h"

// =====================================================
// handleDataPayload
//
// Entry point for all MSG_TYPE_DATA frames (0x01).
// Distinguishes MEASURE_RESP from SHAKE_ALERT via the
// "e" field in the JSON payload.
//
// Called AFTER sendAck() — the IoT node is already
// released from its retry loop when this runs.
// =====================================================
void handleDataPayload(const char *json, int rssi, float snr);

// =====================================================
// Alert file collection (used internally)
// =====================================================
void collectAlertFiles(JsonDocument& doc, int rssi);
