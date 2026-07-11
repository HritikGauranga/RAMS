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

static CallState  state          = CS_IDLE;
static unsigned long stateEnteredMs = 0;
static CallEntry  currentCall    = {};
static uint16_t   ringTimeoutS   = 30;
static uint16_t   interCallDelayS = 5;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static String sendAT(const String &cmd, unsigned long timeout, bool silent = true) {
  if (!gSerial) return "";
  while (gSerial->available()) gSerial->read();
  if (!silent) Serial.println("[CALL] >> " + cmd);
  gSerial->println(cmd);
  delay(80);
  String resp = "";
  unsigned long start = millis();
  unsigned long lastByte = start;
  while (millis() - start < timeout) {
    while (gSerial->available()) {
      resp += (char)gSerial->read();
      lastByte = millis();
      delay(2);
    }
    if (resp.length() > 0 && millis() - lastByte > 100) break;
    delay(10);
  }
  resp.trim();
  if (!silent) Serial.println("[CALL] << " + (resp.length() ? resp : "[NO RESPONSE]"));
  return resp;
}

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
    // Queue full — drop oldest
    callQueueHead = (callQueueHead + 1) % CALL_QUEUE_SIZE;
  }
  callQueue[callQueueTail] = e;
  callQueueTail = next;
}

static void clearQueueForAlarm(AlarmSource src, size_t index) {
  // Walk the ring buffer and invalidate matching entries
  size_t i = callQueueHead;
  while (i != callQueueTail) {
    if (callQueue[i].valid && callQueue[i].src == src && callQueue[i].index == index) {
      callQueue[i].valid = false;
    }
    i = (i + 1) % CALL_QUEUE_SIZE;
  }
  // Compact: rebuild without invalid entries
  CallEntry tmp[CALL_QUEUE_SIZE] = {};
  size_t count = 0;
  i = callQueueHead;
  while (i != callQueueTail) {
    if (callQueue[i].valid) tmp[count++] = callQueue[i];
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

// Play TTS via AT+CTTS (EC200U supports text-to-speech during a call)
static void playTTS(const char *text) {
  String cmd = String("AT+CTTS=1,\"") + String(text) + "\"";
  sendAT(cmd, 3000, false);
}

// Hang up
static void hangUp() {
  sendAT("ATH", 3000, false);
}

// Dial
static void dial(const char *number) {
  String cmd = String("ATD") + String(number) + ";";
  sendAT(cmd, 5000, false);
}

// Check call status via AT+CLCC
// Returns: 0=no call, 1=active, 2=held, 3=dialing, 4=alerting, 5=incoming
static int getCallStatus() {
  String resp = sendAT("AT+CLCC", 2000);
  if (resp.indexOf("+CLCC:") < 0) return 0; // no active call
  // +CLCC: <idx>,<dir>,<stat>,<mode>,<mpty>
  int comma1 = resp.indexOf(',');
  int comma2 = resp.indexOf(',', comma1 + 1);
  if (comma1 < 0 || comma2 < 0) return 0;
  String statStr = resp.substring(comma1 + 1, comma2);
  statStr.trim();
  return statStr.toInt();
}

// Check for DTMF input via AT+QTONEDET or URC "+DTMF:"
static bool checkDTMF() {
  if (!gSerial) return false;
  // Read any pending URCs
  String buf = "";
  while (gSerial->available()) {
    buf += (char)gSerial->read();
    delay(1);
  }
  if (buf.length() > 0) {
    Serial.print("[CALL] URC: "); Serial.println(buf);
    if (buf.indexOf("+DTMF:") >= 0 || buf.indexOf("+QTONEDET:") >= 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CallManager_init(HardwareSerial &serial) {
  gSerial = &serial;
  // Enable DTMF detection URC
  sendAT("AT+QTONEDET=1", 2000, false);
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
  // If current call is for this alarm, hang up and go idle
  if (state != CS_IDLE && currentCall.src == src && currentCall.index == index) {
    hangUp();
    setState(CS_IDLE);
    Serial.printf("[CALL] ACK received — current call terminated (src=%d idx=%u)\n",
                  (int)src, (unsigned)index);
  }
  clearQueueForAlarm(src, index);
  Serial.printf("[CALL] ACK — cleared call queue for src=%d idx=%u\n",
                (int)src, (unsigned)index);
}

bool CallManager_handleSmsAck(const String &sender, const String &body) {
  // Format: ACK <input_name>  (case-insensitive)
  String upper = body;
  upper.toUpperCase();
  upper.trim();
  if (!upper.startsWith("ACK ") && upper != "ACK") return false;

  String inputName = body.substring(4); // preserve original case for matching
  inputName.trim();

  // Search DI configs
  for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
    DigitalInputConfig cfg = {};
    if (!Shared_getDigitalInputConfig(i, cfg)) continue;
    String cfgName = String(cfg.name);
    if (cfgName.equalsIgnoreCase(inputName)) {
      bool active = false;
      // DI alarm is active if digitalInputs[i] != 0 (checked via snapshot)
      SystemSnapshot snap = Shared_getSnapshot();
      active = (snap.digitalInputs[i] != 0);
      if (active) {
        Serial.printf("[CALL] SMS ACK for DI%u (%s) from %s\n",
                      (unsigned)i + 1, cfg.name, sender.c_str());
        CallManager_ack(ALARM_SRC_DI, i);
        return true;
      }
    }
  }

  // Search AI configs
  for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) {
    AnalogInputConfig cfg = {};
    if (!Shared_getAnalogInputConfig(i, cfg)) continue;
    String cfgName = String(cfg.name);
    if (cfgName.equalsIgnoreCase(inputName)) {
      bool active = false;
      Shared_getAIAlarmState(i, active);
      if (active) {
        Serial.printf("[CALL] SMS ACK for AI%u (%s) from %s\n",
                      (unsigned)i + 1, cfg.name, sender.c_str());
        CallManager_ack(ALARM_SRC_AI, i);
        return true;
      }
    }
  }

  Serial.printf("[CALL] SMS ACK '%s' — no matching active alarm\n", inputName.c_str());
  return true; // consumed the ACK command even if no match
}

void CallManager_tick() {
  switch (state) {

    case CS_IDLE: {
      if (queueEmpty()) return;
      // Peek next entry — skip if already ACKed
      CallEntry next = {};
      while (!queueEmpty()) {
        next = callQueue[callQueueHead];
        bool acked = false;
        Shared_getAlarmAck(next.src, next.index, acked);
        if (!acked) break;
        // Skip this entry
        callQueueHead = (callQueueHead + 1) % CALL_QUEUE_SIZE;
        Serial.println("[CALL] Skipping call — alarm already ACKed");
      }
      if (queueEmpty()) return;
      if (!dequeue(currentCall)) return;

      bool acked = false;
      Shared_getAlarmAck(currentCall.src, currentCall.index, acked);
      if (acked) { setState(CS_IDLE); return; }

      Serial.printf("[CALL] Dialing %s\n", currentCall.number);
      dial(currentCall.number);
      setState(CS_DIALING);
      break;
    }

    case CS_DIALING: {
      // Give modem 2s to register the dial command, then move to waiting
      if (elapsed() >= 2000) setState(CS_WAITING_ANSWER);
      break;
    }

    case CS_WAITING_ANSWER: {
      // Check for DTMF ACK first (shouldn't happen before answer, but be safe)
      if (checkDTMF()) {
        Serial.println("[CALL] DTMF during wait — ACK");
        CallManager_ack(currentCall.src, currentCall.index);
        hangUp();
        setState(CS_IDLE);
        return;
      }

      int cs = getCallStatus();
      if (cs == 0) {
        // Call dropped / not answered
        if (elapsed() >= (unsigned long)ringTimeoutS * 1000UL) {
          Serial.printf("[CALL] No answer from %s — moving to next\n", currentCall.number);
          hangUp();
          setState(CS_INTER_CALL_DELAY);
        }
        return;
      }
      if (cs == 1) {
        // Active (answered)
        Serial.printf("[CALL] Answered by %s — playing TTS\n", currentCall.number);
        playTTS(currentCall.message);
        setState(CS_PLAYING_TTS);
        return;
      }
      // cs == 3 (dialing) or 4 (alerting) — still ringing
      if (elapsed() >= (unsigned long)ringTimeoutS * 1000UL) {
        Serial.printf("[CALL] Ring timeout for %s\n", currentCall.number);
        hangUp();
        setState(CS_INTER_CALL_DELAY);
      }
      break;
    }

    case CS_PLAYING_TTS: {
      // Check for DTMF ACK
      if (checkDTMF()) {
        Serial.println("[CALL] DTMF ACK received during TTS");
        CallManager_ack(currentCall.src, currentCall.index);
        hangUp();
        setState(CS_IDLE);
        return;
      }
      // Check if call dropped
      int cs = getCallStatus();
      if (cs == 0) {
        // Remote hung up
        Serial.println("[CALL] Remote hung up during TTS");
        setState(CS_INTER_CALL_DELAY);
        return;
      }
      // Allow ~10s for TTS playback then hang up
      if (elapsed() >= 12000) {
        Serial.println("[CALL] TTS timeout — hanging up");
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
