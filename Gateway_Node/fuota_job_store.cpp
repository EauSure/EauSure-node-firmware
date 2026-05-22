#include "fuota_job_store.h"

#include <Preferences.h>

namespace {

Preferences prefs;
constexpr const char* kNamespace = "fuotajob";

} // namespace

namespace FuotaJobStore {

void begin() {
  prefs.begin(kNamespace, false);
}

bool save(const StoredJob& job) {
  if (!job.valid) {
    clear();
    return true;
  }

  prefs.putBool("valid", true);
  prefs.putUChar("target", (uint8_t)job.target);
  prefs.putUChar("state", (uint8_t)job.state);
  prefs.putString("cmdId", job.cmdId);
  prefs.putString("nodeId", job.nodeId);
  prefs.putString("url", job.url);
  prefs.putString("ver", job.version);
  prefs.putString("md5", job.md5);
  prefs.putULong("size", (unsigned long)job.size);
  prefs.putString("path", job.sdPath);
  prefs.putULong("sid", (unsigned long)job.sessionId);
  prefs.putULong("attempts", (unsigned long)job.transferAttempts);
  prefs.putULong("lastAt", (unsigned long)job.lastTransferAttemptMs);
  prefs.putString("error", job.error);
  return true;
}

bool load(StoredJob& job) {
  job = StoredJob{};
  if (!prefs.getBool("valid", false)) {
    return false;
  }

  job.valid = true;
  job.target = (StoredTarget)prefs.getUChar("target", 0);
  job.state = (StoredState)prefs.getUChar("state", 0);
  job.cmdId = prefs.getString("cmdId", "");
  job.nodeId = prefs.getString("nodeId", "");
  job.url = prefs.getString("url", "");
  job.version = prefs.getString("ver", "");
  job.md5 = prefs.getString("md5", "");
  job.size = (size_t)prefs.getULong("size", 0);
  job.sdPath = prefs.getString("path", "");
  job.sessionId = (uint32_t)prefs.getULong("sid", 0);
  job.transferAttempts = (uint32_t)prefs.getULong("attempts", 0);
  job.lastTransferAttemptMs = (uint32_t)prefs.getULong("lastAt", 0);
  job.error = prefs.getString("error", "");
  return true;
}

void clear() {
  prefs.clear();
}

} // namespace FuotaJobStore
