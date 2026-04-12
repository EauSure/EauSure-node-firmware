#pragma once
#include <Arduino.h>
#include "wifi_store.h"

namespace BleProvisioning {
  void begin(const String& deviceName);
  void loop();
  bool hasPendingData();
  ProvisioningData consumeData();
  void stop();
}