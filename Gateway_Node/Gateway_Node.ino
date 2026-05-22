#include "wifi_store.h"
#include "provisioning_mode.h"
#include "normal_mode.h"
#include "node_pairing_mode.h"
#include "node_pairing_store.h"
#include "mqtt_gateway.h"
#include "otaa_manager.h"
#include "telemetry.h"
#include "hardware_ui.h"
#include "audio_alert.h"
#include "sd_logger.h"
#include "wifi_manager.h"
#include "fuota_manager.h"
#include <esp_bt.h>

enum class BootMode {
  PROVISIONING,
  NODE_PAIRING,
  NORMAL
};

static BootMode gMode = BootMode::PROVISIONING;
static String gLastPublishedCandidateId = "";

void setup() {
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  Serial.begin(115200);
  delay(300);

  HardwareUI::begin();
  WifiStore::begin();
  NodePairingStore::begin();
  FuotaManager::begin();

  // IMPORTANT: SD mount is deferred on purpose.
  //
  // Cold-boot rage case: on power-on the 3V3 rail is still settling while
  // LoRa + MAX98357A + ESP32 all draw their inrush, and a fresh MicroSD
  // needs ~1s of post-VCC idle before it reliably accepts CMD0. Mounting
  // here was failing on cold plug-in but working after a soft reset
  // because the rail was already stable.
  //
  // Every SD consumer (audio_alert, future FUOTA) already goes through
  // ensureSdReady(), which mounts on demand with backoff. By the time
  // the first demand happens (audio alert, factory-reset voice, OTA...)
  // the rail has been stable for many seconds and BLE/WiFi are quiet.
  Serial.println("[BOOT] SD mount deferred — will mount on first demand");

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
      if (!NodePairingMode::shouldPauseMqtt()) {
        MqttGateway::loop();
      }
      NodePairingMode::loop();

      if (NodePairingMode::hasCandidate()) {
        PairingCandidateInfo c = NodePairingMode::getCandidate();

        if (!c.nodeId.isEmpty() && c.nodeId != gLastPublishedCandidateId) {
          if (MqttGateway::publishCandidateFound(c.nodeId, c.nodeName, c.bleMac)) {
            gLastPublishedCandidateId = c.nodeId;
          }
        }
      } else if (!gLastPublishedCandidateId.isEmpty()) {
        // Allow the same nearby node to be re-published after retries/timeouts.
        gLastPublishedCandidateId = "";
      }

      if (NodePairingMode::isComplete()) {
        Serial.println("[BOOT] Node pairing complete -> restarting...");
        delay(1000);
        ESP.restart();
      }
      break;

    case BootMode::NORMAL:
      // Check if the UI task requested a factory reset while WiFi was up.
      // The UI task cannot safely make HTTPS calls (heap corruption with
      // concurrent TLS clients), so we handle it here in the main loop
      // context where MQTT is already managed.
      if (HardwareUI::isFactoryResetRequested()) {
        Serial.println("[BOOT] Factory reset requested — notifying backend from main loop");
        MqttGateway::setExclusiveTlsWindow(true);
        delay(100);
        WiFiManager::unprovisionGateway();
        delay(200);
        Serial.println("[BOOT] Unprovision done — rebooting");
        ESP.restart();
      }

      FuotaManager::loop();
      if (FuotaManager::isGatewayUpdateInProgress()) {
        delay(10);
        break;
      }
      if (!shouldPauseBackgroundWork() && !telemetryHasPendingUpload()) {
        MqttGateway::loop();
      }
      NormalMode::loop();
      break;
  }
}
