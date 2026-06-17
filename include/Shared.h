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
constexpr size_t MAX_PHONE_PER_LIST  = 10;

enum RegisterStatus : int16_t {
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
constexpr size_t DEVICE_STATUS_REGISTER   = 0;
constexpr size_t MODEM_STATUS_REGISTER    = 1;
constexpr size_t SIM_STATUS_REGISTER      = 2;
constexpr size_t NETWORK_STATUS_REGISTER  = 3;

// Compatibility helper (keeps Modem code working)
bool Shared_writeInputRegister(size_t index, int16_t value);

struct PhoneList {
  size_t count;
  char   numbers[MAX_PHONE_PER_LIST][PHONE_NUMBER_LENGTH];
};

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
  uint16_t tta_ms; // time to alarm
  uint16_t ttr_ms; // time to return
  char name[32];
};

struct AnalogInputConfig {
  bool enabled;
  float scale;
  float alarmHigh;
  float alarmLow;
  char name[32];
};

struct RelayConfig {
  bool enabled;
  char name[32];
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

// Phone list access
bool Shared_getPhoneList(PhoneList &out);
bool Shared_savePhoneList(const PhoneList &list);

// New contact-based API: authorized contacts (can send commands)
bool Shared_getAuthorizedContacts(ContactList &out);
bool Shared_saveAuthorizedContacts(const ContactList &list);

// Event recipients (receive event SMS)
bool Shared_getRecipientContacts(ContactList &out);
bool Shared_saveRecipientContacts(const ContactList &list);

// Alarm result storage (per-input)
bool Shared_writeAlarmResult(size_t index, int16_t value);

// Device model access
SystemSnapshot Shared_getSnapshot();
bool Shared_writeDigitalInput(size_t index, int16_t value);
bool Shared_writeAnalogInput(size_t index, float value);
bool Shared_setRelayState(size_t index, bool on);

// AP mode
bool Shared_isAPModeActive();
void Shared_setAPModeActive(bool active);

// Encoding
uint16_t encodeSignedRegister(int16_t value);
