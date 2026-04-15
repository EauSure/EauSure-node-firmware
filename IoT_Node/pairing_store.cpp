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
  return prefs.getBool("valid", false) &&
         prefs.getString("gwId", "").length() > 0 &&
         prefs.getString("nodeId", "").length() > 0 &&
         prefs.getString("aes", "").length() == 32;
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

bool PairingStore::hasPendingProvision() {
  return prefs.getBool("pvalid", false) &&
         prefs.getString("pgwId", "").length() > 0 &&
         prefs.getString("pnodeId", "").length() > 0 &&
         prefs.getString("pssid", "").length() > 0 &&
         prefs.getString("papi", "").length() > 0 &&
         prefs.getString("ptok", "").length() > 0;
}

PendingProvisionData PairingStore::loadPendingProvision() {
  PendingProvisionData d;
  d.valid = prefs.getBool("pvalid", false);
  d.gatewayHardwareId = prefs.getString("pgwId", "");
  d.nodeId = prefs.getString("pnodeId", "");
  d.wifiSsid = prefs.getString("pssid", "");
  d.wifiPassword = prefs.getString("ppwd", "");
  d.apiBaseUrl = prefs.getString("papi", "");
  d.pairingToken = prefs.getString("ptok", "");
  return d;
}

bool PairingStore::savePendingProvision(const PendingProvisionData& data) {
  prefs.putBool("pvalid", data.valid);
  prefs.putString("pgwId", data.gatewayHardwareId);
  prefs.putString("pnodeId", data.nodeId);
  prefs.putString("pssid", data.wifiSsid);
  prefs.putString("ppwd", data.wifiPassword);
  prefs.putString("papi", data.apiBaseUrl);
  prefs.putString("ptok", data.pairingToken);
  return true;
}

void PairingStore::clearPendingProvision() {
  prefs.remove("pvalid");
  prefs.remove("pgwId");
  prefs.remove("pnodeId");
  prefs.remove("pssid");
  prefs.remove("ppwd");
  prefs.remove("papi");
  prefs.remove("ptok");
}

void PairingStore::clear() {
  prefs.clear();
}
