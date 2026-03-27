#include <SPI.h>
#include <SD.h>
#include <LoRa.h>
#include <ArduinoJson.h>

#include "AudioTools.h"
#include <Update.h>

#include "mbedtls/aes.h"
#include "mbedtls/md.h"

using namespace audio_tools;

// =====================================================
// LoRa pins (ESP32 DevKit) - SPI bus #1
// =====================================================
static const int LORA_NSS  = 5;
static const int LORA_SCK  = 18;
static const int LORA_MISO = 19;
static const int LORA_MOSI = 23;
static const int LORA_RST  = 32;
static const int LORA_DIO0 = 33;

// Reserved for future FUOTA / advanced IRQ usage
static const int LORA_DIO1 = 17;
static const int LORA_DIO2 = 16;

static const long LORA_FREQ = 433E6;
static const int  LORA_SF   = 7;
static const long LORA_BW   = 125E3;
static const int  LORA_CR   = 5;
static const uint8_t LORA_SYNC_WORD = 0x34;

// =====================================================
// Secure protocol config
// MUST MATCH THE NODE
// =====================================================
static const uint8_t  PROTO_VERSION = 1;
static const uint32_t DEVICE_ID     = 0xEA500123;   // must match node exactly

static const uint8_t ENC_KEY[16] = {
  0x11, 0x29, 0x3A, 0x47, 0x58, 0x61, 0x72, 0x8C,
  0x90, 0xAB, 0xBC, 0xCD, 0xDE, 0xE1, 0xF2, 0x04
};

static const uint8_t HMAC_KEY[16] = {
  0x21, 0x39, 0x4A, 0x57, 0x68, 0x71, 0x82, 0x9C,
  0xA0, 0xBB, 0xCC, 0xDD, 0xEE, 0xF1, 0x02, 0x14
};

static const uint8_t MSG_TYPE_DATA = 0x01;
static const uint8_t MSG_TYPE_ACK  = 0x02;

static const size_t IV_LEN         = 8;
static const size_t HMAC_TRUNC_LEN = 8;
static const size_t CRC_LEN        = 2;
static const size_t HEADER_LEN     = 1 + 1 + 4 + 4 + 8 + 2; // ver,type,dev,seq,iv,payload_len
static const size_t MAX_PLAIN_LEN  = 180;
static const size_t MAX_FRAME_LEN  = HEADER_LEN + MAX_PLAIN_LEN + HMAC_TRUNC_LEN + CRC_LEN;

// Anti-replay state
uint32_t lastAcceptedSeq = 0;

// =====================================================
// SD card - SPI bus #2 (separate bus)
// =====================================================
static const int SD_CS   = 4;
static const int SD_SCK  = 14;
static const int SD_MISO = 21;
static const int SD_MOSI = 22;

// Separate SPI bus for SD
SPIClass sdSPI(HSPI);

// =====================================================
// MAX98357A / I2S
// =====================================================
static const int I2S_BCLK = 26;
static const int I2S_LRC  = 25;
static const int I2S_DIN  = 27;

// =====================================================
// AudioTools objects
// =====================================================
I2SStream i2s;
WAVDecoder wavDecoder;
EncodedAudioStream decodedStream(&i2s, &wavDecoder);

// =====================================================
// Alert state
// =====================================================
bool alarmRunning = false;
bool audioBusy = false;

String lastPlayedFile = "";
unsigned long lastAlertPlayAt = 0;
static const unsigned long ALERT_COOLDOWN_MS = 15000;

// =====================================================
// Multi-alert queue
// =====================================================
static const int MAX_ALERTS = 10;
String alertQueue[MAX_ALERTS];
int alertCount = 0;

// =====================================================
// Helpers
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

void buildIv(uint32_t seq, uint8_t iv[IV_LEN]) {
  writeU32BE(&iv[0], DEVICE_ID);
  writeU32BE(&iv[4], seq);
}

bool aesCtrCrypt(const uint8_t *input, size_t len, const uint8_t iv[IV_LEN], uint8_t *output) {
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

bool computeHmac8(const uint8_t *data, size_t len, uint8_t out8[HMAC_TRUNC_LEN]) {
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

void sendFirmwareOTA(const char* path) {
  File updateFile = SD.open(path, FILE_READ);
  if (!updateFile) {
    Serial.println("[FUOTA] Erreur: Impossible d'ouvrir le fichier .bin");
    return;
  }

  size_t totalSize = updateFile.size();
  uint16_t totalPackets = (totalSize + 127) / 128;

  Serial.printf("[FUOTA] Début : %s (%d bytes, %d packets)\n", path, (int)totalSize, totalPackets);

  for (uint16_t i = 0; i < totalPackets; i++) {
    uint8_t payload[128];
    size_t bytesRead = updateFile.read(payload, 128);

    LoRa.beginPacket();
    LoRa.write('F');
    LoRa.write(highByte(i)); LoRa.write(lowByte(i));
    LoRa.write(highByte(totalPackets)); LoRa.write(lowByte(totalPackets));
    LoRa.write(payload, bytesRead);
    LoRa.endPacket();

    Serial.printf("Paquet %d/%d envoyé\n", i + 1, totalPackets);
    delay(150);
  }

  updateFile.close();
  Serial.println("[FUOTA] Transfert terminé.");
}

void deselectLoRa() {
  digitalWrite(LORA_NSS, HIGH);
}

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

bool buildSecureFrame(
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

bool parseAndVerifyDataFrame(
  const uint8_t *frame,
  size_t frameLen,
  uint32_t &seqOut,
  uint8_t *plainOut,
  uint16_t &plainLenOut
) {
  if (frameLen < HEADER_LEN + HMAC_TRUNC_LEN + CRC_LEN) {
    Serial.println("[SEC] frame too short");
    return false;
  }

  uint16_t rxCrc = readU16BE(&frame[frameLen - 2]);
  uint16_t calcCrc = crc16Ccitt(frame, frameLen - 2);
  if (rxCrc != calcCrc) {
    Serial.println("[SEC] bad CRC16");
    return false;
  }

  uint8_t version = frame[0];
  uint8_t msgType = frame[1];
  uint32_t deviceId = readU32BE(&frame[2]);
  uint32_t seq = readU32BE(&frame[6]);
  const uint8_t *iv = &frame[10];
  uint16_t payloadLen = readU16BE(&frame[18]);

  if (version != PROTO_VERSION) {
    Serial.println("[SEC] bad version");
    return false;
  }

  if (msgType != MSG_TYPE_DATA) {
    Serial.println("[SEC] not DATA");
    return false;
  }

  if (deviceId != DEVICE_ID) {
    Serial.printf("[SEC] wrong device id: 0x%08lX\n", (unsigned long)deviceId);
    return false;
  }

  size_t expectedLen = HEADER_LEN + payloadLen + HMAC_TRUNC_LEN + CRC_LEN;
  if (frameLen != expectedLen) {
    Serial.println("[SEC] bad frame length");
    return false;
  }

  if (payloadLen > MAX_PLAIN_LEN) {
    Serial.println("[SEC] payload too large");
    return false;
  }

  uint8_t expectedIv[IV_LEN];
  buildIv(seq, expectedIv);
  if (memcmp(iv, expectedIv, IV_LEN) != 0) {
    Serial.println("[SEC] IV mismatch");
    return false;
  }

  size_t signedLen = frameLen - CRC_LEN - HMAC_TRUNC_LEN;
  const uint8_t *rxTag = &frame[signedLen];

  uint8_t calcTag[HMAC_TRUNC_LEN];
  if (!computeHmac8(frame, signedLen, calcTag)) {
    Serial.println("[SEC] HMAC compute failed");
    return false;
  }

  if (memcmp(rxTag, calcTag, HMAC_TRUNC_LEN) != 0) {
    Serial.println("[SEC] bad HMAC");
    return false;
  }

  const uint8_t *ciphertext = &frame[HEADER_LEN];
  if (payloadLen > 0) {
    if (!aesCtrCrypt(ciphertext, payloadLen, iv, plainOut)) {
      Serial.println("[SEC] decrypt failed");
      return false;
    }
  }

  seqOut = seq;
  plainLenOut = payloadLen;
  return true;
}

// =====================================================
// Init audio
// =====================================================
bool initAudio() {
  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.pin_bck = I2S_BCLK;
  cfg.pin_ws = I2S_LRC;
  cfg.pin_data = I2S_DIN;

  cfg.sample_rate = 16000;
  cfg.bits_per_sample = 16;
  cfg.channels = 1;

  cfg.buffer_count = 8;
  cfg.buffer_size = 1024;

  if (!i2s.begin(cfg)) {
    Serial.println("[AUDIO] ERROR: I2S init failed");
    return false;
  }

  Serial.println("[AUDIO] I2S ready + DMA buffers configured");
  return true;
}

// =====================================================
// Init LoRa on default SPI object
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
// Init SD on separate SPI bus
// =====================================================
bool initSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(20);

  if (!SD.begin(SD_CS, sdSPI, 8000000)) {
    Serial.println("[SD] ERROR: SD.begin failed at 8 MHz. Retrying at 4 MHz...");

    if (!SD.begin(SD_CS, sdSPI, 4000000)) {
      Serial.println("[SD] ERROR: SD.begin failed completely");
      return false;
    }
    Serial.println("[SD] Ready on separate SPI bus (4 MHz)");
  } else {
    Serial.println("[SD] Ready on separate SPI bus (8 MHz)");
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] ERROR: No SD card attached");
    return false;
  }

  Serial.print("[SD] Card size: ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  return true;
}

// =====================================================
// Alert queue helpers
// =====================================================
void clearAlertQueue() {
  alertCount = 0;
}

bool isAlreadyQueued(const char* path) {
  for (int i = 0; i < alertCount; i++) {
    if (alertQueue[i] == path) return true;
  }
  return false;
}

void queueAlert(const char* path) {
  if (path == nullptr) return;
  if (alertCount >= MAX_ALERTS) return;
  if (isAlreadyQueued(path)) return;

  alertQueue[alertCount++] = path;
}

// =====================================================
// Collect all active alerts
// =====================================================
void collectAlertFiles(JsonDocument& doc, int rssi) {
  clearAlertQueue();

  String event = doc["event"] | "";

  int batPct = doc["bat_pct"] | 100;
  int ph10   = doc["ph_10"] | 10;
  int tds10  = doc["tds_10"] | 10;
  int turb10 = doc["turb_10"] | 10;

  if (event == "ALARM_SHAKE") {
    queueAlert("/alerts/alert_fall.wav");
  }

  if (batPct <= 10) {
    queueAlert("/alerts/alert_BAT_high.wav");
  } else if (batPct <= 25) {
    queueAlert("/alerts/alert_BAT_medium.wav");
  }

  if (ph10 <= 4) {
    queueAlert("/alerts/alert_pH_high.wav");
  } else if (ph10 <= 7) {
    queueAlert("/alerts/alert_pH_medium.wav");
  }

  if (tds10 <= 4) {
    queueAlert("/alerts/alert_TDS_high.wav");
  } else if (tds10 <= 7) {
    queueAlert("/alerts/alert_TDS_medium.wav");
  }

  if (turb10 <= 4) {
    queueAlert("/alerts/alert_TURBIDITY_high.wav");
  } else if (turb10 <= 7) {
    queueAlert("/alerts/alert_TURBIDITY_medium.wav");
  }

  if (rssi < -115) {
    queueAlert("/alerts/alert_LoRa.wav");
  }
}

// =====================================================
// WAV playback from SD
// =====================================================
bool playAlertFile(const char* path) {
  if (audioBusy || path == nullptr) return false;

  unsigned long now = millis();
  if (lastPlayedFile == String(path) && (now - lastAlertPlayAt) < ALERT_COOLDOWN_MS) {
    Serial.printf("[AUDIO] Cooldown active for %s\n", path);
    return false;
  }

  audioBusy = true;

  File audioFile = SD.open(path, FILE_READ);
  if (!audioFile) {
    Serial.printf("[AUDIO] ERROR: file not found: %s\n", path);
    audioBusy = false;
    return false;
  }

  Serial.printf("[AUDIO] Playing: %s\n", path);

  decodedStream.begin();
  StreamCopy copier(decodedStream, audioFile, 2048);

  while (audioFile.available()) {
    copier.copy();
  }

  decodedStream.end();
  audioFile.close();

  lastPlayedFile = String(path);
  lastAlertPlayAt = millis();
  audioBusy = false;

  Serial.println("[AUDIO] Playback finished");
  return true;
}

// =====================================================
// Play all queued alerts
// =====================================================
void playQueuedAlerts() {
  if (alertCount == 0) return;

  Serial.printf("[ALERT] %d alert(s) en file\n", alertCount);

  for (int i = 0; i < alertCount; i++) {
    Serial.printf("[ALERT] Lecture %d/%d : %s\n", i + 1, alertCount, alertQueue[i].c_str());
    playAlertFile(alertQueue[i].c_str());
    delay(150);
  }

  clearAlertQueue();
}

// =====================================================
// Alarm handling
// =====================================================
void startAlarm() {
  alarmRunning = true;
  Serial.println("\n[ALARM] Alert triggered");
}

void stopAlarm() {
  alarmRunning = false;
  Serial.println("\n[ALARM] Alert stopped");
}

// =====================================================
// Secure telemetry handling
// =====================================================
void handleSecurePacket(const uint8_t *frame, size_t frameLen, int rssi, float snr) {
  uint32_t seq = 0;
  uint8_t plain[MAX_PLAIN_LEN + 1] = {0};
  uint16_t plainLen = 0;

  if (!parseAndVerifyDataFrame(frame, frameLen, seq, plain, plainLen)) {
    return;
  }

  if (seq < lastAcceptedSeq) {
    Serial.print("[REPLAY] old seq rejected: ");
    Serial.println((unsigned long)seq);
    return;
  }

  if (seq == lastAcceptedSeq) {
    Serial.print("[DUP] seq=");
    Serial.print((unsigned long)seq);
    Serial.println(" -> ACK resend");
    sendSecureAck(seq);
    return;
  }

  plain[plainLen] = '\0';

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, plain, plainLen);

  if (error) {
    Serial.print("[JSON ERROR] Echec du parsing : ");
    Serial.println(error.c_str());
    Serial.print("[JSON RAW] ");
    Serial.println((char*)plain);
    return;
  }

  lastAcceptedSeq = seq;
  sendSecureAck(seq);

  String event = doc["event"] | "None";

  Serial.println("\n========= NOUVELLES DONNEES EAU (SECURE) =========");
  Serial.printf("SEQ         : %lu\n", (unsigned long)seq);
  Serial.printf("Signal LoRa : RSSI %d dBm | SNR %.1f dB\n", rssi, snr);
  Serial.println("--------------------------------------------------");

  if (doc.containsKey("bat_pct")) {
    Serial.printf("BATTERIE    : %d%% | %.2f V | %d mA\n",
                  doc["bat_pct"].as<int>(),
                  doc["bat_v"].as<float>(),
                  doc["bat_ma"].as<int>());
  }

  if (doc.containsKey("ph_val")) {
    Serial.printf("pH          : %.2f | Tension: %.2f V | Score: %d/10 (%s)\n",
                  doc["ph_val"].as<float>(),
                  doc["ph_v"].as<float>(),
                  doc["ph_10"].as<int>(),
                  doc["ph_msg"].as<String>().c_str());
  }

  if (doc.containsKey("tds_ppm")) {
    Serial.printf("TDS         : %d ppm | Score: %d/10 (%s)\n",
                  doc["tds_ppm"].as<int>(),
                  doc["tds_10"].as<int>(),
                  doc["tds_msg"].as<String>().c_str());
  }

  if (doc.containsKey("turb_v")) {
    Serial.printf("TURBIDITE   : %.2f V | Score: %d/10 (%s)\n",
                  doc["turb_v"].as<float>(),
                  doc["turb_10"].as<int>(),
                  doc["turb_msg"].as<String>().c_str());
  }

  if (doc.containsKey("t_water")) {
    Serial.printf("TEMP. EAU   : %.1f °C\n", doc["t_water"].as<float>());
  }

  if (doc.containsKey("t_mpu")) {
    Serial.printf("TEMP. CARTE : MPU %.1f °C\n", doc["t_mpu"].as<float>());
  }

  Serial.printf("EVENT       : %s\n", event.c_str());
  Serial.println("==================================================\n");

  if (event == "ALARM_SHAKE" && !alarmRunning) {
    startAlarm();
  }

  collectAlertFiles(doc, rssi);
  playQueuedAlerts();
}

// =====================================================
// Setup
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n=== ESP32 DEVKIT SECURE LoRa + SD(separate SPI) + WAV Alert Receiver START ===");

  if (!initAudio()) {
    while (true) delay(100);
  }

  if (!initLoRa()) {
    while (true) delay(100);
  }

  if (!initSD()) {
    while (true) delay(100);
  }

  Serial.println("Waiting for secure telemetry data...\n");
}

// =====================================================
// Loop
// =====================================================
void loop() {
  // UART commands
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') return;

    if (c >= '1' && c <= '9') {
      loraSendChar(c);
    } else if (c == 'a') {
      loraSendString("10");
    } else if (c == '0') {
      loraSendString("0");
    } else if (c == 's') {
      stopAlarm();
    }
  }

  // LoRa RX - binary secure frame
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  uint8_t frame[MAX_FRAME_LEN];
  size_t len = 0;

  while (LoRa.available() && len < sizeof(frame)) {
    frame[len++] = (uint8_t)LoRa.read();
  }

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();

  handleSecurePacket(frame, len, rssi, snr);
  LoRa.receive();
}