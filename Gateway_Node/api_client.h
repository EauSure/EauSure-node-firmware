#pragma once
#include <Arduino.h>


struct GatewayProvisionResult {
  bool success = false;
  String message;
  String gatewayId;
  String name;
  String mqttTopic;
};

struct NodePairingResult {
  bool success = false;
  String message;
  String aesKey;
  String nodeId;
  String gatewayHardwareId;
};

namespace ApiClient {

    bool healthCheck(const String& apiBaseUrl);

  bool provisionGateway(
    const String& apiBaseUrl,
    const String& gatewayHardwareId,
    const String& firmwareVersion,
    const String& token,
    const String& gatewayName,
    GatewayProvisionResult& out
  );

  bool pairNode(
    const String& apiBaseUrl,
    const String& jwtToken,
    const String& gatewayHardwareId,
    const String& nodeId,
    const String& nodeName,
    const String& nodeBleMac,
    NodePairingResult& out
  );
}