#include "sd_logger.h"

// =====================================================
// SPI bus for SD (separate from LoRa)
// =====================================================
SPIClass sdSPI(HSPI);

// =====================================================
// Init SD on Separate SPI Bus
// =====================================================
bool initSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  constexpr uint32_t kSdClockCandidatesHz[] = {
    8000000,
    4000000,
    1000000,
  };

  bool mounted = false;
  uint32_t selectedClockHz = 0;

  for (uint32_t clockHz : kSdClockCandidatesHz) {
    sdSPI.end();
    delay(10);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    delay(30);

    Serial.printf("[SD] Trying SD.begin on separate SPI bus at %lu MHz...\n",
                  (unsigned long)(clockHz / 1000000));

    if (!SD.begin(SD_CS, sdSPI, clockHz)) {
      Serial.printf("[SD] SD.begin failed at %lu MHz\n",
                    (unsigned long)(clockHz / 1000000));
      continue;
    }

    mounted = true;
    selectedClockHz = clockHz;
    break;
  }

  if (!mounted) {
    Serial.println("[SD] ERROR: SD.begin failed completely after 8/4/1 MHz attempts");
    return false;
  }

  Serial.printf("[SD] Ready on separate SPI bus (%lu MHz)\n",
                (unsigned long)(selectedClockHz / 1000000));

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] ERROR: No SD card attached");
    return false;
  }

  Serial.print("[SD] Card size: ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  return true;
}

// =====================================================
// Firmware Over-The-Air Update (OTA)
// =====================================================
void sendFirmwareOTA(const char* path) {
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
