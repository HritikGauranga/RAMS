#include "IOScanner.h"
#include "Shared.h"
#include "Modem.h"
#include <driver/adc.h>
#include <esp_adc_cal.h>

// ---------------------------------------------------------------------------
// GPIO Pin Definitions
// ---------------------------------------------------------------------------
// Analog Inputs (ADC1 only - avoid ADC2 as it conflicts with WiFi)
constexpr int AI_PIN[ANALOG_INPUT_COUNT] = {34, 35};  // GPIO34=ADC1_CH6, GPIO35=ADC1_CH7

// Digital Inputs (switches connected to 3.3V via 1k resistor, no pull-up needed)
constexpr int DI_PIN[DIGITAL_INPUT_COUNT] = {26, 27, 0, 0};  // DI1=GPIO26, DI2=GPIO27, DI3/4=unused for now

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
  return cfg.scale_low + normalized * (cfg.scale_high - cfg.scale_low);
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
  switch (cfg.alarm_type) {
    case 0:  // High alarm
      return value >= cfg.set_point;
    case 1:  // Low alarm
      return value <= cfg.set_point;
    case 2:  // In-Band alarm (alarm if value is BETWEEN set_point and reset_point)
      return (value >= cfg.set_point && value <= cfg.reset_point) ||
             (value >= cfg.reset_point && value <= cfg.set_point);
    case 3:  // Out-of-Band alarm (alarm if value is OUTSIDE set_point and reset_point)
      return (value < cfg.set_point && value < cfg.reset_point) ||
             (value > cfg.set_point && value > cfg.reset_point);
    default:
      return false;
  }
}

// Check if analog value triggers return (alarm clear) based on alarm type
static bool checkAnalogReturn(float value, const AnalogInputConfig &cfg) {
  switch (cfg.alarm_type) {
    case 0:  // High alarm - return when below reset point
      return value < cfg.reset_point;
    case 1:  // Low alarm - return when above reset point
      return value > cfg.reset_point;
    case 2:  // In-Band alarm - return when outside the band
      return !((value >= cfg.set_point && value <= cfg.reset_point) ||
               (value >= cfg.reset_point && value <= cfg.set_point));
    case 3:  // Out-of-Band alarm - return when inside the band
      return (value >= cfg.set_point && value <= cfg.reset_point) ||
             (value >= cfg.reset_point && value <= cfg.set_point);
    default:
      return true;
  }
}

// Send alarm SMS for analog input
static void sendAnalogAlarmSMS(size_t index, const AnalogInputConfig &cfg, float value, bool isAlarm) {
  if (!cfg.enabled) return;
  if (isAlarm && !cfg.alarm_sms_enabled) return;
  if (!isAlarm && !cfg.return_sms_enabled) return;
  
  // Get recipient contacts
  ContactList contacts = {};
  Shared_getRecipientContacts(contacts);
  
  // Build message
  String message = isAlarm ? String(cfg.alarm_message) : String(cfg.return_message);
  String name = String(cfg.name);
  String unit = String(cfg.engineering_unit);
  
  // Replace placeholders if message is default or empty
  if (message.length() == 0) {
    if (isAlarm) {
      message = name + " ALARM: " + String(value, 2) + " " + unit;
    } else {
      message = name + " RETURN: " + String(value, 2) + " " + unit;
    }
  }
  
  // Send to selected contacts
  for (size_t i = 0; i < contacts.count; ++i) {
    if (!contacts.items[i].enabled) continue;
    
    // Check if this contact is selected (bitmask)
    if (cfg.selected_contacts & (1 << i)) {
      String number = String(contacts.items[i].number);
      number.trim();
      if (number.length() > 0) {
        Serial.printf("[AI%d] Sending %s SMS to %s: %s\n", 
                      index + 1, isAlarm ? "ALARM" : "RETURN", 
                      number.c_str(), message.c_str());
        // Note: SMS sending will happen via Modem task queue (to be implemented)
        // For now, just log it
      }
    }
  }
}

// Send alarm SMS for digital input
static void sendDigitalAlarmSMS(size_t index, const DigitalInputConfig &cfg, bool isAlarm) {
  if (!cfg.enabled) return;
  if (isAlarm && !cfg.alarm_sms_enabled) return;
  if (!isAlarm && !cfg.return_sms_enabled) return;
  
  // Get recipient contacts
  ContactList contacts = {};
  Shared_getRecipientContacts(contacts);
  
  // Build message
  String message = isAlarm ? String(cfg.alarm_message) : String(cfg.return_message);
  String name = String(cfg.name);
  
  // Replace placeholders if message is default or empty
  if (message.length() == 0) {
    if (isAlarm) {
      message = name + " ALARM";
    } else {
      message = name + " RETURN TO NORMAL";
    }
  }
  
  // Send to selected contacts
  for (size_t i = 0; i < contacts.count; ++i) {
    if (!contacts.items[i].enabled) continue;
    
    // Check if this contact is selected (bitmask)
    if (cfg.selected_contacts & (1 << i)) {
      String number = String(contacts.items[i].number);
      number.trim();
      if (number.length() > 0) {
        Serial.printf("[DI%d] Sending %s SMS to %s: %s\n", 
                      index + 1, isAlarm ? "ALARM" : "RETURN", 
                      number.c_str(), message.c_str());
        // Note: SMS sending will happen via Modem task queue (to be implemented)
        // For now, just log it
      }
    }
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
        
        // Send alarm SMS
        if (!state.alarmSmsSent) {
          sendAnalogAlarmSMS(index, cfg, engValue, true);
          state.alarmSmsSent = true;
        }
        
        // Update alarm result register
        Shared_writeAlarmResult(index, STATUS_ERROR_SEND);
        
        // Trigger relay if configured
        for (size_t r = 0; r < RELAY_OUTPUT_COUNT; ++r) {
          RelayConfig rcfg = {};
          if (Shared_getRelayConfig(r, rcfg) && rcfg.enabled && rcfg.alarm_control_enabled) {
            // Check if this relay is linked to this AI (alarm_source: 1=AI1, 2=AI2)
            if (rcfg.alarm_source == (index + 1)) {
              Shared_setRelayState(r, true);
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
          
          // Send return SMS
          sendAnalogAlarmSMS(index, cfg, engValue, false);
          
          // Update alarm result register
          Shared_writeAlarmResult(index, STATUS_IDLE);
          
          // Deactivate relay if configured
          for (size_t r = 0; r < RELAY_OUTPUT_COUNT; ++r) {
            RelayConfig rcfg = {};
            if (Shared_getRelayConfig(r, rcfg) && rcfg.enabled && rcfg.alarm_control_enabled) {
              if (rcfg.alarm_source == (index + 1)) {
                Shared_setRelayState(r, false);
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
  if (DI_PIN[index] == 0) return;  // Skip unconfigured inputs
  
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
  
  // Update shared state (0=normal, 1=alarm for display purposes)
  Shared_writeDigitalInput(index, alarmCondition ? 1 : 0);
  
  if (alarmCondition) {
    // Alarm condition present
    if (!state.inAlarm) {
      // Just entered alarm condition
      if (state.alarmTriggerTime == 0) {
        state.alarmTriggerTime = now;
        Serial.printf("[DI%d] Alarm condition detected (TTA: %dms)\n", 
                      index + 1, cfg.tta_ms);
      } else if (now - state.alarmTriggerTime >= cfg.tta_ms) {
        // TTA expired - trigger alarm
        state.inAlarm = true;
        state.returnTriggerTime = 0;
        Serial.printf("[DI%d] ALARM TRIGGERED\n", index + 1);
        
        // Send alarm SMS
        if (!state.alarmSmsSent) {
          sendDigitalAlarmSMS(index, cfg, true);
          state.alarmSmsSent = true;
        }
        
        // Update alarm result register
        Shared_writeAlarmResult(index, STATUS_ERROR_SEND);
        
        // Trigger relay if configured
        for (size_t r = 0; r < RELAY_OUTPUT_COUNT; ++r) {
          RelayConfig rcfg = {};
          if (Shared_getRelayConfig(r, rcfg) && rcfg.enabled && rcfg.alarm_control_enabled) {
            // Check if this relay is linked to this DI (alarm_source: 3=DI1, 4=DI2, 5=DI3, 6=DI4)
            if (rcfg.alarm_source == (index + 3)) {
              Shared_setRelayState(r, true);
              Serial.printf("[DI%d] Activating Relay%d due to alarm\n", index + 1, r + 1);
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
      if (state.returnTriggerTime == 0) {
        state.returnTriggerTime = now;
        Serial.printf("[DI%d] Return condition detected (TTR: %dms)\n", 
                      index + 1, cfg.ttr_ms);
      } else if (now - state.returnTriggerTime >= cfg.ttr_ms) {
        // TTR expired - clear alarm
        state.inAlarm = false;
        state.alarmSmsSent = false;
        Serial.printf("[DI%d] ALARM CLEARED\n", index + 1);
        
        // Send return SMS
        sendDigitalAlarmSMS(index, cfg, false);
        
        // Update alarm result register
        Shared_writeAlarmResult(index, STATUS_IDLE);
        
        // Deactivate relay if configured
        for (size_t r = 0; r < RELAY_OUTPUT_COUNT; ++r) {
          RelayConfig rcfg = {};
          if (Shared_getRelayConfig(r, rcfg) && rcfg.enabled && rcfg.alarm_control_enabled) {
            if (rcfg.alarm_source == (index + 3)) {
              Shared_setRelayState(r, false);
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
    SystemSnapshot snap = Shared_getSnapshot();
    bool desiredState = snap.relayState[i];
    
    if (relayOutputs[i] != desiredState) {
      relayOutputs[i] = desiredState;
      // Active LOW LEDs: LOW = ON, HIGH = OFF
      digitalWrite(DO_PIN[i], desiredState ? LOW : HIGH);
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
    
    // Set initial state based on configuration
    RelayConfig cfg = {};
    bool initialState = false;
    if (Shared_getRelayConfig(i, cfg) && cfg.enabled) {
      initialState = cfg.default_power_up_state;
    }
    
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
