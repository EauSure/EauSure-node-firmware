#include "pairing_mode.h"
#include "pairing_store.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <ArduinoJson.h>

static BLECharacteristic* gTx = nullptr;
static bool gComplete = false;

static const char* SERVICE_UUID = "22345678-1234-1234-1234-1234567890ab";
static const char* RX_UUID      = "22345678-1234-1234-1234-1234567890ac";
static const char* TX_UUID      = "22345678-1234-1234-1234-1234567890ad";

static String getNodeIdString() {
  return "7CB597E9"; // replace later with real DEVICE_ID helper
}

class PairServerCallbacks : public BLEServerCallbacks {
  void onDisconnect(BLEServer* server) override {
    if (!gComplete) {
      Serial.println("[PAIRING] Client disconnected — restarting advertising");
      BLEDevice::startAdvertising();
    }
  }
};

class PairRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String value = c->getValue();
    if (value.isEmpty()) return;

    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, value)) {
      if (gTx) {
        gTx->setValue("{\"success\":false,\"message\":\"Invalid JSON\"}");
        gTx->notify();
      }
      return;
    }

    NodePairingData d;
    d.valid = true;
    d.gatewayHardwareId = String(doc["gatewayHardwareId"] | "");
    d.nodeId = String(doc["nodeId"] | "");
    d.nodeName = String(doc["nodeName"] | "");
    d.aesKeyHex = String(doc["aesKey"] | "");

    if (d.gatewayHardwareId.isEmpty() || d.nodeId.isEmpty() || d.aesKeyHex.isEmpty()) {
      if (gTx) {
        gTx->setValue("{\"success\":false,\"message\":\"Missing fields\"}");
        gTx->notify();
      }
      return;
    }

    PairingStore::save(d);
    gComplete = true;

    if (gTx) {
      gTx->setValue("{\"success\":true,\"message\":\"Node paired\"}");
      gTx->notify();
    }
  }
};

void PairingMode::begin() {
  String devName = "IOT-" + getNodeIdString();

  BLEDevice::init(devName.c_str());

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new PairServerCallbacks());  // restart adv on disconnect

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

  String advValue;
  serializeJson(doc, advValue);
  gTx->setValue(advValue.c_str());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->stop();
  advertising->addServiceUUID(BLEUUID(SERVICE_UUID));
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);

  BLEAdvertisementData advData;
  advData.setCompleteServices(BLEUUID(SERVICE_UUID));
  advertising->setAdvertisementData(advData);

  BLEAdvertisementData scanResp;
  scanResp.setName(devName.c_str());
  advertising->setScanResponseData(scanResp);

  BLEDevice::startAdvertising();

  Serial.printf("[PAIRING] Advertising as '%s'\n", devName.c_str());
  Serial.printf("[PAIRING] Service UUID: %s\n", SERVICE_UUID);
}

void PairingMode::loop() {
  if (gComplete) {
    Serial.println("[PAIRING] Pairing complete, restarting...");
    delay(1000);
    ESP.restart();
  }
}

bool PairingMode::isComplete() {
  return gComplete;
}