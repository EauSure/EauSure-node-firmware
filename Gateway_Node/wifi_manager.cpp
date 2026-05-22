#include "wifi_manager.h"
#include "mqtt_gateway.h"
#include "sd_logger.h"
#include "tls_utils.h"
#include "wifi_store.h"

namespace WiFiManager {

    namespace {
        String gSsid = "";
        String gPassword = "";
        uint32_t gFirstFailureMs = 0;

        const char* selectDownloadCaPem(const String& host) {
            (void)host;
            if (TlsUtils::isPemConfigured(DOWNLOAD_TLS_ROOT_CA)) {
                return DOWNLOAD_TLS_ROOT_CA;
            }
            return API_TLS_ROOT_CA;
        }

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
            if (!TlsUtils::configureClient(client, API_TLS_ROOT_CA, "telemetry raw TLS")) {
                return false;
            }
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
            gFirstFailureMs = 0;
            return true;
        }

        if (gSsid.isEmpty()) {
            Serial.println("[WiFi] No stored runtime credentials for reconnect");
            return false;
        }

        Serial.println("[WiFi] Reconnecting...");
        WiFi.disconnect();
        delay(100);
        bool ok = init(gSsid.c_str(), gPassword.c_str());
        
        if (ok) {
            gFirstFailureMs = 0;
        } else {
            if (gFirstFailureMs == 0) {
                gFirstFailureMs = millis();
            } else if (millis() - gFirstFailureMs > 180000) { // 3 minutes
                Serial.println("[WiFi][FATAL] Failed to reconnect for 3 minutes. Erasing credentials and restarting to Provisioning mode...");
                WifiStore::clear();
                delay(1000);
                ESP.restart();
            }
        }
        return ok;
    }

    SubmitResult submitSensorData(
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
        float snr,
        int* httpStatusOut,
        const char* firmwareVersion
    ) {
        if (httpStatusOut) *httpStatusOut = 0;
        if (!nodeId || strlen(nodeId) == 0 || !gatewayHardwareId || strlen(gatewayHardwareId) == 0) {
            Serial.println("[WiFi] Missing nodeId or gatewayHardwareId");
            return SubmitResult::RetryableError;
        }

        if (!isConnected()) {
            Serial.println("[WiFi] Not connected, attempting reconnect...");
            if (!reconnect()) {
                Serial.println("[WiFi] Reconnect failed - data not sent");
                return SubmitResult::RetryableError;
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
        if (!TlsUtils::configureClient(client, API_TLS_ROOT_CA, "telemetry HTTPS POST")) {
            MqttGateway::setExclusiveTlsWindow(false);
            return SubmitResult::RetryableError;
        }

        HTTPClient http;
        http.setReuse(false);
        http.useHTTP10(true);
        http.setTimeout(10000);
        http.setConnectTimeout(10000);

        Serial.println("[WiFi][DIAG] http.begin -> " + url);
        if (!http.begin(client, url)) {
            Serial.println("[WiFi] Failed to begin HTTP connection");
            MqttGateway::setExclusiveTlsWindow(false);
            return SubmitResult::RetryableError;
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
        if (firmwareVersion && firmwareVersion[0] != '\0') {
            payload += ",\"fw\":\"";
            payload += firmwareVersion;
            payload += "\"";
        }
        payload += "}";

        Serial.println("[WiFi] Sending to API:");
        Serial.println("       " + payload);

        int httpCode = http.POST(payload);

        SubmitResult result = SubmitResult::RetryableError;

        if (httpCode > 0) {
            if (httpStatusOut) *httpStatusOut = httpCode;
            Serial.print("[WiFi] HTTP Response Code: ");
            Serial.println(httpCode);

            String response = http.getString();

            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
                Serial.println("[WiFi] Data submitted successfully");
                Serial.print("[WiFi] Server Response: ");
                Serial.println(response);
                result = SubmitResult::Success;
            } else {
                Serial.print("[WiFi] Server Error (");
                Serial.print(httpCode);
                Serial.println(")");
                Serial.print("[WiFi] Response: ");
                Serial.println(response);
                if (httpCode == 429) {
                    result = SubmitResult::RateLimited;
                }
            }
        } else {
            Serial.print("[WiFi] HTTP Error: ");
            Serial.println(http.errorToString(httpCode));
        }

        http.end();
        MqttGateway::setExclusiveTlsWindow(false);
        Serial.println("[WiFi][DIAG] ===== Sensor Submit end =====");
        return result;
    }

    bool unprovisionGateway() {
        if (!isConnected()) {
            Serial.println("[WiFi] Cannot unprovision via API (no Wi-Fi)");
            return false;
        }

        String gatewayHardwareId = String(GATEWAY_DEVICE_ID);
        gatewayHardwareId.trim();
        gatewayHardwareId.toUpperCase();
        if (!gatewayHardwareId.startsWith("GW-")) {
            gatewayHardwareId = "GW-" + gatewayHardwareId;
        }

        String url = String(API_BASE_URL) + "/api/registry/gateways/" + gatewayHardwareId + "/unprovision";
        String host = extractHostFromUrl(url);
        
        Serial.println("[WiFi] Unprovisioning Gateway on API: " + url);
        MqttGateway::setExclusiveTlsWindow(true);

        WiFiClientSecure client;
        if (!TlsUtils::configureClient(client, API_TLS_ROOT_CA, "unprovision HTTPS POST")) {
            MqttGateway::setExclusiveTlsWindow(false);
            return false;
        }

        HTTPClient http;
        http.setReuse(false);
        http.setTimeout(10000);
        http.setConnectTimeout(10000);

        if (!http.begin(client, url)) {
            Serial.println("[WiFi] Failed to begin HTTP connection for unprovision");
            MqttGateway::setExclusiveTlsWindow(false);
            return false;
        }

        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Gateway-Key", API_KEY);
        
        int httpCode = http.POST("{}");
        bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED);
        
        if (success) {
            Serial.println("[WiFi] Gateway unprovisioned on API successfully");
        } else {
            Serial.printf("[WiFi] Unprovision API Error: %d %s\n", httpCode, http.getString().c_str());
        }

        http.end();
        MqttGateway::setExclusiveTlsWindow(false);
        return success;
    }

    bool downloadFileToSd(
        const String& url,
        const char* sdPath,
        size_t expectedSize,
        String* errorOut
    ) {
        if (errorOut) *errorOut = "";

        if (!sdPath || strlen(sdPath) == 0) {
            if (errorOut) *errorOut = "Empty SD path";
            return false;
        }

        if (!isConnected() && !reconnect()) {
            if (errorOut) *errorOut = "WiFi unavailable";
            return false;
        }

        if (!ensureSdReady()) {
            if (errorOut) *errorOut = "SD not ready";
            return false;
        }

        String host = extractHostFromUrl(url);
        Serial.println();
        Serial.println("[WiFi][DIAG] ===== File Download begin =====");
        Serial.println("[WiFi][DIAG] URL: " + url);
        Serial.println("[WiFi][DIAG] Host: " + host);
        printHeapStats();
        printDnsLookup(host);

        const char* downloadCaPem = selectDownloadCaPem(host);
        const char* downloadTlsLabel = TlsUtils::isPemConfigured(DOWNLOAD_TLS_ROOT_CA)
            ? "download raw TLS"
            : "download raw TLS (API CA fallback)";

        MqttGateway::setExclusiveTlsWindow(true);
        rawTcpConnectTest(host, 443);
        {
            WiFiClientSecure rawTlsClient;
            if (TlsUtils::configureClient(rawTlsClient, downloadCaPem, downloadTlsLabel)) {
                Serial.printf("[WiFi][DIAG] raw TLS connect -> %s:%u\n", host.c_str(), 443);
                bool ok = rawTlsClient.connect(host.c_str(), 443);
                Serial.printf("[WiFi][DIAG] raw TLS connect: %s\n", ok ? "OK" : "FAIL");
                if (ok) rawTlsClient.stop();
            } else {
                Serial.println("[WiFi][DIAG] raw TLS connect: skipped (CA not configured)");
            }
        }

        WiFiClientSecure client;
        if (!TlsUtils::configureClient(client, downloadCaPem, "download HTTPS GET")) {
            MqttGateway::setExclusiveTlsWindow(false);
            if (errorOut) *errorOut = "TLS setup failed";
            return false;
        }

        HTTPClient http;
        http.setReuse(false);
        http.useHTTP10(true);
        http.setTimeout(15000);
        http.setConnectTimeout(15000);

        if (!http.begin(client, url)) {
            MqttGateway::setExclusiveTlsWindow(false);
            if (errorOut) *errorOut = "HTTP begin failed";
            return false;
        }

        const int code = http.GET();
        if (code != HTTP_CODE_OK) {
            String response = http.getString();
            http.end();
            MqttGateway::setExclusiveTlsWindow(false);
            if (errorOut) *errorOut = "HTTP GET failed: " + String(code) + " " + response;
            return false;
        }

        if (SD.exists(sdPath)) {
            SD.remove(sdPath);
        }

        File out = SD.open(sdPath, FILE_WRITE);
        if (!out) {
            http.end();
            MqttGateway::setExclusiveTlsWindow(false);
            if (errorOut) *errorOut = "Cannot open SD output file";
            return false;
        }

        WiFiClient* stream = http.getStreamPtr();
        uint8_t buffer[512];
        size_t totalWritten = 0;

        while (http.connected() && (stream->available() || stream->connected())) {
            const size_t available = stream->available();
            if (available == 0) {
                delay(10);
                continue;
            }

            const size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
            const int readLen = stream->readBytes(buffer, toRead);
            if (readLen <= 0) {
                continue;
            }

            const size_t written = out.write(buffer, (size_t)readLen);
            if (written != (size_t)readLen) {
                out.close();
                http.end();
                MqttGateway::setExclusiveTlsWindow(false);
                if (errorOut) *errorOut = "SD write failed during download";
                return false;
            }

            totalWritten += written;
        }

        out.close();
        http.end();
        MqttGateway::setExclusiveTlsWindow(false);

        if (expectedSize > 0 && totalWritten != expectedSize) {
            if (errorOut) {
                *errorOut = "Size mismatch. expected=" + String((unsigned)expectedSize) +
                            " actual=" + String((unsigned)totalWritten);
            }
            return false;
        }

        Serial.printf("[WiFi] Downloaded %u bytes to %s\n",
                      (unsigned)totalWritten,
                      sdPath);
        Serial.println("[WiFi][DIAG] ===== File Download end =====");
        return true;
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
