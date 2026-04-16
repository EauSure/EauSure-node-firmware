#include "audio_alert.h"
#include "sd_logger.h"

namespace {
void logAudioHeap(const char* stage) {
  Serial.printf("[AUDIO][HEAP] %s free=%u min=%u\n", stage, ESP.getFreeHeap(), ESP.getMinFreeHeap());
}
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
I2SStream i2s;
WAVDecoder wavDecoder;
EncodedAudioStream decodedStream(&i2s, &wavDecoder);

// =====================================================
// Init Audio (I2S + MAX98357A)
// =====================================================
bool initAudio() {
  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.pin_bck = I2S_BCLK;
  cfg.pin_ws = I2S_LRC;
  cfg.pin_data = I2S_DIN;

  cfg.sample_rate = 16000;
  cfg.bits_per_sample = 16;
  cfg.channels = 1;

  cfg.buffer_count = 8;
  cfg.buffer_size = 1024;

  if (!i2s.begin(cfg)) {
    Serial.println("[AUDIO] ERROR: I2S init failed");
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

  audioBusy = true;
  logAudioHeap("before-open");

  if (!isSdReady()) {
    Serial.printf("[AUDIO] Skipping %s because SD is not mounted\n", path);
    audioBusy = false;
    return false;
  }

  if (!SD.exists(path)) {
    Serial.printf("[AUDIO] ERROR: SD.exists false for %s\n", path);
    audioBusy = false;
    return false;
  }

  File audioFile = SD.open(path, FILE_READ);
  if (!audioFile) {
    Serial.printf("[AUDIO] ERROR: file not found: %s\n", path);
    audioBusy = false;
    return false;
  }

  Serial.printf("[AUDIO] Playing: %s (size=%u)\n", path, static_cast<unsigned>(audioFile.size()));
  Serial.printf("[AUDIO] Settling power rail for %lu ms\n", AUDIO_POWER_SETTLE_MS);
  delay(AUDIO_POWER_SETTLE_MS);

  decodedStream.begin();
  StreamCopy copier(decodedStream, audioFile, 2048);
  logAudioHeap("after-begin");

  while (audioFile.available()) {
    copier.copy();
    delay(1);
  }

  decodedStream.end();
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

  if (!isSdReady()) {
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
