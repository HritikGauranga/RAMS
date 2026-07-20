#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// RAMS data model: 4 digital inputs, 2 analog inputs, 2 relays, phone lists
constexpr size_t DIGITAL_INPUT_COUNT = 4;
constexpr size_t ANALOG_INPUT_COUNT  = 2;
constexpr size_t RELAY_OUTPUT_COUNT  = 2;

constexpr size_t MAX_PHONE_LIST      = 30; // total phone slots for device
constexpr size_t PHONE_NUMBER_LENGTH = 20;
constexpr size_t MAX_PHONE_PER_LIST  = 30;

struct Contact {
  bool enabled;
  char number[PHONE_NUMBER_LENGTH];
  bool sms_enabled;   // receives SMS notifications
  bool call_enabled;  // receives voice call notifications
};

struct ContactList {
  size_t count;
  Contact items[MAX_PHONE_PER_LIST];
};

// Alarm acknowledgement state (per DI/AI input)
enum AlarmSource : uint8_t { ALARM_SRC_DI = 0, ALARM_SRC_AI = 1 };

// Notification event posted by IOScanner, consumed by Modem task
struct NotificationEvent {
  AlarmSource source;   // DI or AI
  size_t      index;    // input index
  bool        isAlarm;  // true=alarm, false=return
  float       value;    // analog value (AI only)
  char        message[64]; // alarm or return message text
  uint32_t    selected_contacts; // bitmask
  bool        valid;
};
constexpr size_t NOTIFICATION_QUEUE_DEPTH = 8;
bool Shared_postNotificationEvent(const NotificationEvent &ev);
bool Shared_takeNotificationEvent(NotificationEvent &out);

// Alarm ACK state
bool Shared_setAlarmAck(AlarmSource src, size_t index, bool acked);
bool Shared_getAlarmAck(AlarmSource src, size_t index, bool &out);

// Voice call settings
struct VoiceCallSettings {
  bool     enabled;
  uint16_t ring_timeout_s;    // seconds to wait for answer (default 30)
  uint16_t inter_call_delay_s; // seconds between calls (default 5)
};
bool Shared_getVoiceCallSettings(VoiceCallSettings &out);
bool Shared_saveVoiceCallSettings(const VoiceCallSettings &cfg);

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
  uint32_t selected_contacts; // bitmask for selected contact recipients
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
  uint32_t selected_contacts;
  float offset;               // Added to engineering value after 4-20mA scaling
};

struct RelayConfig {
  bool enabled;
  char name[32];
  bool default_power_up_state;  // OFF=false, ON=true
  bool sms_control_enabled;
  bool alarm_control_enabled;
  uint8_t alarm_source;  // 0=none, 1-2=AI1-AI2, 3-6=DI1-DI4
  uint32_t selected_contacts; // bitmask for selected contact recipients
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

// AI alarm state (set by IOScanner, read by AP for dashboard)
bool Shared_setAIAlarmState(size_t index, bool inAlarm);
bool Shared_getAIAlarmState(size_t index, bool &out);

// Heartbeat (Status Message) config
struct HeartbeatConfig {
  bool     enabled;
  uint32_t selected_contacts; // bitmask
  uint8_t  frequency;         // 0=once_a_day, 1=twice_a_day, 2=once_a_week
  uint8_t  days_mask;         // bit0=daily, bit1=Mon...bit7=Sun
  uint8_t  time1_h;
  uint8_t  time1_m;
  uint8_t  time2_h;
  uint8_t  time2_m;
};
bool Shared_getHeartbeatConfig(HeartbeatConfig &out);
bool Shared_saveHeartbeatConfig(const HeartbeatConfig &cfg);
// Called periodically by Modem task; returns true once per scheduled minute
bool Shared_tickHeartbeat();

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

