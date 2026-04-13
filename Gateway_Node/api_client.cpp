#include "api_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_system.h>

// =====================================================
// Helpers
// =====================================================
static String extractHostFromUrl(const String& url) {
  int start = 0;

  if (url.startsWith("https://")) {
    start = 8;
  } else if (url.startsWith("http://")) {
    start = 7;
  }

  int slash = url.indexOf('/', start);
  if (slash < 0) return url.substring(start);
  return url.substring(start, slash);
}

static void printHeapStats() {
  Serial.printf("[API][DIAG] Free heap: %u\n", ESP.getFreeHeap());
  Serial.printf("[API][DIAG] Min free heap: %u\n", ESP.getMinFreeHeap());
}

static bool rawTcpConnectTest(const String& host, uint16_t port) {
  WiFiClient client;
  client.setTimeout(5000);

  Serial.printf("[API][DIAG] raw TCP connect -> %s:%u\n", host.c_str(), port);
  bool ok = client.connect(host.c_str(), port);
  Serial.printf("[API][DIAG] raw TCP connect: %s\n", ok ? "OK" : "FAIL");

  if (ok) {
    client.stop();
  }
  return ok;
}

static bool rawTlsConnectTest(const String& host, uint16_t port) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);
  client.setHandshakeTimeout(15);

  Serial.printf("[API][DIAG] raw TLS connect -> %s:%u\n", host.c_str(), port);
  bool ok = client.connect(host.c_str(), port);
  Serial.printf("[API][DIAG] raw TLS connect: %s\n", ok ? "OK" : "FAIL");

  if (ok) {
    client.stop();
  }
  return ok;
}

static void printDnsLookup(const String& host) {
  IPAddress resolved;
  if (WiFi.hostByName(host.c_str(), resolved)) {
    Serial.printf("[API][DIAG] DNS %s -> %s\n",
                  host.c_str(),
                  resolved.toString().c_str());
  } else {
    Serial.printf("[API][DIAG] DNS FAILED for %s\n", host.c_str());
  }
}

// =====================================================
// Internal helper — runs one HTTPS POST and fills out
// the raw HTTP code + response string.
// Returns the HTTP code (<=0 means transport error).
// =====================================================
static int httpsPost(const String& url, const String& body, String& responseOut) {
  responseOut = "";

  String host = extractHostFromUrl(url);

  Serial.println();
  Serial.println("[API][DIAG] ===== HTTPS POST begin =====");
  Serial.println("[API][DIAG] URL: " + url);
  Serial.println("[API][DIAG] Host: " + host);
  Serial.printf("[API][DIAG] WiFi status: %d\n", (int)WiFi.status());
  Serial.printf("[API][DIAG] Local IP: %s\n", WiFi.localIP().toString().c_str());
  printHeapStats();

  printDnsLookup(host);
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

  Serial.println("[API][DIAG] http.begin -> " + url);
  if (!http.begin(client, url)) {
    Serial.println("[API][DIAG] http.begin FAILED");
    return -1;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  int code = http.POST(body);
  responseOut = (code > 0) ? http.getString() : "";

  Serial.printf("[API][DIAG] HTTP code: %d\n", code);
  if (code <= 0) {
    Serial.println("[API][DIAG] errorToString: " + http.errorToString(code));
  } else {
    Serial.println("[API][DIAG] Response: " + responseOut);
  }

  http.end();
  Serial.println("[API][DIAG] ===== HTTPS POST end =====");
  return code;
}

// =====================================================
// Internal helper — runs one HTTPS GET and fills out
// the raw HTTP code + response string.
// Returns the HTTP code (<=0 means transport error).
// =====================================================
static int httpsGet(const String& url, String& responseOut) {
  responseOut = "";

  String host = extractHostFromUrl(url);

  Serial.println();
  Serial.println("[API][DIAG] ===== HTTPS GET begin =====");
  Serial.println("[API][DIAG] URL: " + url);
  Serial.println("[API][DIAG] Host: " + host);
  Serial.printf("[API][DIAG] WiFi status: %d\n", (int)WiFi.status());
  Serial.printf("[API][DIAG] Local IP: %s\n", WiFi.localIP().toString().c_str());
  printHeapStats();

  printDnsLookup(host);
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

  Serial.println("[API][DIAG] http.begin -> " + url);
  if (!http.begin(client, url)) {
    Serial.println("[API][DIAG] http.begin FAILED");
    return -1;
  }

  http.addHeader("Connection", "close");

  int code = http.GET();
  responseOut = (code > 0) ? http.getString() : "";

  Serial.printf("[API][DIAG] HTTP code: %d\n", code);
  if (code <= 0) {
    Serial.println("[API][DIAG] errorToString: " + http.errorToString(code));
  } else {
    Serial.println("[API][DIAG] Response: " + responseOut);
  }

  http.end();
  Serial.println("[API][DIAG] ===== HTTPS GET end =====");
  return code;
}

namespace ApiClient {

bool healthCheck(const String& apiBaseUrl) {
  Serial.println("\n[API][HEALTH] -- HTTPS reachability check --");

  if (apiBaseUrl.isEmpty()) {
    Serial.println("[API][HEALTH] FAILED - empty API base URL");
    return false;
  }

  String url = apiBaseUrl + "/health";
  Serial.println("[API][HEALTH] Target: " + url);

  String resp;
  int code = httpsGet(url, resp);

  if (code <= 0) {
    Serial.printf("[API][HEALTH] FAILED - code=%d\n", code);
    return false;
  }

  Serial.printf("[API][HEALTH] HTTP %d | %s\n", code, resp.c_str());
  return (code >= 200 && code < 300);
}

bool provisionGateway(
  const String& apiBaseUrl,
  const String& gatewayHardwareId,
  const String& firmwareVersion,
  const String& token,
  const String& gatewayName,
  GatewayProvisionResult& out
) {
  out = GatewayProvisionResult{};

  if (apiBaseUrl.isEmpty()) {
    out.message = "API base URL is empty";
    return false;
  }

  String url = apiBaseUrl + "/api/registry/gateway/provision";

  StaticJsonDocument<256> req;
  req["gatewayHardwareId"] = gatewayHardwareId;
  req["firmwareVersion"]   = firmwareVersion;
  req["token"]             = token;
  req["gatewayName"]       = gatewayName;

  String body;
  serializeJson(req, body);

  Serial.println("\n[API] POST " + url);
  Serial.println("[API] Body: " + body);

  String response;
  int code = httpsPost(url, body, response);

  if (code <= 0) {
    out.message = "HTTP POST failed (code=" + String(code) + ")";
    return false;
  }

  StaticJsonDocument<512> resp;
  if (deserializeJson(resp, response)) {
    out.message = "Invalid JSON response";
    return false;
  }

  out.success = resp["success"] | false;
  out.message = String(resp["message"] | "");

  if (out.success) {
    JsonObject data = resp["data"];
    out.gatewayId = String(data["gatewayId"] | "");
    out.name      = String(data["name"] | "");
    out.mqttTopic = String(data["mqttTopic"] | "");
    return true;
  }

  return false;
}

bool pairNode(
  const String& apiBaseUrl,
  const String& jwtToken,
  const String& gatewayHardwareId,
  const String& nodeId,
  const String& nodeName,
  const String& nodeBleMac,
  NodePairingResult& out
) {
  out = NodePairingResult{};

  if (apiBaseUrl.isEmpty()) { out.message = "API base URL is empty"; return false; }
  if (jwtToken.isEmpty())   { out.message = "JWT token is empty";    return false; }

  String url = apiBaseUrl + "/api/registry/pair-node";

  StaticJsonDocument<320> req;
  req["gatewayHardwareId"] = gatewayHardwareId;
  req["nodeId"]            = nodeId;
  req["nodeName"]          = nodeName;
  req["nodeBleMac"]        = nodeBleMac;
  req["token"]             = jwtToken;

  String body;
  serializeJson(req, body);

  Serial.println("\n[API] POST " + url);
  Serial.println("[API] Body: " + body);

  String response;
  int code = httpsPost(url, body, response);

  if (code <= 0) {
    out.message = "HTTP POST failed (code=" + String(code) + ")";
    return false;
  }

  StaticJsonDocument<512> resp;
  if (deserializeJson(resp, response)) {
    out.message = "Invalid JSON response";
    return false;
  }

  out.success = resp["success"] | false;
  out.message = String(resp["message"] | "");

  if (!out.success) return false;

  JsonObject data = resp["data"];
  out.aesKey            = String(data["aesKey"] | "");
  out.nodeId            = String(data["nodeId"] | "");
  out.gatewayHardwareId = String(data["gatewayHardwareId"] | "");

  if (out.aesKey.isEmpty() || out.nodeId.isEmpty() || out.gatewayHardwareId.isEmpty()) {
    out.message = "Missing fields in pair-node response";
    return false;
  }

  return true;
}

} // namespace ApiClient