#include "wifi_manager.h"
#include "mqtt_gateway.h"

namespace WiFiManager {

    namespace {
        String gSsid = "";
        String gPassword = "";

        String extractHostFromUrl(const String& url) {
            int start = 0;
            if (url.startsWith("https://")) start = 8;
            else if (url.startsWith("http://")) start = 7;

            int slash = url.indexOf('/', start);
            if (slash < 0) return url.substring(start);
            return url.substring(start, slash);
        }

        void printHeapStats() {
            Serial.printf("[WiFi][DIAG] Free heap: %u\n", ESP.getFreeHeap());
            Serial.printf("[WiFi][DIAG] Min free heap: %u\n", ESP.getMinFreeHeap());
        }

        void printDnsLookup(const String& host) {
            IPAddress resolved;
            if (WiFi.hostByName(host.c_str(), resolved)) {
                Serial.printf("[WiFi][DIAG] DNS %s -> %s\n", host.c_str(), resolved.toString().c_str());
            } else {
                Serial.printf("[WiFi][DIAG] DNS FAILED for %s\n", host.c_str());
            }
        }

        bool rawTcpConnectTest(const String& host, uint16_t port) {
            WiFiClient client;
            client.setTimeout(5000);
            Serial.printf("[WiFi][DIAG] raw TCP connect -> %s:%u\n", host.c_str(), port);
            bool ok = client.connect(host.c_str(), port);
            Serial.printf("[WiFi][DIAG] raw TCP connect: %s\n", ok ? "OK" : "FAIL");
            if (ok) client.stop();
            return ok;
        }

        bool rawTlsConnectTest(const String& host, uint16_t port) {
            WiFiClientSecure client;
            client.setInsecure();
            client.setTimeout(10000);
            client.setHandshakeTimeout(15);
            Serial.printf("[WiFi][DIAG] raw TLS connect -> %s:%u\n", host.c_str(), port);
            bool ok = client.connect(host.c_str(), port);
            Serial.printf("[WiFi][DIAG] raw TLS connect: %s\n", ok ? "OK" : "FAIL");
            if (ok) client.stop();
            return ok;
        }
    }

    bool init(const char* ssid, const char* password) {
        Serial.println("[WiFi] Initializing...");

        if (!ssid || !password || strlen(ssid) == 0) {
            Serial.println("[WiFi] Missing runtime credentials");
            return false;
        }

        gSsid = ssid;
        gPassword = password;

        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect();
            delay(100);
        }

        WiFi.mode(WIFI_STA);
        WiFi.begin(gSsid.c_str(), gPassword.c_str());

        Serial.print("[WiFi] Connecting to '");
        Serial.print(gSsid);
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

    bool isConnected() {
        return WiFi.status() == WL_CONNECTED;
    }

    bool reconnect() {
        if (isConnected()) {
            return true;
        }

        if (gSsid.isEmpty()) {
            Serial.println("[WiFi] No stored runtime credentials for reconnect");
            return false;
        }

        Serial.println("[WiFi] Reconnecting...");
        WiFi.disconnect();
        delay(100);
        return init(gSsid.c_str(), gPassword.c_str());
    }

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
    ) {
        if (!nodeId || strlen(nodeId) == 0 || !gatewayHardwareId || strlen(gatewayHardwareId) == 0) {
            Serial.println("[WiFi] Missing nodeId or gatewayHardwareId");
            return false;
        }

        if (!isConnected()) {
            Serial.println("[WiFi] Not connected, attempting reconnect...");
            if (!reconnect()) {
                Serial.println("[WiFi] Reconnect failed - data not sent");
                return false;
            }
        }

        String url = API_URL;
        String host = extractHostFromUrl(url);
        Serial.println();
        Serial.println("[WiFi][DIAG] ===== Sensor Submit begin =====");
        Serial.println("[WiFi][DIAG] URL: " + url);
        Serial.println("[WiFi][DIAG] Host: " + host);
        Serial.printf("[WiFi][DIAG] WiFi status: %d\n", (int)WiFi.status());
        Serial.printf("[WiFi][DIAG] Local IP: %s\n", WiFi.localIP().toString().c_str());
        printHeapStats();
        printDnsLookup(host);
        MqttGateway::setExclusiveTlsWindow(true);
        rawTcpConnectTest(host, 443);
        rawTlsConnectTest(host, 443);

        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(10000);
        client.setHandshakeTimeout(15);

        HTTPClient http;
        http.setReuse(false);
        http.useHTTP10(true);
        http.setTimeout(10000);
        http.setConnectTimeout(10000);

        Serial.println("[WiFi][DIAG] http.begin -> " + url);
        if (!http.begin(client, url)) {
            Serial.println("[WiFi] Failed to begin HTTP connection");
            MqttGateway::setExclusiveTlsWindow(false);
            return false;
        }

        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Gateway-Key", API_KEY);
        http.addHeader("Connection", "close");
        http.setTimeout(HTTP_TIMEOUT_MS);

        String payload = "{";
        payload += "\"seq\":" + String(seq);
        payload += ",\"nodeId\":\"" + String(nodeId) + "\"";
        payload += ",\"gatewayHardwareId\":\"" + String(gatewayHardwareId) + "\"";
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
        payload += ",\"e\":\"" + String(errorMsg ? errorMsg : "None") + "\"";
        payload += ",\"rssi\":" + String(rssi);
        payload += ",\"snr\":" + String(snr, 1);
        payload += "}";

        Serial.println("[WiFi] Sending to API:");
        Serial.println("       " + payload);

        int httpCode = http.POST(payload);

        bool success = false;

        if (httpCode > 0) {
            Serial.print("[WiFi] HTTP Response Code: ");
            Serial.println(httpCode);

            String response = http.getString();

            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
                Serial.println("[WiFi] Data submitted successfully");
                Serial.print("[WiFi] Server Response: ");
                Serial.println(response);
                success = true;
            } else {
                Serial.print("[WiFi] Server Error (");
                Serial.print(httpCode);
                Serial.println(")");
                Serial.print("[WiFi] Response: ");
                Serial.println(response);
            }
        } else {
            Serial.print("[WiFi] HTTP Error: ");
            Serial.println(http.errorToString(httpCode));
        }

        http.end();
        MqttGateway::setExclusiveTlsWindow(false);
        Serial.println("[WiFi][DIAG] ===== Sensor Submit end =====");
        return success;
    }

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

    int8_t getSignalStrength() {
        if (!isConnected()) {
            return 0;
        }
        return WiFi.RSSI();
    }

    String getIP() {
        return WiFi.localIP().toString();
    }

    String getMacAddress() {
        return WiFi.macAddress();
    }
}
