#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "config.h"

// =====================================================
// SD Card Configuration
// =====================================================
static const int SD_CS   = 4;
static const int SD_SCK  = 14;
static const int SD_MISO = 21;
static const int SD_MOSI = 22;

// Separate SPI bus for SD
extern SPIClass sdSPI;

// =====================================================
// SD Card Init
// =====================================================
bool initSD();

// =====================================================
// OTA Functions (optional, for future use)
// =====================================================
void sendFirmwareOTA(const char* path);
