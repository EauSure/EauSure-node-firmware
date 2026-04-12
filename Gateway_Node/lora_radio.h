#pragma once

#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>
#include "config.h"
#include "app_state.h"

// =====================================================
// LoRa init
// =====================================================
bool initLoRa();

// =====================================================
// Gateway → IoT  typed command TX functions
//
//  sendActivate()      — boot handshake, activates node
//  sendMeasureReq()    — request fresh sensor reading
//  sendHeartbeatReq()  — lightweight liveness ping
// =====================================================
bool isGatewayCommandInFlight();
bool sendActivate();
bool sendMeasureReq();
bool sendHeartbeatReq();

// =====================================================
// Transport-level ACK sent to IoT node
// =====================================================
bool sendAck(uint32_t seq);

// =====================================================
// Incoming DATA frame parser
// Called from loop() when a DATA frame arrives.
// Handles MEASURE_RESP and SHAKE_ALERT (both type=0x01).
// =====================================================
bool parseAndDispatchDataFrame(const uint8_t *frame, size_t frameLen,
                                int rssi, float snr);

// =====================================================
// Incoming IoT→GW typed frame parser
// Called from loop() for non-DATA frames (ACTIVATE_OK, HB_ACK).
// =====================================================
bool parseAndDispatchTypedFrame(const uint8_t *frame, size_t frameLen, int rssi, float snr);