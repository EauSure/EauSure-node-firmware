#include "provisioning_mode.h"
#include "ble_provisioning.h"
#include "audio_alert.h"
#include "config.h"
#include "wifi_store.h"
#include <WiFi.h>

namespace {
  bool gComplete = false;

  String getGatewayHardwareId() {
    String configured = String(GATEWAY_DEVICE_ID);
    configured.trim();
    configured.toUpperCase();
    if (!configured.startsWith("GW-")) {
      configured = "GW-" + configured;
    }
    return configured;
  }

  String getGatewayDisplayName() {
    String configured = String(GATEWAY_DEVICE_NAME);
    configured.trim();
    return configured;
  }

  void cleanupWifiAfterProvisioningAttempt() {
    Serial.printf("[PROVISIONING] Heap before WiFi cleanup: %u\n", ESP.getFreeHeap());
    WiFi.disconnect(false, false);
    delay(300);
    WiFi.mode(WIFI_OFF);
    delay(800);
    Serial.printf("[PROVISIONING] Heap after WiFi cleanup: %u\n", ESP.getFreeHeap());
  }

  void notifyFailureAndRestart(const String& message) {
    BleProvisioning::sendStatus(false, message + " Gateway restarting for a clean retry.");
    delay(1200);
    cleanupWifiAfterProvisioningAttempt();
    delay(300);
    Serial.println("[PROVISIONING] Restarting after failed WiFi provisioning");
    ESP.restart();
  }
}

namespace ProvisioningMode {

void begin() {
  gComplete = false;
  Serial.println("[PROVISIONING] Starting provisioning mode...");

  // Defensive barrier: never let a stray I2S / WAV decoder allocation
  // sit in heap while we bring the BLE stack up. BLE GATT + advertising
  // allocates several kilobytes at begin(), and any audio leftover would
  // compete for the same heap region.
  forceReleaseAudioResources();

  BleProvisioning::begin(getGatewayHardwareId(), getGatewayDisplayName());
}

void loop() {
  BleProvisioning::loop();

  if (!BleProvisioning::hasPendingData()) {
    delay(20);
    return;
  }

  ProvisioningData data = BleProvisioning::consumeData();

  Serial.printf("[PROVISIONING] Received SSID: %s\n", data.ssid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(data.ssid.c_str(), data.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    delay(500);
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[PROVISIONING] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    if (!WifiStore::save(data)) {
      Serial.println("[PROVISIONING] Failed to save credentials");
      notifyFailureAndRestart("WiFi connected, but credentials could not be saved.");
      return;
    }
    BleProvisioning::sendStatus(true, "WiFi connected. Provisioning complete.");
    gComplete = true;
    delay(500);
    BleProvisioning::stop();
  } else {
    wl_status_t status = WiFi.status();
    Serial.printf("[PROVISIONING] WiFi connect failed, status=%d\n", status);
    WifiStore::clear();
    if (status == WL_NO_SSID_AVAIL) {
      notifyFailureAndRestart("WiFi network not found. Check the SSID.");
    } else if (status == WL_CONNECT_FAILED) {
      notifyFailureAndRestart("WiFi connection failed. Check the password.");
    } else {
      notifyFailureAndRestart("WiFi connection timed out. Check SSID, password, or router availability.");
    }
  }
}

bool isComplete() {
  return gComplete;
}

}
