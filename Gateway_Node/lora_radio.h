#pragma once

#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>
#include "config.h"
#include "app_state.h"

// =====================================================
// LoRa Init
// =====================================================
bool initLoRa();

// =====================================================
// LoRa Transmit Functions
// =====================================================
void deselectLoRa();
bool loraSendRaw(const uint8_t *data, size_t len);
void loraSendString(const String& msg);
void loraSendChar(char c);

// =====================================================
// Secure Protocol
// =====================================================
bool sendSecureAck(uint32_t seq);

bool parseAndVerifyDataFrame(
  const uint8_t *frame,
  size_t frameLen,
  uint32_t &seqOut,
  uint8_t *plainOut,
  uint16_t &plainLenOut
);
