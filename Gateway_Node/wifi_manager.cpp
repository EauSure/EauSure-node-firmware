#include "wifi_manager.h"

namespace WiFiManager {
    
    // Initialize WiFi
    bool init() {
        Serial.println("[WiFi] Initializing...");
        
        // Disconnect if already connected
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect();
            delay(100);
        }
        
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        
        Serial.print("[WiFi] Connecting to '");
        Serial.print(WIFI_SSID);
        Serial.print("'");
        
        unsigned long startTime = millis();
        
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - startTime > WIFI_TIMEOUT_MS) {
                Serial.println(" TIMEOUT");
                Serial.println("[WiFi] Failed to connect");
                return false;
            }
            delay(500);
            Serial.print(".");
        }
        
        Serial.println(" CONNECTED");
        Serial.print("[WiFi] IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.print("[WiFi] Signal Strength: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
        Serial.print("[WiFi] Gateway MAC: ");
        Serial.println(WiFi.macAddress());
        
        return true;
    }
    
    // Check connection status
    bool isConnected() {
        return WiFi.status() == WL_CONNECTED;
    }
    
    // Reconnect if needed
    bool reconnect() {
        if (isConnected()) {
            return true;
        }
        
        Serial.println("[WiFi] Reconnecting...");
        WiFi.disconnect();
        delay(100);
        return init();
    }
    
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
    ) {
        // Check WiFi connection
        if (!isConnected()) {
            Serial.println("[WiFi] Not connected, attempting reconnect...");
            if (!reconnect()) {
                Serial.println("[WiFi] Reconnect failed - data not sent");
                return false;
            }
        }
        
        HTTPClient http;
        
        // Begin HTTP connection
        if (!http.begin(API_URL)) {
            Serial.println("[WiFi] Failed to begin HTTP connection");
            return false;
        }
        
        // Set headers
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Gateway-Key", API_KEY);
        http.setTimeout(HTTP_TIMEOUT_MS);
        
        // Build compact JSON payload (matches API format)
        String payload = "{";
        payload += "\"seq\":" + String(seq);
        payload += ",\"b\":" + String(battery);
        payload += ",\"v\":" + String(voltage, 2);
        payload += ",\"m\":" + String(current);
        payload += ",\"p\":" + String(pH, 2);
        payload += ",\"ps\":" + String(phStatus);
        payload += ",\"t\":" + String(tds);
        payload += ",\"ts\":" + String(tdsStatus);
        payload += ",\"u\":" + String(turbidity, 1);
        payload += ",\"us\":" + String(turbidityStatus);
        payload += ",\"tw\":" + String(waterTemp, 1);
        payload += ",\"tm\":" + String(moduleTemp, 1);
        payload += ",\"te\":" + String(esp32Temp, 1);
        payload += ",\"e\":\"" + String(errorMsg) + "\"";
        payload += ",\"rssi\":" + String(rssi);
        payload += ",\"snr\":" + String(snr, 1);
        payload += "}";
        
        Serial.println("[WiFi] Sending to API:");
        Serial.println("       " + payload);
        
        // Send POST request
        int httpCode = http.POST(payload);
        
        bool success = false;
        
        if (httpCode > 0) {
            Serial.print("[WiFi] HTTP Response Code: ");
            Serial.println(httpCode);
            
            String response = http.getString();
            
            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
                Serial.println("[WiFi] ✓ Data submitted successfully!");
                Serial.print("[WiFi] Server Response: ");
                Serial.println(response);
                success = true;
            } else {
                Serial.print("[WiFi] ✗ Server Error (");
                Serial.print(httpCode);
                Serial.println(")");
                Serial.print("[WiFi] Response: ");
                Serial.println(response);
            }
        } else {
            Serial.print("[WiFi] ✗ HTTP Error: ");
            Serial.println(http.errorToString(httpCode));
        }
        
        http.end();
        return success;
    }
    
    // Get status string
    const char* getStatusString() {
        switch (WiFi.status()) {
            case WL_CONNECTED:       return "Connected";
            case WL_NO_SSID_AVAIL:   return "SSID Not Found";
            case WL_CONNECT_FAILED:  return "Connection Failed";
            case WL_CONNECTION_LOST: return "Connection Lost";
            case WL_DISCONNECTED:    return "Disconnected";
            case WL_IDLE_STATUS:     return "Idle";
            default:                 return "Unknown";
        }
    }
    
    // Get signal strength
    int8_t getSignalStrength() {
        if (!isConnected()) {
            return 0;
        }
        return WiFi.RSSI();
    }
}
