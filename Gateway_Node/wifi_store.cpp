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

bool save(const ProvisioningData& data) {
  prefs.putString("ssid", data.ssid);
  prefs.putString("pass", data.password);
  prefs.putString("token", data.token);
  prefs.putString("gname", data.gatewayName);
  prefs.putBool("valid", true);
  return true;
}

ProvisioningData load() {
  ProvisioningData d;
  d.ssid = prefs.getString("ssid", "");
  d.password = prefs.getString("pass", "");
  d.token = prefs.getString("token", "");
  d.gatewayName = prefs.getString("gname", "");
  d.valid = prefs.getBool("valid", false) && d.ssid.length() > 0;
  return d;
}

void clear() {
  prefs.clear();
}

}