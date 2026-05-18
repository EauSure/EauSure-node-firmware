#pragma once
#include <Arduino.h>

enum class GatewayStatus {
  PROVISIONING,
  WIFI_CONNECTING,
  MQTT_CONNECTING,
  CONNECTED
};

namespace HardwareUI {
  void begin();
  void setStatus(GatewayStatus status);
  GatewayStatus getStatus();
  bool isFactoryResetRequested();
}
