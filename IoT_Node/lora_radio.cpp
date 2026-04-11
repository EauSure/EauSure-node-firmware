#include "lora_radio.h"
#include "app_state.h"

#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include <esp_sleep.h>

static const uint8_t MSG_TYPE_DATA = 0x01;
static const uint8_t MSG_TYPE_ACK  = 0x02;
static const uint8_t MSG_TYPE_CTRL = 0x03;

static const size_t GCM_NONCE_LEN  = 12;  // GCM nonce (IV) - 96-bit is standard
static const size_t GCM_TAG_LEN    = 16;  // GCM authentication tag - full 128-bit
static const size_t CRC_LEN        = 2;
static const size_t HEADER_LEN     = 1 + 1 + 4 + 4 + GCM_NONCE_LEN + 2; // ver,type,device,seq,nonce,payload_len
static const size_t MAX_PLAIN_LEN  = 180;
static const size_t MAX_FRAME_LEN  = HEADER_LEN + MAX_PLAIN_LEN + GCM_TAG_LEN + CRC_LEN;

static void handleOtaaControl(const char* json, size_t len, uint32_t seq);

static void writeU16BE(uint8_t *dst, uint16_t v) {
  dst[0] = (uint8_t)((v >> 8) & 0xFF);
  dst[1] = (uint8_t)(v & 0xFF);
}

static void writeU32BE(uint8_t *dst, uint32_t v) {
  dst[0] = (uint8_t)((v >> 24) & 0xFF);
  dst[1] = (uint8_t)((v >> 16) & 0xFF);
  dst[2] = (uint8_t)((v >>  8) & 0xFF);
  dst[3] = (uint8_t)(v & 0xFF);
}

static uint16_t readU16BE(const uint8_t *src) {
  return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

static uint32_t readU32BE(const uint8_t *src) {
  return ((uint32_t)src[0] << 24) |
         ((uint32_t)src[1] << 16) |
         ((uint32_t)src[2] <<  8) |
         ((uint32_t)src[3]);
}

static uint16_t crc16Ccitt(const uint8_t *data, size_t len) {
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

static void buildNonce(uint32_t seq, uint8_t nonce[GCM_NONCE_LEN]) {
  // Nonce: [DEVICE_ID (4B)] [Sequence (4B)] [Random (4B)]
  writeU32BE(&nonce[0], DEVICE_ID);
  writeU32BE(&nonce[4], seq);
  // Last 4 bytes: random counter for uniqueness
  nonce[8]  = random(0, 256);
  nonce[9]  = random(0, 256);
  nonce[10] = random(0, 256);
  nonce[11] = random(0, 256);
}

static bool aesgcmEncrypt(
  const uint8_t *plain,
  size_t plainLen,
  const uint8_t nonce[GCM_NONCE_LEN],
  const uint8_t *aad,
  size_t aadLen,
  uint8_t *cipher,
  uint8_t tag[GCM_TAG_LEN]
) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, ENC_KEY, 128) != 0) {
    mbedtls_gcm_free(&gcm);
    return false;
  }

  // mbedTLS doesn't like nullptr for plaintext/ciphertext even with length 0
  // Use dummy buffer for empty payloads
  uint8_t dummy[1] = {0};
  const uint8_t *plainPtr = (plainLen > 0) ? plain : dummy;
  uint8_t *cipherPtr = (plainLen > 0) ? cipher : dummy;

  int rc = mbedtls_gcm_crypt_and_tag(
    &gcm,
    MBEDTLS_GCM_ENCRYPT,
    plainLen,
    nonce, GCM_NONCE_LEN,
    aad, aadLen,
    plainPtr,
    cipherPtr,
    GCM_TAG_LEN, tag
  );

  mbedtls_gcm_free(&gcm);
  return rc == 0;
}

static bool aesgcmDecrypt(
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

static bool loraSendRaw(const uint8_t *data, size_t len) {
  LoRa.idle();
  vTaskDelay(pdMS_TO_TICKS(5));
  LoRa.beginPacket();
  LoRa.write(data, len);
  bool ok = LoRa.endPacket();
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
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  LoRa.receive();
  return false;
}

static bool buildSecureFrame(
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

  Serial.print("[GCM SEND] Building frame: ver=");
  Serial.print(PROTO_VERSION);
  Serial.print(" type=");
  Serial.print(msgType);
  Serial.print(" seq=");
  Serial.print(seq);
  Serial.print(" plainLen=");
  Serial.print(plainLen);
  Serial.print(" aadLen=");
  Serial.print(pos);
  Serial.println();

  // Encrypt payload using GCM with header (including payload len) as Additional Authenticated Data (AAD)
  uint8_t *ciphertext = &outFrame[pos];
  uint8_t tag[GCM_TAG_LEN];

  // AAD includes everything up to and including the payload length field
  size_t aadLen = pos;  // pos = HEADER_LEN at this point

  if (plainLen > 0) {
    if (!aesgcmEncrypt(plain, plainLen, nonce, outFrame, aadLen, ciphertext, tag)) {
      Serial.println("[GCM] Encryption failed");
      return false;
    }
  } else {
    // For empty payload, still generate tag
    if (!aesgcmEncrypt(nullptr, 0, nonce, outFrame, aadLen, nullptr, tag)) {
      Serial.println("[GCM] Encryption failed (empty payload)");
      return false;
    }
  }

  Serial.print("[GCM] Encrypted ");
  Serial.print(plainLen);
  Serial.print(" bytes, tag: ");
  for (int i = 0; i < 4; i++) {
    Serial.print(tag[i], HEX);
    Serial.print(" ");
  }
  Serial.println("...");

  pos += plainLen;

  // Append authentication tag
  memcpy(&outFrame[pos], tag, GCM_TAG_LEN);
  pos += GCM_TAG_LEN;

  // Append CRC-16 for physical layer error detection
  uint16_t crc = crc16Ccitt(outFrame, pos);
  writeU16BE(&outFrame[pos], crc);
  pos += 2;

  outLen = pos;
  return true;
}

static bool parseSecureFrame(
  const uint8_t *frame,
  size_t frameLen,
  uint8_t expectedType,
  uint32_t &seqOut,
  uint8_t *outPlain,
  size_t &outPlainLen
) {
  if (frameLen < HEADER_LEN + GCM_TAG_LEN + CRC_LEN) {
    Serial.print("[GCM] Frame too short: ");
    Serial.println(frameLen);
    return false;
  }

  uint16_t rxCrc = readU16BE(&frame[frameLen - 2]);
  uint16_t calcCrc = crc16Ccitt(frame, frameLen - 2);
  if (rxCrc != calcCrc) {
    Serial.println("[GCM] CRC mismatch");
    return false;
  }

  if (frame[0] != PROTO_VERSION) {
    Serial.println("[GCM] Version mismatch");
    return false;
  }
  if (frame[1] != expectedType) {
    Serial.println("[GCM] Type mismatch");
    return false;
  }

  uint32_t devId = readU32BE(&frame[2]);
  uint32_t seq   = readU32BE(&frame[6]);

  if (devId != DEVICE_ID) {
    Serial.print("[GCM] Device ID mismatch: got 0x");
    Serial.print(devId, HEX);
    Serial.print(" expected 0x");
    Serial.println(DEVICE_ID, HEX);
    return false;
  }

  const uint8_t *nonce = &frame[10];
  uint16_t cipherLen = readU16BE(&frame[10 + GCM_NONCE_LEN]);

  size_t cipherPos = HEADER_LEN;
  size_t tagPos = cipherPos + cipherLen;
  size_t crcPos = tagPos + GCM_TAG_LEN;

  if (crcPos + 2 != frameLen) {
    Serial.println("[GCM] Frame structure mismatch");
    return false;
  }

  if (cipherLen > MAX_PLAIN_LEN) {
    Serial.println("[GCM] Payload too large");
    return false;
  }

  const uint8_t *cipher = &frame[cipherPos];
  const uint8_t *tag = &frame[tagPos];

  if (!aesgcmDecrypt(cipher, cipherLen, nonce, frame, HEADER_LEN, tag, outPlain)) {
    Serial.println("[GCM] Authentication failed - corrupted or invalid packet");
    return false;
  }

  seqOut = seq;
  outPlainLen = cipherLen;
  return true;
}

static bool verifySecureFrame(
  const uint8_t *frame,
  size_t frameLen,
  uint8_t expectedType,
  uint32_t expectedSeq,
  uint8_t *outPlain,
  size_t &outPlainLen
) {
  uint32_t seq = 0;
  if (!parseSecureFrame(frame, frameLen, expectedType, seq, outPlain, outPlainLen)) {
    return false;
  }
  if (seq != expectedSeq) {
    Serial.print("[GCM] Sequence mismatch: got ");
    Serial.print(seq);
    Serial.print(" expected ");
    Serial.println(expectedSeq);
    return false;
  }
  return true;
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

    Serial.print("[GCM WAIT ACK] got packet len=");
    Serial.print((int)len);
    Serial.print(" seq=");
    Serial.println(seq);

    uint8_t plain[32];
    size_t plainLen = 0;
    if (verifySecureFrame(buf, len, MSG_TYPE_ACK, seq, plain, plainLen)) {
      return true;
    }
  }

  Serial.print("[GCM WAIT ACK] timeout seq=");
  Serial.println(seq);
  return false;
}

bool loraInit() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] init failed");
    return false;
  }

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc(); // hardware CRC still useful for PHY corruption detection
  LoRa.setTxPower(17);
  LoRa.receive();

  Serial.println("[LoRa] init ok");
  return true;
}


// keep only for temporary debug/manual plaintext tests
bool loraSendText(const String& payload) {
  if (gLoRaMutex && xSemaphoreTake(gLoRaMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false;
  }

  bool ok = loraSendRaw((const uint8_t*)payload.c_str(), payload.length());

  if (gLoRaMutex) xSemaphoreGive(gLoRaMutex);

  Serial.print("[LoRa TX PLAINTEXT] ");
  Serial.println(payload);
  return ok;
}

bool secureSendJson(const String& json) {
  uint8_t plain[MAX_PLAIN_LEN];
  size_t plainLen = json.length();

  Serial.print("[SEC_SEND] Received JSON of length: ");
  Serial.println((int)plainLen);
  Serial.print("[SEC_SEND] First 100 chars: ");
  if (plainLen > 100) {
    Serial.println(json.substring(0, 100));
  } else {
    Serial.println(json);
  }

  if (plainLen == 0 || plainLen > MAX_PLAIN_LEN) {
    Serial.print("[SEC] invalid plaintext length: ");
    Serial.print((int)plainLen);
    Serial.print(" (max: ");
    Serial.print((int)MAX_PLAIN_LEN);
    Serial.println(")");
    return false;
  }

  memcpy(plain, json.c_str(), plainLen);

  if (gLoRaMutex && xSemaphoreTake(gLoRaMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    Serial.println("[SEC_SEND] ERROR: Could not acquire LoRa mutex");
    return false;
  }

  bool delivered = false;
  uint32_t seq = gTxSeq;

  for (uint8_t attempt = 0; attempt < ACK_RETRY_MAX; ++attempt) {
    uint8_t frame[MAX_FRAME_LEN];
    size_t frameLen = 0;

    if (!buildSecureFrame(MSG_TYPE_DATA, seq, plain, (uint16_t)plainLen, frame, frameLen)) {
      Serial.println("[SEC] frame build failed");
      break;
    }

    if (!loraSendRaw(frame, frameLen)) {
      Serial.println("[SEC] tx failed");
      continue;
    }

    Serial.print("[SEC TX] seq=");
    Serial.print(seq);
    Serial.print(" attempt=");
    Serial.println((int)(attempt + 1));

    if (waitForAck(seq, ACK_TIMEOUT_MS)) {
      delivered = true;
      Serial.print("[SEC ACK] seq=");
      Serial.println(seq);
      gTxSeq++;
      break;
    }

    Serial.print("[SEC ACK TIMEOUT] seq=");
    Serial.println(seq);
  }

  LoRa.receive();
  if (gLoRaMutex) xSemaphoreGive(gLoRaMutex);
  return delivered;
}

static bool sendSecureAck(uint32_t seq) {
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

bool secureSendControl(const String& json) {
  uint8_t plain[MAX_PLAIN_LEN];
  size_t plainLen = json.length();

  if (plainLen == 0 || plainLen > MAX_PLAIN_LEN) {
    Serial.print("[SEC_CTRL] invalid plaintext length: ");
    Serial.println((int)plainLen);
    return false;
  }

  memcpy(plain, json.c_str(), plainLen);

  if (gLoRaMutex && xSemaphoreTake(gLoRaMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    Serial.println("[SEC_CTRL] ERROR: Could not acquire LoRa mutex");
    return false;
  }

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
  if (gLoRaMutex) xSemaphoreGive(gLoRaMutex);
  return delivered;
}

void pollControlFrames(uint32_t timeoutMs) {
  if (gLoRaMutex && xSemaphoreTake(gLoRaMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }

  uint8_t buf[MAX_FRAME_LEN];
  size_t len = 0;
  if (!loraReadRaw(buf, sizeof(buf), len, timeoutMs)) {
    if (gLoRaMutex) xSemaphoreGive(gLoRaMutex);
    return;
  }

  uint8_t plain[MAX_PLAIN_LEN + 1] = {0};
  size_t plainLen = 0;
  uint32_t seq = 0;
  if (!parseSecureFrame(buf, len, MSG_TYPE_CTRL, seq, plain, plainLen)) {
    if (gLoRaMutex) xSemaphoreGive(gLoRaMutex);
    return;
  }

  sendSecureAck(seq);
  plain[plainLen] = '\0';
  if (gLoRaMutex) xSemaphoreGive(gLoRaMutex);

  handleOtaaControl((const char*)plain, plainLen, seq);
}

static String getNodeMacId() {
  uint64_t chip = ESP.getEfuseMac();
  char mac[13];
  snprintf(mac, sizeof(mac), "%04X%08lX", (uint16_t)(chip >> 32), (uint32_t)chip);
  return String(mac);
}

static void sendNodeStatus(const char *evt, uint32_t seq) {
  JsonDocument doc;
  doc["evt"] = evt;
  doc["state"] = gNodeActive ? "active" : "sleep";
  doc["mac"] = getNodeMacId();
  doc["uptime"] = millis();
  doc["seq"] = seq;
  doc["tx_seq"] = gTxSeq;

  String out;
  serializeJson(doc, out);
  secureSendControl(out);
}

static void handleOtaaControl(const char* json, size_t len, uint32_t seq) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) {
    Serial.print("[OTAA] Control parse error: ");
    Serial.println(err.c_str());
    return;
  }

  String cmd = doc["cmd"] | "";
  if (cmd.length() == 0) {
    Serial.println("[OTAA] Control frame missing cmd");
    return;
  }

  if (cmd == "PAIR_REQ") {
    gNodeActive = true;
    sendNodeStatus("PAIR_OK", seq);
    Serial.println("[OTAA] PAIR_REQ handled -> PAIR_OK");
    return;
  }

  if (cmd == "HEARTBEAT_REQ") {
    sendNodeStatus("HEARTBEAT", seq);
    Serial.println("[OTAA] HEARTBEAT sent");
    return;
  }

  if (cmd == "STATUS_REQ") {
    sendNodeStatus("STATUS", seq);
    Serial.println("[OTAA] STATUS sent");
    return;
  }

  if (cmd == "SET_ACTIVE") {
    int value = doc["value"] | 1;
    gNodeActive = (value != 0);
    sendNodeStatus("STATUS", seq);
    Serial.printf("[OTAA] SET_ACTIVE value=%d\n", value);
    return;
  }

  if (cmd == "SET_SLEEP") {
    uint32_t sleepSeconds = doc["seconds"] | 120;
    gNodeActive = false;
    sendNodeStatus("STATUS", seq);
    Serial.printf("[OTAA] Entering deep sleep for %lu s\n", (unsigned long)sleepSeconds);
    delay(100);
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
    return;
  }

  Serial.print("[OTAA] Unknown control cmd: ");
  Serial.println(cmd);
}

void sendJsonData() {
  Serial.println("[SEND_JSON] Starting JSON creation");
  
  StaticJsonDocument<512> doc;

  // Optimized field names (shorter) to fit within 180-byte limit
  doc["b"] = gSensorData.battPercent;
  doc["v"] = round(gSensorData.battLoadV * 100.0) / 100.0;
  doc["m"] = round(gSensorData.battCurrentmA);

  doc["p"]  = round(gSensorData.lastPhValue * 100.0) / 100.0;
  doc["ps"] = gSensorData.phScale10;

  doc["t"]  = round(gSensorData.lastTdsValue);
  doc["ts"] = gSensorData.tdsScale10;

  doc["u"]  = round(gSensorData.lastTurbSensorVoltage * 100.0) / 100.0;
  doc["us"] = gSensorData.turbScale10;

  doc["tw"] = round(gSensorData.waterTempC * 10.0) / 10.0;
  doc["tm"] = round(gSensorData.mpuTempC * 10.0) / 10.0;
  doc["te"] = round(gSensorData.espTempC * 10.0) / 10.0;
  doc["e"]  = gEventState.lastEvent;

  String jsonString;
  size_t written = serializeJson(doc, jsonString);
  
  Serial.print("[SEND_JSON] Serialized ");
  Serial.print((int)written);
  Serial.print(" bytes, JSON length: ");
  Serial.println((int)jsonString.length());
  Serial.print("[SEND_JSON] JSON: ");
  Serial.println(jsonString);

  if (jsonString.length() == 0) {
    Serial.println("[SEND_JSON] ERROR: JSON serialization produced empty string");
    return;
  }

  Serial.print("[SEND_JSON] Calling secureSendJson with ");
  Serial.print((int)jsonString.length());
  Serial.println(" bytes");
  
  if (!secureSendJson(jsonString)) {
    Serial.println("[SEC] delivery failed after retries");
  } else {
    Serial.println("[SEND_JSON] JSON delivery successful");
  }
}
void handleCommand(const String &json) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return;

  String cmd = doc["cmd"] | "";

  if (cmd == "PING") {
    secureSendJson("{\"resp\":\"PONG\"}");
  }

  else if (cmd == "READ_NOW") {
    secureSendJson("{\"resp\":\"READING\"}");
    // trigger sensors manually
  }

  else if (cmd == "STOP_ALARM") {
    Serial.println("Stopping alarm");
    // stop buzzer / alert
  }

  else if (cmd == "SET_MODE") {
    String mode = doc["value"] | "auto";
    Serial.println("Mode set to: " + mode);
  }
}
