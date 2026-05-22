#include "mqtt_gateway.h"

#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "api_client.h"
#include "wifi_manager.h"
#include "node_pairing_mode.h"
#include "tls_utils.h"
#include "lora_radio.h"
#include "otaa_manager.h"
#include "fuota_manager.h"

namespace {

WiFiClientSecure gTlsClient;
PubSubClient     gMqttClient(gTlsClient);

String gGatewayHardwareId;
String gCommandTopic;
String gEventTopic;

uint32_t gLastConnectAttemptMs = 0;
bool gSubscribed = false;
bool gExclusiveTlsWindow = false;
bool gTlsReady = false;

constexpr size_t kPendingCommandCapacity = 4;
constexpr uint8_t kAckRetryMax = 4;
constexpr uint32_t kAckRetryBaseMs = 5000;
constexpr uint32_t kAckRetryMaxMs = 60000;

struct PendingCommand {
  bool used = false;
  bool processed = false;
  bool acked = false;
  uint8_t ackAttempts = 0;
  uint32_t nextAckAttemptMs = 0;
  String topic;
  String body;
  String cmdId;
};

PendingCommand gPendingCommands[kPendingCommandCapacity];

String getGatewayHardwareIdString() {
  String configured = String(GATEWAY_DEVICE_ID);
  configured.trim();
  configured.toUpperCase();
  if (!configured.startsWith("GW-")) {
    configured = "GW-" + configured;
  }
  return configured;
}

String makeClientId() {
  String cid = MQTT_CLIENT_ID_PREFIX;
  cid += gGatewayHardwareId;
  return cid;
}

bool ensureWifiReady() {
  if (WiFiManager::isConnected()) return true;
  return WiFiManager::reconnect();
}

void ensureTopics() {
  if (!gGatewayHardwareId.isEmpty()) return;

  gGatewayHardwareId = getGatewayHardwareIdString();
  gCommandTopic = String(MQTT_COMMAND_TOPIC_PREFIX) + gGatewayHardwareId;
  gEventTopic   = String(MQTT_EVENT_TOPIC_PREFIX) + gGatewayHardwareId;
}

void handleConfirmPairing(JsonDocument& doc) {
  String nodeId = String(doc["nodeId"] | "");
  String sessionId = String(doc["sessionId"] | "");
  String apPassword = String(doc["apPassword"] | "");

  if (nodeId.isEmpty() || sessionId.isEmpty() || apPassword.isEmpty()) {
    Serial.println("[MQTT] CONFIRM_PAIRING missing nodeId, sessionId, or apPassword");
    return;
  }

  bool ok = NodePairingMode::confirmCandidate(nodeId, sessionId, apPassword);
  Serial.printf("[MQTT] CONFIRM_PAIRING node=%s result=%s\n",
                nodeId.c_str(),
                ok ? "accepted" : "ignored");
}

void handlePairingKeyReady(JsonDocument& doc) {
  String nodeId = String(doc["nodeId"] | "");
  String aesKey = String(doc["aesKey"] | "");

  if (nodeId.isEmpty() || aesKey.isEmpty()) {
    Serial.println("[MQTT] PAIRING_KEY_READY missing nodeId or aesKey");
    return;
  }

  bool ok = NodePairingMode::providePairingKey(nodeId, aesKey);
  Serial.printf("[MQTT] PAIRING_KEY_READY node=%s result=%s\n",
                nodeId.c_str(),
                ok ? "accepted" : "ignored");
}

void handleSetConfig(JsonDocument& doc) {
  JsonObject config = doc["config"];
  if (config.isNull()) {
    Serial.println("[MQTT] SET_CONFIG missing config object");
    return;
  }

  // ── Node-side config (forward to node via LoRa) ──
  float shakeThreshold = config["shakeThreshold"] | 1.1f;
  bool  shakeEnabled   = config["shakeEnabled"]   | true;

  // ── Gateway-side config (apply locally) ──
  uint32_t measureIntervalSec = config["measureInterval"] | 0;
  bool hasNodeActive         = !config["nodeActive"].isNull();
  bool nodeActive            = config["nodeActive"] | true;
  bool hasVocalAlerts        = !config["gatewayVocalAlerts"].isNull();
  bool vocalAlerts           = config["gatewayVocalAlerts"] | true;

  Serial.printf("[MQTT] SET_CONFIG st=%.2f se=%d mi=%lu na=%d va=%d\n",
                shakeThreshold, (int)shakeEnabled,
                (unsigned long)measureIntervalSec, (int)nodeActive, (int)vocalAlerts);

  // Apply gateway-side config
  if (measureIntervalSec > 0) {
    setMeasureIntervalMs(measureIntervalSec * 1000UL);
  }
  if (hasNodeActive) {
    setNodeActiveFlag(nodeActive);
  }
  if (hasVocalAlerts) {
    setVocalAlertsEnabled(vocalAlerts);
  }

  // Queue shake config to be sent on next node wake
  queueSetConfig(shakeThreshold, shakeEnabled);
  Serial.println("[MQTT] SET_CONFIG queued for next LoRa transmission");
}

void handleUnpairNode(JsonDocument& doc) {
  Serial.println("[MQTT] UNPAIR_NODE command received");

  // Send UNPAIR LoRa frame to node — node will erase pairing and reboot
  bool ok = sendUnpair();
  Serial.printf("[MQTT] UNPAIR LoRa result=%s\n", ok ? "OK" : "FAIL");

  // Gateway side: erase local pairing state regardless of LoRa result
  // (node may be unreachable but we still clean up gateway side)
  erasePairingAndEnterPairingMode();
}

bool enqueuePendingCommand(const String& topicStr, const String& body, const String& cmdId) {
  for (PendingCommand& slot : gPendingCommands) {
    if (slot.used && !cmdId.isEmpty() && slot.cmdId == cmdId) {
      Serial.printf("[MQTT] Duplicate pending command ignored cmdId=%s\n", cmdId.c_str());
      return true;
    }
  }

  for (PendingCommand& slot : gPendingCommands) {
    if (!slot.used) {
      slot.used = true;
      slot.processed = false;
      slot.acked = cmdId.isEmpty();
      slot.ackAttempts = 0;
      slot.nextAckAttemptMs = millis();
      slot.topic = topicStr;
      slot.body = body;
      slot.cmdId = cmdId;
      return true;
    }
  }

  Serial.println("[MQTT] Pending command queue full - dropping command");
  return false;
}

bool executePendingCommand(JsonDocument& doc) {
  String cmd = String(doc["cmd"] | "");
  cmd.toUpperCase();

  if (cmd == "CONFIRM_PAIRING") {
    handleConfirmPairing(doc);
    return true;
  }

  if (cmd == "PAIRING_KEY_READY") {
    handlePairingKeyReady(doc);
    return true;
  }

  if (cmd == "SCAN_NODES") {
    Serial.println("[MQTT] SCAN_NODES command received");
    NodePairingMode::startScanning();
    return true;
  }

  if (cmd == "CANCEL_PAIRING") {
    Serial.println("[MQTT] CANCEL_PAIRING command received");
    NodePairingMode::cancelPairing();
    return true;
  }

  if (cmd == "SET_CONFIG") {
    Serial.println("[MQTT] SET_CONFIG command received");
    handleSetConfig(doc);
    return true;
  }

  if (cmd == "MEASURE_NOW") {
    Serial.println("[MQTT] MEASURE_NOW command received");
    requestMeasureNow();
    return true;
  }

  if (cmd == "ACTIVATE_NODE") {
    Serial.println("[MQTT] ACTIVATE_NODE command received");
    sendActivate();
    return true;
  }

  if (cmd == "DEACTIVATE_NODE") {
    Serial.println("[MQTT] DEACTIVATE_NODE command received");
    setNodeActiveFlag(false);
    return true;
  }

  if (cmd == "HEALTH_CHECK") {
    Serial.println("[MQTT] HEALTH_CHECK command received");
    requestMeasureNow();
    return true;
  }

  if (cmd == "UNPAIR_NODE") {
    handleUnpairNode(doc);
    return true;
  }

  if (cmd == "UPDATE_FIRMWARE") {
    String target = String(doc["target"] | "");
    String url = String(doc["url"] | "");
    String version = String(doc["version"] | "");
    String md5 = String(doc["md5"] | "");
    size_t size = (size_t)(doc["size"] | 0);
    String nodeId = String(doc["nodeId"] | "");
    String cmdId = String(doc["cmdId"] | "");

    if (target == "gateway") {
      Serial.printf("[MQTT] UPDATE_FIRMWARE gateway version=%s size=%u cmdId=%s\n",
                    version.c_str(), (unsigned)size, cmdId.c_str());
      FuotaManager::queueGatewayUpdate(url, version, md5, size, cmdId);
      return true;
    }

    if (target == "node") {
      Serial.printf("[MQTT] UPDATE_FIRMWARE node=%s version=%s size=%u cmdId=%s\n",
                    nodeId.c_str(), version.c_str(), (unsigned)size, cmdId.c_str());
      FuotaManager::queueNodeUpdate(nodeId, url, version, md5, size, cmdId);
      return true;
    }

    Serial.println("[MQTT] UPDATE_FIRMWARE missing or invalid target");
    return true;
  }

  Serial.printf("[MQTT] Ignoring unsupported cmd=%s\n", cmd.c_str());
  return true;
}

void processPendingCommands() {
  for (PendingCommand& slot : gPendingCommands) {
    if (!slot.used) {
      continue;
    }

    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, slot.body) != DeserializationError::Ok) {
      Serial.println("[MQTT] Invalid JSON payload in pending queue");
      slot = PendingCommand{};
      continue;
    }

    if (!slot.processed) {
      executePendingCommand(doc);
      slot.processed = true;
    }

    if (slot.acked) {
      slot = PendingCommand{};
      continue;
    }

    if (gExclusiveTlsWindow) {
      return;
    }

    const uint32_t now = millis();
    if ((int32_t)(now - slot.nextAckAttemptMs) < 0) {
      continue;
    }

    const String boundFuotaCmdId = FuotaManager::getBoundCommandId();
    if (!slot.cmdId.isEmpty() && boundFuotaCmdId == slot.cmdId) {
      String failReason;
      if (FuotaManager::commandShouldFail(failReason)) {
        ApiBasicResult failResult;
        MqttGateway::setExclusiveTlsWindow(true);
        const bool failOk = ApiClient::failCommand(API_BASE_URL, slot.cmdId, failReason, failResult);
        MqttGateway::setExclusiveTlsWindow(false);
        Serial.printf("[MQTT][API] Failed FUOTA command %s result=%s reason=%s\n",
                      slot.cmdId.c_str(),
                      failOk ? "OK" : "FAIL",
                      failReason.c_str());
        FuotaManager::dismissFailedJob();
        slot = PendingCommand{};
        continue;
      }

      if (!FuotaManager::commandReadyToAck()) {
        continue;
      }
    }

    ApiBasicResult ackResult;
    MqttGateway::setExclusiveTlsWindow(true);
    const bool ackOk = ApiClient::ackCommand(API_BASE_URL, slot.cmdId, ackResult);
    MqttGateway::setExclusiveTlsWindow(false);

    if (ackOk) {
      Serial.printf("[MQTT][API] Acked command %s\n", slot.cmdId.c_str());
      if (!boundFuotaCmdId.isEmpty() && boundFuotaCmdId == slot.cmdId) {
        FuotaManager::clearCommandBinding();
      }
      slot = PendingCommand{};
      continue;
    }

    ++slot.ackAttempts;
    Serial.printf("[MQTT][API] Failed to ack command %s (attempt=%u code=%d msg=%s)\n",
                  slot.cmdId.c_str(),
                  (unsigned)slot.ackAttempts,
                  ackResult.httpCode,
                  ackResult.message.c_str());

    if (slot.ackAttempts >= kAckRetryMax) {
      Serial.printf("[MQTT][API] Giving up ack retries for command %s\n", slot.cmdId.c_str());
      slot = PendingCommand{};
      continue;
    }

    uint32_t delayMs = kAckRetryBaseMs;
    for (uint8_t i = 1; i < slot.ackAttempts; ++i) {
      delayMs = min(delayMs * 2UL, kAckRetryMaxMs);
    }
    slot.nextAckAttemptMs = millis() + delayMs;
    return;
  }
}

void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);

  // Empty retained messages arrive after we clear them; ignore them silently
  if (length == 0) {
    return;
  }

  String body;
  body.reserve(length + 1);

  for (unsigned int i = 0; i < length; ++i) {
    body += (char)payload[i];
  }

  Serial.printf("[MQTT] RX topic=%s payload=%s\n", topicStr.c_str(), body.c_str());

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    Serial.println("[MQTT] Invalid JSON payload");
    return;
  }

  String cmdId = String(doc["cmdId"] | "");

  // Clear retained message from broker so we don't reprocess on reconnect.
  // After that, the command is owned by the local pending queue.
  gMqttClient.publish(topicStr.c_str(), (const uint8_t*)"", 0, true);
  enqueuePendingCommand(topicStr, body, cmdId);
}

bool connectBroker() {
  if (gExclusiveTlsWindow) {
    return false;
  }

  if (!ensureWifiReady()) {
    Serial.println("[MQTT] WiFi not ready");
    return false;
  }

  if (!gTlsReady) {
    return false;
  }

  ensureTopics();

  if (gMqttClient.connected()) return true;

  uint32_t now = millis();
  if (now - gLastConnectAttemptMs < MQTT_RECONNECT_INTERVAL_MS) {
    return false;
  }
  gLastConnectAttemptMs = now;

  Serial.printf("[MQTT] Connecting to %s:%d as %s\n",
                MQTT_BROKER_HOST,
                MQTT_BROKER_PORT,
                makeClientId().c_str());

  bool ok = gMqttClient.connect(
    makeClientId().c_str(),
    MQTT_USERNAME,
    MQTT_PASSWORD
  );

  if (!ok) {
    Serial.printf("[MQTT] Connect failed, state=%d\n", gMqttClient.state());
    gSubscribed = false;
    return false;
  }

  Serial.println("[MQTT] Connected");
  gSubscribed = false;
  return true;
}

bool ensureSubscribed() {
  if (!gMqttClient.connected()) return false;
  if (gSubscribed) return true;

  ensureTopics();

  if (!gMqttClient.subscribe(gCommandTopic.c_str(), MQTT_QOS)) {
    Serial.printf("[MQTT] Subscribe failed topic=%s\n", gCommandTopic.c_str());
    return false;
  }

  Serial.printf("[MQTT] Subscribed topic=%s\n", gCommandTopic.c_str());
  gSubscribed = true;
  return true;
}

} // namespace

namespace MqttGateway {

void begin() {
  ensureTopics();

  gTlsReady = TlsUtils::configureClient(gTlsClient, MQTT_TLS_ROOT_CA, "MQTT broker");
  if (!gTlsReady) {
    Serial.println("[MQTT] TLS CA missing - broker connection disabled until configured");
  }
  gMqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  gMqttClient.setCallback(mqttMessageCallback);
  gMqttClient.setBufferSize(1024);

  Serial.printf("[MQTT] Command topic: %s\n", gCommandTopic.c_str());
  Serial.printf("[MQTT] Event topic: %s\n", gEventTopic.c_str());
}

void loop() {
  if (gExclusiveTlsWindow) {
    if (gMqttClient.connected()) {
      Serial.println("[MQTT] Exclusive TLS window requested — disconnecting broker session");
      gMqttClient.disconnect();
    }
    gSubscribed = false;
    delay(10);
    return;
  }

  if (!connectBroker()) {
    delay(10);
    return;
  }

  ensureSubscribed();
  gMqttClient.loop();
  processPendingCommands();
}

bool isConnected() {
  return gMqttClient.connected();
}

void setExclusiveTlsWindow(bool enabled) {
  if (enabled == gExclusiveTlsWindow) {
    return;
  }

  gExclusiveTlsWindow = enabled;
  if (enabled) {
    if (gMqttClient.connected()) {
      Serial.println("[MQTT] Pausing MQTT for exclusive TLS operation");
      gMqttClient.disconnect();
    }
    gSubscribed = false;
  } else {
    Serial.println("[MQTT] Exclusive TLS window released");
    gLastConnectAttemptMs = 0;
  }
}

bool publishEvent(const String& eventName, const String& payloadJson) {
  if (eventName.isEmpty()) return false;
  ensureTopics();

  if (!connectBroker()) return false;
  if (!ensureSubscribed()) return false;

  StaticJsonDocument<768> doc;
  doc["event"] = eventName;

  DeserializationError err = deserializeJson(doc, payloadJson);
  if (err) {
    Serial.printf("[MQTT] publishEvent invalid payload JSON: %s\n", err.c_str());
    return false;
  }

  String out;
  serializeJson(doc, out);

  bool ok = gMqttClient.publish(gEventTopic.c_str(), out.c_str(), false);
  Serial.printf("[MQTT] Publish event=%s result=%s\n",
                eventName.c_str(),
                ok ? "OK" : "FAIL");
  return ok;
}

bool publishCandidateFound(const String& nodeId, const String& nodeName, const String& bleMac) {
  ensureTopics();

  if (!connectBroker()) return false;
  if (!ensureSubscribed()) return false;

  StaticJsonDocument<256> doc;
  doc["event"]    = "candidate_found";
  doc["nodeId"]   = nodeId;
  doc["nodeName"] = nodeName;
  doc["bleMac"]   = bleMac;

  String out;
  serializeJson(doc, out);

  bool ok = gMqttClient.publish(gEventTopic.c_str(), out.c_str(), false);
  Serial.printf("[MQTT] Publish candidate_found node=%s result=%s\n",
                nodeId.c_str(),
                ok ? "OK" : "FAIL");
  return ok;
}

bool publishScanComplete(bool found) {
  ensureTopics();

  if (!connectBroker()) return false;
  if (!ensureSubscribed()) return false;

  StaticJsonDocument<64> doc;
  doc["event"] = "scan_complete";
  doc["found"] = found;

  String out;
  serializeJson(doc, out);

  bool ok = gMqttClient.publish(gEventTopic.c_str(), out.c_str(), false);
  Serial.printf("[MQTT] Publish scan_complete found=%d result=%s\n",
                (int)found, ok ? "OK" : "FAIL");
  return ok;
}

bool publishPairingFailed(const String& nodeId, const String& reason) {
  ensureTopics();

  if (!connectBroker()) return false;
  if (!ensureSubscribed()) return false;

  StaticJsonDocument<256> doc;
  doc["event"]  = "pairing_failed";
  doc["nodeId"] = nodeId;
  doc["reason"] = reason;

  String out;
  serializeJson(doc, out);

  bool ok = gMqttClient.publish(gEventTopic.c_str(), out.c_str(), false);
  Serial.printf("[MQTT] Publish pairing_failed node=%s result=%s\n",
                nodeId.c_str(), ok ? "OK" : "FAIL");
  return ok;
}

String commandTopic() {
  ensureTopics();
  return gCommandTopic;
}

String eventTopic() {
  ensureTopics();
  return gEventTopic;
}

} // namespace MqttGateway
