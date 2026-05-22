#pragma once

#include <Arduino.h>

namespace FuotaManager {

void begin();
void loop();

bool hasPendingNodeUpdate();
bool handleNodeDataWindow();

void queueGatewayUpdate(const String& url, const String& version, const String& md5, size_t size,
                        const String& cmdId = "");
void queueNodeUpdate(const String& nodeId, const String& url, const String& version, const String& md5,
                     size_t size, const String& cmdId = "");

bool isBusy();
bool isGatewayUpdateInProgress();

// Deferred MQTT/API command lifecycle (UPDATE_FIRMWARE only).
String getBoundCommandId();
bool commandReadyToAck();
bool commandShouldFail(String& reason);
void clearCommandBinding();
void dismissFailedJob();

} // namespace FuotaManager
