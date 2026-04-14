#pragma once
#include <Arduino.h>

struct PairingCandidateInfo {
  bool valid = false;
  String nodeId;
  String nodeName;
  String bleMac;
};

namespace NodePairingMode {
  void begin();
  void loop();
  bool isComplete();
  bool shouldPauseMqtt();

  bool hasCandidate();
  PairingCandidateInfo getCandidate();

  // Call this from your MQTT handler after receiving CONFIRM_PAIRING.
  bool confirmCandidate(const String& nodeId, const String& sessionId, const String& apPassword);

  // Call this from your MQTT handler after receiving PAIRING_KEY_READY.
  bool providePairingKey(const String& nodeId, const String& aesKey);

  // Optional manual reset for cancel flows.
  void cancelPendingConfirmation();
}
