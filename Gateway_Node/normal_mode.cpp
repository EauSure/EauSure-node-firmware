#include "normal_mode.h"

#include <LoRa.h>
#include "app_state.h"
#include "lora_radio.h"
#include "audio_alert.h"
#include "sd_logger.h"
#include "telemetry.h"
#include "wifi_manager.h"
#include "otaa_manager.h"
#include "wifi_store.h"
#include "api_client.h"
#include "hardware_ui.h"
#include "audio_alert.h"
#include "sd_logger.h"
static String getGatewayHardwareIdString() {
  String configured = String(GATEWAY_DEVICE_ID);
  configured.trim();
  configured.toUpperCase();
  if (!configured.startsWith("GW-")) {
    configured = "GW-" + configured;
  }
  return configured;
}

namespace NormalMode {

void begin() {
  HardwareUI::setStatus(GatewayStatus::CONNECTED);
  Serial.println("[NORMAL] Starting gateway normal mode...");

  // Defensive barrier: make sure no I2S DMA / WAV decoder leftover is
  // sitting in heap before we bring WiFi + TLS up. A stuck audio pipeline
  // would otherwise silently cost ~14 KB of heap right when the first
  // cloud provisioning or telemetry POST needs it.
  forceReleaseAudioResources();

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
  if (!prov.cloudProvisioned) {
    if (prov.token.isEmpty()) {
      Serial.println("[FATAL] Missing provisioning token before initial cloud claim");
      while (true) delay(100);
    }

    GatewayProvisionResult provision;
    bool apiOk = ApiClient::provisionGateway(
      API_BASE_URL,
      gatewayHardwareId,
      GATEWAY_FIRMWARE_VERSION,
      GATEWAY_DEVICE_SECRET,
      prov.token,
      prov.gatewayName,
      provision
    );

    if (!apiOk) {
      Serial.printf("[FATAL] Gateway provisioning failed: %s\n", provision.message.c_str());
      while (true) delay(100);
    }

    WifiStore::markCloudProvisioned();
    Serial.println("[Gateway] Provisioned successfully");
    Serial.printf("[Gateway] gatewayId: %s\n", provision.gatewayId.c_str());
    Serial.printf("[Gateway] mqttTopic: %s\n", provision.mqttTopic.c_str());
  } else {
    Serial.println("[Gateway] Cloud provisioning already completed");
  }

  // Fetch persisted config from backend — restores measureInterval, shakeThreshold,
  // nodeActive, vocalAlerts settings that are otherwise lost after reflash.
  {
    Serial.println("[Gateway] Fetching persisted config from backend...");
    ApiClient::GatewayConfigResult res;
    if (ApiClient::fetchGatewayConfig(API_BASE_URL, gatewayHardwareId, res)) {
      setMeasureIntervalMs(res.measureIntervalSec * 1000UL);
      setNodeActiveFlag(res.nodeActive);
      setVocalAlertsEnabled(res.gatewayVocalAlerts);
      Serial.printf("[Gateway] Config restored: mi=%lu st=%.2f se=%d na=%d va=%d\n",
                    (unsigned long)res.measureIntervalSec,
                    res.shakeThreshold,
                    (int)res.shakeEnabled,
                    (int)res.nodeActive,
                    (int)res.gatewayVocalAlerts);

      // Queue hardware config to be transmitted to the IoT Node
      queueSetConfig(res.shakeThreshold, res.shakeEnabled);
    } else {
      Serial.println("[Gateway] Config fetch failed — using defaults");
    }
  }

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
  if (packetSize) {
    uint8_t frame[MAX_FRAME_LEN];
    size_t  len = 0;
    while (LoRa.available() && len < sizeof(frame)) frame[len++] = (uint8_t)LoRa.read();

    int   rssi = LoRa.packetRssi();
    float snr  = LoRa.packetSnr();

    if (len >= 2) {
      uint8_t msgType = frame[1];

      if (msgType == MSG_TYPE_DATA) {
        parseAndDispatchDataFrame(frame, len, rssi, snr);
      } else {
        parseAndDispatchTypedFrame(frame, len, rssi, snr);
      }
    }

    LoRa.receive();
  }

  telemetryTick();
}
}
