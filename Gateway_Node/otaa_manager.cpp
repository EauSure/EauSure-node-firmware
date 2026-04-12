#include "otaa_manager.h"
#include "lora_radio.h"
#include "app_state.h"
#include <ArduinoJson.h>

// =====================================================
// Internal state
// =====================================================
namespace {
  bool     gNodePaired   = false;
  bool     gNodeActive   = false;
  String   gNodeMac      = "";

  uint32_t gLastMeasureAt   = 0;
  uint32_t gLastHeartbeatAt = 0;
  uint32_t gLastActivateAt  = 0;

  uint32_t gLastCommandTxAt = 0;
  static const uint32_t COMMAND_GAP_MS = 300;

  bool     gActivatePending = true;   // true until first ACTIVATE_OK received

  // How long to wait between ACTIVATE retries at boot (ms)
  static const uint32_t ACTIVATE_RETRY_MS  = 8000;
  // Measure interval
  static const uint32_t MEASURE_INTERVAL_MS   = 60000;
  // Heartbeat interval
  static const uint32_t HEARTBEAT_INTERVAL_MS = 15000;
}

// =====================================================
// initOtaaManager
// Called once from setup() after LoRa and WiFi are up.
// Sends the first ACTIVATE after a short boot delay so
// the IoT node has time to start its ControlTask.
// =====================================================
void initOtaaManager() {
  gNodePaired      = false;
  gNodeActive      = false;
  gNodeMac         = "";
  gActivatePending = true;
  gLastMeasureAt   = millis();   // avoid immediate MEASURE_REQ before pairing
  gLastHeartbeatAt = millis();
  gLastActivateAt  = 0;          // force first attempt immediately in otaaTick

  Serial.println("[OTAA] Manager ready — will send ACTIVATE on first tick");
}

// =====================================================
// otaaTick
// Called every iteration of loop().
// Drives the boot handshake and periodic command schedule.
// =====================================================
void otaaTick() {
  const uint32_t now = millis();

  if (isGatewayCommandInFlight()) {
    return;
  }

  // ── Phase 1: ACTIVATE handshake ──
  // Retry every ACTIVATE_RETRY_MS until ACTIVATE_OK received.
  if (gActivatePending) {
    if ((now - gLastActivateAt >= ACTIVATE_RETRY_MS) &&
        (now - gLastCommandTxAt >= COMMAND_GAP_MS)) {
      gLastActivateAt = now;
      Serial.println("[OTAA] Sending ACTIVATE...");
      gLastCommandTxAt = now;
      if (sendActivate()) {
        Serial.println("[OTAA] ACTIVATE delivered — waiting for ACTIVATE_OK");
      } else {
        Serial.println("[OTAA] ACTIVATE failed — will retry");
      }
    }
    return;   // don't send MEASURE_REQ or HEARTBEAT until paired
  }

  // ── Phase 2: Periodic HEARTBEAT ──
  if ((now - gLastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) &&
      (now - gLastCommandTxAt >= COMMAND_GAP_MS)) {
    gLastHeartbeatAt = now;
    gLastCommandTxAt = now;
    if (!sendHeartbeatReq()) {
      Serial.println("[OTAA] HEARTBEAT_REQ failed — node may be unreachable");
    }
    return;
  }

  // ── Phase 3: Periodic MEASURE_REQ ──
  if ((now - gLastMeasureAt >= MEASURE_INTERVAL_MS) &&
      (now - gLastCommandTxAt >= COMMAND_GAP_MS)) {
    gLastCommandTxAt = now;
    gLastMeasureAt = now;
    Serial.println("[OTAA] Auto MEASURE_REQ (60 s interval)");
    if (!sendMeasureReq()) {
      Serial.println("[OTAA] MEASURE_REQ failed");
    }
    return;
  }
}

// =====================================================
// requestMeasureNow
// Called from loop() on serial 'm' key — manual trigger.
// =====================================================
void requestMeasureNow() {
  if (!gNodePaired) {
    Serial.println("[OTAA] Cannot measure — node not paired yet");
    return;
  }

  if (isGatewayCommandInFlight()) {
    Serial.println("[OTAA] Cannot measure now — another command is in flight");
    return;
  }

  Serial.println("[OTAA] Manual MEASURE_REQ triggered");
  gLastMeasureAt = millis();   // reset auto-timer so we don't double-fire
  gLastCommandTxAt = millis();
  if (!sendMeasureReq()) {
    Serial.println("[OTAA] Manual MEASURE_REQ failed");
  }
}

// =====================================================
// handleActivateOk
// Called by lora_radio.cpp when an ACTIVATE_OK frame arrives.
// JSON: {"evt":"ACTIVATE_OK","state":"active","mac":"..."}
// =====================================================
void handleActivateOk(const char *json, int rssi, float snr) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    Serial.println("[OTAA] ACTIVATE_OK: JSON parse error");
    return;
  }

  String state = doc["state"] | "unknown";
  String mac   = doc["mac"]   | "";

  gNodePaired      = true;
  gNodeActive      = (state == "active");
  gNodeMac         = mac;
  gActivatePending = false;

  lastAcceptedSeq  = 0;

  // Kick off timers from now so we don't immediately fire
  gLastMeasureAt   = millis();
  gLastHeartbeatAt = millis();

  Serial.printf("[OTAA] ACTIVATE_OK — node paired! mac=%s state=%s RSSI=%d SNR=%.1f\n",
                mac.c_str(), state.c_str(), rssi, snr);
  Serial.println("[OTAA] Gateway is now commanding — MEASURE_REQ in 60 s");
}

// =====================================================
// handleHeartbeatAck
// Called by lora_radio.cpp when a HEARTBEAT_ACK frame arrives.
// JSON: {"evt":"HB_ACK","batt":<pct>,"state":"active"|"sleep"}
// =====================================================
void handleHeartbeatAck(const char *json, int rssi, float snr) {
  StaticJsonDocument<64> doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    Serial.println("[OTAA] HB_ACK: JSON parse error");
    return;
  }

  int    batt  = doc["batt"]  | -1;
  String state = doc["state"] | "unknown";
  gNodeActive  = (state == "active");

  Serial.printf("[OTAA] HEARTBEAT_ACK — batt=%d%% state=%s RSSI=%d SNR=%.1f\n",
                batt, state.c_str(), rssi, snr);

  // Si le nœud a redémarré sans que le Gateway le sache, relancer le handshake
  if (state == "inactive") {
    Serial.println("[OTAA] Node inactive — triggering re-ACTIVATE");
    gActivatePending = true;
    gLastActivateAt  = 0;  // force envoi immédiat au prochain tick
  }
}