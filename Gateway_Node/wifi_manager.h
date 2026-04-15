#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "config.h"

namespace WiFiManager {
    // Initialize WiFi connection with runtime credentials
    bool init(const char* ssid, const char* password);

    // Retry using the last runtime credentials
    bool reconnect();

    // Check if connected
    bool isConnected();

    // Submit sensor data to API
    bool submitSensorData(
        const char* nodeId,
        const char* gatewayHardwareId,
        uint32_t seq,
        uint8_t battery,
        float voltage,
        uint16_t current,
        float pH,
        uint8_t phStatus,
        uint16_t tds,
        uint8_t tdsStatus,
        float turbidity,
        uint8_t turbidityStatus,
        float waterTemp,
        float moduleTemp,
        float esp32Temp,
        const char* errorMsg,
        int8_t rssi,
        float snr
    );

    // Get WiFi status string
    const char* getStatusString();

    // Get signal strength
    int8_t getSignalStrength();

    // Useful helpers
    String getIP();
    String getMacAddress();
}

#endif
