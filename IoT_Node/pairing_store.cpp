#include "pairing_store.h"
#include <Preferences.h>

namespace {
  Preferences prefs;
  const char* NS = "nodepair";
}

void PairingStore::begin() {
  prefs.begin(NS, false);
}

bool PairingStore::hasPairing() {
  return prefs.getBool("valid", false);
}

NodePairingData PairingStore::load() {
  NodePairingData d;
  d.valid = prefs.getBool("valid", false);
  d.gatewayHardwareId = prefs.getString("gwId", "");
  d.nodeId = prefs.getString("nodeId", "");
  d.nodeName = prefs.getString("name", "");
  d.aesKeyHex = prefs.getString("aes", "");
  return d;
}

bool PairingStore::save(const NodePairingData& data) {
  prefs.putBool("valid", data.valid);
  prefs.putString("gwId", data.gatewayHardwareId);
  prefs.putString("nodeId", data.nodeId);
  prefs.putString("name", data.nodeName);
  prefs.putString("aes", data.aesKeyHex);
  return true;
}

void PairingStore::clear() {
  prefs.clear();
}