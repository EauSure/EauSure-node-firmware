#include "ble_provisioning.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

namespace {
  const char* SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab";
  const char* RX_UUID      = "12345678-1234-1234-1234-1234567890ac";
  const char* TX_UUID      = "12345678-1234-1234-1234-1234567890ad";

  BLECharacteristic* gTx = nullptr;
  bool gPending = false;
  ProvisioningData gData;
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string value = c->getValue();
    if (value.empty()) return;

    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, value)) {
      if (gTx) {
        gTx->setValue("{\"success\":false,\"message\":\"Invalid JSON\"}");
        gTx->notify();
      }
      return;
    }

    ProvisioningData d;
    d.ssid = String((const char*)doc["ssid"] | "");
    d.password = String((const char*)doc["password"] | "");
    d.token = String((const char*)doc["token"] | "");
    d.gatewayName = String((const char*)doc["gatewayName"] | "");
    d.valid = d.ssid.length() > 0 && d.password.length() > 0 && d.token.length() > 0;

    if (!d.valid) {
      if (gTx) {
        gTx->setValue("{\"success\":false,\"message\":\"Missing required fields\"}");
        gTx->notify();
      }
      return;
    }

    gData = d;
    gPending = true;

    if (gTx) {
      gTx->setValue("{\"success\":true,\"message\":\"Provisioning data received\"}");
      gTx->notify();
    }
  }
};

namespace BleProvisioning {

void begin(const String& deviceName) {
  BLEDevice::init(deviceName.c_str());

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
  gTx->addDescriptor(new BLE2902());

  rx->setCallbacks(new RxCallbacks());

  service->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();

  Serial.println("[BLE] Provisioning BLE started");
}

void loop() {
}

bool hasPendingData() {
  return gPending;
}

ProvisioningData consumeData() {
  gPending = false;
  return gData;
}

void stop() {
  BLEDevice::deinit(true);
}

}