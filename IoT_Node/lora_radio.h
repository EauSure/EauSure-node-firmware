#pragma once
#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>
#include "config.h"
#include "app_state.h"

// =====================================================
// LoRa init
// =====================================================
bool loraInit();

// =====================================================
// IoT → Gateway  typed TX functions
//
//  sendActivateOk()     — reply to ACTIVATE handshake
//  sendMeasureResp()    — reply to MEASURE_REQ with full sensor JSON
//  sendHeartbeatAck()   — reply to HEARTBEAT_REQ with batt/state
//  sendShakeAlert()     — autonomous SHAKE_ALERT (portMAX_DELAY mutex)
// =====================================================
bool sendActivateOk();
bool sendMeasureResp(const String &sensorJson);
bool sendHeartbeatAck();
void sendShakeAlert(const String &shakeJson);

// =====================================================
// Transport-level ACK (used internally — caller must hold gLoRaMutex)
// =====================================================
bool sendAck(uint32_t seq);

// =====================================================
// Incoming frame listener
// Called from ControlTask loop — blocks up to timeoutMs.
// Returns true if a valid command frame was received and dispatched.
// =====================================================
bool pollCommandFrame(uint32_t timeoutMs);