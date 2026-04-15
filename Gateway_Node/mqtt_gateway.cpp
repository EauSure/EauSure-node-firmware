#include "mqtt_gateway.h"

#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "wifi_manager.h"
#include "node_pairing_mode.h"

namespace {

WiFiClientSecure gTlsClient;
PubSubClient     gMqttClient(gTlsClient);

String gGatewayHardwareId;
String gCommandTopic;
String gEventTopic;

uint32_t gLastConnectAttemptMs = 0;
bool gSubscribed = false;
bool gExclusiveTlsWindow = false;

String getGatewayHardwareIdString() {
  String mac = WiFiManager::getMacAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  return mac;
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

void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String body;
  body.reserve(length + 1);

  for (unsigned int i = 0; i < length; ++i) {
    body += (char)payload[i];
  }

  Serial.printf("[MQTT] RX topic=%s payload=%s\n", topicStr.c_str(), body.c_str());

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    Serial.println("[MQTT] Invalid JSON payload");
    return;
  }

  String cmd = String(doc["cmd"] | "");
  cmd.toUpperCase();

  if (cmd == "CONFIRM_PAIRING") {
    handleConfirmPairing(doc);
    return;
  }

  if (cmd == "PAIRING_KEY_READY") {
    handlePairingKeyReady(doc);
    return;
  }

  Serial.printf("[MQTT] Ignoring unsupported cmd=%s\n", cmd.c_str());
}

bool connectBroker() {
  if (gExclusiveTlsWindow) {
    return false;
  }

  if (!ensureWifiReady()) {
    Serial.println("[MQTT] WiFi not ready");
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

  gTlsClient.setInsecure();
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

String commandTopic() {
  ensureTopics();
  return gCommandTopic;
}

String eventTopic() {
  ensureTopics();
  return gEventTopic;
}

} // namespace MqttGateway
