#pragma once
#include <Arduino.h>

// Initialize I/O pins and scanner
bool IOScanner_init();

// Main scanning task loop (call from FreeRTOS task)
void IOScanner_taskLoop(void *pvParameters);

// Manual control functions (for testing/web UI)
void IOScanner_setRelayOutput(size_t index, bool state);
bool IOScanner_getRelayOutput(size_t index);

// Read current raw values
float IOScanner_getAnalogRaw(size_t index);
bool IOScanner_getDigitalRaw(size_t index);
