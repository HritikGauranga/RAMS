# LittleFS image examples (ESP32)

This folder is intended to hold an example filesystem image for the ESP32's **LittleFS**.

The firmware stores files at absolute paths beginning with `/`.
When PlatformIO builds the LittleFS image, it copies files from `data/LittleFS/` into the filesystem root.

## Files

- `io_config.bin`  -> `/io_config.bin`
  - Binary struct written by `Shared.cpp` (magic = `IOCF`, version = `6`).
- `contacts.json`  -> `/contacts.json`
  - Authorized recipients + per-recipient flags.
- `gateway.conf`   -> `/gateway.conf`
  - Key/value network configuration used in AP mode.
- `sim_config.json`-> `/sim_config.json`
  - SIM / modem-related configuration.
- `heartbeat.json` -> `/heartbeat.json`
  - Status/heartbeat scheduling configuration.
- `voicecall.json` -> `/voicecall.json`
  - Voice call dispatch configuration.

## Notes

- All values in these examples are **dummy** and should be edited to match your device.
- Phone numbers use dummy values.

