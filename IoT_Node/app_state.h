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
extern OneWire oneWire;
extern DallasTemperature sensors;

// ================= MPU6050 =================
extern bool mpuOk;

// ================= RGB =================
extern Adafruit_NeoPixel pixel;
extern bool rgbActive;
extern uint32_t rgbOffAt;

// ================= Security / protocol =================
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
extern volatile bool gNodeActive;

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
