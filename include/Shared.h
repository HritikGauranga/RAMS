#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// RAMS data model: 4 digital inputs, 2 analog inputs, 2 relays, phone lists
constexpr size_t DIGITAL_INPUT_COUNT = 4;
constexpr size_t ANALOG_INPUT_COUNT  = 2;
constexpr size_t RELAY_OUTPUT_COUNT  = 2;

constexpr size_t MAX_PHONE_LIST      = 20; // total phone slots for device
constexpr size_t PHONE_NUMBER_LENGTH = 20;
constexpr size_t MAX_PHONE_PER_LIST  = 5;

enum RegisterStatus : int16_t { //this is used for Modbus register status values, and also for internal alarm result storage, since we are not using Modbus RTU or TCP in RAMS we can remove the Modbus dependency and just use these values internally, but this reduces readability of the code since we are not using modbus anymore, but we can keep the same values for compatibility with the existing code, or we can change th
  STATUS_IDLE           =  0,
  STATUS_ERROR_SEND     = -1,
  STATUS_ERROR_SIM      = -2,
  STATUS_ERROR_CONFIG   = -4,
  STATUS_ERROR_EMPTY    = -5,
  STATUS_ERROR_MODEM    = -6
};

enum RuntimeState : int16_t {
  STATE_IDLE = 0,
  STATE_READY   = 1,
  STATE_BUSY    = 2,
  STATE_ERROR   = -1
};

// Compatibility: small input register set used for modem/device status
constexpr size_t DEVICE_STATUS_REGISTER   = 0; // this is used to indicate overall device status (ready, error, etc.), but where is it displays? the answer to this is that it is used in the Modem code to indicate the overall device status, and it is also used in the Shared code to indicate the overall device status, but it is not displayed anywhere in the UI, it is just used internally for status tracking. But "Register" is the term we used 
constexpr size_t MODEM_STATUS_REGISTER    = 1;
constexpr size_t SIM_STATUS_REGISTER      = 2;
constexpr size_t NETWORK_STATUS_REGISTER  = 3;

// Compatibility helper (keeps Modem code working)
bool Shared_writeInputRegister(size_t index, int16_t value);

// Legacy newline phone list removed in favor of Contact/ContactList

struct Contact {
  bool enabled;
  char name[32];
  char number[PHONE_NUMBER_LENGTH];
};

struct ContactList {
  size_t count;
  Contact items[MAX_PHONE_PER_LIST];
};

struct DigitalInputConfig {
  bool enabled;
  bool normallyClosed; // true if NC
  uint32_t tta_ms; // time to alarm
  uint32_t ttr_ms; // time to return
  char name[32];
  bool alarm_sms_enabled;
  bool return_sms_enabled;
  char alarm_message[64];
  char return_message[64];
  uint8_t selected_contacts; // bitmask for selected contact recipients
};

struct AnalogInputConfig {
  bool enabled;
  char name[32];
  char engineering_unit[16];  // e.g., "Liters", "Bar", "%", "°C"
  float scale_low;            // Engineering value at 4mA
  float scale_high;           // Engineering value at 20mA
  uint8_t alarm_type;         // 0=High, 1=Low, 2=In-Band, 3=Out-of-Band
  float set_point;            // Alarm trigger threshold
  float reset_point;          // Alarm clear threshold
  uint32_t tta_ms;            // Time to alarm (milliseconds)
  uint32_t ttr_ms;            // Time to return (milliseconds)
  bool alarm_sms_enabled;
  bool return_sms_enabled;
  char alarm_message[64];
  char return_message[64];
  uint8_t selected_contacts;
  float offset;               // Added to engineering value after 4-20mA scaling
};

struct RelayConfig {
  bool enabled;
  char name[32];
  bool default_power_up_state;  // OFF=false, ON=true
  bool sms_control_enabled;
  bool alarm_control_enabled;
  uint8_t alarm_source;  // 0=none, 1-2=AI1-AI2, 3-6=DI1-DI4
  uint8_t selected_contacts; // bitmask for selected contact recipients
};

struct SIMConfig {
  char service_provider[64];   // e.g., "Vodafone", "AT&T"
  char phone_number[PHONE_NUMBER_LENGTH];  // User's SIM phone number
  char relay_pin[16];          // PIN for SMS relay control
};

struct SystemSnapshot {
  bool apModeActive;
  int16_t digitalInputs[DIGITAL_INPUT_COUNT];
  float   analogInputs[ANALOG_INPUT_COUNT];
  bool    relayState[RELAY_OUTPUT_COUNT];
};

struct GatewaySettings {
  bool    useDhcp;
  uint8_t staticIp[4];
  uint8_t subnetMask[4];
  uint8_t gatewayIp[4];
  uint16_t httpPort;
};

extern const int BUTTON_PIN;
extern const int AP_STATUS_LED_PIN;
extern const int MODEM_INIT_STATUS_PIN;
extern const int MODEM_RX;
extern const int MODEM_TX;
extern const int MODEM_PWRKEY;

extern const unsigned long BUTTON_DEBOUNCE_MS;

// Lifecycle
void Shared_init();

// Mutex helpers
bool Shared_lockState(TickType_t timeout = pdMS_TO_TICKS(50));
void Shared_unlockState();
bool Shared_lockFileSystem(TickType_t timeout = pdMS_TO_TICKS(500));
void Shared_unlockFileSystem();
bool Shared_lockSPI(TickType_t timeout = pdMS_TO_TICKS(10));
void Shared_unlockSPI();

// Config
bool Shared_loadGatewaySettings();
bool Shared_getGatewaySettings(GatewaySettings &settings);
bool Shared_saveGatewaySettings(const GatewaySettings &settings);

// Contact-based API: event recipients (also authorized for SMS relay control)
bool Shared_getRecipientContacts(ContactList &out);
bool Shared_saveRecipientContacts(const ContactList &list);

// Alarm result storage (per-input)
bool Shared_writeAlarmResult(size_t index, int16_t value);

// AI alarm state (set by IOScanner, read by AP for dashboard)
bool Shared_setAIAlarmState(size_t index, bool inAlarm);
bool Shared_getAIAlarmState(size_t index, bool &out);

// AI pending SMS queue (posted by IOScanner, consumed by Modem task)
// Queue depth 4 per AI: alarm and return can both be pending without loss.
struct AIPendingSMS {
  size_t  index;
  bool    isAlarm;
  float   value;
  bool    valid;
};
constexpr size_t AI_SMS_QUEUE_DEPTH = 4;
bool Shared_postAIPendingSMS(size_t index, bool isAlarm, float value);
bool Shared_takeAIPendingSMS(AIPendingSMS &out);

// DI pending SMS queue (posted by IOScanner on alarm/return, consumed by Modem task)
struct DIPendingSMS {
  size_t index;
  bool   isAlarm;
  bool   valid;
};
constexpr size_t DI_SMS_QUEUE_DEPTH = 4;
bool Shared_postDIPendingSMS(size_t index, bool isAlarm);
bool Shared_takeDIPendingSMS(DIPendingSMS &out);

// Heartbeat (Status Message) config
struct HeartbeatConfig {
  bool     enabled;
  uint8_t  selected_contacts; // bitmask
  uint8_t  frequency;         // 0=once_a_day, 1=twice_a_day, 2=once_a_week
  uint8_t  days_mask;         // bit0=daily, bit1=Mon...bit7=Sun
  uint8_t  time1_h;
  uint8_t  time1_m;
  uint8_t  time2_h;
  uint8_t  time2_m;
};
bool Shared_getHeartbeatConfig(HeartbeatConfig &out);
bool Shared_saveHeartbeatConfig(const HeartbeatConfig &cfg);
// Called periodically by Modem task; posts a heartbeat SMS if schedule matches
bool Shared_tickHeartbeat();
bool Shared_takeHeartbeatSMS(); // returns true if a heartbeat SMS is pending

// Device model access
SystemSnapshot Shared_getSnapshot();
bool Shared_writeDigitalInput(size_t index, int16_t value);
bool Shared_writeAnalogInput(size_t index, float value);
bool Shared_setRelayState(size_t index, bool on);

// Input/Output Configuration access
bool Shared_getDigitalInputConfig(size_t index, DigitalInputConfig &out);
bool Shared_saveDigitalInputConfig(size_t index, const DigitalInputConfig &cfg);
bool Shared_getAnalogInputConfig(size_t index, AnalogInputConfig &out);
bool Shared_saveAnalogInputConfig(size_t index, const AnalogInputConfig &cfg);
bool Shared_getRelayConfig(size_t index, RelayConfig &out);
bool Shared_saveRelayConfig(size_t index, const RelayConfig &cfg);

// SIM Configuration
bool Shared_getSIMConfig(SIMConfig &out);
bool Shared_saveSIMConfig(const SIMConfig &cfg);

// AP mode
bool Shared_isAPModeActive();
void Shared_setAPModeActive(bool active);

// Relay trigger source tracking
enum RelayTriggerSource : uint8_t { RELAY_SOURCE_NONE = 0, RELAY_SOURCE_SMS, RELAY_SOURCE_ALARM };
void Shared_setRelayTriggerSource(size_t index, RelayTriggerSource src);
RelayTriggerSource Shared_getRelayTriggerSource(size_t index);

// Last I/O event timestamp (set on DI/AI alarm or return)
void Shared_setLastEventTime();
time_t Shared_getLastEventTime();

// Encoding
uint16_t encodeSignedRegister(int16_t value);
