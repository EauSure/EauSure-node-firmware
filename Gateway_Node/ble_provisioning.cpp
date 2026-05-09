#include "ble_provisioning.h"
#include "config.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <esp_system.h>

namespace {
  const char* SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab";
  const char* RX_UUID      = "12345678-1234-1234-1234-1234567890ac";
  const char* TX_UUID      = "12345678-1234-1234-1234-1234567890ad";

  BLECharacteristic* gTx = nullptr;
  bool gPending = false;
  ProvisioningData gData;
  String gGatewayHardwareId;
  String gGatewayDisplayName;
  String gChallenge;
  String gChunkTransferId;
  String gChunkBuffer;
  int gChunkExpectedTotal = 0;
  int gChunkNextIndex = 0;

  void resetChunkState() {
    gChunkTransferId = "";
    gChunkBuffer = "";
    gChunkExpectedTotal = 0;
    gChunkNextIndex = 0;
  }

  void notifyProvisioningError(const String& message) {
    if (!gTx) return;
    gTx->setValue((String("{\"success\":false,\"message\":\"") + message + "\"}").c_str());
    gTx->notify();
  }

  String makeRandomHex(size_t numBytes) {
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

  String hmacSha256HexRaw(const uint8_t* key, size_t keyLen, const String& message) {
    unsigned char out[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, key, keyLen);
    mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(message.c_str()), message.length());
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);

    static const char* kHexChars = "0123456789abcdef";
    String hex;
    hex.reserve(64);
    for (size_t i = 0; i < sizeof(out); ++i) {
      hex += kHexChars[(out[i] >> 4) & 0x0F];
      hex += kHexChars[out[i] & 0x0F];
    }
    return hex;
  }

  String hmacSha256Hex(const String& key, const String& message) {
    return hmacSha256HexRaw(
      reinterpret_cast<const uint8_t*>(key.c_str()),
      key.length(),
      message
    );
  }

  bool decodeBase64(const String& input, uint8_t* out, size_t outCap, size_t& outLen) {
    outLen = 0;
    return mbedtls_base64_decode(
      out,
      outCap,
      &outLen,
      reinterpret_cast<const unsigned char*>(input.c_str()),
      input.length()
    ) == 0;
  }

  bool hexToBytes(const String& hex, uint8_t* out, size_t outLen) {
    if (hex.length() != outLen * 2) return false;

    for (size_t i = 0; i < outLen; ++i) {
      char hi = hex[i * 2];
      char lo = hex[i * 2 + 1];

      auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
      };

      int a = nibble(hi);
      int b = nibble(lo);
      if (a < 0 || b < 0) return false;
      out[i] = static_cast<uint8_t>((a << 4) | b);
    }

    return true;
  }

  bool constantEquals(const String& left, const String& right) {
    if (left.length() != right.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < left.length(); ++i) {
      diff |= static_cast<uint8_t>(left[i] ^ right[i]);
    }
    return diff == 0;
  }

  String escapeJsonString(const String& input) {
    String out;
    out.reserve(input.length() + 8);
    for (size_t i = 0; i < input.length(); ++i) {
      char c = input[i];
      if (c == '\\' || c == '"') {
        out += '\\';
        out += c;
      } else if (c == '\n') {
        out += "\\n";
      } else if (c == '\r') {
        out += "\\r";
      } else {
        out += c;
      }
    }
    return out;
  }

  bool decryptAesCbcPkcs7(
    const uint8_t* cipher,
    size_t cipherLen,
    const uint8_t key[16],
    const uint8_t iv[16],
    String& plaintextOut
  ) {
    if (cipherLen == 0 || (cipherLen % 16) != 0) return false;

    uint8_t buffer[512];
    if (cipherLen > sizeof(buffer)) return false;
    memcpy(buffer, cipher, cipherLen);

    uint8_t ivCopy[16];
    memcpy(ivCopy, iv, sizeof(ivCopy));

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, key, 128) != 0) {
      mbedtls_aes_free(&aes);
      return false;
    }

    if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, ivCopy, buffer, buffer) != 0) {
      mbedtls_aes_free(&aes);
      return false;
    }
    mbedtls_aes_free(&aes);

    uint8_t pad = buffer[cipherLen - 1];
    if (pad == 0 || pad > 16 || pad > cipherLen) return false;
    for (size_t i = 0; i < pad; ++i) {
      if (buffer[cipherLen - 1 - i] != pad) return false;
    }

    size_t plainLen = cipherLen - pad;
    plaintextOut = "";
    plaintextOut.reserve(plainLen);
    for (size_t i = 0; i < plainLen; ++i) {
      plaintextOut += static_cast<char>(buffer[i]);
    }
    return true;
  }

  String buildMacMessage(
    const String& gatewayHardwareId,
    const String& challenge,
    const String& sessionId,
    const String& ivBase64,
    const String& ciphertextBase64
  ) {
    return "gwprov:v2|" + gatewayHardwareId + "|" + challenge + "|" + sessionId + "|" + ivBase64 + "|" + ciphertextBase64;
  }

  bool parseSecureProvisioningPayload(JsonDocument& doc, ProvisioningData& outData, String& errorMessage) {
    const String gatewayHardwareId = String(doc["gatewayHardwareId"] | "");
    const String challenge = String(doc["challenge"] | "");
    const String sessionId = String(doc["sessionId"] | "");
    const String serverProof = String(doc["serverProof"] | "");
    const String ivBase64 = String(doc["iv"] | "");
    const String ciphertextBase64 = String(doc["ciphertext"] | "");
    const String mac = String(doc["mac"] | "");

    if (gatewayHardwareId.isEmpty() || challenge.isEmpty() || sessionId.isEmpty() || serverProof.isEmpty() || ivBase64.isEmpty() || ciphertextBase64.isEmpty() || mac.isEmpty()) {
      errorMessage = "Missing secure provisioning fields";
      return false;
    }

    if (gatewayHardwareId != gGatewayHardwareId) {
      errorMessage = "Gateway hardware ID mismatch";
      return false;
    }

    if (challenge != gChallenge) {
      errorMessage = "Provisioning challenge mismatch";
      return false;
    }

    const String expectedServerProof = hmacSha256Hex(
      String(GATEWAY_DEVICE_SECRET),
      "gwprov:v2|proof|" + gatewayHardwareId + "|" + challenge + "|" + sessionId
    );
    if (!constantEquals(serverProof, expectedServerProof)) {
      errorMessage = "Invalid provisioning proof";
      return false;
    }

    const String encKeyHex = hmacSha256Hex(
      String(GATEWAY_DEVICE_SECRET),
      "gwprov:v2|enc|" + gatewayHardwareId + "|" + challenge + "|" + sessionId
    ).substring(0, 32);
    const String macKeyHex = hmacSha256Hex(
      String(GATEWAY_DEVICE_SECRET),
      "gwprov:v2|mac|" + gatewayHardwareId + "|" + challenge + "|" + sessionId
    );

    uint8_t macKey[32];
    if (!hexToBytes(macKeyHex, macKey, sizeof(macKey))) {
      errorMessage = "Invalid derived MAC key";
      return false;
    }

    const String expectedMac = hmacSha256HexRaw(
      macKey,
      sizeof(macKey),
      buildMacMessage(gatewayHardwareId, challenge, sessionId, ivBase64, ciphertextBase64)
    );
    if (!constantEquals(mac, expectedMac)) {
      errorMessage = "Invalid provisioning MAC";
      return false;
    }

    uint8_t iv[16];
    uint8_t cipher[512];
    uint8_t aesKey[16];
    size_t ivLen = 0;
    size_t cipherLen = 0;

    if (!hexToBytes(encKeyHex, aesKey, sizeof(aesKey))) {
      errorMessage = "Invalid derived AES key";
      return false;
    }
    if (!decodeBase64(ivBase64, iv, sizeof(iv), ivLen) || ivLen != sizeof(iv)) {
      errorMessage = "Invalid IV";
      return false;
    }
    if (!decodeBase64(ciphertextBase64, cipher, sizeof(cipher), cipherLen)) {
      errorMessage = "Invalid ciphertext";
      return false;
    }

    String plaintext;
    if (!decryptAesCbcPkcs7(cipher, cipherLen, aesKey, iv, plaintext)) {
      errorMessage = "Provisioning decrypt failed";
      return false;
    }

    StaticJsonDocument<768> innerDoc;
    if (deserializeJson(innerDoc, plaintext) != DeserializationError::Ok) {
      errorMessage = "Provisioning plaintext JSON invalid";
      return false;
    }

    outData.ssid = String(innerDoc["ssid"] | "");
    outData.password = String(innerDoc["password"] | "");
    outData.token = String(innerDoc["token"] | "");
    outData.gatewayName = String(innerDoc["gatewayName"] | "");
    outData.valid = outData.ssid.length() > 0 && outData.password.length() > 0 && outData.token.length() > 0;

    if (!outData.valid) {
      errorMessage = "Provisioning plaintext missing required fields";
      return false;
    }

    return true;
  }

  bool parseProvisioningDocument(JsonDocument& doc, ProvisioningData& outData, String& errorMessage) {
    if ((doc["version"] | 0) == 2) {
      return parseSecureProvisioningPayload(doc, outData, errorMessage);
    }

    outData.ssid = String(doc["ssid"] | "");
    outData.password = String(doc["password"] | "");
    outData.token = String(doc["token"] | "");
    outData.gatewayName = String(doc["gatewayName"] | "");
    outData.valid = outData.ssid.length() > 0 && outData.password.length() > 0 && outData.token.length() > 0;

    if (!outData.valid) {
      errorMessage = "Missing required fields";
      return false;
    }

    return true;
  }

  void acceptProvisioningDocument(JsonDocument& doc) {
    ProvisioningData d;
    String errorMessage;
    if (!parseProvisioningDocument(doc, d, errorMessage)) {
      notifyProvisioningError(errorMessage);
      return;
    }

    gData = d;
    gPending = true;

    BleProvisioning::sendStatus(true, "Provisioning data received. Testing WiFi...", false);
  }

  bool handleChunkDocument(JsonDocument& doc) {
    if (String(doc["type"] | "") != "chunk") return false;

    const String transferId = String(doc["transferId"] | "");
    const int index = doc["index"] | -1;
    const int total = doc["total"] | 0;
    const int totalLength = doc["totalLength"] | 0;
    const String data = String(doc["data"] | "");

    if (transferId.isEmpty() || index < 0 || total <= 0 || total > 20 || data.isEmpty() || totalLength <= 0 || totalLength > 2048) {
      resetChunkState();
      notifyProvisioningError("Invalid BLE chunk");
      return true;
    }

    if (index == 0) {
      resetChunkState();
      gChunkTransferId = transferId;
      gChunkExpectedTotal = total;
      gChunkBuffer.reserve(totalLength);
    }

    if (transferId != gChunkTransferId || total != gChunkExpectedTotal || index != gChunkNextIndex) {
      resetChunkState();
      notifyProvisioningError("BLE chunk order mismatch");
      return true;
    }

    gChunkBuffer += data;
    gChunkNextIndex += 1;

    if (gChunkNextIndex < gChunkExpectedTotal) {
      return true;
    }

    if (gChunkBuffer.length() != static_cast<size_t>(totalLength)) {
      resetChunkState();
      notifyProvisioningError("BLE chunk length mismatch");
      return true;
    }

    String assembled = gChunkBuffer;
    resetChunkState();

    StaticJsonDocument<1536> assembledDoc;
    if (deserializeJson(assembledDoc, assembled)) {
      notifyProvisioningError("Invalid chunked JSON");
      return true;
    }

    acceptProvisioningDocument(assembledDoc);
    return true;
  }

  void publishHello() {
    if (!gTx) return;

    StaticJsonDocument<256> helloDoc;
    helloDoc["type"] = "hello";
    helloDoc["version"] = 2;
    helloDoc["gatewayHardwareId"] = gGatewayHardwareId;
    helloDoc["gatewayName"] = gGatewayDisplayName;
    helloDoc["challenge"] = gChallenge;

    String hello;
    serializeJson(helloDoc, hello);
    gTx->setValue(hello.c_str());
  }
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String value = c->getValue();
    if (value.length() == 0) return;

    StaticJsonDocument<1536> doc;
    if (deserializeJson(doc, value)) {
      notifyProvisioningError("Invalid JSON");
      return;
    }

    if (handleChunkDocument(doc)) return;
    acceptProvisioningDocument(doc);
  }
};

namespace BleProvisioning {

void begin(const String& gatewayHardwareId, const String& gatewayDisplayName) {
  gGatewayHardwareId = gatewayHardwareId;
  gGatewayDisplayName = gatewayDisplayName;
  gChallenge = makeRandomHex(16);
  String advertisedName = gGatewayHardwareId;
  if (!gGatewayDisplayName.isEmpty()) {
    advertisedName += "|";
    advertisedName += gGatewayDisplayName;
  }
  BLEDevice::init(advertisedName.c_str());

  BLEServer* server = BLEDevice::createServer();
  BLEService* service = server->createService(SERVICE_UUID);

  BLECharacteristic* rx = service->createCharacteristic(
    RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );

  gTx = service->createCharacteristic(
    TX_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  gTx->addDescriptor(new BLE2902());

  rx->setCallbacks(new RxCallbacks());

  service->start();
  publishHello();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();

  Serial.printf(
    "[BLE] Provisioning BLE started for %s (%s)\n",
    gGatewayHardwareId.c_str(),
    gGatewayDisplayName.c_str()
  );
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

void sendStatus(bool success, const String& message, bool final) {
  if (!gTx) return;
  String payload = String("{\"success\":") + (success ? "true" : "false") +
    ",\"final\":" + (final ? "true" : "false") +
    ",\"message\":\"" + escapeJsonString(message) + "\"}";
  gTx->setValue(payload.c_str());
  gTx->notify();
}

void restartAdvertising() {
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  if (!adv) return;
  adv->stop();
  delay(100);
  adv->start();
  Serial.println("[BLE] Provisioning advertising restarted");
}

void stop() {
  BLEDevice::deinit(true);
}

}
