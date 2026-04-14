#include "node_pairing_mode.h"
#include "node_pairing_store.h"
#include "wifi_manager.h"
#include "wifi_store.h"
#include "api_client.h"
#include "config.h"

#include <NimBLEDevice.h>
#include <ArduinoJson.h>

static const char* SERVICE_UUID = "22345678-1234-1234-1234-1234567890ab";
static const char* RX_UUID      = "22345678-1234-1234-1234-1234567890ac";
static const char* TX_UUID      = "22345678-1234-1234-1234-1234567890ad";

static const uint32_t SCAN_SECONDS           = 4;
static const uint32_t BLE_RESULT_TIMEOUT_MS  = 20000;
static const uint32_t API_ROLLBACK_RETRY_MS  = 2000;

namespace {

enum class PairState {
  IDLE,
  SCANNING,
  CONNECTING,
  DISCOVERING,
  WAITING_CONFIRMATION,
  WRITE_PACKET,
  WAIT_NODE_RESULT,
  WAIT_KEY_READY,
  SAVE_LOCAL,
  COMPLETE,
  FAILED_WAIT,
  FATAL_ERROR
};

PairState gState = PairState::IDLE;

bool gComplete             = false;
bool gBusy                 = false;
bool gBleInitialized       = false;
bool gGatewayProvisioned   = false;
bool gConfirmationPending  = false;
bool gBleWriteDone         = false;
bool gNodeReportedSuccess  = false;
bool gNodeReportedFailure  = false;
bool gRollbackNeeded       = false;
bool gScanInProgress       = false;
bool gStopScanRequested    = false;
bool gClientConnected      = false;
bool gNotificationReceived = false;

NimBLEAdvertisedDevice* gFoundDevice = nullptr;
NimBLEClient* gClient = nullptr;
NimBLERemoteCharacteristic* gTxChar = nullptr;
NimBLERemoteCharacteristic* gRxChar = nullptr;

String gFoundBleMac = "";
String gFoundName = "";
String gFoundNodeId = "";

String gTargetNodeId = "";
String gTargetBleMac = "";
String gPendingPairingToken = "";
String gReceivedAesKey = "";
String gNodeResultMessage = "";

GatewayProvisionResult gProvisionResult;
ApiBasicResult gRollbackResult;

uint32_t gStateAtMs = 0;
uint32_t gRetryAtMs = 0;

String getGatewayHardwareIdString() {
  String mac = WiFiManager::getMacAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  return mac;
}

void resetCandidate() {
  if (gFoundDevice) {
    delete gFoundDevice;
    gFoundDevice = nullptr;
  }
  gFoundBleMac = "";
  gFoundName = "";
  gFoundNodeId = "";
}

void clearCharacteristicCache() {
  gTxChar = nullptr;
  gRxChar = nullptr;
}

void cleanupClient(bool keepBleStack = true) {
  clearCharacteristicCache();
  gClientConnected = false;
  gNotificationReceived = false;

  if (gClient) {
    if (gClient->isConnected()) {
      gClient->disconnect();
      delay(30);
    }
    NimBLEDevice::deleteClient(gClient);
    gClient = nullptr;
  }

  if (!keepBleStack) {
    gBleInitialized = false;
  }
}

void resetScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan) {
    scan->stop();
    scan->clearResults();
  }
  gScanInProgress = false;
  gStopScanRequested = false;
}

void resetPendingConfirmationState() {
  gConfirmationPending = false;
  gBleWriteDone = false;
  gNodeReportedSuccess = false;
  gNodeReportedFailure = false;
  gRollbackNeeded = false;
  gPendingPairingToken = "";
  gReceivedAesKey = "";
  gNodeResultMessage = "";
  gNotificationReceived = false;
}

void fatalError(const String& reason) {
  Serial.println("\n[PAIRING][FATAL] =============================");
  Serial.printf("[PAIRING][FATAL] %s\n", reason.c_str());
  Serial.println("[PAIRING][FATAL] Pairing halted. Please fix the issue and reboot the gateway.");
  Serial.println("[PAIRING][FATAL] =============================\n");

  resetScan();
  cleanupClient();
  resetCandidate();
  resetPendingConfirmationState();
  gState = PairState::FATAL_ERROR;
  gStateAtMs = millis();
}

bool ensureWifiReady() {
  if (WiFiManager::isConnected()) return true;
  return WiFiManager::reconnect();
}

bool ensureGatewayProvisioned(const ProvisioningData& prov) {
  if (gGatewayProvisioned) return true;

  if (!ensureWifiReady()) {
    gProvisionResult = GatewayProvisionResult{};
    gProvisionResult.message = "WiFi not ready";
    return false;
  }

  String gatewayHardwareId = getGatewayHardwareIdString();
  if (gatewayHardwareId == "000000000000" || gatewayHardwareId.length() != 12) {
    gProvisionResult = GatewayProvisionResult{};
    gProvisionResult.message = "Invalid gateway hardware ID: " + gatewayHardwareId;
    return false;
  }

  bool ok = ApiClient::provisionGateway(
    API_BASE_URL,
    gatewayHardwareId,
    GATEWAY_FIRMWARE_VERSION,
    prov.token,
    prov.gatewayName,
    gProvisionResult
  );

  if (!ok) return false;

  gGatewayProvisioned = true;
  return true;
}

bool rollbackPendingPairing() {
  if (!gRollbackNeeded || gPendingPairingToken.isEmpty()) return true;
  if (!ensureWifiReady()) return false;

  String gatewayHardwareId = getGatewayHardwareIdString();
  if (gatewayHardwareId.isEmpty()) return false;

  Serial.printf("[PAIRING][ROLLBACK] Calling rollback for gw=%s\n", gatewayHardwareId.c_str());
  bool ok = ApiClient::rollbackPairNode(
    API_BASE_URL,
    gatewayHardwareId,
    gPendingPairingToken,
    gRollbackResult
  );

  if (ok) {
    Serial.println("[PAIRING][ROLLBACK] Backend rollback succeeded");
    gRollbackNeeded = false;
  } else {
    Serial.printf("[PAIRING][ROLLBACK] Backend rollback failed code=%d msg=%s\n",
                  gRollbackResult.httpCode,
                  gRollbackResult.message.c_str());
  }
  return ok;
}

void failAndReturnToScan(const String& reason, bool shouldRollback) {
  Serial.printf("[PAIRING][FAIL] %s\n", reason.c_str());

  resetScan();
  cleanupClient();

  if (shouldRollback) {
    gRollbackNeeded = true;
    if (!rollbackPendingPairing()) {
      gRetryAtMs = millis() + API_ROLLBACK_RETRY_MS;
      gState = PairState::FAILED_WAIT;
      gStateAtMs = millis();
      return;
    }
  }

  resetPendingConfirmationState();
  resetCandidate();
  gState = PairState::SCANNING;
  gStateAtMs = millis();
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* client) override {
    gClientConnected = true;
    Serial.printf("[PAIRING][BLE] Connected, mtu=%u\n", client->getMTU());
  }

  void onDisconnect(NimBLEClient* client, int reason) override {
    (void)client;
    gClientConnected = false;
    clearCharacteristicCache();
    Serial.printf("[PAIRING][BLE] Disconnected, reason=%d\n", reason);
  }

#if defined(CONFIG_NIMBLE_CPP_IDF)
  bool onConnParamsUpdateRequest(NimBLEClient* client, const ble_gap_upd_params* params) override {
    (void)client;
    (void)params;
    return true;
  }
#endif
};

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (gFoundDevice != nullptr || dev == nullptr) return;

    String addr = dev->getAddress().toString().c_str();
    String name = dev->getName().c_str();
    addr.toUpperCase();

    bool matched = false;
    if (dev->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
      matched = true;
    } else if (name.startsWith("IOT-")) {
      matched = true;
    }

    if (!matched) return;

    gFoundDevice = new NimBLEAdvertisedDevice(*dev);
    gFoundBleMac = addr;
    gFoundName = name;
    gStopScanRequested = true;
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results;
    (void)reason;
    gScanInProgress = false;
  }
};

void handleNodeMessage(const String& json) {
  if (json.isEmpty()) return;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;

  if (!doc.containsKey("success")) return;

  bool success = doc["success"] | false;
  String nodeId = String(doc["nodeId"] | "");
  String message = String(doc["message"] | "");
  nodeId.toUpperCase();

  if (!nodeId.isEmpty() && !gTargetNodeId.isEmpty() && nodeId != gTargetNodeId) {
    Serial.printf("[PAIRING][BLE] Ignoring result for unexpected node %s\n", nodeId.c_str());
    return;
  }

  gNodeResultMessage = message;
  gNotificationReceived = true;
  if (success) {
    gNodeReportedSuccess = true;
    gNodeReportedFailure = false;
    Serial.printf("[PAIRING][BLE] Node reported success for %s\n", gTargetNodeId.c_str());
  } else {
    gNodeReportedFailure = true;
    gNodeReportedSuccess = false;
    Serial.printf("[PAIRING][BLE] Node reported failure: %s\n", message.c_str());
  }
}

void txNotifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
  (void)characteristic;
  (void)isNotify;

  String json;
  json.reserve(length);
  for (size_t i = 0; i < length; ++i) json += static_cast<char>(data[i]);
  Serial.printf("[PAIRING][BLE] Notification received: %s\n", json.c_str());
  handleNodeMessage(json);
}

void startBleStack() {
  if (gBleInitialized) return;

  NimBLEDevice::init("GW-SCANNER");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(false, false, false);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCallbacks(), false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(90);

  gBleInitialized = true;
  Serial.printf("[BLE] NimBLE stack started - heap=%u\n", ESP.getFreeHeap());
}

bool readNodeInfo(NimBLERemoteCharacteristic* txChar, String& outNodeId, String& outNodeName) {
  if (txChar == nullptr) return false;

  std::string value = txChar->readValue();
  if (value.empty()) return false;

  String json = value.c_str();
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

  outNodeId = String(doc["nodeId"] | "");
  outNodeName = String(doc["nodeName"] | "");
  outNodeId.toUpperCase();

  return !outNodeId.isEmpty();
}

bool connectAndCacheCharacteristics() {
  if (gFoundDevice == nullptr) return false;

  resetScan();
  cleanupClient();

  gClient = NimBLEDevice::createClient();
  if (!gClient) return false;

  gClient->setClientCallbacks(new ClientCallbacks(), true);
  gClient->setConnectionParams(12, 12, 0, 120);
  gClient->setConnectTimeout(5);

  Serial.printf("[PAIRING][BLE] Connecting to node [%s]...\n", gFoundBleMac.c_str());
  if (!gClient->connect(gFoundDevice, false)) return false;
  if (!gClient->isConnected()) return false;

  NimBLERemoteService* service = gClient->getService(NimBLEUUID(SERVICE_UUID));
  if (!service) return false;

  gTxChar = service->getCharacteristic(NimBLEUUID(TX_UUID));
  gRxChar = service->getCharacteristic(NimBLEUUID(RX_UUID));
  if (gTxChar == nullptr || gRxChar == nullptr) return false;

  if (gTxChar->canNotify()) {
    if (!gTxChar->subscribe(true, txNotifyCallback, false)) {
      Serial.println("[PAIRING][BLE] TX notify subscribe failed, falling back to polling");
    }
  }

  return true;
}

bool writeProvisioningPacket() {
  if (!gRxChar || !gClient || !gClient->isConnected()) return false;

  ProvisioningData prov = WifiStore::load();
  if (!prov.valid) return false;

  String gatewayHardwareId = getGatewayHardwareIdString();
  if (gatewayHardwareId.isEmpty()) return false;

  StaticJsonDocument<512> doc;
  doc["gatewayHardwareId"] = gatewayHardwareId;
  doc["nodeId"] = gTargetNodeId;
  doc["wifiSsid"] = prov.ssid;
  doc["wifiPassword"] = prov.password;
  doc["apiBaseUrl"] = API_BASE_URL;
  doc["pairingToken"] = gPendingPairingToken;

  String payload;
  serializeJson(doc, payload);

  Serial.println("[PAIRING][WRITE] Sending provisioning packet:");
  Serial.println(payload);

  try {
    bool ok = gRxChar->writeValue(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(), true);
    gBleWriteDone = ok;
    return ok;
  } catch (...) {
    return false;
  }
}

bool readNodeResultOnce() {
  if (!gTxChar || !gClient || !gClient->isConnected()) return false;

  std::string value = gTxChar->readValue();
  if (value.empty()) return false;

  String json = value.c_str();
  handleNodeMessage(json);
  return gNotificationReceived;
}

} // namespace

namespace NodePairingMode {

void begin() {
  gComplete = false;
  gBusy = false;
  gGatewayProvisioned = false;

  resetScan();
  cleanupClient();
  resetCandidate();
  resetPendingConfirmationState();

  ProvisioningData prov = WifiStore::load();
  if (!prov.valid) {
    fatalError("No valid WiFi provisioning data - re-provision the gateway via BLE");
    return;
  }
  if (prov.token.isEmpty()) {
    fatalError("No provisioning token - re-provision the gateway via BLE app");
    return;
  }

  Serial.println("[PAIRING] Initializing WiFi for node pairing...");
  if (!WiFiManager::init(prov.ssid.c_str(), prov.password.c_str())) {
    fatalError("WiFi init failed before gateway provisioning");
    return;
  }

  if (!ensureGatewayProvisioned(prov)) {
    fatalError("Gateway provisioning failed: " + gProvisionResult.message);
    return;
  }

  startBleStack();
  Serial.println("[PAIRING] Gateway node scan mode started");
  gState = PairState::SCANNING;
  gStateAtMs = millis();
}

void loop() {
  if (gComplete || gBusy) return;
  gBusy = true;

  switch (gState) {
    case PairState::SCANNING: {
      if (!gBleInitialized) startBleStack();
      if (gScanInProgress) {
        if (gStopScanRequested) {
          resetScan();
        } else {
          delay(50);
          break;
        }
      }

      resetCandidate();
      Serial.println("[PAIRING] Scanning for unpaired nodes...");
      gScanInProgress = true;
      NimBLEDevice::getScan()->start(SCAN_SECONDS, false, true);

      if (gFoundDevice == nullptr) {
        delay(250);
        break;
      }

      gState = PairState::CONNECTING;
      gStateAtMs = millis();
      break;
    }

    case PairState::CONNECTING: {
      if (!connectAndCacheCharacteristics()) {
        failAndReturnToScan("BLE connect failed", false);
        break;
      }

      gState = PairState::DISCOVERING;
      gStateAtMs = millis();
      break;
    }

    case PairState::DISCOVERING: {
      String nodeId;
      String nodeName;
      if (!readNodeInfo(gTxChar, nodeId, nodeName)) {
        failAndReturnToScan("Failed reading node info", false);
        break;
      }

      gFoundNodeId = nodeId;
      if (!nodeName.isEmpty()) gFoundName = nodeName;
      gTargetNodeId = gFoundNodeId;
      gTargetBleMac = gFoundBleMac;

      Serial.printf("[PAIRING][CANDIDATE] nodeId=%s nodeName=%s bleMac=%s\n",
                    gFoundNodeId.c_str(), gFoundName.c_str(), gFoundBleMac.c_str());

      gState = PairState::WAITING_CONFIRMATION;
      gStateAtMs = millis();
      break;
    }

    case PairState::WAITING_CONFIRMATION: {
      if (!gClient || !gClient->isConnected()) {
        failAndReturnToScan("Lost BLE connection while waiting confirmation", false);
        break;
      }

      if (gConfirmationPending) {
        gState = PairState::WRITE_PACKET;
        gStateAtMs = millis();
        break;
      }

      delay(50);
      break;
    }

    case PairState::WRITE_PACKET: {
      if (!gClient || !gClient->isConnected()) {
        failAndReturnToScan("BLE link lost before provisioning write", false);
        break;
      }

      if (!writeProvisioningPacket()) {
        failAndReturnToScan("BLE provisioning write failed", false);
        break;
      }

      gRollbackNeeded = true;
      gNotificationReceived = false;
      gNodeReportedSuccess = false;
      gNodeReportedFailure = false;
      gNodeResultMessage = "";
      gState = PairState::WAIT_NODE_RESULT;
      gStateAtMs = millis();
      break;
    }

    case PairState::WAIT_NODE_RESULT: {
      if (!gClient || !gClient->isConnected()) {
        failAndReturnToScan("Node disconnected before reporting result", true);
        break;
      }

      if (!gNotificationReceived) {
        readNodeResultOnce();
      }

      if (gNodeReportedFailure) {
        failAndReturnToScan("Node pairing failed: " + gNodeResultMessage, true);
        break;
      }
      if (gNodeReportedSuccess) {
        cleanupClient();
        resetCandidate();
        gState = PairState::WAIT_KEY_READY;
        gStateAtMs = millis();
        break;
      }

      if (millis() - gStateAtMs > BLE_RESULT_TIMEOUT_MS) {
        failAndReturnToScan("Timed out waiting for node BLE result", true);
      } else {
        delay(150);
      }
      break;
    }

    case PairState::WAIT_KEY_READY: {
      if (!gReceivedAesKey.isEmpty()) {
        gState = PairState::SAVE_LOCAL;
        gStateAtMs = millis();
      } else {
        delay(100);
      }
      break;
    }

    case PairState::SAVE_LOCAL: {
      NodePairingData local;
      local.valid = true;
      local.nodeId = gTargetNodeId;
      local.nodeName = gFoundName;
      local.gatewayHardwareId = getGatewayHardwareIdString();
      local.aesKeyHex = gReceivedAesKey;
      local.bleAddress = gTargetBleMac;

      if (!NodePairingStore::save(local)) {
        failAndReturnToScan("Failed to save local pairing data", true);
        break;
      }

      gRollbackNeeded = false;
      resetPendingConfirmationState();
      Serial.println("[PAIRING] Node pairing complete");
      gState = PairState::COMPLETE;
      gStateAtMs = millis();
      break;
    }

    case PairState::FAILED_WAIT: {
      if ((int32_t)(millis() - gRetryAtMs) >= 0) {
        failAndReturnToScan("Retrying after rollback failure", true);
      }
      break;
    }

    case PairState::COMPLETE: {
      gComplete = true;
      break;
    }

    case PairState::FATAL_ERROR: {
      static uint32_t lastPrintMs = 0;
      if (millis() - lastPrintMs > 30000) {
        lastPrintMs = millis();
        Serial.println("[PAIRING][FATAL] Halted. Reboot the gateway to retry.");
      }
      break;
    }

    case PairState::IDLE:
    default:
      break;
  }

  gBusy = false;
}

bool isComplete() {
  return gComplete;
}

bool hasCandidate() {
  return gState == PairState::WAITING_CONFIRMATION && !gFoundNodeId.isEmpty();
}

PairingCandidateInfo getCandidate() {
  PairingCandidateInfo info;
  info.valid = hasCandidate();
  info.nodeId = gFoundNodeId;
  info.nodeName = gFoundName;
  info.bleMac = gFoundBleMac;
  return info;
}

bool confirmCandidate(const String& nodeId, const String& pairingToken) {
  String normalizedNodeId = nodeId;
  normalizedNodeId.toUpperCase();

  if (gState != PairState::WAITING_CONFIRMATION) return false;
  if (normalizedNodeId.isEmpty() || normalizedNodeId != gFoundNodeId) return false;
  if (pairingToken.isEmpty()) return false;
  if (!gClient || !gClient->isConnected()) return false;

  gTargetNodeId = normalizedNodeId;
  gTargetBleMac = gFoundBleMac;
  gPendingPairingToken = pairingToken;
  gConfirmationPending = true;
  gReceivedAesKey = "";
  gNodeReportedSuccess = false;
  gNodeReportedFailure = false;
  gNodeResultMessage = "";
  gNotificationReceived = false;

  Serial.printf("[PAIRING] Candidate confirmed for node %s\n", gTargetNodeId.c_str());
  gState = PairState::WRITE_PACKET;
  gStateAtMs = millis();
  return true;
}

bool providePairingKey(const String& nodeId, const String& aesKey) {
  String normalizedNodeId = nodeId;
  normalizedNodeId.toUpperCase();

  if (normalizedNodeId.isEmpty() || normalizedNodeId != gTargetNodeId) return false;
  if (aesKey.length() != 32) return false;

  gReceivedAesKey = aesKey;
  Serial.printf("[PAIRING] Received AES key for node %s\n", gTargetNodeId.c_str());

  if (gState == PairState::WAIT_KEY_READY) {
    gState = PairState::SAVE_LOCAL;
    gStateAtMs = millis();
  }
  return true;
}

void cancelPendingConfirmation() {
  if (gState == PairState::WAITING_CONFIRMATION || gState == PairState::WAIT_KEY_READY) {
    resetScan();
    cleanupClient();
    resetPendingConfirmationState();
    resetCandidate();
    gState = PairState::SCANNING;
    gStateAtMs = millis();
  }
}

} // namespace NodePairingMode
