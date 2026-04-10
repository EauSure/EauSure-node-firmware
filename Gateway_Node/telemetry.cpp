#include "telemetry.h"
#include "wifi_manager.h"

// =====================================================
// Collect Alert Files Based on Sensor Data
// =====================================================
void collectAlertFiles(JsonDocument& doc, int rssi) {
  clearAlertQueue();

  String event = doc["e"] | "";

  int batPct = doc["b"] | 100;
  float batMa = doc["m"] | 0.0f;
  int ph10   = doc["ps"] | 10;
  int tds10  = doc["ts"] | 10;
  int turb10 = doc["us"] | 10;
  float espTemp = doc["te"] | 0.0f;

  // Shake/fall detection
  if (event == "ALARM_SHAKE") {
    queueAlert("/alerts/alert_fall.wav");
  }

  // Battery alerts
  if (batPct <= 10) {
    queueAlert("/alerts/alert_BAT_high.wav");
  } else if (batPct <= 25) {
    queueAlert("/alerts/alert_BAT_medium.wav");
  }

  // pH alerts
  if (ph10 <= 4) {
    queueAlert("/alerts/alert_pH_high.wav");
  } else if (ph10 <= 7) {
    queueAlert("/alerts/alert_pH_medium.wav");
  }

  // TDS alerts
  if (tds10 <= 4) {
    queueAlert("/alerts/alert_TDS_high.wav");
  } else if (tds10 <= 7) {
    queueAlert("/alerts/alert_TDS_medium.wav");
  }

  // Turbidity alerts
  if (turb10 <= 4) {
    queueAlert("/alerts/alert_TURBIDITY_high.wav");
  } else if (turb10 <= 7) {
    queueAlert("/alerts/alert_TURBIDITY_medium.wav");
  }

  // LoRa signal quality alert
  if (rssi < -115) {
    queueAlert("/alerts/alert_LoRa.wav");
  }
}

// =====================================================
// Handle Secure Telemetry Packet
// =====================================================
void handleSecurePacket(const uint8_t *frame, size_t frameLen, int rssi, float snr) {
  uint32_t seq = 0;
  uint8_t plain[MAX_PLAIN_LEN + 1] = {0};
  uint16_t plainLen = 0;

  // Parse and decrypt the frame
  if (!parseAndVerifyDataFrame(frame, frameLen, seq, plain, plainLen)) {
    return;
  }

  // Anti-replay: reject old sequences
  if (seq < lastAcceptedSeq) {
    Serial.print("[REPLAY] old seq rejected: ");
    Serial.println((unsigned long)seq);
    return;
  }

  // Duplicate detection: resend ACK
  if (seq == lastAcceptedSeq) {
    Serial.print("[DUP] seq=");
    Serial.print((unsigned long)seq);
    Serial.println(" -> ACK resend");
    sendSecureAck(seq);
    return;
  }

  // Accept new sequence
  lastAcceptedSeq = seq;

  // Send ACK
  sendSecureAck(seq);

  // Parse JSON payload
  plain[plainLen] = '\0';
  
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char*)plain);

  if (err) {
    Serial.print("[JSON] Parse error: ");
    Serial.println(err.c_str());
    return;
  }

  // Display received data
  String event = doc["e"] | "None";

  Serial.println("\n========= NEW WATER DATA (SECURE) =========");
  Serial.printf("SEQ         : %lu\n", (unsigned long)seq);
  Serial.printf("LoRa Signal : RSSI %d dBm | SNR %.1f dB\n", rssi, snr);
  Serial.println("--------------------------------------------------");

  if (doc.containsKey("b")) {
    Serial.printf("BATTERY     : %d%% | %.2f V | %.0f mA\n",
                  doc["b"].as<int>(),
                  doc["v"].as<float>(),
                  doc["m"].as<float>());
  }

  if (doc.containsKey("p")) {
    Serial.printf("pH          : %.2f | Score: %d/10\n",
                  doc["p"].as<float>(),
                  doc["ps"].as<int>());
  }

  if (doc.containsKey("t")) {
    Serial.printf("TDS         : %d ppm | Score: %d/10\n",
                  doc["t"].as<int>(),
                  doc["ts"].as<int>());
  }

  if (doc.containsKey("u")) {
    Serial.printf("TURBIDITY   : %.2f V | Score: %d/10\n",
                  doc["u"].as<float>(),
                  doc["us"].as<int>());
  }

  if (doc.containsKey("tw")) {
    Serial.printf("TEMP. WATER : %.1f °C\n", doc["tw"].as<float>());
  }

  if (doc.containsKey("tm")) {
    Serial.printf("TEMP. MPU   : %.1f °C\n", doc["tm"].as<float>());
  }

  if (doc.containsKey("te")) {
    Serial.printf("TEMP. ESP32 : %.1f °C\n", doc["te"].as<float>());
  }

  Serial.printf("EVENT       : %s\n", event.c_str());
  
  if (event == "ALARM_SHAKE") {
    if (doc.containsKey("ag")) {
      Serial.printf("SHAKE FORCE : %.2f G (dynamic: %.2f G)\n",
                    doc["ag"].as<float>(),
                    doc["dg"].as<float>());
    }
  }
  
  Serial.println("==================================================\n");

  // Trigger alarm if shake detected
  if (event == "ALARM_SHAKE" && !alarmRunning) {
    startAlarm();
  }

  // Collect and play alerts
  collectAlertFiles(doc, rssi);
  playQueuedAlerts();
  
  // =====================================================
  // Submit data to Cloud API via WiFi
  // =====================================================
  Serial.println("[Cloud] Submitting data to API...");
  
  bool cloudSuccess = WiFiManager::submitSensorData(
    seq,
    doc["b"] | 0,                    // battery percentage
    doc["v"] | 0.0f,                 // voltage
    doc["m"] | 0,                    // current
    doc["p"] | 0.0f,                 // pH value
    doc["ps"] | 0,                   // pH status (0-10)
    doc["t"] | 0,                    // TDS
    doc["ts"] | 0,                   // TDS status (0-10)
    doc["u"] | 0.0f,                 // turbidity
    doc["us"] | 0,                   // turbidity status (0-10)
    doc["tw"] | 0.0f,                // water temp
    doc["tm"] | 0.0f,                // module temp
    doc["te"] | 0.0f,                // ESP32 temp
    event.c_str(),                   // error/event message
    rssi,                            // LoRa RSSI
    snr                              // LoRa SNR
  );
  
  if (cloudSuccess) {
    Serial.println("[Cloud] ✓ Data successfully sent to API");
  } else {
    Serial.println("[Cloud] ✗ Failed to send data to API");
    Serial.println("[Cloud] Data saved locally on SD card");
  }
  
  Serial.println();
}
