#include "sd_logger.h"
#include <Update.h>

// =====================================================
// SPI bus for SD (separate from LoRa)
// =====================================================
SPIClass sdSPI(HSPI);

namespace {
bool gSdReady = false;
bool gInitialBootPassComplete = false;
bool gSdPowerOn = false;
uint32_t gLastSdEnsureAttemptAt = 0;

constexpr uint32_t kSdClockCandidatesHz[] = {
  2000000,
  1000000,
};

// Cold-boot: MicroSD spec requires >= 1 ms + 74 SCK cycles with CS high
// before the first command. On top of that, ESP32 devkits powered over USB
// need ~500 ms for the LDO to stabilise once LoRa + MAX98357A settle.
constexpr uint32_t kSdStartupSettleMs       = 800;
constexpr uint32_t kSdBetweenAttemptsMs     = 40;
constexpr uint32_t kSdRetryPassDelayMs      = 500;
constexpr uint32_t kSdFinalRetryPassDelayMs = 1200;
constexpr uint32_t kSdBusResetDelayMs       = 15;
constexpr uint32_t kSdEnsureRetryIntervalMs = 4000;
constexpr uint32_t kSdWarmupClockHz         = 400000;
constexpr uint8_t  kSdWarmupBytes           = 10;   // 10 * 8 = 80 clocks, >= 74

// Power-cycle timing
constexpr uint32_t kSdPowerOffMs            = 700;  // hold power off long enough for card/module caps to fully drain
constexpr uint32_t kSdPowerOnSettleMs       = 900;  // let card regulator + level shifter stabilise after power-on
constexpr uint32_t kSdLineDischargeMs       = 80;   // keep SPI lines quiet/low to avoid phantom powering the card

void floatSdBusPins() {
  pinMode(SD_CS, INPUT_PULLUP);
  pinMode(SD_SCK, INPUT);
  pinMode(SD_MOSI, INPUT);
  pinMode(SD_MISO, INPUT);
}

void driveSdBusIdleLow() {
  pinMode(SD_CS, OUTPUT);
  pinMode(SD_SCK, OUTPUT);
  pinMode(SD_MOSI, OUTPUT);
  digitalWrite(SD_CS, LOW);
  digitalWrite(SD_SCK, LOW);
  digitalWrite(SD_MOSI, LOW);
}

// Turn SD module power ON via GPIO → BC547 → BS250
void sdPowerOn() {
  if (gSdPowerOn) return;
  pinMode(SD_POWER_PIN, OUTPUT);
  digitalWrite(SD_POWER_PIN, HIGH);
  gSdPowerOn = true;
  Serial.printf("[SD] Power ON (GPIO %d HIGH) — settling %lu ms\n",
                SD_POWER_PIN, (unsigned long)kSdPowerOnSettleMs);
  delay(kSdPowerOnSettleMs);
}

// Turn SD module power OFF
void sdPowerOff() {
  pinMode(SD_POWER_PIN, OUTPUT);
  digitalWrite(SD_POWER_PIN, LOW);
  gSdPowerOn = false;
  driveSdBusIdleLow();
  Serial.printf("[SD] Power OFF (GPIO %d LOW) — discharging SPI lines for %lu ms\n",
                SD_POWER_PIN,
                (unsigned long)kSdLineDischargeMs);
  delay(kSdLineDischargeMs);
  floatSdBusPins();
}

// Full power cycle: OFF → wait → ON → wait
void sdPowerCycle() {
  Serial.println("[SD] Power-cycling SD module");
  SD.end();
  sdSPI.end();
  sdPowerOff();
  delay(kSdPowerOffMs);
  sdPowerOn();
}

// Emit >= 74 dummy SCK cycles with CS held HIGH. Many MicroSD cards will not
// accept CMD0 on the first cold boot without this sequence, which is exactly
// the "first power-on fails, reset works" symptom observed on this gateway.
void emitSdWarmupClocks() {
  digitalWrite(SD_CS, HIGH);
  sdSPI.beginTransaction(SPISettings(kSdWarmupClockHz, MSBFIRST, SPI_MODE0));
  for (uint8_t i = 0; i < kSdWarmupBytes; ++i) {
    sdSPI.transfer(0xFF);
  }
  sdSPI.endTransaction();
}

bool tryMountSdPass(uint32_t& selectedClockHz) {
  for (uint32_t clockHz : kSdClockCandidatesHz) {
    sdSPI.end();
    delay(kSdBusResetDelayMs);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    delay(kSdBetweenAttemptsMs);

    emitSdWarmupClocks();

    Serial.printf("[SD] Trying SD.begin on separate SPI bus at %lu kHz (free heap=%u)...\n",
                  (unsigned long)(clockHz / 1000),
                  ESP.getFreeHeap());

    if (!SD.begin(SD_CS, sdSPI, clockHz)) {
      Serial.printf("[SD] SD.begin failed at %lu kHz\n",
                    (unsigned long)(clockHz / 1000));
      continue;
    }

    // Validate the mount is really usable. Some cheap cards on cold boot
    // return OK from SD.begin() but fail CSD reads at higher clocks,
    // producing cardSize == 0. SD.exists() then fails and every subsequent
    // file operation returns "not found". Detect that broken-mount state
    // here and fall through to a slower clock.
    //
    // Note: cardType may legitimately report CARD_UNKNOWN (4) on some cheap
    // SDHC cards even when the mount is fully usable. Trust cardSize as the
    // authoritative signal instead of cardType.
    uint64_t cardBytes = SD.cardSize();

    if (cardBytes == 0) {
      Serial.printf("[SD] Mount reported OK but size=0 at %lu kHz — rejecting and trying slower\n",
                    (unsigned long)(clockHz / 1000));
      SD.end();
      delay(kSdBusResetDelayMs);
      continue;
    }

    selectedClockHz = clockHz;
    return true;
  }

  return false;
}
}

// =====================================================
// Init SD on Separate SPI Bus
// =====================================================
bool initSD() {
  gSdReady = false;
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // Ensure the SD module is powered on. On first boot this transitions
  // from the default-off state (GPIO2 low at reset → BS250 off).
  sdPowerOn();

  const uint32_t settleMs = gInitialBootPassComplete ? 0 : kSdStartupSettleMs;
  if (settleMs > 0) {
    Serial.printf("[SD] Cold-boot settle for %lu ms before init (first attempt only)\n",
                  (unsigned long)settleMs);
    delay(settleMs);
  }

  uint32_t selectedClockHz = 0;
  bool mounted = tryMountSdPass(selectedClockHz);

  if (!mounted) {
    Serial.printf("[SD] First init pass failed — waiting %lu ms before second full retry\n",
                  (unsigned long)kSdRetryPassDelayMs);
    delay(kSdRetryPassDelayMs);
    mounted = tryMountSdPass(selectedClockHz);
  }

  if (!mounted) {
    // Two software-only passes failed. Power-cycle the card to reset its
    // internal state machine (equivalent to physically removing/reinserting).
    sdPowerCycle();
    Serial.printf("[SD] Post-power-cycle settle for %lu ms before final retry\n",
                  (unsigned long)kSdFinalRetryPassDelayMs);
    delay(kSdFinalRetryPassDelayMs);
    mounted = tryMountSdPass(selectedClockHz);
  }

  gInitialBootPassComplete = true;

  if (!mounted) {
    Serial.println("[SD] ERROR: SD.begin failed after two passes + power-cycle retry");
    Serial.println("[SD] Will re-try on demand once subsystems are stable");
    return false;
  }

  Serial.printf("[SD] Ready on separate SPI bus (%lu kHz)\n",
                (unsigned long)(selectedClockHz / 1000));

  uint64_t cardBytes = SD.cardSize();
  if (cardBytes == 0) {
    Serial.println("[SD] ERROR: Card reports 0 MB — broken mount, rejecting");
    SD.end();
    return false;
  }

  Serial.print("[SD] Card size: ");
  Serial.print(cardBytes / (1024 * 1024));
  Serial.println(" MB");

  gSdReady = true;
  return true;
}

bool isSdReady() {
  return gSdReady;
}

void markSdNotReady() {
  if (!gSdReady) return;

  gSdReady = false;
  SD.end();
  sdSPI.end();
  sdPowerOff();
  Serial.println("[SD] Marked not ready — powered off, runtime re-mount required");
}

bool ensureSdReady() {
  if (gSdReady) {
    return true;
  }

  const uint32_t now = millis();
  if (gLastSdEnsureAttemptAt != 0 &&
      (now - gLastSdEnsureAttemptAt) < kSdEnsureRetryIntervalMs) {
    return false;
  }

  gLastSdEnsureAttemptAt = now;

  // Power-cycle the module before runtime re-mount. This resets the card's
  // internal state machine — equivalent to physically removing/reinserting
  // the card, which is the only reliable recovery for these cheap level-
  // shifter modules when the card gets stuck.
  sdPowerCycle();
  Serial.println("[SD] Runtime re-mount requested (after power cycle)");
  return initSD();
}

// =====================================================
// Firmware Over-The-Air Update (OTA)
// =====================================================
void sendFirmwareOTA(const char* path) {
  if (!ensureSdReady()) {
    Serial.println("[FUOTA] Error: SD not ready");
    return;
  }

  File updateFile = SD.open(path, FILE_READ);
  if (!updateFile) {
    Serial.println("[FUOTA] Error: Cannot open .bin file");
    return;
  }

  size_t totalSize = updateFile.size();
  uint16_t totalPackets = (totalSize + 127) / 128;

  Serial.printf("[FUOTA] Start: %s (%d bytes, %d packets)\n", path, (int)totalSize, totalPackets);

  // Note: This requires LoRa to be included, consider moving to lora_radio.cpp
  // or creating a separate fuota module if needed
  
  updateFile.close();
  Serial.println("[FUOTA] Transfer complete.");
}

bool applyFirmwareFromSd(const char* path, const char* expectedMd5, String* errorOut) {
  if (errorOut) *errorOut = "";

  if (!ensureSdReady()) {
    if (errorOut) *errorOut = "SD not ready";
    return false;
  }

  File updateFile = SD.open(path, FILE_READ);
  if (!updateFile) {
    if (errorOut) *errorOut = "Cannot open firmware file";
    return false;
  }

  const size_t totalSize = updateFile.size();
  if (totalSize == 0) {
    updateFile.close();
    if (errorOut) *errorOut = "Firmware file is empty";
    return false;
  }

  Serial.printf("[OTA] Applying firmware from SD: %s (%u bytes)\n", path, (unsigned)totalSize);

  if (!Update.begin(totalSize, U_FLASH)) {
    updateFile.close();
    if (errorOut) *errorOut = Update.errorString();
    return false;
  }

  if (expectedMd5 && strlen(expectedMd5) > 0) {
    Update.setMD5(expectedMd5);
  }

  uint8_t buffer[512];
  size_t totalWritten = 0;
  while (updateFile.available()) {
    const size_t len = updateFile.read(buffer, sizeof(buffer));
    if (len == 0) {
      continue;
    }

    const size_t written = Update.write(buffer, len);
    if (written != len) {
      updateFile.close();
      Update.abort();
      if (errorOut) *errorOut = Update.errorString();
      return false;
    }
    totalWritten += written;
  }

  updateFile.close();

  if (totalWritten != totalSize) {
    Update.abort();
    if (errorOut) *errorOut = "Written size mismatch";
    return false;
  }

  if (!Update.end()) {
    if (errorOut) *errorOut = Update.errorString();
    return false;
  }

  if (!Update.isFinished()) {
    if (errorOut) *errorOut = "Update not finished";
    return false;
  }

  Serial.println("[OTA] Firmware applied successfully. Rebooting...");
  delay(300);
  ESP.restart();
  return true;
}
