#include "task_mpu.h"
#include "app_state.h"

// =====================================================
// MpuTask
//
// Runs at 20 Hz on Core 0.
// Calls checkShakeAndAlert() which:
//   - reads accelerometer
//   - on shake: calls sendShakeAlert() with portMAX_DELAY mutex
//     (highest priority TX — will wait out any ongoing sensor read)
//
// Skips if node is not yet active (not yet ACTIVATE'd by gateway).
// =====================================================
static void mpuTask(void *pv) {
  TickType_t       lastWake = xTaskGetTickCount();
  const TickType_t period   = pdMS_TO_TICKS(50); // 20 Hz

  for (;;) {
    // Only check for shake once the gateway has activated the node.
    // This prevents spurious alerts during boot handling.
    if (gNodeActive) {
      if (xSemaphoreTake(gDataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        checkShakeAndAlert();
        xSemaphoreGive(gDataMutex);
      }
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
    2,          // Priority 2 — same as ControlTask
    nullptr,
    0           // Core 0 — separate from ControlTask/SensorTask (Core 1)
  );
}
