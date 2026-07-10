# wokwi/gateway-node

Wokwi simulator project for PLAN.md Phase 4's "gateway + 2 nodes end-to-end
against local Mosquitto" deliverable — scaled to 1 gateway + 1 node for the
web IDE, against a public broker instead of a local one (see below for
why). Radio is idealized here, not an RF testbed (TRD.md §9, D-009) — this
validates integration logic (join, uplink → publish, downlink → ack), not
real channel/interference behavior.

## What's in here

- `gateway/`, `node/` — **mechanically generated** flattened copies of
  `../../gateway/gateway.ino` and `../../node/node.ino` plus their
  transitive `core/`/`transport/`/`gateway/` header dependencies, with
  every `#include "../core/pl_x.h"`-style relative include rewritten to
  `#include "pl_x.h"` (wokwi.com's browser IDE needs same-directory
  includes). `gateway.ino` additionally gets its WiFi/MQTT config
  constants swapped for Wokwi-specific defaults.
- `sync.py` — the script that generates both folders. **Don't hand-edit
  files inside `gateway/` or `node/`** — re-run `python3 sync.py` from
  this directory after changing the real source instead; a hand-edit
  gets silently overwritten on the next run. It fails loudly
  (`RuntimeError`) if the source files it expects to find and substitute
  have changed shape enough that its patterns no longer match.
- `diagram.json` — two ESP32 DevKit boards, `gateway` and `node`.

Both flattened copies are syntax-checked the same way as the real source
(`g++ -std=c++11 -Wall -Wextra -fsyntax-only`, zero warnings) — see
`gateway/README.md` for exactly what that does and doesn't prove.

## Config: why a public broker, not your local Mosquitto

Wokwi's simulator runs in the cloud; your Mosquitto container is on your
LAN. The simulated gateway can't reach `localhost` or your LAN IP from
there. `gateway/gateway.ino` in this folder is pre-configured for:

- WiFi: `Wokwi-GUEST` (Wokwi's built-in open network with internet access
  — no credentials needed)
- MQTT: `test.mosquitto.org:1883` (a public test broker — anyone can
  publish/subscribe there, so don't send anything sensitive; expect other
  people's traffic if you subscribe to a wildcard)

Once you've confirmed the flow works here, point `kMqttHost` back at your
own broker for real hardware.

## One thing I couldn't verify: assigning two different sketches to two boards

`diagram.json` has both boards in one project, which is necessary — I'm
fairly confident two *separate* Wokwi projects (two browser tabs) would
each be an isolated simulation with no shared virtual radio, so ESP-NOW
between them wouldn't actually work even if both compiled and ran fine
independently.

What I'm **not** certain of is the exact current mechanism wokwi.com's
browser IDE uses to run two *different* compiled sketches against two
different parts in one project — that's changed across Wokwi versions and
I can't check their live UI from here. A few things to try, roughly in
order:

1. Check whether your project's file panel lets you group files per board
   (some newer Wokwi project templates support a folder-per-chip layout).
2. Search Wokwi's own example library for "ESP-NOW" — they've had official
   multi-board ESP-NOW demos, and copying their project structure (then
   swapping in the code from `gateway/` and `node/` here) is likely more
   reliable than guessing at the current mechanism from outside.
3. If neither works, the fallback that's guaranteed to work: create two
   *separate* single-board Wokwi projects first (one gateway, one node)
   purely to confirm each compiles and boots cleanly in isolation (WiFi
   connects, "MQTT connected" prints on the gateway, no crash/reboot loop
   on either) — that alone is a real, useful checkpoint — before spending
   more time on the multi-board wiring specifically.

## What to expect once it's running

- Gateway: `WiFi connected` → `MQTT connected` → (once the node joins)
  `joined device_id=0`.
- Node: `broadcasting JOIN_REQ` → `paired: device_id=0 channel=...` → a
  `send_data()` call roughly every 5s.
- Subscribe from your machine to watch it live:
  `mosquitto_sub -h test.mosquitto.org -t 'pulsecore/acme/#' -v`
  You should see `pulsecore/acme/0/temperature` update every ~5s, plus
  `pulsecore/acme/gateway/ring_overflow` and `pulsecore/acme/0/loss_rate`
  every ~30s (health metrics — see `core/pl_loss_tracker.h`,
  `core/pl_topics.h`).
- Test downlink: `mosquitto_pub -h test.mosquitto.org -t
  'pulsecore/acme/0/cmd' -m 'x'` — the node's Serial output should show
  `executing cmd_id=...` and `pulsecore/acme/0/cmd_status` should publish
  `ok` shortly after.
