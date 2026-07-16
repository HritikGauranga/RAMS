#include "Shared.h"
#include "StringUtils.h"
#include "PhoneUtils.h"
#include "NetworkUtils.h"
#include <LittleFS.h>
#include <cstring>
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

static constexpr uint16_t IO_CONFIG_VERSION = 6;
static bool loadIOConfigFromFile();
static bool saveIOConfigToFile();

static GatewaySettings gatewaySettings = {
  true,            // useDhcp
  {0,0,0,0},       // staticIp (user must configure via AP mode)
  {0,0,0,0},       // subnet (user must configure via AP mode)
  {0,0,0,0},       // gateway (user must configure via AP mode)
  80               // httpPort default
};

static bool    aiAlarmState[ANALOG_INPUT_COUNT]  = {false, false};
static RelayTriggerSource relayTriggerSource[RELAY_OUTPUT_COUNT] = {RELAY_SOURCE_NONE, RELAY_SOURCE_NONE};
static time_t  lastEventTime = 0;

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

static bool isValidIOConfigStore(const IOConfigStore &store) {
  return store.magic[0] == 'I' &&
         store.magic[1] == 'O' &&
         store.magic[2] == 'C' &&
         store.magic[3] == 'F' &&
         store.version == IO_CONFIG_VERSION;
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

  // Load persisted phone list
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
                String val = util_trimCopy(obj.substring(colon + 1));
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
                  if (util_isValidPhoneFormat(n)) {
                    n.toCharArray(c.number, PHONE_NUMBER_LENGTH);
                    hasValidNumber = true;
                  }
                }
              }
            }
            int smsIdx = obj.indexOf("\"sms_enabled\"");
            if (smsIdx >= 0) {
              int colon = obj.indexOf(':', smsIdx);
              if (colon >= 0) {
                String val = util_trimCopy(obj.substring(colon + 1));
                c.sms_enabled = val.startsWith("true");
              }
            } else {
              c.sms_enabled = true; // backward compat default
            }
            int callIdx = obj.indexOf("\"call_enabled\"");
            if (callIdx >= 0) {
              int colon = obj.indexOf(':', callIdx);
              if (colon >= 0) {
                String val = util_trimCopy(obj.substring(colon + 1));
                c.call_enabled = val.startsWith("true");
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
      String line = util_trimCopy(f.readStringUntil('\n'));
      if (line.length() == 0) continue;
      int eq = line.indexOf('=');
      if (eq <= 0) continue;
      String key = util_trimCopy(line.substring(0, eq));
      String val = util_trimCopy(line.substring(eq + 1));

      if (key == "use_dhcp") loaded.useDhcp = (val == "1");
      else if (key == "static_ip") { IPAddress ip; if (!util_parseIPv4(val, ip)) return false; loaded.staticIp[0] = ip[0]; loaded.staticIp[1] = ip[1]; loaded.staticIp[2] = ip[2]; loaded.staticIp[3] = ip[3]; }
      else if (key == "subnet_mask") { IPAddress ip; if (!util_parseIPv4(val, ip)) return false; loaded.subnetMask[0] = ip[0]; loaded.subnetMask[1] = ip[1]; loaded.subnetMask[2] = ip[2]; loaded.subnetMask[3] = ip[3]; }
      else if (key == "gateway_ip") { IPAddress ip; if (!util_parseIPv4(val, ip)) return false; loaded.gatewayIp[0] = ip[0]; loaded.gatewayIp[1] = ip[1]; loaded.gatewayIp[2] = ip[2]; loaded.gatewayIp[3] = ip[3]; }
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
  f.println(String("static_ip=") + util_ipToString(IPAddress(settings.staticIp[0], settings.staticIp[1], settings.staticIp[2], settings.staticIp[3])));
  f.println(String("subnet_mask=") + util_ipToString(IPAddress(settings.subnetMask[0], settings.subnetMask[1], settings.subnetMask[2], settings.subnetMask[3])));
  f.println(String("gateway_ip=") + util_ipToString(IPAddress(settings.gatewayIp[0], settings.gatewayIp[1], settings.gatewayIp[2], settings.gatewayIp[3])));
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

bool Shared_saveRecipientContacts(const ContactList &list) {
  ContactList filtered = {};
  for (size_t i = 0; i < list.count; ++i) {
    String num = String(list.items[i].number);
    num.trim();
    if (num.length() == 0) continue;
    if (!util_isValidPhoneFormat(num)) return false;
    if (filtered.count >= MAX_PHONE_PER_LIST) return false;
    filtered.items[filtered.count++] = list.items[i];
  }

  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  File out = LittleFS.open("/contacts.json", "w");
  if (!out) { Shared_unlockFileSystem(); return false; }
  out.print("{\"authorized\":[],\"recipients\":[");
  for (size_t i = 0; i < filtered.count; ++i) {
    if (i) out.print(',');
    String name = util_escapeJson(String(filtered.items[i].name));
    String num = util_escapeJson(String(filtered.items[i].number));
    out.print("{\"enabled\":");
    out.print(filtered.items[i].enabled ? "true" : "false");
    out.print(",\"name\":\""); out.print(name); out.print("\"");
    out.print(",\"number\":\""); out.print(num); out.print("\"");
    out.print(",\"sms_enabled\":"); out.print(filtered.items[i].sms_enabled ? "true" : "false");
    out.print(",\"call_enabled\":"); out.print(filtered.items[i].call_enabled ? "true" : "false");
    out.print("}");
  }
  out.print("]}");
  out.close();
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

void Shared_setRelayTriggerSource(size_t index, RelayTriggerSource src) {
  if (index >= RELAY_OUTPUT_COUNT) return;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return;
  relayTriggerSource[index] = src;
  Shared_unlockState();
}

RelayTriggerSource Shared_getRelayTriggerSource(size_t index) {
  if (index >= RELAY_OUTPUT_COUNT) return RELAY_SOURCE_NONE;
  RelayTriggerSource src = RELAY_SOURCE_NONE;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return src;
  src = relayTriggerSource[index];
  Shared_unlockState();
  return src;
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
  
  String provider = util_escapeJson(String(cfg.service_provider));
  String phone = util_escapeJson(String(cfg.phone_number));
  String pin = util_escapeJson(String(cfg.relay_pin));
  
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

// ---------------------------------------------------------------------------
// Heartbeat (Status Message) config
// ---------------------------------------------------------------------------
static HeartbeatConfig heartbeatConfig = {};
static uint16_t lastHeartbeatMinute = 0xFFFF;

bool Shared_getHeartbeatConfig(HeartbeatConfig &out) {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(500))) return false;
  if (LittleFS.exists("/heartbeat.json")) {
    File f = LittleFS.open("/heartbeat.json", "r");
    if (f) {
      String json = f.readString();
      f.close();
      Shared_unlockFileSystem();
      HeartbeatConfig c = {};
      auto readUint8 = [&](const char *key) -> uint8_t {
        String pat = String('"') + key + '"';
        int ki = json.indexOf(pat);
        if (ki < 0) return 0;
        int ci = json.indexOf(':', ki + pat.length());
        if (ci < 0) return 0;
        return (uint8_t)json.substring(ci + 1).toInt();
      };
      auto readUint32 = [&](const char *key) -> uint32_t {
        String pat = String('"') + key + '"';
        int ki = json.indexOf(pat);
        if (ki < 0) return 0;
        int ci = json.indexOf(':', ki + pat.length());
        if (ci < 0) return 0;
        return (uint32_t)json.substring(ci + 1).toInt();
      };
      auto readBool = [&](const char *key) -> bool {
        String pat = String('"') + key + '"';
        int ki = json.indexOf(pat);
        if (ki < 0) return false;
        int ci = json.indexOf(':', ki + pat.length());
        if (ci < 0) return false;
        String v = json.substring(ci + 1);
        v.trim();
        return v.startsWith("true");
      };
      c.enabled           = readBool("enabled");
      c.selected_contacts = readUint32("selected_contacts");
      c.frequency         = readUint8("frequency");
      c.days_mask         = readUint8("days_mask");
      c.time1_h           = readUint8("time1_h");
      c.time1_m           = readUint8("time1_m");
      c.time2_h           = readUint8("time2_h");
      c.time2_m           = readUint8("time2_m");
      out = c;
      if (Shared_lockState(pdMS_TO_TICKS(50))) { heartbeatConfig = c; Shared_unlockState(); }
      return true;
    }
  }
  Shared_unlockFileSystem();
  out = heartbeatConfig;
  return true;
}

bool Shared_saveHeartbeatConfig(const HeartbeatConfig &cfg) {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  File f = LittleFS.open("/heartbeat.json", "w");
  if (!f) { Shared_unlockFileSystem(); return false; }
  f.printf("{\"enabled\":%s,\"selected_contacts\":%u,\"frequency\":%u,\"days_mask\":%u,"
           "\"time1_h\":%u,\"time1_m\":%u,\"time2_h\":%u,\"time2_m\":%u}",
           cfg.enabled ? "true" : "false",
           cfg.selected_contacts, cfg.frequency, cfg.days_mask,
           cfg.time1_h, cfg.time1_m, cfg.time2_h, cfg.time2_m);
  f.close();
  Shared_unlockFileSystem();
  if (Shared_lockState(pdMS_TO_TICKS(50))) { heartbeatConfig = cfg; Shared_unlockState(); }
  Serial.println("[HEARTBEAT] Config saved");
  return true;
}

bool Shared_tickHeartbeat() {
  HeartbeatConfig cfg = {};
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  cfg = heartbeatConfig;
  Shared_unlockState();

  if (!cfg.enabled || cfg.selected_contacts == 0) return false;

  time_t now;
  time(&now);
  struct tm *ti = localtime(&now);
  if (!ti) return false;

  // tm_wday: 0=Sun,1=Mon...6=Sat  -> map to days_mask bit1=Mon...bit7=Sun, bit0=daily
  // days_mask bit0 = daily, bit1=Mon, bit2=Tue, bit3=Wed, bit4=Thu, bit5=Fri, bit6=Sat, bit7=Sun
  uint8_t wdayBit;
  if (ti->tm_wday == 0) wdayBit = (1 << 7); // Sunday
  else                  wdayBit = (1 << ti->tm_wday); // Mon=bit1 ... Sat=bit6

  bool dayMatch = (cfg.days_mask & 0x01) || (cfg.days_mask & wdayBit); // daily or specific day
  if (!dayMatch) return false;

  // For once_a_week (freq==2), only fire on the single selected day
  // days_mask for once_a_week should have exactly one weekday bit set (no daily bit)

  uint8_t h = (uint8_t)ti->tm_hour;
  uint8_t m = (uint8_t)ti->tm_min;
  uint16_t nowKey = (uint16_t)(h * 60 + m);

  bool fire = false;
  if (h == cfg.time1_h && m == cfg.time1_m) fire = true;
  if (cfg.frequency == 1 && h == cfg.time2_h && m == cfg.time2_m) fire = true;

  if (!fire) {
    // Reset unconditionally — if lock fails we accept the stale value rather
    // than skipping the reset and potentially suppressing the next heartbeat.
    if (Shared_lockState(pdMS_TO_TICKS(50))) {
      lastHeartbeatMinute = 0xFFFF;
      Shared_unlockState();
    }
    return false;
  }

  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  bool alreadyFired = (lastHeartbeatMinute == nowKey);
  if (!alreadyFired) {
    lastHeartbeatMinute = nowKey;
  }
  Shared_unlockState();
  return !alreadyFired;
}


// ---------------------------------------------------------------------------
// Alarm ACK state
// ---------------------------------------------------------------------------
constexpr size_t ALARM_ACK_COUNT = DIGITAL_INPUT_COUNT + ANALOG_INPUT_COUNT;
static bool alarmAckState[ALARM_ACK_COUNT] = {};

static size_t alarmAckIndex(AlarmSource src, size_t index) {
  if (src == ALARM_SRC_DI) return index;
  return DIGITAL_INPUT_COUNT + index;
}

bool Shared_setAlarmAck(AlarmSource src, size_t index, bool acked) {
  size_t i = alarmAckIndex(src, index);
  if (i >= ALARM_ACK_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  alarmAckState[i] = acked;
  Shared_unlockState();
  return true;
}

bool Shared_getAlarmAck(AlarmSource src, size_t index, bool &out) {
  size_t i = alarmAckIndex(src, index);
  if (i >= ALARM_ACK_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  out = alarmAckState[i];
  Shared_unlockState();
  return true;
}

// ---------------------------------------------------------------------------
// Notification event queue (for voice call dispatch)
// ---------------------------------------------------------------------------
static NotificationEvent notifQueue[NOTIFICATION_QUEUE_DEPTH] = {};
static size_t notifQueueHead = 0;
static size_t notifQueueTail = 0;

bool Shared_postNotificationEvent(const NotificationEvent &ev) {
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  size_t next = (notifQueueTail + 1) % NOTIFICATION_QUEUE_DEPTH;
  if (next == notifQueueHead) {
    notifQueueHead = (notifQueueHead + 1) % NOTIFICATION_QUEUE_DEPTH; // drop oldest
  }
  notifQueue[notifQueueTail] = ev;
  notifQueue[notifQueueTail].valid = true;
  notifQueueTail = next;
  Shared_unlockState();
  return true;
}

bool Shared_takeNotificationEvent(NotificationEvent &out) {
  if (!Shared_lockState(pdMS_TO_TICKS(50))) return false;
  if (notifQueueHead == notifQueueTail) {
    Shared_unlockState();
    return false;
  }
  out = notifQueue[notifQueueHead];
  notifQueue[notifQueueHead].valid = false;
  notifQueueHead = (notifQueueHead + 1) % NOTIFICATION_QUEUE_DEPTH;
  Shared_unlockState();
  return true;
}

// ---------------------------------------------------------------------------
// Voice call settings
// ---------------------------------------------------------------------------
static const char *VOICE_CALL_CFG_PATH = "/voicecall.json";

bool Shared_getVoiceCallSettings(VoiceCallSettings &out) {
  out = { false, 30, 5 }; // defaults
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(500))) return true;
  if (!LittleFS.exists(VOICE_CALL_CFG_PATH)) {
    Shared_unlockFileSystem();
    return true;
  }
  File f = LittleFS.open(VOICE_CALL_CFG_PATH, "r");
  if (!f) { Shared_unlockFileSystem(); return true; }
  String json = f.readString();
  f.close();
  Shared_unlockFileSystem();

  auto readBool = [&](const char *key) -> bool {
    String pat = String('"') + key + '"';
    int ki = json.indexOf(pat);
    if (ki < 0) return false;
    int ci = json.indexOf(':', ki + pat.length());
    if (ci < 0) return false;
    String v = json.substring(ci + 1); v.trim();
    return v.startsWith("true");
  };
  auto readUint16 = [&](const char *key) -> uint16_t {
    String pat = String('"') + key + '"';
    int ki = json.indexOf(pat);
    if (ki < 0) return 0;
    int ci = json.indexOf(':', ki + pat.length());
    if (ci < 0) return 0;
    return (uint16_t)json.substring(ci + 1).toInt();
  };
  out.enabled          = readBool("enabled");
  out.ring_timeout_s   = readUint16("ring_timeout_s");
  out.inter_call_delay_s = readUint16("inter_call_delay_s");
  if (out.ring_timeout_s == 0)    out.ring_timeout_s = 30;
  if (out.inter_call_delay_s == 0) out.inter_call_delay_s = 5;
  return true;
}

bool Shared_saveVoiceCallSettings(const VoiceCallSettings &cfg) {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  File f = LittleFS.open(VOICE_CALL_CFG_PATH, "w");
  if (!f) { Shared_unlockFileSystem(); return false; }
  f.printf("{\"enabled\":%s,\"ring_timeout_s\":%u,\"inter_call_delay_s\":%u}",
           cfg.enabled ? "true" : "false",
           cfg.ring_timeout_s, cfg.inter_call_delay_s);
  f.close();
  Shared_unlockFileSystem();
  return true;
}
