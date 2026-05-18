#include "audio_alert.h"
#include "sd_logger.h"
#include "otaa_manager.h"
#include "ble_provisioning.h"
#include "wifi_manager.h"

namespace {
void logAudioHeap(const char* stage) {
  Serial.printf("[AUDIO][HEAP] %s free=%u min=%u\n", stage, ESP.getFreeHeap(), ESP.getMinFreeHeap());
}

// I2S DMA buffers (buffer_count * buffer_size), the WAV decoder, the encoded
// stream wrapper and the SD read buffer together eat ~14 KB. Below this floor
// we refuse to start a clip so BLE pairing / WiFi TLS never get starved by
// an audio alert firing at the wrong moment.
constexpr uint32_t kAudioHeapFloorBytes = 26000;

// Force a full teardown of any lingering I2S objects. Called both on normal
// playback end AND on every failure path, so retries never stack leaked
// I2SStream / WAVDecoder / EncodedAudioStream instances.
void teardownAudioPipeline() {
  if (decodedStream != nullptr) {
    decodedStream->end();
    delete decodedStream;
    decodedStream = nullptr;
  }
  if (wavDecoder != nullptr) {
    delete wavDecoder;
    wavDecoder = nullptr;
  }
  if (i2s != nullptr) {
    i2s->end();
    delete i2s;
    i2s = nullptr;
  }
}
}

// Public guard: used by provisioning_mode / normal_mode as a defensive barrier
// before bringing up BLE or WiFi, so a pending alert can never leak DMA into
// a BLE/WiFi-sensitive window.
void forceReleaseAudioResources() {
  if (i2s == nullptr && wavDecoder == nullptr && decodedStream == nullptr) {
    return;
  }
  Serial.println("[AUDIO] Forcing I2S/WAV/decoded stream teardown before BLE/WiFi bring-up");
  teardownAudioPipeline();
  audioBusy = false;
}

// =====================================================
// Audio Global State
// =====================================================
bool alarmRunning = false;
bool audioBusy = false;
String lastPlayedFile = "";
unsigned long lastAlertPlayAt = 0;

// =====================================================
// Alert Queue
// =====================================================
String alertQueue[MAX_ALERTS];
int alertCount = 0;

// =====================================================
// AudioTools Objects
// =====================================================
I2SStream* i2s = nullptr;
WAVDecoder* wavDecoder = nullptr;
EncodedAudioStream* decodedStream = nullptr;

// =====================================================
// Init Audio (I2S + MAX98357A)
// =====================================================
bool initAudio() {
  if (ESP.getFreeHeap() < kAudioHeapFloorBytes) {
    Serial.printf("[AUDIO] Refusing I2S bring-up: free heap %u below floor %u\n",
                  ESP.getFreeHeap(), kAudioHeapFloorBytes);
    return false;
  }

  if (i2s == nullptr) {
    i2s = new I2SStream();
  }
  auto cfg = i2s->defaultConfig(TX_MODE);
  cfg.pin_bck = I2S_BCLK;
  cfg.pin_ws = I2S_LRC;
  cfg.pin_data = I2S_DIN;

  cfg.sample_rate = 16000;
  cfg.bits_per_sample = 16;
  cfg.channels = 1;

  cfg.buffer_count = 8;
  cfg.buffer_size = 1024;

  if (!i2s->begin(cfg)) {
    Serial.println("[AUDIO] ERROR: I2S init failed");
    // Hard reset the I2S pointer so the next attempt does not stack leaks.
    delete i2s;
    i2s = nullptr;
    return false;
  }

  Serial.println("[AUDIO] I2S ready + DMA buffers configured");
  return true;
}

// =====================================================
// Alert Queue Management
// =====================================================
void clearAlertQueue() {
  alertCount = 0;
}

bool isAlreadyQueued(const char* path) {
  for (int i = 0; i < alertCount; i++) {
    if (alertQueue[i] == path) return true;
  }
  return false;
}

void queueAlert(const char* path) {
  if (path == nullptr) return;
  if (alertCount >= MAX_ALERTS) return;
  if (isAlreadyQueued(path)) return;

  alertQueue[alertCount++] = path;
}

// =====================================================
// WAV Playback from SD
// =====================================================
bool playAlertFile(const char* path) {
  if (audioBusy || path == nullptr) return false;

  unsigned long now = millis();
  if (lastPlayedFile == String(path) && (now - lastAlertPlayAt) < ALERT_COOLDOWN_MS) {
    Serial.printf("[AUDIO] Cooldown active for %s\n", path);
    return false;
  }

  // Hard guard: never fire audio while BLE provisioning is accepting writes.
  // I2S DMA + WAV decoder would otherwise share heap with the BLE stack
  // right when the gateway needs to acknowledge an encrypted chunk.
  if (BleProvisioning::isActive()) {
    Serial.printf("[AUDIO] Skipping %s — BLE provisioning active\n", path);
    return false;
  }

  // Pre-flight heap check BEFORE any allocation so we fail fast without
  // touching SD or I2S when memory is already tight.
  if (ESP.getFreeHeap() < kAudioHeapFloorBytes) {
    Serial.printf("[AUDIO] Skipping %s — heap %u below floor %u\n",
                  path, ESP.getFreeHeap(), kAudioHeapFloorBytes);
    return false;
  }

  audioBusy = true;
  logAudioHeap("before-open");

  if (!ensureSdReady()) {
    Serial.printf("[AUDIO] Skipping %s because SD is not mounted\n", path);
    audioBusy = false;
    return false;
  }

  if (!SD.exists(path)) {
    Serial.printf("[AUDIO] SD.exists false for %s — attempting runtime re-mount\n", path);
    markSdNotReady();
    if (!ensureSdReady() || !SD.exists(path)) {
      Serial.printf("[AUDIO] ERROR: SD.exists false for %s after re-mount\n", path);
      audioBusy = false;
      return false;
    }
  }

  File audioFile = SD.open(path, FILE_READ);
  if (!audioFile) {
    Serial.printf("[AUDIO] Open failed for %s — attempting runtime re-mount\n", path);
    markSdNotReady();
    if (ensureSdReady()) {
      audioFile = SD.open(path, FILE_READ);
    }
  }

  if (!audioFile) {
    Serial.printf("[AUDIO] ERROR: file not found: %s\n", path);
    audioBusy = false;
    return false;
  }

  Serial.printf("[AUDIO] Playing: %s (size=%u)\n", path, static_cast<unsigned>(audioFile.size()));
  Serial.printf("[AUDIO] Settling power rail for %lu ms\n", AUDIO_POWER_SETTLE_MS);
  delay(AUDIO_POWER_SETTLE_MS);

  // Initialize I2S dynamically to save heap for WiFi/BLE
  if (!initAudio()) {
    audioFile.close();
    teardownAudioPipeline();
    audioBusy = false;
    return false;
  }

  if (wavDecoder == nullptr) wavDecoder = new WAVDecoder();
  if (decodedStream == nullptr) decodedStream = new EncodedAudioStream(i2s, wavDecoder);

  decodedStream->begin();
  StreamCopy copier(*decodedStream, audioFile, 2048);
  logAudioHeap("after-begin");

  // Playback loop with stall detection + hard time budget.
  //
  // Previously this was `while (audioFile.available()) { copier.copy(); delay(1); }`
  // which could deadlock forever when copier.copy() returned 0 (SD read glitch,
  // decoder underrun). We now bail out if no bytes move for 500 ms, and we
  // cap total playback time at 20 s as a hard safety net so the UI task can
  // never be stuck mid-alert.
  constexpr uint32_t kCopyStallTimeoutMs = 500;
  constexpr uint32_t kPlaybackBudgetMs   = 20000;
  const uint32_t startedAtMs = millis();
  uint32_t lastProgressAtMs = startedAtMs;

  while (audioFile.available()) {
    size_t moved = copier.copy();

    const uint32_t nowMs = millis();
    if (moved > 0) {
      lastProgressAtMs = nowMs;
    } else if (nowMs - lastProgressAtMs >= kCopyStallTimeoutMs) {
      Serial.printf("[AUDIO] Playback stalled — no bytes moved for %lu ms, aborting clip\n",
                    (unsigned long)(nowMs - lastProgressAtMs));
      break;
    }

    if (nowMs - startedAtMs >= kPlaybackBudgetMs) {
      Serial.printf("[AUDIO] Playback budget exceeded (%lu ms), aborting clip\n",
                    (unsigned long)(nowMs - startedAtMs));
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(1));   // feed watchdog + yield to other tasks
  }

  // Wait for the DMA buffers to finish flushing to the speaker before shutting down I2S
  // 8192 bytes of buffer at 16000Hz mono is ~256ms, adding a bit of margin
  delay(400);

  // CRITICAL: full teardown on every exit path so we never leave I2S DMA
  // or WAV decoder heap behind — otherwise BLE/WiFi lose ~14 KB on each
  // retry and the gateway eventually fails to bring up TLS.
  teardownAudioPipeline();

  audioFile.close();
  delay(20);

  lastPlayedFile = String(path);
  lastAlertPlayAt = millis();
  audioBusy = false;

  Serial.println("[AUDIO] Playback finished");
  logAudioHeap("after-playback");
  return true;
}

// =====================================================
// Play All Queued Alerts
// =====================================================
void playQueuedAlerts() {
  if (alertCount == 0) return;

  // Skip audio playback if vocal alerts are disabled for this gateway
  if (!areVocalAlertsEnabled()) {
    Serial.println("[ALERT] Vocal alerts disabled — clearing queue without playback");
    clearAlertQueue();
    return;
  }

  if (!ensureSdReady()) {
    Serial.println("[ALERT] Clearing queued alerts because SD is not mounted");
    clearAlertQueue();
    return;
  }

  Serial.printf("[ALERT] %d alert(s) queued\n", alertCount);

  for (int i = 0; i < alertCount; i++) {
    Serial.printf("[ALERT] Playing %d/%d : %s\n", i + 1, alertCount, alertQueue[i].c_str());
    playAlertFile(alertQueue[i].c_str());
    delay(ALERT_GAP_MS);
  }

  clearAlertQueue();
}

// =====================================================
// Alarm State Management
// =====================================================
void startAlarm() {
  alarmRunning = true;
  Serial.println("\n[ALARM] Alert triggered");
}

void stopAlarm() {
  alarmRunning = false;
  Serial.println("\n[ALARM] Alert stopped");
}
