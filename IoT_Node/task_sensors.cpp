#include "task_sensors.h"
#include "app_state.h"
#include "lora_radio.h"

// =====================================================
// buildSensorJson — serialise gSensorData into the
// compact wire format used since the beginning.
// Called inside the mutex after readSensorsRoutine().
// =====================================================
static String buildSensorJson() {
  StaticJsonDocument<256> doc;

  doc["b"]  = gSensorData.battPercent;
  doc["v"]  = round(gSensorData.battLoadV     * 100.0f) / 100.0f;
  doc["m"]  = (int)gSensorData.battCurrentmA;

  doc["p"]  = round(gSensorData.lastPhValue   * 100.0f) / 100.0f;
  doc["ps"] = gSensorData.phScale10;

  doc["t"]  = (int)gSensorData.lastTdsValue;
  doc["ts"] = gSensorData.tdsScale10;

  doc["u"]  = round(gSensorData.lastTurbSensorVoltage * 100.0f) / 100.0f;
  doc["us"] = gSensorData.turbScale10;

  doc["tw"] = round(gSensorData.waterTempC * 10.0f) / 10.0f;
  doc["tm"] = round(gSensorData.mpuTempC   * 10.0f) / 10.0f;
  doc["te"] = round(gSensorData.espTempC   * 10.0f) / 10.0f;

  doc["e"]  = gEventState.lastEvent;

  String out;
  serializeJson(doc, out);
  return out;
}

// =====================================================
// SensorTask
//
// Sleeps indefinitely on ulTaskNotifyTake().
// Wakes only when ControlTask calls xTaskNotifyGive()
// in response to a MEASURE_REQ from the gateway.
//
// Flow on each wakeup:
//   1. Acquire gDataMutex
//   2. Read all sensors (~5 s)
//   3. Build JSON
//   4. Release gDataMutex
//   5. Send MEASURE_RESP (acquires gLoRaMutex internally)
//
// Runs on Core 1, priority 1 (below ControlTask priority 2).
// =====================================================
static void sensorsTask(void *pv) {
  // Store own handle so ControlTask can notify us
  gSensorTaskHandle = xTaskGetCurrentTaskHandle();

  Serial.println("[SENSOR TASK] Ready — waiting for MEASURE_REQ notification");

  for (;;) {
    // Block here until ControlTask sends xTaskNotifyGive()
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (!gNodeActive) {
      Serial.println("[SENSOR TASK] Node not active — ignoring notification");
      continue;
    }

    Serial.println("[SENSOR TASK] MEASURE_REQ received — starting sensor read");

    // ── Acquire data mutex for the full sensor read ──
    if (xSemaphoreTake(gDataMutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
      Serial.println("[SENSOR TASK] ERROR: could not acquire gDataMutex — skipping");
      continue;
    }

    readSensorsRoutine();   // ~5 s — sets gEventState.measureInProgress internally
    gEventState.lastEvent = "None"; 
    String json = buildSensorJson();

    xSemaphoreGive(gDataMutex);

    // ── Send MEASURE_RESP ──
    // sendMeasureResp() acquires gLoRaMutex internally.
    // The gateway has already stopped retrying (it got the early ACK
    // from pollCommandFrame before this task even started reading).
    Serial.println("[SENSOR TASK] Sending MEASURE_RESP");
    bool ok = sendMeasureResp(json);

    if (ok) {
      Serial.println("[SENSOR TASK] MEASURE_RESP delivered");
    } else {
      Serial.println("[SENSOR TASK] MEASURE_RESP delivery failed — data lost this cycle");
    }
  }
}

void startSensorsTask() {
  xTaskCreatePinnedToCore(
    sensorsTask,
    "SensorsTask",
    12288,
    nullptr,
    1,          // Priority 1 — below ControlTask(2) and MpuTask(2)
    nullptr,    // Handle stored via xTaskGetCurrentTaskHandle() inside the task
    1           // Core 1
  );
}
