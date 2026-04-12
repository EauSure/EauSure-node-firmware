#pragma once
#include <Arduino.h>

struct ProvisioningData {
  String ssid;
  String password;
  String token;
  String gatewayName;
  bool valid = false;
};

namespace WifiStore {
  void begin();
  bool hasWifiCredentials();
  bool save(const ProvisioningData& data);
  ProvisioningData load();
  void clear();
}