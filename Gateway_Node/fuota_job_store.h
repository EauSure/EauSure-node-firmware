#pragma once

#include <Arduino.h>

// Persists an in-flight FUOTA job across reboots (NVS).
namespace FuotaJobStore {

enum class StoredTarget : uint8_t {
  None = 0,
  Gateway = 1,
  Node = 2,
};

enum class StoredState : uint8_t {
  Idle = 0,
  DownloadPending = 1,
  Downloaded = 2,
  Failed = 3,
};

struct StoredJob {
  bool valid = false;
  StoredTarget target = StoredTarget::None;
  StoredState state = StoredState::Idle;
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

void begin();
bool save(const StoredJob& job);
bool load(StoredJob& job);
void clear();

} // namespace FuotaJobStore
