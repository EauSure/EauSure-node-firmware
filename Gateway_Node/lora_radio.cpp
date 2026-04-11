#include "lora_radio.h"

// =====================================================
// LoRa Deselect
// =====================================================
void deselectLoRa() {
  digitalWrite(LORA_NSS, HIGH);
}

// =====================================================
// Init LoRa on default SPI
// =====================================================
bool initLoRa() {
  pinMode(LORA_NSS, OUTPUT);
  pinMode(LORA_RST, OUTPUT);
  pinMode(LORA_DIO0, INPUT);
  pinMode(LORA_DIO1, INPUT);
  pinMode(LORA_DIO2, INPUT);

  deselectLoRa();

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] ERROR: LoRa.begin failed");
    return false;
  }

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.setTxPower(17);

  LoRa.receive();
  Serial.println("[LoRa] RX mode ON");
  Serial.println("[LoRa] Secure mode enabled");
  return true;
}

// =====================================================
// Send Raw Binary Frame
// =====================================================
bool loraSendRaw(const uint8_t *data, size_t len) {
  LoRa.idle();
  delay(2);
  LoRa.beginPacket();
  LoRa.write(data, len);
  bool ok = LoRa.endPacket();
  delay(2);
  LoRa.receive();
  return ok;
}

static bool loraReadRaw(uint8_t *buf, size_t cap, size_t &outLen, uint32_t timeoutMs) {
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    int packetSize = LoRa.parsePacket();
    if (packetSize > 0) {
      if ((size_t)packetSize > cap) {
        while (LoRa.available()) LoRa.read();
        LoRa.receive();
        return false;
      }
      size_t i = 0;
      while (LoRa.available() && i < cap) {
        buf[i++] = (uint8_t)LoRa.read();
      }
      outLen = i;
      LoRa.receive();
      return true;
    }
    delay(10);
  }
  LoRa.receive();
  return false;
}

// =====================================================
// Send String
// =====================================================
void loraSendString(const String& msg) {
  LoRa.idle();
  delay(2);
  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();
  Serial.printf("[LoRa TX] \"%s\"\n", msg.c_str());
  delay(2);
  LoRa.receive();
}

// =====================================================
// Send Single Character
// =====================================================
void loraSendChar(char c) {
  LoRa.idle();
  delay(2);
  LoRa.beginPacket();
  LoRa.write((uint8_t)c);
  LoRa.endPacket();
  Serial.printf("[LoRa TX] '%c'\n", c);
  delay(2);
  LoRa.receive();
}

// =====================================================
// Send Secure ACK
// =====================================================
bool sendSecureAck(uint32_t seq) {
  uint8_t frame[MAX_FRAME_LEN];
  size_t frameLen = 0;

  if (!buildSecureFrame(MSG_TYPE_ACK, seq, nullptr, 0, frame, frameLen)) {
    Serial.println("[ACK] build failed");
    return false;
  }

  bool ok = loraSendRaw(frame, frameLen);
  Serial.printf("[ACK] seq=%lu sent=%s\n", (unsigned long)seq, ok ? "yes" : "no");
  return ok;
}

// =====================================================
// Parse and Verify Secure Data Frame
// =====================================================
bool parseAndVerifyDataFrame(
  const uint8_t *frame,
  size_t frameLen,
  uint32_t &seqOut,
  uint8_t *plainOut,
  uint16_t &plainLenOut
) {
  if (frameLen < HEADER_LEN + GCM_TAG_LEN + CRC_LEN) {
    Serial.println("[GCM] frame too short");
    return false;
  }

  // Verify CRC-16
  uint16_t rxCrc = readU16BE(&frame[frameLen - 2]);
  uint16_t calcCrc = crc16Ccitt(frame, frameLen - 2);
  if (rxCrc != calcCrc) {
    Serial.println("[GCM] CRC mismatch");
    return false;
  }

  // Parse header
  uint8_t version = frame[0];
  uint8_t msgType = frame[1];
  uint32_t deviceId = readU32BE(&frame[2]);
  uint32_t seq = readU32BE(&frame[6]);
  const uint8_t *nonce = &frame[10];
  uint16_t cipherLen = readU16BE(&frame[10 + GCM_NONCE_LEN]);

  Serial.print("[GCM RX] Parsing frame: ver=");
  Serial.print(version);
  Serial.print(" type=");
  Serial.print(msgType);
  Serial.print(" seq=");
  Serial.print(seq);
  Serial.print(" cipherLen=");
  Serial.print(cipherLen);
  Serial.print(" frameLen=");
  Serial.println(frameLen);

  // Verify header fields
  if (version != PROTO_VERSION) {
    Serial.println("[GCM] Version mismatch");
    return false;
  }

  if (msgType != MSG_TYPE_DATA) {
    Serial.println("[GCM] Message type is not DATA");
    return false;
  }

  if (deviceId != DEVICE_ID) {
    Serial.printf("[GCM] Device ID mismatch: got 0x%08lX\n", (unsigned long)deviceId);
    return false;
  }

  // Verify frame length
  size_t expectedLen = HEADER_LEN + cipherLen + GCM_TAG_LEN + CRC_LEN;
  if (frameLen != expectedLen) {
    Serial.printf("[GCM] Frame length mismatch: got %zu expected %zu\n", frameLen, expectedLen);
    return false;
  }

  if (cipherLen > MAX_PLAIN_LEN) {
    Serial.printf("[GCM] Payload too large: %u\n", cipherLen);
    return false;
  }

  // Extract tag and ciphertext
  size_t cipherPos = HEADER_LEN;
  size_t tagPos = cipherPos + cipherLen;
  
  const uint8_t *cipher = &frame[cipherPos];
  const uint8_t *tag = &frame[tagPos];

  Serial.print("[GCM] Decrypting: cipherPos=");
  Serial.print(cipherPos);
  Serial.print(" tagPos=");
  Serial.print(tagPos);
  Serial.print(" aadLen=");
  Serial.print(HEADER_LEN);
  Serial.print(" tag: ");
  for (int i = 0; i < 4; i++) {
    Serial.print(tag[i], HEX);
    Serial.print(" ");
  }
  Serial.println("...");

  // Decrypt and verify authentication tag
  if (!aesgcmDecrypt(cipher, cipherLen, nonce, frame, HEADER_LEN, tag, plainOut)) {
    Serial.println("[GCM] Authentication failed - corrupted or invalid packet");
    return false;
  }

  Serial.print("[GCM] Decryption successful: ");
  Serial.print(cipherLen);
  Serial.println(" bytes");

  seqOut = seq;
  plainLenOut = cipherLen;
  return true;
}

bool parseAndVerifyControlFrame(
  const uint8_t *frame,
  size_t frameLen,
  uint32_t &seqOut,
  uint8_t *plainOut,
  uint16_t &plainLenOut
) {
  if (frameLen < HEADER_LEN + GCM_TAG_LEN + CRC_LEN) {
    Serial.println("[GCM] frame too short");
    return false;
  }

  uint16_t rxCrc = readU16BE(&frame[frameLen - 2]);
  uint16_t calcCrc = crc16Ccitt(frame, frameLen - 2);
  if (rxCrc != calcCrc) {
    Serial.println("[GCM] CRC mismatch");
    return false;
  }

  uint8_t version = frame[0];
  uint8_t msgType = frame[1];
  uint32_t deviceId = readU32BE(&frame[2]);
  uint32_t seq = readU32BE(&frame[6]);
  const uint8_t *nonce = &frame[10];
  uint16_t cipherLen = readU16BE(&frame[10 + GCM_NONCE_LEN]);

  if (version != PROTO_VERSION) {
    Serial.println("[GCM] Version mismatch");
    return false;
  }

  if (msgType != MSG_TYPE_CTRL) {
    Serial.println("[GCM] Message type is not CTRL");
    return false;
  }

  if (deviceId != DEVICE_ID) {
    Serial.printf("[GCM] Device ID mismatch: got 0x%08lX\n", (unsigned long)deviceId);
    return false;
  }

  size_t expectedLen = HEADER_LEN + cipherLen + GCM_TAG_LEN + CRC_LEN;
  if (frameLen != expectedLen) {
    Serial.printf("[GCM] Frame length mismatch: got %zu expected %zu\n", frameLen, expectedLen);
    return false;
  }

  if (cipherLen > MAX_PLAIN_LEN) {
    Serial.printf("[GCM] Payload too large: %u\n", cipherLen);
    return false;
  }

  size_t cipherPos = HEADER_LEN;
  size_t tagPos = cipherPos + cipherLen;

  const uint8_t *cipher = &frame[cipherPos];
  const uint8_t *tag = &frame[tagPos];

  if (!aesgcmDecrypt(cipher, cipherLen, nonce, frame, HEADER_LEN, tag, plainOut)) {
    Serial.println("[GCM] Authentication failed - corrupted or invalid packet");
    return false;
  }

  seqOut = seq;
  plainLenOut = cipherLen;
  return true;
}

static bool verifySecureAckFrame(const uint8_t *frame, size_t frameLen, uint32_t expectedSeq) {
  if (frameLen < HEADER_LEN + GCM_TAG_LEN + CRC_LEN) {
    return false;
  }

  uint16_t rxCrc = readU16BE(&frame[frameLen - 2]);
  uint16_t calcCrc = crc16Ccitt(frame, frameLen - 2);
  if (rxCrc != calcCrc) {
    return false;
  }

  if (frame[0] != PROTO_VERSION || frame[1] != MSG_TYPE_ACK) {
    return false;
  }

  uint32_t deviceId = readU32BE(&frame[2]);
  uint32_t seq = readU32BE(&frame[6]);
  if (deviceId != DEVICE_ID || seq != expectedSeq) {
    return false;
  }

  const uint8_t *nonce = &frame[10];
  uint16_t cipherLen = readU16BE(&frame[10 + GCM_NONCE_LEN]);
  if (cipherLen != 0) {
    return false;
  }

  size_t expectedLen = HEADER_LEN + cipherLen + GCM_TAG_LEN + CRC_LEN;
  if (frameLen != expectedLen) {
    return false;
  }

  const uint8_t *cipher = &frame[HEADER_LEN];
  const uint8_t *tag = &frame[HEADER_LEN + cipherLen];
  uint8_t dummy[1] = {0};
  return aesgcmDecrypt(cipher, cipherLen, nonce, frame, HEADER_LEN, tag, dummy);
}

static bool waitForAck(uint32_t seq, uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint8_t buf[MAX_FRAME_LEN];
  size_t len = 0;

  while ((millis() - start) < timeoutMs) {
    const uint32_t elapsed = millis() - start;
    const uint32_t remaining = timeoutMs - elapsed;
    const uint32_t slice = (remaining > 60) ? 60 : remaining;

    if (!loraReadRaw(buf, sizeof(buf), len, slice)) {
      continue;
    }

    if (verifySecureAckFrame(buf, len, seq)) {
      return true;
    }

    Serial.print("[GCM WAIT ACK] non-ACK or mismatched ACK while waiting seq=");
    Serial.println(seq);
  }

  Serial.print("[GCM WAIT ACK] timeout seq=");
  Serial.println(seq);
  return false;
}

bool secureSendControl(const String& json) {
  uint8_t plain[MAX_PLAIN_LEN];
  size_t plainLen = json.length();

  if (plainLen == 0 || plainLen > MAX_PLAIN_LEN) {
    Serial.print("[SEC_CTRL] invalid plaintext length: ");
    Serial.println((int)plainLen);
    return false;
  }

  memcpy(plain, json.c_str(), plainLen);

  bool delivered = false;
  uint32_t seq = gTxSeq;

  for (uint8_t attempt = 0; attempt < ACK_RETRY_MAX; ++attempt) {
    uint8_t frame[MAX_FRAME_LEN];
    size_t frameLen = 0;

    if (!buildSecureFrame(MSG_TYPE_CTRL, seq, plain, (uint16_t)plainLen, frame, frameLen)) {
      Serial.println("[SEC_CTRL] frame build failed");
      break;
    }

    if (!loraSendRaw(frame, frameLen)) {
      Serial.println("[SEC_CTRL] tx failed");
      continue;
    }

    Serial.print("[SEC CTRL TX] seq=");
    Serial.print(seq);
    Serial.print(" attempt=");
    Serial.println((int)(attempt + 1));

    if (waitForAck(seq, ACK_TIMEOUT_MS)) {
      delivered = true;
      Serial.print("[SEC CTRL ACK] seq=");
      Serial.println(seq);
      gTxSeq++;
      break;
    }

    Serial.print("[SEC CTRL ACK TIMEOUT] seq=");
    Serial.println(seq);
  }

  LoRa.receive();
  return delivered;
}
