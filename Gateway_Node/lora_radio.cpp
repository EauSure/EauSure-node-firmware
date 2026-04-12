#include "lora_radio.h"
#include "telemetry.h"
#include "otaa_manager.h"

// =====================================================
// CAD — Channel Activity Detection (wireless only)
//
// The DIO2 busy-wire scheme has been removed entirely.
// Both nodes are remote; there is no physical connection.
// Collision avoidance is handled purely by LoRa CAD +
// random backoff.
// =====================================================
static const uint32_t CAD_TIMEOUT_MS  = 15;
static const uint8_t  CAD_MAX_RETRIES = 8;

static volatile bool cadDone     = false;
static volatile bool cadDetected = false;
static bool gCommandInFlight = false;

// =====================================================
// ISR — DIO0 signals CadDone
// =====================================================
static void IRAM_ATTR onDio0Rise() {
  cadDone = true;
}

// =====================================================
// Direct SPI register write
// =====================================================
static void spiWriteReg(uint8_t reg, uint8_t value) {
  digitalWrite(LORA_NSS, LOW);
  SPI.transfer(reg | 0x80);
  SPI.transfer(value);
  digitalWrite(LORA_NSS, HIGH);
}

// =====================================================
// Direct SPI register read
// =====================================================
static uint8_t spiReadReg(uint8_t reg) {
  digitalWrite(LORA_NSS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t val = SPI.transfer(0x00);
  digitalWrite(LORA_NSS, HIGH);
  return val;
}

// =====================================================
// CAD
// =====================================================
static bool runCad() {
  cadDone     = false;
  cadDetected = false;

  LoRa.idle();
  delayMicroseconds(500);

  spiWriteReg(0x40, 0x00);
  spiWriteReg(0x01, 0x87);

  uint32_t start = millis();
  while (!cadDone && (millis() - start) < CAD_TIMEOUT_MS) {
    delayMicroseconds(100);
  }

  if (!cadDone) {
    Serial.println("[CAD] timeout (treated as clear)");
    LoRa.idle();
    return true;
  }

  // Read IRQ flags: bit2 = CadDetected
  uint8_t irq = spiReadReg(0x12);
  cadDetected = (irq & 0x04) != 0;
  spiWriteReg(0x12, 0xFF);   // clear IRQ flags

  LoRa.idle();
  return !cadDetected;
}

// =====================================================
// cadWaitAndSend — pure CAD-based collision avoidance
// =====================================================
static bool cadWaitAndSend(const uint8_t *data, size_t len) {
  for (uint8_t attempt = 0; attempt < CAD_MAX_RETRIES; attempt++) {
    if (!runCad()) {
      Serial.printf("[CAD] channel busy, retry %d/%d\n", attempt + 1, CAD_MAX_RETRIES);
      delay(random(5, 25));
      continue;
    }

    LoRa.idle();
    delay(2);
    LoRa.beginPacket();
    LoRa.write(data, len);

    // Blocking endPacket — waits for TxDone before returning.
    // Prevents the race where LoRa.receive() was called before
    // the packet was actually transmitted.
    bool ok = LoRa.endPacket(true);

    LoRa.receive();
    return ok;
  }

  LoRa.receive();
  Serial.println("[CAD] max retries — TX aborted");
  return false;
}

// =====================================================
// loraReadRaw — polling RX with timeout
// =====================================================
static bool loraReadRaw(uint8_t *buf, size_t cap, size_t &outLen, uint32_t timeoutMs) {
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    int pktSize = LoRa.parsePacket();
    if (pktSize > 0) {
      if ((size_t)pktSize > cap) {
        while (LoRa.available()) LoRa.read();
        LoRa.receive();
        return false;
      }
      size_t i = 0;
      while (LoRa.available() && i < cap) buf[i++] = (uint8_t)LoRa.read();
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
// waitForAck — waits for transport ACK from IoT node
//
// Slice raised to 150 ms (was 60 ms) to give the IoT node
// enough turnaround time: RX → CRC → GCM → CAD → TX.
//
// Non-ACK frames arriving during the wait are dispatched
// so they are not lost — this includes ACTIVATE_OK and
// HEARTBEAT_ACK which were previously silently dropped.
// =====================================================


bool isGatewayCommandInFlight() {
  return gCommandInFlight;
}
static bool waitForAck(uint8_t expectedCmdType, uint32_t seq, uint32_t timeoutMs = 8000) {
  uint8_t  buf[MAX_FRAME_LEN];
  size_t   len   = 0;
  uint32_t start = millis();

  while ((millis() - start) < timeoutMs) {
    uint32_t remaining = timeoutMs - (millis() - start);
    uint32_t slice     = (remaining > 300) ? 300 : remaining;

    if (!loraReadRaw(buf, sizeof(buf), len, slice)) continue;
    if (len < HEADER_LEN + GCM_TAG_LEN + CRC_LEN) continue;

    uint16_t rxCrc   = readU16BE(&buf[len - 2]);
    uint16_t calcCrc = crc16Ccitt(buf, len - 2);
    if (rxCrc != calcCrc) continue;

    if (buf[0] != PROTO_VERSION || buf[1] != MSG_TYPE_ACK) {
      // A non-ACK frame arrived while waiting for our ACK.
      //
      // ACTIVATE_OK/HEARTBEAT_ACK are implicit delivery confirmations:
      // the IoT only sends them after successfully processing our command,
      // so receiving them means our command was delivered. Dispatch and
      // return true immediately — no separate ACK frame will follow.
      //
      // DATA frames are dispatched and we keep waiting (the IoT does send
      // a separate ACK for those via the normal secureSend path).
      int   rssi = LoRa.packetRssi();
      float snr  = LoRa.packetSnr();
            switch (buf[1]) {
        case MSG_TYPE_ACTIVATE_OK:
          if (expectedCmdType == MSG_TYPE_ACTIVATE) {
            Serial.println("[GW ACK WAIT] ACTIVATE_OK received — implicit ACK for ACTIVATE");
            parseAndDispatchTypedFrame(buf, len, rssi, snr);
            return true;
          }
          Serial.println("[GW ACK WAIT] ACTIVATE_OK received out of context — dispatched only");
          parseAndDispatchTypedFrame(buf, len, rssi, snr);
          break;

        case MSG_TYPE_HEARTBEAT_ACK:
          if (expectedCmdType == MSG_TYPE_HEARTBEAT_REQ) {
            Serial.println("[GW ACK WAIT] HEARTBEAT_ACK received — implicit ACK for HEARTBEAT_REQ");
            parseAndDispatchTypedFrame(buf, len, rssi, snr);
            return true;
          }
          Serial.println("[GW ACK WAIT] HEARTBEAT_ACK received out of context — dispatched only");
          parseAndDispatchTypedFrame(buf, len, rssi, snr);
          break;

        case MSG_TYPE_DATA:
          parseAndDispatchDataFrame(buf, len, rssi, snr);
          break;

        default:
          Serial.printf("[GW ACK WAIT] unexpected type 0x%02X — ignored\n", buf[1]);
          break;
      }
      continue;
    }

    uint32_t deviceId = readU32BE(&buf[2]);
    uint32_t rxSeq    = readU32BE(&buf[6]);
    if (deviceId != DEVICE_ID || rxSeq != seq) continue;

    uint16_t cipherLen = readU16BE(&buf[10 + GCM_NONCE_LEN]);
    if (cipherLen != 0) continue;
    if (len != HEADER_LEN + GCM_TAG_LEN + CRC_LEN) continue;

    const uint8_t *nonce = &buf[10];
    uint8_t dummy[1] = {0};
    if (aesgcmDecrypt(buf + HEADER_LEN, 0, nonce, buf, HEADER_LEN,
                      buf + HEADER_LEN, dummy)) {
      return true;
    }
  }

  Serial.printf("[GW ACK WAIT] timeout seq=%lu\n", (unsigned long)seq);
  return false;
}

// =====================================================
// secureCommand — internal: build + CAD-send + wait ACK
// =====================================================
static bool secureCommand(uint8_t msgType, const uint8_t *plain, uint16_t plainLen) {
  if (gCommandInFlight) {
    Serial.println("[GW CMD] another command is already in flight");
    return false;
  }

  gCommandInFlight = true;

  bool success = false;
  uint32_t seq = gTxSeq;

  for (uint8_t attempt = 0; attempt < ACK_RETRY_MAX; attempt++) {
    uint8_t frame[MAX_FRAME_LEN];
    size_t  frameLen = 0;

    if (!buildSecureFrame(msgType, seq, plain, plainLen, frame, frameLen)) {
      Serial.println("[GW CMD] frame build failed");
      break;
    }

    if (!cadWaitAndSend(frame, frameLen)) {
      Serial.println("[GW CMD] cadWaitAndSend failed");
      continue;
    }

    Serial.printf("[GW CMD TX] type=0x%02X seq=%lu attempt=%d\n",
                  msgType, (unsigned long)seq, attempt + 1);

    if (waitForAck(msgType, seq)) {
      gTxSeq++;
      Serial.printf("[GW CMD ACK] type=0x%02X seq=%lu confirmed\n",
                    msgType, (unsigned long)seq);
      success = true;
      break;
    }
  }

  if (!success) {
    Serial.printf("[GW CMD] delivery failed after %d attempts (type=0x%02X)\n",
                  ACK_RETRY_MAX, msgType);
    LoRa.receive();
  }

  gCommandInFlight = false;
  return success;
}

// =====================================================
// sendAck — transport ACK back to IoT node
// =====================================================
bool sendAck(uint32_t seq) {
  uint8_t frame[MAX_FRAME_LEN];
  size_t  frameLen = 0;

  if (!buildSecureFrame(MSG_TYPE_ACK, seq, nullptr, 0, frame, frameLen)) {
    Serial.println("[GW ACK] build failed");
    return false;
  }

  bool ok = cadWaitAndSend(frame, frameLen);
  Serial.printf("[GW ACK] seq=%lu sent=%s\n", (unsigned long)seq, ok ? "yes" : "no");
  return ok;
}

// =====================================================
// sendActivate  (MSG_TYPE_ACTIVATE = 0x06)
// =====================================================
bool sendActivate() {
  Serial.println("[GW] Sending ACTIVATE to IoT node...");
  return secureCommand(MSG_TYPE_ACTIVATE, nullptr, 0);
}

// =====================================================
// sendMeasureReq  (MSG_TYPE_MEASURE_REQ = 0x03)
// =====================================================
bool sendMeasureReq() {
  Serial.println("[GW] Sending MEASURE_REQ...");
  return secureCommand(MSG_TYPE_MEASURE_REQ, nullptr, 0);
}

// =====================================================
// sendHeartbeatReq  (MSG_TYPE_HEARTBEAT_REQ = 0x04)
// =====================================================
bool sendHeartbeatReq() {
  Serial.println("[GW] Sending HEARTBEAT_REQ...");
  return secureCommand(MSG_TYPE_HEARTBEAT_REQ, nullptr, 0);
}

// =====================================================
// parseGenericFrame — shared header parser
// =====================================================
static bool parseGenericFrame(const uint8_t *frame, size_t frameLen,
                               uint8_t &typeOut, uint32_t &seqOut,
                               uint8_t *plainOut, uint16_t &plainLenOut) {
  if (frameLen < HEADER_LEN + GCM_TAG_LEN + CRC_LEN) {
    Serial.println("[GW RX] frame too short");
    return false;
  }

  uint16_t rxCrc   = readU16BE(&frame[frameLen - 2]);
  uint16_t calcCrc = crc16Ccitt(frame, frameLen - 2);
  if (rxCrc != calcCrc) { Serial.println("[GW RX] CRC mismatch"); return false; }

  uint8_t  version  = frame[0];
  uint8_t  msgType  = frame[1];
  uint32_t deviceId = readU32BE(&frame[2]);
  uint32_t seq      = readU32BE(&frame[6]);
  const uint8_t *nonce = &frame[10];
  uint16_t cipherLen   = readU16BE(&frame[10 + GCM_NONCE_LEN]);

  if (version != PROTO_VERSION) { Serial.println("[GW RX] version mismatch"); return false; }
  if (deviceId != DEVICE_ID) {
    Serial.printf("[GW RX] deviceId mismatch: 0x%08lX\n", (unsigned long)deviceId);
    return false;
  }

  size_t expectedLen = HEADER_LEN + cipherLen + GCM_TAG_LEN + CRC_LEN;
  if (frameLen != expectedLen || cipherLen > MAX_PLAIN_LEN) {
    Serial.println("[GW RX] length mismatch"); return false;
  }

  size_t tagPos = HEADER_LEN + cipherLen;
  if (!aesgcmDecrypt(&frame[HEADER_LEN], cipherLen, nonce,
                     frame, HEADER_LEN, &frame[tagPos], plainOut)) {
    Serial.println("[GW RX] GCM auth failed"); return false;
  }

  plainOut[cipherLen] = '\0';
  typeOut     = msgType;
  seqOut      = seq;
  plainLenOut = cipherLen;
  return true;
}

// =====================================================
// parseAndDispatchDataFrame  (MSG_TYPE_DATA = 0x01)
// =====================================================
bool parseAndDispatchDataFrame(const uint8_t *frame, size_t frameLen,
                                int rssi, float snr) {
  uint8_t  msgType  = 0;
  uint32_t seq      = 0;
  uint8_t  plain[MAX_PLAIN_LEN + 1] = {0};
  uint16_t plainLen = 0;

  if (!parseGenericFrame(frame, frameLen, msgType, seq, plain, plainLen)) return false;
  if (msgType != MSG_TYPE_DATA) return false;

  if (seq < lastAcceptedSeq) {
    Serial.printf("[GW RX] DATA old seq %lu rejected\n", (unsigned long)seq);
    sendAck(seq);
    return true;
  }
  if (seq == lastAcceptedSeq) {
    Serial.printf("[GW RX] DATA dup seq=%lu — re-ACK\n", (unsigned long)seq);
    sendAck(seq);
    return true;
  }

  lastAcceptedSeq = seq;

  // ACK FIRST — before JSON parse, WiFi, or audio
  sendAck(seq);

  handleDataPayload((const char *)plain, rssi, snr);
  return true;
}

// =====================================================
// parseAndDispatchTypedFrame  (ACTIVATE_OK, HB_ACK)
// =====================================================
bool parseAndDispatchTypedFrame(const uint8_t *frame, size_t frameLen,
                                 int rssi, float snr) {
  uint8_t  msgType  = 0;
  uint32_t seq      = 0;
  uint8_t  plain[MAX_PLAIN_LEN + 1] = {0};
  uint16_t plainLen = 0;

  if (!parseGenericFrame(frame, frameLen, msgType, seq, plain, plainLen)) return false;

  sendAck(seq);

  switch (msgType) {
    case MSG_TYPE_ACTIVATE_OK:
      handleActivateOk((const char *)plain, rssi, snr);
      break;
    case MSG_TYPE_HEARTBEAT_ACK:
      handleHeartbeatAck((const char *)plain, rssi, snr);
      break;
    default:
      Serial.printf("[GW RX] unhandled type 0x%02X\n", msgType);
      break;
  }

  return true;
}

// =====================================================
// initLoRa
// =====================================================
bool initLoRa() {
  // No busy-pin setup — pure wireless CAD only.
  pinMode(LORA_NSS, OUTPUT);
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_NSS, HIGH);

  pinMode(LORA_DIO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(LORA_DIO0), onDio0Rise, RISING);

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
  Serial.println("[LoRa] CAD-only collision avoidance (no busy wire)");
  return true;
}