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
Set Relay%<PIN>,<RelayName>,<ON|OFF>,<seconds>
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
