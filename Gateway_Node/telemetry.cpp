#include "telemetry.h"
#include "wifi_manager.h"
#include "otaa_manager.h"
#include "config.h"

static String getNodeIdString() {
  char buf[11];
  snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)DEVICE_ID);
  return String(buf);
}

static String getGatewayHardwareIdString() {
  return WiFiManager::getMacAddress();
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
static void submitToCloud(JsonDocument& doc, uint32_t seq, int rssi, float snr) {
  Serial.println("[Cloud] Submitting to API...");

  String nodeId = getNodeIdString();
  String gatewayHardwareId = getGatewayHardwareIdString();

  bool ok = WiFiManager::submitSensorData(
    nodeId.c_str(),
    gatewayHardwareId.c_str(),
    seq,
    doc["b"]  | 0,
    doc["v"]  | 0.0f,
    doc["m"]  | 0,
    doc["p"]  | 0.0f,
    doc["ps"] | 0,
    doc["t"]  | 0,
    doc["ts"] | 0,
    doc["u"]  | 0.0f,
    doc["us"] | 0,
    doc["tw"] | 0.0f,
    doc["tm"] | 0.0f,
    doc["te"] | 0.0f,
    (doc["e"] | "None"),
    rssi,
    snr
  );

  Serial.println(ok ? "[Cloud] ✓ Sent successfully" : "[Cloud] ✗ Send failed");
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

    // POST to cloud immediately with e=SHAKE
    submitToCloud(doc, seq, rssi, snr);

    // Queue fall audio alert and play
    collectAlertFiles(doc, rssi);
    playQueuedAlerts();

    // Follow-up MEASURE_REQ — get fresh sensor state after the event
    Serial.println("[Telemetry] Sending follow-up MEASURE_REQ after shake");
    if (!sendMeasureReq()) {
      Serial.println("[Telemetry] Follow-up MEASURE_REQ failed");
    }
    return;
  }

  // ── MEASURE_RESP path ──
  collectAlertFiles(doc, rssi);
  playQueuedAlerts();
  submitToCloud(doc, seq, rssi, snr);
}
