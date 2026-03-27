#include "task_sensors.h"
#include "app_state.h"
#include "lora_radio.h"

static void sensorsTask(void *pv) {
  // Wait for sensors to initialize
  vTaskDelay(pdMS_TO_TICKS(500));
  
  Serial.println("[SENSORS TASK] Calling readSensorsRoutine (initial)");
  readSensorsRoutine();
  Serial.println("[SENSORS TASK] Calling updateIna219 (initial)");
  updateIna219();
  Serial.println("[SENSORS TASK] Calling sendJsonData (initial)");
  sendJsonData();

  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(AUTO_READ_INTERVAL_MS);

  for (;;) {
    Serial.print("[SENSORS TASK] Attempting to acquire gDataMutex (timeout 10000ms)...");
    // Note: readSensorsRoutine() takes several seconds to complete (analog stabilization)
    // Use longer timeout to accommodate sensor reading time
    if (xSemaphoreTake(gDataMutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
      Serial.println(" SUCCESS");
      Serial.println("[SENSORS TASK] Reading sensors...");
      readSensorsRoutine();
      Serial.println("[SENSORS TASK] Updating INA219...");
      updateIna219();
      Serial.println("[SENSORS TASK] Releasing mutex");
      xSemaphoreGive(gDataMutex);
    } else {
      Serial.println(" FAILED - TIMEOUT");
      Serial.println("[SENSORS] Mutex timeout - sensor reading skipped");
    }

    Serial.println("[SENSORS TASK] Sending JSON data");
    sendJsonData();
    Serial.println("[SENSORS TASK] Waiting for next cycle (60s)");
    vTaskDelayUntil(&lastWake, period);
  }
}

void startSensorsTask() {
  xTaskCreatePinnedToCore(
    sensorsTask,
    "SensorsTask",
    12288,
    nullptr,
    2,
    nullptr,
    1
  );
}