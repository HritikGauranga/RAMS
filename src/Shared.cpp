#include "Shared.h"
#include <LittleFS.h>

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

static ContactList authorizedContacts = {0};
static ContactList recipientContacts = {0};

static DigitalInputConfig digitalInputConfig[DIGITAL_INPUT_COUNT] = {};
static AnalogInputConfig  analogInputConfig[ANALOG_INPUT_COUNT]  = {};
static RelayConfig        relayConfig[RELAY_OUTPUT_COUNT]        = {};

static GatewaySettings gatewaySettings = {
  true,            // useDhcp
  {192,168,8,200}, // staticIp
  {255,255,255,0}, // subnet
  {192,168,8,1},   // gateway
  80               // httpPort default
};

static int16_t alarmResults[DIGITAL_INPUT_COUNT] = {0};
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

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Shared_init() {
  if (stateMutex == nullptr)      stateMutex      = xSemaphoreCreateMutex();
  if (filesystemMutex == nullptr) filesystemMutex = xSemaphoreCreateMutex();
  if (spiMutex == nullptr)        spiMutex        = xSemaphoreCreateMutex();
  // Load persisted phone list / contacts if present. Prefer JSON contacts file.
  if (Shared_lockFileSystem(pdMS_TO_TICKS(1000))) {
    // Migration: if old /phones.conf exists, load into recipients list
    if (LittleFS.exists("/phones.conf")) {
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
          // leave name empty
          rec.items[rec.count] = c;
          ++rec.count;
        }
        f.close();
        if (Shared_lockState(pdMS_TO_TICKS(100))) {
          recipientContacts = rec;
          Shared_unlockState();
        }
      }
    }
    // Try loading new contacts.json
    if (LittleFS.exists("/contacts.json")) {
      File f = LittleFS.open("/contacts.json", "r");
      if (f) {
        String json = f.readString();
        f.close();
        // Minimal JSON parsing to avoid adding a JSON library.
        // Expecting: {"authorized":[{...}],"recipients":[{...}]}
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
            // parse fields: enabled, name, number
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
              // Only keep entries that include a valid non-empty phone number
              if (hasValidNumber) {
                list.items[list.count++] = c;
              }
            pos = objEnd + 1;
          }
          return list;
        };

        ContactList auth = extractArray(json, String("authorized"));
        ContactList recs = extractArray(json, String("recipients"));
        if (Shared_lockState(pdMS_TO_TICKS(100))) {
          if (auth.count > 0) authorizedContacts = auth;
          if (recs.count > 0) recipientContacts = recs;
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

bool Shared_getAnalogInputConfig(size_t index, AnalogInputConfig &out) {
  if (index >= ANALOG_INPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  out = analogInputConfig[index];
  Shared_unlockState();
  return true;
}

bool Shared_getRelayConfig(size_t index, RelayConfig &out) {
  if (index >= RELAY_OUTPUT_COUNT) return false;
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  out = relayConfig[index];
  Shared_unlockState();
  return true;
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

bool Shared_getAuthorizedContacts(ContactList &out) {
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  out = authorizedContacts;
  Shared_unlockState();
  return true;
}

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

bool Shared_saveAuthorizedContacts(const ContactList &list) {
  // Filter provided list: drop empty or invalid numbers
  ContactList filtered = {};
  for (size_t i = 0; i < list.count; ++i) {
    String num = String(list.items[i].number);
    num.trim();
    if (num.length() == 0) continue; // skip blanks
    if (!isValidPhoneFormat(num)) return false;
    if (filtered.count >= MAX_PHONE_PER_LIST) return false;
    filtered.items[filtered.count++] = list.items[i];
  }

  // Also ensure recipients we write are filtered (drop blanks)
  ContactList otherFiltered = {};
  for (size_t i = 0; i < recipientContacts.count; ++i) {
    String num = String(recipientContacts.items[i].number);
    num.trim();
    if (num.length() == 0) continue;
    if (!isValidPhoneFormat(num)) continue;
    if (otherFiltered.count >= MAX_PHONE_PER_LIST) break;
    otherFiltered.items[otherFiltered.count++] = recipientContacts.items[i];
  }

  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  File out = LittleFS.open("/contacts.json", "w");
  if (!out) { Shared_unlockFileSystem(); return false; }
  out.print("{");
  out.print("\"authorized\":[");
  for (size_t i = 0; i < filtered.count; ++i) {
    if (i) out.print(',');
    String name = escapeJsonString(String(filtered.items[i].name));
    String num = escapeJsonString(String(filtered.items[i].number));
    out.print("{\"enabled\":"); out.print(filtered.items[i].enabled ? "true" : "false");
    out.print(",\"name\":\""); out.print(name); out.print("\"");
    out.print(",\"number\":\""); out.print(num); out.print("\"}");
  }
  out.print("],\"recipients\":[");
  for (size_t i = 0; i < otherFiltered.count; ++i) {
    if (i) out.print(',');
    String name = escapeJsonString(String(otherFiltered.items[i].name));
    String num = escapeJsonString(String(otherFiltered.items[i].number));
    out.print("{\"enabled\":"); out.print(otherFiltered.items[i].enabled ? "true" : "false");
    out.print(",\"name\":\""); out.print(name); out.print("\"");
    out.print(",\"number\":\""); out.print(num); out.print("\"}");
  }
  out.print("]}");
  out.close();
  Shared_unlockFileSystem();

  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  authorizedContacts = filtered;
  Shared_unlockState();
  return true;
}

bool Shared_saveRecipientContacts(const ContactList &list) {
  // Filter provided list: drop empty or invalid numbers
  ContactList filtered = {};
  for (size_t i = 0; i < list.count; ++i) {
    String num = String(list.items[i].number);
    num.trim();
    if (num.length() == 0) continue;
    if (!isValidPhoneFormat(num)) return false;
    if (filtered.count >= MAX_PHONE_PER_LIST) return false;
    filtered.items[filtered.count++] = list.items[i];
  }

  // Ensure authorized contacts we write are filtered as well
  ContactList otherFiltered = {};
  for (size_t i = 0; i < authorizedContacts.count; ++i) {
    String num = String(authorizedContacts.items[i].number);
    num.trim();
    if (num.length() == 0) continue;
    if (!isValidPhoneFormat(num)) continue;
    if (otherFiltered.count >= MAX_PHONE_PER_LIST) break;
    otherFiltered.items[otherFiltered.count++] = authorizedContacts.items[i];
  }

  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return false;
  File out = LittleFS.open("/contacts.json", "w");
  if (!out) { Shared_unlockFileSystem(); return false; }
  out.print("{");
  out.print("\"authorized\":[");
  for (size_t i = 0; i < otherFiltered.count; ++i) {
    if (i) out.print(',');
    String name = escapeJsonString(String(otherFiltered.items[i].name));
    String num = escapeJsonString(String(otherFiltered.items[i].number));
    out.print("{\"enabled\":"); out.print(otherFiltered.items[i].enabled ? "true" : "false");
    out.print(",\"name\":\""); out.print(name); out.print("\"");
    out.print(",\"number\":\""); out.print(num); out.print("\"}");
  }
  out.print("],\"recipients\":[");
  for (size_t i = 0; i < filtered.count; ++i) {
    if (i) out.print(',');
    String name = escapeJsonString(String(filtered.items[i].name));
    String num = escapeJsonString(String(filtered.items[i].number));
    out.print("{\"enabled\":"); out.print(filtered.items[i].enabled ? "true" : "false");
    out.print(",\"name\":\""); out.print(name); out.print("\"");
    out.print(",\"number\":\""); out.print(num); out.print("\"}");
  }
  out.print("]}");
  out.close();
  Shared_unlockFileSystem();

  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  recipientContacts = filtered;
  Shared_unlockState();
  return true;
}
