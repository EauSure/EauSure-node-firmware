// =====================================================
// Gateway Node - FreeRTOS Water Quality Monitor
// =====================================================
// Modular architecture for maintainability
// Receives encrypted LoRa telemetry; SD logging and audio alerts currently disabled
// Sends data to cloud API via WiFi
//
// Modules:
// - app_state: Core state, encryption, protocol helpers
// - lora_radio: LoRa communication and secure frame handling
// - sd_logger: SD card initialization and logging
// - audio_alert: Audio playback and alert management
// - telemetry: Data parsing, display, and alert triggering
// - wifi_manager: WiFi connectivity and API communication
// - otaa_manager: Custom secure OTAA control (pairing, heartbeat, remote state)
// =====================================================

#include <LoRa.h>
#include "app_state.h"
#include "lora_radio.h"
#include "sd_logger.h"
#include "audio_alert.h"
#include "telemetry.h"
#include "wifi_manager.h"
#include "otaa_manager.h"

// =====================================================
// Setup
// =====================================================
void setup() {
  initApp();

  // Audio alerts temporarily disabled (enable when needed).
  // if (!initAudio()) {
  //   Serial.println("[FATAL] Audio init failed");
  //   while (true) delay(100);
  // }

  if (!initLoRa()) {
    Serial.println("[FATAL] LoRa init failed");
    while (true) delay(100);
  }

  // SD logging temporarily disabled (enable when needed).
  // if (!initSD()) {
  //   Serial.println("[FATAL] SD init failed");
  //   while (true) delay(100);
  // }

  // Initialize WiFi for cloud connectivity
  Serial.println("\n[Gateway] Initializing WiFi...");
  if (WiFiManager::init()) {
    Serial.println("[Gateway] WiFi connected - Cloud API ready");
  } else {
    Serial.println("[Gateway] WiFi failed - continuing with LoRa only");
    Serial.println("[Gateway] Cloud upload disabled; SD logging currently disabled");
  }

  Serial.println("\n==============================================");
  Serial.println("Gateway Ready - Waiting for telemetry data...");
  Serial.println("==============================================\n");

  initOtaaManager();
}

// =====================================================
// Loop
// =====================================================
void loop() {
  // Check WiFi connection periodically (every 30 seconds)
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > WIFI_RECONNECT_INTERVAL) {
    lastWiFiCheck = millis();
    
    if (!WiFiManager::isConnected()) {
      Serial.println("[Gateway] WiFi disconnected, attempting reconnect...");
      if (WiFiManager::reconnect()) {
        Serial.println("[Gateway] WiFi reconnected successfully");
      } else {
        Serial.println("[Gateway] WiFi reconnect failed - will retry later");
      }
    }
  }
  
  // UART Commands
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') return;

    if (c >= '1' && c <= '9') {
      loraSendChar(c);
    } else if (c == 'a') {
      loraSendString("10");
    } else if (c == '0') {
      loraSendString("0");
    } else if (c == 's') {
      // stopAlarm(); // Audio alerts disabled
    } else if (c == 'w') {
      // WiFi status command
      Serial.print("[WiFi] Status: ");
      Serial.println(WiFiManager::getStatusString());
      if (WiFiManager::isConnected()) {
        Serial.print("[WiFi] Signal: ");
        Serial.print(WiFiManager::getSignalStrength());
        Serial.println(" dBm");
      }
    } else if (c == 'x') {
      Serial.println("[OTAA] Request node ACTIVE");
      requestNodeActive();
    } else if (c == 'z') {
      Serial.println("[OTAA] Request node SLEEP (120s)");
      requestNodeSleep(120);
    }
  }

  otaaTick();

  // LoRa RX - binary secure frame
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  uint8_t frame[MAX_FRAME_LEN];
  size_t len = 0;

  while (LoRa.available() && len < sizeof(frame)) {
    frame[len++] = (uint8_t)LoRa.read();
  }

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();

  if (len >= 2 && frame[1] == MSG_TYPE_CTRL) {
    handleOtaaControlFrame(frame, len, rssi, snr);
  } else {
    handleSecurePacket(frame, len, rssi, snr);
  }
  LoRa.receive();
}
