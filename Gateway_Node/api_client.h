#pragma once
#include <Arduino.h>

struct GatewayProvisionResult {
  bool success = false;
  String message;
  String gatewayId;
  String name;
  String mqttTopic;
};

namespace ApiClient {
  bool provisionGateway(
    const String& apiBaseUrl,
    const String& gatewayHardwareId,
    const String& firmwareVersion,
    const String& token,
    const String& gatewayName,
    GatewayProvisionResult& out
  );
}