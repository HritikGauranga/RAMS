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

static PhoneList phoneList = {0};

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
  if (number.length() == 0) return true;
  if (number.length() < 10 || number.length() > 20) return false;
  String trimmed = number;
  trimmed.trim();
  if (trimmed.charAt(0) == '+') {
    if (trimmed.length() < 11 || trimmed.length() > 15) return false;
    for (size_t i = 1; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  } else {
    if (trimmed.length() < 10 || trimmed.length() > 15) return false;
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
      else if (key == "http_port") loaded.httpPort = (uint16_t)val.toInt();
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
// Phone list + alarm results
// ---------------------------------------------------------------------------

bool Shared_getPhoneList(PhoneList &out) {
  if (!Shared_lockState(pdMS_TO_TICKS(100))) return false;
  out = phoneList;
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
