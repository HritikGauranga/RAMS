#include "Shared.h"
#include <LittleFS.h>

const int BUTTON_PIN             = 33;
const int AP_STATUS_LED_PIN      = 4;
const int MODEM_INIT_STATUS_PIN  = 2;
const int MODEM_RX               = 16;
const int MODEM_TX               = 17;
const int MODEM_PWRKEY           = 32;

const unsigned long BUTTON_DEBOUNCE_MS = 100;

static SemaphoreHandle_t stateMutex      = nullptr; // Guards all shared state: registers, message configs, AP mode active, and lastSeen arrays.
static SemaphoreHandle_t filesystemMutex = nullptr; // Guards LittleFS access for config load and AP file upload. Not needed for read-only access from RTU/TCP tasks since they never touch the filesystem directly.
static SemaphoreHandle_t spiMutex        = nullptr; // Protects W5500 SPI access from LittleFS operations during AP web server file serving.

static bool     apModeActive = false;
static uint16_t triggerRegs[MESSAGE_SLOT_COUNT]  = {}; // 
static int16_t  resultRegs[MESSAGE_SLOT_COUNT]   = {};
static int16_t  inputRegs[INPUT_REGISTER_COUNT]  = {
  (int16_t)STATE_READY,
  (int16_t)STATE_IDLE,
  (int16_t)STATE_IDLE,
  (int16_t)STATE_IDLE
};
static MessageConfig messageConfigs[MESSAGE_SLOT_COUNT] = {};
static size_t loadedMessageCount = 0;
static uint16_t truncatedMessageRows[MESSAGE_SLOT_COUNT] = {};
static size_t truncatedMessageRowCount = 0;
static size_t truncatedExtraRowCount = 0;
static uint16_t faultyMessageRows[MESSAGE_SLOT_COUNT] = {};
static size_t faultyMessageRowCount = 0;
static InvalidPhoneWarning invalidPhoneWarnings[MAX_INVALID_PHONE_WARNINGS] = {};
static size_t invalidPhoneWarningCount = 0;
static GatewaySettings gatewaySettings = {
  true,            // useDhcp
  {192,168,8,200}, // staticIp
  {255,255,255,0}, // subnet
  {192,168,8,1},   // gateway
  502,             // tcpPort
  1,               // slaveId
  9600,            // baudRate
  8,               // dataBits
  'N',             // parity
  1                // stopBits
};

// ---------------------------------------------------------------------------
// LastSeen tracking — both arrays are guarded by stateMutex.
// RTU and TCP tasks read/write these via the get/set helpers below.
// Shared_updateLastSeenTriggers() is called from syncTo() after mirroring
// so that neither interface mistakes its own mirror as a new master write.
// ---------------------------------------------------------------------------
static uint16_t rtuLastSeenTriggers[MESSAGE_SLOT_COUNT] = {};
static uint16_t tcpLastSeenTriggers[MESSAGE_SLOT_COUNT] = {};

void Shared_updateRTULastSeenTriggers() { 
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return;
  for (size_t i = 0; i < MESSAGE_SLOT_COUNT; ++i) {
    rtuLastSeenTriggers[i] = triggerRegs[i];
  }
  Shared_unlockState(); 
}

void Shared_updateTCPLastSeenTriggers() {
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return;
  for (size_t i = 0; i < MESSAGE_SLOT_COUNT; ++i) {
    tcpLastSeenTriggers[i] = triggerRegs[i];
  }
  Shared_unlockState();
}

bool Shared_getRTULastSeenTrigger(size_t index, uint16_t &value) {
  if (index >= MESSAGE_SLOT_COUNT) return false;
  if (!Shared_lockState()) return false;
  value = rtuLastSeenTriggers[index];
  Shared_unlockState();
  return true;
}

bool Shared_getTCPLastSeenTrigger(size_t index, uint16_t &value) {
  if (index >= MESSAGE_SLOT_COUNT) return false;
  if (!Shared_lockState()) return false;
  value = tcpLastSeenTriggers[index];
  Shared_unlockState();
  return true;
}

bool Shared_setRTULastSeenTrigger(size_t index, uint16_t value) {
  if (index >= MESSAGE_SLOT_COUNT) return false;
  if (!Shared_lockState()) return false;
  rtuLastSeenTriggers[index] = value;
  Shared_unlockState();
  return true;
}

bool Shared_setTCPLastSeenTrigger(size_t index, uint16_t value) {
  if (index >= MESSAGE_SLOT_COUNT) return false;
  if (!Shared_lockState()) return false;
  tcpLastSeenTriggers[index] = value;
  Shared_unlockState(); 
  return true;
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
static String trimCopy(const String &value) {
  String copy = value;
  copy.trim();
  return copy;
}

// ---------------------------------------------------------------------------
// Phone number validation for CSV upload warnings
// Accepts: pure numbers (10-15 digits) or +<country><number> (E.164 format)
// ---------------------------------------------------------------------------
static bool isValidPhoneFormat(const String &number) {
  if (number.length() == 0) return true;  // Empty is OK (no phone for this slot)
  if (number.length() < 10 || number.length() > 20) return false;

  String trimmed = number;
  trimmed.trim();
  
  if (trimmed.charAt(0) == '+') {
    // E.164 format: +<1-3 digit country><7-12 digit number>
    if (trimmed.length() < 11 || trimmed.length() > 15) return false;
    for (size_t i = 1; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  } else {
    // Pure digits: 10-15 digits
    if (trimmed.length() < 10 || trimmed.length() > 15) return false;
    for (size_t i = 0; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  }
}


// CSV parser

static bool parseMessageLine(const String &line,
                             MessageConfig &config,
                             uint16_t csvRowNumber,
                             uint16_t truncatedRows[MESSAGE_SLOT_COUNT],
                             size_t &truncatedCount,
                             InvalidPhoneWarning localInvalidPhoneWarnings[MAX_INVALID_PHONE_WARNINGS],
                             size_t &localInvalidPhoneWarningCount,
                             uint16_t localFaultyMessageRows[MESSAGE_SLOT_COUNT],
                             size_t &localFaultyMessageCount) {
  int commas[6] = {-1, -1, -1, -1, -1, -1};
  int found = 0;

  for (int i = 0; i < (int)line.length() && found < 6; ++i) {
    if (line.charAt(i) == ',') commas[found++] = i;
  }

  if (found < 6) return false;

  String msgNoStr = trimCopy(line.substring(0, commas[0]));
  int msgNo = msgNoStr.toInt();
  if (msgNo < 1 || msgNo > (int)MESSAGE_SLOT_COUNT) return false;

  memset(&config, 0, sizeof(config));
  config.valid = true;
  config.msgNo = (uint8_t)msgNo;

  for (size_t phoneIndex = 0; phoneIndex < PHONE_SLOTS_PER_MESSAGE; ++phoneIndex) {
    int start = commas[phoneIndex] + 1;
    int end   = commas[phoneIndex + 1];
    String number = trimCopy(line.substring(start, end));
    
    if (number.length() == 0) continue;
    
    // Validate phone format
    if (!isValidPhoneFormat(number)) {
      // Collect invalid phone warning
      if (localInvalidPhoneWarningCount < MAX_INVALID_PHONE_WARNINGS) {
        localInvalidPhoneWarnings[localInvalidPhoneWarningCount].csvRow = csvRowNumber;
        localInvalidPhoneWarnings[localInvalidPhoneWarningCount].msgNo = (uint8_t)msgNo;
        localInvalidPhoneWarnings[localInvalidPhoneWarningCount].phoneColumn = (uint8_t)phoneIndex;
        number.toCharArray(localInvalidPhoneWarnings[localInvalidPhoneWarningCount].invalidNumber, PHONE_NUMBER_LENGTH);
        localInvalidPhoneWarningCount++;
      }
      continue;  // Skip invalid number
    }
    
    number.toCharArray(config.phoneNumbers[phoneIndex], PHONE_NUMBER_LENGTH);
    config.phoneCount++;
  }

  String rawMessage = trimCopy(line.substring(commas[5] + 1));
  String message = "";
  bool messageFaulty = false;

  // Message field handling:
  // - Unquoted message: allowed only when it does not contain commas.
  // - Quoted message: allows commas inside, with doubled quotes ("") as escapes.
  // - Faulty format: keep message blank for this row.
  if (rawMessage.length() > 0) {
    if (rawMessage.charAt(0) == '\"') {
      if (rawMessage.length() >= 2 && rawMessage.charAt(rawMessage.length() - 1) == '\"') {
        message = rawMessage.substring(1, rawMessage.length() - 1);
        message.replace("\"\"", "\"");
      } else {
        message = "";
        messageFaulty = true;
      }
    } else {
      if (rawMessage.indexOf(',') >= 0) {
        message = "";
        messageFaulty = true;
      } else {
        message = rawMessage;
      }
    }
  }

  if (messageFaulty && localFaultyMessageCount < MESSAGE_SLOT_COUNT) {
    localFaultyMessageRows[localFaultyMessageCount++] = csvRowNumber;
  }

  if (message.length() > (MESSAGE_TEXT_LENGTH - 1)) {
    message = message.substring(0, MESSAGE_TEXT_LENGTH - 1);
    if (truncatedCount < MESSAGE_SLOT_COUNT) {
      truncatedRows[truncatedCount++] = csvRowNumber;
    }
  }
  message.toCharArray(config.text, MESSAGE_TEXT_LENGTH);
  return true;
}

static void clearMessageConfig() {
  memset(messageConfigs, 0, sizeof(messageConfigs));
  loadedMessageCount = 0;
  memset(truncatedMessageRows, 0, sizeof(truncatedMessageRows));
  truncatedMessageRowCount = 0;
  truncatedExtraRowCount = 0;
  memset(faultyMessageRows, 0, sizeof(faultyMessageRows));
  faultyMessageRowCount = 0;
  memset(invalidPhoneWarnings, 0, sizeof(invalidPhoneWarnings));
  invalidPhoneWarningCount = 0;
}

static bool parseIPv4(const String &src, uint8_t out[4]) {
  int parts[4] = {0, 0, 0, 0};
  int p = 0;
  String token = "";
  for (size_t i = 0; i < src.length(); ++i) {
    char c = src.charAt(i);
    if (c == '.') {
      if (p > 2 || token.length() == 0) return false;
      parts[p++] = token.toInt();
      token = "";
      continue;
    }
    if (c < '0' || c > '9') return false;
    token += c;
  }
  if (p != 3 || token.length() == 0) return false;
  parts[3] = token.toInt();
  for (int i = 0; i < 4; ++i) {
    if (parts[i] < 0 || parts[i] > 255) return false;
    out[i] = (uint8_t)parts[i];
  }
  return true;
}

static String ipToString(const uint8_t ip[4]) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}


// Lifecycle
void Shared_init() {
  if (stateMutex == nullptr)      stateMutex      = xSemaphoreCreateMutex();
  if (filesystemMutex == nullptr) filesystemMutex = xSemaphoreCreateMutex();
  if (spiMutex == nullptr)        spiMutex        = xSemaphoreCreateMutex();
}


// Mutex helpers
bool Shared_lockState(TickType_t timeout) {
  return stateMutex != nullptr && xSemaphoreTake(stateMutex, timeout) == pdTRUE;
}

void Shared_unlockState() {
  if (stateMutex != nullptr) xSemaphoreGive(stateMutex);
}

bool Shared_lockFileSystem(TickType_t timeout) {
  return filesystemMutex != nullptr && xSemaphoreTake(filesystemMutex, timeout) == pdTRUE;
}

void Shared_unlockFileSystem() {
  if (filesystemMutex != nullptr) xSemaphoreGive(filesystemMutex);
}

bool Shared_lockSPI(TickType_t timeout) {
  return spiMutex != nullptr && xSemaphoreTake(spiMutex, timeout) == pdTRUE;
}

void Shared_unlockSPI() {
  if (spiMutex != nullptr) xSemaphoreGive(spiMutex);
}


// Config load
bool Shared_loadMessageConfig() {
  static MessageConfig parsedConfigs[MESSAGE_SLOT_COUNT];
  uint16_t localTruncatedRows[MESSAGE_SLOT_COUNT] = {};
  size_t localTruncatedCount = 0;
  size_t localExtraRowCount = 0;
  uint16_t localFaultyMessageRows[MESSAGE_SLOT_COUNT] = {};
  size_t localFaultyMessageCount = 0;
  InvalidPhoneWarning localInvalidPhoneWarnings[MAX_INVALID_PHONE_WARNINGS] = {};
  size_t localInvalidPhoneWarningCount = 0;
  size_t parsedCount = 0;
  memset(parsedConfigs, 0, sizeof(parsedConfigs));
  uint16_t csvRowNumber = 1; // Header row
  size_t dataRowCount = 0;

  if (!Shared_lockFileSystem(pdMS_TO_TICKS(2000))) return false;

  File f = LittleFS.open("/MBmapconf.csv", "r");
  if (!f) {
    Shared_unlockFileSystem();
    if (Shared_lockState(pdMS_TO_TICKS(2000))) {
      clearMessageConfig();
      Shared_unlockState();
    }
    return false;
  }

  if (f.available()) f.readStringUntil('\n'); // skip header

  while (f.available()) {
    csvRowNumber++;
    String line = trimCopy(f.readStringUntil('\n'));
    if (line.length() == 0) continue;
    dataRowCount++;
    if (dataRowCount > MESSAGE_SLOT_COUNT) {
      localExtraRowCount++;
      continue;
    }
    MessageConfig config = {};
    if (!parseMessageLine(line, config, csvRowNumber, localTruncatedRows, localTruncatedCount,
                          localInvalidPhoneWarnings, localInvalidPhoneWarningCount,
                          localFaultyMessageRows, localFaultyMessageCount)) continue;
    size_t slot = (size_t)(config.msgNo - 1);
    parsedConfigs[slot] = config;
    parsedCount++;
  }

  f.close();
  Shared_unlockFileSystem();

  if (!Shared_lockState(pdMS_TO_TICKS(2000))) return false;
  memset(messageConfigs, 0, sizeof(messageConfigs));
  memcpy(messageConfigs, parsedConfigs, sizeof(parsedConfigs));
  loadedMessageCount = parsedCount;
  memset(truncatedMessageRows, 0, sizeof(truncatedMessageRows));
  memcpy(truncatedMessageRows, localTruncatedRows, sizeof(localTruncatedRows));
  truncatedMessageRowCount = localTruncatedCount;
  truncatedExtraRowCount = localExtraRowCount;
  memset(faultyMessageRows, 0, sizeof(faultyMessageRows));
  memcpy(faultyMessageRows, localFaultyMessageRows, sizeof(localFaultyMessageRows));
  faultyMessageRowCount = localFaultyMessageCount;
  memset(invalidPhoneWarnings, 0, sizeof(invalidPhoneWarnings));
  memcpy(invalidPhoneWarnings, localInvalidPhoneWarnings, sizeof(localInvalidPhoneWarnings));
  invalidPhoneWarningCount = localInvalidPhoneWarningCount;
  Shared_unlockState();
  return true;
}

size_t Shared_getLoadedMessageCount() {
  size_t count = 0;
  if (Shared_lockState(pdMS_TO_TICKS(100))) {
    count = loadedMessageCount;
    Shared_unlockState();
  }
  return count;
}

bool Shared_getMessageConfig(size_t index, MessageConfig &config) {
  if (index >= MESSAGE_SLOT_COUNT) return false;
  if (!Shared_lockState()) return false;
  config = messageConfigs[index];
  Shared_unlockState();
  return config.valid;
}

String Shared_getTruncatedMessageRowsCSV() {
  String rows = "";
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return rows;
  for (size_t i = 0; i < truncatedMessageRowCount; ++i) {
    if (i > 0) rows += ",";
    rows += String((unsigned int)truncatedMessageRows[i]);
  }
  Shared_unlockState();
  return rows;
}

// ---------------------------------------------------------------------------
// Return invalid phone warnings as JSON array for frontend display
// Format: [{"row":2,"col":"Phone1","value":"+91 123456"},...]
// ---------------------------------------------------------------------------
String Shared_getInvalidPhoneWarningsJSON() {
  String json = "[";
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return json + "]";
  
  const char *phoneColName[] = {"Phone1", "Phone2", "Phone3", "Phone4", "Phone5"};
  
  for (size_t i = 0; i < invalidPhoneWarningCount; ++i) {
    if (i > 0) json += ",";
    json += "{\"row\":" + String((unsigned)invalidPhoneWarnings[i].csvRow)
          + ",\"no\":" + String((unsigned)invalidPhoneWarnings[i].msgNo)
          + ",\"col\":\"" + phoneColName[invalidPhoneWarnings[i].phoneColumn]
          + "\",\"value\":\"" + String(invalidPhoneWarnings[i].invalidNumber) + "\"}";
  }
  
  Shared_unlockState();
  json += "]";
  return json;
}

String Shared_getFaultyMessageRowsCSV() {
  String rows = "";
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return rows;
  for (size_t i = 0; i < faultyMessageRowCount; ++i) {
    if (i > 0) rows += ",";
    rows += String((unsigned int)faultyMessageRows[i]);
  }
  Shared_unlockState();
  return rows;
}

size_t Shared_getTruncatedExtraRowCount() {
  size_t count = 0;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return count;
  count = truncatedExtraRowCount;
  Shared_unlockState();
  return count;
}


// Snapshot & register access

SystemSnapshot Shared_getSnapshot() {
  SystemSnapshot snapshot = {};
  if (!Shared_lockState()) return snapshot;
  snapshot.apModeActive = apModeActive;
  memcpy(snapshot.triggerRegs, triggerRegs, sizeof(triggerRegs));
  memcpy(snapshot.resultRegs,  resultRegs,  sizeof(resultRegs));
  memcpy(snapshot.inputRegs,   inputRegs,   sizeof(inputRegs));
  Shared_unlockState();
  return snapshot;
}

bool Shared_writeTriggerRegister(size_t index, uint16_t value) {
  if (index >= MESSAGE_SLOT_COUNT || !Shared_lockState()) return false;
  triggerRegs[index] = value;
  Shared_unlockState();
  return true;
}

bool Shared_writeResultRegister(size_t index, int16_t value) {
  if (index >= MESSAGE_SLOT_COUNT || !Shared_lockState()) return false;
  resultRegs[index] = value;
  Shared_unlockState();
  return true;
}

bool Shared_writeInputRegister(size_t index, int16_t value) {
  if (index >= INPUT_REGISTER_COUNT || !Shared_lockState()) return false;
  inputRegs[index] = value;
  Shared_unlockState();
  return true;
}

bool Shared_isAPModeActive() {
  bool active = false;
  if (Shared_lockState()) {
    active = apModeActive;
    Shared_unlockState();
  }
  return active;
}

void Shared_setAPModeActive(bool active) {
  if (Shared_lockState()) {
    apModeActive = active;
    Shared_unlockState();
  }
}

uint16_t encodeSignedRegister(int16_t value) {
  return static_cast<uint16_t>(value);
}

bool Shared_loadGatewaySettings() {
  GatewaySettings loaded = gatewaySettings;
  bool found = false;

  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  File f = LittleFS.open("/gateway.conf", "r");
  if (f) {
    found = true;
    while (f.available()) {
      String line = trimCopy(f.readStringUntil('\n'));
      if (line.length() == 0) continue;
      int eq = line.indexOf('=');
      if (eq <= 0) continue;
      String key = trimCopy(line.substring(0, eq));
      String val = trimCopy(line.substring(eq + 1));

      if (key == "use_dhcp") loaded.useDhcp = (val == "1");
      else if (key == "static_ip") parseIPv4(val, loaded.staticIp);
      else if (key == "subnet_mask") parseIPv4(val, loaded.subnetMask);
      else if (key == "gateway_ip") parseIPv4(val, loaded.gatewayIp);
      else if (key == "tcp_port") loaded.tcpPort = (uint16_t)val.toInt();
      else if (key == "slave_id") loaded.slaveId = (uint8_t)val.toInt();
      else if (key == "baud_rate") loaded.baudRate = (uint32_t)val.toInt();
      else if (key == "data_bits") loaded.dataBits = (uint8_t)val.toInt();
      else if (key == "parity" && val.length() > 0) loaded.parity = val.charAt(0);
      else if (key == "stop_bits") loaded.stopBits = (uint8_t)val.toInt();
    }
    f.close();
  }
  Shared_unlockFileSystem();

  if (!found) return true; // keep defaults

  if (!Shared_lockState(pdMS_TO_TICKS(200))) return false;
  gatewaySettings = loaded;
  Shared_unlockState();
  return true;
}


bool Shared_getGatewaySettings(GatewaySettings &settings) {
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  settings = gatewaySettings;
  Shared_unlockState();
  return true;
}

bool Shared_saveGatewaySettings(const GatewaySettings &settings) {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  File f = LittleFS.open("/gateway.conf", "w");
  if (!f) {
    Shared_unlockFileSystem();
    return false;
  }

  f.println(String("use_dhcp=") + (settings.useDhcp ? "1" : "0"));
  f.println(String("static_ip=") + ipToString(settings.staticIp));
  f.println(String("subnet_mask=") + ipToString(settings.subnetMask));
  f.println(String("gateway_ip=") + ipToString(settings.gatewayIp));
  f.println(String("tcp_port=") + String(settings.tcpPort));
  f.println(String("slave_id=") + String(settings.slaveId));
  f.println(String("baud_rate=") + String(settings.baudRate));
  f.println(String("data_bits=") + String(settings.dataBits));
  f.println(String("parity=") + String(settings.parity));
  f.println(String("stop_bits=") + String(settings.stopBits));
  f.close();
  Shared_unlockFileSystem();

  if (!Shared_lockState(pdMS_TO_TICKS(200))) return false;
  gatewaySettings = settings;
  Shared_unlockState();
  return true;
}
