# Omote-Greenfield
A from-scratch, configurable firmware for the OMOTE open universal remote hardware.

This project is designed for the OMOTE hardware platform originally created by Maximilian Kern / the OMOTE Community.

It is not the official OMOTE firmware and is not affiliated with or endorsed by the original OMOTE maintainers unless stated otherwise.

## Credits

- Original OMOTE hardware and firmware project: OMOTE Community
- Original creator/project: Maximilian Kern
- Portions of this project were inspired by or adapted from the original OMOTE firmware.
- Original OMOTE firmware is licensed under GPL-3.0.
=======
# Omote OS

Greenfield firmware for the [OMOTE](https://github.com/OMOTE-Community/OMOTE-Hardware/) universal remote (Rev 1–4, ESP32), with:

- Device-hosted web portal for no-code Home Assistant button mapping
- Runtime LVGL touchscreen UI driven by JSON config
- Direct BLE HID (Google TV / Android TV), IR send/learn, and HA service calls

## Quick start

1. Install [PlatformIO](https://platformio.org/).
2. Copy `firmware/secrets.example.h` to `firmware/src/secrets.h` (optional; WiFi can be set via web UI).
3. Build and flash:

```bash
cd firmware
pio run -t upload
pio run -t uploadfs   # flash web UI (from firmware/data/)
```

First boot: connect to WiFi AP **Omote-Setup**, configure SSID/password, then open **http://omote.local** on your LAN.

4. Connect to WiFi via the setup portal, then open **http://omote.local**.

## Repository layout

| Path | Description |
|------|-------------|
| `firmware/` | ESP32 PlatformIO project |
| `web/` | Config SPA (build → `firmware/data/`) |
| `ha-integration/` | Optional Home Assistant custom component |
| `docs/` | Hardware pins and HA setup |

## License

This project is licensed under GPL-3.0. See `LICENSE`.

Because this firmware includes or adapts portions of the original OMOTE firmware, it is distributed under GPL-3.0.