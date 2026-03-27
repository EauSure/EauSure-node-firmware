#include "task_sensors.h"
#include "app_state.h"
#include "lora_radio.h"

static void sensorsTask(void *pv) {
  readSensorsRoutine();
  updateIna219();
  sendJsonData();

  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(AUTO_READ_INTERVAL_MS);

  for (;;) {
    if (xSemaphoreTake(gDataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      readSensorsRoutine();
      updateIna219();
      xSemaphoreGive(gDataMutex);
    }

    sendJsonData();
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