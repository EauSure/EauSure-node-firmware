#include "task_display.h"
#include "app_state.h"
#include "display_oled.h"

static void displayTask(void *pv) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(OLED_UPDATE_MS);

  for (;;) {
    if (xSemaphoreTake(gDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      updateRgb();
      updateIna219();
      renderOLED();
      xSemaphoreGive(gDataMutex);
    }

    vTaskDelayUntil(&lastWake, period);
  }
}

void startDisplayTask() {
  xTaskCreatePinnedToCore(
    displayTask,
    "DisplayTask",
    8192,
    nullptr,
    1,
    nullptr,
    0
  );
}