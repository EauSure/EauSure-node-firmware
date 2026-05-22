#pragma once
#include <Arduino.h>

// =====================================================
// OTAA Manager
//
// Drives the gateway command schedule:
//   - Boot: sends ACTIVATE, waits for ACTIVATE_OK
//   - Every 60 s (+ manual 'm' key): sends MEASURE_REQ
//   - Every 10 s: sends HEARTBEAT_REQ
//
// Called from Gateway loop() — not FreeRTOS.
// =====================================================
void initOtaaManager();
void otaaTick();
bool shouldPauseBackgroundWork();

// Manual trigger (serial 'm' key)
void requestMeasureNow();
void notifyMeasureRequestDispatched();
void notifyMeasureResponseHandled();

// Wake window: from MEASURE_REQ until SLEEP (or timeout). Blocks scheduler + MQTT pause.
bool isMeasureWakeWindowActive();
void onMeasureWakeWindowClosed();

// Sleep until the next scheduled MEASURE or HEARTBEAT (whichever is sooner), minus wake margin.
uint32_t computeSleepDurationSec();

// Config update from SET_CONFIG MQTT command
void setMeasureIntervalMs(uint32_t ms);
uint32_t getMeasureIntervalMs();
void setNodeActiveFlag(bool active);
void setVocalAlertsEnabled(bool enabled);
bool areVocalAlertsEnabled();
bool isNodeActiveConfigured();

// Dissociation — erase pairing and reboot into pairing mode
void erasePairingAndEnterPairingMode();

// =====================================================
// Incoming frame handlers — called from lora_radio.cpp
// =====================================================
void handleActivateOk(const char *json, int rssi, float snr);
void handleHeartbeatAck(const char *json, int rssi, float snr);
