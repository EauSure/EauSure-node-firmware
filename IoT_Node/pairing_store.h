#pragma once
#include <Arduino.h>

struct NodePairingData {
  bool valid = false;
  String gatewayHardwareId;
  String nodeId;
  String nodeName;
  String aesKeyHex;
};

namespace PairingStore {
  void begin();
  bool hasPairing();
  NodePairingData load();
  bool save(const NodePairingData& data);
  void clear();
}