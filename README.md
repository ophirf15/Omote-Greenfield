# Omote-Greenfield
A more easily configurable firmware for the Omote Remote
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

MIT — hardware pin reference derived from OMOTE community docs; no GPLv3 firmware code copied.