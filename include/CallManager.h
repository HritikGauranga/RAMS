#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>
#include "Shared.h"

// ---------------------------------------------------------------------------
// CallManager — voice call queue, state machine, TTS, DTMF/SMS ACK
// ---------------------------------------------------------------------------

void CallManager_init(HardwareSerial &serial);

// Queue a voice call sequence for an alarm/return event.
// Called by Modem task after SMS dispatch is complete.
void CallManager_enqueue(const NotificationEvent &ev);

// Acknowledge all pending calls for a given alarm (SMS ACK or DTMF ACK).
// Stops the current call and clears the queue for that alarm.
void CallManager_ack(AlarmSource src, size_t index);

// Tick — call from Modem task loop every 25 ms (non-blocking state machine).
void CallManager_tick();

// Called by Modem task when an incoming SMS line is parsed.
// Returns true if the line was an ACK command and was handled.
bool CallManager_handleSmsAck(const String &sender, const String &body);
