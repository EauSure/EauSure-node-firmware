#include "telemetry.h"
#include "wifi_manager.h"
#include "mqtt_gateway.h"
#include "otaa_manager.h"
#include "config.h"
#include <string.h>

namespace {
constexpr uint32_t kCloudSubmitPreferredHeapFloor = 30000;
constexpr uint32_t kCloudSubmitEmergencyHeapFloor = 24000;
constexpr uint32_t kAudioPlaybackHeapFloor = 22000;
constexpr uint32_t kTelemetryRetryIntervalMs = 5000;
constexpr size_t   kPendingTelemetryCapacity = 6;

struct TelemetryRecord {
  uint32_t seq;
  uint8_t battery;
  float voltage;
  uint16_t current;
  float pH;
  uint8_t phStatus;
  uint16_t tds;
  uint8_t tdsStatus;
  float turbidity;
  uint8_t turbidityStatus;
  float waterTemp;
  float moduleTemp;
  float esp32Temp;
  int8_t rssi;
  float snr;
  char errorMsg[24];
};

TelemetryRecord gPendingTelemetry[kPendingTelemetryCapacity];
size_t gPendingTelemetryHead = 0;
size_t gPendingTelemetryTail = 0;
size_t gPendingTelemetryCount = 0;
uint32_t gLastTelemetryAttemptAt = 0;

bool heapReadyForTask(const char* taskName, uint32_t floorBytes) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t minHeap  = ESP.getMinFreeHeap();

  Serial.printf("[SYS][HEAP] pretask=%s free=%lu min=%lu floor=%lu\n",
                taskName,
                (unsigned long)freeHeap,
                (unsigned long)minHeap,
                (unsigned long)floorBytes);

  if (freeHeap >= floorBytes) {
    return true;
  }

  Serial.printf("[SYS][HEAP] optimization required before %s — task deferred\n", taskName);
  return false;
}

void copyEventString(char* dest, size_t destLen, const char* src) {
  if (destLen == 0) return;
  if (!src) src = "None";

  strncpy(dest, src, destLen - 1);
  dest[destLen - 1] = '\0';
}

TelemetryRecord buildTelemetryRecord(JsonDocument& doc, uint32_t seq, int rssi, float snr) {
  TelemetryRecord record;
  memset(&record, 0, sizeof(record));
  record.seq = seq;
  record.battery = doc["b"] | 0;
  record.voltage = doc["v"] | 0.0f;
  record.current = doc["m"] | 0;
  record.pH = doc["p"] | 0.0f;
  record.phStatus = doc["ps"] | 0;
  record.tds = doc["t"] | 0;
  record.tdsStatus = doc["ts"] | 0;
  record.turbidity = doc["u"] | 0.0f;
  record.turbidityStatus = doc["us"] | 0;
  record.waterTemp = doc["tw"] | 0.0f;
  record.moduleTemp = doc["tm"] | 0.0f;
  record.esp32Temp = doc["te"] | 0.0f;
  record.rssi = (int8_t)rssi;
  record.snr = snr;
  copyEventString(record.errorMsg, sizeof(record.errorMsg), doc["e"] | "None");
  return record;
}

void popPendingTelemetry() {
  if (gPendingTelemetryCount == 0) return;

  gPendingTelemetryHead = (gPendingTelemetryHead + 1) % kPendingTelemetryCapacity;
  --gPendingTelemetryCount;
}

void enqueueTelemetryRecord(const TelemetryRecord& record) {
  if (gPendingTelemetryCount == kPendingTelemetryCapacity) {
    const TelemetryRecord& dropped = gPendingTelemetry[gPendingTelemetryHead];
    Serial.printf("[Telemetry] Queue full — dropping oldest pending seq=%lu to keep latest data flowing\n",
                  (unsigned long)dropped.seq);
    popPendingTelemetry();
  }

  gPendingTelemetry[gPendingTelemetryTail] = record;
  gPendingTelemetryTail = (gPendingTelemetryTail + 1) % kPendingTelemetryCapacity;
  ++gPendingTelemetryCount;

  Serial.printf("[Telemetry] Queued seq=%lu for cloud upload (pending=%u)\n",
                (unsigned long)record.seq,
                (unsigned)gPendingTelemetryCount);
}

bool ensureTelemetryHeapBudget() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t minHeap  = ESP.getMinFreeHeap();

  Serial.printf("[SYS][HEAP] pretask=cloud-submit free=%lu min=%lu floor=%lu\n",
                (unsigned long)freeHeap,
                (unsigned long)minHeap,
                (unsigned long)kCloudSubmitPreferredHeapFloor);

  if (freeHeap >= kCloudSubmitPreferredHeapFloor) {
    return true;
  }

  Serial.println("[SYS][HEAP] optimization required before cloud-submit — pausing MQTT and retrying heap check");
  MqttGateway::setExclusiveTlsWindow(true);
  delay(60);

  const uint32_t optimizedFreeHeap = ESP.getFreeHeap();
  const uint32_t optimizedMinHeap  = ESP.getMinFreeHeap();
  Serial.printf("[SYS][HEAP] optimized=cloud-submit free=%lu min=%lu preferred=%lu emergency=%lu\n",
                (unsigned long)optimizedFreeHeap,
                (unsigned long)optimizedMinHeap,
                (unsigned long)kCloudSubmitPreferredHeapFloor,
                (unsigned long)kCloudSubmitEmergencyHeapFloor);

  if (optimizedFreeHeap >= kCloudSubmitPreferredHeapFloor) {
    return true;
  }

  if (optimizedFreeHeap >= kCloudSubmitEmergencyHeapFloor) {
    Serial.println("[SYS][HEAP] cloud-submit proceeding below preferred floor because telemetry is high priority");
    return true;
  }

  Serial.println("[SYS][HEAP] cloud-submit still below emergency floor — upload deferred");
  MqttGateway::setExclusiveTlsWindow(false);
  return false;
}
}

static String getNodeIdString() {
  uint32_t nodeId = getPairedNodeDeviceId();
  if (nodeId == 0) {
    return String("unpaired");
  }

  char buf[9];
  snprintf(buf, sizeof(buf), "%08lX", (unsigned long)nodeId);
  return String(buf);
}

static String getGatewayHardwareIdString() {
  String mac = WiFiManager::getMacAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  return mac;
}
// =====================================================
// collectAlertFiles — queue WAV alerts based on payload
// =====================================================
void collectAlertFiles(JsonDocument& doc, int rssi) {
  clearAlertQueue();

  String event = doc["e"] | "";

  int   batPct = doc["b"]  | 100;
  int   ph10   = doc["ps"] | 10;
  int   tds10  = doc["ts"] | 10;
  int   turb10 = doc["us"] | 10;

  if (event == "SHAKE") {
    queueAlert("/alerts/alert_fall.wav");
  }

  if (batPct <= 10)      queueAlert("/alerts/alert_BAT_high.wav");
  else if (batPct <= 25) queueAlert("/alerts/alert_BAT_medium.wav");

  if (ph10 <= 4)        queueAlert("/alerts/alert_pH_high.wav");
  else if (ph10 <= 7)   queueAlert("/alerts/alert_pH_medium.wav");

  if (tds10 <= 4)       queueAlert("/alerts/alert_TDS_high.wav");
  else if (tds10 <= 7)  queueAlert("/alerts/alert_TDS_medium.wav");

  if (turb10 <= 4)      queueAlert("/alerts/alert_TURBIDITY_high.wav");
  else if (turb10 <= 7) queueAlert("/alerts/alert_TURBIDITY_medium.wav");

  if (rssi < -115)      queueAlert("/alerts/alert_LoRa.wav");
}

// =====================================================
// printDataPayload — serial pretty-print
// =====================================================
static void printDataPayload(JsonDocument& doc, uint32_t seq,
                              int rssi, float snr, bool isShake) {
  Serial.println("\n========= DATA FRAME RECEIVED =========");
  Serial.printf("SEQ         : %lu\n", (unsigned long)seq);
  Serial.printf("TYPE        : %s\n", isShake ? "SHAKE_ALERT" : "MEASURE_RESP");
  Serial.printf("LoRa Signal : RSSI %d dBm | SNR %.1f dB\n", rssi, snr);
  Serial.println("---------------------------------------");

  if (doc.containsKey("b"))
    Serial.printf("BATTERY     : %d%% | %.2f V | %.0f mA\n",
                  doc["b"].as<int>(), doc["v"].as<float>(), doc["m"].as<float>());
  if (doc.containsKey("p"))
    Serial.printf("pH          : %.2f | Score: %d/10\n",
                  doc["p"].as<float>(), doc["ps"].as<int>());
  if (doc.containsKey("t"))
    Serial.printf("TDS         : %d ppm | Score: %d/10\n",
                  doc["t"].as<int>(), doc["ts"].as<int>());
  if (doc.containsKey("u"))
    Serial.printf("TURBIDITY   : %.2f V | Score: %d/10\n",
                  doc["u"].as<float>(), doc["us"].as<int>());
  if (doc.containsKey("tw"))
    Serial.printf("TEMP WATER  : %.1f °C\n", doc["tw"].as<float>());
  if (doc.containsKey("tm"))
    Serial.printf("TEMP MPU    : %.1f °C\n", doc["tm"].as<float>());
  if (doc.containsKey("te"))
    Serial.printf("TEMP ESP32  : %.1f °C\n", doc["te"].as<float>());

  if (isShake)
    Serial.printf("SHAKE FORCE : %.2f G (dynamic: %.2f G)\n",
                  doc["ag"].as<float>(), doc["dg"].as<float>());

  Serial.println("=======================================\n");
}

// =====================================================
// submitToCloud — WiFi POST
// =====================================================
static bool submitToCloud(const TelemetryRecord& record) {
  Serial.println("[Cloud] Submitting to API...");

  String nodeId = getNodeIdString();
  String gatewayHardwareId = getGatewayHardwareIdString();

  bool ok = WiFiManager::submitSensorData(
    nodeId.c_str(),
    gatewayHardwareId.c_str(),
    record.seq,
    record.battery,
    record.voltage,
    record.current,
    record.pH,
    record.phStatus,
    record.tds,
    record.tdsStatus,
    record.turbidity,
    record.turbidityStatus,
    record.waterTemp,
    record.moduleTemp,
    record.esp32Temp,
    record.errorMsg,
    record.rssi,
    record.snr
  );

  Serial.println(ok ? "[Cloud] ✓ Sent successfully" : "[Cloud] ✗ Send failed");
  return ok;
}

bool telemetryHasPendingUpload() {
  return gPendingTelemetryCount > 0;
}

void telemetryTick() {
  if (gPendingTelemetryCount == 0) {
    return;
  }

  if (audioBusy || shouldPauseBackgroundWork()) {
    return;
  }

  const uint32_t now = millis();
  if (gLastTelemetryAttemptAt != 0 && (now - gLastTelemetryAttemptAt) < kTelemetryRetryIntervalMs) {
    return;
  }

  gLastTelemetryAttemptAt = now;
  const TelemetryRecord& record = gPendingTelemetry[gPendingTelemetryHead];

  if (!ensureTelemetryHeapBudget()) {
    Serial.printf("[Telemetry] Pending seq=%lu will retry later (pending=%u)\n",
                  (unsigned long)record.seq,
                  (unsigned)gPendingTelemetryCount);
    return;
  }

  if (submitToCloud(record)) {
    popPendingTelemetry();
    Serial.printf("[Telemetry] Upload complete for seq=%lu (remaining=%u)\n",
                  (unsigned long)record.seq,
                  (unsigned)gPendingTelemetryCount);
  } else {
    Serial.printf("[Telemetry] Upload failed for seq=%lu — leaving it queued for retry\n",
                  (unsigned long)record.seq);
  }
}
// =====================================================
// handleDataPayload
//
// Single entry point for all MSG_TYPE_DATA frames.
// Dispatches on the "e" field:
//   "SHAKE" → SHAKE_ALERT path
//   anything else → MEASURE_RESP path
//
// ACK has already been sent before this is called.
// =====================================================
void handleDataPayload(const char *json, int rssi, float snr) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("[Telemetry] JSON parse error: %s\n", err.c_str());
    return;
  }

  // Use a local seq counter based on lastAcceptedSeq which was set by caller
  uint32_t seq     = lastAcceptedSeq;
  String   event   = doc["e"] | "None";
  bool     isShake = (event == "SHAKE");

  printDataPayload(doc, seq, rssi, snr, isShake);

  // ── SHAKE_ALERT path ──
  if (isShake) {
    Serial.println("[Telemetry] SHAKE_ALERT received — triggering alarm");
    startAlarm();

    enqueueTelemetryRecord(buildTelemetryRecord(doc, seq, rssi, snr));
    telemetryTick();

    // Queue fall audio alert and play
    collectAlertFiles(doc, rssi);
    if (telemetryHasPendingUpload()) {
      Serial.println("[Telemetry] Deferring queued alerts until cloud backlog is cleared");
      clearAlertQueue();
    } else if (heapReadyForTask("audio-playback", kAudioPlaybackHeapFloor)) {
      playQueuedAlerts();
    } else {
      Serial.println("[Telemetry] Skipping queued alerts due low heap");
      clearAlertQueue();
    }

    // Follow-up MEASURE_REQ — get fresh sensor state after the event
    Serial.println("[Telemetry] Sending follow-up MEASURE_REQ after shake");
    if (sendMeasureReq()) {
      notifyMeasureRequestDispatched();
    } else {
      Serial.println("[Telemetry] Follow-up MEASURE_REQ failed");
    }
    return;
  }

  // ── MEASURE_RESP path ──
  notifyMeasureResponseHandled();
  collectAlertFiles(doc, rssi);
  enqueueTelemetryRecord(buildTelemetryRecord(doc, seq, rssi, snr));
  telemetryTick();

  if (telemetryHasPendingUpload()) {
    Serial.println("[Telemetry] Deferring queued alerts until cloud backlog is cleared");
    clearAlertQueue();
  } else if (heapReadyForTask("audio-playback", kAudioPlaybackHeapFloor)) {
    playQueuedAlerts();
  } else {
    Serial.println("[Telemetry] Skipping queued alerts due low heap");
    clearAlertQueue();
  }
}
