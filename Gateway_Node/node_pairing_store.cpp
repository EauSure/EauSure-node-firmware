#include "node_pairing_store.h"
#include <Preferences.h>

namespace {
  Preferences prefs;
  const char* NS = "nodepair";
}

namespace NodePairingStore {

void begin() {
  prefs.begin(NS, false);
}

bool hasPairing() {
  return prefs.getBool("valid", false) &&
         prefs.getString("nodeId", "").length() > 0 &&
         prefs.getString("aes", "").length() == 32 &&
         prefs.getString("gwId", "").length() > 0;
}

NodePairingData load() {
  NodePairingData d;
  d.valid             = prefs.getBool("valid", false);
  d.nodeId            = prefs.getString("nodeId", "");
  d.nodeName          = prefs.getString("name", "");
  d.gatewayHardwareId = prefs.getString("gwId", "");
  d.aesKeyHex         = prefs.getString("aes", "");
  d.bleAddress        = prefs.getString("ble", "");

  if (d.nodeId.isEmpty() || d.gatewayHardwareId.isEmpty() || d.aesKeyHex.isEmpty()) {
    d.valid = false;
  }

  return d;
}

bool save(const NodePairingData& data) {
  if (data.nodeId.isEmpty() ||
      data.gatewayHardwareId.isEmpty() ||
      data.aesKeyHex.isEmpty()) {
    return false;
  }

  prefs.putString("nodeId", data.nodeId);
  prefs.putString("name", data.nodeName);
  prefs.putString("gwId", data.gatewayHardwareId);
  prefs.putString("aes", data.aesKeyHex);
  prefs.putString("ble", data.bleAddress);
  prefs.putBool("valid", true);

  return true;
}

void clear() {
  prefs.clear();
}

}