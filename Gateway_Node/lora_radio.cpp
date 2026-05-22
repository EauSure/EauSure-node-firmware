#include "lora_radio.h"
#include "telemetry.h"
#include "otaa_manager.h"
#include "fuota_manager.h"
#include <ArduinoJson.h>
#include <cstring>

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
// Let the node finish TX→RX and process our DATA ACK before SET_CONFIG / FUOTA / SLEEP.
static const uint32_t NODE_POST_DATA_ACK_SETTLE_MS = 600;

static volatile bool cadDone     = false;
static volatile bool cadDetected = false;
static bool gCommandInFlight = false;

// Pending config
static bool gHasPendingConfig = false;
static float gPendingShakeThresholdG = 1.1f;
static bool gPendingShakeEnabled = true;

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
  uint32_t pairedNodeId = getPairedNodeDeviceId();

  if (pairedNodeId == 0) {
    Serial.println("[GW ACK WAIT] paired node ID unavailable");
    return false;
  }

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
    if (deviceId != pairedNodeId || rxSeq != seq) continue;

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
static bool secureCommand(uint8_t msgType, const uint8_t *plain, uint16_t plainLen,
                          uint32_t ackTimeoutMs = 8000) {
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

    if (waitForAck(msgType, seq, ackTimeoutMs)) {
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
    // Always advance seq so the next command (e.g. SLEEP after FUOTA_BEGIN) cannot
    // reuse a seq the node already accepted (replay reject / silent drop).
    gTxSeq++;
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
// sendSetConfig  (MSG_TYPE_SET_CONFIG = 0x08)
// Payload JSON: {"st":1.1,"se":1}
//   st = shakeThreshold (float, g)
//   se = shakeEnabled   (0/1)
// measureInterval and nodeActive are managed by the gateway,
// not forwarded to the node.
// =====================================================
bool sendSetConfig(float shakeThresholdG, bool shakeEnabled) {
  StaticJsonDocument<64> doc;
  doc["st"] = round(shakeThresholdG * 100.0f) / 100.0f;
  doc["se"] = shakeEnabled ? 1 : 0;

  String json;
  serializeJson(doc, json);

  Serial.printf("[GW] Sending SET_CONFIG: %s\n", json.c_str());

  uint8_t plain[64];
  size_t  plainLen = json.length();
  memcpy(plain, json.c_str(), plainLen);

  return secureCommand(MSG_TYPE_SET_CONFIG, plain, (uint16_t)plainLen);
}

// =====================================================
// Queued Configuration
// =====================================================
void queueSetConfig(float shakeThresholdG, bool shakeEnabled) {
  gPendingShakeThresholdG = shakeThresholdG;
  gPendingShakeEnabled = shakeEnabled;
  gHasPendingConfig = true;
  Serial.printf("[GW] Queued SET_CONFIG: st=%.2f se=%d (will send on next wake)\n", shakeThresholdG, shakeEnabled);
}

bool hasPendingConfig() {
  return gHasPendingConfig;
}

bool sendPendingConfig() {
  if (!gHasPendingConfig) return false;
  Serial.println("[GW] Node is awake. Transmitting queued SET_CONFIG...");
  bool ok = sendSetConfig(gPendingShakeThresholdG, gPendingShakeEnabled);
  if (ok) {
    gHasPendingConfig = false;
    Serial.println("[GW] Queued SET_CONFIG successfully delivered");
  } else {
    Serial.println("[GW] Queued SET_CONFIG delivery failed (will retry on next wake)");
  }
  return ok;
}

// =====================================================
// sendUnpair  (MSG_TYPE_UNPAIR = 0x09)
// No payload — tells node to erase pairing and reboot
// into pairing mode.
// =====================================================
bool sendUnpair() {
  Serial.println("[GW] Sending UNPAIR to IoT node...");
  return secureCommand(MSG_TYPE_UNPAIR, nullptr, 0);
}

// =====================================================
// sendSleepReq  (MSG_TYPE_SLEEP = 0x0A)
// Commands the node to deep sleep for X seconds
// =====================================================
bool sendSleepReq(uint32_t sleepDurationSec) {
  Serial.printf("[GW] Sending SLEEP command: %lu seconds\n", sleepDurationSec);
  uint8_t payload[4];
  writeU32BE(payload, sleepDurationSec);
  return secureCommand(MSG_TYPE_SLEEP, payload, 4);
}

bool sendFuotaBegin(uint32_t sessionId, uint32_t totalSize, uint16_t chunkSize, uint16_t totalChunks, const String& version, const String& md5) {
  StaticJsonDocument<384> doc;
  doc["sid"] = sessionId;
  doc["sz"] = totalSize;
  doc["cs"] = chunkSize;
  doc["tc"] = totalChunks;
  doc["v"] = version;
  doc["m"] = md5;

  String json;
  serializeJson(doc, json);
  Serial.printf("[GW] Sending FUOTA_BEGIN: %s\n", json.c_str());
  return secureCommand(MSG_TYPE_FUOTA_BEGIN, (const uint8_t*)json.c_str(), (uint16_t)json.length(), 12000);
}

bool sendFuotaChunk(uint32_t sessionId, uint32_t chunkIndex, const uint8_t* data, uint16_t len) {
  if (!data || len == 0 || len > (MAX_PLAIN_LEN - 10)) {
    return false;
  }

  uint8_t payload[MAX_PLAIN_LEN] = {0};
  writeU32BE(&payload[0], sessionId);
  writeU32BE(&payload[4], chunkIndex);
  writeU16BE(&payload[8], len);
  memcpy(&payload[10], data, len);

  Serial.printf("[GW] Sending FUOTA_CHUNK sid=%lu idx=%lu len=%u\n",
                (unsigned long)sessionId,
                (unsigned long)chunkIndex,
                (unsigned)len);
  return secureCommand(MSG_TYPE_FUOTA_CHUNK, payload, (uint16_t)(10 + len));
}

bool sendFuotaEnd(uint32_t sessionId, uint32_t totalSize, uint16_t totalChunks) {
  uint8_t payload[10];
  writeU32BE(&payload[0], sessionId);
  writeU32BE(&payload[4], totalSize);
  writeU16BE(&payload[8], totalChunks);
  Serial.printf("[GW] Sending FUOTA_END sid=%lu totalSize=%lu totalChunks=%u\n",
                (unsigned long)sessionId,
                (unsigned long)totalSize,
                (unsigned)totalChunks);
  return secureCommand(MSG_TYPE_FUOTA_END, payload, sizeof(payload));
}

bool sendFuotaCommit(uint32_t sessionId) {
  uint8_t payload[4];
  writeU32BE(&payload[0], sessionId);
  Serial.printf("[GW] Sending FUOTA_COMMIT sid=%lu\n", (unsigned long)sessionId);
  return secureCommand(MSG_TYPE_FUOTA_COMMIT, payload, sizeof(payload));
}

// =====================================================
// parseGenericFrame — shared header parser
// =====================================================
static bool parseGenericFrame(const uint8_t *frame, size_t frameLen,
                               uint8_t &typeOut, uint32_t &seqOut,
                               uint8_t *plainOut, uint16_t &plainLenOut) {
  uint32_t pairedNodeId = getPairedNodeDeviceId();
  if (pairedNodeId == 0) {
    Serial.println("[GW RX] paired node ID unavailable");
    return false;
  }

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
  if (deviceId != pairedNodeId) {
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
  delay(NODE_POST_DATA_ACK_SETTLE_MS);
  Serial.printf("[GW] Post-DATA-ACK settle %lu ms before downstream commands\n",
                (unsigned long)NODE_POST_DATA_ACK_SETTLE_MS);

  StaticJsonDocument<128> peek;
  const char *event = "MEASURE";
  if (deserializeJson(peek, plain) == DeserializationError::Ok) {
    event = peek["e"] | "MEASURE";
  }

  // SHAKE opens a follow-up MEASURE_REQ — do not SLEEP before that second window.
  if (strcmp(event, "SHAKE") == 0) {
    handleDataPayload((const char *)plain, rssi, snr);
    return true;
  }

  // ── MEASURE_RESP wake window: config → FUOTA → SLEEP → telemetry ──
  if (gHasPendingConfig) {
    delay(150);
    sendPendingConfig();
  }

  const bool fuotaConsumedWindow = FuotaManager::handleNodeDataWindow();

  delay(150);
  uint32_t sleepDurationSec = computeSleepDurationSec();
  if (FuotaManager::hasPendingNodeUpdate()) {
    if (!fuotaConsumedWindow) {
      // FUOTA did not finish — keep the node listening for the next gateway command.
      Serial.println("[FUOTA] Transfer incomplete — skipping SLEEP this wake window");
      notifyMeasureResponseHandled();
      onMeasureWakeWindowClosed();
      handleDataPayload((const char *)plain, rssi, snr);
      return true;
    }
    if (sleepDurationSec > 90) {
      sleepDurationSec = 90;
    }
    Serial.println("[FUOTA] Pending node update — capping SLEEP for earlier retry window");
  } else if (fuotaConsumedWindow && sleepDurationSec > 60) {
    sleepDurationSec = 60;
    Serial.println("[FUOTA] Partial transfer — short SLEEP before next wake window");
  }

  if (sendSleepReq(sleepDurationSec)) {
    notifyMeasureResponseHandled();
    onMeasureWakeWindowClosed();
  } else {
    Serial.println("[GW] SLEEP command failed — keeping wake window open for scheduler retry");
  }

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

  if (msgType == MSG_TYPE_ACK) {
    // Command ACKs are consumed by waitForAck() during secureCommand().
    return true;
  }

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
