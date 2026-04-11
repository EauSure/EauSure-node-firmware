#include "otaa_manager.h"

#include <ArduinoJson.h>

#include "app_state.h"
#include "lora_radio.h"

namespace {
  bool gNodePaired = false;
  bool gNodeActive = true;
  String gNodeMac = "";

  uint32_t gLastControlSeq = 0;
  uint32_t gLastHeartbeatRxAt = 0;
  uint32_t gLastStatusRxAt = 0;
  uint32_t gLastPairReqAt = 0;
  uint32_t gLastHeartbeatReqAt = 0;
  uint32_t gLastStatusReqAt = 0;

  String getGatewayMacId() {
    uint64_t chip = ESP.getEfuseMac();
    char mac[13];
    snprintf(mac, sizeof(mac), "%04X%08lX", (uint16_t)(chip >> 32), (uint32_t)chip);
    return String(mac);
  }

  bool sendControlDoc(JsonDocument& doc, const char *label) {
    String out;
    serializeJson(doc, out);

    bool ok = secureSendControl(out);
    if (!ok) {
      Serial.print("[OTAA] Control TX failed: ");
      Serial.println(label);
      gNodePaired = false;
      return false;
    }
    return true;
  }

  void sendPairReq(const char *reason) {
    JsonDocument doc;
    doc["cmd"] = "PAIR_REQ";
    doc["reason"] = reason;
    doc["gw_mac"] = getGatewayMacId();
    doc["want"] = gNodeActive ? "active" : "sleep";

    if (sendControlDoc(doc, "PAIR_REQ")) {
      gLastPairReqAt = millis();
      Serial.println("[OTAA] PAIR_REQ sent");
    }
  }

  void sendHeartbeatReq(const char *reason) {
    JsonDocument doc;
    doc["cmd"] = "HEARTBEAT_REQ";
    doc["reason"] = reason;
    doc["gw_mac"] = getGatewayMacId();
    doc["paired"] = gNodePaired;

    if (sendControlDoc(doc, "HEARTBEAT_REQ")) {
      gLastHeartbeatReqAt = millis();
      Serial.println("[OTAA] HEARTBEAT_REQ sent");
    }
  }

  void sendStatusReq(const char *reason) {
    JsonDocument doc;
    doc["cmd"] = "STATUS_REQ";
    doc["reason"] = reason;
    doc["gw_mac"] = getGatewayMacId();

    if (sendControlDoc(doc, "STATUS_REQ")) {
      gLastStatusReqAt = millis();
      Serial.println("[OTAA] STATUS_REQ sent");
    }
  }
}

void initOtaaManager() {
  gNodePaired = false;
  gNodeActive = true;
  gNodeMac = "";
  gLastControlSeq = 0;
  gLastHeartbeatRxAt = millis();
  gLastStatusRxAt = millis();
  gLastPairReqAt = 0;
  gLastHeartbeatReqAt = 0;
  gLastStatusReqAt = 0;

  sendPairReq("boot");
}

void requestNodeActive() {
  gNodeActive = true;
  JsonDocument doc;
  doc["cmd"] = "SET_ACTIVE";
  doc["value"] = 1;
  doc["gw_mac"] = getGatewayMacId();
  sendControlDoc(doc, "SET_ACTIVE");
}

void requestNodeSleep(uint32_t sleepSeconds) {
  gNodeActive = false;
  JsonDocument doc;
  doc["cmd"] = "SET_SLEEP";
  doc["seconds"] = sleepSeconds;
  doc["gw_mac"] = getGatewayMacId();
  sendControlDoc(doc, "SET_SLEEP");
}

void otaaTick() {
  const uint32_t now = millis();

  if (!gNodePaired) {
    if (now - gLastPairReqAt >= 15000) {
      sendPairReq("not_paired");
    }
    return;
  }

  if (gNodeActive) {
    if (now - gLastHeartbeatReqAt >= 10000) {
      sendHeartbeatReq("periodic");
    }

    if (now - gLastHeartbeatRxAt >= 30000) {
      sendStatusReq("heartbeat_timeout");
      if (now - gLastPairReqAt >= 5000) {
        sendPairReq("heartbeat_lost");
      }
    }
  }

  if (now - gLastStatusReqAt >= 30000) {
    sendStatusReq("periodic");
  }
}

bool handleOtaaControlFrame(const uint8_t *frame, size_t frameLen, int rssi, float snr) {
  uint32_t seq = 0;
  uint8_t plain[MAX_PLAIN_LEN + 1] = {0};
  uint16_t plainLen = 0;

  if (!parseAndVerifyControlFrame(frame, frameLen, seq, plain, plainLen)) {
    return false;
  }

  if (seq < gLastControlSeq) {
    Serial.print("[OTAA] old control seq rejected: ");
    Serial.println((unsigned long)seq);
    return true;
  }

  if (seq == gLastControlSeq) {
    sendSecureAck(seq);
    return true;
  }

  gLastControlSeq = seq;
  sendSecureAck(seq);

  plain[plainLen] = '\0';
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char*)plain);
  if (err) {
    Serial.print("[OTAA] JSON parse error: ");
    Serial.println(err.c_str());
    return true;
  }

  String evt = doc["evt"] | "";
  String state = doc["state"] | "";
  String mac = doc["mac"] | "";

  if (evt == "PAIR_OK") {
    gNodePaired = true;
    gNodeMac = mac;
    gNodeActive = (state != "sleep");
    gLastStatusRxAt = millis();
    gLastHeartbeatRxAt = millis();
    Serial.printf("[OTAA] PAIR_OK mac=%s state=%s RSSI=%d SNR=%.1f\n", gNodeMac.c_str(), state.c_str(), rssi, snr);
    return true;
  }

  if (evt == "HEARTBEAT") {
    gNodePaired = true;
    gLastHeartbeatRxAt = millis();
    if (state.length() > 0) gNodeActive = (state != "sleep");
    Serial.printf("[OTAA] HEARTBEAT state=%s RSSI=%d SNR=%.1f\n", state.c_str(), rssi, snr);
    return true;
  }

  if (evt == "STATUS") {
    gNodePaired = true;
    gLastStatusRxAt = millis();
    gLastHeartbeatRxAt = millis();
    if (state.length() > 0) gNodeActive = (state != "sleep");
    if (mac.length() > 0) gNodeMac = mac;
    Serial.printf("[OTAA] STATUS state=%s mac=%s RSSI=%d SNR=%.1f\n", state.c_str(), gNodeMac.c_str(), rssi, snr);
    return true;
  }

  Serial.print("[OTAA] Unknown control event: ");
  Serial.println(evt);
  return true;
}
