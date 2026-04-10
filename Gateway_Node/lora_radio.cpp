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
