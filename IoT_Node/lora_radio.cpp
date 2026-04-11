#include "lora_radio.h"
#include "app_state.h"
#include "esp_mac.h"

// =====================================================
// DIO pin roles (SX1276 DIO mapping we configure)
//
//  DIO0 = CadDone    (RegDioMapping1 bits 7:6 = 10)
//  DIO1 = TxDone     (RegDioMapping1 bits 5:4 = 01 in TX / 00 in RX gives RxTimeout, but
//                     we use it only as a TX completion signal — see setDioMapping())
//  DIO2 = RxDone     (RegDioMapping1 bits 3:2 = 00)
//
//  This frees DIO0 exclusively for CAD so the ISR never
//  races with TxDone or RxDone signals.
//
//  SX1276 RegDioMapping1 (0x40) bit layout:
//    [7:6] DIO0   [5:4] DIO1   [3:2] DIO2   [1:0] DIO3
//
//  For TX mode  we write 0x40:  DIO0=CadDone(10) DIO1=TxDone(00) DIO2=FhssChangeChannel(00)
//    Wait — in LoRa mode the DIO1 mapping for TxDone is actually bits[5:4]=01
//    Full table (LoRa mode):
//      DIO0: 00=RxDone  01=TxDone  10=CadDone
//      DIO1: 00=RxTimeout  01=FhssChangeChannel  10=CadDetected
//      DIO2: 00=FhssChangeChannel  01=CadDetected  10=--
//
//  So to get DIO1=TxDone we cannot — TxDone is only routable to DIO0.
//  We keep DIO0 for TxDone (library default) AND for CadDone.
//  The trick: we SWITCH the mapping:
//    - During CAD:    DIO0 = CadDone  (0x40 bits[7:6]=10  → 0x80)
//    - During TX/RX:  DIO0 = TxDone/RxDone (0x40 bits[7:6]=00 → 0x00, library default)
//
//  DIO1 → RxTimeout  (useful for loraReadRaw future use, no ISR needed now)
//  DIO2 → RxDone ISR (bits[3:2] not directly available for RxDone in SX1276 —
//                     RxDone is only on DIO0; DIO2 max = CadDetected)
//
//  REVISED FINAL SCHEME (matches SX1276 datasheet table 18):
//    CAD phase:   RegDioMapping1 = 0x80  → DIO0=CadDone(10), DIO1=RxTimeout(00), DIO2=FhssChg(00)
//    TX phase:    RegDioMapping1 = 0x40  → DIO0=TxDone(01),  DIO1=RxTimeout(00), DIO2=FhssChg(00)
//    RX phase:    RegDioMapping1 = 0x00  → DIO0=RxDone(00),  DIO1=RxTimeout(00), DIO2=FhssChg(00)
//
//  DIO1 pin: attach ISR for RxTimeout — lets loraReadRaw wake early on timeout.
//  DIO2 pin: unused ISR (CadDetected already read via IRQ register in runCad).
//
// =====================================================

// =====================================================
// CAD state
// =====================================================
static const uint32_t CAD_TIMEOUT_MS  = 20;   // raised from 15 — gives mode settle time
static const uint8_t  CAD_MAX_RETRIES = 8;

static volatile bool cadDone     = false;
static volatile bool cadDetected = false;

// TxDone / RxDone flags set by ISRs
static volatile bool txDoneFlag  = false;
static volatile bool rxDoneFlag  = false;

// =====================================================
// ISRs
// =====================================================

// DIO0: context-dependent — CadDone during CAD, TxDone during TX, RxDone during RX
static void IRAM_ATTR onDio0Rise() {
  cadDone    = true;   // used during CAD window
  txDoneFlag = true;   // used during TX window
  rxDoneFlag = true;   // used during RX window (parsePacket still primary path)
}

// DIO1: RxTimeout — not strictly needed but prevents loraReadRaw from spinning
// when the modem signals a timeout. Currently a stub; extend if needed.
static void IRAM_ATTR onDio1Rise() {
  // No action needed — loraReadRaw uses polling with millis() timeout.
  // Keeping the ISR registered prevents floating-pin noise from crashing.
}

// =====================================================
// Direct SPI register write / read
// =====================================================
static void spiWriteReg(uint8_t reg, uint8_t value) {
  digitalWrite(LORA_NSS, LOW);
  SPI.transfer(reg | 0x80);
  SPI.transfer(value);
  digitalWrite(LORA_NSS, HIGH);
}

static uint8_t spiReadReg(uint8_t reg) {
  digitalWrite(LORA_NSS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t val = SPI.transfer(0x00);
  digitalWrite(LORA_NSS, HIGH);
  return val;
}

// =====================================================
// setDioMapping — switch DIO0 role
// =====================================================
static void setDioMappingCad() {
  // DIO0=CadDone(10), DIO1=RxTimeout(00), DIO2=FhssChg(00) → 0x80
  spiWriteReg(0x40, 0x80);
}

static void setDioMappingTx() {
  // DIO0=TxDone(01), DIO1=RxTimeout(00), DIO2=FhssChg(00) → 0x40
  spiWriteReg(0x40, 0x40);
}

static void setDioMappingRx() {
  // DIO0=RxDone(00), DIO1=RxTimeout(00), DIO2=FhssChg(00) → 0x00
  spiWriteReg(0x40, 0x00);
}

// =====================================================
// enterStandby / enterRxCont — direct register control
//
// LoRa.idle() and LoRa.receive() are unreliable on ESP32-S3
// because the library polls DIO0 state internally which can
// interfere with our ISR-based flag scheme. We drive the
// OpMode register directly and verify the transition.
// =====================================================
static void enterStandby() {
  spiWriteReg(0x01, 0x81);  // LoRa | STANDBY
  delayMicroseconds(200);
  // Verify — retry once if needed
  if ((spiReadReg(0x01) & 0x07) != 0x01) {
    delayMicroseconds(500);
    spiWriteReg(0x01, 0x81);
    delayMicroseconds(200);
  }
}

static void enterRxCont() {
  setDioMappingRx();
  spiWriteReg(0x12, 0xFF);  // clear all IRQ flags
  spiWriteReg(0x01, 0x85);  // LoRa | RxContinuous
  delayMicroseconds(500);
  // Verify — the modem needs a moment after standby→rx
  uint8_t mode = spiReadReg(0x01) & 0x07;
  if (mode != 0x05) {
    delay(2);
    spiWriteReg(0x01, 0x85);
    delay(2);
  }
  Serial.printf("[RX] RegOpMode after enterRxCont: 0x%02X\n", spiReadReg(0x01));
}

// =====================================================
// waitTxDone — poll TxDone IRQ flag with timeout
//
// Called immediately after LoRa.endPacket(false).
// Returns true if TxDone fired within timeoutMs.
// At SF7/125kHz: 42-byte packet ≈ 30 ms, 101-byte ≈ 70 ms.
// We allow up to 500 ms to be safe.
// =====================================================
static bool waitTxDone(uint32_t timeoutMs = 500) {
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    // Check ISR flag (set by DIO0 rising edge in TX mapping)
    if (txDoneFlag) {
      txDoneFlag = false;
      spiWriteReg(0x12, 0xFF);  // clear IRQ flags
      return true;
    }
    // Also poll IRQ register directly as belt-and-suspenders
    uint8_t irq = spiReadReg(0x12);
    if (irq & 0x08) {  // TxDone bit3
      txDoneFlag = false;
      spiWriteReg(0x12, 0xFF);
      return true;
    }
    delayMicroseconds(500);
  }
  Serial.printf("[TX] waitTxDone: TIMEOUT after %lu ms  IRQ=0x%02X\n",
                (unsigned long)timeoutMs, spiReadReg(0x12));
  return false;
}

// =====================================================
// runCad — Channel Activity Detection
// =====================================================
static bool runCad() {
  cadDone     = false;
  cadDetected = false;

  enterStandby();
  delay(2);  // let standby settle before mode switch

  spiWriteReg(0x12, 0xFF);   // clear all IRQ flags before CAD
  setDioMappingCad();        // DIO0 → CadDone
  delayMicroseconds(200);

  spiWriteReg(0x01, 0x87);   // LoRa | CAD mode
  delayMicroseconds(200);

  // Verify CAD mode engaged
  uint8_t mode = spiReadReg(0x01) & 0x07;
  if (mode != 0x07) {
    Serial.printf("[CAD] mode didn't engage: 0x%02X — treating as clear\n",
                  spiReadReg(0x01));
    enterStandby();
    return true;
  }

  uint32_t start = millis();
  while (!cadDone && (millis() - start) < CAD_TIMEOUT_MS) {
    delayMicroseconds(100);
  }

  if (!cadDone) {
    Serial.println("[CAD] timeout (treated as clear)");
    enterStandby();
    return true;
  }

  // CadDone fired — check CadDetected bit (bit 0 of IRQ flags, bit 2 in some revisions)
  // SX1276: IRQ bit0 = CadDetected, bit2 = CadDone
  uint8_t irq = spiReadReg(0x12);
  cadDetected = (irq & 0x01) != 0;
  spiWriteReg(0x12, 0xFF);  // clear

  enterStandby();
  return !cadDetected;
}

// =====================================================
// cadWaitAndSend — CAD collision avoidance + TX
//
// Key changes vs old version:
//   1. enterStandby() explicitly before beginPacket
//   2. setDioMappingTx() so DIO0 fires on TxDone
//   3. endPacket(false) — non-blocking, we poll ourselves
//   4. waitTxDone() — polls IRQ register + ISR flag
//   5. enterRxCont() — direct register write, verified
// =====================================================
static bool cadWaitAndSend(const uint8_t *data, size_t len) {
  for (uint8_t attempt = 0; attempt < CAD_MAX_RETRIES; attempt++) {
    if (!runCad()) {
      Serial.printf("[CAD] channel busy, retry %d/%d\n", attempt + 1, CAD_MAX_RETRIES);
      delay(random(5, 25));
      continue;
    }

    // Prepare radio for TX
    enterStandby();
    delay(2);
    setDioMappingTx();     // DIO0 → TxDone
    txDoneFlag = false;
    spiWriteReg(0x12, 0xFF);  // clear IRQ flags

    LoRa.beginPacket();
    LoRa.write(data, len);

    // Non-blocking endPacket — we handle TxDone ourselves
    LoRa.endPacket(false);

    // Poll for TxDone with timeout
    bool txOk = waitTxDone(500);

    if (!txOk) {
      Serial.println("[TX] TxDone timeout — aborting TX attempt");
      enterStandby();
      enterRxCont();
      return false;
    }

    Serial.printf("[TX] TxDone confirmed  len=%u  IRQ=0x%02X\n",
                  (unsigned)len, spiReadReg(0x12));

    // Switch back to RxContinuous
    enterStandby();
    delayMicroseconds(500);
    enterRxCont();

    return true;
  }

  enterRxCont();
  Serial.println("[CAD] max retries — TX aborted");
  return false;
}

// =====================================================
// loraReadRaw — polling RX with timeout
//
// Uses LoRa.parsePacket() which reads RegIrqFlags internally.
// Radio must be in RxContinuous before calling.
// =====================================================
static bool loraReadRaw(uint8_t *buf, size_t cap, size_t &outLen, uint32_t timeoutMs) {
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    int pktSize = LoRa.parsePacket();
    if (pktSize > 0) {
      if ((size_t)pktSize > cap) {
        while (LoRa.available()) LoRa.read();
        return false;
      }
      size_t i = 0;
      while (LoRa.available() && i < cap) buf[i++] = (uint8_t)LoRa.read();
      outLen = i;
      return true;
    }
    delay(5);
  }
  return false;
}

// =====================================================
// waitForAck — waits for transport ACK from Gateway
//
// Radio is already in RxCont when this is called
// (cadWaitAndSend ends with enterRxCont).
// =====================================================
static bool waitForAck(uint32_t seq, uint32_t timeoutMs = 1500) {
  uint8_t  buf[MAX_FRAME_LEN];
  size_t   len   = 0;
  uint32_t start = millis();

  while ((millis() - start) < timeoutMs) {
    uint32_t remaining = timeoutMs - (millis() - start);
    uint32_t slice     = (remaining > 100) ? 100 : remaining;

    if (!loraReadRaw(buf, sizeof(buf), len, slice)) continue;
    if (len < HEADER_LEN + GCM_TAG_LEN + CRC_LEN) continue;

    uint16_t rxCrc   = readU16BE(&buf[len - 2]);
    uint16_t calcCrc = crc16Ccitt(buf, len - 2);
    if (rxCrc != calcCrc) {
      Serial.println("[ACK WAIT] CRC mismatch — ignored");
      continue;
    }

    // Must be an ACK frame for our device and seq
    if (buf[0] != PROTO_VERSION)       continue;
    if (buf[1] != MSG_TYPE_ACK) {
      // A command frame arrived while we're waiting for our ACK.
      // This happens when the gateway retries ACTIVATE before our ACTIVATE_OK landed.
      // Re-ACK it so the gateway stops retrying, then keep waiting for our ACK.
      if (buf[1] == MSG_TYPE_ACTIVATE || buf[1] == MSG_TYPE_MEASURE_REQ ||
          buf[1] == MSG_TYPE_HEARTBEAT_REQ) {
        uint16_t rxCrc2   = readU16BE(&buf[len - 2]);
        uint16_t calcCrc2 = crc16Ccitt(buf, len - 2);
        if (rxCrc2 == calcCrc2) {
          uint32_t retrySeq = readU32BE(&buf[6]);
          Serial.printf("[ACK WAIT] command retry type=0x%02X seq=%lu — re-ACKing\n",
                        buf[1], (unsigned long)retrySeq);
          sendAck(retrySeq);
        }
      }
      continue;
    }

    uint32_t deviceId  = readU32BE(&buf[2]);
    uint32_t rxSeq     = readU32BE(&buf[6]);
    if (deviceId != DEVICE_ID || rxSeq != seq) continue;

    // ACK carries no payload — cipherLen must be 0
    uint16_t cipherLen = readU16BE(&buf[10 + GCM_NONCE_LEN]);
    if (cipherLen != 0) continue;
    if (len != HEADER_LEN + GCM_TAG_LEN + CRC_LEN) continue;

    const uint8_t *nonce = &buf[10];
    uint8_t dummy[1] = {0};
    if (aesgcmDecrypt(buf + HEADER_LEN, 0, nonce, buf, HEADER_LEN,
                      buf + HEADER_LEN, dummy)) {
      Serial.printf("[ACK WAIT] ACK confirmed seq=%lu\n", (unsigned long)seq);
      return true;
    }
    Serial.println("[ACK WAIT] GCM auth failed on ACK — ignored");
  }

  Serial.printf("[ACK WAIT] timeout after %lu ms — seq=%lu never ACKed\n",
                (unsigned long)timeoutMs, (unsigned long)seq);
  return false;
}

// =====================================================
// secureSend — build frame + CAD TX + wait ACK
//
// Used for all IoT→Gateway typed messages:
//   ACTIVATE_OK, HEARTBEAT_ACK, DATA (MEASURE_RESP / SHAKE_ALERT)
// =====================================================
static bool secureSend(uint8_t msgType, uint32_t seq,
                        const uint8_t *plain, uint16_t plainLen,
                        uint32_t ackTimeoutMs = 1500) {
  for (uint8_t attempt = 0; attempt < ACK_RETRY_MAX; attempt++) {
    uint8_t frame[MAX_FRAME_LEN];
    size_t  frameLen = 0;

    if (!buildSecureFrame(msgType, seq, plain, plainLen, frame, frameLen)) {
      Serial.println("[SECURE SEND] buildSecureFrame failed");
      return false;
    }

    Serial.printf("[SECURE SEND] type=0x%02X seq=%lu plainLen=%u attempt=%d/%d\n",
                  msgType, (unsigned long)seq, plainLen, attempt + 1, ACK_RETRY_MAX);

    if (!cadWaitAndSend(frame, frameLen)) {
      Serial.println("[SECURE SEND] cadWaitAndSend failed — retrying");
      // ── Relâcher le mutex pour laisser ControlTask ACK les commandes entrantes
      xSemaphoreGive(gLoRaMutex);
      vTaskDelay(pdMS_TO_TICKS(80));
      xSemaphoreTake(gLoRaMutex, portMAX_DELAY);
      continue;
    }

    Serial.printf("[LORA TX] type=0x%02X seq=%lu attempt=%d cadWaitAndSend=OK\n",
                  msgType, (unsigned long)seq, attempt + 1);

    bool acked = waitForAck(seq, ackTimeoutMs);

    if (acked) return true;

    Serial.printf("[SECURE SEND] no ACK for attempt %d\n", attempt + 1);

    // ── Fenêtre inter-tentative : relâche le mutex brièvement
    xSemaphoreGive(gLoRaMutex);
    vTaskDelay(pdMS_TO_TICKS(80));   // ControlTask peut ACK une commande entrante ici
    xSemaphoreTake(gLoRaMutex, portMAX_DELAY);
  }

  Serial.printf("[LORA] delivery failed after %d attempts (type=0x%02X seq=%lu)\n",
                ACK_RETRY_MAX, msgType, (unsigned long)seq);
  return false;
}

// =====================================================
// pollCommandFrame
//
// Called from ControlTask in a tight loop.
// Blocks up to timeoutMs waiting for a command from the
// gateway. On receipt: validates, ACKs, dispatches.
// Returns true if a valid frame was received.
// =====================================================
bool pollCommandFrame(uint32_t timeoutMs) {
  uint8_t buf[MAX_FRAME_LEN];
  size_t  len = 0;

  if (!loraReadRaw(buf, sizeof(buf), len, timeoutMs)) return false;

  Serial.printf("[RX] packet detected: %u bytes  RSSI=%d dBm  SNR=%.1f dB\n",
                (unsigned)len, LoRa.packetRssi(), LoRa.packetSnr());

  if (len < HEADER_LEN + GCM_TAG_LEN + CRC_LEN) {
    Serial.println("[POLL] frame too short — discarded");
    return false;
  }

  Serial.printf("[POLL] received %u bytes — parsing...\n", (unsigned)len);

  // ── CRC check ──
  uint16_t rxCrc   = readU16BE(&buf[len - 2]);
  uint16_t calcCrc = crc16Ccitt(buf, len - 2);
  Serial.printf("[CMD PARSE] CRC: rx=0x%04X calc=0x%04X %s\n",
                rxCrc, calcCrc, rxCrc == calcCrc ? "OK" : "FAIL");
  if (rxCrc != calcCrc) return false;

  // ── Header ──
  uint8_t  version   = buf[0];
  uint8_t  msgType   = buf[1];
  uint32_t deviceId  = readU32BE(&buf[2]);
  uint32_t seq       = readU32BE(&buf[6]);
  const uint8_t *nonce = &buf[10];
  uint16_t cipherLen = readU16BE(&buf[10 + GCM_NONCE_LEN]);

  Serial.printf("[CMD PARSE] ver=0x%02X type=0x%02X device=0x%08lX seq=%lu cipherLen=%u\n",
                version, msgType, (unsigned long)deviceId, (unsigned long)seq, cipherLen);

  if (version != PROTO_VERSION) { Serial.println("[CMD PARSE] version mismatch"); return false; }
  if (deviceId != DEVICE_ID)   { Serial.println("[CMD PARSE] device ID mismatch"); return false; }

  size_t expectedLen = HEADER_LEN + cipherLen + GCM_TAG_LEN + CRC_LEN;
  Serial.printf("[CMD PARSE] length check: frameLen=%u expectedLen=%u\n",
                (unsigned)len, (unsigned)expectedLen);
  if (len != expectedLen || cipherLen > MAX_PLAIN_LEN) {
    Serial.println("[CMD PARSE] length mismatch");
    return false;
  }
  if (msgType == MSG_TYPE_ACK) {
    // Les ACK ne s'ACKent pas — ignorer
    Serial.printf("[POLL] received ACK seq=%lu — ignored\n", seq);
    return false;
  }

 // ── Replay protection ──
  // ACTIVATE est exempté : le Gateway peut redémarrer avec seq=1
  // et l'IoT doit toujours accepter le handshake de boot.
  if (msgType != MSG_TYPE_ACTIVATE && seq <= gCtrlRxSeq && gCtrlRxSeq != 0) {
    Serial.printf("[CMD PARSE] replay rejected: seq=%lu last=%lu\n",
                  (unsigned long)seq, (unsigned long)gCtrlRxSeq);
    sendAck(seq);
    return false;
  }

  // Sur ACTIVATE : reset du compteur pour accepter la nouvelle session Gateway
  if (msgType == MSG_TYPE_ACTIVATE) {
    Serial.printf("[CMD PARSE] ACTIVATE — reset gCtrlRxSeq (%lu → %lu)\n",
                  (unsigned long)gCtrlRxSeq, (unsigned long)seq);
    gCtrlRxSeq = 0;  // sera mis à jour à seq après GCM
  }

  // ── GCM decrypt ──
  uint8_t plain[MAX_PLAIN_LEN + 1] = {0};
  size_t  tagPos = HEADER_LEN + cipherLen;

  bool gcmOk = aesgcmDecrypt(&buf[HEADER_LEN], cipherLen, nonce,
                              buf, HEADER_LEN, &buf[tagPos], plain);
  Serial.printf("[CMD PARSE] GCM auth: %s\n", gcmOk ? "OK" : "FAIL");
  if (!gcmOk) return false;

  plain[cipherLen] = '\0';
  gCtrlRxSeq = seq;

  Serial.printf("[CMD RX] type=0x%02X seq=%lu accepted\n", msgType, (unsigned long)seq);

  // ── ACK immediately ──
  Serial.println("[CMD RX] acquiring mutex for ACK...");
  if (xSemaphoreTake(gLoRaMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    Serial.println("[CMD RX] could not acquire gLoRaMutex for ACK — skipping");
    return false;
  }
  sendAck(seq);
  xSemaphoreGive(gLoRaMutex);
  Serial.println("[CMD RX] ACK sent and mutex released");

  // ── Dispatch ──
  switch (msgType) {

    case MSG_TYPE_ACTIVATE: {
      Serial.println("[CTRL] ACTIVATE received — node is now active");
      gNodeActive = true;

      // Build ACTIVATE_OK payload
      uint8_t mac[6];
      esp_read_mac(mac, ESP_MAC_WIFI_STA);
      char macStr[13];
      snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

      char jsonBuf[80];
      snprintf(jsonBuf, sizeof(jsonBuf),
               "{\"evt\":\"ACTIVATE_OK\",\"state\":\"active\",\"mac\":\"%s\"}", macStr);

      Serial.printf("[ACTIVATE_OK] mac=%s gNodeActive=%d\n", macStr, (int)gNodeActive);
      Serial.printf("[ACTIVATE_OK] payload: %s (%u bytes)\n",
                    jsonBuf, (unsigned)strlen(jsonBuf));

      if (xSemaphoreTake(gLoRaMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        Serial.println("[ACTIVATE_OK] mutex acquired — sending");
        bool ok = secureSend(MSG_TYPE_ACTIVATE_OK, gTxSeq,
                             (const uint8_t *)jsonBuf, (uint16_t)strlen(jsonBuf),
                             3000);  // 3 s — gateway needs CAD + TX time to ACK
        if (ok) gTxSeq++;
        Serial.printf("[ACTIVATE_OK] result: %s\n", ok ? "OK" : "FAILED");
        xSemaphoreGive(gLoRaMutex);
      } else {
        Serial.println("[ACTIVATE_OK] could not acquire gLoRaMutex — ACTIVATE_OK dropped");
      }
      break;
    }

    case MSG_TYPE_MEASURE_REQ: {
      Serial.println("[CTRL] MEASURE_REQ received — notifying SensorTask");
      if (gSensorTaskHandle != nullptr) {
        xTaskNotifyGive(gSensorTaskHandle);
      } else {
        Serial.println("[CTRL] ERROR: gSensorTaskHandle is null");
      }
      break;
    }

    case MSG_TYPE_HEARTBEAT_REQ: {
        Serial.println("[CTRL] HEARTBEAT_REQ received — sending HEARTBEAT_ACK");

        char jsonBuf[48];
        snprintf(jsonBuf, sizeof(jsonBuf),
                "{\"evt\":\"HB_ACK\",\"batt\":%d,\"state\":\"%s\"}",
                gSensorData.battPercent,
                gNodeActive ? "active" : "inactive");  // ← signale le vrai état

        if (xSemaphoreTake(gLoRaMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
          bool ok = secureSend(MSG_TYPE_HEARTBEAT_ACK, gTxSeq,
                              (const uint8_t *)jsonBuf, (uint16_t)strlen(jsonBuf));
          if (ok) gTxSeq++;
          Serial.printf("[HEARTBEAT_ACK] result: %s\n", ok ? "OK" : "FAILED");
          xSemaphoreGive(gLoRaMutex);
        }
        break;
      }

    default:
      Serial.printf("[CTRL] unhandled command type 0x%02X\n", msgType);
      break;
  }

  return true;
}

// =====================================================
// sendAck — transport ACK back to Gateway
//
// Sends a zero-payload authenticated frame.
// Does NOT acquire gLoRaMutex — caller must hold it.
// =====================================================
bool sendAck(uint32_t seq) {
  Serial.printf("[ACK] building ACK for seq=%lu\n", (unsigned long)seq);

  uint8_t frame[MAX_FRAME_LEN];
  size_t  frameLen = 0;

  if (!buildSecureFrame(MSG_TYPE_ACK, seq, nullptr, 0, frame, frameLen)) {
    Serial.println("[ACK] buildSecureFrame failed");
    return false;
  }

  Serial.printf("[ACK] buildSecureFrame: OK  frameLen=%u\n", (unsigned)frameLen);

  bool ok = cadWaitAndSend(frame, frameLen);
  Serial.printf("[ACK] seq=%lu sent=%s\n", (unsigned long)seq, ok ? "yes" : "no");
  return ok;
}

// =====================================================
// sendActivateOk — not used directly; handled inside
// pollCommandFrame dispatch. Kept for API completeness.
// =====================================================
bool sendActivateOk() {
  // Handled inline in MSG_TYPE_ACTIVATE dispatch above.
  return false;
}

// =====================================================
// sendMeasureResp — called from SensorTask after read
// Acquires gLoRaMutex internally.
// =====================================================
bool sendMeasureResp(const String &sensorJson) {
  if (xSemaphoreTake(gLoRaMutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
    Serial.println("[MEASURE_RESP] could not acquire gLoRaMutex");
    return false;
  }

  bool ok = secureSend(MSG_TYPE_DATA, gTxSeq,
                       (const uint8_t *)sensorJson.c_str(),
                       (uint16_t)sensorJson.length(),
                       2000);
  if (ok) gTxSeq++;

  xSemaphoreGive(gLoRaMutex);
  return ok;
}

// =====================================================
// sendHeartbeatAck — standalone version (not used;
// handled inside pollCommandFrame). Kept for API.
// =====================================================
bool sendHeartbeatAck() {
  return false;
}

// =====================================================
// sendShakeAlert — autonomous high-priority TX
//
// Called from checkShakeAndAlert() in MpuTask.
// Acquires gLoRaMutex with portMAX_DELAY — shake is
// highest priority and will wait out anything else.
// =====================================================
void sendShakeAlert(const String &shakeJson) {
  if (xSemaphoreTake(gLoRaMutex, portMAX_DELAY) != pdTRUE) return;

  bool ok = secureSend(MSG_TYPE_DATA, gTxSeq,
                       (const uint8_t *)shakeJson.c_str(),
                       (uint16_t)shakeJson.length(),
                       2000);
  if (ok) gTxSeq++;

  xSemaphoreGive(gLoRaMutex);
}

// =====================================================
// loraInit
// =====================================================
bool loraInit() {
  Serial.println("[LoRa INIT] starting...");
  Serial.printf("[LoRa INIT] pins: NSS=%d RST=%d DIO0=%d SCK=%d MISO=%d MOSI=%d\n",
                LORA_NSS, LORA_RST, LORA_DIO0, LORA_SCK, LORA_MISO, LORA_MOSI);
  Serial.printf("[LoRa INIT] config: freq=%ld SF=%d BW=%ld CR=%d sync=0x%02X\n",
                (long)LORA_FREQ, LORA_SF, (long)LORA_BW, LORA_CR, LORA_SYNC_WORD);

  // GPIO setup
  pinMode(LORA_NSS,  OUTPUT);
  pinMode(LORA_RST,  OUTPUT);
  digitalWrite(LORA_NSS, HIGH);

  // DIO0 — primary IRQ (CadDone / TxDone / RxDone depending on mapping)
  pinMode(LORA_DIO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(LORA_DIO0), onDio0Rise, RISING);

  // DIO1 — RxTimeout stub ISR (prevents floating-pin noise)
  pinMode(LORA_DIO1, INPUT);
  attachInterrupt(digitalPinToInterrupt(LORA_DIO1), onDio1Rise, RISING);

  // DIO2 — not used for ISR; configured as input to avoid floating
  pinMode(LORA_DIO2, INPUT);

  // SPI + library init
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  Serial.println("[LoRa INIT] calling LoRa.begin()...");
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

  // Enter RxContinuous via direct register control
  enterRxCont();

  Serial.println("[LoRa] init ok");
  Serial.println("[LoRa] CAD-only collision avoidance (no busy wire)");
  Serial.println("[LoRa] DIO0=CadDone/TxDone/RxDone  DIO1=RxTimeout(stub)  DIO2=unused");
  return true;
}