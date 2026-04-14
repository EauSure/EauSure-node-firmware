#include "wifi_store.h"
#include "provisioning_mode.h"
#include "normal_mode.h"
#include "node_pairing_mode.h"
#include "node_pairing_store.h"
#include "mqtt_gateway.h"

enum class BootMode {
  PROVISIONING,
  NODE_PAIRING,
  NORMAL
};

static BootMode gMode = BootMode::PROVISIONING;
static String gLastPublishedCandidateId = "";

void setup() {
  Serial.begin(115200);
  delay(300);

  WifiStore::begin();
  NodePairingStore::begin();

  if (!WifiStore::hasWifiCredentials()) {
    gMode = BootMode::PROVISIONING;
    Serial.println("[BOOT] No WiFi -> PROVISIONING mode");
    ProvisioningMode::begin();
    return;
  }

  if (!NodePairingStore::hasPairing()) {
    gMode = BootMode::NODE_PAIRING;
    Serial.println("[BOOT] No node pairing -> NODE_PAIRING mode");
    NodePairingMode::begin();
    MqttGateway::begin();
    return;
  }

  gMode = BootMode::NORMAL;
  Serial.println("[BOOT] Ready -> NORMAL mode");
  NormalMode::begin();
  MqttGateway::begin();
}

void loop() {
  switch (gMode) {

    case BootMode::PROVISIONING:
      ProvisioningMode::loop();

      if (ProvisioningMode::isComplete()) {
        Serial.println("[BOOT] Provisioning complete -> restarting...");
        delay(1000);
        ESP.restart();
      }
      break;

    case BootMode::NODE_PAIRING:
      MqttGateway::loop();
      NodePairingMode::loop();

      if (NodePairingMode::hasCandidate()) {
        PairingCandidateInfo c = NodePairingMode::getCandidate();

        if (!c.nodeId.isEmpty() && c.nodeId != gLastPublishedCandidateId) {
          if (MqttGateway::publishCandidateFound(c.nodeId, c.nodeName, c.bleMac)) {
            gLastPublishedCandidateId = c.nodeId;
          }
        }
      }

      if (NodePairingMode::isComplete()) {
        Serial.println("[BOOT] Node pairing complete -> restarting...");
        delay(1000);
        ESP.restart();
      }
      break;

    case BootMode::NORMAL:
      MqttGateway::loop();
      NormalMode::loop();
      break;
  }
}