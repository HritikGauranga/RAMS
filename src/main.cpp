#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "config.h"
#include "AP.h"
#include "Shared.h"

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("GuardianMini starting...");

  // Initialize filesystem
  if (!Shared_lockFileSystem()) {
    Serial.println("Warning: LittleFS init failed or busy");
  }

  // Configure pins
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(AP_STATUS_LED_PIN, OUTPUT);
  digitalWrite(AP_STATUS_LED_PIN, LOW);

  // Create AP task
  xTaskCreate(
    AP_taskLoop,
    "APTask",
    16 * 1024,
    NULL,
    1,
    NULL
  );
}

void loop() {
  // Main loop can be idle; AP runs in its own FreeRTOS task
  vTaskDelay(pdMS_TO_TICKS(1000));
}
