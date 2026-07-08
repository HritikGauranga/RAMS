# RAMS Guardian Mini — Pin Connection Reference

---

## 4G Modem (EC200U)

ESP32 GPIO  |  4G Modem |  Description
Pin 32      |  Pin 16   |  MODEM_PWRKEY — power trigger pin
Pin 16      |  Pin 8    |  ESP32 RX ← Modem TX
Pin 17      |  Pin 9    |  ESP32 TX → Modem RX
---         |  Pin 1    |  Modem +9V DC supply
---         |  Pin 6/7  |  Modem GND
GND         |  Pin 6/7  |  ESP32 GND → Modem GND (common ground)

---

## W5500 Ethernet

ESP32 GPIO  |  W5500    |  Description
3V3         |  VCC      |  3.3V supply
GND         |  GND      |  Ground
Pin 23      |  MOSI     |  SPI MOSI
Pin 19      |  MISO     |  SPI MISO
Pin 18      |  SCK      |  SPI Clock
Pin 5       |  CS       |  SPI Chip Select
Pin 14      |  RST      |  Reset

---

## FT232 (USB-Serial / Debug)

ESP32 GPIO  |  FT232    |  Description
GND         |  GND      |  Common ground
Pin SD3     |  RX       |  ESP32 TX (SD3) → FT232 RX
Pin SD2     |  TX       |  FT232 TX → ESP32 RX (SD2)

---

## AP Mode Latching Switch

ESP32 GPIO  |  Switch   |  Description
GND         |  Term 1   |  Ground
Pin 33      |  Term 2   |  Button input (BUTTON_PIN)
Pin 4       |  Term 3   |  AP mode status LED +ve (AP_STATUS_LED_PIN)
GND         |  Term 4   |  AP mode status LED -ve

---

## Modem Init Status LED

ESP32 GPIO  |  LED      |  Description
Pin 2       |  LED +ve  |  Modem ready indicator (MODEM_INIT_STATUS_PIN)
GND         |  LED -ve  |  Ground

---

## Digital Inputs (Prototype: latching switches)

ESP32 GPIO  |  Switch   |  Description
Pin 26      |  DI1      |  Digital Input 1 — one terminal to GPIO26, other to 3.3V via 1kΩ
Pin 27      |  DI2      |  Digital Input 2 — one terminal to GPIO27, other to 3.3V via 1kΩ
---         |  DI3      |  Not connected (prototype) — GPIO TBD for final hardware
---         |  DI4      |  Not connected (prototype) — GPIO TBD for final hardware

Note: DI3 and DI4 are configurable in the UI but will always show Normal
until GPIO pins are assigned in IOScanner.cpp (DI_PIN[]).

---

## Analog Inputs (Prototype: potentiometer simulating 4-20mA)

ESP32 GPIO  |  Device   |  Description
Pin 34      |  AI1      |  Analog Input 1 — ADC1_CH6 (0–3.3V, maps to 4–20mA scale)
Pin 35      |  AI2      |  Analog Input 2 — ADC1_CH7 (0–3.3V, maps to 4–20mA scale)

Note: ADC1 pins used to avoid conflict with WiFi (ADC2 disabled when WiFi active).

---

## Digital Outputs / Relays (Prototype: LEDs, Active LOW)

ESP32 GPIO  |  Device   |  Description
Pin 25      |  DO1      |  Digital Output 1 / Relay 1 — Active LOW (LOW=ON, HIGH=OFF)
Pin 13      |  DO2      |  Digital Output 2 / Relay 2 — Active LOW (LOW=ON, HIGH=OFF)

---

## GPIO Summary

GPIO  |  Function
2     |  Modem Init Status LED
4     |  AP Mode Status LED
5     |  W5500 CS
13    |  DO2 (Relay 2)
14    |  W5500 RST
16    |  Modem RX
17    |  Modem TX
18    |  W5500 SCK
19    |  W5500 MISO
23    |  W5500 MOSI
25    |  DO1 (Relay 1)
26    |  DI1
27    |  DI2
32    |  Modem PWRKEY
33    |  Button (AP mode toggle)
34    |  AI1 (ADC1_CH6, input only)
35    |  AI2 (ADC1_CH7, input only)

