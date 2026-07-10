# transport/espnow

Real ESP-NOW transport backend (ESP32-only). Implements `pulselink::Transport`
against the ESP-IDF/Arduino ESP-NOW API.

Requires (TRD.md §2, §4.1):
- `esp_wifi_set_ps(WIFI_PS_NONE)` mandatory on the gateway.
- RX callback runs in WiFi task context: memcpy frame + sender MAC into the
  ring buffer and return immediately. No parsing, no publishing, no
  dispatch from callback context.
- Unicast peer table management (join/pairing owns peer add/remove).

Built starting Phase 4 (PLAN.md) — this is where hardware bring-up begins.
