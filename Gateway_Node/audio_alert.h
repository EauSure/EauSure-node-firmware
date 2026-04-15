#pragma once

#include <Arduino.h>
#include <SD.h>
#include "AudioTools.h"
#include "config.h"

using namespace audio_tools;

// =====================================================
// I2S Configuration (MAX98357A)
// =====================================================
static const int I2S_BCLK = 26;
static const int I2S_LRC  = 25;
static const int I2S_DIN  = 27;

// =====================================================
// Alert State
// =====================================================
extern bool alarmRunning;
extern bool audioBusy;
extern String lastPlayedFile;
extern unsigned long lastAlertPlayAt;
static const unsigned long ALERT_COOLDOWN_MS = 15000;
static const unsigned long AUDIO_POWER_SETTLE_MS = 250;
static const unsigned long ALERT_GAP_MS = 400;

// =====================================================
// Multi-alert Queue
// =====================================================
static const int MAX_ALERTS = 10;
extern String alertQueue[MAX_ALERTS];
extern int alertCount;

// =====================================================
// AudioTools Objects
// =====================================================
extern I2SStream i2s;
extern WAVDecoder wavDecoder;
extern EncodedAudioStream decodedStream;

// =====================================================
// Audio Init
// =====================================================
bool initAudio();

// =====================================================
// Alert Queue Management
// =====================================================
void clearAlertQueue();
bool isAlreadyQueued(const char* path);
void queueAlert(const char* path);

// =====================================================
// WAV Playback
// =====================================================
bool playAlertFile(const char* path);
void playQueuedAlerts();

// =====================================================
// Alarm State
// =====================================================
void startAlarm();
void stopAlarm();
