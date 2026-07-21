#include "CallManager.h"
#include "Shared.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct CallEntry {
  char   number[PHONE_NUMBER_LENGTH];
  char   message[64];
  AlarmSource src;
  size_t index;
  bool   valid;
};

constexpr size_t CALL_QUEUE_SIZE = MAX_PHONE_PER_LIST;

enum CallState : uint8_t {
  CS_IDLE,
  CS_DIALING,
  CS_WAITING_ANSWER,
  CS_CALL_ANSWERED,   // non-blocking wait for voice path to open
  CS_PLAYING_TTS,
  CS_HANGING_UP,
  CS_INTER_CALL_DELAY,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static HardwareSerial *gSerial = nullptr;

static CallEntry  callQueue[CALL_QUEUE_SIZE] = {};
static size_t     callQueueHead = 0;
static size_t     callQueueTail = 0;

static CallState     state           = CS_IDLE;
static unsigned long stateEnteredMs  = 0;
static CallEntry     currentCall     = {};
static uint16_t      ringTimeoutS    = 30;
static uint16_t      interCallDelayS = 5;

// URC accumulator — incoming serial bytes are drained here so DTMF URCs
// are never lost when the Modem task issues AT commands on the same UART.
static String urcBuf = "";

// ---------------------------------------------------------------------------
// Serial helpers
// ---------------------------------------------------------------------------

// Drain all pending bytes from the UART into urcBuf.
static void drainToUrcBuf() {
  if (!gSerial) return;
  while (gSerial->available()) {
    char c = (char)gSerial->read();
    urcBuf += c;
    // Keep buffer bounded
    if (urcBuf.length() > 512) urcBuf = urcBuf.substring(256);
  }
}

// Check urcBuf for a DTMF URC and consume it.
// EC200U URC format: +QTONEDET: <digit>\r\n
static bool consumeDTMF() {
  drainToUrcBuf();
  int idx = urcBuf.indexOf("+QTONEDET:");
  if (idx < 0) idx = urcBuf.indexOf("+DTMF:");
  if (idx < 0) return false;
  Serial.println("[CALL] DTMF URC: " + urcBuf.substring(idx, idx + 20));
  // Remove everything up to and including this URC line
  int nl = urcBuf.indexOf('\n', idx);
  urcBuf = (nl >= 0) ? urcBuf.substring(nl + 1) : "";
  return true;
}

// Send an AT command and return the response.
// Drains pending bytes into urcBuf first so URCs are not lost.
static String sendAT(const String &cmd, unsigned long timeout, bool silent = true) {
  if (!gSerial) return "";
  drainToUrcBuf(); // preserve any pending URCs before flushing
  if (!silent) Serial.println("[CALL] >> " + cmd);
  gSerial->println(cmd);
  delay(80);
  String resp = "";
  unsigned long start   = millis();
  unsigned long lastByte = start;
  while (millis() - start < timeout) {
    while (gSerial->available()) {
      char c = (char)gSerial->read();
      resp += c;
      lastByte = millis();
      delay(2);
    }
    if (resp.length() > 0 && millis() - lastByte > 100) break;
    delay(10);
  }
  resp.trim();
  if (!silent) Serial.println("[CALL] << " + (resp.length() ? resp : "[NO RESPONSE]"));
  // Any URC-like lines in the response also go into urcBuf for later inspection
  if (resp.indexOf("+QTONEDET:") >= 0 || resp.indexOf("+DTMF:") >= 0) {
    urcBuf += resp + "\n";
  }
  return resp;
}

// ---------------------------------------------------------------------------
// Queue helpers
// ---------------------------------------------------------------------------

static bool queueEmpty() { return callQueueHead == callQueueTail; }

static bool dequeue(CallEntry &out) {
  if (queueEmpty()) return false;
  out = callQueue[callQueueHead];
  callQueue[callQueueHead].valid = false;
  callQueueHead = (callQueueHead + 1) % CALL_QUEUE_SIZE;
  return true;
}

static void enqueueEntry(const CallEntry &e) {
  size_t next = (callQueueTail + 1) % CALL_QUEUE_SIZE;
  if (next == callQueueHead) {
    callQueueHead = (callQueueHead + 1) % CALL_QUEUE_SIZE; // drop oldest
  }
  callQueue[callQueueTail] = e;
  callQueueTail = next;
}

static void clearQueueForAlarm(AlarmSource src, size_t index) {
  CallEntry tmp[CALL_QUEUE_SIZE] = {};
  size_t count = 0;
  size_t i = callQueueHead;
  while (i != callQueueTail) {
    if (callQueue[i].valid && !(callQueue[i].src == src && callQueue[i].index == index)) {
      tmp[count++] = callQueue[i];
    }
    i = (i + 1) % CALL_QUEUE_SIZE;
  }
  callQueueHead = 0;
  callQueueTail = count;
  for (size_t j = 0; j < count; ++j) callQueue[j] = tmp[j];
}

static void setState(CallState s) {
  state = s;
  stateEnteredMs = millis();
}

static unsigned long elapsed() { return millis() - stateEnteredMs; }

// ---------------------------------------------------------------------------
// Modem call control
// ---------------------------------------------------------------------------

static void dial(const char *number) {
  String cmd = String("ATD") + String(number) + ";";
  sendAT(cmd, 5000, false);
}

static void hangUp() {
  sendAT("ATH", 3000, false);
}

// EC200U TTS: AT+QTTS=<mode>,<text>
//   mode 1 = play immediately during active call
// Returns true if the modem accepted the command.

static bool playTTS(const char *text)
{
    String msg(text);
    msg.replace("\"", "'");
    if (msg.length() > 200)
        msg = msg.substring(0, 200);

    String cmd =
        "AT+QWTTS=1,0,0,\"" + msg + "\"";

    String resp = sendAT(cmd, 15000, false);

    if (resp.indexOf("OK") >= 0)
    {
        Serial.println("[CALL] TTS command accepted");
        return true;
    }

    Serial.println("[CALL] TTS failed:");
    Serial.println(resp);

    return false;
}

// Check call status via AT+CLCC
// Returns: 0=no call, 1=active, 3=dialing, 4=alerting/ringing
static int getCallStatus() {
  String resp = sendAT("AT+CLCC", 2000);
  if (resp.indexOf("+CLCC:") < 0) return 0;
  // +CLCC: <idx>,<dir>,<stat>,<mode>,<mpty>,...
  // stat: 0=active, 1=held, 2=dialing, 3=alerting, 4=incoming, 5=waiting
  int clccStart = resp.indexOf("+CLCC:");
  int comma1 = resp.indexOf(',', clccStart);
  int comma2 = resp.indexOf(',', comma1 + 1);
  if (comma1 < 0 || comma2 < 0) return 0;
  String statStr = resp.substring(comma1 + 1, comma2);
  statStr.trim();
  int stat = statStr.toInt();
  // Map EC200U stat to our convention: 0=active→1, 2=dialing→3, 3=alerting→4
  if (stat == 0) return 1; // active
  if (stat == 2) return 3; // dialing (MO)
  if (stat == 3) return 4; // alerting (remote ringing)
  return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CallManager_init(HardwareSerial &serial) {
  gSerial = &serial;
  urcBuf = "";

  // Redirect all URCs to uart1 (the UART the ESP32 is connected to).
  sendAT("AT+QURCCFG=\"urcport\",\"uart1\"", 2000, false);

  // Keep DAI in its current mode (mode 3, I2S) — do NOT change it.
  // Voice call audio goes through the baseband codec path, not DAI.
  // AT+QAUDPLAY plays audio through the voice call codec path directly.

  // Set call audio volume to maximum
  sendAT("AT+CLVL=5", 1000, false);

  // Configure QWTTS language: mode 0 = English
  // (No separate QTTSETUP needed for QWTTS)

  // NOTE: AT+QTONEDET must be sent during an active call, not at init.

  Serial.println("[CALL] CallManager initialized");
}

void CallManager_enqueue(const NotificationEvent &ev) {
  VoiceCallSettings vcs = {};
  Shared_getVoiceCallSettings(vcs);
  if (!vcs.enabled) return;

  ringTimeoutS    = vcs.ring_timeout_s    > 0 ? vcs.ring_timeout_s    : 30;
  interCallDelayS = vcs.inter_call_delay_s > 0 ? vcs.inter_call_delay_s : 5;

  ContactList rec = {};
  if (!Shared_getRecipientContacts(rec) || rec.count == 0) return;

  for (size_t i = 0; i < rec.count && i < MAX_PHONE_PER_LIST; ++i) {
    if (!rec.items[i].enabled) continue;
    if (!(ev.selected_contacts & (1UL << i))) continue;
    if (!rec.items[i].call_enabled) continue;

    CallEntry e = {};
    strncpy(e.number,  rec.items[i].number, sizeof(e.number) - 1);
    strncpy(e.message, ev.message,          sizeof(e.message) - 1);
    e.src   = ev.source;
    e.index = ev.index;
    e.valid = true;
    enqueueEntry(e);
    Serial.printf("[CALL] Queued call to %s for alarm %d/%u\n",
                  e.number, (int)e.src, (unsigned)e.index);
  }
}

void CallManager_ack(AlarmSource src, size_t index) {
  Shared_setAlarmAck(src, index, true);
  if (state != CS_IDLE && currentCall.src == src && currentCall.index == index) {
    hangUp();
    setState(CS_IDLE);
    Serial.printf("[CALL] ACK — current call terminated (src=%d idx=%u)\n",
                  (int)src, (unsigned)index);
  }
  clearQueueForAlarm(src, index);
  Serial.printf("[CALL] ACK — queue cleared for src=%d idx=%u\n",
                (int)src, (unsigned)index);
}

bool CallManager_handleSmsAck(const String &sender, const String &body) {
  String upper = body;
  upper.toUpperCase();
  upper.trim();
  if (!upper.startsWith("ACK%")) return false;

  // Extract the input identifier after "ACK%"
  String inputName = body.substring(4);
  inputName.trim();

  // Reject empty input name — bare "ACK" must not acknowledge all alarms
  if (inputName.length() == 0) {
    Serial.println("[CALL] SMS ACK with no input identifier - ignored to prevent all-acknowledge bug");
    return true;
  }

  // ---------------------------------------------------------------
  // 1. Try matching by input identifier first (DI1, AI1, etc.)
  // ---------------------------------------------------------------
  {
    size_t idx = 0;
    String upperName = inputName;
    upperName.toUpperCase();
    if (upperName.startsWith("DI")) {
      String num = upperName.substring(2);
      if (num.length() > 0) {
        idx = (size_t)num.toInt();
        if (idx >= 1 && idx <= DIGITAL_INPUT_COUNT) {
          idx -= 1;
          SystemSnapshot snap = Shared_getSnapshot();
          if (snap.digitalInputs[idx] != 0) {
            DigitalInputConfig cfg = {};
            Shared_getDigitalInputConfig(idx, cfg);
            Serial.printf("[CALL] SMS ACK DI%u (%s) from %s\n",
                          (unsigned)idx + 1, cfg.name, sender.c_str());
            CallManager_ack(ALARM_SRC_DI, idx);
            return true;
          }
        }
      }
    } else if (upperName.startsWith("AI")) {
      String num = upperName.substring(2);
      if (num.length() > 0) {
        idx = (size_t)num.toInt();
        if (idx >= 1 && idx <= ANALOG_INPUT_COUNT) {
          idx -= 1;
          bool active = false;
          Shared_getAIAlarmState(idx, active);
          if (active) {
            AnalogInputConfig cfg = {};
            Shared_getAnalogInputConfig(idx, cfg);
            Serial.printf("[CALL] SMS ACK AI%u (%s) from %s\n",
                          (unsigned)idx + 1, cfg.name, sender.c_str());
            CallManager_ack(ALARM_SRC_AI, idx);
            return true;
          }
        }
      }
    }
  }

  // ---------------------------------------------------------------
  // 2. Fall back to matching by configured name
  // ---------------------------------------------------------------

  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
    DigitalInputConfig cfg = {};
    if (!Shared_getDigitalInputConfig(i, cfg)) continue;
    if (String(cfg.name).equalsIgnoreCase(inputName)) {
      SystemSnapshot snap = Shared_getSnapshot();
      if (snap.digitalInputs[i] != 0) {
        Serial.printf("[CALL] SMS ACK DI%u (%s) from %s\n",
                      (unsigned)i + 1, cfg.name, sender.c_str());
        CallManager_ack(ALARM_SRC_DI, i);
        return true;
      }
    }
  }

  for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) {
    AnalogInputConfig cfg = {};
    if (!Shared_getAnalogInputConfig(i, cfg)) continue;
    if (String(cfg.name).equalsIgnoreCase(inputName)) {
      bool active = false;
      Shared_getAIAlarmState(i, active);
      if (active) {
        Serial.printf("[CALL] SMS ACK AI%u (%s) from %s\n",
                      (unsigned)i + 1, cfg.name, sender.c_str());
        CallManager_ack(ALARM_SRC_AI, i);
        return true;
      }
    }
  }

  Serial.printf("[CALL] SMS ACK '%s' — no matching active alarm\n", inputName.c_str());
  return true;
}

// ---------------------------------------------------------------------------
// State machine tick — called every ~25 ms from Modem task
// ---------------------------------------------------------------------------
void CallManager_tick() {
  // Always drain UART into urcBuf so URCs are never lost between ticks
  drainToUrcBuf();

  switch (state) {

    case CS_IDLE: {
      if (queueEmpty()) return;
      // Skip ACKed entries
      while (!queueEmpty()) {
        bool acked = false;
        Shared_getAlarmAck(callQueue[callQueueHead].src,
                           callQueue[callQueueHead].index, acked);
        if (!acked) break;
        callQueueHead = (callQueueHead + 1) % CALL_QUEUE_SIZE;
        Serial.println("[CALL] Skipping — alarm already ACKed");
      }
      if (queueEmpty()) return;
      if (!dequeue(currentCall)) return;

      bool acked = false;
      Shared_getAlarmAck(currentCall.src, currentCall.index, acked);
      if (acked) return;

      Serial.printf("[CALL] Dialing %s\n", currentCall.number);
      dial(currentCall.number);
      setState(CS_DIALING);
      break;
    }

    case CS_DIALING: {
      // Wait 3 s for the modem to register the outgoing call before polling CLCC
      if (elapsed() >= 3000) setState(CS_WAITING_ANSWER);
      break;
    }

    case CS_WAITING_ANSWER: {
      int cs = getCallStatus();
      if (cs == 1) {
        Serial.printf("[CALL] Answered by %s — waiting for voice path\n", currentCall.number);
        setState(CS_CALL_ANSWERED);
        return;
      }
      if (elapsed() >= (unsigned long)ringTimeoutS * 1000UL) {
        Serial.printf("[CALL] Ring timeout for %s\n", currentCall.number);
        hangUp();
        setState(CS_INTER_CALL_DELAY);
      }
      break;
    }

    case CS_CALL_ANSWERED: {
      // Wait 3s non-blocking for the voice bearer to fully open.
      // AT+QAUDPLAY returns 903 if attempted before the voice path is ready.
      if (elapsed() < 3000) {
        // Check if remote hung up during the wait
        if (getCallStatus() == 0) {
          Serial.println("[CALL] Remote hung up before TTS");
          setState(CS_INTER_CALL_DELAY);
        }
        return;
      }

      // Enable DTMF detection now that call is active and settled
      sendAT("AT+QTONEDET=1,0", 1000, false);

      Serial.printf("[CALL] Voice path ready — playing TTS\n");
      playTTS(currentCall.message);
      setState(CS_PLAYING_TTS);
      break;
    }

    case CS_PLAYING_TTS: {
      // Check for DTMF ACK — any keypress during playback
      if (consumeDTMF()) {
        Serial.println("[CALL] DTMF ACK during TTS");
        sendAT("AT+QAUDSTOP", 1000, false); // stop playback
        CallManager_ack(currentCall.src, currentCall.index);
        hangUp();
        setState(CS_IDLE);
        return;
      }

      // Check if remote hung up
      int cs = getCallStatus();
      if (cs == 0) {
        Serial.println("[CALL] Remote hung up during TTS");
        setState(CS_INTER_CALL_DELAY);
        return;
      }

      // 30s window: covers QWTTS generation (~5s) + playback of long messages
      if (elapsed() >= 30000) {
        Serial.println("[CALL] TTS playback window elapsed — hanging up");
        sendAT("AT+QAUDSTOP", 1000, false);
        hangUp();
        setState(CS_INTER_CALL_DELAY);
      }
      break;
    }

    case CS_HANGING_UP: {
      if (elapsed() >= 2000) setState(CS_INTER_CALL_DELAY);
      break;
    }

    case CS_INTER_CALL_DELAY: {
      if (elapsed() >= (unsigned long)interCallDelayS * 1000UL) {
        setState(CS_IDLE);
      }
      break;
    }
  }
}
