#pragma once
#include <Arduino.h>
#include "wifi_store.h"

namespace BleProvisioning {
  void begin(const String& gatewayHardwareId, const String& gatewayDisplayName);
  void loop();
  bool hasPendingData();
  ProvisioningData consumeData();
  void sendStatus(bool success, const String& message, bool final = true);
  void restartAdvertising();
  void stop();
}
