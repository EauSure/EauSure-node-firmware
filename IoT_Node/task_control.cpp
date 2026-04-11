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
  // Ne lire le canal que si secureSend n'est pas en cours,
  // pour éviter de voler l'ACK destiné à waitForAck
  if (uxSemaphoreGetCount(gLoRaMutex) > 0) {
    pollCommandFrame(300);
  } else {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Envoyer le shake en attente si aucune tâche en cours
  if (gPendingShake.pending && !gEventState.measureInProgress) {
    // Vérifier aussi que le mutex est libre avant de tenter l'envoi
    if (uxSemaphoreGetCount(gLoRaMutex) > 0) {
      StaticJsonDocument<96> doc;
      doc["e"]  = "SHAKE";
      doc["ag"] = round(gPendingShake.amag     * 100.0f) / 100.0f;
      doc["dg"] = round(gPendingShake.dynamicG * 100.0f) / 100.0f;
      String payload;
      serializeJson(doc, payload);

      gPendingShake.pending = false;
      sendShakeAlert(payload);
    }
  }

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
