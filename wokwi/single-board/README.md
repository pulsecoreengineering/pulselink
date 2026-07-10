# wokwi/single-board

Wokwi simulator project for PLAN.md Phase 4's Wokwi deliverable — reshaped
after confirming Wokwi doesn't support running different firmware on
multiple boards in one project (the earlier `wokwi/gateway-node/` attempt
assumed it did and was removed).

## The approach

One simulated ESP32 runs **both** gateway and node logic in the same
process, connected by `transport/fake/pl_fake_transport.h`'s
`FakeMedium`/`FakeTransport` instead of a real ESP-NOW radio — the exact
same in-process fake medium 127 host-native tests already exercise
(`test/test_gateway_bridge.cpp`, `test_join_flow.cpp`, `test_cmd_flow.cpp`,
...). This works cleanly *because* `Transport` is an interface
(`transport/pl_transport.h`): all the join/uplink/downlink logic only ever
calls `send_unicast()`/`send_broadcast()`/`receive()`/`local_mac()` on
whatever `Transport` it's handed. Swapping `EspNowTransport` for
`FakeTransport` is a type change at construction, not a logic change.

WiFi and MQTT stay completely real — that's the part Wokwi can actually
add on top of what host-native tests already prove. This setup validates:
a real WiFi connection, a real MQTT session over real TCP/IP, real
`millis()`-based timing, and that the whole codebase genuinely compiles
and runs against a real (simulated) ESP32 toolchain — stronger than my
`g++ -fsyntax-only` stub-header check, which can't catch a wrong real
Arduino/ESP-IDF API signature.

What it does **not** validate: anything about the real ESP-NOW radio
(channel behavior, real MAC-layer ack timing/loss, range) — that's
`transport/espnow/pl_espnow_transport.h`'s job, gated on real hardware
(PLAN.md's remaining Phase 4 hardware-rig item).

## What's in here

- `combined/combined.ino` — hand-written (not mechanically generated —
  see `sync.py`'s docstring for why merging two sketches needs real
  judgment a regex script can't safely make). Same logic as
  `gateway/gateway.ino` + `node/node.ino`, functions prefixed `gw_`/`node_`
  to avoid name collisions in one file.
- `sync.py` — copies the transitive `core/`/`transport/`/`gateway/` header
  dependencies into `combined/`, flattening `#include "../core/pl_x.h"`
  to `#include "pl_x.h"` (wokwi.com needs same-directory includes). Only
  the headers are generated; `combined.ino` itself is maintained by hand.
  Re-run after changing any of the real source headers:
  `python3 wokwi/single-board/sync.py`
- `diagram.json` — one ESP32 DevKit board.

## A bug this project caught before ever running in Wokwi

Writing `combined.ino` (and re-reading `gateway.ino` side by side with it)
surfaced a real bug in the already-pushed gateway firmware: **nothing ever
called `Registry::set_field_name()`**. `find_field_name()` would therefore
always return `nullptr` for every field, so the uplink path would silently
never publish a single DATA field to MQTT — join would work, DATA frames
would arrive, and nothing would ever show up on `pulsecore/.../temperature`.

127 host-native tests never caught it because `test_gateway_bridge.cpp`
and `test_health_metrics.cpp` provision the field mapping directly in test
setup, bypassing the join path — they tested "does bridging work once a
field is mapped," not "does joining actually map a field." That's the gap
firmware glue code often falls into: it's not library logic, so it's not
unit-testable, and the only way to catch it is exactly what happened here
— writing (or re-reading) the orchestration code end to end. Fixed in both
`gateway/gateway.ino` and this project's `combined.ino`:
`handle_join_req()` now calls `g_registry.set_field_name(ack.device_id,
kFieldIdTemperatureC10, "temperature")` right after a successful join.

## Config: why a public broker, not your local Mosquitto

Wokwi's simulator runs in the cloud; your Mosquitto container is on your
LAN — the simulated board can't reach `localhost` or your LAN IP from
there. `combined.ino` is pre-configured for:

- WiFi: `Wokwi-GUEST` (Wokwi's built-in open network with internet access)
- MQTT: `test.mosquitto.org:1883` (a public test broker — don't publish
  anything sensitive; expect other people's traffic on wildcard
  subscriptions)

Point `kMqttHost` back at your own broker once you're testing on real
hardware instead of in Wokwi.

## What to expect once it's running

Serial output, roughly in order:
```
WiFi connected
gateway: MQTT connected
node: broadcasting JOIN_REQ
gateway: joined device_id=0
node: paired: device_id=0 channel=6
gateway: publishing pulsecore/acme/0/temperature
```
then a new `gateway: publishing ...` roughly every 5s.

Watch it from your machine:
```
mosquitto_sub -h test.mosquitto.org -t 'pulsecore/acme/#' -v
```
You should see `pulsecore/acme/0/temperature` update every ~5s, plus
`pulsecore/acme/gateway/ring_overflow` and `pulsecore/acme/0/loss_rate`
every ~30s. Test downlink:
```
mosquitto_pub -h test.mosquitto.org -t 'pulsecore/acme/0/cmd' -m x
```
— Serial should show `node: executing cmd_id=...`, and
`pulsecore/acme/0/cmd_status` should publish `ok` shortly after.
