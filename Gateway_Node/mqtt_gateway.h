#pragma once
#include <Arduino.h>

namespace MqttGateway {

void begin();
void loop();

bool isConnected();
void setExclusiveTlsWindow(bool enabled);

bool publishEvent(const String& eventName, const String& payloadJson);
bool publishCandidateFound(const String& nodeId, const String& nodeName, const String& bleMac);

String commandTopic();
String eventTopic();

} // namespace MqttGateway
