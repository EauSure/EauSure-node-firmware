#include "firmware_version.h"

#include <cstring>
#include <esp_ota_ops.h>

namespace {

bool isGenericEspVersion(const char* version) {
  if (version == nullptr || version[0] == '\0') {
    return true;
  }
  return strcmp(version, "1.0.0") == 0 || strcmp(version, "1.0") == 0;
}

} // namespace

const char* getNodeFirmwareVersion() {
  const esp_app_desc_t* desc = esp_ota_get_app_description();
  if (desc != nullptr && !isGenericEspVersion(desc->version)) {
    return desc->version;
  }

  return "dev";
}
