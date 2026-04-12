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
  BLEDevice::init(("IOT-" + getNodeIdString()).c_str());

  BLEServer* server = BLEDevice::createServer();
  BLEService* service = server->createService(SERVICE_UUID);

  BLECharacteristic* rx = service->createCharacteristic(
    RX_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );

  gTx = service->createCharacteristic(
    TX_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );

  rx->setCallbacks(new PairRxCallbacks());

  StaticJsonDocument<160> doc;
  doc["nodeId"] = getNodeIdString();
  doc["nodeName"] = "iot-node";
  doc["nodeType"] = "water-quality";

  String adv;
  serializeJson(doc, adv);
  gTx->setValue(adv.c_str());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->start();

  Serial.println("[PAIRING] IoT node BLE pairing mode started");
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