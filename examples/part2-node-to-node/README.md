# examples/part2-node-to-node

Code artifact for LogicFrenzy series Part 2 ("Two boards talking, done
right"): a minimal two-board pair using the frame codec and RX-callback ring
discipline.

- `sender/sender.ino` — reads a (fake) sensor value, encodes it as a
  self-describing DATA field tuple (`core/pl_fields.h`), wraps it in a frame
  (`core/pl_frame.h`), unicasts it.
- `receiver/receiver.ino` — RX callback does nothing but copy frame + MAC
  into `core/pl_ring.h`'s `UplinkRing`; `loop()` drains the ring, decodes,
  and prints. This split is the article's point: parsing never happens in
  callback context.

No pairing, no channel handling, no app-level acks — Part 3 covers
discovery/channel lock-in, Part 5 covers the CMD_ACK loop. This example is
scoped to just the wire format and the callback-safety pattern.

The frame/field codec these sketches call is fully covered by
`test/test_frame.cpp` and `test/test_fields.cpp` running against
`transport/fake/` — that's where the logic is actually verified. The
`.ino` files themselves are not compiled in this repo's CI (no ESP32
Arduino toolchain in the host-native test environment); real-hardware
compilation starts at Phase 4 (PLAN.md).

Article artifact — keep it minimal and readable, it appears in print
(CLAUDE.md).
