#include "Shared.h"

static bool gAPModeActive = false;
static bool gLittleFSInitialized = false;

// Define file path constants declared in header
const char *MBMAP_FILE_PATH = "/MBmapconf.csv";
const char *SERIAL_FILE_PATH = "/serialnumber.txt";
const char *SERIAL_META_PATH = "/serial_meta.txt";

bool Shared_lockFileSystem(uint32_t timeout) {
  if (!gLittleFSInitialized) {
    if (!LittleFS.begin()) {
      Serial.println("[LittleFS] begin() failed - attempting format...");
      // Try to format once to recover corrupted filesystem (this will erase files)
      if (!LittleFS.format()) {
        Serial.println("[LittleFS] format() failed. LittleFS unavailable.");
        return false;
      }
      delay(20);
      if (!LittleFS.begin()) {
        Serial.println("[LittleFS] begin() failed after format.");
        return false;
      }
      Serial.println("[LittleFS] formatted and mounted successfully.");
    }
    gLittleFSInitialized = true;
  }
  // Minimal stub: no real locking implemented
  return true;
}

void Shared_unlockFileSystem() {
  // stub
}

bool Shared_getGatewaySettings(GatewaySettings &s) {
  // Provide sensible defaults
  s.useDhcp = true;
  s.staticIp[0] = 192; s.staticIp[1] = 168; s.staticIp[2] = 4; s.staticIp[3] = 90;
  s.subnetMask[0] = 255; s.subnetMask[1] = 255; s.subnetMask[2] = 255; s.subnetMask[3] = 0;
  s.gatewayIp[0] = 192; s.gatewayIp[1] = 168; s.gatewayIp[2] = 4; s.gatewayIp[3] = 1;
  s.tcpPort = 502;
  s.slaveId = 1;
  s.baudRate = 9600;
  s.dataBits = 8;
  s.parity = 'N';
  s.stopBits = 1;
  return true;
}

bool Shared_saveGatewaySettings(const GatewaySettings &s) {
  // stub: do nothing, pretend success
  return true;
}

size_t Shared_getLoadedMessageCount() { return 0; }
bool Shared_getMessageConfig(size_t idx, MessageConfig &out) { (void)idx; (void)out; return false; }
bool Shared_loadMessageConfig() { return false; }
String Shared_getTruncatedMessageRowsCSV() { return String(""); }
String Shared_getFaultyMessageRowsCSV() { return String(""); }
String Shared_getInvalidPhoneWarningsJSON() { return String("[]"); }
size_t Shared_getTruncatedExtraRowCount() { return 0; }

bool Shared_isAPModeActive() { return gAPModeActive; }
void Shared_setAPModeActive(bool active) { gAPModeActive = active; }
