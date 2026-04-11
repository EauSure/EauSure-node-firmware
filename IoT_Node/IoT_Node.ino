#include <Arduino.h>
#include "config.h"
#include "app_state.h"
#include "task_sensors.h"
#include "task_display.h"
#include "task_mpu.h"
#include "task_control.h"
#include "lora_radio.h"
#include "display_oled.h"

// =====================================================
// setup
//
// Boot sequence:
//   1. initApp()  — hardware, I2C, LoRa, display
//   2. Create mutexes and start FreeRTOS tasks
//   3. ControlTask waits for gateway ACTIVATE frame
//   4. On ACTIVATE: gNodeActive = true, ACTIVATE_OK sent
//   5. Gateway then drives all further activity
// =====================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println("\n=== IoT Node — Gateway-commanded mode ===");

  initApp();

  gDataMutex = xSemaphoreCreateMutex();
  gI2CMutex  = xSemaphoreCreateMutex();
  gLoRaMutex = xSemaphoreCreateMutex();

  // Start sensor task first so gSensorTaskHandle is populated
  // before ControlTask could potentially dispatch a MEASURE_REQ.
  startSensorsTask();
  startDisplayTask();
  startMpuTask();
  startControlTask();

  Serial.println("[SETUP] All tasks started — waiting for gateway ACTIVATE");
}

void loop() {
  // All work done in FreeRTOS tasks.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
