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

#include <esp_bt.h>
#include <esp_bt_main.h>

static const char* SERVICE_UUID = "22345678-1234-1234-1234-1234567890ab";
static const char* RX_UUID      = "22345678-1234-1234-1234-1234567890ac";
static const char* TX_UUID      = "22345678-1234-1234-1234-1234567890ad";

enum class PairState {
  IDLE,
  SCANNING,
  HAVE_CANDIDATE,
  CONNECT_FOR_READ,
  READ_INFO,
  DISCONNECT_AFTER_READ,
  CALL_API,
  CONNECT_FOR_WRITE,
  WRITE_PACKET,
  DISCONNECT_AFTER_WRITE,
  SAVE_LOCAL,
  COMPLETE,
  FAILED_WAIT
};

static PairState gState = PairState::IDLE;

static bool gComplete = false;
static bool gBusy = false;
static bool gBleInitialized = false;
static bool gWritePhasePending = false;

static BLEAddress* gFoundAddress = nullptr;
static String gFoundBleMac = "";
static String gFoundName = "";
static String gFoundNodeId = "";

static BLEClient* gClient = nullptr;

static String gApiNodeId = "";
static String gApiNodeName = "";
static NodePairingResult gPairResult;

static uint32_t gStateAtMs = 0;
static uint32_t gRetryAtMs = 0;
static int gAttempt = 0;

static String getGatewayHardwareIdString() {
  String mac = WiFiManager::getMacAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  return mac;
}

static void resetCandidate() {
  if (gFoundAddress) {
    delete gFoundAddress;
    gFoundAddress = nullptr;
  }
  gFoundBleMac = "";
  gFoundName = "";
  gFoundNodeId = "";
  gApiNodeId = "";
  gApiNodeName = "";
  gPairResult = NodePairingResult{};
}

static void resetBleCandidateOnly() {
  if (gFoundAddress) {
    delete gFoundAddress;
    gFoundAddress = nullptr;
  }
  gFoundBleMac = "";
  gFoundName = "";
  gFoundNodeId = "";
}

static void cleanupClient() {
  if (gClient) {
    if (gClient->isConnected()) {
      gClient->disconnect();
      delay(50);
    }
    delete gClient;
    gClient = nullptr;
  }
}

static void resetScan() {
  BLEScan* scan = BLEDevice::getScan();
  if (scan) {
    scan->stop();
    delay(20);
    scan->clearResults();
  }
}

static void failAndRetry(const String& reason, uint32_t retryDelayMs = 1000) {
  Serial.printf("[PAIRING][FAIL] %s\n", reason.c_str());
  cleanupClient();
  if (gBleInitialized) resetScan();
  resetCandidate();
  gWritePhasePending = false;
  gRetryAtMs = millis() + retryDelayMs;
  gState = PairState::FAILED_WAIT;
  gStateAtMs = millis();
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (gFoundAddress != nullptr) return;

    String name = dev.getName().c_str();
    String addr = dev.getAddress().toString().c_str();

    if (dev.haveServiceUUID() && dev.isAdvertisingService(BLEUUID(SERVICE_UUID))) {
      gFoundName = name;
      gFoundAddress = new BLEAddress(dev.getAddress());
      gFoundBleMac = addr;
      gFoundBleMac.toUpperCase();
      return;
    }

    if (name.startsWith("IOT-")) {
      gFoundName = name;
      gFoundAddress = new BLEAddress(dev.getAddress());
      gFoundBleMac = addr;
      gFoundBleMac.toUpperCase();
    }
  }
};

static void stopBleStack() {
  if (!gBleInitialized) return;

  cleanupClient();

  BLEScan* scan = BLEDevice::getScan();
  if (scan) {
    scan->stop();
    delay(20);
    scan->clearResults();
  }

  BLEDevice::deinit(true);
  delay(150);

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
    esp_bluedroid_disable();
    delay(20);
  }
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    esp_bluedroid_deinit();
    delay(20);
  }

  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_bt_controller_disable();
    delay(20);
  }
  if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_deinit();
    delay(20);
  }

  gBleInitialized = false;
}

static void startBleStack() {
  if (gBleInitialized) return;

  BLEDevice::init("GW-SCANNER");

  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(90);

  gBleInitialized = true;
}

static bool readNodeInfo(
  BLERemoteCharacteristic* txChar,
  String& outNodeId,
  String& outNodeName
) {
  if (txChar == nullptr) return false;

  String json = txChar->readValue();
  if (json.isEmpty()) return false;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

  outNodeId = String(doc["nodeId"] | "");
  outNodeName = String(doc["nodeName"] | "");

  if (outNodeId.isEmpty()) return false;

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

  try {
    rxChar->writeValue((uint8_t*)payload.c_str(), payload.length(), true);
    return true;
  } catch (...) {
    return false;
  }
}

static bool ensureWifiReady() {
  if (WiFiManager::isConnected()) return true;

  if (!WiFiManager::reconnect()) {
    return false;
  }

  return true;
}

void NodePairingMode::begin() {
  gComplete = false;
  gBusy = false;
  gAttempt = 0;
  gWritePhasePending = false;

  cleanupClient();
  resetCandidate();

  ProvisioningData prov = WifiStore::load();
  if (!prov.valid) {
    Serial.println("[PAIRING] ERROR: no valid WiFi provisioning data");
    gState = PairState::FAILED_WAIT;
    gRetryAtMs = millis() + 3000;
    return;
  }

  Serial.println("[PAIRING] Initializing WiFi for node pairing...");
  if (!WiFiManager::init(prov.ssid.c_str(), prov.password.c_str())) {
    Serial.println("[PAIRING] ERROR: WiFi init failed");
    gState = PairState::FAILED_WAIT;
    gRetryAtMs = millis() + 3000;
    return;
  }

  Serial.printf("[PAIRING] WiFi connected | IP=%s | MAC=%s\n",
                WiFiManager::getIP().c_str(),
                WiFiManager::getMacAddress().c_str());

  delay(200);

  startBleStack();

  Serial.println("[PAIRING] Gateway node scan mode started");

  gState = PairState::SCANNING;
  gStateAtMs = millis();
}

void NodePairingMode::loop() {
  if (gComplete || gBusy) return;
  gBusy = true;

  switch (gState) {
    case PairState::SCANNING: {
      if (!gBleInitialized) {
        startBleStack();
      }

      if (gFoundAddress == nullptr) {
        Serial.println("[PAIRING] Scanning for unpaired nodes...");
        BLEDevice::getScan()->start(4, false);

        if (gFoundAddress != nullptr) {
          gState = PairState::HAVE_CANDIDATE;
          gStateAtMs = millis();
        } else {
          delay(300);
        }
      } else {
        gState = PairState::HAVE_CANDIDATE;
        gStateAtMs = millis();
      }
      break;
    }

    case PairState::HAVE_CANDIDATE: {
      Serial.printf("[PAIRING][CANDIDATE] name='%s' mac=%s\n",
        gFoundName.c_str(), gFoundBleMac.c_str());

      if (gWritePhasePending) {
        gState = PairState::CONNECT_FOR_WRITE;
      } else {
        gState = PairState::CONNECT_FOR_READ;
      }

      gStateAtMs = millis();
      break;
    }

    case PairState::CONNECT_FOR_READ: {
      resetScan();

      if (gFoundAddress == nullptr) {
        failAndRetry("candidate disappeared before read-connect");
        break;
      }

      gClient = BLEDevice::createClient();
      Serial.printf("[PAIRING][BLE] Connecting to node [%s] for info read...\n", gFoundBleMac.c_str());

      if (!gClient->connect(*gFoundAddress)) {
        failAndRetry("BLE connect failed in CONNECT_FOR_READ");
        break;
      }

      gState = PairState::READ_INFO;
      gStateAtMs = millis();
      break;
    }

    case PairState::READ_INFO: {
      if (!gClient || !gClient->isConnected()) {
        failAndRetry("READ_INFO entered without connected client");
        break;
      }

      BLERemoteService* service = gClient->getService(BLEUUID(SERVICE_UUID));
      if (!service) {
        failAndRetry("Service not found during READ_INFO");
        break;
      }

      BLERemoteCharacteristic* txChar = service->getCharacteristic(BLEUUID(TX_UUID));
      if (!txChar) {
        failAndRetry("TX characteristic not found during READ_INFO");
        break;
      }

      if (!readNodeInfo(txChar, gApiNodeId, gApiNodeName)) {
        failAndRetry("Failed reading node info");
        break;
      }

      gFoundNodeId = gApiNodeId;
      if (gApiNodeName.length() > 0) {
        gFoundName = gApiNodeName;
      }

      Serial.printf("[PAIRING][READ] nodeId=%s nodeName=%s\n",
        gApiNodeId.c_str(), gFoundName.c_str());

      gState = PairState::DISCONNECT_AFTER_READ;
      gStateAtMs = millis();
      break;
    }

    case PairState::DISCONNECT_AFTER_READ: {
      cleanupClient();
      delay(150);

      stopBleStack();
      delay(300);

      gState = PairState::CALL_API;
      gStateAtMs = millis();
      break;
    }

    case PairState::CALL_API: {
      ProvisioningData prov = WifiStore::load();
      if (!prov.valid || prov.token.isEmpty()) {
        failAndRetry("Missing provisioning token before API call");
        break;
      }

      if (!ensureWifiReady()) {
        failAndRetry("WiFi not ready before API call");
        break;
      }

      String gatewayHardwareId = getGatewayHardwareIdString();
      if (gatewayHardwareId == "000000000000" || gatewayHardwareId.length() != 12) {
        failAndRetry("Invalid gatewayHardwareId before API call");
        break;
      }

      bool apiOk = ApiClient::pairNode(
        API_BASE_URL,
        prov.token,
        gatewayHardwareId,
        gApiNodeId,
        gFoundName,
        gFoundBleMac,
        gPairResult
      );

      if (!apiOk) {
        failAndRetry("API pair-node failed: " + gPairResult.message, 1500);
        break;
      }

      Serial.printf("[PAIRING][API] success nodeId=%s gw=%s\n",
        gPairResult.nodeId.c_str(),
        gPairResult.gatewayHardwareId.c_str());

      delay(250);

      startBleStack();
      delay(300);

      resetBleCandidateOnly();
      gWritePhasePending = true;

      gState = PairState::SCANNING;
      gStateAtMs = millis();
      break;
    }

    case PairState::CONNECT_FOR_WRITE: {
      if (!gBleInitialized) {
        startBleStack();
      }

      if (gFoundAddress == nullptr) {
        failAndRetry("candidate missing before write-connect");
        break;
      }

      resetScan();

      gClient = BLEDevice::createClient();
      Serial.printf("[PAIRING][BLE] Connecting to node [%s] for pairing write...\n", gFoundBleMac.c_str());

      if (!gClient->connect(*gFoundAddress)) {
        failAndRetry("BLE reconnect failed in CONNECT_FOR_WRITE");
        break;
      }

      gWritePhasePending = false;
      gState = PairState::WRITE_PACKET;
      gStateAtMs = millis();
      break;
    }

    case PairState::WRITE_PACKET: {
      if (!gClient || !gClient->isConnected()) {
        failAndRetry("WRITE_PACKET entered without connected client");
        break;
      }

      BLERemoteService* service = gClient->getService(BLEUUID(SERVICE_UUID));
      if (!service) {
        failAndRetry("Service not found during WRITE_PACKET");
        break;
      }

      BLERemoteCharacteristic* rxChar = service->getCharacteristic(BLEUUID(RX_UUID));
      if (!rxChar) {
        failAndRetry("RX characteristic not found during WRITE_PACKET");
        break;
      }

      if (!writePairingPacket(
            rxChar,
            gPairResult.gatewayHardwareId,
            gPairResult.nodeId,
            gFoundName,
            gPairResult.aesKey)) {
        failAndRetry("Failed writing pairing packet");
        break;
      }

      Serial.println("[PAIRING][WRITE] Pairing payload sent to node");

      gState = PairState::DISCONNECT_AFTER_WRITE;
      gStateAtMs = millis();
      break;
    }

    case PairState::DISCONNECT_AFTER_WRITE: {
      cleanupClient();
      delay(150);
      gState = PairState::SAVE_LOCAL;
      gStateAtMs = millis();
      break;
    }

    case PairState::SAVE_LOCAL: {
      NodePairingData local;
      local.valid = true;
      local.nodeId = gPairResult.nodeId;
      local.nodeName = gFoundName;
      local.gatewayHardwareId = gPairResult.gatewayHardwareId;
      local.aesKeyHex = gPairResult.aesKey;
      local.bleAddress = gFoundBleMac;

      if (!NodePairingStore::save(local)) {
        failAndRetry("Failed to save local node pairing");
        break;
      }

      Serial.println("[PAIRING] Node pairing complete");
      stopBleStack();
      gState = PairState::COMPLETE;
      gStateAtMs = millis();
      break;
    }

    case PairState::COMPLETE: {
      gComplete = true;
      break;
    }

    case PairState::FAILED_WAIT: {
      if ((int32_t)(millis() - gRetryAtMs) >= 0) {
        gAttempt++;
        Serial.printf("[PAIRING][RETRY] restarting scan flow, attempt=%d\n", gAttempt);
        startBleStack();
        gState = PairState::SCANNING;
        gStateAtMs = millis();
      }
      break;
    }

    case PairState::IDLE:
    default:
      break;
  }

  gBusy = false;
}

bool NodePairingMode::isComplete() {
  return gComplete;
}