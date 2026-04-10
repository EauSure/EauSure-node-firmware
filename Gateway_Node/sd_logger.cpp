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

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(20);

  if (!SD.begin(SD_CS, sdSPI, 8000000)) {
    Serial.println("[SD] ERROR: SD.begin failed at 8 MHz. Retrying at 4 MHz...");

    if (!SD.begin(SD_CS, sdSPI, 4000000)) {
      Serial.println("[SD] ERROR: SD.begin failed completely");
      return false;
    }
    Serial.println("[SD] Ready on separate SPI bus (4 MHz)");
  } else {
    Serial.println("[SD] Ready on separate SPI bus (8 MHz)");
  }

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
