#pragma once

#include <Arduino.h>
#include "config.h"
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"

// =====================================================
// Protocol Constants
// =====================================================
static const uint8_t MSG_TYPE_DATA = 0x01;
static const uint8_t MSG_TYPE_ACK  = 0x02;
static const uint8_t MSG_TYPE_CTRL = 0x03;

static const size_t GCM_NONCE_LEN  = 12;
static const size_t GCM_TAG_LEN    = 16;
static const size_t CRC_LEN        = 2;
static const size_t HEADER_LEN     = 1 + 1 + 4 + 4 + GCM_NONCE_LEN + 2;
static const size_t MAX_PLAIN_LEN  = 180;
static const size_t MAX_FRAME_LEN  = HEADER_LEN + MAX_PLAIN_LEN + GCM_TAG_LEN + CRC_LEN;

// =====================================================
// Global State
// =====================================================
extern uint32_t lastAcceptedSeq;
extern uint32_t gTxSeq;

// =====================================================
// Helper Functions - Binary Encoding/Decoding
// =====================================================
void writeU16BE(uint8_t *dst, uint16_t v);
void writeU32BE(uint8_t *dst, uint32_t v);
uint16_t readU16BE(const uint8_t *src);
uint32_t readU32BE(const uint8_t *src);

// =====================================================
// CRC-16 CCITT
// =====================================================
uint16_t crc16Ccitt(const uint8_t *data, size_t len);

// =====================================================
// Cryptography Functions
// =====================================================
void buildNonce(uint32_t seq, uint8_t nonce[GCM_NONCE_LEN]);

bool aesgcmDecrypt(
  const uint8_t *cipher,
  size_t cipherLen,
  const uint8_t nonce[GCM_NONCE_LEN],
  const uint8_t *aad,
  size_t aadLen,
  const uint8_t tag[GCM_TAG_LEN],
  uint8_t *plain
);

bool buildSecureFrame(
  uint8_t msgType,
  uint32_t seq,
  const uint8_t *plain,
  uint16_t plainLen,
  uint8_t *outFrame,
  size_t &outLen
);

// =====================================================
// Application Init
// =====================================================
void initApp();
