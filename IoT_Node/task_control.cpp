#include "task_control.h"
#include "lora_radio.h"
#include "app_state.h"

// =====================================================
// ControlTask
//
// Sole responsibility: listen for gateway commands and
// dispatch them via pollCommandFrame().
//
// All command handling (ACK, dispatch, mutex) happens
// inside pollCommandFrame() in lora_radio.cpp.
//
// This task runs on Core 1, priority 2.
// SensorTask also runs on Core 1 (priority 1) and only
// wakes when ControlTask sends it a TaskNotification —
// so there is no autonomous sensor work competing here.
// =====================================================
static void controlTask(void *pv) {
  // Let LoRa and app init settle before listening
  vTaskDelay(pdMS_TO_TICKS(1500));

  Serial.println("[CTRL TASK] Started — listening for gateway commands");
  Serial.println("[CTRL TASK] Waiting for ACTIVATE from gateway...");

  for (;;) {
    // pollCommandFrame blocks up to 300 ms then returns.
    // When a frame arrives it: ACKs it, dispatches it.
    // When MEASURE_REQ arrives it sends xTaskNotifyGive to SensorTask.
    pollCommandFrame(300);

    // Yield briefly between listen windows
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void startControlTask() {
  xTaskCreatePinnedToCore(
    controlTask,
    "ControlTask",
    4096,
    nullptr,
    2,        // Priority 2 — same as MpuTask, higher than SensorTask(1)
    nullptr,
    1         // Core 1
  );
}
