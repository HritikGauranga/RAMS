#ifndef SHARED_H
#define SHARED_H

#include <Arduino.h>
#include <LittleFS.h>

// Basic definitions used by AP.cpp
#define BUTTON_PIN 33
#define AP_STATUS_LED_PIN 2
#define BUTTON_DEBOUNCE_MS 500

#define MESSAGE_SLOT_COUNT 150
#define PHONE_SLOTS_PER_MESSAGE 5

extern const char *MBMAP_FILE_PATH;
extern const char *SERIAL_FILE_PATH;
extern const char *SERIAL_META_PATH;

struct MessageConfig {
  uint16_t msgNo;
  char phoneNumbers[PHONE_SLOTS_PER_MESSAGE][32];
  char text[256];
};

struct GatewaySettings {
  bool useDhcp;
  uint8_t staticIp[4];
  uint8_t subnetMask[4];
  uint8_t gatewayIp[4];
  uint16_t tcpPort;
  uint8_t slaveId;
  uint32_t baudRate;
  uint8_t dataBits;
  char parity;
  uint8_t stopBits;
};

bool Shared_lockFileSystem(uint32_t timeout = 0);
void Shared_unlockFileSystem();
bool Shared_getGatewaySettings(GatewaySettings &s);
bool Shared_saveGatewaySettings(const GatewaySettings &s);

size_t Shared_getLoadedMessageCount();
bool Shared_getMessageConfig(size_t idx, MessageConfig &out);
bool Shared_loadMessageConfig();
String Shared_getTruncatedMessageRowsCSV();
String Shared_getFaultyMessageRowsCSV();
String Shared_getInvalidPhoneWarningsJSON();
size_t Shared_getTruncatedExtraRowCount();

bool Shared_isAPModeActive();
void Shared_setAPModeActive(bool active);

#endif // SHARED_H
