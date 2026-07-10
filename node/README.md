# node

Node firmware (ESP32), both sleep profiles (TRD.md §5.2):

- `ALWAYS_ON` — receives CMD any time.
- `WAKE_AND_POLL` — deep sleep -> wake -> send DATA -> ~100 ms RX listen
  window -> receive queued CMD if any -> CMD_ACK -> sleep.

Owns join/discovery and channel-change recovery (TRD.md §5.1): broadcast
`JOIN_REQ` on first boot, persist gateway MAC + channel to NVS, unicast
thereafter; N consecutive unicast send failures triggers re-discovery.

Built across Phase 4 (always-on) and Phase 5 (sleep profiles, PLAN.md).
