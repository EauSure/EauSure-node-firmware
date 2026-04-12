#include "node_pairing_mode.h"
#include "node_pairing_store.h"
#include "wifi_manager.h"
#include "wifi_store.h"
#include "api_client.h"
#include "config.h"

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <ArduinoJson.h>

static bool gComplete = false;
static bool gBusy = false;
static BLEAddress* gFoundAddress = nullptr;
static String gFoundBleMac = "";
static String gFoundName = "";
static String gFoundNodeId = "";

static const char* SERVICE_UUID = "22345678-1234-1234-1234-1234567890ab";
static const char* RX_UUID      = "22345678-1234-1234-1234-1234567890ac";
static const char* TX_UUID      = "22345678-1234-1234-1234-1234567890ad";

static String getGatewayHardwareIdString() {
  String mac = WiFiManager::getMacAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  return mac;
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveServiceUUID()) return;
    if (!dev.isAdvertisingService(BLEUUID(SERVICE_UUID))) return;
    if (gFoundAddress != nullptr) return;  // first match only for now

    gFoundName = dev.getName().c_str();
    gFoundAddress = new BLEAddress(dev.getAddress());
    gFoundBleMac = dev.getAddress().toString().c_str();
    gFoundBleMac.toUpperCase();

    Serial.printf("[PAIRING] Found node candidate: %s [%s]\n",
      gFoundName.c_str(),
      gFoundBleMac.c_str());
  }
};

static bool readNodeInfo(
  BLERemoteCharacteristic* txChar,
  String& outNodeId,
  String& outNodeName
) {
  if (txChar == nullptr) return false;

  std::string raw = txChar->readValue();
  if (raw.empty()) {
    Serial.println("[PAIRING] TX characteristic returned empty payload");
    return false;
  }

  String json = String(raw.c_str());
  Serial.println("[PAIRING] Node TX payload: " + json);

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    Serial.println("[PAIRING] Failed to parse node TX JSON");
    return false;
  }

  outNodeId = String(doc["nodeId"] | "");
  outNodeName = String(doc["nodeName"] | "");

  if (outNodeId.isEmpty()) {
    Serial.println("[PAIRING] Node info missing nodeId");
    return false;
  }

  outNodeId.toUpperCase();
  return true;
}

static bool writePairingPacket(
  BLERemoteCharacteristic* rxChar,
  const String& gatewayHardwareId,
  const String& nodeId,
  const String& nodeName,
  const String& aesKey
) {
  if (rxChar == nullptr) return false;

  StaticJsonDocument<256> doc;
  doc["gatewayHardwareId"] = gatewayHardwareId;
  doc["nodeId"] = nodeId;
  doc["nodeName"] = nodeName;
  doc["aesKey"] = aesKey;

  String payload;
  serializeJson(doc, payload);

  Serial.println("[PAIRING] Writing pairing payload to node:");
  Serial.println(payload);

  try {
    rxChar->writeValue((uint8_t*)payload.c_str(), payload.length(), true);
    return true;
  } catch (...) {
    Serial.println("[PAIRING] BLE write failed");
    return false;
  }
}

static bool connectAndPair() {
  if (gFoundAddress == nullptr) return false;

  BLEClient* client = BLEDevice::createClient();
  Serial.printf("[PAIRING] Connecting to node [%s]...\n", gFoundBleMac.c_str());

  if (!client->connect(*gFoundAddress)) {
    Serial.println("[PAIRING] BLE connect failed");
    delete client;
    return false;
  }

  BLERemoteService* service = nullptr;
  BLERemoteCharacteristic* rxChar = nullptr;
  BLERemoteCharacteristic* txChar = nullptr;

  try {
    service = client->getService(BLEUUID(SERVICE_UUID));
    if (service == nullptr) {
      Serial.println("[PAIRING] Service not found");
      client->disconnect();
      delete client;
      return false;
    }

    rxChar = service->getCharacteristic(BLEUUID(RX_UUID));
    txChar = service->getCharacteristic(BLEUUID(TX_UUID));

    if (rxChar == nullptr || txChar == nullptr) {
      Serial.println("[PAIRING] Required characteristics not found");
      client->disconnect();
      delete client;
      return false;
    }

    String nodeId;
    String nodeName;
    if (!readNodeInfo(txChar, nodeId, nodeName)) {
      client->disconnect();
      delete client;
      return false;
    }

    gFoundNodeId = nodeId;
    if (nodeName.length() > 0) {
      gFoundName = nodeName;
    }

    ProvisioningData prov = WifiStore::load();
    if (!prov.valid || prov.token.isEmpty()) {
      Serial.println("[PAIRING] Missing provisioning token");
      client->disconnect();
      delete client;
      return false;
    }

    String gatewayHardwareId = getGatewayHardwareIdString();

    NodePairingResult pairResult;
    bool apiOk = ApiClient::pairNode(
      API_BASE_URL,
      prov.token,
      gatewayHardwareId,
      nodeId,
      gFoundName,
      gFoundBleMac,
      pairResult
    );

    if (!apiOk) {
      Serial.printf("[PAIRING] API pair-node failed: %s\n", pairResult.message.c_str());
      client->disconnect();
      delete client;
      return false;
    }

    if (!writePairingPacket(
          rxChar,
          pairResult.gatewayHardwareId,
          pairResult.nodeId,
          gFoundName,
          pairResult.aesKey)) {
      client->disconnect();
      delete client;
      return false;
    }

    NodePairingData local;
    local.valid = true;
    local.nodeId = pairResult.nodeId;
    local.nodeName = gFoundName;
    local.gatewayHardwareId = pairResult.gatewayHardwareId;
    local.aesKeyHex = pairResult.aesKey;
    local.bleAddress = gFoundBleMac;

    if (!NodePairingStore::save(local)) {
      Serial.println("[PAIRING] Failed to save local node pairing");
      client->disconnect();
      delete client;
      return false;
    }

    Serial.println("[PAIRING] Node paired successfully");
    client->disconnect();
    delete client;
    return true;

  } catch (...) {
    Serial.println("[PAIRING] Exception during BLE pairing flow");
    if (client->isConnected()) client->disconnect();
    delete client;
    return false;
  }
}

void NodePairingMode::begin() {
  gComplete = false;
  gBusy = false;

  if (gFoundAddress) {
    delete gFoundAddress;
    gFoundAddress = nullptr;
  }

  gFoundBleMac = "";
  gFoundName = "";
  gFoundNodeId = "";

  BLEDevice::init("GW-SCANNER");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(90);

  Serial.println("[PAIRING] Gateway node scan mode started");
}

void NodePairingMode::loop() {
  if (gComplete || gBusy) return;

  if (!gFoundAddress) {
    Serial.println("[PAIRING] Scanning for unpaired nodes...");
    BLEDevice::getScan()->start(4, false);
    delay(500);
    return;
  }

  gBusy = true;

  bool ok = connectAndPair();
  if (ok) {
    gComplete = true;
    Serial.println("[PAIRING] BLE node pairing complete");
  } else {
    Serial.println("[PAIRING] Pairing attempt failed, rescanning...");

    delete gFoundAddress;
    gFoundAddress = nullptr;
    gFoundBleMac = "";
    gFoundName = "";
    gFoundNodeId = "";
  }

  gBusy = false;
}

bool NodePairingMode::isComplete() {
  return gComplete;
}