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

NVS read/write in the `.ino` files is pseudocode (commented out) — the
byte layout it would persist is `NodePairingState::serialize()`, which
`test/test_join.cpp` round-trips through a buffer to prove it survives a
simulated reboot. Real flash I/O, and the NVS-backed `RegistryStorage` on
the gateway side, land in Phase 4 hardware bring-up (PLAN.md).

The join protocol logic (`core/pl_join.h`, `core/pl_registry.h`) is fully
covered by `test/test_join.cpp`, `test/test_registry.cpp`, and
`test/test_join_flow.cpp` — including the provisioning-token check,
join-spam rate limiting, and the channel-change-recovery scenario above.

Article artifact — keep it minimal and readable, it appears in print
(CLAUDE.md).
