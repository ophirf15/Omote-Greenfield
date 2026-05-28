# Omote Remote — Home Assistant custom component

Copy `custom_components/omote_remote` into your Home Assistant `config/custom_components/` directory, restart HA, then add **Omote Remote** via Settings → Devices & services.

Discovery uses mDNS (`omote.local`). The integration polls device status and exposes button-press events for automations. Button mappings remain on the Omote web UI.
