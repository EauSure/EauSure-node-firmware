#include "provisioning_mode/wifi_store.h"
#include "provisioning_mode/provisioning_mode.h"
#include "normal_mode/normal_mode.h"

enum class BootMode {
  PROVISIONING,
  NORMAL
};

static BootMode gMode = BootMode::PROVISIONING;

void setup() {
  Serial.begin(115200);
  delay(300);

  WifiStore::begin();

  if (WifiStore::hasWifiCredentials()) {
    gMode = BootMode::NORMAL;
    Serial.println("[BOOT] WiFi credentials found → NORMAL mode");
    NormalMode::begin();
  } else {
    gMode = BootMode::PROVISIONING;
    Serial.println("[BOOT] No WiFi credentials → PROVISIONING mode");
    ProvisioningMode::begin();
  }
}

void loop() {
  switch (gMode) {
    case BootMode::PROVISIONING:
      ProvisioningMode::loop();

      // if provisioning completed and wifi creds now exist, reboot into normal mode
      if (ProvisioningMode::isComplete()) {
        Serial.println("[BOOT] Provisioning complete → restarting...");
        delay(1000);
        ESP.restart();
      }
      break;

    case BootMode::NORMAL:
      NormalMode::loop();
      break;
  }
}