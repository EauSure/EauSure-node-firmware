#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

#ifndef API_TLS_ROOT_CA
#define API_TLS_ROOT_CA ""
#endif

#ifndef MQTT_TLS_ROOT_CA
#define MQTT_TLS_ROOT_CA ""
#endif

namespace TlsUtils {

bool isPemConfigured(const char* pem);
bool configureClient(WiFiClientSecure& client, const char* pem, const char* label);

} // namespace TlsUtils
