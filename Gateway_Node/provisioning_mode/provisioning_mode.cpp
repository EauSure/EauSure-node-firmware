#include "provisioning_mode.h"
#include "ble_provisioning.h"
#include "wifi_store.h"
#include <WiFi.h>

namespace {
  bool gComplete = false;
  String getGatewayName() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[20];
    snprintf(buf, sizeof(buf), "GW-%04X%08X",
             (uint16_t)(mac >> 32),
             (uint32_t)mac);
    return String(buf);
  }
}

namespace ProvisioningMode {

void begin() {
  gComplete = false;
  Serial.println("[PROVISIONING] Starting provisioning mode...");
  BleProvisioning::begin(getGatewayName());
}

void loop() {
  BleProvisioning::loop();

  if (!BleProvisioning::hasPendingData()) {
    delay(20);
    return;
  }

  ProvisioningData data = BleProvisioning::consumeData();

  Serial.printf("[PROVISIONING] Received SSID: %s\n", data.ssid.c_str());

  if (!WifiStore::save(data)) {
    Serial.println("[PROVISIONING] Failed to save credentials");
    return;
  }

  // optional: quick WiFi validation before reboot
  WiFi.mode(WIFI_STA);
  WiFi.begin(data.ssid.c_str(), data.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[PROVISIONING] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    gComplete = true;
    BleProvisioning::stop();
  } else {
    Serial.println("[PROVISIONING] WiFi connect failed, clearing saved data");
    WifiStore::clear();
  }
}

bool isComplete() {
  return gComplete;
}

}