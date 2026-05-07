#include "tls_utils.h"

namespace TlsUtils {

bool isPemConfigured(const char* pem) {
  if (!pem) return false;

  String value = String(pem);
  value.trim();
  return value.indexOf("BEGIN CERTIFICATE") >= 0;
}

bool configureClient(WiFiClientSecure& client, const char* pem, const char* label) {
  client.setTimeout(10000);
  client.setHandshakeTimeout(15);

  if (!isPemConfigured(pem)) {
    Serial.printf("[TLS] Missing CA certificate for %s\n", label ? label : "unknown target");
    return false;
  }

  client.setCACert(pem);
  return true;
}

} // namespace TlsUtils
