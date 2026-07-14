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

See `wokwi/single-board/combined/` for a version of this logic (merged
with the gateway side) that actually runs in the Wokwi simulator — Wokwi
doesn't support different firmware on multiple boards in one project, so
there's no standalone two-board Wokwi project for this file.

## Two security bugs an independent review caught here (2026-07-14)

Neither is hypothetical — both are exploitable by anything else on the
same channel, no special access required:

1. **JOIN_ACK had no authentication at all.** `handle_join_ack()` accepted
   *any* frame shaped like a JOIN_ACK and unconditionally re-pointed
   `gateway_mac_` at whoever sent it — including a broadcast frame, which
   would hijack every node in range simultaneously, even ones already
   working. Fixed two ways at once (D-015, DECISIONS.md): JOIN_ACK now
   echoes the provisioning token, checked with `join_ack_is_authentic()`
   before anything else runs; and `NodePairingState::on_join_ack()` now
   refuses to re-pair a node that's already paired, full stop, regardless
   of the token check — a legitimate node only ever expects a JOIN_ACK
   while unpaired in the first place.
2. **`handle_cmd()` executed any CMD frame without checking who sent it.**
   `src` was only used to address the reply, never compared against the
   node's paired gateway MAC — so any radio on the channel could unicast a
   forged CMD and get it executed and acked as if from the real gateway.
   Fixed with one `mac_equal()` check before anything else in the handler
   runs.

Both are in `handle_cmd`/`handle_join_ack` above — read them alongside
this note rather than trusting the summary; the actual checks are a few
lines each.
