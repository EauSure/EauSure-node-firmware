#include "pairing_mode.h"
#include "pairing_store.h"
#include "config.h"
#include "app_state.h"
#include "display_oled.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <mbedtls/md.h>

namespace {
WebServer gServer(80);
bool gComplete = false;
bool gProvisionPending = false;
bool gProvisionInProgress = false;
bool gApRestartRequested = false;
uint32_t gApRestartAtMs = 0;
uint32_t gProvisionStartAtMs = 0;
bool gRebootRequested = false;
uint32_t gRebootAtMs = 0;
bool gResumingPendingProvision = false;
String gApSsid = "";
String gApPassword = "";
String gStatusTitle = "PAIRING";
String gStatusLine1 = "Booting...";
String gStatusLine2 = "";
String gStatusLine3 = "";

constexpr int8_t kPairingStaTxPowerQuarterDbm = 20;

struct PairingProvisionPacket {
  String gatewayHardwareId;
  String nodeId;
  String wifiSsid;
  String wifiPassword;
  String apiBaseUrl;
  String pairingToken;
  bool valid = false;
};

PairingProvisionPacket gPendingProvisionPacket;

const char* wifiReasonName(uint8_t reason) {
  switch (reason) {
    case 1: return "UNSPECIFIED";
    case 2: return "AUTH_EXPIRE";
    case 3: return "AUTH_LEAVE";
    case 4: return "ASSOC_EXPIRE";
    case 5: return "ASSOC_TOOMANY";
    case 6: return "NOT_AUTHED";
    case 7: return "NOT_ASSOCED";
    case 8: return "ASSOC_LEAVE";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    default: return "UNKNOWN";
  }
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

const char* wifiModeName(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL: return "OFF";
    case WIFI_MODE_STA: return "STA";
    case WIFI_MODE_AP: return "AP";
    case WIFI_MODE_APSTA: return "AP+STA";
    default: return "UNKNOWN";
  }
}

const char* authModeName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK: return "WAPI_PSK";
    default: return "UNKNOWN";
  }
}

void logWifiSnapshot(const char* stage) {
  Serial.printf("[PAIRING][WIFI] Snapshot stage=%s mode=%s status=%s (%d) staIP=%s apIP=%s\n",
                stage,
                wifiModeName(WiFi.getMode()),
                wifiStatusName(WiFi.status()),
                WiFi.status(),
                WiFi.localIP().toString().c_str(),
                WiFi.softAPIP().toString().c_str());
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("[PAIRING][WIFI] SoftAP started");
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      Serial.println("[PAIRING][WIFI] SoftAP stopped");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.printf("[PAIRING][WIFI] STA connected to SoftAP aid=%u\n", info.wifi_ap_staconnected.aid);
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.printf("[PAIRING][WIFI] STA disconnected from SoftAP aid=%u\n", info.wifi_ap_stadisconnected.aid);
      if (!gProvisionPending && !gProvisionInProgress && !gComplete) {
        gApRestartRequested = true;
        gApRestartAtMs = millis() + 500;
        Serial.println("[PAIRING][WIFI] Scheduling SoftAP refresh after client disconnect");
      } else {
        Serial.println("[PAIRING][WIFI] Skipping SoftAP refresh because provisioning is active");
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[PAIRING][WIFI] Node STA connected to upstream WiFi");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[PAIRING][WIFI] Node STA disconnected reason=%d (%s)\n",
                    info.wifi_sta_disconnected.reason,
                    wifiReasonName(info.wifi_sta_disconnected.reason));
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[PAIRING][WIFI] Node STA got IP %s\n", WiFi.localIP().toString().c_str());
      break;
    default:
      break;
  }
}

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

  static const char* kHexChars = "0123456789abcdef";
  String hex;
  hex.reserve(64);
  for (size_t i = 0; i < sizeof(out); ++i) {
    hex += kHexChars[(out[i] >> 4) & 0x0F];
    hex += kHexChars[out[i] & 0x0F];
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

PairingProvisionPacket toProvisionPacket(const PendingProvisionData& data) {
  PairingProvisionPacket packet;
  packet.gatewayHardwareId = data.gatewayHardwareId;
  packet.nodeId = data.nodeId;
  packet.wifiSsid = data.wifiSsid;
  packet.wifiPassword = data.wifiPassword;
  packet.apiBaseUrl = data.apiBaseUrl;
  packet.pairingToken = data.pairingToken;
  packet.valid = data.valid;
  return packet;
}

void stopPairingApForProvisioning() {
  Serial.println("[PAIRING] Stopping SoftAP before home WiFi join");
  logWifiSnapshot("before-softap-stop");
  gApRestartRequested = false;
  gApRestartAtMs = 0;
  wifi_mode_t mode = WiFi.getMode();
  if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
    Serial.println("[PAIRING][WIFI] No SoftAP active, preparing pure STA join");
    WiFi.mode(WIFI_OFF);
    delay(300);
    logWifiSnapshot("after-wifi-off");
    return;
  }
  gServer.stop();
  WiFi.softAPdisconnect(true);
  delay(100);
  logWifiSnapshot("after-softap-stop");
  Serial.println("[PAIRING][WIFI] Powering WiFi radio down after SoftAP stop");
  WiFi.mode(WIFI_OFF);
  delay(300);
  logWifiSnapshot("after-wifi-off");
}

bool connectWifiForPairing(const String& ssid, const String& password, String& errorOut) {
  errorOut = "";
  Serial.printf("[PAIRING] Connecting node STA to home WiFi SSID=%s\n", ssid.c_str());
  stopPairingApForProvisioning();
  Serial.println("[PAIRING][WIFI] Resetting WiFi radio before STA join");
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  delay(300);
  logWifiSnapshot("before-home-prescan");

  esp_err_t txPowerErr = esp_wifi_set_max_tx_power(kPairingStaTxPowerQuarterDbm);
  if (txPowerErr == ESP_OK) {
    int8_t actualTxPower = 0;
    if (esp_wifi_get_max_tx_power(&actualTxPower) == ESP_OK) {
      Serial.printf("[PAIRING][WIFI] TX power requested=%d qdBm actual=%d qdBm\n",
                    kPairingStaTxPowerQuarterDbm,
                    actualTxPower);
    } else {
      Serial.printf("[PAIRING][WIFI] TX power requested=%d qdBm\n",
                    kPairingStaTxPowerQuarterDbm);
    }
  } else {
    Serial.printf("[PAIRING][WIFI] Failed to set TX power err=0x%x\n",
                  static_cast<unsigned>(txPowerErr));
  }

  int bestIndex = -1;
  int bestRssi = -1000;
  int networkCount = WiFi.scanNetworks(false, true);
  if (networkCount > 0) {
    for (int i = 0; i < networkCount; ++i) {
      if (WiFi.SSID(i) != ssid) continue;
      int rssi = WiFi.RSSI(i);
      if (bestIndex < 0 || rssi > bestRssi) {
        bestIndex = i;
        bestRssi = rssi;
      }
    }
  }

  if (bestIndex >= 0) {
    String bssid = WiFi.BSSIDstr(bestIndex);
    int channel = WiFi.channel(bestIndex);
    wifi_auth_mode_t authMode = WiFi.encryptionType(bestIndex);
    Serial.printf("[PAIRING][WIFI] Found home AP ssid=%s bssid=%s channel=%d rssi=%d auth=%s (%d)\n",
                  ssid.c_str(),
                  bssid.c_str(),
                  channel,
                  bestRssi,
                  authModeName(authMode),
                  authMode);
  } else {
    Serial.printf("[PAIRING][WIFI] Home SSID '%s' not found in prescan, proceeding with plain WiFi.begin\n", ssid.c_str());
  }
  WiFi.scanDelete();
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.printf("[PAIRING][WIFI] WiFi.begin issued in plain STA mode for SSID=%s\n", ssid.c_str());
  logWifiSnapshot("after-wifi-begin");

  renderPairingScreen("PAIRING", "Joining home WiFi", ssid, "Please wait...");
  uint32_t start = millis();
  wl_status_t lastStatus = static_cast<wl_status_t>(-1);
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    wl_status_t currentStatus = WiFi.status();
    if (currentStatus != lastStatus) {
      Serial.printf("[PAIRING][WIFI] STA status=%s (%d)\n",
                    wifiStatusName(currentStatus),
                    currentStatus);
      lastStatus = currentStatus;
    }
    delay(300);
  }

  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "WiFi connect failed";
    Serial.printf("[PAIRING] Home WiFi connection failed for SSID=%s finalStatus=%s (%d)\n",
                  ssid.c_str(),
                  wifiStatusName(WiFi.status()),
                  WiFi.status());
    renderPairingScreen("PAIRING", "WiFi failed", ssid, "Retry from gateway");
    return false;
  }

  Serial.printf("[PAIRING] Home WiFi connected IP=%s\n", WiFi.localIP().toString().c_str());
  renderPairingScreen("PAIRING", "Home WiFi connected", WiFi.localIP().toString(), "Requesting key...");
  return true;
}

void disconnectStationAfterPairing() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(false, true);
    delay(100);
  }
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
  req["nodeId"] = packet.nodeId;
  req["pairingToken"] = packet.pairingToken;

  String body;
  serializeJson(req, body);
  Serial.printf("[PAIRING][API] POST %s\n", url.c_str());

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
  Serial.printf("[PAIRING][API] pair-node code=%d\n", code);
  if (!response.isEmpty()) {
    Serial.printf("[PAIRING][API] pair-node response=%s\n", response.c_str());
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
  d.nodeName = NODE_DEVICE_NAME;
  d.aesKeyHex = aesKey;

  if (!PairingStore::save(d)) {
    disconnectStationAfterPairing();
    errorOut = "Failed to save pairing";
    return false;
  }

  PairingStore::clearPendingProvision();

  renderPairingScreen("PAIRING", "Pairing complete", "Restarting node", gatewayHardwareId);
  return true;
}

void handleIdentity() {
  Serial.println("[PAIRING] /identity requested");
  renderPairingScreen("PAIRING AP", "Gateway probing", getNodeIdString(), "Await proof");

  StaticJsonDocument<256> doc;
  doc["success"] = true;
  JsonObject data = doc.createNestedObject("data");
  data["nodeId"] = getNodeIdString();
  data["nodeName"] = NODE_DEVICE_NAME;
  data["mode"] = "PAIRING_AP";

  String payload;
  serializeJson(doc, payload);
  gServer.send(200, "application/json", payload);
}

void handleProve() {
  Serial.println("[PAIRING] /prove requested");
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
  Serial.println("[PAIRING] /provision requested");
  PairingProvisionPacket packet;
  if (!parseProvisionPacket(gServer.arg("plain"), packet)) {
    gServer.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON or missing fields\"}");
    return;
  }

  Serial.println("[PAIRING][DEBUG] Provision payload received from gateway:");
  Serial.printf("[PAIRING][DEBUG]   wifiSsid=%s\n", packet.wifiSsid.c_str());
  Serial.printf("[PAIRING][DEBUG]   wifiPassword=%s\n", packet.wifiPassword.c_str());
  Serial.printf("[PAIRING][DEBUG]   gatewayHardwareId=%s\n", packet.gatewayHardwareId.c_str());
  Serial.printf("[PAIRING][DEBUG]   nodeId=%s\n", packet.nodeId.c_str());

  if (gProvisionInProgress || gProvisionPending || gRebootRequested) {
    StaticJsonDocument<256> resp;
    resp["success"] = false;
    resp["message"] = "Provisioning already in progress";
    String payload;
    serializeJson(resp, payload);
    gServer.send(409, "application/json", payload);
    return;
  }

  PendingProvisionData pending;
  pending.valid = true;
  pending.gatewayHardwareId = packet.gatewayHardwareId;
  pending.nodeId = packet.nodeId;
  pending.wifiSsid = packet.wifiSsid;
  pending.wifiPassword = packet.wifiPassword;
  pending.apiBaseUrl = packet.apiBaseUrl;
  pending.pairingToken = packet.pairingToken;

  if (!PairingStore::savePendingProvision(pending)) {
    gServer.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to persist provision\"}");
    return;
  }

  Serial.printf("[PAIRING] Provision accepted for SSID=%s gateway=%s\n",
                packet.wifiSsid.c_str(),
                packet.gatewayHardwareId.c_str());
  Serial.println("[PAIRING] Pending provision saved - scheduling reboot into pure STA mode");
  renderPairingScreen("PAIRING AP", "Provision saved", packet.wifiSsid, "Rebooting...");

  StaticJsonDocument<256> resp;
  resp["success"] = true;
  resp["message"] = "Provisioning accepted";
  String payload;
  serializeJson(resp, payload);
  gServer.send(202, "application/json", payload);

  gRebootRequested = true;
  gRebootAtMs = millis() + 1200;
}

void startPairingAp() {
  gApSsid = String("IOT-") + getNodeIdString();
  gApPassword = deriveApPassword();

  logWifiSnapshot("before-start-softap");
  WiFi.mode(WIFI_AP);
  WiFi.softAPdisconnect(true);
  delay(100);
  logWifiSnapshot("after-softapdisconnect");
  if (!WiFi.softAP(gApSsid.c_str(), gApPassword.c_str())) {
    renderPairingScreen("PAIRING AP", "SoftAP failed", gApSsid, "Reboot required");
    Serial.printf("[PAIRING] SoftAP start failed for SSID=%s\n", gApSsid.c_str());
    return;
  }
  delay(100);
  logWifiSnapshot("after-softap-start");

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
  gProvisionPending = false;
  gProvisionInProgress = false;
  gApRestartRequested = false;
  gApRestartAtMs = 0;
  gProvisionStartAtMs = 0;
  gRebootRequested = false;
  gRebootAtMs = 0;
  gResumingPendingProvision = false;
  WiFi.onEvent(onWiFiEvent);
  Wire.begin(I2C_SDA, I2C_SCL);
  displayInit();
  renderPairingScreen("PAIRING AP", "Starting WiFi AP", getNodeIdString(), "Please wait...");

  String deviceSecret = NODE_DEVICE_SECRET;
  if (deviceSecret.isEmpty() || deviceSecret.startsWith("replace-with-")) {
    renderPairingScreen("PAIRING AP", "Secret missing", getNodeIdString(), "Set config.h");
    Serial.println("[PAIRING] NODE_DEVICE_SECRET is not configured");
    return;
  }

  if (PairingStore::hasPendingProvision()) {
    PendingProvisionData pending = PairingStore::loadPendingProvision();
    if (pending.valid) {
      gPendingProvisionPacket = toProvisionPacket(pending);
      gProvisionPending = true;
      gProvisionStartAtMs = millis() + 300;
      gResumingPendingProvision = true;
      WiFi.mode(WIFI_OFF);
      delay(100);
      renderPairingScreen("PAIRING", "Resuming setup", pending.wifiSsid, "Pure STA mode");
      Serial.printf("[PAIRING] Resuming pending provision after reboot for SSID=%s gateway=%s\n",
                    pending.wifiSsid.c_str(),
                    pending.gatewayHardwareId.c_str());
      return;
    }

    Serial.println("[PAIRING] Pending provision record was invalid, clearing it");
    PairingStore::clearPendingProvision();
  }

  startPairingAp();
}

void loop() {
  if (gRebootRequested) {
    if ((int32_t)(millis() - gRebootAtMs) >= 0) {
      Serial.println("[PAIRING] Rebooting to resume provisioning in pure STA mode");
      delay(100);
      ESP.restart();
    }
    delay(20);
    return;
  }

  gServer.handleClient();

  if (gApRestartRequested && !gProvisionPending && !gProvisionInProgress && !gComplete) {
    if ((int32_t)(millis() - gApRestartAtMs) >= 0) {
      gApRestartRequested = false;
      Serial.println("[PAIRING][WIFI] Refreshing SoftAP for next pairing step");
      startPairingAp();
    }
  }

  if (gProvisionPending && !gProvisionInProgress) {
    if ((int32_t)(millis() - gProvisionStartAtMs) < 0) {
      delay(20);
      return;
    }
    gProvisionPending = false;
    gProvisionInProgress = true;
    Serial.println("[PAIRING] Starting asynchronous provisioning flow");

    renderPairingScreen("PAIRING AP", "Joining home WiFi", gPendingProvisionPacket.wifiSsid, "Please wait...");

    String error;
    if (!performProvisioningFlow(gPendingProvisionPacket, error)) {
      Serial.printf("[PAIRING] Provisioning failed: %s\n", error.c_str());
      renderPairingScreen("PAIRING AP", "Provision failed", error, "Waiting retry");
      gProvisionInProgress = false;
      if (gResumingPendingProvision) {
        Serial.println("[PAIRING] Clearing pending provision after failed reboot-based resume");
        PairingStore::clearPendingProvision();
        gResumingPendingProvision = false;
      }
      startPairingAp();
    } else {
      Serial.println("[PAIRING] Provisioning complete");
      gResumingPendingProvision = false;
      gComplete = true;
    }
  }

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

bool isResumingPendingProvision() {
  return gResumingPendingProvision;
}

} // namespace PairingMode
