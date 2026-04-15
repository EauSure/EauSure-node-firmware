#pragma once
#include <Arduino.h>

struct NodePairingData {
  bool valid = false;
  String gatewayHardwareId;
  String nodeId;
  String nodeName;
  String aesKeyHex;
};

struct PendingProvisionData {
  bool valid = false;
  String gatewayHardwareId;
  String nodeId;
  String wifiSsid;
  String wifiPassword;
  String apiBaseUrl;
  String pairingToken;
};

namespace PairingStore {
  void begin();
  bool hasPairing();
  NodePairingData load();
  bool save(const NodePairingData& data);
  bool hasPendingProvision();
  PendingProvisionData loadPendingProvision();
  bool savePendingProvision(const PendingProvisionData& data);
  void clearPendingProvision();
  void clear();
}
