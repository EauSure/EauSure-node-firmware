#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"

namespace WiFiManager {
    // Initialize WiFi connection
    bool init();
    
    // Check if connected
    bool isConnected();
    
    // Reconnect if disconnected
    bool reconnect();
    
    // Submit sensor data to API
    bool submitSensorData(
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
}

#endif
