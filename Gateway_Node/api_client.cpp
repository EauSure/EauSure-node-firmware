#include "api_client.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace ApiClient {

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

  HTTPClient http;
  String url = apiBaseUrl + "/api/registry/gateway/provision";

  if (!http.begin(url)) {
    out.message = "HTTP begin failed";
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);

  StaticJsonDocument<256> req;
  req["gatewayHardwareId"] = gatewayHardwareId;
  req["firmwareVersion"] = firmwareVersion;
  req["token"] = token;
  req["gatewayName"] = gatewayName;

  String body;
  serializeJson(req, body);

  Serial.println("[API] POST " + url);
  Serial.println("[API] Body: " + body);

  int code = http.POST(body);
  String response = http.getString();
  http.end();

  if (code <= 0) {
    out.message = "HTTP POST failed";
    return false;
  }

  Serial.printf("[API] HTTP %d\n", code);
  Serial.println("[API] Response: " + response);

  StaticJsonDocument<512> resp;
  DeserializationError err = deserializeJson(resp, response);
  if (err) {
    out.message = "Invalid JSON response";
    return false;
  }

  out.success = resp["success"] | false;
  out.message = String((const char*)resp["message"] | "");

  if (out.success) {
    JsonObject data = resp["data"];
    out.gatewayId = String((const char*)data["gatewayId"] | "");
    out.name = String((const char*)data["name"] | "");
    out.mqttTopic = String((const char*)data["mqttTopic"] | "");
    return true;
  }

  return false;
}

}