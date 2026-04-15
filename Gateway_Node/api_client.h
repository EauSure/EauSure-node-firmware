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

struct PendingPairingKeyResult {
  bool success = false;
  bool found = false;
  int httpCode = 0;
  String message;
  String commandId;
  String nodeId;
  String aesKey;
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

bool fetchPendingPairingKey(
  const String& apiBaseUrl,
  const String& gatewayHardwareId,
  const String& expectedNodeId,
  PendingPairingKeyResult& out
);

bool ackCommand(
  const String& apiBaseUrl,
  const String& commandId,
  ApiBasicResult& out
);

} // namespace ApiClient
