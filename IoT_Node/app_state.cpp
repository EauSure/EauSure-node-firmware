#include "app_state.h"
#include "lora_radio.h"
#include "display_oled.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

Adafruit_INA219 ina219;
bool inaOk = false;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

bool mpuOk = false;

Adafruit_NeoPixel pixel(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);
bool rgbActive = false;
uint32_t rgbOffAt = 0;

SensorData gSensorData;
EventState gEventState;
volatile bool gNodeActive = true;

float phVoltageAtNeutral = PH_VOLTAGE_AT_NEUTRAL;
float phSlope = PH_SLOPE;

SemaphoreHandle_t gDataMutex = nullptr;
SemaphoreHandle_t gI2CMutex  = nullptr;
SemaphoreHandle_t gLoRaMutex = nullptr;

uint32_t gTxSeq = 1;

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

bool mpuInit() {
  if (!i2cWrite8(MPU_ADDR, REG_PWR_MGMT_1, 0x00)) return false;
  delay(50);
  uint8_t accelCfg = (ACCEL_RANGE & 0x03) << 3;
  if (!i2cWrite8(MPU_ADDR, REG_ACCEL_CONFIG, accelCfg)) return false;
  return true;
}

void readMpuTemp() {
  if (!mpuOk) return;

  uint8_t b[2];
  if (i2cReadN(MPU_ADDR, REG_TEMP_OUT_H, b, 2)) {
    int16_t rawT = (b[0] << 8) | b[1];
    gSensorData.mpuTempC = (rawT / 340.0f) + 36.53f;
  }
}

void rgbInit() {
  pixel.begin();
  pixel.clear();
  pixel.show();
}

void rgbFlash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
  rgbActive = true;
  rgbOffAt = millis() + ms;
}

void updateRgb() {
  if (rgbActive && (int32_t)(rgbOffAt - millis()) <= 0) {
    rgbActive = false;
    pixel.setPixelColor(0, pixel.Color(0, 0, 0));
    pixel.show();
  }
}

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
  if (ph >= 6.5 && ph <= 8.5) return "Ideal";
  if (ph < 6.5) return "Acide";
  return "Basique";
}

void readSensorsRoutine() {
  gEventState.sensorReadingInProgress = true;

  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C) {
    gSensorData.waterTempC = t;
  }

  digitalWrite(SENSOR_POWER_PIN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(SENSOR_WARMUP_MS));
  vTaskDelay(pdMS_TO_TICKS(SENSOR_RC_SETTLE_MS));

  for (int i = 0; i < 8; i++) {
    analogRead(TURB_ADC_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  uint32_t tSum = 0;
  for (int i = 0; i < 20; i++) {
    tSum += analogRead(TURB_ADC_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  float tAdcV = ((tSum / 20.0f) * 3.3f) / 4095.0f;
  gSensorData.lastTurbSensorVoltage = tAdcV * ((TURB_DIVIDER_RTOP + TURB_DIVIDER_RBOT) / TURB_DIVIDER_RBOT);
  gSensorData.turbScale10 = calculateTurbidityScale(gSensorData.lastTurbSensorVoltage);

  for (int i = 0; i < 8; i++) {
    analogRead(TDS_ADC_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  uint32_t tdsSum = 0;
  for (int i = 0; i < 20; i++) {
    tdsSum += analogRead(TDS_ADC_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  float tdsV = ((tdsSum / 20.0f) * 3.3f) / 4095.0f;
  float compensationCoeff = 1.0 + 0.02 * (gSensorData.waterTempC - 25.0);
  float compV = tdsV / compensationCoeff;

  gSensorData.lastTdsValue = (133.42f * pow(compV, 3) - 255.86f * pow(compV, 2) + 857.39f * compV) * 0.5f;
  if (gSensorData.lastTdsValue < 0) gSensorData.lastTdsValue = 0;
  gSensorData.tdsScale10 = calculateTdsScale(gSensorData.lastTdsValue);

  for (int i = 0; i < 8; i++) {
    analogRead(PH_ADC_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  uint32_t phSum = 0;
  for (int i = 0; i < 20; i++) {
    phSum += analogRead(PH_ADC_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  float phAdcV = ((phSum / 20.0f) * 3.3f) / 4095.0f;
  gSensorData.lastPhVoltage = phAdcV * 2.5f;
  gSensorData.lastPhValue = 7.0f + ((phVoltageAtNeutral - gSensorData.lastPhVoltage) / -phSlope);
  gSensorData.lastPhValue = constrain(gSensorData.lastPhValue, 0.0f, 14.0f);
  gSensorData.phScale10 = calculatePhScale(gSensorData.lastPhValue);

  digitalWrite(SENSOR_POWER_PIN, LOW);
  gEventState.sensorReadingInProgress = false;

  gSensorData.espTempC = temperatureRead();
  readMpuTemp();
}

void checkShakeAndSend() {
  if (gEventState.sensorReadingInProgress || !mpuOk) return;

  uint8_t b[14];
  if (i2cReadN(MPU_ADDR, REG_ACCEL_XOUT_H, b, sizeof(b))) {
    int16_t axRaw = (int16_t)((b[0] << 8) | b[1]);
    int16_t ayRaw = (int16_t)((b[2] << 8) | b[3]);
    int16_t azRaw = (int16_t)((b[4] << 8) | b[5]);

    float lsb = 4096.0f;
    float axG = (float)axRaw / lsb;
    float ayG = (float)ayRaw / lsb;
    float azG = (float)azRaw / lsb;

    float amag = sqrtf(axG * axG + ayG * ayG + azG * azG);
    float dynamicG = fabsf(amag - 1.0f);

    if (dynamicG >= SHAKE_THRESHOLD_G && (millis() - gEventState.lastShakeAt > SHAKE_COOLDOWN_MS)) {
      gEventState.lastShakeAt = millis();
      gEventState.lastEvent = "SHAKE";
      gEventState.isExploding = true;
      gEventState.explosionStartAt = millis();

      rgbFlash(255, 0, 80, 700);

      StaticJsonDocument<64> doc;
      doc["e"] = "ALARM_SHAKE";  // Use compressed field name to match sensor JSON format
      doc["ag"] = round(amag * 100.0) / 100.0;        // Total acceleration magnitude (G)
      doc["dg"] = round(dynamicG * 100.0) / 100.0;    // Dynamic acceleration (deviation from 1G)

      String out;
      serializeJson(doc, out);
      secureSendJson(out);
    }
  }
}

void initApp() {
  rgbInit();
  sensors.begin();

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);

  pinMode(TURB_ADC_PIN, INPUT);
  pinMode(TDS_ADC_PIN, INPUT);
  pinMode(PH_ADC_PIN, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(TURB_ADC_PIN, ADC_11db);
  analogSetPinAttenuation(TDS_ADC_PIN, ADC_11db);
  analogSetPinAttenuation(PH_ADC_PIN, ADC_11db);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  displayInit();

  inaOk = ina219.begin();
  mpuOk = mpuInit();
  loraInit();
}
