#include "fuota_manager.h"

#include <SD.h>
#include <esp_system.h>
#include <ArduinoJson.h>

#include "audio_alert.h"
#include "fuota_job_store.h"
#include "lora_radio.h"
#include "mqtt_gateway.h"
#include "node_pairing_store.h"
#include "sd_logger.h"
#include "telemetry.h"
#include "wifi_manager.h"

namespace {

constexpr const char* kGatewayFwPath = "/fw/gateway_update.bin";
constexpr const char* kNodeFwPath    = "/fw/node_update.bin";
constexpr uint16_t    kFuotaChunkSize = 160;
constexpr uint32_t    kTransferRetryMinGapMs = 60000;
constexpr uint32_t    kMaxTransferAttempts = 40;

enum class UpdateTarget {
  None,
  Gateway,
  Node,
};

enum class UpdateState {
  Idle,
  DownloadPending,
  Downloaded,
  Failed,
};

struct PendingUpdate {
  UpdateTarget target = UpdateTarget::None;
  UpdateState state = UpdateState::Idle;
  String cmdId;
  String nodeId;
  String url;
  String version;
  String md5;
  size_t size = 0;
  String sdPath;
  uint32_t sessionId = 0;
  uint32_t transferAttempts = 0;
  uint32_t lastTransferAttemptMs = 0;
  String error;
};

PendingUpdate gPendingUpdate;
bool gBusy = false;
bool gCommandAcked = false;
bool gCommandFailed = false;

void publishFuotaPhase(const char* phase, const char* message = "") {
  StaticJsonDocument<384> doc;
  doc["phase"] = phase;
  doc["target"] = (gPendingUpdate.target == UpdateTarget::Gateway) ? "gateway" : "node";
  if (!gPendingUpdate.nodeId.isEmpty()) {
    doc["nodeId"] = gPendingUpdate.nodeId;
  }
  if (!gPendingUpdate.version.isEmpty()) {
    doc["version"] = gPendingUpdate.version;
  }
  if (!gPendingUpdate.cmdId.isEmpty()) {
    doc["cmdId"] = gPendingUpdate.cmdId;
  }
  if (message && message[0] != '\0') {
    doc["message"] = message;
  }
  doc["attempt"] = gPendingUpdate.transferAttempts;

  String payload;
  serializeJson(doc, payload);
  MqttGateway::publishEvent("fuota_progress", payload);
}

void persistJob() {
  if (gPendingUpdate.target == UpdateTarget::None) {
    FuotaJobStore::clear();
    return;
  }

  FuotaJobStore::StoredJob job;
  job.valid = true;
  job.target = (gPendingUpdate.target == UpdateTarget::Gateway)
                   ? FuotaJobStore::StoredTarget::Gateway
                   : FuotaJobStore::StoredTarget::Node;
  switch (gPendingUpdate.state) {
    case UpdateState::DownloadPending:
      job.state = FuotaJobStore::StoredState::DownloadPending;
      break;
    case UpdateState::Downloaded:
      job.state = FuotaJobStore::StoredState::Downloaded;
      break;
    case UpdateState::Failed:
      job.state = FuotaJobStore::StoredState::Failed;
      break;
    default:
      job.state = FuotaJobStore::StoredState::Idle;
      break;
  }
  job.cmdId = gPendingUpdate.cmdId;
  job.nodeId = gPendingUpdate.nodeId;
  job.url = gPendingUpdate.url;
  job.version = gPendingUpdate.version;
  job.md5 = gPendingUpdate.md5;
  job.size = gPendingUpdate.size;
  job.sdPath = gPendingUpdate.sdPath;
  job.sessionId = gPendingUpdate.sessionId;
  job.transferAttempts = gPendingUpdate.transferAttempts;
  job.lastTransferAttemptMs = gPendingUpdate.lastTransferAttemptMs;
  job.error = gPendingUpdate.error;
  FuotaJobStore::save(job);
}

bool ensureFirmwareDirectory() {
  if (!ensureSdReady()) {
    return false;
  }

  if (SD.exists("/fw")) {
    return true;
  }

  return SD.mkdir("/fw");
}

void clearPendingUpdate() {
  gPendingUpdate = PendingUpdate{};
  gBusy = false;
  FuotaJobStore::clear();
}

void markFailed(const String& reason) {
  gPendingUpdate.state = UpdateState::Failed;
  gPendingUpdate.error = reason;
  gCommandFailed = true;
  persistJob();
  publishFuotaPhase("failed", reason.c_str());
  Serial.printf("[FUOTA] Job failed: %s\n", reason.c_str());
}

bool restoreFromStore() {
  FuotaJobStore::StoredJob job;
  if (!FuotaJobStore::load(job)) {
    return false;
  }

  gPendingUpdate = PendingUpdate{};
  gPendingUpdate.target = (job.target == FuotaJobStore::StoredTarget::Gateway)
                              ? UpdateTarget::Gateway
                              : UpdateTarget::Node;
  switch (job.state) {
    case FuotaJobStore::StoredState::DownloadPending:
      gPendingUpdate.state = UpdateState::DownloadPending;
      break;
    case FuotaJobStore::StoredState::Downloaded:
      gPendingUpdate.state = UpdateState::Downloaded;
      break;
    case FuotaJobStore::StoredState::Failed:
      gPendingUpdate.state = UpdateState::Failed;
      gCommandFailed = true;
      break;
    default:
      return false;
  }

  gPendingUpdate.cmdId = job.cmdId;
  gPendingUpdate.nodeId = job.nodeId;
  gPendingUpdate.url = job.url;
  gPendingUpdate.version = job.version;
  gPendingUpdate.md5 = job.md5;
  gPendingUpdate.size = job.size;
  gPendingUpdate.sdPath = job.sdPath;
  gPendingUpdate.sessionId = job.sessionId;
  gPendingUpdate.transferAttempts = job.transferAttempts;
  gPendingUpdate.lastTransferAttemptMs = job.lastTransferAttemptMs;
  gPendingUpdate.error = job.error;

  Serial.printf("[FUOTA] Restored job target=%d state=%d version=%s attempts=%lu\n",
                (int)gPendingUpdate.target,
                (int)gPendingUpdate.state,
                gPendingUpdate.version.c_str(),
                (unsigned long)gPendingUpdate.transferAttempts);
  return true;
}

bool downloadPendingUpdate() {
  if (gPendingUpdate.state != UpdateState::DownloadPending) {
    return false;
  }

  publishFuotaPhase("downloading");

  if (!ensureFirmwareDirectory()) {
    markFailed("SD not ready");
    return false;
  }

  String error;
  if (!WiFiManager::downloadFileToSd(gPendingUpdate.url, gPendingUpdate.sdPath.c_str(),
                                     gPendingUpdate.size, &error)) {
    markFailed(error.isEmpty() ? "Download failed" : error);
    return false;
  }

  gPendingUpdate.state = UpdateState::Downloaded;
  gPendingUpdate.error = "";
  persistJob();
  // Acknowledge the MQTT command once the image is on SD (LoRa transfer may still be pending for nodes).
  if (!gPendingUpdate.cmdId.isEmpty()) {
    gCommandAcked = true;
  }
  publishFuotaPhase("downloaded", "Firmware on SD — waiting for apply window");
  Serial.printf("[FUOTA] Download complete for target=%d version=%s path=%s\n",
                (int)gPendingUpdate.target,
                gPendingUpdate.version.c_str(),
                gPendingUpdate.sdPath.c_str());
  return true;
}

bool applyGatewayUpdateNow() {
  String error;
  gBusy = true;
  stopAlarm();
  publishFuotaPhase("applying", "Gateway OTA apply");
  MqttGateway::setExclusiveTlsWindow(true);
  const bool ok = applyFirmwareFromSd(gPendingUpdate.sdPath.c_str(), gPendingUpdate.md5.c_str(), &error);
  MqttGateway::setExclusiveTlsWindow(false);
  gBusy = false;

  if (!ok) {
    markFailed(error.isEmpty() ? "Gateway OTA failed" : error);
    return false;
  }

  publishFuotaPhase("succeeded", "Gateway rebooting with new firmware");
  gCommandAcked = true;
  clearPendingUpdate();
  return true;
}

bool canAttemptNodeTransferNow() {
  if (gPendingUpdate.transferAttempts >= kMaxTransferAttempts) {
    return false;
  }

  const uint32_t now = millis();
  if (gPendingUpdate.lastTransferAttemptMs != 0 &&
      (now - gPendingUpdate.lastTransferAttemptMs) < kTransferRetryMinGapMs) {
    return false;
  }

  return true;
}

void recordTransferAttempt() {
  ++gPendingUpdate.transferAttempts;
  gPendingUpdate.lastTransferAttemptMs = millis();
  persistJob();
}

} // namespace

namespace FuotaManager {

void begin() {
  FuotaJobStore::begin();
  clearPendingUpdate();
  gCommandAcked = false;
  gCommandFailed = false;

  if (restoreFromStore()) {
    if (gPendingUpdate.state == UpdateState::Failed) {
      Serial.println("[FUOTA] Restored failed job — awaiting API fail/ack cleanup");
    } else {
      publishFuotaPhase("restored", "Resuming persisted FUOTA job after reboot");
    }
  }
}

void loop() {
  if (gPendingUpdate.state == UpdateState::DownloadPending) {
    downloadPendingUpdate();
    return;
  }

  if (gPendingUpdate.target == UpdateTarget::Gateway &&
      gPendingUpdate.state == UpdateState::Downloaded &&
      !telemetryHasPendingUpload() &&
      !isGatewayCommandInFlight()) {
    applyGatewayUpdateNow();
  }
}

bool hasPendingNodeUpdate() {
  return gPendingUpdate.target == UpdateTarget::Node &&
         gPendingUpdate.state == UpdateState::Downloaded;
}

bool handleNodeDataWindow() {
  if (!hasPendingNodeUpdate()) {
    return false;
  }

  if (!canAttemptNodeTransferNow()) {
    if (gPendingUpdate.transferAttempts >= kMaxTransferAttempts) {
      markFailed("Max LoRa transfer attempts exceeded");
    }
    return false;
  }

  NodePairingData pairing = NodePairingStore::load();
  if (!pairing.valid || pairing.nodeId != gPendingUpdate.nodeId) {
    markFailed("Node pairing mismatch for pending firmware update");
    return false;
  }

  if (!ensureSdReady()) {
    Serial.println("[FUOTA] Node update pending but SD is not ready");
    return false;
  }

  File updateFile = SD.open(gPendingUpdate.sdPath.c_str(), FILE_READ);
  if (!updateFile) {
    markFailed("Cannot open node firmware on SD");
    return false;
  }

  const size_t totalSize = updateFile.size();
  const uint16_t totalChunks = (uint16_t)((totalSize + kFuotaChunkSize - 1) / kFuotaChunkSize);
  if (totalChunks == 0) {
    updateFile.close();
    markFailed("Empty node firmware");
    return false;
  }

  delay(400);
  Serial.println("[FUOTA] Pre-transfer settle — node should have processed DATA ACK");

  recordTransferAttempt();
  gBusy = true;
  publishFuotaPhase("transferring", "LoRa FUOTA session started");

  Serial.printf("[FUOTA] Starting node transfer sid=%lu size=%u chunks=%u attempt=%lu\n",
                (unsigned long)gPendingUpdate.sessionId,
                (unsigned)totalSize,
                (unsigned)totalChunks,
                (unsigned long)gPendingUpdate.transferAttempts);

  if (!sendFuotaBegin(gPendingUpdate.sessionId,
                      (uint32_t)totalSize,
                      kFuotaChunkSize,
                      totalChunks,
                      gPendingUpdate.version,
                      gPendingUpdate.md5)) {
    updateFile.close();
    gBusy = false;
    Serial.println("[FUOTA] FUOTA_BEGIN failed — job kept for next wake window");
    publishFuotaPhase("transfer_retry", "FUOTA_BEGIN failed — will retry");
    return false;
  }

  uint8_t buffer[kFuotaChunkSize];
  uint32_t chunkIndex = 0;
  while (updateFile.available()) {
    const size_t len = updateFile.read(buffer, sizeof(buffer));
    if (len == 0) {
      updateFile.close();
      gBusy = false;
      publishFuotaPhase("transfer_retry", "SD read error during transfer");
      Serial.println("[FUOTA] SD read returned 0 bytes during node transfer");
      return false;
    }

    if (!sendFuotaChunk(gPendingUpdate.sessionId, chunkIndex, buffer, (uint16_t)len)) {
      updateFile.close();
      gBusy = false;
      Serial.printf("[FUOTA] FUOTA_CHUNK failed at index=%lu — job kept\n",
                    (unsigned long)chunkIndex);
      publishFuotaPhase("transfer_retry", "FUOTA_CHUNK failed — will retry");
      return false;
    }

    ++chunkIndex;
    delay(25);
  }
  updateFile.close();

  if (!sendFuotaEnd(gPendingUpdate.sessionId, (uint32_t)totalSize, totalChunks)) {
    gBusy = false;
    publishFuotaPhase("transfer_retry", "FUOTA_END failed — will retry");
    Serial.println("[FUOTA] FUOTA_END failed");
    return false;
  }

  delay(80);
  if (!sendFuotaCommit(gPendingUpdate.sessionId)) {
    gBusy = false;
    publishFuotaPhase("transfer_retry", "FUOTA_COMMIT failed — will retry");
    Serial.println("[FUOTA] FUOTA_COMMIT failed");
    return false;
  }

  Serial.println("[FUOTA] Node update session completed and commit sent");
  publishFuotaPhase("succeeded", "Node FUOTA commit acknowledged over LoRa");
  gCommandAcked = true;
  clearPendingUpdate();
  gBusy = false;
  return true;
}

void queueGatewayUpdate(const String& url, const String& version, const String& md5, size_t size,
                        const String& cmdId) {
  gPendingUpdate = PendingUpdate{};
  gPendingUpdate.target = UpdateTarget::Gateway;
  gPendingUpdate.state = UpdateState::DownloadPending;
  gPendingUpdate.cmdId = cmdId;
  gPendingUpdate.url = url;
  gPendingUpdate.version = version;
  gPendingUpdate.md5 = md5;
  gPendingUpdate.size = size;
  gPendingUpdate.sdPath = kGatewayFwPath;
  gPendingUpdate.sessionId = (uint32_t)esp_random();
  gPendingUpdate.error = "";
  gCommandAcked = false;
  gCommandFailed = false;
  persistJob();
  publishFuotaPhase("queued", "Gateway firmware download queued");
  Serial.printf("[FUOTA] Queued gateway OTA version=%s size=%u cmdId=%s\n",
                version.c_str(),
                (unsigned)size,
                cmdId.c_str());
}

void queueNodeUpdate(const String& nodeId, const String& url, const String& version, const String& md5,
                     size_t size, const String& cmdId) {
  String normalizedNodeId = nodeId;
  normalizedNodeId.trim();
  normalizedNodeId.toUpperCase();

  gPendingUpdate = PendingUpdate{};
  gPendingUpdate.target = UpdateTarget::Node;
  gPendingUpdate.state = UpdateState::DownloadPending;
  gPendingUpdate.cmdId = cmdId;
  gPendingUpdate.nodeId = normalizedNodeId;
  gPendingUpdate.url = url;
  gPendingUpdate.version = version;
  gPendingUpdate.md5 = md5;
  gPendingUpdate.size = size;
  gPendingUpdate.sdPath = kNodeFwPath;
  gPendingUpdate.sessionId = (uint32_t)esp_random();
  gPendingUpdate.error = "";
  gCommandAcked = false;
  gCommandFailed = false;
  persistJob();
  publishFuotaPhase("queued", "Node firmware download queued");
  Serial.printf("[FUOTA] Queued node OTA node=%s version=%s size=%u cmdId=%s\n",
                normalizedNodeId.c_str(),
                version.c_str(),
                (unsigned)size,
                cmdId.c_str());
}

bool isBusy() {
  return gBusy || gPendingUpdate.state == UpdateState::DownloadPending;
}

bool isGatewayUpdateInProgress() {
  return gPendingUpdate.target == UpdateTarget::Gateway &&
         gPendingUpdate.state != UpdateState::Idle &&
         gPendingUpdate.state != UpdateState::Failed;
}

String getBoundCommandId() {
  return gPendingUpdate.cmdId;
}

bool commandReadyToAck() {
  if (gPendingUpdate.cmdId.isEmpty()) {
    return false;
  }
  if (gCommandAcked) {
    return true;
  }
  return gPendingUpdate.state == UpdateState::Downloaded;
}

bool commandShouldFail(String& reason) {
  if (gPendingUpdate.cmdId.isEmpty()) {
    return false;
  }
  if (gCommandFailed || gPendingUpdate.state == UpdateState::Failed) {
    reason = gPendingUpdate.error.isEmpty() ? "FUOTA job failed" : gPendingUpdate.error;
    return true;
  }
  return false;
}

void clearCommandBinding() {
  gCommandAcked = false;
  gCommandFailed = false;
}

void dismissFailedJob() {
  if (gPendingUpdate.state == UpdateState::Failed) {
    clearPendingUpdate();
  }
  clearCommandBinding();
}

} // namespace FuotaManager
