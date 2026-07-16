#include "IOScanner.h"
#include "Shared.h"
#include <driver/adc.h>
#include <esp_adc_cal.h>

// ---------------------------------------------------------------------------
// GPIO Pin Definitions
// ---------------------------------------------------------------------------
// Analog Inputs (ADC1 only - avoid ADC2 as it conflicts with WiFi)
constexpr int AI_PIN[ANALOG_INPUT_COUNT] = {34, 35};  // GPIO34=ADC1_CH6, GPIO35=ADC1_CH7

// Digital Inputs (DI3/DI4 not yet wired — set to 0 to mark as unassigned)
// WARNING: DI3 and DI4 are configurable in the UI but will always read Normal
// at runtime until real GPIO pins are assigned here.
constexpr int DI_PIN[DIGITAL_INPUT_COUNT] = {26, 27, 0, 0};  // DI1=GPIO26, DI2=GPIO27, DI3/4=unassigned

// Digital Outputs (Relay mock LEDs)
constexpr int DO_PIN[RELAY_OUTPUT_COUNT] = {25, 13};  // DO1=GPIO25, DO2=GPIO13

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr uint32_t SCAN_INTERVAL_MS = 100;        // Scan every 100ms
constexpr uint32_t ANALOG_SAMPLES = 10;           // Average 10 samples per reading
constexpr uint32_t DEBOUNCE_TIME_MS = 50;         // Debounce digital inputs

// ADC characteristics
constexpr float ADC_MAX_VALUE = 4095.0f;          // 12-bit ADC
constexpr float ADC_VREF = 3.3f;                  // ESP32 ADC reference voltage

// 4-20mA simulation mapping (POT gives 0-3.3V, we map it to 4-20mA scale)
constexpr float CURRENT_MIN = 4.0f;               // 4mA
constexpr float CURRENT_MAX = 20.0f;              // 20mA

// ---------------------------------------------------------------------------
// State tracking structures
// ---------------------------------------------------------------------------
struct AnalogInputState {
  float lastValue;                    // Last engineering value
  bool inAlarm;                       // Currently in alarm state
  unsigned long alarmTriggerTime;     // Time when alarm condition started
  unsigned long returnTriggerTime;    // Time when return condition started
  bool alarmSmsSent;                  // Whether alarm SMS was sent
};

struct DigitalInputState {
  bool lastStableValue;               // Last debounced value
  bool currentRawValue;               // Current reading
  unsigned long lastChangeTime;       // Time of last state change
  bool inAlarm;                       // Currently in alarm state
  unsigned long alarmTriggerTime;     // Time when alarm condition started
  unsigned long returnTriggerTime;    // Time when return condition started
  bool alarmSmsSent;                  // Whether alarm SMS was sent
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static AnalogInputState aiState[ANALOG_INPUT_COUNT] = {};
static DigitalInputState diState[DIGITAL_INPUT_COUNT] = {};
static bool relayOutputs[RELAY_OUTPUT_COUNT] = {false, false};
static bool relayAlarmHeld[RELAY_OUTPUT_COUNT] = {false, false}; // true = relay ON due to active alarm

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

// Convert ADC raw value (0-4095) to simulated current (4-20mA)
static float adcToCurrent(uint16_t adcValue) {
  float voltage = (adcValue / ADC_MAX_VALUE) * ADC_VREF;
  // Map 0-3.3V to 4-20mA linearly
  return CURRENT_MIN + (voltage / ADC_VREF) * (CURRENT_MAX - CURRENT_MIN);
}

// Convert current (4-20mA) to engineering units based on config
static float currentToEngineering(float current, const AnalogInputConfig &cfg) {
  if (current < CURRENT_MIN) current = CURRENT_MIN;
  if (current > CURRENT_MAX) current = CURRENT_MAX;
  
  // Linear scaling: 4mA = scale_low, 20mA = scale_high
  float range = CURRENT_MAX - CURRENT_MIN;
  float normalized = (current - CURRENT_MIN) / range;
  return cfg.scale_low + normalized * (cfg.scale_high - cfg.scale_low) + cfg.offset;
}

// Read analog input with averaging
static float readAnalogAveraged(size_t index) {
  if (index >= ANALOG_INPUT_COUNT) return 0.0f;
  
  uint32_t sum = 0;
  for (uint32_t i = 0; i < ANALOG_SAMPLES; ++i) {
    sum += analogRead(AI_PIN[index]);
    delayMicroseconds(100);  // Small delay between samples
  }
  
  uint16_t avgAdc = sum / ANALOG_SAMPLES;
  float current = adcToCurrent(avgAdc);
  
  return current;
}

// Check if analog value triggers alarm based on alarm type
static bool checkAnalogAlarm(float value, const AnalogInputConfig &cfg) {
  float lo = (cfg.set_point < cfg.reset_point) ? cfg.set_point : cfg.reset_point;
  float hi = (cfg.set_point < cfg.reset_point) ? cfg.reset_point : cfg.set_point;
  switch (cfg.alarm_type) {
    case 0:  // High alarm
      return value >= cfg.set_point;
    case 1:  // Low alarm
      return value <= cfg.set_point;
    case 2:  // In-Band alarm (alarm if value is BETWEEN set_point and reset_point)
      return (value >= lo && value <= hi);
    case 3:  // Out-of-Band alarm (alarm if value is OUTSIDE set_point and reset_point)
      return (value < lo || value > hi);
    default:
      return false;
  }
}

// Check if analog value triggers return (alarm clear) based on alarm type
static bool checkAnalogReturn(float value, const AnalogInputConfig &cfg) {
  float lo = (cfg.set_point < cfg.reset_point) ? cfg.set_point : cfg.reset_point;
  float hi = (cfg.set_point < cfg.reset_point) ? cfg.reset_point : cfg.set_point;
  switch (cfg.alarm_type) {
    case 0:  // High alarm - return when below reset point
      return value < cfg.reset_point;
    case 1:  // Low alarm - return when above reset point
      return value > cfg.reset_point;
    case 2:  // In-Band alarm - return when outside the band
      return (value < lo || value > hi);
    case 3:  // Out-of-Band alarm - return when inside the band
      return (value >= lo && value <= hi);
    default:
      return true;
  }
}

// Process analog input
static void processAnalogInput(size_t index) {
  if (index >= ANALOG_INPUT_COUNT) return;
  
  AnalogInputConfig cfg = {};
  if (!Shared_getAnalogInputConfig(index, cfg)) return;
  if (!cfg.enabled) return;
  
  // Read current value
  float current = readAnalogAveraged(index);
  float engValue = currentToEngineering(current, cfg);
  
  // Update shared state
  Shared_writeAnalogInput(index, engValue);
  
  AnalogInputState &state = aiState[index];
  state.lastValue = engValue;
  
  unsigned long now = millis();
  
  // Check alarm condition
  bool alarmCondition = checkAnalogAlarm(engValue, cfg);
  
  if (alarmCondition) {
    // Alarm condition present
    if (!state.inAlarm) {
      // Just entered alarm condition
      if (state.alarmTriggerTime == 0) {
        state.alarmTriggerTime = now;
        Serial.printf("[AI%d] Alarm condition detected: %.2f %s (TTA: %dms)\n", 
                      index + 1, engValue, cfg.engineering_unit, cfg.tta_ms);
      } else if (now - state.alarmTriggerTime >= cfg.tta_ms) {
        // TTA expired - trigger alarm
        state.inAlarm = true;
        state.returnTriggerTime = 0;
        Serial.printf("[AI%d] ALARM TRIGGERED: %.2f %s\n", 
                      index + 1, engValue, cfg.engineering_unit);
        Shared_setAIAlarmState(index, true);
        Shared_setLastEventTime();
        
        if (!state.alarmSmsSent) {
          // Reset ACK state for this new alarm
          Shared_setAlarmAck(ALARM_SRC_AI, index, false);
          // Post unified notification event
          NotificationEvent ev = {};
          ev.source = ALARM_SRC_AI;
          ev.index  = index;
          ev.isAlarm = true;
          ev.value  = engValue;
          strncpy(ev.message, cfg.alarm_message, sizeof(ev.message) - 1);
          if (ev.message[0] == '\0') snprintf(ev.message, sizeof(ev.message), "%s ALARM", cfg.name);
          ev.selected_contacts = cfg.selected_contacts;
          ev.valid = true;
          Shared_postNotificationEvent(ev);
          state.alarmSmsSent = true;
        }

        // Trigger relay if configured
        for (size_t r = 0; r < RELAY_OUTPUT_COUNT; ++r) {
          RelayConfig rcfg = {};
          if (Shared_getRelayConfig(r, rcfg) && rcfg.enabled && rcfg.alarm_control_enabled) {
            // Check if this relay is linked to this AI (alarm_source: 1=AI1, 2=AI2)
            if (rcfg.alarm_source == (index + 1) || rcfg.alarm_source == 7) {
              Shared_setRelayState(r, true);
              relayAlarmHeld[r] = true;
              Shared_setRelayTriggerSource(r, RELAY_SOURCE_ALARM);
              Serial.printf("[AI%d] Activating Relay%d due to alarm\n", index + 1, r + 1);
            }
          }
        }
      }
    }
  } else {
    // No alarm condition
    state.alarmTriggerTime = 0;  // Reset alarm trigger timer
    
    if (state.inAlarm) {
      // Currently in alarm, check for return
      bool returnCondition = checkAnalogReturn(engValue, cfg);
      
      if (returnCondition) {
        if (state.returnTriggerTime == 0) {
          state.returnTriggerTime = now;
          Serial.printf("[AI%d] Return condition detected: %.2f %s (TTR: %dms)\n", 
                        index + 1, engValue, cfg.engineering_unit, cfg.ttr_ms);
        } else if (now - state.returnTriggerTime >= cfg.ttr_ms) {
          // TTR expired - clear alarm
          state.inAlarm = false;
          state.alarmSmsSent = false;
          Serial.printf("[AI%d] ALARM CLEARED: %.2f %s\n", 
                        index + 1, engValue, cfg.engineering_unit);
          Shared_setAIAlarmState(index, false);
          Shared_setLastEventTime();
          
          {
            NotificationEvent ev = {};
            ev.source = ALARM_SRC_AI;
            ev.index  = index;
            ev.isAlarm = false;
            ev.value  = engValue;
            strncpy(ev.message, cfg.return_message, sizeof(ev.message) - 1);
            if (ev.message[0] == '\0') snprintf(ev.message, sizeof(ev.message), "%s RETURN TO NORMAL", cfg.name);
            ev.selected_contacts = cfg.selected_contacts;
            ev.valid = true;
            Shared_postNotificationEvent(ev);
          }

          // Deactivate relay if configured
          for (size_t r = 0; r < RELAY_OUTPUT_COUNT; ++r) {
            RelayConfig rcfg = {};
            if (Shared_getRelayConfig(r, rcfg) && rcfg.enabled && rcfg.alarm_control_enabled) {
              if (rcfg.alarm_source == (index + 1) || rcfg.alarm_source == 7) {
                Shared_setRelayState(r, false);
                relayAlarmHeld[r] = false;
                Shared_setRelayTriggerSource(r, RELAY_SOURCE_ALARM);
                Serial.printf("[AI%d] Deactivating Relay%d - alarm cleared\n", index + 1, r + 1);
              }
            }
          }
        }
      } else {
        state.returnTriggerTime = 0;  // Reset return trigger timer
      }
    }
  }
}

// Process digital input
static void processDigitalInput(size_t index) {
  if (index >= DIGITAL_INPUT_COUNT) return;
  if (DI_PIN[index] == 0) {
    // No GPIO assigned — force Normal so dashboard doesn't show stale alarm state
    Shared_writeDigitalInput(index, 0);
    return;
  }
  
  DigitalInputConfig cfg = {};
  if (!Shared_getDigitalInputConfig(index, cfg)) return;
  if (!cfg.enabled) return;
  
  // Read raw digital value
  bool rawValue = digitalRead(DI_PIN[index]) == HIGH;
  
  DigitalInputState &state = diState[index];
  unsigned long now = millis();
  
  // Debouncing logic
  if (rawValue != state.currentRawValue) {
    state.currentRawValue = rawValue;
    state.lastChangeTime = now;
  } else if (now - state.lastChangeTime >= DEBOUNCE_TIME_MS) {
    // Stable for debounce time - update stable value
    if (rawValue != state.lastStableValue) {
      state.lastStableValue = rawValue;
      Serial.printf("[DI%d] State changed: %s (raw: %d)\n", 
                    index + 1, rawValue ? "HIGH" : "LOW", rawValue);
    }
  }
  
  // Determine alarm condition based on NO/NC configuration
  // Switch connection: One terminal to GPIO via 1k, other terminal to 3.3V
  // When switch OPEN: GPIO floats/reads LOW (no connection)
  // When switch CLOSED: GPIO reads HIGH (connected to 3.3V via 1k)
  bool alarmCondition;
  if (cfg.normallyClosed) {
    // NC: Alarm when circuit opens (GPIO goes LOW)
    alarmCondition = state.lastStableValue == false;
  } else {
    // NO: Alarm when circuit closes (GPIO goes HIGH)
    alarmCondition = state.lastStableValue == true;
  }
  
  if (alarmCondition) {
    // Alarm condition present
    if (!state.inAlarm) {
      if (state.alarmTriggerTime == 0) {
        state.alarmTriggerTime = now;
        Serial.printf("[DI%d] Alarm condition detected (TTA: %dms)\n", 
                      index + 1, cfg.tta_ms);
      } else if (now - state.alarmTriggerTime >= cfg.tta_ms) {
        // TTA expired - trigger alarm
        state.inAlarm = true;
        state.returnTriggerTime = 0;
        Serial.printf("[DI%d] ALARM TRIGGERED\n", index + 1);
        Shared_setLastEventTime();

        // Write 1 so dashboard reflects alarm state
        Shared_writeDigitalInput(index, 1);

        if (!state.alarmSmsSent) {
          DigitalInputConfig smsCfg = {};
          Shared_getDigitalInputConfig(index, smsCfg);
          // Reset ACK state for this new alarm
          Shared_setAlarmAck(ALARM_SRC_DI, index, false);
          // Post unified notification event (Modem task handles SMS + voice)
          NotificationEvent ev = {};
          ev.source = ALARM_SRC_DI;
          ev.index  = index;
          ev.isAlarm = true;
          ev.value  = 0.0f;
          strncpy(ev.message, smsCfg.alarm_message, sizeof(ev.message) - 1);
          if (ev.message[0] == '\0') snprintf(ev.message, sizeof(ev.message), "%s ALARM", smsCfg.name);
          ev.selected_contacts = smsCfg.selected_contacts;
          ev.valid = true;
          Shared_postNotificationEvent(ev);
          state.alarmSmsSent = true;
        }

        for (size_t r = 0; r < RELAY_OUTPUT_COUNT; ++r) {
          RelayConfig rcfg = {};
          if (Shared_getRelayConfig(r, rcfg) && rcfg.enabled && rcfg.alarm_control_enabled) {
            if (rcfg.alarm_source == (index + 3) || rcfg.alarm_source == 7) {
              Shared_setRelayState(r, true);
              relayAlarmHeld[r] = true;
              Shared_setRelayTriggerSource(r, RELAY_SOURCE_ALARM);
              Serial.printf("[DI%d] Activating Relay%d due to alarm\n", index + 1, r + 1);
            }
          }
        }
      }
    }
  } else {
    // No alarm condition
    state.alarmTriggerTime = 0;

    if (state.inAlarm) {
      if (state.returnTriggerTime == 0) {
        state.returnTriggerTime = now;
        Serial.printf("[DI%d] Return condition detected (TTR: %dms)\n", 
                      index + 1, cfg.ttr_ms);
      } else if (now - state.returnTriggerTime >= cfg.ttr_ms) {
        // TTR expired - clear alarm
        state.inAlarm = false;
        state.alarmSmsSent = false;
        Serial.printf("[DI%d] ALARM CLEARED\n", index + 1);
        Shared_setLastEventTime();

        // Write 0 so dashboard reflects cleared state
        Shared_writeDigitalInput(index, 0);

        DigitalInputConfig smsCfg = {};
        Shared_getDigitalInputConfig(index, smsCfg);
        {
          NotificationEvent ev = {};
          ev.source = ALARM_SRC_DI;
          ev.index  = index;
          ev.isAlarm = false;
          ev.value  = 0.0f;
          strncpy(ev.message, smsCfg.return_message, sizeof(ev.message) - 1);
          if (ev.message[0] == '\0') snprintf(ev.message, sizeof(ev.message), "%s RETURN TO NORMAL", smsCfg.name);
          ev.selected_contacts = smsCfg.selected_contacts;
          ev.valid = true;
          Shared_postNotificationEvent(ev);
        }

        for (size_t r = 0; r < RELAY_OUTPUT_COUNT; ++r) {
          RelayConfig rcfg = {};
          if (Shared_getRelayConfig(r, rcfg) && rcfg.enabled && rcfg.alarm_control_enabled) {
            if (rcfg.alarm_source == (index + 3) || rcfg.alarm_source == 7) {
              Shared_setRelayState(r, false);
              relayAlarmHeld[r] = false;
              Shared_setRelayTriggerSource(r, RELAY_SOURCE_ALARM);
              Serial.printf("[DI%d] Deactivating Relay%d - alarm cleared\n", index + 1, r + 1);
            }
          }
        }
      }
    }
  }
}

// Update physical relay outputs
static void updateRelayOutputs() {
  for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) {
    RelayConfig rcfg = {};
    Shared_getRelayConfig(i, rcfg);

    // If output is disabled, force OFF regardless of shared state
    if (!rcfg.enabled) {
      if (relayOutputs[i]) {
        relayOutputs[i] = false;
        relayAlarmHeld[i] = false;
        Shared_setRelayState(i, false);
        digitalWrite(DO_PIN[i], HIGH); // Active LOW: HIGH = OFF
        Serial.printf("[DO%d] Forced OFF - output disabled\n", i + 1);
      }
      continue;
    }

    // If alarm control was turned off while relay was held ON by alarm, release it
    if (relayAlarmHeld[i] && !rcfg.alarm_control_enabled) {
      relayAlarmHeld[i] = false;
      Shared_setRelayState(i, false);
      Serial.printf("[DO%d] Released - alarm control disabled\n", i + 1);
    }

    SystemSnapshot snap = Shared_getSnapshot();
    bool desiredState = snap.relayState[i];
    if (relayOutputs[i] != desiredState) {
      relayOutputs[i] = desiredState;
      digitalWrite(DO_PIN[i], desiredState ? LOW : HIGH); // Active LOW
      Serial.printf("[DO%d] Output set to %s (GPIO=%s)\n", i + 1, desiredState ? "ON" : "OFF", desiredState ? "LOW" : "HIGH");
    }
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool IOScanner_init() {
  Serial.println("[IOScanner] Initializing I/O pins...");
  
  // Configure analog inputs
  analogReadResolution(12);  // 12-bit resolution
  analogSetAttenuation(ADC_11db);  // 0-3.3V range
  
  for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) {
    pinMode(AI_PIN[i], INPUT);
    Serial.printf("[IOScanner] AI%d configured on GPIO%d\n", i + 1, AI_PIN[i]);
  }
  
  // Configure digital inputs (external pull via 1k to 3.3V, so use INPUT mode)
  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
    if (DI_PIN[i] != 0) {
      pinMode(DI_PIN[i], INPUT);
      diState[i].currentRawValue = digitalRead(DI_PIN[i]) == HIGH;
      diState[i].lastStableValue = diState[i].currentRawValue;
      diState[i].lastChangeTime = millis();
      Serial.printf("[IOScanner] DI%d configured on GPIO%d (external 1k pull-up to 3.3V)\n", i + 1, DI_PIN[i]);
    }
  }
  
  // Configure digital outputs (relay mock LEDs - ACTIVE LOW)
  for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) {
    pinMode(DO_PIN[i], OUTPUT);
    
    // Both relay outputs start OFF by default
    RelayConfig cfg = {};
    bool initialState = false;
    (void)Shared_getRelayConfig(i, cfg);
    
    // Active LOW: LOW = ON, HIGH = OFF
    digitalWrite(DO_PIN[i], initialState ? LOW : HIGH);
    relayOutputs[i] = initialState;
    Shared_setRelayState(i, initialState);
    Serial.printf("[IOScanner] DO%d (Relay%d) configured on GPIO%d (ACTIVE LOW), initial state: %s (GPIO=%s)\n", 
                  i + 1, i + 1, DO_PIN[i], initialState ? "ON" : "OFF", initialState ? "LOW" : "HIGH");
  }
  
  Serial.println("[IOScanner] I/O initialization complete");
  return true;
}

void IOScanner_taskLoop(void *pvParameters) {
  Serial.println("[IOScanner] Task started");
  
  while (true) {
    // Process all analog inputs
    for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) {
      processAnalogInput(i);
    }
    
    // Process all digital inputs
    for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
      processDigitalInput(i);
    }
    
    // Update physical relay outputs
    updateRelayOutputs();
    
    // Scan interval
    vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
  }
}

void IOScanner_setRelayOutput(size_t index, bool state) {
  if (index >= RELAY_OUTPUT_COUNT) return;
  Shared_setRelayState(index, state);
  Serial.printf("[IOScanner] Manual relay control: Relay%d set to %s\n", 
                index + 1, state ? "ON" : "OFF");
}

bool IOScanner_getRelayOutput(size_t index) {
  if (index >= RELAY_OUTPUT_COUNT) return false;
  SystemSnapshot snap = Shared_getSnapshot();
  return snap.relayState[index];
}

float IOScanner_getAnalogRaw(size_t index) {
  if (index >= ANALOG_INPUT_COUNT) return 0.0f;
  return readAnalogAveraged(index);
}

bool IOScanner_getDigitalRaw(size_t index) {
  if (index >= DIGITAL_INPUT_COUNT || DI_PIN[index] == 0) return false;
  return digitalRead(DI_PIN[index]) == HIGH;
}
