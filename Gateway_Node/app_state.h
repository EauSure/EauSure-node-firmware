//gateway_node/app_state.h
#pragma once

#include <Arduino.h>
#include "config.h"
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"

// =====================================================
// Protocol MSG_TYPE constants  (must match IoT node)
//
//  0x01  DATA            IoT → GW   MEASURE_RESP or SHAKE_ALERT
//  0x02  ACK             both dirs  transport acknowledgement
//  0x03  MEASURE_REQ     GW  → IoT  request fresh sensor reading
//  0x04  HEARTBEAT_REQ   GW  → IoT  lightweight liveness ping
//  0x05  HEARTBEAT_ACK   IoT → GW   liveness reply {batt%, state}
//  0x06  ACTIVATE        GW  → IoT  boot handshake — activate node
//  0x07  ACTIVATE_OK     IoT → GW   handshake reply {state, mac}
// =====================================================
static const uint8_t MSG_TYPE_DATA          = 0x01;
static const uint8_t MSG_TYPE_ACK           = 0x02;
static const uint8_t MSG_TYPE_MEASURE_REQ   = 0x03;
static const uint8_t MSG_TYPE_HEARTBEAT_REQ = 0x04;
static const uint8_t MSG_TYPE_HEARTBEAT_ACK = 0x05;
static const uint8_t MSG_TYPE_ACTIVATE      = 0x06;
static const uint8_t MSG_TYPE_ACTIVATE_OK   = 0x07;

// =====================================================
// Frame layout constants
// =====================================================
static const size_t GCM_NONCE_LEN = 12;
static const size_t GCM_TAG_LEN   = 16;
static const size_t CRC_LEN       = 2;
static const size_t HEADER_LEN    = 1 + 1 + 4 + 4 + GCM_NONCE_LEN + 2;
static const size_t MAX_PLAIN_LEN = 180;
static const size_t MAX_FRAME_LEN = HEADER_LEN + MAX_PLAIN_LEN + GCM_TAG_LEN + CRC_LEN;

// =====================================================
// Global state
// =====================================================
extern uint32_t gTxSeq;          // outgoing command sequence counter
extern uint32_t lastAcceptedSeq; // last DATA seq accepted from IoT node

// =====================================================
// Binary encode/decode helpers
// =====================================================
void     writeU16BE(uint8_t *dst, uint16_t v);
void     writeU32BE(uint8_t *dst, uint32_t v);
uint16_t readU16BE(const uint8_t *src);
uint32_t readU32BE(const uint8_t *src);

// =====================================================
// CRC-16 CCITT
// =====================================================
uint16_t crc16Ccitt(const uint8_t *data, size_t len);

// =====================================================
// Cryptography
// =====================================================
void buildNonce(uint32_t seq, uint8_t nonce[GCM_NONCE_LEN]);

bool aesgcmDecrypt(
  const uint8_t *cipher,
  size_t         cipherLen,
  const uint8_t  nonce[GCM_NONCE_LEN],
  const uint8_t *aad,
  size_t         aadLen,
  const uint8_t  tag[GCM_TAG_LEN],
  uint8_t       *plain
);

bool buildSecureFrame(
  uint8_t        msgType,
  uint32_t       seq,
  const uint8_t *plain,
  uint16_t       plainLen,
  uint8_t       *outFrame,
  size_t        &outLen
);

// =====================================================
// App init
// =====================================================
void initApp();