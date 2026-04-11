// =====================================================
// Gateway Node — Commander architecture
// =====================================================
// The gateway is the sole initiator of all IoT node
// activity except for autonomous SHAKE_ALERT frames.
//
// Boot sequence:
//   1. LoRa + WiFi init
//   2. initOtaaManager() — schedules first ACTIVATE
//   3. otaaTick() sends ACTIVATE; IoT replies ACTIVATE_OK
//   4. From then: gateway drives MEASURE_REQ (60 s / manual)
//                 and HEARTBEAT_REQ (10 s)
// =====================================================

#include <LoRa.h>
#include "app_state.h"
#include "lora_radio.h"
#include "audio_alert.h"
#include "telemetry.h"
#include "wifi_manager.h"
#include "otaa_manager.h"

void setup() {
  initApp();

  if (!initLoRa()) {
    Serial.println("[FATAL] LoRa init failed");
    while (true) delay(100);
  }

  // Audio alerts — enable when SD card and WAV files are present
  // if (!initAudio()) { Serial.println("[FATAL] Audio init failed"); while(true) delay(100); }

  Serial.println("\n[Gateway] Initializing WiFi...");
  if (WiFiManager::init()) {
    Serial.println("[Gateway] WiFi connected — Cloud API ready");
  } else {
    Serial.println("[Gateway] WiFi failed — continuing without cloud upload");
  }

  Serial.println("\n==============================================");
  Serial.println("Gateway Ready — commander mode");
  Serial.println("  m = manual MEASURE_REQ");
  Serial.println("  s = stop alarm");
  Serial.println("  w = WiFi status");
  Serial.println("==============================================\n");

  initOtaaManager();
}

void loop() {
  // ── Serial commands ──
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {}
    else if (c == 'm') requestMeasureNow();
    else if (c == 's') stopAlarm();
    else if (c == 'w') {
      Serial.printf("[WiFi] %s", WiFiManager::getStatusString());
      if (WiFiManager::isConnected())
        Serial.printf(" | %d dBm\n", WiFiManager::getSignalStrength());
      else
        Serial.println();
    }
  }

  // ── WiFi watchdog (every 30 s) ──
  static uint32_t lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > WIFI_RECONNECT_INTERVAL) {
    lastWiFiCheck = millis();
    if (!WiFiManager::isConnected()) {
      Serial.println("[Gateway] WiFi dropped — reconnecting...");
      WiFiManager::reconnect();
    }
  }

  // ── OTAA tick — drives ACTIVATE / MEASURE_REQ / HEARTBEAT timers ──
  otaaTick();

  // ── LoRa RX — check for incoming IoT frames ──
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  uint8_t frame[MAX_FRAME_LEN];
  size_t  len = 0;
  while (LoRa.available() && len < sizeof(frame)) frame[len++] = (uint8_t)LoRa.read();

  int   rssi = LoRa.packetRssi();
  float snr  = LoRa.packetSnr();

  if (len < 2) return;

  uint8_t msgType = frame[1];

  if (msgType == MSG_TYPE_DATA) {
    // MEASURE_RESP or SHAKE_ALERT — heavy handler, ACK sent inside
    parseAndDispatchDataFrame(frame, len, rssi, snr);
  } else {
    // ACTIVATE_OK, HEARTBEAT_ACK — lightweight, ACK sent inside
    parseAndDispatchTypedFrame(frame, len, rssi, snr);
  }

  LoRa.receive();
}
