#include <Arduino.h>
#include "config.h"
#include "app_state.h"
#include "task_sensors.h"
#include "task_display.h"
#include "task_mpu.h"
#include "lora_radio.h"
#include "display_oled.h"

void setup() {
  Serial.begin(115200);
  delay(300);

  initApp();

  gDataMutex = xSemaphoreCreateMutex();
  gI2CMutex  = xSemaphoreCreateMutex();
  gLoRaMutex = xSemaphoreCreateMutex();

  startSensorsTask();
  startDisplayTask();
  startMpuTask();
}

void loop() {
  vTaskDelete(nullptr);
}