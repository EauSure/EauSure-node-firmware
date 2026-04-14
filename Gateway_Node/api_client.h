#pragma once
#include <Arduino.h>

struct GatewayProvisionResult {
  bool success = false;
  int httpCode = 0;
  String message;
  String gatewayId;
  String name;
  String mqttTopic;
};

struct ApiBasicResult {
  bool success = false;
  int httpCode = 0;
  String message;
};

struct PairingTokenResult {
  bool success = false;
  int httpCode = 0;
  String message;
  String pairingToken;
};

namespace ApiClient {

bool healthCheck(const String& apiBaseUrl);

bool provisionGateway(
  const String& apiBaseUrl,
  const String& gatewayHardwareId,
  const String& firmwareVersion,
  const String& deviceSecret,
  const String& token,
  const String& gatewayName,
  GatewayProvisionResult& out
);

bool rollbackPairNode(
  const String& apiBaseUrl,
  const String& gatewayHardwareId,
  const String& nodeId,
  const String& pairingToken,
  ApiBasicResult& out
);

bool verifyNodeProof(
  const String& apiBaseUrl,
  const String& gatewayHardwareId,
  const String& nodeId,
  const String& sessionId,
  const String& nonce,
  const String& proof,
  PairingTokenResult& out
);

} // namespace ApiClient
