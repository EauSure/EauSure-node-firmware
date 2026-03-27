#include "task_mpu.h"
#include "app_state.h"

static void mpuTask(void *pv) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(50); // 20 Hz

  for (;;) {
    if (xSemaphoreTake(gDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      checkShakeAndSend();
      xSemaphoreGive(gDataMutex);
    }

    vTaskDelayUntil(&lastWake, period);
  }
}

void startMpuTask() {
  xTaskCreatePinnedToCore(
    mpuTask,
    "MpuTask",
    4096,
    nullptr,
    2,
    nullptr,
    0
  );
}