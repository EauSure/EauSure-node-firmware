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

// GPIO-controlled power switch for the SD module.
// BS250 P-MOSFET high-side switch driven via BC547 NPN.
// HIGH = SD powered, LOW = SD unpowered.
static const int SD_POWER_PIN = 2;

// Separate SPI bus for SD
extern SPIClass sdSPI;

// =====================================================
// SD Card Init
// =====================================================
bool initSD();
bool isSdReady();
bool ensureSdReady();
void markSdNotReady();

// =====================================================
// OTA Functions (optional, for future use)
// =====================================================
void sendFirmwareOTA(const char* path);
