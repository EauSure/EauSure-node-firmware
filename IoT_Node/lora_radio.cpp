#include "lora_radio.h"
#include "app_state.h"

#include "mbedtls/aes.h"
#include "mbedtls/md.h"

static const uint8_t MSG_TYPE_DATA = 0x01;
static const uint8_t MSG_TYPE_ACK  = 0x02;

static const size_t IV_LEN         = 8;
static const size_t HMAC_TRUNC_LEN = 8;
static const size_t CRC_LEN        = 2;
static const size_t HEADER_LEN     = 1 + 1 + 4 + 4 + 8 + 2; // ver,type,device,seq,iv,payload_len
static const size_t MAX_PLAIN_LEN  = 180;
static const size_t MAX_FRAME_LEN  = HEADER_LEN + MAX_PLAIN_LEN + HMAC_TRUNC_LEN + CRC_LEN;

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

static void buildIv(uint32_t seq, uint8_t iv[IV_LEN]) {
  writeU32BE(&iv[0], DEVICE_ID);
  writeU32BE(&iv[4], seq);
}

static bool aesCtrCrypt(const uint8_t *input, size_t len, const uint8_t iv[IV_LEN], uint8_t *output) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);

  if (mbedtls_aes_setkey_enc(&aes, ENC_KEY, 128) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }

  uint8_t nonce_counter[16] = {0};
  uint8_t stream_block[16] = {0};
  size_t nc_off = 0;

  memcpy(nonce_counter, iv, IV_LEN);

  int rc = mbedtls_aes_crypt_ctr(
    &aes,
    len,
    &nc_off,
    nonce_counter,
    stream_block,
    input,
    output
  );

  mbedtls_aes_free(&aes);
  return rc == 0;
}

static bool computeHmac8(const uint8_t *data, size_t len, uint8_t out8[HMAC_TRUNC_LEN]) {
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md_info) return false;

  uint8_t full[32] = {0};

  int rc = mbedtls_md_hmac(
    md_info,
    HMAC_KEY, sizeof(HMAC_KEY),
    data, len,
    full
  );
  if (rc != 0) return false;

  memcpy(out8, full, HMAC_TRUNC_LEN);
  return true;
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

  uint8_t iv[IV_LEN];
  buildIv(seq, iv);

  size_t pos = 0;
  outFrame[pos++] = PROTO_VERSION;
  outFrame[pos++] = msgType;
  writeU32BE(&outFrame[pos], DEVICE_ID); pos += 4;
  writeU32BE(&outFrame[pos], seq);       pos += 4;
  memcpy(&outFrame[pos], iv, IV_LEN);    pos += IV_LEN;
  writeU16BE(&outFrame[pos], plainLen);  pos += 2;

  if (plainLen > 0) {
    if (!aesCtrCrypt(plain, plainLen, iv, &outFrame[pos])) return false;
    pos += plainLen;
  }

  uint8_t tag[HMAC_TRUNC_LEN];
  if (!computeHmac8(outFrame, pos, tag)) return false;
  memcpy(&outFrame[pos], tag, HMAC_TRUNC_LEN);
  pos += HMAC_TRUNC_LEN;

  uint16_t crc = crc16Ccitt(outFrame, pos);
  writeU16BE(&outFrame[pos], crc);
  pos += 2;

  outLen = pos;
  return true;
}

static bool verifySecureFrame(
  const uint8_t *frame,
  size_t frameLen,
  uint8_t expectedType,
  uint32_t expectedSeq
) {
  if (frameLen < HEADER_LEN + HMAC_TRUNC_LEN + CRC_LEN) return false;

  uint16_t rxCrc = readU16BE(&frame[frameLen - 2]);
  uint16_t calcCrc = crc16Ccitt(frame, frameLen - 2);
  if (rxCrc != calcCrc) return false;

  if (frame[0] != PROTO_VERSION) return false;
  if (frame[1] != expectedType)  return false;

  uint32_t devId = readU32BE(&frame[2]);
  uint32_t seq   = readU32BE(&frame[6]);

  if (devId != DEVICE_ID) return false;
  if (seq != expectedSeq) return false;

  size_t signedLen = frameLen - 2 - HMAC_TRUNC_LEN;
  const uint8_t *rxTag = &frame[signedLen];

  uint8_t calcTag[HMAC_TRUNC_LEN];
  if (!computeHmac8(frame, signedLen, calcTag)) return false;

  if (memcmp(rxTag, calcTag, HMAC_TRUNC_LEN) != 0) return false;

  return true;
}

static bool waitForAck(uint32_t seq, uint32_t timeoutMs) {
  uint8_t buf[MAX_FRAME_LEN];
  size_t len = 0;

  if (!loraReadRaw(buf, sizeof(buf), len, timeoutMs)) {
    Serial.print("[SEC WAIT ACK] timeout seq=");
    Serial.println(seq);
    return false;
  }

  Serial.print("[SEC WAIT ACK] got packet len=");
  Serial.print((int)len);
  Serial.print(" seq=");
  Serial.println(seq);

  return verifySecureFrame(buf, len, MSG_TYPE_ACK, seq);
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