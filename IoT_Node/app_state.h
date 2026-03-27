#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_INA219.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ================= OLED =================
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_ADDR      0x3C
#define I2C_SDA 19
#define I2C_SCL 20

// ================= INA219 =================
extern Adafruit_INA219 ina219;
extern bool inaOk;

// ================= DS18B20 =================
#define ONE_WIRE_BUS 21
extern OneWire oneWire;
extern DallasTemperature sensors;

// ================= MPU6050 =================
#define MPU_ADDR 0x68
#define REG_PWR_MGMT_1   0x6B
#define REG_ACCEL_CONFIG 0x1C
#define REG_ACCEL_XOUT_H 0x3B
#define REG_TEMP_OUT_H   0x41

extern const uint8_t ACCEL_RANGE;
extern bool mpuOk;

// ================= RGB =================
#define RGB_PIN 48
#define RGB_COUNT 1
extern Adafruit_NeoPixel pixel;
extern bool rgbActive;
extern uint32_t rgbOffAt;

// ================= LoRa =================
extern const int LORA_RST;
extern const int LORA_DIO0;
extern const int LORA_NSS;
extern const int LORA_MOSI;
extern const int LORA_MISO;
extern const int LORA_SCK;
extern const int LORA_DIO1;
extern const int LORA_DIO2;

extern const long LORA_FREQ;
extern const int  LORA_SF;
extern const long LORA_BW;
extern const int  LORA_CR;
extern const uint8_t LORA_SYNC_WORD;

// ================= Battery =================
extern const float BATT_EMPTY_V;
extern const float BATT_FULL_V;

// ================= Capteurs eau =================
#define SENSOR_POWER_PIN 40
#define TURB_ADC_PIN 6
#define TDS_ADC_PIN 2
#define PH_ADC_PIN 17

extern const uint32_t SENSOR_WARMUP_MS;
extern const uint32_t SENSOR_RC_SETTLE_MS;
extern const float TURB_DIVIDER_RTOP;
extern const float TURB_DIVIDER_RBOT;

// ================= Shake =================
extern const float SHAKE_THRESHOLD_G;
extern const uint32_t SHAKE_COOLDOWN_MS;

// ================= Timing =================
extern const uint32_t OLED_UPDATE_MS;
extern const uint32_t AUTO_READ_INTERVAL_MS;

// ================= Security / protocol =================
extern const uint8_t  PROTO_VERSION;
extern const uint32_t DEVICE_ID;
extern const uint8_t  ENC_KEY[16];
extern const uint8_t  HMAC_KEY[16];
extern const uint8_t  ACK_RETRY_MAX;
extern const uint32_t ACK_TIMEOUT_MS;
extern uint32_t gTxSeq;

// ================= Shared data =================
struct SensorData {
  float waterTempC = 25.0f;
  float mpuTempC = 0.0f;
  float espTempC = 0.0f;

  float battBusV = 0.0f;
  float battShuntmV = 0.0f;
  float battCurrentmA = 0.0f;
  float battPowermW = 0.0f;
  float battLoadV = 0.0f;
  int battPercent = 0;

  float lastTurbSensorVoltage = 0.0f;
  int turbScale10 = 0;

  float lastTdsValue = 0.0f;
  int tdsScale10 = 0;

  float lastPhVoltage = 0.0f;
  float lastPhValue = 7.0f;
  int phScale10 = 0;
};

struct EventState {
  uint32_t lastShakeAt = 0;
  String lastEvent = "None";

  bool isExploding = false;
  uint32_t explosionStartAt = 0;

  bool sensorReadingInProgress = false;
};

extern SensorData gSensorData;
extern EventState gEventState;

extern float phVoltageAtNeutral;
extern float phSlope;

// ================= Global devices =================
extern Adafruit_SSD1306 display;

// ================= Mutex =================
extern SemaphoreHandle_t gDataMutex;
extern SemaphoreHandle_t gI2CMutex;
extern SemaphoreHandle_t gLoRaMutex;

// ================= Init =================
void initApp();

// ================= Low-level I2C / MPU =================
bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t val);
bool i2cReadN(uint8_t addr, uint8_t startReg, uint8_t *buf, size_t n);
bool mpuInit();
void readMpuTemp();

// ================= RGB =================
void rgbInit();
void rgbFlash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);
void updateRgb();

// ================= Battery =================
void updateIna219();

// ================= Scores / messages =================
int calculateTurbidityScale(float sensorV);
int calculateTdsScale(float ppm);
int calculatePhScale(float ph);

String getTdsConclusion(int ppm);
String getTurbConclusion(int score);
String getPhConclusion(float ph);

// ================= Main sensor routine =================
void readSensorsRoutine();
void checkShakeAndSend();