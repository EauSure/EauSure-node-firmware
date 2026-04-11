#pragma once

#include <Arduino.h>
#include "config.h"
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
#include "mbedtls/gcm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// =====================================================
// Protocol MSG_TYPE constants  (shared with Gateway)
//
//  0x01  DATA         IoT → GW   MEASURE_RESP or SHAKE_ALERT
//  0x02  ACK          both dirs  transport acknowledgement
//  0x03  MEASURE_REQ  GW  → IoT  request a fresh sensor reading
//  0x04  HEARTBEAT_REQ GW → IoT  lightweight liveness ping
//  0x05  HEARTBEAT_ACK IoT → GW  liveness reply {batt%, state}
//  0x06  ACTIVATE     GW  → IoT  boot handshake — activate node
//  0x07  ACTIVATE_OK  IoT → GW   handshake reply {state, mac}
// =====================================================
static const uint8_t MSG_TYPE_DATA          = 0x01;
static const uint8_t MSG_TYPE_ACK           = 0x02;
static const uint8_t MSG_TYPE_MEASURE_REQ   = 0x03;
static const uint8_t MSG_TYPE_HEARTBEAT_REQ = 0x04;
static const uint8_t MSG_TYPE_HEARTBEAT_ACK = 0x05;
static const uint8_t MSG_TYPE_ACTIVATE      = 0x06;
static const uint8_t MSG_TYPE_ACTIVATE_OK   = 0x07;

// =====================================================
// Frame layout constants
// =====================================================
static const size_t GCM_NONCE_LEN = 12;
static const size_t GCM_TAG_LEN   = 16;
static const size_t CRC_LEN       = 2;
// Header: [ver(1) | type(1) | deviceId(4) | seq(4) | nonce(12) | plainLen(2)] = 24 bytes
static const size_t HEADER_LEN    = 1 + 1 + 4 + 4 + GCM_NONCE_LEN + 2;
static const size_t MAX_PLAIN_LEN = 180;
static const size_t MAX_FRAME_LEN = HEADER_LEN + MAX_PLAIN_LEN + GCM_TAG_LEN + CRC_LEN;

// =====================================================
// OLED
// =====================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
#define I2C_SDA       19
#define I2C_SCL       20

// =====================================================
// INA219
// =====================================================
extern Adafruit_INA219 ina219;
extern bool inaOk;

// =====================================================
// DS18B20
// =====================================================
extern OneWire oneWire;
extern DallasTemperature sensors;

// =====================================================
// MPU6050
// =====================================================
extern bool mpuOk;

// =====================================================
// RGB NeoPixel
// =====================================================
extern Adafruit_NeoPixel pixel;
extern bool rgbActive;
extern uint32_t rgbOffAt;

// =====================================================
// Sequence counters
//   gTxSeq     — incremented for every DATA / ACK_OK frame IoT sends
//   gCtrlRxSeq — last accepted sequence from Gateway commands
//                used for replay protection on incoming frames
// =====================================================
extern uint32_t gTxSeq;
extern uint32_t gCtrlRxSeq;

// =====================================================
// Shared sensor data
// =====================================================
struct SensorData {
  float waterTempC  = 25.0f;
  float mpuTempC    = 0.0f;
  float espTempC    = 0.0f;

  float battBusV      = 0.0f;
  float battShuntmV   = 0.0f;
  float battCurrentmA = 0.0f;
  float battPowermW   = 0.0f;
  float battLoadV     = 0.0f;
  int   battPercent   = 0;

  float lastTurbSensorVoltage = 0.0f;
  int   turbScale10           = 0;

  float lastTdsValue = 0.0f;
  int   tdsScale10   = 0;

  float lastPhVoltage = 0.0f;
  float lastPhValue   = 7.0f;
  int   phScale10     = 0;
};

struct EventState {
  uint32_t lastShakeAt      = 0;
  String   lastEvent        = "None";
  bool     isExploding      = false;
  uint32_t explosionStartAt = 0;
  // True while ControlTask is waiting for sensor task to finish —
  // MPU shake TX checks this to avoid stomping an in-progress read.
  bool     measureInProgress = false;
};

extern SensorData      gSensorData;
extern EventState      gEventState;
extern volatile bool   gNodeActive;

extern float phVoltageAtNeutral;
extern float phSlope;

// =====================================================
// Global devices
// =====================================================
extern Adafruit_SSD1306 display;

// =====================================================
// FreeRTOS primitives
// =====================================================
extern SemaphoreHandle_t gDataMutex;
extern SemaphoreHandle_t gI2CMutex;
extern SemaphoreHandle_t gLoRaMutex;

// Handle of the SensorTask — ControlTask sends TaskNotifications to it
extern TaskHandle_t gSensorTaskHandle;

// =====================================================
// App init
// =====================================================
void initApp();

// =====================================================
// Low-level I2C / MPU
// =====================================================
bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t val);
bool i2cReadN(uint8_t addr, uint8_t startReg, uint8_t *buf, size_t n);
bool mpuInit();
void readMpuTemp();

// =====================================================
// RGB
// =====================================================
void rgbInit();
void rgbFlash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);
void updateRgb();

// =====================================================
// Battery
// =====================================================
void updateIna219();

// =====================================================
// Scores / conclusion strings
// =====================================================
int    calculateTurbidityScale(float sensorV);
int    calculateTdsScale(float ppm);
int    calculatePhScale(float ph);
String getTdsConclusion(int ppm);
String getTurbConclusion(int score);
String getPhConclusion(float ph);

// =====================================================
// Sensor routine (called only from SensorTask)
// =====================================================
void readSensorsRoutine();

// =====================================================
// Shake check (called from MpuTask at 20 Hz)
// =====================================================
void checkShakeAndAlert();

// =====================================================
// Binary encoding helpers (used by lora_radio.cpp)
// =====================================================
void     writeU16BE(uint8_t *dst, uint16_t v);
void     writeU32BE(uint8_t *dst, uint32_t v);
uint16_t readU16BE(const uint8_t *src);
uint32_t readU32BE(const uint8_t *src);

// =====================================================
// CRC-16 CCITT
// =====================================================
uint16_t crc16Ccitt(const uint8_t *data, size_t len);

// =====================================================
// Cryptography helpers (used by lora_radio.cpp)
// =====================================================
void buildNonce(uint32_t seq, uint8_t nonce[GCM_NONCE_LEN]);

bool aesgcmDecrypt(
  const uint8_t *cipher,
  size_t cipherLen,
  const uint8_t nonce[GCM_NONCE_LEN],
  const uint8_t *aad,
  size_t aadLen,
  const uint8_t tag[GCM_TAG_LEN],
  uint8_t *plain
);

bool buildSecureFrame(
  uint8_t msgType,
  uint32_t seq,
  const uint8_t *plain,
  uint16_t plainLen,
  uint8_t *outFrame,
  size_t &outLen
);