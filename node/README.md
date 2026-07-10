# node

Node firmware (ESP32), both sleep profiles (TRD.md §5.2):

- `ALWAYS_ON` — receives CMD any time.
- `WAKE_AND_POLL` — deep sleep -> wake -> send DATA -> ~100 ms RX listen
  window -> receive queued CMD if any -> CMD_ACK -> sleep.

Owns join/discovery and channel-change recovery (TRD.md §5.1): broadcast
`JOIN_REQ` on first boot, persist gateway MAC + channel to NVS, unicast
thereafter; N consecutive unicast send failures triggers re-discovery.

- `node.ino` — the ALWAYS_ON profile, complete: join, periodic DATA send,
  CMD receive/execute/ack with dedupe (`NodeCmdDedupe`), real NVS pairing
  persistence via `Preferences`. Uses `EspNowTransport`
  (`transport/espnow/`) for a correct synchronous MAC-ack — earlier
  examples (`examples/part3-pairing/node_join.ino`) used
  `esp_now_send()`'s return value directly and flagged that as a known
  simplification; this file uses the real fix (D-014, DECISIONS.md).
  It does **not** call `WiFi.begin()` — ESP-NOW doesn't need an AP
  connection, and skipping it is the point (no WiFi credentials or join
  latency on a battery node). That means it relies on already being on
  the gateway's channel; full TRD.md §5.1 channel-scanning discovery
  (broadcast on channels 1-13) isn't implemented, same simplification
  `node_join.ino` already noted.
- `WAKE_AND_POLL` deep-sleep timing is real-hardware-only territory
  (TRD.md §5.2) and isn't attempted here or in Wokwi.

Same verification story as `gateway/gateway.ino` (see `gateway/README.md`
for what "syntax-checked, not compiled" does and doesn't prove): every
piece of logic this file calls is covered by host-native tests against
`transport/fake/`; the file itself is syntax-checked (`g++ -fsyntax-only`,
zero warnings) against stub Arduino/ESP-NOW headers, not compiled against
the real ESP32 Arduino core.

See `wokwi/gateway-node/` for a flattened copy of this file (plus
`gateway.ino`) set up to run both boards together in the Wokwi simulator.
