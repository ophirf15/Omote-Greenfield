# Home Assistant setup

1. In Home Assistant: **Profile → Security → Long-lived access tokens** → Create token.
2. On device setup page (**http://omote.local/setup.html**): enter HA URL (e.g. `http://homeassistant.local:8123`) and token, then save.
3. Run the external editor from this repo:
   - `cd tools/web-editor`
   - `python -m http.server 8080`
4. Open **http://localhost:8080**, set **Device URL** to `http://omote.local`, then connect.
5. In **Layout**, add pages/buttons and deploy. The device reboots and applies `/config.json`.

## IR library

- Use **Learn IR** in the editor, then **Save capture to library**.
- IR library entries are stored on device in `/irlib.json`.
- IR actions can reference a library entry (`ir_id`) so physical keys and touch buttons reuse the same learned code.

## Optional: Omote Remote integration (HACS)

Install `ha-integration/omote_remote` for mDNS discovery, battery sensor, and automation triggers when buttons are pressed.
