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

  if (!initApp()) {
  Serial.println("[BOOT] initApp failed");
  while (true) delay(100);
}

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

  // Check if we woke up from deep sleep (measure interval timer)
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
      Serial.println("[WAKEUP] Timer wakeup -> Triggering measurement cycle");
      if (gNodeActive) {
         if (gSensorTaskHandle != nullptr) {
            xTaskNotifyGive(gSensorTaskHandle);
         }
      } else {
         Serial.println("[WAKEUP] Node not active, waiting for ACTIVATE");
      }
  } else if (cause == ESP_SLEEP_WAKEUP_EXT0) {
      Serial.println("[WAKEUP] EXT0 wakeup (Shake) -> Sending immediate SHAKE_ALERT");

      // The node woke because the MPU6050 motion engine detected a shake
      // during deep sleep. The gateway hasn't re-ACTIVATE'd us yet, but
      // a shake is a critical safety event — send it immediately.
      //
      // Force gNodeActive so the LoRa TX path doesn't refuse to send.
      // The gateway will accept the DATA frame regardless of ACTIVATE state
      // because it uses seq-based dedup, not state gating.
      gNodeActive = true;

      // Queue the shake alert for ControlTask to send.
      // We don't have real accelerometer values (the MPU was in motion-detect
      // mode, not continuous read mode), so report the configured threshold
      // as the approximate magnitude.
      gPendingShake.pending  = true;
      gPendingShake.amag     = gRuntimeShakeThresholdG + 1.0f;
      gPendingShake.dynamicG = gRuntimeShakeThresholdG;

      gEventState.lastShakeAt = millis();
      gEventState.lastEvent   = "SHAKE";

      rgbFlash(255, 0, 80, 700);
  }
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
    if (PairingStore::hasPendingProvision()) {
      Serial.println("[BOOT] Pending provision found -> RESUME WIFI PAIRING in STA mode");
    } else {
      Serial.println("[BOOT] No pairing found -> WIFI PAIRING AP mode");
    }
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
