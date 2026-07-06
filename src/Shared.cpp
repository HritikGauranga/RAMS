#include "Shared.h"
#include <LittleFS.h>
#include <time.h>

const int BUTTON_PIN             = 33;
const int AP_STATUS_LED_PIN      = 4;
const int MODEM_INIT_STATUS_PIN  = 2;
const int MODEM_RX               = 16;
const int MODEM_TX               = 17;
const int MODEM_PWRKEY           = 32;

const unsigned long BUTTON_DEBOUNCE_MS = 100;

static SemaphoreHandle_t stateMutex      = nullptr; // Guards all shared state.
static SemaphoreHandle_t filesystemMutex = nullptr; // Guards LittleFS access.
static SemaphoreHandle_t spiMutex        = nullptr; // Protects W5500 SPI access.

static bool apModeActive = false;

// Device runtime snapshot storage
static int16_t digitalInputs[DIGITAL_INPUT_COUNT] = {0};
static float   analogInputs[ANALOG_INPUT_COUNT]   = {0.0f};
static bool    relayStates[RELAY_OUTPUT_COUNT]    = {false, false};

static ContactList recipientContacts = {0};

static DigitalInputConfig digitalInputConfig[DIGITAL_INPUT_COUNT] = {};
static AnalogInputConfig  analogInputConfig[ANALOG_INPUT_COUNT]  = {};
static RelayConfig        relayConfig[RELAY_OUTPUT_COUNT]        = {};

static const char *IO_CONFIG_PATH = "/io_config.bin";

struct IOConfigStore {
  char magic[4];
  uint16_t version;
  DigitalInputConfig digital[DIGITAL_INPUT_COUNT];
  AnalogInputConfig analog[ANALOG_INPUT_COUNT];
  RelayConfig relay[RELAY_OUTPUT_COUNT];
};

struct IOConfigStoreV2 {
  char magic[4];
  uint16_t version;
  DigitalInputConfig digital[DIGITAL_INPUT_COUNT];
  AnalogInputConfig analog[ANALOG_INPUT_COUNT];
  struct RelayConfigV2 {
    bool enabled;
    char name[32];
    bool default_power_up_state;
    bool sms_control_enabled;
    bool alarm_control_enabled;
    uint8_t alarm_source;
  } relay[RELAY_OUTPUT_COUNT];
};

static constexpr uint16_t IO_CONFIG_VERSION = 3;
static bool loadIOConfigFromFile();
static bool saveIOConfigToFile();

static bool writeLegacyPhonesFile(const ContactList &list) {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  if (list.count == 0) {
    LittleFS.remove("/phones.conf");
    Shared_unlockFileSystem();
    return true;
  }

  File out = LittleFS.open("/phones.conf", "w");
  if (!out) {
    Shared_unlockFileSystem();
    return false;
  }
  for (size_t i = 0; i < list.count; ++i) {
    if (list.items[i].number[0] != '\0') {
      out.println(String(list.items[i].number));
    }
  }
  out.close();
  Shared_unlockFileSystem();
  return true;
}

static GatewaySettings gatewaySettings = {
  true,            // useDhcp
  {192,168,8,200}, // staticIp
  {255,255,255,0}, // subnet
  {192,168,8,1},   // gateway
  80               // httpPort default
};

static int16_t alarmResults[DIGITAL_INPUT_COUNT] = {0};
static bool    aiAlarmState[ANALOG_INPUT_COUNT]  = {false, false};
static time_t  lastEventTime = 0;
static int16_t inputRegsCompat[4] = {
  (int16_t)STATE_READY,
  (int16_t)STATE_IDLE,
  (int16_t)STATE_IDLE,
  (int16_t)STATE_IDLE
};

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
static String trimCopy(const String &value) {
  String copy = value;
  copy.trim();
  return copy;
}

static bool isValidPhoneFormat(const String &number) {
  String trimmed = number;
  trimmed.trim();
  if (trimmed.length() == 0) return false;
  if (trimmed.length() > PHONE_NUMBER_LENGTH - 1) return false;
  if (trimmed.charAt(0) == '+') {
    size_t digitCount = trimmed.length() - 1;
    if (digitCount < 10 || digitCount > 15) return false;
    for (size_t i = 1; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  } else {
    size_t digitCount = trimmed.length();
    if (digitCount < 10 || digitCount > 15) return false;
    for (size_t i = 0; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  }
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

static bool isValidIOConfigStore(const IOConfigStore &store) {
  return store.magic[0] == 'I' &&
         store.magic[1] == 'O' &&
         store.magic[2] == 'C' &&
         store.magic[3] == 'F' &&
         store.version == IO_CONFIG_VERSION;
}

static bool isValidLegacyIOConfigStore(const IOConfigStoreV2 &store) {
  return store.magic[0] == 'I' &&
         store.magic[1] == 'O' &&
         store.magic[2] == 'C' &&
         store.magic[3] == 'F' &&
         store.version == 2;
}

static bool loadIOConfigFromFile() {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  if (!LittleFS.exists(IO_CONFIG_PATH)) {
    Shared_unlockFileSystem();
    Serial.println("[IOCFG] No persisted config file, using defaults");
    return true;
  }

  File f = LittleFS.open(IO_CONFIG_PATH, "r");
  if (!f) {
    Shared_unlockFileSystem();
    Serial.println("[IOCFG] Failed to open config file for reading");
    return false;
  }

  size_t fileSize = f.size();
  Serial.printf("[IOCFG] Config file size: %u bytes (current: %u bytes, legacy: %u bytes)\n",
                fileSize, (unsigned int)sizeof(IOConfigStore), (unsigned int)sizeof(IOConfigStoreV2));

  bool loaded = false;
  if (fileSize == sizeof(IOConfigStore)) {
    IOConfigStore store = {};
    size_t readLen = f.readBytes(reinterpret_cast<char *>(&store), sizeof(store));
    f.close();
    Shared_unlockFileSystem();

    Serial.printf("[IOCFG] Read %u bytes from file\n", readLen);
    if (readLen != sizeof(store)) {
      Serial.printf("[IOCFG] Size mismatch: read %u bytes but expected %u bytes\n", readLen, (unsigned int)sizeof(store));
      return false;
    }

    if (!isValidIOConfigStore(store)) {
      Serial.printf("[IOCFG] Invalid magic or version - Magic: %c%c%c%c, Version: %u (expected %u)\n",
                    store.magic[0], store.magic[1], store.magic[2], store.magic[3],
                    store.version, IO_CONFIG_VERSION);
      return false;
    }

    if (!Shared_lockState(pdMS_TO_TICKS(200))) return false;
    for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) digitalInputConfig[i] = store.digital[i];
    for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) analogInputConfig[i] = store.analog[i];
    for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) relayConfig[i] = store.relay[i];
    Shared_unlockState();
    loaded = true;
  } else if (fileSize == sizeof(IOConfigStoreV2)) {
    IOConfigStoreV2 legacyStore = {};
    size_t readLen = f.readBytes(reinterpret_cast<char *>(&legacyStore), sizeof(legacyStore));
    f.close();
    Shared_unlockFileSystem();

    Serial.printf("[IOCFG] Read %u bytes from legacy config file\n", readLen);
    if (readLen != sizeof(legacyStore)) {
      Serial.printf("[IOCFG] Legacy size mismatch: read %u bytes but expected %u bytes\n", readLen, (unsigned int)sizeof(legacyStore));
      return false;
    }

    if (!isValidLegacyIOConfigStore(legacyStore)) {
      Serial.printf("[IOCFG] Invalid legacy magic or version - Magic: %c%c%c%c, Version: %u\n",
                    legacyStore.magic[0], legacyStore.magic[1], legacyStore.magic[2], legacyStore.magic[3],
                    legacyStore.version);
      return false;
    }

    if (!Shared_lockState(pdMS_TO_TICKS(200))) return false;
    for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) digitalInputConfig[i] = legacyStore.digital[i];
    for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) analogInputConfig[i] = legacyStore.analog[i];
    for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) {
      relayConfig[i].enabled = legacyStore.relay[i].enabled;
      memcpy(relayConfig[i].name, legacyStore.relay[i].name, sizeof(relayConfig[i].name));
      relayConfig[i].default_power_up_state = legacyStore.relay[i].default_power_up_state;
      relayConfig[i].sms_control_enabled = legacyStore.relay[i].sms_control_enabled;
      relayConfig[i].alarm_control_enabled = legacyStore.relay[i].alarm_control_enabled;
      relayConfig[i].alarm_source = legacyStore.relay[i].alarm_source;
      relayConfig[i].selected_contacts = 0;
    }
    Shared_unlockState();
    loaded = true;
  } else {
    f.close();
    Shared_unlockFileSystem();
    Serial.println("[IOCFG] Unsupported config file layout, using defaults");
    return false;
  }

  if (loaded) {
    Serial.println("[IOCFG] Loaded persisted IO config");
    return true;
  }
  return false;
}

static bool saveIOConfigToFile() {
  IOConfigStore store = {};
  store.magic[0] = 'I';
  store.magic[1] = 'O';
  store.magic[2] = 'C';
  store.magic[3] = 'F';
  store.version = IO_CONFIG_VERSION;

  if (!Shared_lockState(pdMS_TO_TICKS(200))) return false;
  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) store.digital[i] = digitalInputConfig[i];
  for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) store.analog[i] = analogInputConfig[i];
  for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) store.relay[i] = relayConfig[i];
  Shared_unlockState();

  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) {
    Serial.println("[IOCFG] Failed to acquire filesystem lock for writing");
    return false;
  }

  File f = LittleFS.open(IO_CONFIG_PATH, "w");
  if (!f) {
    Shared_unlockFileSystem();
    Serial.println("[IOCFG] Failed to open config file for writing");
    return false;
  }

  size_t written = f.write(reinterpret_cast<const uint8_t *>(&store), sizeof(store));
  f.close();
  Shared_unlockFileSystem();

  if (written != sizeof(store)) {
    Serial.printf("[IOCFG] Write incomplete: wrote %u bytes but expected %u bytes\n", written, (unsigned int)sizeof(store));
    return false;
  }

  Serial.printf("[IOCFG] Successfully saved IO config (%u bytes) to %s\n", (unsigned int)sizeof(store), IO_CONFIG_PATH);
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Shared_init() {
  Serial.println("[SHARED] Initializing Shared state...");
  if (stateMutex == nullptr)      stateMutex      = xSemaphoreCreateMutex();
  if (filesystemMutex == nullptr) filesystemMutex = xSemaphoreCreateMutex();
  if (spiMutex == nullptr)        spiMutex        = xSemaphoreCreateMutex();

  loadIOConfigFromFile();

  // Load persisted phone list / contacts if present. Prefer JSON contacts file.
  if (Shared_lockFileSystem(pdMS_TO_TICKS(1000))) {
    bool loadedFromJson = false;
    if (LittleFS.exists("/contacts.json")) {
      File f = LittleFS.open("/contacts.json", "r");
      if (f) {
        String json = f.readString();
        f.close();
        auto extractArray = [&](const String &root, const String &name) {
          ContactList list = {0};
          int idx = root.indexOf('"' + name + '"');
          if (idx < 0) return list;
          int a = root.indexOf('[', idx);
          if (a < 0) return list;
          int b = root.indexOf(']', a);
          if (b < 0) return list;
          String arr = root.substring(a + 1, b);
          int pos = 0;
          while (pos < arr.length() && list.count < MAX_PHONE_PER_LIST) {
            int objStart = arr.indexOf('{', pos);
            if (objStart < 0) break;
            int objEnd = arr.indexOf('}', objStart);
            if (objEnd < 0) break;
            String obj = arr.substring(objStart + 1, objEnd);
            Contact c = {};
            int enIdx = obj.indexOf("\"enabled\"");
            if (enIdx >= 0) {
              int colon = obj.indexOf(':', enIdx);
              if (colon >= 0) {
                String val = trimCopy(obj.substring(colon + 1));
                if (val.startsWith("true")) c.enabled = true;
                else c.enabled = false;
              }
            }
            int nameIdx = obj.indexOf("\"name\"");
            if (nameIdx >= 0) {
              int colon = obj.indexOf(':', nameIdx);
              if (colon >= 0) {
                int q1 = obj.indexOf('"', colon + 1);
                int q2 = obj.indexOf('"', q1 + 1);
                if (q1 >= 0 && q2 >= 0) {
                  String n = obj.substring(q1 + 1, q2);
                  n.trim();
                  n.toCharArray(c.name, sizeof(c.name));
                }
              }
            }
            int numIdx = obj.indexOf("\"number\"");
            bool hasValidNumber = false;
            if (numIdx >= 0) {
              int colon = obj.indexOf(':', numIdx);
              if (colon >= 0) {
                int q1 = obj.indexOf('"', colon + 1);
                int q2 = obj.indexOf('"', q1 + 1);
                if (q1 >= 0 && q2 >= 0) {
                  String n = obj.substring(q1 + 1, q2);
                  n.trim();
                  if (isValidPhoneFormat(n)) {
                    n.toCharArray(c.number, PHONE_NUMBER_LENGTH);
                    hasValidNumber = true;
                  }
                }
              }
            }
            if (hasValidNumber) {
              list.items[list.count++] = c;
            }
            pos = objEnd + 1;
          }
          return list;
        };

        ContactList recs = extractArray(json, String("recipients"));
        if (recs.count > 0) {
          if (Shared_lockState(pdMS_TO_TICKS(100))) {
            recipientContacts = recs;
            Shared_unlockState();
          }
          loadedFromJson = true;
        }
      }
    }

    if (!loadedFromJson && LittleFS.exists("/phones.conf")) {
      File f = LittleFS.open("/phones.conf", "r");
      if (f) {
        ContactList rec = {0};
        while (f.available() && rec.count < MAX_PHONE_PER_LIST) {
          String line = trimCopy(f.readStringUntil('\n'));
          if (line.length() == 0) continue;
          if (!isValidPhoneFormat(line)) continue;
          Contact c = {};
          c.enabled = true;
          line.toCharArray(c.number, PHONE_NUMBER_LENGTH);
          rec.items[rec.count] = c;
          ++rec.count;
        }
        f.close();
        if (rec.count > 0 && Shared_lockState(pdMS_TO_TICKS(100))) {
          recipientContacts = rec;
          Shared_unlockState();
        }
      }
    }

    Shared_unlockFileSystem();
  }
}

// ---------------------------------------------------------------------------
// Mutex helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Config load
// ---------------------------------------------------------------------------
bool Shared_loadMessageConfig() {
  // Message CSV support removed for RAMS. No-op loader.
  return true;
}

// CSV/message-related accessors removed
size_t Shared_getLoadedMessageCount() { return 0; }

// ---------------------------------------------------------------------------
// Snapshot & register access
// ---------------------------------------------------------------------------
SystemSnapshot Shared_getSnapshot() {
  SystemSnapshot snapshot = {};
  if (!Shared_lockState()) return snapshot;
  snapshot.apModeActive = apModeActive;
  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) snapshot.digitalInputs[i] = digitalInputs[i];
  for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i)  snapshot.analogInputs[i] = analogInputs[i];
  for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i)  snapshot.relayState[i]   = relayStates[i];
  Shared_unlockState();
  return snapshot;
}

bool Shared_writeDigitalInput(size_t index, int16_t value) {
  if (index >= DIGITAL_INPUT_COUNT || !Shared_lockState()) return false;
  digitalInputs[index] = value;
  Shared_unlockState();
  return true;
}

bool Shared_writeAnalogInput(size_t index, float value) {
  if (index >= ANALOG_INPUT_COUNT || !Shared_lockState()) return false;
  analogInputs[index] = value;
  Shared_unlockState();
  return true;
}

bool Shared_setRelayState(size_t index, bool on) {
  if (index >= RELAY_OUTPUT_COUNT || !Shared_lockState()) return false;
  relayStates[index] = on;
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

// ---------------------------------------------------------------------------
// Input/Output Configuration access
// ---------------------------------------------------------------------------
bool Shared_getDigitalInputConfig(size_t index, DigitalInputConfig &out) {
  if (index >= DIGITAL_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  out = digitalInputConfig[index];
  Shared_unlockState();
  return true;
}

bool Shared_saveDigitalInputConfig(size_t index, const DigitalInputConfig &cfg) {
  if (index >= DIGITAL_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  digitalInputConfig[index] = cfg;
  Shared_unlockState();
  return saveIOConfigToFile();
}

bool Shared_getAnalogInputConfig(size_t index, AnalogInputConfig &out) {
  if (index >= ANALOG_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  out = analogInputConfig[index];
  Shared_unlockState();
  return true;
}

bool Shared_saveAnalogInputConfig(size_t index, const AnalogInputConfig &cfg) {
  if (index >= ANALOG_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  analogInputConfig[index] = cfg;
  Shared_unlockState();
  return saveIOConfigToFile();
}

bool Shared_getRelayConfig(size_t index, RelayConfig &out) {
  if (index >= RELAY_OUTPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  out = relayConfig[index];
  Shared_unlockState();
  return true;
}

bool Shared_saveRelayConfig(size_t index, const RelayConfig &cfg) {
  if (index >= RELAY_OUTPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  relayConfig[index] = cfg;
  Shared_unlockState();
  return saveIOConfigToFile();
}

// ---------------------------------------------------------------------------
// Gateway settings (read/write)
// ---------------------------------------------------------------------------
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
      else if (key == "http_port") {
        int port = val.toInt();
        loaded.httpPort = (port >= 1 && port <= 65535) ? (uint16_t)port : 80;
      }
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
  f.println(String("http_port=") + String(settings.httpPort));
  f.close();
  Shared_unlockFileSystem();

  if (!Shared_lockState(pdMS_TO_TICKS(200))) return false;
  gatewaySettings = settings;
  Shared_unlockState();
  return true;
}

// ---------------------------------------------------------------------------
// Contact lists + alarm results
// ---------------------------------------------------------------------------

bool Shared_getRecipientContacts(ContactList &out) {
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  out = recipientContacts;
  Shared_unlockState();
  return true;
}

bool Shared_writeAlarmResult(size_t index, int16_t value) {
  if (index >= DIGITAL_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  alarmResults[index] = value;
  Shared_unlockState();
  return true;
}

bool Shared_setAIAlarmState(size_t index, bool inAlarm) {
  if (index >= ANALOG_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  aiAlarmState[index] = inAlarm;
  Shared_unlockState();
  return true;
}

bool Shared_getAIAlarmState(size_t index, bool &out) {
  if (index >= ANALOG_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  out = aiAlarmState[index];
  Shared_unlockState();
  return true;
}

// ---------------------------------------------------------------------------
// AI pending SMS queue
// ---------------------------------------------------------------------------
static AIPendingSMS aiPendingQueue[ANALOG_INPUT_COUNT * AI_SMS_QUEUE_DEPTH] = {};
static size_t aiQueueHead = 0;
static size_t aiQueueTail = 0;
constexpr size_t AI_QUEUE_SIZE = ANALOG_INPUT_COUNT * AI_SMS_QUEUE_DEPTH;

bool Shared_postAIPendingSMS(size_t index, bool isAlarm, float value) {
  if (index >= ANALOG_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  size_t nextTail = (aiQueueTail + 1) % AI_QUEUE_SIZE;
  if (nextTail == aiQueueHead) {
    // Queue full — drop oldest
    aiQueueHead = (aiQueueHead + 1) % AI_QUEUE_SIZE;
  }
  aiPendingQueue[aiQueueTail] = { index, isAlarm, value, true };
  aiQueueTail = nextTail;
  Shared_unlockState();
  return true;
}

bool Shared_takeAIPendingSMS(AIPendingSMS &out) {
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  if (aiQueueHead == aiQueueTail) {
    Shared_unlockState();
    return false;
  }
  out = aiPendingQueue[aiQueueHead];
  aiPendingQueue[aiQueueHead].valid = false;
  aiQueueHead = (aiQueueHead + 1) % AI_QUEUE_SIZE;
  Shared_unlockState();
  return true;
}

// ---------------------------------------------------------------------------
// DI pending SMS queue
// ---------------------------------------------------------------------------
static DIPendingSMS diPendingQueue[DIGITAL_INPUT_COUNT * DI_SMS_QUEUE_DEPTH] = {};
static size_t diQueueHead = 0;
static size_t diQueueTail = 0;
constexpr size_t DI_QUEUE_SIZE = DIGITAL_INPUT_COUNT * DI_SMS_QUEUE_DEPTH;

bool Shared_postDIPendingSMS(size_t index, bool isAlarm) {
  if (index >= DIGITAL_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  size_t nextTail = (diQueueTail + 1) % DI_QUEUE_SIZE;
  if (nextTail == diQueueHead) {
    // Queue full — drop oldest
    diQueueHead = (diQueueHead + 1) % DI_QUEUE_SIZE;
  }
  diPendingQueue[diQueueTail] = { index, isAlarm, true };
  diQueueTail = nextTail;
  Shared_unlockState();
  return true;
}

bool Shared_takeDIPendingSMS(DIPendingSMS &out) {
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  if (diQueueHead == diQueueTail) {
    Shared_unlockState();
    return false;
  }
  out = diPendingQueue[diQueueHead];
  diPendingQueue[diQueueHead].valid = false;
  diQueueHead = (diQueueHead + 1) % DI_QUEUE_SIZE;
  Shared_unlockState();
  return true;
}

bool Shared_writeInputRegister(size_t index, int16_t value) {
  if (index >= 4) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  inputRegsCompat[index] = value;
  Shared_unlockState();
  return true;
}

static String escapeJsonString(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    if (c == '"' || c == '\\') {
      out += '\\'; out += c;
    } else {
      out += c;
    }
  }
  return out;
}

bool Shared_saveRecipientContacts(const ContactList &list) {
  ContactList filtered = {};
  for (size_t i = 0; i < list.count; ++i) {
    String num = String(list.items[i].number);
    num.trim();
    if (num.length() == 0) continue;
    if (!isValidPhoneFormat(num)) return false;
    if (filtered.count >= MAX_PHONE_PER_LIST) return false;
    filtered.items[filtered.count++] = list.items[i];
  }

  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  File out = LittleFS.open("/contacts.json", "w");
  if (!out) { Shared_unlockFileSystem(); return false; }
  out.print("{\"authorized\":[],\"recipients\":[");
  for (size_t i = 0; i < filtered.count; ++i) {
    if (i) out.print(',');
    String name = escapeJsonString(String(filtered.items[i].name));
    String num = escapeJsonString(String(filtered.items[i].number));
    out.print("{\"enabled\":");
    out.print(filtered.items[i].enabled ? "true" : "false");
    out.print(",\"name\":\""); out.print(name); out.print("\"");
    out.print(",\"number\":\""); out.print(num); out.print("\"}");
  }
  out.print("]}");
  out.close();
  writeLegacyPhonesFile(filtered);
  Shared_unlockFileSystem();

  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  recipientContacts = filtered;
  Shared_unlockState();
  return true;
}

void Shared_setLastEventTime() {
  time_t now;
  time(&now);
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return;
  lastEventTime = now;
  Shared_unlockState();
}

time_t Shared_getLastEventTime() {
  time_t t = 0;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return t;
  t = lastEventTime;
  Shared_unlockState();
  return t;
}

// ---------------------------------------------------------------------------
// SIM Configuration
// ---------------------------------------------------------------------------
bool Shared_getSIMConfig(SIMConfig &out) {
  out = {};
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(500))) return true; // return defaults if can't lock
  
  if (LittleFS.exists("/sim_config.json")) {
    File f = LittleFS.open("/sim_config.json", "r");
    if (f) {
      String json = f.readString();
      f.close();
      
      // Parse JSON: {"service_provider":"...", "phone_number":"...", "relay_pin":"..."}
      int provIdx = json.indexOf("\"service_provider\"");
      if (provIdx >= 0) {
        int q1 = json.indexOf('"', provIdx + 19);
        int q2 = json.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 >= 0) {
          String provider = json.substring(q1 + 1, q2);
          provider.toCharArray(out.service_provider, sizeof(out.service_provider));
        }
      }
      
      int phoneIdx = json.indexOf("\"phone_number\"");
      if (phoneIdx >= 0) {
        int q1 = json.indexOf('"', phoneIdx + 14);
        int q2 = json.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 >= 0) {
          String phone = json.substring(q1 + 1, q2);
          phone.toCharArray(out.phone_number, sizeof(out.phone_number));
        }
      }
      
      int pinIdx = json.indexOf("\"relay_pin\"");
      if (pinIdx >= 0) {
        int q1 = json.indexOf('"', pinIdx + 12);
        int q2 = json.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 >= 0) {
          String pin = json.substring(q1 + 1, q2);
          pin.toCharArray(out.relay_pin, sizeof(out.relay_pin));
        }
      }
    }
  }
  
  Shared_unlockFileSystem();
  return true;
}

bool Shared_saveSIMConfig(const SIMConfig &cfg) {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  
  File f = LittleFS.open("/sim_config.json", "w");
  if (!f) {
    Shared_unlockFileSystem();
    return false;
  }
  
  String provider = escapeJsonString(String(cfg.service_provider));
  String phone = escapeJsonString(String(cfg.phone_number));
  String pin = escapeJsonString(String(cfg.relay_pin));
  
  f.print("{");
  f.print("\"service_provider\":\""); f.print(provider); f.print("\",");
  f.print("\"phone_number\":\""); f.print(phone); f.print("\",");
  f.print("\"relay_pin\":\""); f.print(pin); f.print("\"");
  f.print("}");
  f.close();
  Shared_unlockFileSystem();
  
  Serial.println("[SIM] SIM configuration saved");
  return true;
}
