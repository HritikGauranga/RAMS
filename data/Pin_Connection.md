4G Modem Connection

ESP32 GPIO  |  4G Modem |             Discreption
Pin 32      |   Pin 16  |  MODEM_PWRKEY(Power pin to trigger the modem before using it)
Pin 16      |   Pin 8   |  Pin is 16 RX of ESP32 and Pin 8 is TX of Modem
pin 17      |   Pin 9   |  Pin is 16 RX of ESP32 and Pin 8 is TX of Modem
---         |   Pin 1   |  +ve of Modem(9v DC)
---         |  Pin 6/7  |  -ve of Modem(GND)
GND         |  pin 6/7  |  ESP GND to Modem GND

W5500 Connection

ESP32 GPIO  |   W5500   |             Discreption
Pin 3v3     |   V-Pin   |   3v3 of ESP32 to W5500 V pin
Pin GND     |   G-Pin   |   ESP32 GND to W5500 GND
Pin 23      |     MO    |  Pin 23 of ESP32 to MOSI pin of W5500
Pin 19      |     MI    |  Pin 19 of ESP32 to MISO pin of W5500
Pin 18      |    SCK    |  Pin 18 of ESP32 to Serial Clock pin of W5500
Pin 5       |    CS     |  Pin 5 of ESP32 to Chip Select pin of W5500
pin 14      |    RST    |  Pin 14 of ESP32 to Reset pin of W5500

FT232 Connection

ESP32 GPIO  |  FT232    |      Discreption
GND         |   GND     |    GND of ESp32 to GND of FT232
Pin SD3     |   RX      |   Pin SD3(Tx) of ESP32 to RX of FT232
Pin SD2     |   TX      |   Pin SD2(Rx) of ESP32 to TX of FT232

Latching Switch Connection

ESP32 GPIO  |  LSwitch  |       Discreption
GND         | Terminal1 | GND of ESP32 to Termial1 of Latching Switch 
Pin 33      | Terminal2 | Pin 33 of ESP32 to Terminal2 of Latching Switch
Pin 04      | Terminal3 | Pin 04 of ESP32 to Terminal3 of Latching Switch for AP mode LED +ve
GND         | Terminal4 | GND of ESP32 to Terminal4 of Latching Switch for AP mode LED -ve

Modem Init Status LED

ESP32 GPIO  |  LED      |       Discreption
Pin 02      | LED +ve   | Pin 02 of ESP32 to modem init success LED +ve
GND         | LED -ve   | GND of ESP32 to modem init success LED -ve

