#include "Modem.h"
#include "Shared.h"
#include "CallManager.h"
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <ETH.h>

HardwareSerial SerialAT(1);
static bool modemReady = false;
static uint8_t consecutiveModemHealthFailures = 0;
static unsigned long lastReinitAttemptMs = 0;
static unsigned long lastNotReadyLogMs = 0;
static bool simMissingLatched = false;
static unsigned long lastSimRecheckMs = 0;
static unsigned long lastSmsCheckMs = 0;
static volatile bool modemSerialBusy = false;
static volatile int8_t cachedRssi = -1;
static unsigned long lastRssiUpdateMs = 0;


// ---------------------------------------------------------------------------
// Serial AT helpers
// ---------------------------------------------------------------------------
static String readSerialATResponse(unsigned long timeout) {
  String response = "";
  unsigned long start      = millis();
  unsigned long lastByteAt = start;

  while (millis() - start < timeout) {
    while (SerialAT.available()) {
      response += (char)SerialAT.read();
      lastByteAt = millis();
      delay(2);
    }
    if (response.length() > 0 && (millis() - lastByteAt) > 100) break;
    delay(10);
  }

  response.trim();
  return response;
}

static void setModemInitStatusLED(bool ready) {
  digitalWrite(MODEM_INIT_STATUS_PIN, ready ? HIGH : LOW);
}

static void setModemReady(bool ready) {
  modemReady = ready;
  setModemInitStatusLED(ready);
}

static bool sendSMS(const String &number, const String &message);

// Normalize to a modem-safe destination:
// - keeps a single leading '+' (E.164)
// - allows digits only after optional '+'
// - rejects any separators/spaces inside the number
static bool normalizePhoneNumber(const String &input, String &normalized) {
  normalized = "";
  String number = input;
  number.trim();
  if (number.length() == 0) return false;

  bool plusSeen = false;
  for (size_t i = 0; i < number.length(); ++i) {
    char c = number.charAt(i);
    if (c == '+') {
      if (i != 0 || plusSeen) return false;
      plusSeen = true;
      normalized += c;
      continue;
    }
    if (c >= '0' && c <= '9') {
      normalized += c;
      continue;
    }
    return false;
  }

  size_t digitStart = (normalized.length() > 0 && normalized.charAt(0) == '+') ? 1 : 0;
  size_t digitCount = normalized.length() - digitStart;
  return digitCount >= 10 && digitCount <= 15;
}

// ---------------------------------------------------------------------------
// AT command (with optional silent mode)
// ---------------------------------------------------------------------------
static String sendAT(const String &cmd, int timeout, bool silent = false) {
  while (SerialAT.available()) SerialAT.read();

  if (!silent) {
    Serial.println("[AT] >> " + cmd);
  }
  SerialAT.println(cmd);
  delay(100);

  String response = readSerialATResponse((unsigned long)timeout);
  if (!silent) {
    Serial.println(response.length() ? "[AT] << " + response : "[AT] << [NO RESPONSE]");
  }
  return response;
}

struct PulseSlot {
  size_t relayIdx;
  unsigned long offAtMs;
};
static PulseSlot pulseSlots[RELAY_OUTPUT_COUNT];

static void resetPulseSlots() {
  for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) {
    pulseSlots[i].relayIdx = (size_t)-1;
    pulseSlots[i].offAtMs = 0;
  }
}

static void schedulePulse(size_t relayIdx, unsigned long seconds) {
  for (auto &p : pulseSlots) {
    if (p.relayIdx == relayIdx && p.offAtMs > 0) {
      p.offAtMs = millis() + seconds * 1000;
      return;
    }
  }
  for (auto &p : pulseSlots) {
    if (p.relayIdx == (size_t)-1) {
      p.relayIdx = relayIdx;
      p.offAtMs = millis() + seconds * 1000;
      return;
    }
  }
}

static void checkPulses() {
  unsigned long now = millis();
  for (auto &p : pulseSlots) {
    if (p.offAtMs > 0 && now >= p.offAtMs) {
      Shared_setRelayState(p.relayIdx, false);
      Serial.printf("[PULSE] Relay %u auto OFF\n", (unsigned)p.relayIdx);
      p.relayIdx = (size_t)-1;
      p.offAtMs = 0;
    }
  }
}

static bool isAuthorizedSender(const String &sender) {
  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec)) return false;
  for (size_t i = 0; i < rec.count; ++i) {
    if (!rec.items[i].enabled) continue;
    String num = String(rec.items[i].number);
    if (sender == num || sender == "+" + num) {
      return true;
    }
    if (num.startsWith("+")) num = num.substring(1);
    if (sender == num || sender == "+" + num) {
      return true;
    }
  }
  return false;
}

static String readTextFileValue(const char *path, const String &fallback) {
  String value = fallback;
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return value;
  
  if (!LittleFS.exists(path)) {
    Shared_unlockFileSystem();
    return fallback;
  }
  
  File f = LittleFS.open(path, "r");
  if (f) {
    value = f.readString();
    f.close();
  }
  Shared_unlockFileSystem();
  value.trim();
  return value.length() > 0 ? value : fallback;
}

static String readSystemConfigValue(const String &key, const String &fallback) {
  String value = fallback;
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(1000))) return value;
  
  if (!LittleFS.exists("/system_config.json")) {
    Shared_unlockFileSystem();
    return fallback;
  }
  
  File f = LittleFS.open("/system_config.json", "r");
  if (f) {
    String json = f.readString();
    f.close();
    String pattern = "\"" + key + "\"";
    int keyPos = json.indexOf(pattern);
    if (keyPos >= 0) {
      int colonPos = json.indexOf(':', keyPos + pattern.length());
      if (colonPos >= 0) {
        int q1 = json.indexOf('"', colonPos + 1);
        int q2 = json.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 > q1) value = json.substring(q1 + 1, q2);
      }
    }
  }
  Shared_unlockFileSystem();
  value.trim();
  return value.length() > 0 ? value : fallback;
}

static String formatUptime(unsigned long ms) {
  unsigned long seconds = ms / 1000;
  unsigned long days = seconds / 86400;
  unsigned long hours = (seconds % 86400) / 3600;
  unsigned long mins = (seconds % 3600) / 60;
  String out = "";
  if (days > 0) {
    out += String(days) + (days == 1 ? " day" : " days");
  }
  if (hours > 0) {
    if (out.length() > 0) out += " ";
    out += String(hours) + (hours == 1 ? " hr" : " hrs");
  }
  if (mins > 0 || out.length() == 0) {
    if (out.length() > 0) out += " ";
    out += String(mins) + (mins == 1 ? " min" : " mins");
  }
  return out.length() > 0 ? out : "0 mins";
}

static String mapSignalStrength(int8_t rssi) {
  if (rssi == -2) return "SIM Not Inserted";
  if (rssi < 0) return "Unknown";
  if (rssi >= 25) return "Excellent";
  if (rssi >= 20) return "Very Good";
  if (rssi >= 15) return "Good";
  if (rssi >= 10) return "Fair";
  if (rssi >= 5) return "Weak";
  return "Very Weak";
}

static String ipBytesToString(const uint8_t ip[4]) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

static String buildSMSHeader() {
  String siteName = readSystemConfigValue("site_name", "Not Set");
  return "Site: " + siteName;
}

static String buildSystemStatusSMS() {
  String siteName = readSystemConfigValue("site_name", "Not Set");
  String siteAddress = readSystemConfigValue("site_address", "");
  String location = siteAddress.length() > 0 ? siteAddress : siteName;
  if (location.length() == 0) location = "Not Set";

  String serial = readTextFileValue("/serialnumber.txt", "Not Set");
  if (serial.length() == 0) serial = "Not Set";

  SystemSnapshot snapshot = Shared_getSnapshot();
  size_t activeAlarmCount = 0;
  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
    if (snapshot.digitalInputs[i] != 0) ++activeAlarmCount;
  }

  int8_t rssi = Modem_getSignalStrength();
  String signal = mapSignalStrength(rssi);
  String relayState = "R1=" + String(snapshot.relayState[0] ? "ON" : "OFF") + ", R2=" + String(snapshot.relayState[1] ? "ON" : "OFF");

  String msg = "Location: " + location + "\n";
  msg += "Site Name: " + siteName + "\n";
  msg += "Serial No.: " + serial + "\n";
  msg += "Uptime: " + formatUptime(millis()) + "\n";
  msg += "Active Alarms: " + String((unsigned)activeAlarmCount) + "\n";
  msg += "Signal Strength: " + signal + "\n";
  msg += "Relays: " + relayState;
  return msg;
}

static String buildInputStatusSMS() {
  SystemSnapshot snap = Shared_getSnapshot();
  String msg = "[Digital Inputs]\n";
  msg += buildSMSHeader() + "\n";
  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
    DigitalInputConfig cfg = {};
    Shared_getDigitalInputConfig(i, cfg);
    bool inAlarm = snap.digitalInputs[i] != 0;
    String name = String(cfg.name);
    bool nameDefined = name.length() > 0;
    if (!nameDefined)
        name = "--";
    msg += "DI" + String(i + 1) + "(" + name + "): "
        + String(nameDefined ? (inAlarm ? "Alarm" : "Normal") : "Not Defined")
        + "\n";
}
  msg += "[Analog Inputs]\n";
  for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) {
    AnalogInputConfig cfg = {};
    Shared_getAnalogInputConfig(i, cfg);

    bool inAlarm = false;
    Shared_getAIAlarmState(i, inAlarm);

    String name = String(cfg.name);
    bool nameDefined = name.length() > 0;

    if (!nameDefined)
        name = "--";
    msg += "AI" + String(i + 1) + "(" + name + "): ";
    if (!nameDefined) {
        msg += "Not Defined\n";
    } else {
        msg += String(snap.analogInputs[i], 2);
        if (cfg.enabled && cfg.engineering_unit[0] != '\0')
            msg += " " + String(cfg.engineering_unit);
        msg += ", " + String(inAlarm ? "Alarm" : "Normal") + "\n";
    }
}
  return msg;
}

static String buildRelayStatusSMS() {
  SystemSnapshot snap = Shared_getSnapshot();
  String msg = "[Relay Outputs]\n";
  msg += buildSMSHeader() + "\n";
  for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) {
    RelayConfig cfg = {};
    Shared_getRelayConfig(i, cfg);
    String name = String(cfg.name);
    bool nameDefined = name.length() > 0;
    if (!nameDefined)
    name = "--";
    RelayTriggerSource src = Shared_getRelayTriggerSource(i);
    String prefix = "";
    if (src == RELAY_SOURCE_SMS) prefix = "S-";
    else if (src == RELAY_SOURCE_ALARM) prefix = "A-";
    if (!nameDefined) {
      msg += prefix + "DO" + String(i + 1) + "(" + name + "): Not Defined";
    } else {
      msg += prefix + "DO" + String(i + 1) + "(" + name + "): "
         + String(snap.relayState[i] ? "ON" : "OFF");
    }
    if (i + 1 < RELAY_OUTPUT_COUNT) msg += "\n";
  }
  return msg;
}

static String buildAlarmStatusSMS() {
  SystemSnapshot snap = Shared_getSnapshot();
  String msg = "[Active Alarms]\n";
  msg += buildSMSHeader() + "\n";
  bool any = false;
  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
    if (snap.digitalInputs[i] == 0) continue;
    DigitalInputConfig cfg = {};
    Shared_getDigitalInputConfig(i, cfg);
    String name = String(cfg.name);
    if (name.length() == 0) name = "DI" + String(i + 1);
    msg += name + ": Alarm\n";
    any = true;
  }
  for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) {
    bool inAlarm = false;
    Shared_getAIAlarmState(i, inAlarm);
    if (!inAlarm) continue;
    AnalogInputConfig cfg = {};
    Shared_getAnalogInputConfig(i, cfg);
    String name = String(cfg.name);
    if (name.length() == 0) name = "AI" + String(i + 1);
    msg += name + ": " + String(snap.analogInputs[i], 2);
    if (cfg.engineering_unit[0] != '\0') msg += " " + String(cfg.engineering_unit);
    msg += ", Alarm\n";
    any = true;
  }
  if (!any) msg += "No Active Alarms";
  return msg;
}

static void processAlarmRequest(const String &sender) {
  if (!isAuthorizedSender(sender)) {
    Serial.println("[SMS] Unauthorized sender for ALARM request: " + sender);
    return;
  }
  String msg = buildAlarmStatusSMS();
  if (!sendSMS(sender, msg)) {
    Serial.println("[SMS] Failed to send alarm status SMS to " + sender);
  }
}

static void processInputRequest(const String &sender) {
  if (!isAuthorizedSender(sender)) {
    Serial.println("[SMS] Unauthorized sender for INPUT request: " + sender);
    return;
  }
  String msg = buildInputStatusSMS();
  if (!sendSMS(sender, msg)) {
    Serial.println("[SMS] Failed to send input status SMS to " + sender);
  }
}

static void processRelayStatusRequest(const String &sender) {
  if (!isAuthorizedSender(sender)) {
    Serial.println("[SMS] Unauthorized sender for RELAY request: " + sender);
    return;
  }
  String msg = buildRelayStatusSMS();
  if (!sendSMS(sender, msg)) {
    Serial.println("[SMS] Failed to send relay status SMS to " + sender);
  }
}

static void processStatusRequest(const String &sender) {
  if (!isAuthorizedSender(sender)) {
    Serial.println("[SMS] Unauthorized sender for status request: " + sender);
    return;
  }

  String msg = buildSystemStatusSMS();
  if (!sendSMS(sender, msg)) {
    Serial.println("[SMS] Failed to send status SMS to " + sender);
  }
}

static String buildIpStatusSMS() {
  GatewaySettings settings = {};
  Shared_getGatewaySettings(settings);

  String ipAddress;
  if (settings.useDhcp) {
    IPAddress localIp = ETH.localIP();
    if (localIp[0] != 0 || localIp[1] != 0 || localIp[2] != 0 || localIp[3] != 0) {
      ipAddress = localIp.toString();
    }
  }
  if (ipAddress.length() == 0) {
    ipAddress = ipBytesToString(settings.staticIp);
  }

  String gateway = ipBytesToString(settings.gatewayIp);
  String dhcp = settings.useDhcp ? "Enabled" : "Disabled";

  String msg = buildSMSHeader() + "\n";
  msg += "IP Address: " + ipAddress + "\n";
  msg += "DHCP: " + dhcp + "\n";
  msg += "Gateway: " + gateway;
  return msg;
}

// static void processIpRequest(const String &sender, const String &body) {
//   if (!isAuthorizedSender(sender)) {
//     Serial.println("[SMS] Unauthorized sender for IP request: " + sender);
//     return;
//   }

//   String upperBody = body;
//   upperBody.toUpperCase();
//   int cmdIdx = upperBody.indexOf("GET IP%");
//   if (cmdIdx < 0) {
//     Serial.println("[SMS] Invalid IP request format from: " + sender);
//     return;
//   }

//   String pin = body.substring(cmdIdx + 7);
//   int newline = pin.indexOf('\n');
//   if (newline >= 0) pin = pin.substring(0, newline);
//   pin.trim();

//   SIMConfig simCfg = {};
//   Shared_getSIMConfig(simCfg);
//   String storedPin = String(simCfg.relay_pin);
//   storedPin.trim();

//   if (storedPin.length() == 0 || pin != storedPin) {
//     Serial.println("[SMS] Invalid PIN for IP request from: " + sender);
//     return;
//   }

//   String msg = buildIpStatusSMS();
//   if (!sendSMS(sender, msg)) {
//     Serial.println("[SMS] Failed to send IP SMS to " + sender);
//   }
// }

static bool splitCommandLine(const String &line, String parts[], size_t maxParts, size_t &outCount) {
  outCount = 0;
  String trimmed = line;
  trimmed.trim();
  int len = trimmed.length();
  int start = 0;
  while (start < len && outCount < maxParts) {
    int pos = trimmed.indexOf('%', start);
    if (pos < 0) {
      parts[outCount++] = trimmed.substring(start);
      return true;
    }
    parts[outCount++] = trimmed.substring(start, pos);
    start = pos + 1;
  }
  return true;
}

static bool parseInputIdentifier(const String &token, bool &isAnalog, size_t &index) {
  String upper = token;
  upper.toUpperCase();
  if (upper.startsWith("DI")) {
    String num = upper.substring(2);
    if (num.length() == 0) return false;
    index = (size_t)num.toInt();
    if (index == 0 || index > DIGITAL_INPUT_COUNT) return false;
    isAnalog = false;
    index -= 1;
    return true;
  }
  if (upper.startsWith("AI")) {
    String num = upper.substring(2);
    if (num.length() == 0) return false;
    index = (size_t)num.toInt();
    if (index == 0 || index > ANALOG_INPUT_COUNT) return false;
    isAnalog = true;
    index -= 1;
    return true;
  }
  return false;
}

static String inputIdentifierToString(bool isAnalog, size_t index) {
  return String(isAnalog ? "AI" : "DI") + String((unsigned)(index + 1));
}

static size_t countBitmaskOnes(uint32_t mask) {
  size_t count = 0;
  while (mask) {
    count += (mask & 1);
    mask >>= 1;
  }
  return count;
}

static bool findContactIndexByNumber(const String &number, size_t &outIndex) {
  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec)) return false;
  String trimmed = number;
  trimmed.trim();
  for (size_t i = 0; i < rec.count; ++i) {
    if (trimmed == String(rec.items[i].number)) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

static bool updateContactNotificationFlags(size_t contactIndex, bool enableVoice, bool enableSms) {
  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec)) return false;
  if (contactIndex >= rec.count) return false;

  // Explicitly apply BOTH flags so that missing %V/%S clears previous settings.
  rec.items[contactIndex].call_enabled = enableVoice;
  rec.items[contactIndex].sms_enabled  = enableSms;

  return Shared_saveRecipientContacts(rec);
}


static bool validatePin(const String &pin) {
  SIMConfig simCfg = {};
  Shared_getSIMConfig(simCfg);
  String storedPin = String(simCfg.relay_pin);
  storedPin.trim();
  return storedPin.length() > 0 && pin == storedPin;
}

static String buildListResponse(bool isAnalog, size_t index) {
  String header = inputIdentifierToString(isAnalog, index);
  String out;

  if (isAnalog) {
    AnalogInputConfig cfg = {};
    if (!Shared_getAnalogInputConfig(index, cfg)) return "ERROR: Unable to read input config";
    size_t assigned = countBitmaskOnes(cfg.selected_contacts);
    out = header + " Contacts " + String((unsigned)assigned) + "/5\n\n";
    if (assigned == 0) {
      out += "No contacts assigned";
      return out;
    }
    ContactList rec = {};
    if (!Shared_getRecipientContacts(rec)) {
      out += "No contacts assigned";
      return out;
    }
    size_t lineNo = 0;
    for (size_t i = 0; i < rec.count && lineNo < 5; ++i) {
      if (!(cfg.selected_contacts & (1UL << i))) continue;
      ++lineNo;
      out += String(lineNo) + ". " + String(rec.items[i].name) + "\n";
      out += "   " + String(rec.items[i].number) + "\n";
      out += "   Voice: " + String(rec.items[i].call_enabled ? "Yes" : "No") + "\n";
      out += "   SMS: " + String(rec.items[i].sms_enabled ? "Yes" : "No");
      if (lineNo < assigned) out += "\n\n";
    }
    return out;
  }

  DigitalInputConfig cfg = {};
  if (!Shared_getDigitalInputConfig(index, cfg)) return "ERROR: Unable to read input config";
  size_t assigned = countBitmaskOnes(cfg.selected_contacts);
  out = header + " Contacts " + String((unsigned)assigned) + "/5\n\n";
  if (assigned == 0) {
    out += "No contacts assigned";
    return out;
  }
  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec)) {
    out += "No contacts assigned";
    return out;
  }
  size_t lineNo = 0;
  for (size_t i = 0; i < rec.count && lineNo < 5; ++i) {
    if (!(cfg.selected_contacts & (1UL << i))) continue;
    ++lineNo;
    out += String(lineNo) + ". " + String(rec.items[i].name) + "\n";
    out += "   " + String(rec.items[i].number) + "\n";
    out += "   Voice: " + String(rec.items[i].call_enabled ? "Yes" : "No") + "\n";
    out += "   SMS: " + String(rec.items[i].sms_enabled ? "Yes" : "No");
    if (lineNo < assigned) out += "\n\n";
  }
  return out;
}

static bool processContactAssignmentCommand(const String &sender, const String &body) {
  String line = body;
  int nl = line.indexOf('\n');
  if (nl >= 0) line = line.substring(0, nl);
  line.trim();
  if (line.length() == 0) return false;

  const size_t MAX_PARTS = 8;
  String parts[MAX_PARTS] = {};
  size_t partCount = 0;
  splitCommandLine(line, parts, MAX_PARTS, partCount);
  if (partCount == 0) return false;

  String cmd = parts[0];
  cmd.toUpperCase();

  if (cmd == "LST") {
    if (partCount != 2) {
      sendSMS(sender, "ERROR: Invalid command format");
      return true;
    }
    bool isAnalog;
    size_t inputIndex;
    if (!parseInputIdentifier(parts[1], isAnalog, inputIndex)) {
      sendSMS(sender, "ERROR: Invalid input identifier");
      return true;
    }
    String response = buildListResponse(isAnalog, inputIndex);
    sendSMS(sender, response);
    return true;
  }

  if (cmd == "DEL") {
    if (partCount != 4) return false; // not a DEL command for us
    String pin = parts[1];
    String number = parts[2];
    String inputToken = parts[3];
    pin.trim(); number.trim(); inputToken.trim();
    if (!validatePin(pin)) {
      sendSMS(sender, "ERROR: Invalid PIN");
      return true;
    }
    bool isAnalog;
    size_t inputIndex;
    if (!parseInputIdentifier(inputToken, isAnalog, inputIndex)) {
      sendSMS(sender, "ERROR: Invalid input identifier");
      return true;
    }
    size_t contactIndex;
    if (!findContactIndexByNumber(number, contactIndex)) {
      return true; // silently ignore unknown contacts
    }
    if (isAnalog) {
      AnalogInputConfig cfg = {};
      if (!Shared_getAnalogInputConfig(inputIndex, cfg)) return true;
      if (cfg.selected_contacts & (1UL << contactIndex)) {
        cfg.selected_contacts &= ~(1UL << contactIndex);
        Shared_saveAnalogInputConfig(inputIndex, cfg);
      }
    } else {
      DigitalInputConfig cfg = {};
      if (!Shared_getDigitalInputConfig(inputIndex, cfg)) return true;
      if (cfg.selected_contacts & (1UL << contactIndex)) {
        cfg.selected_contacts &= ~(1UL << contactIndex);
        Shared_saveDigitalInputConfig(inputIndex, cfg);
      }
    }
    return true;
  }

  if (cmd == "ADD") {
    if (partCount < 5) {
      sendSMS(sender, "ERROR: Invalid command format");
      return true;
    }
    String pin = parts[1];
    String number = parts[2];
    String inputToken = parts[3];
    pin.trim(); number.trim(); inputToken.trim();
    bool voiceFlag = false;
    bool smsFlag = false;
  for (size_t i = 4; i < partCount; ++i) {
    String flag = parts[i];
    flag.trim();
    flag.toUpperCase();
    for (size_t j = 0; j < flag.length(); ++j) {
      char c = flag.charAt(j);
      if (c == 'V') voiceFlag = true;
      else if (c == 'S') smsFlag = true;
    }
  }
  if (!voiceFlag && !smsFlag) {
    sendSMS(sender, "ERROR: Invalid command format");
    return true;
  }

  // IMPORTANT: updateContactNotificationFlags() currently only sets bits to true,
  // but does not clear the other mode. That makes %V unable to uncheck %S in UI.
  // So we apply clear+set here by writing both flags.

    if (!validatePin(pin)) {
      sendSMS(sender, "ERROR: Invalid PIN");
      return true;
    }
    size_t contactIndex;
    if (!findContactIndexByNumber(number, contactIndex)) {
      sendSMS(sender, "ERROR: Contact does not exist in Contact List");
      return true;
    }
    bool isAnalog;
    size_t inputIndex;
    if (!parseInputIdentifier(inputToken, isAnalog, inputIndex)) {
      sendSMS(sender, "ERROR: Invalid input identifier");
      return true;
    }
    if (isAnalog) {
      AnalogInputConfig cfg = {};
      if (!Shared_getAnalogInputConfig(inputIndex, cfg)) {
        sendSMS(sender, "ERROR: Unable to read input config");
        return true;
      }
      bool alreadyAssigned = (cfg.selected_contacts & (1UL << contactIndex)) != 0;
      size_t assignedCount = countBitmaskOnes(cfg.selected_contacts);
      if (!alreadyAssigned && assignedCount >= 5) {
        sendSMS(sender, "ERROR: Maximum contacts reached");
        return true;
      }
      cfg.selected_contacts |= (1UL << contactIndex);
      if (!Shared_saveAnalogInputConfig(inputIndex, cfg)) {
        sendSMS(sender, "ERROR: Save failed");
        return true;
      }
      if (!updateContactNotificationFlags(contactIndex, voiceFlag, smsFlag)) {
        sendSMS(sender, "ERROR: Save failed");
        return true;
      }
      return true;
    }
    DigitalInputConfig cfg = {};
    if (!Shared_getDigitalInputConfig(inputIndex, cfg)) {
      sendSMS(sender, "ERROR: Unable to read input config");
      return true;
    }
    bool alreadyAssigned = (cfg.selected_contacts & (1UL << contactIndex)) != 0;
    size_t assignedCount = countBitmaskOnes(cfg.selected_contacts);
    if (!alreadyAssigned && assignedCount >= 5) {
      sendSMS(sender, "ERROR: Maximum contacts reached");
      return true;
    }
    cfg.selected_contacts |= (1UL << contactIndex);
    if (!Shared_saveDigitalInputConfig(inputIndex, cfg)) {
      sendSMS(sender, "ERROR: Save failed");
      return true;
    }
    if (!updateContactNotificationFlags(contactIndex, voiceFlag, smsFlag)) {
      sendSMS(sender, "ERROR: Save failed");
      return true;
    }
    return true;
  }

  return false;
}

static void processRelayCommand(const String &sender, const String &body) {
  String content = body;
  int firstPercent = content.indexOf('%');
  int lastPercent = content.lastIndexOf('%');
  if (firstPercent >= 0 && lastPercent > firstPercent) {
    content = content.substring(firstPercent + 1, lastPercent);
  } else if (firstPercent >= 0) {
    content = content.substring(firstPercent + 1);
  }
  content.trim();

  int c1 = content.indexOf(',');
  int c2 = content.indexOf(',', c1 + 1);
  int c3 = content.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) {
    Serial.println("[SMS] Bad relay command format: " + body);
    return;
  }

  String pin = content.substring(0, c1);
  String relayName = content.substring(c1 + 1, c2);
  String state = content.substring(c2 + 1, c3);
  String onTimeStr = content.substring(c3 + 1);
  pin.trim(); relayName.trim(); state.trim(); onTimeStr.trim();

  if (!isAuthorizedSender(sender)) {
    Serial.println("[SMS] Unauthorized sender for relay: " + sender);
    return;
  }

  SIMConfig simCfg = {};
  Shared_getSIMConfig(simCfg);
  String storedPin = String(simCfg.relay_pin);
  storedPin.trim();
  if (storedPin.length() == 0 || pin != storedPin) {
    Serial.println("[SMS] Invalid PIN from: " + sender);
    return;
  }

  int relayIdx = -1;
  for (int i = 0; i < RELAY_OUTPUT_COUNT; ++i) {
    RelayConfig cfg = {};
    Shared_getRelayConfig(i, cfg);
    String name = String(cfg.name);
    name.trim();
    if (name.length() > 0 && name.equalsIgnoreCase(relayName)) {
      relayIdx = i;
      break;
    }
  }
  if (relayIdx < 0) {
    Serial.println("[SMS] Unknown relay: " + relayName);
    return;
  }

  RelayConfig cfg = {};
  Shared_getRelayConfig(relayIdx, cfg);
  if (!cfg.enabled) {
    Serial.println("[SMS] Relay disabled: " + relayName);
    return;
  }
  if (!cfg.sms_control_enabled) {
    Serial.println("[SMS] SMS control disabled for relay: " + relayName);
    return;
  }



  bool isOn = state.equalsIgnoreCase("on");
  bool isOff = state.equalsIgnoreCase("off");

  if (isOn) {
    Shared_setRelayState(relayIdx, true);
    Shared_setRelayTriggerSource(relayIdx, RELAY_SOURCE_SMS);
    Serial.println("[SMS] Relay " + relayName + " ON");
    for (auto &p : pulseSlots) {
      if (p.relayIdx == (size_t)relayIdx) { p.relayIdx = (size_t)-1; p.offAtMs = 0; }
    }
    unsigned long onTime = onTimeStr.toInt();
    if (onTime > 0) schedulePulse(relayIdx, onTime);
  } else if (isOff) {
    Shared_setRelayState(relayIdx, false);
    Shared_setRelayTriggerSource(relayIdx, RELAY_SOURCE_SMS);
    Serial.println("[SMS] Relay " + relayName + " OFF");
    for (auto &p : pulseSlots) {
      if (p.relayIdx == (size_t)relayIdx) { p.relayIdx = (size_t)-1; p.offAtMs = 0; }
    }
  } else {
    Serial.println("[SMS] Invalid state: " + state);
  }
}

static String getQuotedField(const String &line, uint8_t fieldIndex) {
  int searchFrom = 0;
  for (uint8_t i = 0; i <= fieldIndex; ++i) {
    int q1 = line.indexOf('"', searchFrom);
    if (q1 < 0) return "";
    int q2 = line.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    if (i == fieldIndex) return line.substring(q1 + 1, q2);
    searchFrom = q2 + 1;
  }
  return "";
}

static void checkAndProcessSMS() {
  if (!modemReady) return;

  String modeResp = sendAT("AT+CMGF=1", 2000, true);
  if (modeResp.indexOf("OK") == -1) return;

  String resp = sendAT("AT+CMGL=\"ALL\"", 5000, true);
  if (resp.indexOf("+CMGL:") < 0) return;

  int pos = 0;
  int currentIndex = -1;
  String sender = "";
  String body = "";

  while (pos < resp.length()) {
    int next = resp.indexOf('\n', pos);
    if (next < 0) next = resp.length();
    String line = resp.substring(pos, next);
    line.trim();

    if (line.startsWith("+CMGL:")) {
      if (currentIndex >= 0) {
        String upperBody = body;
        upperBody.toUpperCase();
        if (processContactAssignmentCommand(sender, body)) {
          // handled by new contact management commands
        } else if (upperBody.indexOf("GET STATUS") >= 0) {
          processStatusRequest(sender);
        // } else if (upperBody.indexOf("GET IP%") >= 0) {
        //   processIpRequest(sender, body);
        } else if (upperBody.indexOf("GET ALARM") >= 0) {
          processAlarmRequest(sender);
        } else if (upperBody.indexOf("GET INPUT") >= 0) {
          processInputRequest(sender);
        } else if (upperBody.indexOf("GET RELAY") >= 0) {
          processRelayStatusRequest(sender);
        } else if (upperBody.startsWith("ACK ") || upperBody == "ACK") {
          if (isAuthorizedSender(sender)) {
            CallManager_handleSmsAck(sender, body);
          }
        } else {
          int cmdIdx = body.indexOf("Set Relay%");
          if (cmdIdx >= 0) {
            processRelayCommand(sender, body.substring(cmdIdx));
          }
        }
        sendAT("AT+CMGD=" + String(currentIndex), 2000, true);
      }
      int idxStart = line.indexOf(' ');
      int idxEnd = line.indexOf(',', idxStart);
      currentIndex = line.substring(idxStart + 1, idxEnd).toInt();
      sender = getQuotedField(line, 1);
      body = "";
    } else if (currentIndex >= 0 && line.length() > 0 && line != "OK" && !line.startsWith("+CMGL")) {
      body += line + "\n";
    }
    pos = next + 1;
  }

  if (currentIndex >= 0) {
    String upperBody = body;
    upperBody.toUpperCase();
    if (processContactAssignmentCommand(sender, body)) {
      // handled by new contact management commands
    } else if (upperBody.indexOf("GET STATUS") >= 0) {
      processStatusRequest(sender);
    // } else if (upperBody.indexOf("GET IP%") >= 0) {
    //   processIpRequest(sender, body);
    } else if (upperBody.indexOf("GET ALARM") >= 0) {
      processAlarmRequest(sender);
    } else if (upperBody.indexOf("GET INPUT") >= 0) {
      processInputRequest(sender);
    } else if (upperBody.indexOf("GET RELAY") >= 0) {
      processRelayStatusRequest(sender);
    } else if (upperBody.startsWith("ACK ") || upperBody == "ACK") {
      if (isAuthorizedSender(sender)) {
        CallManager_handleSmsAck(sender, body);
      }
    } else {
      int cmdIdx = body.indexOf("Set Relay%");
      if (cmdIdx >= 0) {
        processRelayCommand(sender, body.substring(cmdIdx));
      }
    }
    sendAT("AT+CMGD=" + String(currentIndex), 2000, true);
  }
}

// ---------------------------------------------------------------------------
// Modem checks
// ---------------------------------------------------------------------------
static bool modemSimReady() {
  String sim   = sendAT("AT+CPIN?", 2000);
  bool   ready = sim.indexOf("READY") != -1;
  simMissingLatched = (sim.indexOf("SIM not inserted") != -1);
  return ready;
}

static bool waitForNetwork() {
  for (int i = 0; i < 10; ++i) {
    String res = sendAT("AT+CREG?", 2000);
    if (res.indexOf("0,1") != -1 || res.indexOf("0,5") != -1) {
      Serial.println("[MODEM] Network registered");
      return true;
    }
    Serial.printf("[MODEM] Waiting for network... (%d/10)\n", i + 1);
    delay(2000);
  }
  Serial.println("[MODEM] Network FAILED");
  return false;
}

// ---------------------------------------------------------------------------
// sendSMS — two-step AT+CMGS exchange
// ---------------------------------------------------------------------------
static bool sendSMS(const String &number, const String &message) {
  if (!modemReady) {
    Serial.println("[SMS] ERROR: Modem not ready");
    return false;
  }

  if (!modemSimReady()) {
    Serial.println("[SMS] SIM not ready - marking modem not ready");
    setModemReady(false);
    consecutiveModemHealthFailures = 0;
    return false;
  }

  String netRes  = sendAT("AT+CREG?", 2000);
  bool onNetwork = netRes.indexOf("0,1") != -1 || netRes.indexOf("0,5") != -1;
  if (!onNetwork) {
    Serial.println("[SMS] Network lost - marking modem not ready");
    setModemReady(false);
    consecutiveModemHealthFailures = 0;
    return false;
  }

  sendAT("AT",                 1000);
  sendAT("AT+CMEE=2",          2000);
  sendAT("AT+CSCS=\"GSM\"",    2000);
  sendAT("AT+CSMP=17,167,0,0", 2000);
  sendAT("AT+CMGF=1",          2000);

  Serial.printf("[SMS] Sending to %s\n", number.c_str());
  modemSerialBusy = true;
  while (SerialAT.available()) SerialAT.read();

  SerialAT.print("AT+CMGS=\"");
  SerialAT.print(number);
  SerialAT.println("\"");

  String prompt = readSerialATResponse(5000);
  Serial.printf("[SMS] CMGS prompt: %s\n", prompt.c_str());

  if (prompt.indexOf(">") == -1) {
    Serial.println("[SMS] No '>' prompt received - aborting");
    SerialAT.write(26);
    delay(500);
    modemSerialBusy = false;
    consecutiveModemHealthFailures++;
    if (consecutiveModemHealthFailures >= 2) {
      setModemReady(false);
      Serial.println("[SMS] Prompt missing repeatedly - marking modem not ready");
    } else {
      Serial.println("[SMS] Prompt missing once - keeping modem ready");
    }
    return false;
  }

  SerialAT.print(message);
  delay(200);
  SerialAT.write(26);

  String res = readSerialATResponse(15000);
  Serial.printf("[SMS] CMGS result: %s\n", res.c_str());

  modemSerialBusy = false;
  bool ok = res.indexOf("+CMGS:") != -1 && res.indexOf("ERROR") == -1;
  if (ok) {
    consecutiveModemHealthFailures = 0;
  } else {
    Serial.println("[SMS] Delivery failed for this number - flushing modem state");
    SerialAT.write(26);
    delay(300);
    SerialAT.write(27);
    delay(300);
    while (SerialAT.available()) SerialAT.read();

    String pingRes = sendAT("AT", 2000);
    if (pingRes.indexOf("OK") == -1) {
      consecutiveModemHealthFailures++;
      if (consecutiveModemHealthFailures >= 2) {
        Serial.println("[SMS] Modem unresponsive repeatedly - marking not ready");
        setModemReady(false);
      } else {
        Serial.println("[SMS] Modem unresponsive once - keeping modem ready for now");
      }
    } else {
      consecutiveModemHealthFailures = 0;
      Serial.println("[SMS] Modem recovered - continuing to next number");
    }
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Power on
// ---------------------------------------------------------------------------
static void modemPowerOn() {
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, HIGH);
  Serial.println("[MODEM] Power ON triggered");
  delay(8000);
}

// ---------------------------------------------------------------------------
// initModem
// ---------------------------------------------------------------------------
static void initModem() {
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(500);

  setModemInitStatusLED(false);

  Serial.println("\n=== Initializing 4G Modem (EC200U) ===");
  modemPowerOn();

  String res = sendAT("AT", 2000);
  if (res.indexOf("OK") == -1) {
    delay(3000);
    res = sendAT("AT", 2000);
  }

  if (res.indexOf("OK") == -1) {
    setModemReady(false);
    simMissingLatched = false;
    Serial.println("[MODEM] No modem response after power ON");
    return;
  }

  sendAT("AT+CMEE=2", 2000);
  sendAT("AT+CMGF=1", 2000);
  sendAT("AT+CNMI=2,1,0,0,0", 2000);
  sendAT("AT+CMGD=1,4", 2000);
  bool simOk     = modemSimReady();
  bool networkOk = simOk && waitForNetwork();
  setModemReady(simOk && networkOk);

  Serial.println(modemReady
    ? "[MODEM] Modem initialized successfully"
    : "[MODEM] Modem initialization failed");
}

// ---------------------------------------------------------------------------
// Modem_init — called from setup()
// ---------------------------------------------------------------------------
bool Modem_init() {
  return true;
}

// ---------------------------------------------------------------------------
// buildAlarmSMS / buildReturnSMS - structured SMS formatters
// ---------------------------------------------------------------------------
static String alarmTypeName(uint8_t t) {
  switch (t) {
    case 0: return "High";
    case 1: return "Low";
    case 2: return "In-Band";
    case 3: return "Out-of-Band";
    default: return "Unknown";
  }
}

static String buildAnalogAlarmSMS(const AnalogInputConfig &cfg, float value) {
  String state = String(cfg.alarm_message);
  if (state.length() == 0) state = String(cfg.name) + " ALARM";
  String msg = "[ALARM]\n";
  msg += buildSMSHeader() + "\n";
  msg += "Input: " + String(cfg.name) + "\n";
  msg += "Type: " + alarmTypeName(cfg.alarm_type) + "\n";
  msg += "Value: " + String(value, 2) + " " + String(cfg.engineering_unit) + "\n";
  msg += "State: " + state;
  return msg;
}

static String buildAnalogReturnSMS(const AnalogInputConfig &cfg, float value) {
  String state = String(cfg.return_message);
  if (state.length() == 0) state = String(cfg.name) + " RETURN TO NORMAL";
  String msg = "[RETURN]\n";
  msg += buildSMSHeader() + "\n";
  msg += "Input: " + String(cfg.name) + "\n";
  msg += "Type: " + alarmTypeName(cfg.alarm_type) + "\n";
  msg += "Value: " + String(value, 2) + " " + String(cfg.engineering_unit) + "\n";
  msg += "State: " + state;
  return msg;
}

static void dispatchAISMS(const AIPendingSMS &pending) {
  AnalogInputConfig cfg = {};
  if (!Shared_getAnalogInputConfig(pending.index, cfg)) return;
  if (!cfg.enabled) return;

  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec) || rec.count == 0) return;

  String msg = pending.isAlarm ? buildAnalogAlarmSMS(cfg, pending.value)
                               : buildAnalogReturnSMS(cfg, pending.value);

  for (size_t i = 0; i < rec.count && i < MAX_PHONE_PER_LIST; ++i) {
    if (!rec.items[i].enabled) continue;
    if (!(cfg.selected_contacts & (1 << i))) continue;
    String number = String(rec.items[i].number);
    if (number.length() == 0) continue;
    String normalized;
    if (!normalizePhoneNumber(number, normalized)) continue;
    if (!modemReady) break;
    sendSMS(normalized, msg);
  }
}

static String buildAlarmSMS(const DigitalInputConfig &cfg) {
  String type  = cfg.normallyClosed ? "NC" : "NO";
  String state = String(cfg.alarm_message);
  if (state.length() == 0) state = String(cfg.name) + " ALARM";

  String msg = "[ALARM]\n";
  msg += buildSMSHeader() + "\n";
  msg += "Input: " + String(cfg.name) + "\n";
  msg += "Type: " + type + "\n";
  msg += "State: " + state;
  return msg;
}

static String buildReturnSMS(const DigitalInputConfig &cfg) {
  String type  = cfg.normallyClosed ? "NC" : "NO";
  String state = String(cfg.return_message);
  if (state.length() == 0) state = String(cfg.name) + " RETURN TO NORMAL";

  String msg = "[RETURN]\n";
  msg += buildSMSHeader() + "\n";
  msg += "Input: " + String(cfg.name) + "\n";
  msg += "Type: " + type + "\n";
  msg += "State: " + state;
  return msg;
}

// ---------------------------------------------------------------------------
// dispatchDISMS - sends alarm or return SMS from DI queue entry
// ---------------------------------------------------------------------------
static void dispatchDISMS(const DIPendingSMS &pending) {
  DigitalInputConfig diCfg = {};
  if (!Shared_getDigitalInputConfig(pending.index, diCfg)) return;
  if (!diCfg.enabled) return;

  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec) || rec.count == 0) return;

  String msg = pending.isAlarm ? buildAlarmSMS(diCfg) : buildReturnSMS(diCfg);

  for (size_t i = 0; i < rec.count && i < MAX_PHONE_PER_LIST; ++i) {
    if (!rec.items[i].enabled) continue;
    if (!(diCfg.selected_contacts & (1 << i))) continue;
    String number = String(rec.items[i].number);
    if (number.length() == 0) continue;
    String normalized;
    if (!normalizePhoneNumber(number, normalized)) continue;
    if (!modemReady) break;
    sendSMS(normalized, msg);
  }
}

// ---------------------------------------------------------------------------
// Modem_getSignalStrength — returns cached RSSI, updated by modem task
// ---------------------------------------------------------------------------
int8_t Modem_getSignalStrength() {
  if (simMissingLatched) return -2;
  if (!modemReady) return -1;
  return cachedRssi;
}

static void updateCachedRssi() {
  if (!modemReady || modemSerialBusy) return;
  unsigned long now = millis();
  if (now - lastRssiUpdateMs < 30000) return; // update every 30s
  lastRssiUpdateMs = now;

  String response = sendAT("AT+CSQ", 2000, true);
  if (response.indexOf("+CSQ:") == -1) return;

  int colonIdx = response.indexOf(':');
  int commaIdx = response.indexOf(',', colonIdx);
  if (colonIdx < 0 || commaIdx < 0) return;

  String rssiStr = response.substring(colonIdx + 1, commaIdx);
  rssiStr.trim();
  int rssi = rssiStr.toInt();
  if (rssi >= 0 && rssi <= 31) cachedRssi = (int8_t)rssi;
}

void Modem_task(void *pvParameters) {
  (void)pvParameters;
  resetPulseSlots();

  // Blocking init — only affects core 0, core 1 tasks run freely
  initModem();
  CallManager_init(SerialAT);
  Serial.println("[MODEM TASK] Init complete, starting SMS processing");

  for (;;) {
    unsigned long now = millis();

    // Handle modem reinit if not ready
    if (!modemReady) {
      if (simMissingLatched) {
        if (now - lastSimRecheckMs >= 15000) {
          lastSimRecheckMs = now;
          Serial.println("[MODEM] SIM missing latched - running full modem init...");
          initModem();
          if (modemReady) {
            simMissingLatched = false;
            consecutiveModemHealthFailures = 0;
          }
        } else if (now - lastNotReadyLogMs >= 5000) {
          Serial.println("[MODEM] Not ready - SIM-missing init cooldown active");
          lastNotReadyLogMs = now;
        }
      } else if (now - lastReinitAttemptMs >= 12000) {
        Serial.println("[MODEM] Not ready - attempting reinit...");
        lastReinitAttemptMs = now;
        initModem();
      } else if (now - lastNotReadyLogMs >= 5000) {
        Serial.println("[MODEM] Not ready - reinit cooldown active");
        lastNotReadyLogMs = now;
      }
    }

    // Heartbeat (Status Message) check
    if (modemReady && Shared_tickHeartbeat()) {
      HeartbeatConfig hbCfg = {};
      Shared_getHeartbeatConfig(hbCfg);
      ContactList rec = {};
      if (Shared_getRecipientContacts(rec) && rec.count > 0) {
        String msg = buildSystemStatusSMS();
        for (size_t i = 0; i < rec.count && i < MAX_PHONE_PER_LIST; ++i) {
          if (!rec.items[i].enabled) continue;
          if (!(hbCfg.selected_contacts & (1UL << i))) continue;
          String number = String(rec.items[i].number);
          if (number.length() == 0) continue;
          String normalized;
          if (!normalizePhoneNumber(number, normalized)) continue;
          if (!modemReady) break;
          sendSMS(normalized, msg);
        }
        Serial.println("[HEARTBEAT] Status SMS dispatched");
      }
    }

    // Drain unified notification event queue: SMS first, then enqueue voice calls
    NotificationEvent notifEv = {};
    if (Shared_takeNotificationEvent(notifEv)) {
      if (modemReady) {
        // 1. Send SMS to all selected contacts with sms_enabled
        ContactList rec = {};
        if (Shared_getRecipientContacts(rec) && rec.count > 0) {
          String msg;
          if (notifEv.source == ALARM_SRC_DI) {
            DigitalInputConfig diCfg = {};
            Shared_getDigitalInputConfig(notifEv.index, diCfg);
            bool smsEnabled = notifEv.isAlarm ? diCfg.alarm_sms_enabled : diCfg.return_sms_enabled;
            if (smsEnabled) {
              msg = notifEv.isAlarm ? buildAlarmSMS(diCfg) : buildReturnSMS(diCfg);
            }
          } else {
            AnalogInputConfig aiCfg = {};
            Shared_getAnalogInputConfig(notifEv.index, aiCfg);
            bool smsEnabled = notifEv.isAlarm ? aiCfg.alarm_sms_enabled : aiCfg.return_sms_enabled;
            if (smsEnabled) {
              msg = notifEv.isAlarm ? buildAnalogAlarmSMS(aiCfg, notifEv.value)
                                    : buildAnalogReturnSMS(aiCfg, notifEv.value);
            }
          }
          if (msg.length() > 0) {
            for (size_t i = 0; i < rec.count && i < MAX_PHONE_PER_LIST; ++i) {
              if (!rec.items[i].enabled) continue;
              if (!(notifEv.selected_contacts & (1UL << i))) continue;
              if (!rec.items[i].sms_enabled) continue;
              String normalized;
              if (!normalizePhoneNumber(String(rec.items[i].number), normalized)) continue;
              if (!modemReady) break;
              sendSMS(normalized, msg);
            }
          }
        }
        // 2. Enqueue voice calls (only for alarm events, not return)
        if (notifEv.isAlarm) {
          CallManager_enqueue(notifEv);
        }
      }
    }

    // Tick CallManager state machine
    CallManager_tick();

    if (now - lastSmsCheckMs >= 5000) {
      lastSmsCheckMs = now;
      checkAndProcessSMS();
    }

    checkPulses();
    updateCachedRssi();

    vTaskDelay(pdMS_TO_TICKS(25));
  }
}
