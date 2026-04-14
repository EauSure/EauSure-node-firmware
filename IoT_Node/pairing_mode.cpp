#include "pairing_mode.h"
#include "pairing_store.h"
#include "config.h"
#include "app_state.h"
#include "display_oled.h"

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <mbedtls/md.h>

namespace {
WebServer gServer(80);
bool gComplete = false;
String gApSsid = "";
String gApPassword = "";
String gStatusTitle = "PAIRING";
String gStatusLine1 = "Booting...";
String gStatusLine2 = "";
String gStatusLine3 = "";

struct PairingProvisionPacket {
  String gatewayHardwareId;
  String nodeId;
  String wifiSsid;
  String wifiPassword;
  String apiBaseUrl;
  String pairingToken;
  bool valid = false;
};

String getNodeIdString() {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lX", (unsigned long)DEVICE_ID);
  return String(buf);
}

String clipLine(const String& value, size_t maxLen = 20) {
  if (value.length() <= maxLen) return value;
  if (maxLen <= 3) return value.substring(0, maxLen);
  return value.substring(0, maxLen - 3) + "...";
}

String extractHostFromUrl(const String& url) {
  int start = 0;
  if (url.startsWith("https://")) start = 8;
  else if (url.startsWith("http://")) start = 7;

  int slash = url.indexOf('/', start);
  if (slash < 0) return url.substring(start);
  return url.substring(start, slash);
}

String hmacSha256Hex(const String& key, const String& message) {
  unsigned char out[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(key.c_str()), key.length());
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(message.c_str()), message.length());
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);

  static const char* HEX = "0123456789abcdef";
  String hex;
  hex.reserve(64);
  for (size_t i = 0; i < sizeof(out); ++i) {
    hex += HEX[(out[i] >> 4) & 0x0F];
    hex += HEX[out[i] & 0x0F];
  }
  return hex;
}

String deriveApPassword() {
  return hmacSha256Hex(NODE_DEVICE_SECRET, String("node-ap:") + getNodeIdString()).substring(0, 12);
}

String buildNodeProof(const String& nonce, const String& gatewayHardwareId) {
  return hmacSha256Hex(
    NODE_DEVICE_SECRET,
    nonce + "|" + getNodeIdString() + "|" + gatewayHardwareId
  );
}

void renderPairingScreen(const String& title, const String& line1, const String& line2 = "", const String& line3 = "") {
  gStatusTitle = title;
  gStatusLine1 = line1;
  gStatusLine2 = line2;
  gStatusLine3 = line3;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  display.setCursor(0, 18);
  display.print(clipLine(line1));
  display.setCursor(0, 32);
  display.print(clipLine(line2));
  display.setCursor(0, 46);
  display.print(clipLine(line3));
  display.display();
}

bool parseProvisionPacket(const String& value, PairingProvisionPacket& out) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, value) != DeserializationError::Ok) {
    return false;
  }

  out.gatewayHardwareId = String(doc["gatewayHardwareId"] | "");
  out.nodeId = String(doc["nodeId"] | "");
  out.wifiSsid = String(doc["wifiSsid"] | "");
  out.wifiPassword = String(doc["wifiPassword"] | "");
  out.apiBaseUrl = String(doc["apiBaseUrl"] | "");
  out.pairingToken = String(doc["pairingToken"] | "");

  out.valid = !out.gatewayHardwareId.isEmpty() &&
              !out.nodeId.isEmpty() &&
              !out.wifiSsid.isEmpty() &&
              !out.apiBaseUrl.isEmpty() &&
              !out.pairingToken.isEmpty();
  return out.valid;
}

bool connectWifiForPairing(const String& ssid, const String& password, String& errorOut) {
  errorOut = "";
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, true);
  delay(200);
  WiFi.begin(ssid.c_str(), password.c_str());

  renderPairingScreen("PAIRING", "Joining home WiFi", ssid, "Please wait...");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    delay(300);
  }

  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "WiFi connect failed";
    renderPairingScreen("PAIRING", "WiFi failed", ssid, "Retry from gateway");
    return false;
  }

  renderPairingScreen("PAIRING", "Home WiFi connected", WiFi.localIP().toString(), "Requesting key...");
  return true;
}

void disconnectStationAfterPairing() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(false, true);
    delay(100);
  }
  WiFi.mode(WIFI_AP);
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
  req["nodeId"] = packet.nodeId;
  req["pairingToken"] = packet.pairingToken;

  String body;
  serializeJson(req, body);

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

  if (!(resp["success"] | false)) {
    errorOut = String(resp["message"] | "Pairing failed");
    http.end();
    return false;
  }

  JsonObject data = resp["data"];
  aesKeyOut = String(data["aesKey"] | "");
  nodeIdOut = String(data["nodeId"] | "");
  gatewayHardwareIdOut = String(data["gatewayHardwareId"] | "");

  if (aesKeyOut.isEmpty() || nodeIdOut.isEmpty() || gatewayHardwareIdOut.isEmpty()) {
    errorOut = "Missing fields in pair-node response";
    http.end();
    return false;
  }

  http.end();
  return true;
}

bool performProvisioningFlow(const PairingProvisionPacket& packet, String& errorOut) {
  if (!packet.valid) {
    errorOut = "Invalid provisioning payload";
    return false;
  }

  String expectedNodeId = getNodeIdString();
  String normalizedNodeId = packet.nodeId;
  normalizedNodeId.toUpperCase();
  if (normalizedNodeId != expectedNodeId) {
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

  if (!apiOk) {
    disconnectStationAfterPairing();
    errorOut = apiError;
    return false;
  }

  NodePairingData d;
  d.valid = true;
  d.gatewayHardwareId = gatewayHardwareId;
  d.nodeId = nodeId;
  d.nodeName = "iot-node";
  d.aesKeyHex = aesKey;

  if (!PairingStore::save(d)) {
    disconnectStationAfterPairing();
    errorOut = "Failed to save pairing";
    return false;
  }

  renderPairingScreen("PAIRING", "Pairing complete", "Restarting node", gatewayHardwareId);
  return true;
}

void handleIdentity() {
  renderPairingScreen("PAIRING AP", "Gateway probing", getNodeIdString(), "Await proof");

  StaticJsonDocument<256> doc;
  doc["success"] = true;
  JsonObject data = doc.createNestedObject("data");
  data["nodeId"] = getNodeIdString();
  data["nodeName"] = "iot-node";
  data["mode"] = "PAIRING_AP";

  String payload;
  serializeJson(doc, payload);
  gServer.send(200, "application/json", payload);
}

void handleProve() {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, gServer.arg("plain")) != DeserializationError::Ok) {
    gServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
    return;
  }

  String gatewayHardwareId = String(doc["gatewayHardwareId"] | "");
  String nonce = String(doc["nonce"] | "");
  if (gatewayHardwareId.isEmpty() || nonce.isEmpty()) {
    gServer.send(400, "application/json", "{\"success\":false,\"message\":\"Missing gatewayHardwareId or nonce\"}");
    return;
  }

  renderPairingScreen("PAIRING AP", "Gateway confirmed", getNodeIdString(), "Generating proof");

  StaticJsonDocument<256> resp;
  resp["success"] = true;
  JsonObject data = resp.createNestedObject("data");
  data["nodeId"] = getNodeIdString();
  data["proof"] = buildNodeProof(nonce, gatewayHardwareId);

  String payload;
  serializeJson(resp, payload);
  gServer.send(200, "application/json", payload);
}

void handleProvision() {
  PairingProvisionPacket packet;
  if (!parseProvisionPacket(gServer.arg("plain"), packet)) {
    gServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON or missing fields\"}");
    return;
  }

  renderPairingScreen("PAIRING AP", "Provision request", packet.wifiSsid, "Contacting API");

  String error;
  if (!performProvisioningFlow(packet, error)) {
    StaticJsonDocument<256> resp;
    resp["success"] = false;
    resp["message"] = error;
    String payload;
    serializeJson(resp, payload);
    renderPairingScreen("PAIRING AP", "Provision failed", error, "Waiting retry");
    gServer.send(500, "application/json", payload);
    return;
  }

  StaticJsonDocument<256> resp;
  resp["success"] = true;
  resp["message"] = "Node provisioned";
  String payload;
  serializeJson(resp, payload);
  gServer.send(200, "application/json", payload);
  gComplete = true;
}

void startPairingAp() {
  gApSsid = String("IOT-") + getNodeIdString();
  gApPassword = deriveApPassword();

  WiFi.mode(WIFI_AP);
  WiFi.softAPdisconnect(true);
  delay(100);
  if (!WiFi.softAP(gApSsid.c_str(), gApPassword.c_str())) {
    renderPairingScreen("PAIRING AP", "SoftAP failed", gApSsid, "Reboot required");
    Serial.printf("[PAIRING] SoftAP start failed for SSID=%s\n", gApSsid.c_str());
    return;
  }
  delay(100);

  IPAddress apIp = WiFi.softAPIP();
  renderPairingScreen("PAIRING AP", gApSsid, apIp.toString(), "Await MQTT confirm");

  gServer.on("/identity", HTTP_GET, handleIdentity);
  gServer.on("/prove", HTTP_POST, handleProve);
  gServer.on("/provision", HTTP_POST, handleProvision);
  gServer.begin();

  Serial.printf("[PAIRING] SoftAP ready SSID=%s IP=%s\n", gApSsid.c_str(), apIp.toString().c_str());
}
} // namespace

namespace PairingMode {

void begin() {
  gComplete = false;
  Wire.begin(I2C_SDA, I2C_SCL);
  displayInit();
  renderPairingScreen("PAIRING AP", "Starting WiFi AP", getNodeIdString(), "Please wait...");

  String deviceSecret = NODE_DEVICE_SECRET;
  if (deviceSecret.isEmpty() || deviceSecret.startsWith("replace-with-")) {
    renderPairingScreen("PAIRING AP", "Secret missing", getNodeIdString(), "Set config.h");
    Serial.println("[PAIRING] NODE_DEVICE_SECRET is not configured");
    return;
  }

  startPairingAp();
}

void loop() {
  gServer.handleClient();

  if (gComplete) {
    Serial.println("[PAIRING] Pairing complete, restarting...");
    delay(1500);
    ESP.restart();
  }

  delay(20);
}

bool isComplete() {
  return gComplete;
}

} // namespace PairingMode
