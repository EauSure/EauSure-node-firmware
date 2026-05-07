#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

namespace TlsUtils {

bool isPemConfigured(const char* pem);
bool configureClient(WiFiClientSecure& client, const char* pem, const char* label);

} // namespace TlsUtils
