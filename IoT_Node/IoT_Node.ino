#include <Arduino.h>
#include "config.h"
#include "app_state.h"
#include "task_sensors.h"
#include "task_display.h"
#include "task_mpu.h"
#include "task_control.h"
#include "lora_radio.h"
#include "display_oled.h"
#include "pairing_mode.h"
#include "pairing_store.h"

enum class BootMode {
  PAIRING,
  NORMAL
};

static BootMode gMode = BootMode::PAIRING;

// =====================================================
// startNormalRuntime
//
// Boot sequence in paired mode:
//   1. initApp()  — hardware, I2C, display, LoRa
//   2. Create mutexes
//   3. Start FreeRTOS tasks
//   4. ControlTask waits for gateway ACTIVATE frame
// =====================================================
static void startNormalRuntime() {
  Serial.println("[BOOT] Paired configuration found → NORMAL mode");

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

// =====================================================
// setup
// =====================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println("\n=== IoT Node — boot ===");

  PairingStore::begin();

  if (!PairingStore::hasPairing()) {
    gMode = BootMode::PAIRING;
    Serial.println("[BOOT] No pairing found → BLE PAIRING mode");
    PairingMode::begin();
    return;
  }

  gMode = BootMode::NORMAL;
  startNormalRuntime();
}

void loop() {
  switch (gMode) {
    case BootMode::PAIRING:
      PairingMode::loop();
      delay(20);
      break;

    case BootMode::NORMAL:
      // All work done in FreeRTOS tasks.
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;
  }
}