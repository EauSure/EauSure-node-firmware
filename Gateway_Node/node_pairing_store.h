#pragma once
#include <Arduino.h>

struct NodePairingData {
  bool valid = false;
  String nodeId;
  String nodeName;
  String gatewayHardwareId;
  String aesKeyHex;
  String bleAddress;
};

namespace NodePairingStore {
  void begin();
  bool hasPairing();
  NodePairingData load();
  bool save(const NodePairingData& data);
  void clear();
}