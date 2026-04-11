#include "app_state.h"

// =====================================================
// Global State
// =====================================================
uint32_t lastAcceptedSeq = 0;
uint32_t gTxSeq = 1;

// =====================================================
// Helper Functions - Binary Encoding/Decoding
// =====================================================
void writeU16BE(uint8_t *dst, uint16_t v) {
  dst[0] = (uint8_t)((v >> 8) & 0xFF);
  dst[1] = (uint8_t)(v & 0xFF);
}

void writeU32BE(uint8_t *dst, uint32_t v) {
  dst[0] = (uint8_t)((v >> 24) & 0xFF);
  dst[1] = (uint8_t)((v >> 16) & 0xFF);
  dst[2] = (uint8_t)((v >> 8) & 0xFF);
  dst[3] = (uint8_t)(v & 0xFF);
}

uint16_t readU16BE(const uint8_t *src) {
  return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

uint32_t readU32BE(const uint8_t *src) {
  return ((uint32_t)src[0] << 24) |
         ((uint32_t)src[1] << 16) |
         ((uint32_t)src[2] << 8)  |
         ((uint32_t)src[3]);
}

// =====================================================
// CRC-16 CCITT
// =====================================================
uint16_t crc16Ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; ++b) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else              crc = (crc << 1);
    }
  }
  return crc;
}

// =====================================================
// Build Nonce for GCM
// =====================================================
void buildNonce(uint32_t seq, uint8_t nonce[GCM_NONCE_LEN]) {
  writeU32BE(&nonce[0], DEVICE_ID);
  writeU32BE(&nonce[4], seq);
  nonce[8]  = random(0, 256);
  nonce[9]  = random(0, 256);
  nonce[10] = random(0, 256);
  nonce[11] = random(0, 256);
}

// =====================================================
// AES-GCM Decryption
// =====================================================
bool aesgcmDecrypt(
  const uint8_t *cipher,
  size_t cipherLen,
  const uint8_t nonce[GCM_NONCE_LEN],
  const uint8_t *aad,
  size_t aadLen,
  const uint8_t tag[GCM_TAG_LEN],
  uint8_t *plain
) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, ENC_KEY, 128) != 0) {
    mbedtls_gcm_free(&gcm);
    return false;
  }

  int rc = mbedtls_gcm_auth_decrypt(
    &gcm,
    cipherLen,
    nonce, GCM_NONCE_LEN,
    aad, aadLen,
    tag, GCM_TAG_LEN,
    cipher,
    plain
  );

  mbedtls_gcm_free(&gcm);
  return rc == 0;
}

// =====================================================
// Build Secure Frame with AES-GCM
// =====================================================
bool buildSecureFrame(
  uint8_t msgType,
  uint32_t seq,
  const uint8_t *plain,
  uint16_t plainLen,
  uint8_t *outFrame,
  size_t &outLen
) {
  if (plainLen > MAX_PLAIN_LEN) return false;

  uint8_t nonce[GCM_NONCE_LEN];
  buildNonce(seq, nonce);

  // Build header (unencrypted, authenticated as AAD)
  size_t pos = 0;
  outFrame[pos++] = PROTO_VERSION;
  outFrame[pos++] = msgType;
  writeU32BE(&outFrame[pos], DEVICE_ID); pos += 4;
  writeU32BE(&outFrame[pos], seq);       pos += 4;
  memcpy(&outFrame[pos], nonce, GCM_NONCE_LEN); pos += GCM_NONCE_LEN;
  writeU16BE(&outFrame[pos], plainLen);  pos += 2;

  uint8_t *ciphertext = &outFrame[pos];
  uint8_t tag[GCM_TAG_LEN];
  
  size_t aadLen = pos;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, ENC_KEY, 128) != 0) {
    mbedtls_gcm_free(&gcm);
    return false;
  }

  uint8_t dummy[1] = {0};
  const uint8_t *plainPtr = (plainLen > 0) ? plain : dummy;
  uint8_t *cipherPtr = (plainLen > 0) ? ciphertext : dummy;

  int rc = mbedtls_gcm_crypt_and_tag(
    &gcm,
    MBEDTLS_GCM_ENCRYPT,
    plainLen,
    nonce, GCM_NONCE_LEN,
    outFrame, aadLen,
    plainPtr,
    cipherPtr,
    GCM_TAG_LEN, tag
  );

  mbedtls_gcm_free(&gcm);
  
  if (rc != 0) return false;

  pos += plainLen;

  memcpy(&outFrame[pos], tag, GCM_TAG_LEN);
  pos += GCM_TAG_LEN;

  uint16_t crc = crc16Ccitt(outFrame, pos);
  writeU16BE(&outFrame[pos], crc);
  pos += 2;

  outLen = pos;
  return true;
}

// =====================================================
// Application Init
// =====================================================
void initApp() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println("\n=== Gateway Node - FreeRTOS Water Quality Monitor ===");
}
