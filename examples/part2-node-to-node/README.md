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
- `host_demo.cpp` — the same sender/receiver logic, but both ends run in
  one process over `transport/fake/` instead of real ESP-NOW, so it builds
  and runs on a laptop with no hardware:
  ```
  cmake --build build --target part2_host_demo
  ./build/examples/part2-node-to-node/part2_host_demo
  ```
  It injects a 5% drop / 2% duplicate rate on purpose, so the printed
  output shows the dedupe layer catching the duplicates and the frame
  count not quite matching the send count — worth reading past "it printed
  numbers" to see what's actually being demonstrated.

No pairing, no channel handling, no app-level acks — Part 3 covers
discovery/channel lock-in, Part 5 covers the CMD_ACK loop. This example is
scoped to just the wire format and the callback-safety pattern.

The frame/field codec these sketches call is fully covered by
`test/test_frame.cpp` and `test/test_fields.cpp` running against
`transport/fake/` — that's where the logic is actually verified, and
`host_demo.cpp` is the same stack made visible rather than asserted on.
The `.ino` files themselves are not compiled in this repo's CI (no ESP32
Arduino toolchain in the host-native test environment); real-hardware
compilation starts at Phase 4 (PLAN.md). They are syntax-checked with
`g++ -fsyntax-only` against hand-written stub headers for `esp_now.h`/
`WiFi.h` (see `gateway/README.md` for the caveat: that catches syntax
errors, not wrong real API signatures) — both sketches compile clean.

Article artifact — keep it minimal and readable, it appears in print
(CLAUDE.md).
