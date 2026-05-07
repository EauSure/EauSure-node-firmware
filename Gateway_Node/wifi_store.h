#pragma once
#include <Arduino.h>

struct ProvisioningData {
  String ssid;
  String password;
  String token;
  String gatewayName;
  bool valid = false;
  bool cloudProvisioned = false;
};

namespace WifiStore {
  void begin();
  bool hasWifiCredentials();
  bool isCloudProvisioned();
  bool save(const ProvisioningData& data);
  ProvisioningData load();
  bool markCloudProvisioned();
  void clear();
}
