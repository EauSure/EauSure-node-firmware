#include "hardware_ui.h"
#include "config.h"
#include "wifi_store.h"
#include "wifi_manager.h"
#include "audio_alert.h"
#include "ble_provisioning.h"

namespace HardwareUI {

  static GatewayStatus gCurrentStatus = GatewayStatus::PROVISIONING;
  static TaskHandle_t gUiTaskHandle = nullptr;
  static volatile bool gFactoryResetRequested = false;

  void setStatus(GatewayStatus status) {
    gCurrentStatus = status;
  }

  GatewayStatus getStatus() {
    return gCurrentStatus;
  }

  static void uiTask(void* pvParameters) {
    // GPIO 35 does NOT have an internal pull-up. An external 10k resistor to 3.3V is REQUIRED.
    pinMode(BUTTON_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    uint32_t buttonPressStartMs = 0;
    bool buttonWasPressed = false;
    bool ledState = false;
    uint32_t lastLedUpdateMs = 0;

    while (true) {
      uint32_t now = millis();

      // --- LED BLINKING LOGIC ---
      uint32_t blinkInterval = 1000;
      switch (gCurrentStatus) {
        case GatewayStatus::PROVISIONING:
          blinkInterval = 200; // Fast blink
          break;
        case GatewayStatus::WIFI_CONNECTING:
        case GatewayStatus::MQTT_CONNECTING:
          blinkInterval = 1000; // Slow blink
          break;
        case GatewayStatus::CONNECTED:
          blinkInterval = 0; // Solid ON
          ledState = true;
          digitalWrite(LED_PIN, HIGH);
          break;
      }

      if (blinkInterval > 0) {
        if (now - lastLedUpdateMs >= blinkInterval) {
          lastLedUpdateMs = now;
          ledState = !ledState;
          digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
      }

      // --- BUTTON HOLD LOGIC ---
      // BOOT button is pulled high, pressed is LOW
      bool isPressed = (digitalRead(BUTTON_PIN) == LOW);
      
      if (isPressed && !buttonWasPressed) {
        buttonPressStartMs = now;
        buttonWasPressed = true;
      } else if (!isPressed && buttonWasPressed) {
        buttonWasPressed = false;
      }

      if (isPressed && buttonWasPressed) {
        if (now - buttonPressStartMs >= 5000) {
          Serial.println("[HARDWARE UI] Button held for 5 seconds. Initiating Factory Reset!");
          digitalWrite(LED_PIN, LOW);

          // ─────────────────────────────────────────────────────────────
          // CRITICAL PATH FIRST — clear credentials before anything else.
          // Any reset from this point (brownout, RST button, ESP.restart)
          // will correctly boot into PROVISIONING mode.
          // ─────────────────────────────────────────────────────────────
          Serial.println("[HARDWARE UI] Erasing WiFi credentials...");
          WifiStore::clear();

          if (WiFiManager::isConnected()) {
            // Signal the main loop to handle the unprovision API call safely.
            // We cannot do HTTPS from the UI task (heap corruption with
            // concurrent TLS clients). The main loop will unprovision + reboot.
            gFactoryResetRequested = true;
            Serial.println("[HARDWARE UI] Wi-Fi connected — main loop will handle unprovision + reboot");

            if (BleProvisioning::isActive()) {
              Serial.println("[HARDWARE UI] Stopping BLE before factory-reset voice");
              BleProvisioning::stop();
              delay(200);
            }

            Serial.println("[HARDWARE UI] Playing factory-reset confirmation (best-effort)");
            playAlertFile("/alerts/reset.wav");
            delay(500);

            // Idle until the main loop picks up the flag and reboots.
            Serial.println("[HARDWARE UI] Waiting for main loop to complete unprovision and reboot...");
            while (true) { vTaskDelay(pdMS_TO_TICKS(100)); }
          }

          // WiFi not connected — handle everything locally (no API call needed).
          Serial.println("[HARDWARE UI] Wi-Fi is disconnected, skipping backend notification.");

          if (BleProvisioning::isActive()) {
            Serial.println("[HARDWARE UI] Stopping BLE before factory-reset voice");
            BleProvisioning::stop();
            delay(200);
          }

          Serial.println("[HARDWARE UI] Playing factory-reset confirmation (best-effort)");
          playAlertFile("/alerts/reset.wav");
          delay(500);

          Serial.println("[HARDWARE UI] Rebooting to PROVISIONING mode...");
          delay(200);
          ESP.restart();
        }
      }

      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  void begin() {
    xTaskCreate(
      uiTask,
      "UI_Task",
      8192,   // bumped from 4096: factory-reset voice uses AudioTools
              // (I2SStream + WAVDecoder + EncodedAudioStream + SD read),
              // which overflows a 4 KB task stack and silently kills the task
              // mid-playback.
      nullptr,
      1,
      &gUiTaskHandle
    );
  }

  bool isFactoryResetRequested() {
    return gFactoryResetRequested;
  }

}
