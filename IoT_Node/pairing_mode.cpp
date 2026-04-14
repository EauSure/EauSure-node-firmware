#include "pairing_mode.h"
#include "pairing_store.h"
#include "config.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

static BLECharacteristic* gTx = nullptr;
static BLEAdvertising* gAdvertising = nullptr;
static bool gComplete = false;

static const char* SERVICE_UUID = "22345678-1234-1234-1234-1234567890ab";
static const char* RX_UUID      = "22345678-1234-1234-1234-1234567890ac";
static const char* TX_UUID      = "22345678-1234-1234-1234-1234567890ad";

namespace {
struct PairingProvisionPacket {
  String gatewayHardwareId;
  String nodeId;
  String wifiSsid;
  String wifiPassword;
  String apiBaseUrl;
  String pairingToken;
  bool valid = false;
};

String extractHostFromUrl(const String& url) {
  int start = 0;
  if (url.startsWith("https://")) start = 8;
  else if (url.startsWith("http://")) start = 7;

  int slash = url.indexOf('/', start);
  if (slash < 0) return url.substring(start);
  return url.substring(start, slash);
}

String getNodeIdString() {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lX", (unsigned long)DEVICE_ID);
  return String(buf);
}

void restartAdvertising() {
  if (!gAdvertising) return;

  gAdvertising->stop();
  delay(100);

  BLEAdvertisementData scanResp;
  scanResp.setName(("IOT-" + getNodeIdString()).c_str());
  gAdvertising->setScanResponseData(scanResp);

  BLEDevice::startAdvertising();
  Serial.println("[PAIRING] Advertising restarted");
  Serial.printf("[PAIRING] BLE MAC: %s\n", BLEDevice::getAddress().toString().c_str());
}

void notifyResult(bool success, const String& message, const String& nodeId = "") {
  if (!gTx) return;

  StaticJsonDocument<256> doc;
  doc["success"] = success;
  doc["message"] = message;
  if (success && !nodeId.isEmpty()) {
    doc["nodeId"] = nodeId;
  }

  String payload;
  serializeJson(doc, payload);
  gTx->setValue(payload.c_str());
  gTx->notify();
}

bool parseProvisionPacket(const String& value, PairingProvisionPacket& out) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, value) != DeserializationError::Ok) {
    return false;
  }

  out.gatewayHardwareId = String(doc["gatewayHardwareId"] | "");
  out.nodeId            = String(doc["nodeId"] | "");
  out.wifiSsid          = String(doc["wifiSsid"] | "");
  out.wifiPassword      = String(doc["wifiPassword"] | "");
  out.apiBaseUrl        = String(doc["apiBaseUrl"] | "");
  out.pairingToken      = String(doc["pairingToken"] | "");

  out.valid = !out.gatewayHardwareId.isEmpty() &&
              !out.nodeId.isEmpty() &&
              !out.wifiSsid.isEmpty() &&
              !out.apiBaseUrl.isEmpty() &&
              !out.pairingToken.isEmpty();
  return out.valid;
}

bool connectWifiForPairing(const String& ssid, const String& password, String& errorOut) {
  errorOut = "";

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.printf("[PAIRING][WIFI] Connecting to SSID '%s'...\n", ssid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "WiFi connect failed";
    return false;
  }

  Serial.printf("[PAIRING][WIFI] Connected — IP=%s\n", WiFi.localIP().toString().c_str());
  return true;
}

void disconnectWifiAfterPairing() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[PAIRING][WIFI] Disconnecting temporary WiFi");
  }
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_OFF);
}

bool callPairNodeApi(
  const PairingProvisionPacket& packet,
  String& aesKeyOut,
  String& gatewayHardwareIdOut,
  String& nodeIdOut,
  String& errorOut
) {
  aesKeyOut = "";
  gatewayHardwareIdOut = "";
  nodeIdOut = "";
  errorOut = "";

  String host = extractHostFromUrl(packet.apiBaseUrl);
  if (host.isEmpty()) {
    errorOut = "Invalid apiBaseUrl";
    return false;
  }

  String url = packet.apiBaseUrl;
  if (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/api/registry/pair-node";

  StaticJsonDocument<256> req;
  req["gatewayHardwareId"] = packet.gatewayHardwareId;
  req["nodeId"]            = packet.nodeId;
  req["pairingToken"]      = packet.pairingToken;

  String body;
  serializeJson(req, body);

  Serial.println("[PAIRING][API] POST " + url);
  Serial.println("[PAIRING][API] Body: " + body);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setReuse(false);
  http.useHTTP10(true);
  http.setTimeout(10000);
  http.setConnectTimeout(10000);

  if (!http.begin(client, url)) {
    errorOut = "http.begin failed";
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  int code = http.POST(body);
  String response = (code > 0) ? http.getString() : "";

  Serial.printf("[PAIRING][API] HTTP code=%d\n", code);
  if (!response.isEmpty()) {
    Serial.println("[PAIRING][API] Response: " + response);
  }

  if (code <= 0) {
    errorOut = "HTTP POST failed (code=" + String(code) + ")";
    http.end();
    return false;
  }

  StaticJsonDocument<768> resp;
  if (deserializeJson(resp, response) != DeserializationError::Ok) {
    errorOut = "Invalid JSON response";
    http.end();
    return false;
  }

  bool success = resp["success"] | false;
  if (!success) {
    errorOut = String(resp["message"] | "Pairing failed");
    http.end();
    return false;
  }

  JsonObject data = resp["data"];
  aesKeyOut            = String(data["aesKey"] | "");
  nodeIdOut            = String(data["nodeId"] | "");
  gatewayHardwareIdOut = String(data["gatewayHardwareId"] | "");

  if (aesKeyOut.isEmpty() || nodeIdOut.isEmpty() || gatewayHardwareIdOut.isEmpty()) {
    errorOut = "Missing fields in pair-node response";
    http.end();
    return false;
  }

  http.end();
  return true;
}

bool performPairingFlow(const PairingProvisionPacket& packet, String& errorOut) {
  if (!packet.valid) {
    errorOut = "Invalid pairing payload";
    return false;
  }

  String expectedNodeId = getNodeIdString();
  if (packet.nodeId != expectedNodeId) {
    errorOut = "Node ID mismatch";
    return false;
  }

  String wifiError;
  if (!connectWifiForPairing(packet.wifiSsid, packet.wifiPassword, wifiError)) {
    errorOut = wifiError;
    return false;
  }

  String aesKey;
  String gatewayHardwareId;
  String nodeId;
  String apiError;
  bool apiOk = callPairNodeApi(packet, aesKey, gatewayHardwareId, nodeId, apiError);

  disconnectWifiAfterPairing();

  if (!apiOk) {
    errorOut = apiError;
    return false;
  }

  NodePairingData d;
  d.valid             = true;
  d.gatewayHardwareId = gatewayHardwareId;
  d.nodeId            = nodeId;
  d.nodeName          = "iot-node";
  d.aesKeyHex         = aesKey;

  if (!PairingStore::save(d)) {
    errorOut = "Failed to save pairing";
    return false;
  }

  Serial.printf("[PAIRING] Pairing saved gw=%s node=%s\n",
                d.gatewayHardwareId.c_str(),
                d.nodeId.c_str());
  return true;
}
}  // namespace

class PairServerCallbacks : public BLEServerCallbacks {
  void onDisconnect(BLEServer* server) override {
    (void)server;
    if (!gComplete) {
      Serial.println("[PAIRING] Client disconnected — restarting advertising");
      vTaskDelay(pdMS_TO_TICKS(200));
      restartAdvertising();
    }
  }
};

class PairRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String value = c->getValue();
    if (value.isEmpty()) return;

    PairingProvisionPacket packet;
    if (!parseProvisionPacket(value, packet)) {
      notifyResult(false, "Invalid JSON or missing fields");
      return;
    }

    Serial.printf("[PAIRING] Received pairing packet for node=%s gw=%s\n",
                  packet.nodeId.c_str(), packet.gatewayHardwareId.c_str());

    String error;
    if (!performPairingFlow(packet, error)) {
      Serial.printf("[PAIRING] Pairing failed: %s\n", error.c_str());
      notifyResult(false, error);
      restartAdvertising();
      return;
    }

    gComplete = true;
    notifyResult(true, "Node paired", packet.nodeId);
  }
};

void PairingMode::begin() {
  gComplete = false;

  String devName = "IOT-" + getNodeIdString();

  BLEDevice::init(devName.c_str());

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new PairServerCallbacks());

  BLEService* service = server->createService(BLEUUID(SERVICE_UUID));

  BLECharacteristic* rx = service->createCharacteristic(
    BLEUUID(RX_UUID),
    BLECharacteristic::PROPERTY_WRITE
  );

  gTx = service->createCharacteristic(
    BLEUUID(TX_UUID),
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );

  rx->setCallbacks(new PairRxCallbacks());

  StaticJsonDocument<160> doc;
  doc["nodeId"] = getNodeIdString();
  doc["nodeName"] = "iot-node";
  doc["nodeType"] = "water-quality";

  String infoValue;
  serializeJson(doc, infoValue);
  gTx->setValue(infoValue.c_str());

  service->start();

  gAdvertising = BLEDevice::getAdvertising();
  gAdvertising->stop();
  gAdvertising->addServiceUUID(BLEUUID(SERVICE_UUID));
  gAdvertising->setScanResponse(true);
  gAdvertising->setMinPreferred(0x06);
  gAdvertising->setMinPreferred(0x12);

  BLEAdvertisementData advData;
  advData.setCompleteServices(BLEUUID(SERVICE_UUID));
  gAdvertising->setAdvertisementData(advData);

  BLEAdvertisementData scanResp;
  scanResp.setName(devName.c_str());
  gAdvertising->setScanResponseData(scanResp);

  gAdvertising->start();

  Serial.printf("[PAIRING] Advertising as '%s'\n", devName.c_str());
  Serial.printf("[PAIRING] Service UUID: %s\n", SERVICE_UUID);
  Serial.printf("[PAIRING] BLE MAC: %s\n", BLEDevice::getAddress().toString().c_str());
}

void PairingMode::loop() {
  static uint32_t lastAliveMs = 0;
  if (millis() - lastAliveMs > 3000) {
    lastAliveMs = millis();
    Serial.printf("[PAIRING] alive — complete=%d uptime=%lus\n",
                  gComplete, millis() / 1000);
  }

  if (gComplete) {
    Serial.println("[PAIRING] Pairing complete, restarting...");
    delay(1000);
    ESP.restart();
  }
}

bool PairingMode::isComplete() {
  return gComplete;
}
