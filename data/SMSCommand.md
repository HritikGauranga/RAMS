# RAMS Guardian Mini — SMS Command Reference

---

## Authorization

All commands must be sent from a number saved in the **Contact Config** (recipients list).
Commands from unknown numbers are silently ignored.

Relay control commands additionally require a **PIN** configured in System Config → SIM Configuration.

---

## Query Commands

These commands can be sent as plain SMS text (case-insensitive).

Command            | Response
-------------------|----------------------------------------------------------
`GET STATUS`       | Site name, serial number, uptime, active alarm count, 4G signal strength, relay states
`GET ALARM`        | Lists all currently active DI and AI alarms with names and values
`GET INPUT`        | DI1–DI4 alarm/normal state + AI1–AI2 engineering values with alarm status
`GET RELAY`        | DO1–DO2 names and current ON/OFF state with trigger source (S=SMS, A=Alarm)
`GET IP%<PIN>`     | Ethernet IP address, DHCP mode, gateway IP

### GET IP Example
```
GET IP%1234
```
Response:
```
Site: MySite
IP Address: 192.168.8.200
DHCP: Disabled
Gateway: 192.168.8.1
```

---

## Relay Control Command

Controls a relay output by name. PIN and relay name must match exactly as configured.

### Format
```
Set Relay%<PIN>,<RelayName>,<ON|OFF>,<seconds>%
```

### Parameters

Parameter    | Description
-------------|-------------------------------------------------------------
`PIN`        | Relay control PIN set in System Config → SIM Configuration
`RelayName`  | Relay name as configured in DO Config (case-insensitive)
`ON` / `OFF` | Desired state
`seconds`    | Duration in seconds before auto OFF (0 or blank = permanent)

### Examples

Command                            | seconds | Result
-----------------------------------|---------|----------------------------------------
`Set Relay%1234,Siren,ON,0`        | 0       | Permanently ON
`Set Relay%1234,Siren,ON,`         | blank   | Permanently ON
`Set Relay%1234,Siren,ON,30`       | 30      | ON for 30s then auto OFF
`Set Relay%1234,Siren,OFF,0`       | any     | Permanently OFF, cancels any active pulse

### Notes
- Relay must have **SMS Control** enabled in DO Config
- Sender must be in the relay's **selected contacts** bitmask
- If a pulse timer is active and a new ON command is sent, the timer resets to the new duration
- Alarm-held relays (triggered by DI/AI alarm) can be overridden by SMS command

---

## Contact Assignment Commands

These commands manage per-input notification assignments for DI and AI inputs.
Only contacts already saved in the Web UI Contact Config page may be assigned.

### ADD Contact Assignment
Adds or updates an existing contact assignment for a specific DI/AI input.

#### Format
```
ADD%<PIN>%<PHONE_NUMBER>%<INPUT_NO>%<OPTIONS>
```

#### Parameters
- `PIN` — System PIN from Network Configuration / SIM Configuration
- `PHONE_NUMBER` — Exact saved contact phone number from Contact Config
- `INPUT_NO` — `DI1`..`DI4` or `AI1`..`AI2`
- `OPTIONS` — notification flags, any order: `V` = Voice, `S` = SMS

#### Rules
- `V` and/or `S` are required. If neither is provided, the command is rejected.
- Phone number comparison is exact; `+919876543210` and `9876543210` are different.
- Existing contact must already be present in the Contact List.
- If the contact is already assigned to the input, the existing assignment is updated instead of duplicated.
- Maximum 5 contacts per input. If full, the command returns `ERROR: Maximum contacts reached`.
- Changes persist to storage and appear in the Web UI after refresh.

#### Examples
```
ADD%1234%+918192738217%DI1%V%S
```
Enable both Voice and SMS for contact +918192738217 on DI1.

```
ADD%1234%+918192738217%AI1%S
```
Enable SMS only for contact +918192738217 on AI1.

#### Error responses
- `ERROR: Invalid PIN`
- `ERROR: Contact does not exist in Contact List`
- `ERROR: Maximum contacts reached`

### DEL Contact Assignment
Removes a contact assignment from a specific input.

#### Format
```
DEL%<PIN>%<PHONE_NUMBER>%<INPUT_NO>
```

#### Behavior
- Validates PIN.
- If the assignment exists, it is removed and the change is saved.
- If no assignment exists for the contact/input, the command is ignored silently.

#### Example
```
DEL%1234%+918192738217%DI1
```

### LST Contact Assignment
Lists assigned contacts for a single input.

#### Format
```
LST%<INPUT_NO>
```

#### Behavior
- Returns only currently assigned contacts for the requested input.
- Includes contact name, phone number, Voice status, and SMS status.
- Shows count of assigned contacts and the maximum of 5.

#### Example response
```
DI1 Contacts 2/5

1. Rahul
   +918192738217
   Voice: Yes
   SMS: Yes

2. Amit
   +919999999999
   Voice: No
   SMS: Yes
```

#### Empty assignment response
```
DI1 Contacts 0/5

No contacts assigned
```

---

## Automatic SMS Alerts (sent by device, not triggered by user)

These are sent automatically when alarm conditions are met — no user command needed.

Event                  | Trigger
-----------------------|----------------------------------------------------------
**DI Alarm**           | Digital input enters alarm state after TTA (time-to-alarm) expires
**DI Return**          | Digital input returns to normal after TTR (time-to-return) expires
**AI Alarm**           | Analog input crosses set point after TTA expires
**AI Return**          | Analog input crosses reset point after TTR expires
**Heartbeat / Status** | Scheduled status SMS (configured in System Config → Status Message)

Recipients for each alert are set per-input in DI Config / AI Config using the contact bitmask.

---

## Notes

- DI3 and DI4 are not physically connected on the current prototype — they will always show Normal
- AI inputs simulate 4–20mA via a potentiometer on GPIO34 (AI1) and GPIO35 (AI2)
- Relay outputs are active LOW (LED on = relay ON = GPIO LOW)
