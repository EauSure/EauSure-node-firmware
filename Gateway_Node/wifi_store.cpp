#include "wifi_store.h"
#include <Preferences.h>

namespace {
  Preferences prefs;
  const char* NS = "gwprov";
}

namespace WifiStore {

void begin() {
  prefs.begin(NS, false);
}

bool hasWifiCredentials() {
  return prefs.getString("ssid", "").length() > 0;
}

bool isCloudProvisioned() {
  return prefs.getBool("cloudProv", false);
}

bool save(const ProvisioningData& data) {
  prefs.putString("ssid", data.ssid);
  prefs.putString("pass", data.password);
  prefs.putString("token", data.token);
  prefs.putString("gname", data.gatewayName);
  prefs.putBool("cloudProv", false);
  prefs.putBool("valid", true);
  return true;
}

ProvisioningData load() {
  ProvisioningData d;
  d.ssid = prefs.getString("ssid", "");
  d.password = prefs.getString("pass", "");
  d.token = prefs.getString("token", "");
  d.gatewayName = prefs.getString("gname", "");
  d.cloudProvisioned = prefs.getBool("cloudProv", false);
  d.valid = prefs.getBool("valid", false) && d.ssid.length() > 0;
  return d;
}

bool markCloudProvisioned() {
  prefs.putBool("cloudProv", true);
  prefs.remove("token");
  return true;
}

void clear() {
  prefs.clear();
}

}
