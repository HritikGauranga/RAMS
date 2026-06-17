#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "AP.h"
#include "Modem.h"
#include "RTU.h"
#include "Shared.h"
#include "TCP.h"

namespace {
  constexpr uint32_t RTU_TASK_STACK   = 4096;
  constexpr uint32_t TCP_TASK_STACK   = 6144;
  constexpr uint32_t MODEM_TASK_STACK = 8192;
  constexpr uint32_t AP_TASK_STACK    = 4096;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== Remote Alarm Monitoring System (RAMS) ===");

  Shared_init();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(AP_STATUS_LED_PIN, OUTPUT);
  pinMode(MODEM_INIT_STATUS_PIN, OUTPUT);
  digitalWrite(AP_STATUS_LED_PIN, LOW);
  digitalWrite(MODEM_INIT_STATUS_PIN, LOW);

  if (!LittleFS.begin(true)) {
    Serial.println("[ERROR] LittleFS mount failed — halting");
    while (true) delay(1000);
  }

  Serial.printf("Used: %u\n", LittleFS.usedBytes());
  Serial.printf("Total: %u\n", LittleFS.totalBytes());

  // Initialize shared state after filesystem is mounted
  Shared_init();

  // MBmap CSV and Modbus RTU removed; keep gateway settings load
  Shared_loadGatewaySettings();

  // Keep lwIP active for always-on Web UI; AP task switches to AP_STA when needed.
  WiFi.mode(WIFI_STA);
  delay(100);

  // RTU (Modbus RTU) removed for RAMS firmware
  TCP_init();

  if (!Modem_init()) {
    Serial.println("[MODEM] Modem init setup failed — halting");
    while (true) delay(1000);
  }

  // ---------------------------------------------------------------------------
  // Task layout — 4 tasks total:
  //
  //   Core 0: SmsTask  (priority 2)
  //     Calls initModem() at startup (~15s), then scans edges and sends SMS.
  //     Core 1 runs freely during modem init — no blocking effect on Modbus.
  //
  //   Core 1: RTUTask  (priority 3) — Modbus RTU, highest prio on core 1
  //           TCPTask  (priority 2) — Modbus TCP
  //           ApTask   (priority 1) — Wi-Fi AP config server, lowest prio
  // ---------------------------------------------------------------------------

  xTaskCreatePinnedToCore(Modem_task,   "SmsTask",  MODEM_TASK_STACK, nullptr, 2, nullptr, 0); 
  xTaskCreatePinnedToCore(TCP_taskLoop, "TCPTask",  TCP_TASK_STACK,   nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(AP_taskLoop,  "ApTask",   AP_TASK_STACK,    nullptr, 1, nullptr, 1);

  Serial.println("[SYSTEM] Tasks started: SmsTask, TCPTask, ApTask");
}


void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
