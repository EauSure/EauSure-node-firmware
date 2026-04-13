#include "app_state.h"
#include "lora_radio.h"
#include "display_oled.h"
#include "pairing_store.h"

// =====================================================
// Global device instances
// =====================================================
Adafruit_SSD1306   display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_INA219    ina219;
bool               inaOk = false;

OneWire            oneWire(ONE_WIRE_BUS);
DallasTemperature  sensors(&oneWire);

bool               mpuOk = false;

Adafruit_NeoPixel  pixel(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);
bool               rgbActive = false;
uint32_t           rgbOffAt  = 0;

PendingShake gPendingShake;

// =====================================================
// Shared state
// =====================================================
SensorData     gSensorData;
EventState     gEventState;
volatile bool  gNodeActive = false;   // false until Gateway sends ACTIVATE

float phVoltageAtNeutral = PH_VOLTAGE_AT_NEUTRAL;
float phSlope            = PH_SLOPE;

// =====================================================
// FreeRTOS primitives
// =====================================================
SemaphoreHandle_t gDataMutex      = nullptr;
SemaphoreHandle_t gI2CMutex       = nullptr;
SemaphoreHandle_t gLoRaMutex      = nullptr;
TaskHandle_t      gSensorTaskHandle = nullptr;


volatile bool gAckWaitActive = false;

// =====================================================
// Sequence counters
//   gTxSeq     — IoT outgoing frame counter (DATA / ACTIVATE_OK / HEARTBEAT_ACK)
//   gCtrlRxSeq — last accepted incoming command seq (replay protection)
// =====================================================
uint32_t gTxSeq     = 1;
uint32_t gCtrlRxSeq = 0;


uint8_t gRuntimeEncKey[16] = {0};
bool    gRuntimeEncKeyLoaded = false;

// =====================================================
// Binary encoding helpers
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


bool parseHexKey16(const String& hex, uint8_t out[16]) {
  if (hex.length() != 32) return false;

  for (int i = 0; i < 16; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];

    auto hexVal = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
      if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
      return -1;
    };

    int h = hexVal(hi);
    int l = hexVal(lo);
    if (h < 0 || l < 0) return false;

    out[i] = (uint8_t)((h << 4) | l);
  }

  return true;
}

bool loadRuntimeEncKey() {
  NodePairingData p = PairingStore::load();
  if (!p.valid) {
    Serial.println("[KEY] No valid pairing found");
    gRuntimeEncKeyLoaded = false;
    return false;
  }

  if (!parseHexKey16(p.aesKeyHex, gRuntimeEncKey)) {
    Serial.println("[KEY] Invalid AES key in pairing store");
    gRuntimeEncKeyLoaded = false;
    return false;
  }

  gRuntimeEncKeyLoaded = true;
  Serial.println("[KEY] Runtime AES key loaded from pairing store");
  return true;
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

  if (!gRuntimeEncKeyLoaded) {
    mbedtls_gcm_free(&gcm);
    return false;
  }

  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, gRuntimeEncKey, 128) != 0) {
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

  if (!gRuntimeEncKeyLoaded) {
  mbedtls_gcm_free(&gcm);
  return false;
}

  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, gRuntimeEncKey, 128) != 0) {
    mbedtls_gcm_free(&gcm);
    return false;
  }

  uint8_t dummy[1] = {0};
  const uint8_t *plainPtr  = (plainLen > 0) ? plain      : dummy;
  uint8_t       *cipherPtr = (plainLen > 0) ? ciphertext : dummy;

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
// Low-level I2C helpers
// =====================================================
bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return (Wire.endTransmission(true) == 0);
}

bool i2cReadN(uint8_t addr, uint8_t startReg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((int)addr, (int)n, (int)true);
  if (got != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// =====================================================
// MPU6050
// =====================================================
bool mpuInit() {
  if (!i2cWrite8(MPU_ADDR, REG_PWR_MGMT_1, 0x00)) return false;
  delay(50);
  uint8_t accelCfg = (ACCEL_RANGE & 0x03) << 3;
  return i2cWrite8(MPU_ADDR, REG_ACCEL_CONFIG, accelCfg);
}

void readMpuTemp() {
  if (!mpuOk) return;
  uint8_t b[2];
  if (i2cReadN(MPU_ADDR, REG_TEMP_OUT_H, b, 2)) {
    int16_t rawT = (b[0] << 8) | b[1];
    gSensorData.mpuTempC = (rawT / 340.0f) + 36.53f;
  }
}

// =====================================================
// RGB NeoPixel
// =====================================================
void rgbInit() {
  pixel.begin();
  pixel.clear();
  pixel.show();
}

void rgbFlash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
  rgbActive = true;
  rgbOffAt  = millis() + ms;
}

void updateRgb() {
  if (rgbActive && (int32_t)(rgbOffAt - millis()) <= 0) {
    rgbActive = false;
    pixel.setPixelColor(0, pixel.Color(0, 0, 0));
    pixel.show();
  }
}

// =====================================================
// INA219 battery monitor
// =====================================================
void updateIna219() {
  if (!inaOk) return;
  gSensorData.battShuntmV   = ina219.getShuntVoltage_mV();
  gSensorData.battBusV      = ina219.getBusVoltage_V();
  gSensorData.battCurrentmA = ina219.getCurrent_mA();
  gSensorData.battPowermW   = ina219.getPower_mW();
  gSensorData.battLoadV     = gSensorData.battBusV + (gSensorData.battShuntmV / 1000.0f);

  float p = (gSensorData.battLoadV - BATT_EMPTY_V) * 100.0f / (BATT_FULL_V - BATT_EMPTY_V);
  gSensorData.battPercent = constrain((int)p, 0, 100);
}

// =====================================================
// Scale / conclusion helpers
// =====================================================
int calculateTurbidityScale(float sensorV) {
  float scale = map(sensorV * 100, 100, 420, 0, 10);
  return constrain((int)scale, 0, 10);
}

int calculateTdsScale(float ppm) {
  float scale = map(ppm, 0, 1000, 10, 0);
  return constrain((int)scale, 0, 10);
}

int calculatePhScale(float ph) {
  float distance = fabs(ph - 7.0f);
  int score = 10 - (int)(distance * 3.0f);
  return constrain(score, 0, 10);
}

String getTdsConclusion(int ppm) {
  if (ppm < 150) return "Excellent";
  if (ppm < 300) return "Bon";
  if (ppm < 500) return "Acceptable";
  return "Mediocre";
}

String getTurbConclusion(int score) {
  if (score >= 8) return "Limpide";
  if (score >= 5) return "Trouble";
  return "Sale";
}

String getPhConclusion(float ph) {
  if (ph >= 6.5f && ph <= 8.5f) return "Ideal";
  if (ph < 6.5f) return "Acide";
  return "Basique";
}

// =====================================================
// readSensorsRoutine
// Called exclusively from SensorTask (never from ISR or other tasks).
// Caller must hold gDataMutex before calling.
// =====================================================
void readSensorsRoutine() {
  gEventState.measureInProgress = true;

  // DS18B20 water temperature
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C) gSensorData.waterTempC = t;

  // Power sensors and wait for RC settle
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(SENSOR_WARMUP_MS));
  vTaskDelay(pdMS_TO_TICKS(SENSOR_RC_SETTLE_MS));

  // ── Turbidity ──
  for (int i = 0; i < 8; i++) { analogRead(TURB_ADC_PIN); vTaskDelay(pdMS_TO_TICKS(10)); }
  uint32_t tSum = 0;
  for (int i = 0; i < 20; i++) { tSum += analogRead(TURB_ADC_PIN); vTaskDelay(pdMS_TO_TICKS(20)); }
  float tAdcV = ((tSum / 20.0f) * 3.3f) / 4095.0f;
  gSensorData.lastTurbSensorVoltage =
    tAdcV * ((TURB_DIVIDER_RTOP + TURB_DIVIDER_RBOT) / TURB_DIVIDER_RBOT);
  gSensorData.turbScale10 = calculateTurbidityScale(gSensorData.lastTurbSensorVoltage);

  // ── TDS ──
  for (int i = 0; i < 8; i++) { analogRead(TDS_ADC_PIN); vTaskDelay(pdMS_TO_TICKS(10)); }
  uint32_t tdsSum = 0;
  for (int i = 0; i < 20; i++) { tdsSum += analogRead(TDS_ADC_PIN); vTaskDelay(pdMS_TO_TICKS(20)); }
  float tdsV = ((tdsSum / 20.0f) * 3.3f) / 4095.0f;
  float compensationCoeff = 1.0f + 0.02f * (gSensorData.waterTempC - 25.0f);
  float compV = tdsV / compensationCoeff;
  gSensorData.lastTdsValue =
    (133.42f * powf(compV, 3) - 255.86f * powf(compV, 2) + 857.39f * compV) * 0.5f;
  if (gSensorData.lastTdsValue < 0) gSensorData.lastTdsValue = 0;
  gSensorData.tdsScale10 = calculateTdsScale(gSensorData.lastTdsValue);

  // ── pH ──
  for (int i = 0; i < 8; i++) { analogRead(PH_ADC_PIN); vTaskDelay(pdMS_TO_TICKS(10)); }
  uint32_t phSum = 0;
  for (int i = 0; i < 20; i++) { phSum += analogRead(PH_ADC_PIN); vTaskDelay(pdMS_TO_TICKS(20)); }
  float phAdcV = ((phSum / 20.0f) * 3.3f) / 4095.0f;
  gSensorData.lastPhVoltage = phAdcV * 2.5f;
  gSensorData.lastPhValue   =
    7.0f + ((phVoltageAtNeutral - gSensorData.lastPhVoltage) / -phSlope);
  gSensorData.lastPhValue   = constrain(gSensorData.lastPhValue, 0.0f, 14.0f);
  gSensorData.phScale10     = calculatePhScale(gSensorData.lastPhValue);

  digitalWrite(SENSOR_POWER_PIN, LOW);

  // Internal temperatures
  gSensorData.espTempC = temperatureRead();
  readMpuTemp();

  updateIna219();

  gEventState.measureInProgress = false;
}

// =====================================================
// checkShakeAndAlert
// Called from MpuTask at 20 Hz.
// On detection: sends SHAKE_ALERT frame directly (high-priority TX).
// Does NOT wait for a gateway command — this is the ONE autonomous action.
// Skips if a sensor read is already in progress to avoid I2C contention.
// =====================================================
void checkShakeAndAlert() {
  if (!mpuOk) return;
  if (gEventState.measureInProgress) return;

  uint8_t b[14];
  if (!i2cReadN(MPU_ADDR, REG_ACCEL_XOUT_H, b, sizeof(b))) return;

  int16_t axRaw = (int16_t)((b[0] << 8) | b[1]);
  int16_t ayRaw = (int16_t)((b[2] << 8) | b[3]);
  int16_t azRaw = (int16_t)((b[4] << 8) | b[5]);

  const float lsb  = 4096.0f;
  float axG = (float)axRaw / lsb;
  float ayG = (float)ayRaw / lsb;
  float azG = (float)azRaw / lsb;

  float amag     = sqrtf(axG * axG + ayG * ayG + azG * azG);
  float dynamicG = fabsf(amag - 1.0f);

  if (dynamicG < SHAKE_THRESHOLD_G) return;
  if ((millis() - gEventState.lastShakeAt) < SHAKE_COOLDOWN_MS) return;

  // ── Shake confirmed ──
  gEventState.lastShakeAt      = millis();
  gEventState.lastEvent        = "SHAKE";
  gEventState.isExploding      = true;
  gEventState.explosionStartAt = millis();

  rgbFlash(255, 0, 80, 700);

  // Build compact JSON payload
  StaticJsonDocument<96> doc;
  doc["e"]  = "SHAKE";
  doc["ag"] = round(amag     * 100.0f) / 100.0f;
  doc["dg"] = round(dynamicG * 100.0f) / 100.0f;

  String payload;
  serializeJson(doc, payload);

  Serial.println("[SHAKE] Alert detected — sending SHAKE_ALERT frame");

  // sendShakeAlert acquires gLoRaMutex with portMAX_DELAY —
  // it will wait as long as needed; shake is highest priority.
  if (!gPendingShake.pending) {   // garde seulement le plus fort si plusieurs
  gPendingShake.pending  = true;
  gPendingShake.amag     = amag;
  gPendingShake.dynamicG = dynamicG;
}
}

// =====================================================
// initApp
// =====================================================
bool initApp() {

  if (!loadRuntimeEncKey()) {
  Serial.println("[INIT] ERROR: runtime AES key not available");
  return false;
}
  rgbInit();
  sensors.begin();

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);

  pinMode(TURB_ADC_PIN, INPUT);
  pinMode(TDS_ADC_PIN,  INPUT);
  pinMode(PH_ADC_PIN,   INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(TURB_ADC_PIN, ADC_11db);
  analogSetPinAttenuation(TDS_ADC_PIN,  ADC_11db);
  analogSetPinAttenuation(PH_ADC_PIN,   ADC_11db);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  displayInit();

  inaOk = ina219.begin();
  mpuOk = mpuInit();

  loraInit();
  return true;
}