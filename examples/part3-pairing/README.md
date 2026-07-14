# examples/part3-pairing

Code artifact for LogicFrenzy series Part 3 ("The channel problem &
pairing"): broadcast discovery handshake, NVS persistence, re-scan
fallback.

- `node_join/node_join.ino` — first boot broadcasts JOIN_REQ; a JOIN_ACK
  persists gateway MAC + channel so every later boot skips straight to
  unicast (`core/pl_join.h`'s `NodePairingState`). Enough consecutive
  unicast failures wipes that pairing and falls back to broadcast
  discovery again — no separate gateway-side channel-change mechanism
  exists; this fallback *is* the recovery (TRD.md §5.1 point 4, D-008).
- `gateway_join/gateway_join.ino` — RX callback still only copies frame +
  MAC into a ring; `GatewayJoinHandler` (token check + rate limiting +
  registry) runs from `loop()`, same callback discipline as Part 2.
- `host_demo.cpp` — the same join flow, both ends in one process over
  `transport/fake/`:
  ```
  cmake --build build --target part3_host_demo
  ./build/examples/part3-pairing/part3_host_demo
  ```
  It runs fresh join, then simulates a router channel change (the node's
  unicasts to its paired gateway start failing), watches
  `PULSELINK_MAX_UNICAST_FAILURES` consecutive failures trigger
  re-discovery, and shows the node successfully rejoining.

NVS read/write in `node_join.ino` is pseudocode (commented out) — the byte
layout it would persist is `NodePairingState::serialize()`, which
`test/test_join.cpp` round-trips through a buffer to prove it survives a
simulated reboot. Real flash I/O landed on the gateway side once
`gateway/pl_nvs_registry_storage.h` was written (Phase 4) — see
`gateway/README.md`; node-side NVS is still pseudocode here since node
firmware itself is Phase 5.

The join protocol logic (`core/pl_join.h`, `core/pl_registry.h`) is fully
covered by `test/test_join.cpp`, `test/test_registry.cpp`, and
`test/test_join_flow.cpp` — including the provisioning-token check,
join-spam rate limiting, and the channel-change-recovery scenario above.
Both `.ino` files also syntax-check clean with `g++ -fsyntax-only` against
stub Arduino/ESP-NOW headers (see `gateway/README.md` for what that does
and doesn't prove).

**Updated 2026-07-14 (D-015, DECISIONS.md):** an independent security
review found that JOIN_ACK originally carried no authentication at all —
`node_join.ino` accepted any frame shaped like a JOIN_ACK and re-pointed
its gateway MAC at whoever sent it. `JoinAckPayload` now echoes the
provisioning token; `node_join.ino`'s `on_recv` checks it with
`join_ack_is_authentic()` before trusting anything in the frame, and
`NodePairingState::on_join_ack()` itself now refuses to re-pair a node
that's already paired. `host_demo.cpp`'s channel-change simulation had to
change too: it used to fake "the router moved" by calling `on_join_ack()`
a second time on an already-paired `NodePairingState`, which the new
refusal now blocks (correctly — that's the whole point). It's rebuilt via
`serialize()`/`deserialize()` instead, modeling "the node reboots and
loads a stale pairing from its own flash" rather than a second inbound
JOIN_ACK, which is a real and different thing from the node's own trust
model's point of view.

Article artifact — keep it minimal and readable, it appears in print
(CLAUDE.md).
