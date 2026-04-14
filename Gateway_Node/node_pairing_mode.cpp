#include "node_pairing_mode.h"
#include "node_pairing_store.h"
#include "wifi_manager.h"
#include "wifi_store.h"
#include "api_client.h"
#include "config.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>

static const uint32_t WAIT_KEY_TIMEOUT_MS = 30000;
static const uint32_t API_ROLLBACK_RETRY_MS = 2000;
static const char* NODE_AP_PREFIX = "IOT-";
static const char* NODE_AP_BASE_URL = "http://192.168.4.1";

namespace {

enum class PairState {
  IDLE,
  SCANNING,
  WAITING_CONFIRMATION,
  FETCHING_PROOF,
  VERIFYING_PROOF,
  SENDING_PROVISION,
  WAIT_KEY_READY,
  SAVE_LOCAL,
  COMPLETE,
  FAILED_WAIT,
  FATAL_ERROR
};

PairState gState = PairState::IDLE;
bool gComplete = false;
bool gBusy = false;
bool gGatewayProvisioned = false;
bool gConfirmationPending = false;
bool gRollbackNeeded = false;
bool gPauseMqtt = false;

String gFoundSsid = "";
String gFoundBleMac = "";
String gFoundName = "";
String gFoundNodeId = "";
String gTargetSsid = "";
String gTargetBleMac = "";
String gTargetNodeId = "";
String gPendingSessionId = "";
String gNodeApPassword = "";
String gPendingPairingToken = "";
String gReceivedAesKey = "";
String gChallengeNonce = "";
String gChallengeProof = "";

GatewayProvisionResult gProvisionResult;
PairingTokenResult gPairingTokenResult;
ApiBasicResult gRollbackResult;
uint32_t gStateAtMs = 0;
uint32_t gRetryAtMs = 0;

String getGatewayHardwareIdString() {
  String mac = WiFiManager::getMacAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  return mac;
}

String makeNonceHex(size_t numBytes) {
  static const char* kHexChars = "0123456789abcdef";
  String out;
  out.reserve(numBytes * 2);
  for (size_t i = 0; i < numBytes; ++i) {
    uint8_t value = static_cast<uint8_t>(esp_random() & 0xFF);
    out += kHexChars[(value >> 4) & 0x0F];
    out += kHexChars[value & 0x0F];
  }
  return out;
}

void resetCandidate() {
  gFoundSsid = "";
  gFoundBleMac = "";
  gFoundName = "";
  gFoundNodeId = "";
}

void resetPendingConfirmationState() {
  gConfirmationPending = false;
  gRollbackNeeded = false;
  gTargetSsid = "";
  gTargetBleMac = "";
  gTargetNodeId = "";
  gPendingSessionId = "";
  gNodeApPassword = "";
  gPendingPairingToken = "";
  gReceivedAesKey = "";
  gChallengeNonce = "";
  gChallengeProof = "";
}

void fatalError(const String& reason) {
  Serial.println("\n[PAIRING][FATAL] =============================");
  Serial.printf("[PAIRING][FATAL] %s\n", reason.c_str());
  Serial.println("[PAIRING][FATAL] Pairing halted. Please fix the issue and reboot the gateway.");
  Serial.println("[PAIRING][FATAL] =============================\n");
  gPauseMqtt = false;
  resetCandidate();
  resetPendingConfirmationState();
  gState = PairState::FATAL_ERROR;
  gStateAtMs = millis();
}

bool ensureHomeWifiReady() {
  gPauseMqtt = false;
  if (WiFiManager::isConnected()) return true;
  return WiFiManager::reconnect();
}

bool ensureGatewayProvisioned(const ProvisioningData& prov) {
  if (gGatewayProvisioned) return true;
  if (!ensureHomeWifiReady()) {
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
    GATEWAY_DEVICE_SECRET,
    prov.token,
    prov.gatewayName,
    gProvisionResult
  );

  if (!ok) return false;
  gGatewayProvisioned = true;
  return true;
}

bool connectToWifi(const String& ssid, const String& password, uint32_t timeoutMs, const String& label) {
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.printf("[PAIRING][WIFI] Connecting to %s '%s'\n", label.c_str(), ssid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[PAIRING][WIFI] Failed connecting to %s '%s'\n", label.c_str(), ssid.c_str());
    return false;
  }

  Serial.printf("[PAIRING][WIFI] Connected to %s | IP=%s RSSI=%d\n",
                label.c_str(),
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
  return true;
}

void logNodeApScanResult(const String& expectedSsid) {
  if (expectedSsid.isEmpty()) return;

  Serial.printf("[PAIRING][WIFI] Scanning for AP '%s' before connect...\n", expectedSsid.c_str());
  int count = WiFi.scanNetworks(false, true);
  if (count <= 0) {
    Serial.println("[PAIRING][WIFI] No APs found during diagnostic scan");
    WiFi.scanDelete();
    return;
  }

  bool found = false;
  for (int i = 0; i < count; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid == expectedSsid) {
      found = true;
      Serial.printf("[PAIRING][WIFI] Found target AP ssid=%s bssid=%s rssi=%d channel=%d\n",
                    ssid.c_str(),
                    WiFi.BSSIDstr(i).c_str(),
                    WiFi.RSSI(i),
                    WiFi.channel(i));
    }
  }

  if (!found) {
    Serial.printf("[PAIRING][WIFI] Target AP '%s' not visible in scan\n", expectedSsid.c_str());
  }

  WiFi.scanDelete();
}

bool connectToNodeAp() {
  if (gTargetSsid.isEmpty() || gNodeApPassword.isEmpty()) return false;
  gPauseMqtt = true;
  delay(300);
  bool ok = connectToWifi(gTargetSsid, gNodeApPassword, WIFI_TIMEOUT_MS + 5000, "node AP");
  if (!ok) {
    logNodeApScanResult(gTargetSsid);
  }
  return ok;
}

bool reconnectHomeWifi() {
  WiFi.disconnect(true, true);
  delay(150);
  gPauseMqtt = false;
  return WiFiManager::reconnect();
}

bool scanForCandidate() {
  if (!ensureHomeWifiReady()) return false;

  Serial.println("[PAIRING] Scanning nearby WiFi APs for unpaired nodes...");
  int count = WiFi.scanNetworks(false, true);
  if (count <= 0) {
    WiFi.scanDelete();
    return false;
  }

  int bestIndex = -1;
  int bestRssi = -1000;
  for (int i = 0; i < count; ++i) {
    String ssid = WiFi.SSID(i);
    if (!ssid.startsWith(NODE_AP_PREFIX)) continue;
    const int rssi = WiFi.RSSI(i);
    if (bestIndex < 0 || rssi > bestRssi) {
      bestIndex = i;
      bestRssi = rssi;
    }
  }

  if (bestIndex < 0) {
    WiFi.scanDelete();
    return false;
  }

  gFoundSsid = WiFi.SSID(bestIndex);
  gFoundNodeId = gFoundSsid.substring(strlen(NODE_AP_PREFIX));
  gFoundNodeId.toUpperCase();
  gFoundBleMac = WiFi.BSSIDstr(bestIndex);
  gFoundBleMac.toUpperCase();
  gFoundName = gFoundSsid;

  Serial.printf("[PAIRING][CANDIDATE] ssid=%s nodeId=%s bssid=%s rssi=%d\n",
                gFoundSsid.c_str(),
                gFoundNodeId.c_str(),
                gFoundBleMac.c_str(),
                bestRssi);

  WiFi.scanDelete();
  return true;
}

bool fetchNodeProof() {
  String gatewayHardwareId = getGatewayHardwareIdString();
  if (gatewayHardwareId.isEmpty()) return false;
  if (!connectToNodeAp()) return false;

  bool ok = false;
  String errorMessage;
  HTTPClient http;

  do {
    if (!http.begin(String(NODE_AP_BASE_URL) + "/identity")) {
      errorMessage = "identity begin failed";
      break;
    }
    int code = http.GET();
    String response = (code > 0) ? http.getString() : "";
    http.end();
    if (code <= 0) {
      errorMessage = "identity request failed";
      break;
    }

    StaticJsonDocument<512> identityDoc;
    if (deserializeJson(identityDoc, response) != DeserializationError::Ok) {
      errorMessage = "identity JSON invalid";
      break;
    }

    String nodeId = String(identityDoc["data"]["nodeId"] | "");
    nodeId.toUpperCase();
    if (nodeId.isEmpty() || nodeId != gTargetNodeId) {
      errorMessage = "identity mismatch";
      break;
    }

    gChallengeNonce = makeNonceHex(16);

    StaticJsonDocument<256> req;
    req["gatewayHardwareId"] = gatewayHardwareId;
    req["nonce"] = gChallengeNonce;
    String body;
    serializeJson(req, body);

    if (!http.begin(String(NODE_AP_BASE_URL) + "/prove")) {
      errorMessage = "prove begin failed";
      break;
    }
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
    response = (code > 0) ? http.getString() : "";
    http.end();
    if (code <= 0) {
      errorMessage = "prove request failed";
      break;
    }

    StaticJsonDocument<512> proofDoc;
    if (deserializeJson(proofDoc, response) != DeserializationError::Ok) {
      errorMessage = "proof JSON invalid";
      break;
    }

    if (!(proofDoc["success"] | false)) {
      errorMessage = String(proofDoc["message"] | "node proof failed");
      break;
    }

    gChallengeProof = String(proofDoc["data"]["proof"] | "");
    if (gChallengeProof.isEmpty()) {
      errorMessage = "missing proof";
      break;
    }

    ok = true;
  } while (false);

  if (!reconnectHomeWifi()) {
    Serial.println("[PAIRING][WIFI] Failed reconnecting to home WiFi after proof fetch");
    return false;
  }

  if (!ok) {
    Serial.printf("[PAIRING][FAIL] fetchNodeProof: %s\n", errorMessage.c_str());
  }
  return ok;
}

bool requestPairingToken() {
  if (!ensureHomeWifiReady()) return false;

  PairingTokenResult result;
  bool ok = ApiClient::verifyNodeProof(
    API_BASE_URL,
    getGatewayHardwareIdString(),
    gTargetNodeId,
    gPendingSessionId,
    gChallengeNonce,
    gChallengeProof,
    result
  );

  gPairingTokenResult = result;
  if (!ok) return false;
  gPendingPairingToken = result.pairingToken;
  return true;
}

bool sendProvisionToNode() {
  ProvisioningData prov = WifiStore::load();
  if (!prov.valid || gPendingPairingToken.isEmpty()) return false;
  if (!connectToNodeAp()) return false;

  bool ok = false;
  String errorMessage;
  HTTPClient http;

  do {
    StaticJsonDocument<512> req;
    req["gatewayHardwareId"] = getGatewayHardwareIdString();
    req["nodeId"] = gTargetNodeId;
    req["wifiSsid"] = prov.ssid;
    req["wifiPassword"] = prov.password;
    req["apiBaseUrl"] = API_BASE_URL;
    req["pairingToken"] = gPendingPairingToken;

    String body;
    serializeJson(req, body);

    Serial.println("[PAIRING][DEBUG] Provision payload about to be sent to node:");
    Serial.printf("[PAIRING][DEBUG]   wifiSsid=%s\n", prov.ssid.c_str());
    Serial.printf("[PAIRING][DEBUG]   wifiPassword=%s\n", prov.password.c_str());
    Serial.printf("[PAIRING][DEBUG]   nodeId=%s\n", gTargetNodeId.c_str());

    if (!http.begin(String(NODE_AP_BASE_URL) + "/provision")) {
      errorMessage = "provision begin failed";
      break;
    }
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    String response = (code > 0) ? http.getString() : "";
    http.end();
    if (code <= 0) {
      errorMessage = "provision request failed";
      break;
    }

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
      errorMessage = "provision JSON invalid";
      break;
    }

    if (!(doc["success"] | false)) {
      errorMessage = String(doc["message"] | "provision failed");
      break;
    }

    ok = true;
  } while (false);

  if (!reconnectHomeWifi()) {
    Serial.println("[PAIRING][WIFI] Failed reconnecting to home WiFi after provisioning");
    return false;
  }

  if (!ok) {
    Serial.printf("[PAIRING][FAIL] sendProvisionToNode: %s\n", errorMessage.c_str());
  }
  return ok;
}

bool rollbackPendingPairing() {
  if (!gRollbackNeeded || gPendingPairingToken.isEmpty() || gTargetNodeId.isEmpty()) return true;
  if (!ensureHomeWifiReady()) return false;

  Serial.printf("[PAIRING][ROLLBACK] Calling rollback for node=%s\n", gTargetNodeId.c_str());
  bool ok = ApiClient::rollbackPairNode(
    API_BASE_URL,
    getGatewayHardwareIdString(),
    gTargetNodeId,
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

  if (gPauseMqtt && !reconnectHomeWifi()) {
    Serial.println("[PAIRING][FAIL] Could not restore home WiFi before retry");
  }

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
  gPauseMqtt = false;
  gState = PairState::SCANNING;
  gStateAtMs = millis();
}

} // namespace

namespace NodePairingMode {

void begin() {
  gComplete = false;
  gBusy = false;
  gGatewayProvisioned = false;
  gPauseMqtt = false;

  resetCandidate();
  resetPendingConfirmationState();

  ProvisioningData prov = WifiStore::load();
  if (!prov.valid) {
    fatalError("No valid WiFi provisioning data - re-provision the gateway");
    return;
  }
  if (prov.token.isEmpty()) {
    fatalError("No provisioning token - re-provision the gateway");
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

  Serial.println("[PAIRING] WiFi pairing scan mode started");
  gState = PairState::SCANNING;
  gStateAtMs = millis();
}

void loop() {
  if (gComplete || gBusy) return;
  gBusy = true;

  switch (gState) {
    case PairState::SCANNING:
      resetCandidate();
      if (scanForCandidate()) {
        gState = PairState::WAITING_CONFIRMATION;
        gStateAtMs = millis();
      } else {
        delay(1000);
      }
      break;

    case PairState::WAITING_CONFIRMATION:
      if (!ensureHomeWifiReady()) {
        delay(500);
        break;
      }
      if (gConfirmationPending) {
        gState = PairState::FETCHING_PROOF;
        gStateAtMs = millis();
      } else {
        delay(100);
      }
      break;

    case PairState::FETCHING_PROOF:
      if (!fetchNodeProof()) {
        failAndReturnToScan("Failed to fetch node proof over WiFi", false);
        break;
      }
      gState = PairState::VERIFYING_PROOF;
      gStateAtMs = millis();
      break;

    case PairState::VERIFYING_PROOF:
      if (!requestPairingToken()) {
        failAndReturnToScan("Failed verifying node proof: " + gPairingTokenResult.message, false);
        break;
      }
      gState = PairState::SENDING_PROVISION;
      gStateAtMs = millis();
      break;

    case PairState::SENDING_PROVISION:
      if (!sendProvisionToNode()) {
        failAndReturnToScan("Failed sending WiFi provisioning to node", true);
        break;
      }
      gRollbackNeeded = true;
      gState = PairState::WAIT_KEY_READY;
      gStateAtMs = millis();
      break;

    case PairState::WAIT_KEY_READY:
      if (!ensureHomeWifiReady()) {
        delay(500);
        break;
      }
      if (!gReceivedAesKey.isEmpty()) {
        gState = PairState::SAVE_LOCAL;
        gStateAtMs = millis();
      } else if (millis() - gStateAtMs > WAIT_KEY_TIMEOUT_MS) {
        failAndReturnToScan("Timed out waiting for PAIRING_KEY_READY", true);
      } else {
        delay(150);
      }
      break;

    case PairState::SAVE_LOCAL: {
      NodePairingData local;
      local.valid = true;
      local.nodeId = gTargetNodeId;
      local.nodeName = gFoundName;
      local.gatewayHardwareId = getGatewayHardwareIdString();
      local.aesKeyHex = gReceivedAesKey;
      local.bleAddress = gTargetBleMac;

      if (!NodePairingStore::save(local)) {
        failAndReturnToScan("Failed saving local node pairing", true);
        break;
      }

      gRollbackNeeded = false;
      resetPendingConfirmationState();
      Serial.println("[PAIRING] Node pairing complete");
      gState = PairState::COMPLETE;
      gStateAtMs = millis();
      break;
    }

    case PairState::FAILED_WAIT:
      if ((int32_t)(millis() - gRetryAtMs) >= 0) {
        failAndReturnToScan("Retrying after rollback failure", true);
      }
      break;

    case PairState::COMPLETE:
      gComplete = true;
      break;

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

bool shouldPauseMqtt() {
  return gPauseMqtt;
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

bool confirmCandidate(const String& nodeId, const String& sessionId, const String& apPassword) {
  String normalizedNodeId = nodeId;
  normalizedNodeId.toUpperCase();

  if (gState != PairState::WAITING_CONFIRMATION) return false;
  if (normalizedNodeId.isEmpty() || normalizedNodeId != gFoundNodeId) return false;
  if (sessionId.isEmpty() || apPassword.isEmpty()) return false;

  gTargetNodeId = normalizedNodeId;
  gTargetSsid = gFoundSsid;
  gTargetBleMac = gFoundBleMac;
  gPendingSessionId = sessionId;
  gNodeApPassword = apPassword;
  gConfirmationPending = true;
  gPendingPairingToken = "";
  gReceivedAesKey = "";
  gChallengeNonce = "";
  gChallengeProof = "";

  Serial.printf("[PAIRING] Candidate confirmed for node %s\n", gTargetNodeId.c_str());
  gState = PairState::FETCHING_PROOF;
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
    if (gPauseMqtt) reconnectHomeWifi();
    resetPendingConfirmationState();
    resetCandidate();
    gPauseMqtt = false;
    gState = PairState::SCANNING;
    gStateAtMs = millis();
  }
}

} // namespace NodePairingMode
