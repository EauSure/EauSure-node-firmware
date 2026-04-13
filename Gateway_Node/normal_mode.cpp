#include "normal_mode.h"

#include <LoRa.h>
#include "app_state.h"
#include "lora_radio.h"
#include "audio_alert.h"
#include "telemetry.h"
#include "wifi_manager.h"
#include "otaa_manager.h"
#include "wifi_store.h"
#include "api_client.h"


static String getGatewayHardwareIdString() {
  String mac = WiFiManager::getMacAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  return mac;
}

namespace NormalMode {

void begin() {
  Serial.println("[NORMAL] Starting gateway normal mode...");
  if (!initApp()) {
    Serial.println("[FATAL] initApp failed - runtime AES key not available");
    while (true) delay(100);
  }

  ProvisioningData prov = WifiStore::load();
  if (!prov.valid) {
    Serial.println("[FATAL] No valid provisioning data found");
    while (true) delay(100);
  }

  Serial.println("\n[Gateway] Initializing WiFi...");
  if (WiFiManager::init(prov.ssid.c_str(), prov.password.c_str())) {
    Serial.println("[Gateway] WiFi connected — API provisioning phase");
  } else {
    Serial.println("[Gateway] WiFi failed");
    while (true) delay(100);
  }

  String gatewayHardwareId = getGatewayHardwareIdString();

  GatewayProvisionResult provision;
  bool apiOk = ApiClient::provisionGateway(
    API_BASE_URL,
    gatewayHardwareId,
    GATEWAY_FIRMWARE_VERSION,
    prov.token,
    prov.gatewayName,
    provision
  );

  if (!apiOk) {
    Serial.printf("[FATAL] Gateway provisioning failed: %s\n", provision.message.c_str());
    while (true) delay(100);
  }

  Serial.println("[Gateway] Provisioned successfully");
  Serial.printf("[Gateway] gatewayId: %s\n", provision.gatewayId.c_str());
  Serial.printf("[Gateway] mqttTopic: %s\n", provision.mqttTopic.c_str());

  if (!initLoRa()) {
    Serial.println("[FATAL] LoRa init failed");
    while (true) delay(100);
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
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {}
    else if (c == 'm') requestMeasureNow();
    else if (c == 's') stopAlarm();
    else if (c == 'w') {
      Serial.printf("[WiFi] %s", WiFiManager::getStatusString());
      if (WiFiManager::isConnected())
        Serial.printf(" | %d dBm | IP=%s | MAC=%s\n",
                      WiFiManager::getSignalStrength(),
                      WiFiManager::getIP().c_str(),
                      WiFiManager::getMacAddress().c_str());
      else
        Serial.println();
    }
  }

  static uint32_t lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > WIFI_RECONNECT_INTERVAL) {
    lastWiFiCheck = millis();
    if (!WiFiManager::isConnected()) {
      Serial.println("[Gateway] WiFi dropped — reconnecting...");
      WiFiManager::reconnect();
    }
  }

  otaaTick();

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
    parseAndDispatchDataFrame(frame, len, rssi, snr);
  } else {
    parseAndDispatchTypedFrame(frame, len, rssi, snr);
  }

  LoRa.receive();
}
}