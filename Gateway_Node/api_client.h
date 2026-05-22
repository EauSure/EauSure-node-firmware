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

bool failPairingSession(
  const String& apiBaseUrl,
  const String& sessionId,
  const String& reason,
  ApiBasicResult& out
);

// Fetch persisted gateway + node config (called at boot to restore state after reflash)
struct GatewayConfigResult {
  bool success = false;
  int httpCode = 0;
  String message;

  // Gateway-side settings
  uint32_t measureIntervalSec = 60;
  bool shakeEnabled = true;
  float shakeThreshold = 1.1f;
  bool nodeActive = true;
  bool gatewayVocalAlerts = true;

  // First paired node id (for SET_CONFIG forwarding)
  String primaryNodeId;
};

bool fetchGatewayConfig(
  const String& apiBaseUrl,
  const String& gatewayHardwareId,
  GatewayConfigResult& out
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

bool failCommand(
  const String& apiBaseUrl,
  const String& commandId,
  const String& reason,
  ApiBasicResult& out
);

} // namespace ApiClient
