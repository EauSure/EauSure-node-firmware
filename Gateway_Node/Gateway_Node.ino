#include "wifi_store.h"
#include "provisioning_mode.h"
#include "normal_mode.h"
#include "node_pairing_mode.h"
#include "node_pairing_store.h"

enum class BootMode {
  PROVISIONING,
  NODE_PAIRING,
  NORMAL
};

static BootMode gMode = BootMode::PROVISIONING;

void setup() {
  Serial.begin(115200);
  delay(300);

  WifiStore::begin();
  NodePairingStore::begin();

  if (!WifiStore::hasWifiCredentials()) {
    gMode = BootMode::PROVISIONING;
    Serial.println("[BOOT] No WiFi → PROVISIONING mode");
    ProvisioningMode::begin();
    return;
  }

  // WiFi exists → check node pairing
  if (!NodePairingStore::hasPairing()) {
    gMode = BootMode::NODE_PAIRING;
    Serial.println("[BOOT] No node pairing → NODE_PAIRING mode");
    NodePairingMode::begin();
    return;
  }

  // Everything ready → normal runtime
  gMode = BootMode::NORMAL;
  Serial.println("[BOOT] Ready → NORMAL mode");
  NormalMode::begin();
}

void loop() {
  switch (gMode) {

    case BootMode::PROVISIONING:
      ProvisioningMode::loop();

      if (ProvisioningMode::isComplete()) {
        Serial.println("[BOOT] Provisioning complete → restarting...");
        delay(1000);
        ESP.restart();
      }
      break;

    case BootMode::NODE_PAIRING:
      NodePairingMode::loop();

      if (NodePairingMode::isComplete()) {
        Serial.println("[BOOT] Node pairing complete → restarting...");
        delay(1000);
        ESP.restart();
      }
      break;

    case BootMode::NORMAL:
      NormalMode::loop();
      break;
  }
}