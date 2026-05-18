#include "otaa_manager.h"
#include "lora_radio.h"
#include "app_state.h"
#include "node_pairing_store.h"
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
  uint32_t gLastReplyRxAt   = 0;
  uint32_t gHeartbeatPendingAt = 0;
  uint32_t gMeasurePendingAt = 0;

  uint32_t gLastCommandTxAt = 0;
  static const uint32_t COMMAND_GAP_MS = 300;
  static const uint32_t REPLY_SETTLE_GAP_MS = 400;

  bool     gActivatePending = true;   // true until first ACTIVATE_OK received
  bool     gHeartbeatPending = false; // true after HEARTBEAT_REQ until HEARTBEAT_ACK or timeout
  bool     gMeasurePending  = false;  // true after MEASURE_REQ until MEASURE_RESP or timeout

  // Gateway-side configuration (populated from SET_CONFIG or backend fetch)
  bool     gNodeActiveConfig   = true;   // true = gateway sends MEASURE_REQ; false = node in standby
  bool     gVocalAlertsEnabled = true;   // true = gateway plays local audio alerts

  // How long to wait between ACTIVATE retries at boot (ms)
  static const uint32_t ACTIVATE_RETRY_MS  = 8000;
  // Measure interval — default 60s, overridable via SET_CONFIG
  static uint32_t MEASURE_INTERVAL_MS   = 60000;
  // Heartbeat interval — auto-aligned to MEASURE_INTERVAL_MS / 2 (min 15s, max 30min)
  static uint32_t HEARTBEAT_INTERVAL_MS = 30000;
  static const uint32_t HEARTBEAT_RESPONSE_TIMEOUT_MS = 8000;
  static const uint32_t MEASURE_RESPONSE_TIMEOUT_MS = 12000;

  // Helper: compute heartbeat interval from measure interval
  static uint32_t computeHeartbeatIntervalMs(uint32_t measureMs) {
    uint32_t hb = measureMs / 2;
    if (hb < 15000UL)    hb = 15000UL;     // min 15s
    if (hb > 1800000UL)  hb = 1800000UL;   // max 30min
    return hb;
  }
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
  gLastReplyRxAt   = 0;
  gHeartbeatPending = false;
  gHeartbeatPendingAt = 0;
  gMeasurePending  = false;
  gMeasurePendingAt = 0;

  // Align heartbeat with current measure interval
  HEARTBEAT_INTERVAL_MS = computeHeartbeatIntervalMs(MEASURE_INTERVAL_MS);

  Serial.printf("[OTAA] Manager ready — measure=%lu ms heartbeat=%lu ms\n",
                (unsigned long)MEASURE_INTERVAL_MS, (unsigned long)HEARTBEAT_INTERVAL_MS);
  Serial.println("[OTAA] Will send ACTIVATE on first tick");
}

// =====================================================
// otaaTick
// Called every iteration of loop().
// Drives the boot handshake and periodic command schedule.
// =====================================================
void otaaTick() {
  const uint32_t now = millis();
  const uint32_t schedulerGapMs =
      (gLastReplyRxAt != 0 && (now - gLastReplyRxAt) < REPLY_SETTLE_GAP_MS)
          ? REPLY_SETTLE_GAP_MS
          : COMMAND_GAP_MS;

  if (isGatewayCommandInFlight()) {
    return;
  }

  // ── Phase 1: ACTIVATE handshake ──
  // Retry every ACTIVATE_RETRY_MS until ACTIVATE_OK received.
  if (gActivatePending) {
    if ((now - gLastActivateAt >= ACTIVATE_RETRY_MS) &&
        (now - gLastCommandTxAt >= schedulerGapMs) &&
        (gLastReplyRxAt == 0 || now - gLastReplyRxAt >= schedulerGapMs)) {
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

  if (gMeasurePending) {
    if (now - gMeasurePendingAt >= MEASURE_RESPONSE_TIMEOUT_MS) {
      Serial.printf("[OTAA] MEASURE_RESP timeout after %lu ms — releasing scheduler\n",
                    (unsigned long)(now - gMeasurePendingAt));
      gMeasurePending = false;
      gMeasurePendingAt = 0;
    } else {
      return;
    }
  }

  if (gHeartbeatPending) {
    if (now - gHeartbeatPendingAt >= HEARTBEAT_RESPONSE_TIMEOUT_MS) {
      Serial.printf("[OTAA] HEARTBEAT_ACK timeout after %lu ms — releasing scheduler\n",
                    (unsigned long)(now - gHeartbeatPendingAt));
      gHeartbeatPending = false;
      gHeartbeatPendingAt = 0;
    } else {
      return;
    }
  }

  // ── Phase 2: Periodic HEARTBEAT ──
  if ((now - gLastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) &&
      (now - gLastCommandTxAt >= schedulerGapMs) &&
      (gLastReplyRxAt == 0 || now - gLastReplyRxAt >= schedulerGapMs)) {
    gLastHeartbeatAt = now;
    gLastCommandTxAt = now;
    if (sendHeartbeatReq()) {
      gHeartbeatPending = true;
      gHeartbeatPendingAt = millis();
      Serial.printf("[OTAA] HEARTBEAT_REQ in progress — scheduler paused at t=%lu ms\n",
                    (unsigned long)gHeartbeatPendingAt);
    } else {
      Serial.println("[OTAA] HEARTBEAT_REQ failed — node may be unreachable");
    }
    return;
  }

  // ── Phase 3: Periodic MEASURE_REQ ──
  if (gNodeActiveConfig && gNodeActive &&  // skip if node deactivated
      (now - gLastMeasureAt >= MEASURE_INTERVAL_MS) &&
      (now - gLastCommandTxAt >= schedulerGapMs) &&
      (gLastReplyRxAt == 0 || now - gLastReplyRxAt >= schedulerGapMs)) {
    gLastCommandTxAt = now;
    gLastMeasureAt = now;
    Serial.println("[OTAA] Auto MEASURE_REQ (60 s interval)");
    if (sendMeasureReq()) {
      notifyMeasureRequestDispatched();
    } else {
      Serial.println("[OTAA] MEASURE_REQ failed");
    }
    return;
  }
}

bool shouldPauseBackgroundWork() {
  if (isGatewayCommandInFlight()) {
    return true;
  }

  return gActivatePending || gHeartbeatPending || gMeasurePending;
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

  if (gHeartbeatPending) {
    Serial.println("[OTAA] Cannot measure now — waiting for current HEARTBEAT_ACK");
    return;
  }

  if (gMeasurePending) {
    Serial.println("[OTAA] Cannot measure now — waiting for current MEASURE_RESP");
    return;
  }

  Serial.println("[OTAA] Manual MEASURE_REQ triggered");
  gLastMeasureAt = millis();   // reset auto-timer so we don't double-fire
  gLastCommandTxAt = millis();
  if (sendMeasureReq()) {
    notifyMeasureRequestDispatched();
  } else {
    Serial.println("[OTAA] Manual MEASURE_REQ failed");
  }
}

void notifyMeasureRequestDispatched() {
  gMeasurePending = true;
  gMeasurePendingAt = millis();
  Serial.printf("[OTAA] MEASURE_REQ in progress — scheduler paused at t=%lu ms\n",
                (unsigned long)gMeasurePendingAt);
}

void setMeasureIntervalMs(uint32_t ms) {
  if (ms < 30000UL)    ms = 30000UL;    // min 30s
  if (ms > 28800000UL) ms = 28800000UL; // max 8h
  MEASURE_INTERVAL_MS = ms;
  HEARTBEAT_INTERVAL_MS = computeHeartbeatIntervalMs(ms);
  Serial.printf("[OTAA] measureInterval updated to %lu ms (heartbeat=%lu ms)\n",
                (unsigned long)ms, (unsigned long)HEARTBEAT_INTERVAL_MS);
}

uint32_t getMeasureIntervalMs() {
  return MEASURE_INTERVAL_MS;
}

void setNodeActiveFlag(bool active) {
  gNodeActiveConfig = active;
  Serial.printf("[OTAA] nodeActive config updated: %d\n", (int)active);
}

void setVocalAlertsEnabled(bool enabled) {
  gVocalAlertsEnabled = enabled;
  Serial.printf("[OTAA] vocalAlerts config updated: %d\n", (int)enabled);
}

bool areVocalAlertsEnabled() {
  return gVocalAlertsEnabled;
}

bool isNodeActiveConfigured() {
  return gNodeActiveConfig;
}

void erasePairingAndEnterPairingMode() {
  Serial.println("[OTAA] Erasing node pairing — gateway will re-enter pairing mode");

  // Erase local AES key and pairing data
  NodePairingStore::clear();
  gRuntimeEncKeyLoaded = false;
  memset(gRuntimeEncKey, 0, sizeof(gRuntimeEncKey));

  // Reset OTAA state
  gNodePaired      = false;
  gNodeActive      = false;
  gNodeMac         = "";
  gActivatePending = true;
  gHeartbeatPending = false;
  gMeasurePending   = false;

  Serial.println("[OTAA] Pairing erased — rebooting into NODE_PAIRING mode");
  delay(500);
  ESP.restart();
}

void notifyMeasureResponseHandled() {
  if (!gMeasurePending) {
    return;
  }

  gMeasurePending = false;
  gLastReplyRxAt = millis();
  Serial.printf("[OTAA] MEASURE_RESP received after %lu ms — scheduler resumed\n",
                (unsigned long)(millis() - gMeasurePendingAt));
  gMeasurePendingAt = 0;
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
  gHeartbeatPending = false;
  gHeartbeatPendingAt = 0;
  gMeasurePending  = false;
  gMeasurePendingAt = 0;

  lastAcceptedSeq  = 0;

  // Schedule first MEASURE_REQ in 10 seconds to quickly push config and send node to sleep
  gLastMeasureAt   = millis() - MEASURE_INTERVAL_MS + 10000;
  gLastHeartbeatAt = millis();
  gLastReplyRxAt   = millis();

  Serial.printf("[OTAA] ACTIVATE_OK — node paired! mac=%s state=%s RSSI=%d SNR=%.1f\n",
                mac.c_str(), state.c_str(), rssi, snr);
  Serial.println("[OTAA] Gateway is now commanding — first MEASURE_REQ in 10 s");
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
  if (gHeartbeatPending) {
    gHeartbeatPending = false;
    gLastReplyRxAt = millis();
    Serial.printf("[OTAA] HEARTBEAT_ACK received after %lu ms — scheduler resumed\n",
                  (unsigned long)(millis() - gHeartbeatPendingAt));
    gHeartbeatPendingAt = 0;
  }

  Serial.printf("[OTAA] HEARTBEAT_ACK — batt=%d%% state=%s RSSI=%d SNR=%.1f\n",
                batt, state.c_str(), rssi, snr);

  // Si le nœud a redémarré sans que le Gateway le sache, relancer le handshake
  if (state == "inactive") {
    Serial.println("[OTAA] Node inactive — triggering re-ACTIVATE");
    gActivatePending = true;
    gLastActivateAt  = 0;  // force envoi immédiat au prochain tick
    gHeartbeatPending = false;
    gHeartbeatPendingAt = 0;
    gMeasurePending  = false;
    gMeasurePendingAt = 0;
  }
}
