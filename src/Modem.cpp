#include "Modem.h"
#include "Shared.h"
#include <HardwareSerial.h>

HardwareSerial SerialAT(1);
static bool modemReady = false;
static uint8_t consecutiveModemHealthFailures = 0;
static unsigned long lastReinitAttemptMs = 0;
static unsigned long lastNotReadyLogMs = 0;
static bool simMissingLatched = false;
static unsigned long lastSimRecheckMs = 0;
static unsigned long lastSmsCheckMs = 0;



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

static void updateModemState(int16_t modemState, int16_t simState, int16_t networkState) {
  Shared_writeInputRegister(MODEM_STATUS_REGISTER,   modemState);
  Shared_writeInputRegister(SIM_STATUS_REGISTER,     simState);
  Shared_writeInputRegister(NETWORK_STATUS_REGISTER, networkState);
}

static void setModemInitStatusLED(bool ready) {
  digitalWrite(MODEM_INIT_STATUS_PIN, ready ? HIGH : LOW);
}

static void setModemReady(bool ready) {
  modemReady = ready;
  setModemInitStatusLED(ready);
}

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
// AT command
// ---------------------------------------------------------------------------
static String sendAT(const String &cmd, int timeout) {
  while (SerialAT.available()) SerialAT.read();

  Serial.println("[AT] >> " + cmd);
  SerialAT.println(cmd);
  delay(100);

  String response = readSerialATResponse((unsigned long)timeout);
  Serial.println(response.length() ? "[AT] << " + response : "[AT] << [NO RESPONSE]");
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

  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec)) return;
  bool authorized = false;
  for (size_t i = 0; i < rec.count; ++i) {
    if (!rec.items[i].enabled) continue;
    String num = String(rec.items[i].number);
    if (sender == num || sender == "+" + num) {
      authorized = true;
      break;
    }
    if (num.startsWith("+")) num = num.substring(1);
    if (sender == num || sender == "+" + num) {
      authorized = true;
      break;
    }
  }
  if (!authorized) {
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

  bool isOn = state.equalsIgnoreCase("on");
  bool isOff = state.equalsIgnoreCase("off");

  if (isOn) {
    Shared_setRelayState(relayIdx, true);
    Serial.println("[SMS] Relay " + relayName + " ON");
    if (onTimeStr.length() > 0 && !onTimeStr.equals("0")) {
      unsigned long onTime = onTimeStr.toInt();
      if (onTime > 0 && onTime <= 65535) {
        schedulePulse(relayIdx, onTime);
      }
    }
  } else if (isOff) {
    Shared_setRelayState(relayIdx, false);
    Serial.println("[SMS] Relay " + relayName + " OFF");
    if (!onTimeStr.equals("0")) {
      for (auto &p : pulseSlots) {
        if (p.relayIdx == (size_t)relayIdx) {
          p.relayIdx = (size_t)-1;
          p.offAtMs = 0;
        }
      }
    }
  } else {
    Serial.println("[SMS] Invalid state: " + state);
  }
}

static void checkAndProcessSMS() {
  if (!modemReady) return;

  String resp = sendAT("AT+CMGL=\"ALL\"", 5000);
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
        int cmdIdx = body.indexOf("Set Relay%");
        if (cmdIdx >= 0) {
          processRelayCommand(sender, body.substring(cmdIdx));
        }
        sendAT("AT+CMGD=" + String(currentIndex), 2000);
      }
      int idxStart = line.indexOf(' ');
      int idxEnd = line.indexOf(',', idxStart);
      currentIndex = line.substring(idxStart + 1, idxEnd).toInt();
      int q1 = line.indexOf('"', idxEnd);
      int q2 = line.indexOf('"', q1 + 1);
      sender = (q1 >= 0 && q2 >= 0) ? line.substring(q1 + 1, q2) : "";
      body = "";
    } else if (currentIndex >= 0 && line.length() > 0 && line != "OK" && !line.startsWith("+CMGL")) {
      body += line + "\n";
    }
    pos = next + 1;
  }

  if (currentIndex >= 0) {
    int cmdIdx = body.indexOf("Set Relay%");
    if (cmdIdx >= 0) {
      processRelayCommand(sender, body.substring(cmdIdx));
    }
    sendAT("AT+CMGD=" + String(currentIndex), 2000);
  }
}

// ---------------------------------------------------------------------------
// Modem checks
// ---------------------------------------------------------------------------
static bool modemSimReady() {
  String sim   = sendAT("AT+CPIN?", 2000);
  bool   ready = sim.indexOf("READY") != -1;
  simMissingLatched = (sim.indexOf("SIM not inserted") != -1);
  Shared_writeInputRegister(SIM_STATUS_REGISTER,
    ready ? (int16_t)STATE_READY : (int16_t)STATE_ERROR);
  return ready;
}

static bool waitForNetwork() {
  for (int i = 0; i < 10; ++i) {
    String res = sendAT("AT+CREG?", 2000);
    if (res.indexOf("0,1") != -1 || res.indexOf("0,5") != -1) {
      Shared_writeInputRegister(NETWORK_STATUS_REGISTER, (int16_t)STATE_READY);
      Serial.println("[MODEM] Network registered");
      return true;
    }
    Serial.printf("[MODEM] Waiting for network... (%d/10)\n", i + 1);
    delay(2000);
  }
  Shared_writeInputRegister(NETWORK_STATUS_REGISTER, (int16_t)STATE_ERROR);
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
    Shared_writeInputRegister(NETWORK_STATUS_REGISTER, (int16_t)STATE_ERROR);
    return false;
  }

  sendAT("AT",                 1000);
  sendAT("AT+CMEE=2",          2000);
  sendAT("AT+CSCS=\"GSM\"",    2000);
  sendAT("AT+CSMP=17,167,0,0", 2000);
  sendAT("AT+CMGF=1",          2000);

  Serial.printf("[SMS] Sending to %s\n", number.c_str());
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
  updateModemState((int16_t)STATE_BUSY, (int16_t)STATE_IDLE, (int16_t)STATE_IDLE);

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
    updateModemState((int16_t)STATE_ERROR, (int16_t)STATE_IDLE, (int16_t)STATE_IDLE);
    Serial.println("[MODEM] No modem response after power ON");
    return;
  }

  sendAT("AT+CMEE=2", 2000);
  sendAT("AT+CNMI=2,1,0,0,0", 2000);
  sendAT("AT+CMGD=1,4", 2000);
  bool simOk     = modemSimReady();
  bool networkOk = simOk && waitForNetwork();
  setModemReady(simOk && networkOk);

  updateModemState(
    modemReady  ? (int16_t)STATE_READY : (int16_t)STATE_ERROR,
    simOk       ? (int16_t)STATE_READY : (int16_t)STATE_ERROR,
    networkOk   ? (int16_t)STATE_READY : (int16_t)STATE_ERROR
  );

  Serial.println(modemReady
    ? "[MODEM] Modem initialized successfully"
    : "[MODEM] Modem initialization failed");
}

// ---------------------------------------------------------------------------
// Modem_init — called from setup()
// ---------------------------------------------------------------------------
bool Modem_init() {
  Shared_writeInputRegister(DEVICE_STATUS_REGISTER,  (int16_t)STATE_READY);
  Shared_writeInputRegister(MODEM_STATUS_REGISTER,   (int16_t)STATE_IDLE);
  Shared_writeInputRegister(SIM_STATUS_REGISTER,     (int16_t)STATE_IDLE);
  Shared_writeInputRegister(NETWORK_STATUS_REGISTER, (int16_t)STATE_IDLE);
  return true;
}

// ---------------------------------------------------------------------------
// dispatchMessage - use recipient contacts only
// ---------------------------------------------------------------------------
static int16_t dispatchMessage(size_t inputIndex) {
  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec)) return STATUS_ERROR_CONFIG;
  if (rec.count == 0) return STATUS_ERROR_EMPTY;

  Shared_writeInputRegister(MODEM_STATUS_REGISTER, (int16_t)STATE_BUSY);
  uint8_t sentCount = 0;
  String msg = "Alarm on input " + String((unsigned)inputIndex + 1);

  for (size_t i = 0; i < rec.count && i < MAX_PHONE_PER_LIST; ++i) {
    if (!rec.items[i].enabled) continue;
    String number = String(rec.items[i].number);
    if (number.length() == 0) continue;
    String normalizedNumber;
    if (!normalizePhoneNumber(number, normalizedNumber)) {
      Serial.printf("[SMS] Skipping invalid number format: %s\n", number.c_str());
      continue;
    }

    if (!modemReady) {
      Serial.printf("[SMS] Modem not ready, skipping remaining numbers from slot %u\n", (unsigned)i);
      break;
    }

    if (sendSMS(normalizedNumber, msg)) sentCount++;
  }

  Shared_writeInputRegister(MODEM_STATUS_REGISTER,
    modemReady ? (int16_t)STATE_READY : (int16_t)STATE_ERROR);

  if (sentCount == 0) return STATUS_ERROR_SEND;
  return (int16_t)sentCount;
}

// ---------------------------------------------------------------------------
// Rising-edge scanner + pending-slot tracker
//
// pendingSlots[] — per-slot flag set on rising edge and consumed after
// dispatch. This keeps one-shot behavior per 0->1 transition without queues.
//
// Clear-on-zero — when the PLC writes 0 to a trigger register, the
// corresponding result register is cleared back to STATUS_IDLE (0).
// This resets the slot so the PLC can write 1 again to retrigger.
// ---------------------------------------------------------------------------
static bool pendingSlots[DIGITAL_INPUT_COUNT] = {};
static uint8_t lowStableCount[DIGITAL_INPUT_COUNT] = {};

static void scanTriggerEdges(bool previousState[DIGITAL_INPUT_COUNT]) {
  SystemSnapshot snapshot = Shared_getSnapshot();
  constexpr uint8_t LOW_REARM_SCANS = 4; // 4 * 25ms ~= 100ms stable low

  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
    bool current = snapshot.digitalInputs[i] != 0;

    if (!current) {
      if (lowStableCount[i] < 255) lowStableCount[i]++;
      if (previousState[i] && lowStableCount[i] >= LOW_REARM_SCANS) {
        Shared_writeAlarmResult(i, STATUS_IDLE);
        previousState[i] = false;
        Serial.printf("[EDGE] Input %u cleared (-> 0)\n", (unsigned)i);
      }
      continue;
    }

    lowStableCount[i] = 0;

    if (!previousState[i] && !pendingSlots[i]) {
      pendingSlots[i] = true;
      previousState[i] = true;
    }
  }
}

static bool takeNextPendingSlot(size_t &slotIndex) {
  SystemSnapshot snapshot = Shared_getSnapshot();
  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
    if (!pendingSlots[i]) continue;

    if (snapshot.digitalInputs[i] == 0) {
      pendingSlots[i] = false;
      continue;
    }

    slotIndex = i;
    pendingSlots[i] = false;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Modem_getSignalStrength
// ---------------------------------------------------------------------------
// Returns signal strength 0-31 (where 0=very weak, 31=excellent), -1 on error, -2 if SIM not inserted
int8_t Modem_getSignalStrength() {
  if (simMissingLatched) {
    Serial.println("[MODEM] SIM not inserted, cannot get signal strength");
    return -2;
  }

  if (!modemReady) {
    Serial.println("[MODEM] Modem not ready, cannot get signal strength");
    return -1;
  }

  // AT+CSQ returns +CSQ: <rssi>,<ber>
  // rssi: 0=very weak, 1-30=weak to excellent, 31=excellent
  String response = sendAT("AT+CSQ", 2000);
  
  if (response.indexOf("+CSQ:") == -1) {
    Serial.println("[MODEM] No CSQ response from modem");
    return -1;
  }

  int colonIdx = response.indexOf(':');
  if (colonIdx < 0) return -1;

  // Find first number after ':'
  int commaIdx = response.indexOf(',', colonIdx);
  if (commaIdx < 0) return -1;

  String rssiStr = response.substring(colonIdx + 1, commaIdx);
  rssiStr.trim();

  int rssi = rssiStr.toInt();
  if (rssi < 0 || rssi > 31) {
    Serial.printf("[MODEM] Invalid RSSI value: %d\n", rssi);
    return -1;
  }

  return (int8_t)rssi;
}

void Modem_task(void *pvParameters) {
  (void)pvParameters;
  resetPulseSlots();

  // Blocking init — only affects core 0, core 1 tasks run freely
  initModem();
  Serial.println("[MODEM TASK] Init complete, starting SMS processing");

  bool   previousState[DIGITAL_INPUT_COUNT] = {};
  size_t slotToProcess = 0;

  for (;;) {
    scanTriggerEdges(previousState);

    unsigned long now = millis();

    if (takeNextPendingSlot(slotToProcess)) {
      if (!modemReady) {
        // If SIM was last seen as missing, run a full modem init sequence
        // so hot-inserted SIMs get detected through the normal bring-up path.
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
          // Rate-limit repetitive cooldown logs to keep serial output readable.
          Serial.println("[MODEM] Not ready - reinit cooldown active");
          lastNotReadyLogMs = now;
        }
      }

      if (!modemReady) {
        Shared_writeAlarmResult(slotToProcess, simMissingLatched ? STATUS_ERROR_SIM : STATUS_ERROR_MODEM);
        continue;
      }

      int16_t result = dispatchMessage(slotToProcess);
      Shared_writeAlarmResult(slotToProcess, result);
    }

    if (now - lastSmsCheckMs >= 5000) {
      lastSmsCheckMs = now;
      checkAndProcessSMS();
    }

    checkPulses();

    vTaskDelay(pdMS_TO_TICKS(25));
  }
}
