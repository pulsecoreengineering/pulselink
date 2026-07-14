# gateway

Gateway firmware (ESP32). Bridges the ESP-NOW node cluster to MQTT so
PulseDash sees the standard `pulsecore/{tenant_id}/{device_id}/{field}`
topic tree unchanged.

- `gateway.ino` — the whole thing wired together: join handling (which
  also provisions the field-name map on every successful join — this
  tutorial's fleet is one node type sending one field, so the mapping is
  the same for everyone; see `handle_join_req()`), uplink
  dedupe/loss-tracking/field-bridging, downlink command routing, the
  two-loop ack model, the WAKE_AND_POLL mailbox, health metrics, and the
  `GatewayHsm` backhaul state machine (TRD.md §4.4, D-013):
  ```
  Root
  ├── Connected            (WiFi up + MQTT session live)
  │   ├── Bridging         (normal operation)
  │   └── Draining         (flushing spool after reconnect)
  └── Degraded             (WiFi or broker down)
      ├── ESP-NOW side KEEPS OPERATING
      ├── uplink -> bounded spool (drop-oldest when full)
      └── downlink refused with reason
  ```
- `pl_nvs_registry_storage.h` — `RegistryStorage` backed by Arduino's
  `Preferences` (NVS). The RAM backend (`core/pl_registry.h`) is what
  `test/test_registry.cpp` exercises; this is the same interface,
  persisted.
- `pl_pubsubclient_mqtt.h` — `MqttClient` backed by `PubSubClient`, the
  standard Arduino MQTT library.

Edit the constants at the top of `gateway.ino` (WiFi SSID/password, MQTT
host/port, tenant_id, provisioning token) before flashing.

## Remote access: there is no gateway API, only the broker

The gateway has no HTTP/REST/WebSocket server of its own and never
accepts inbound connections — it's purely an MQTT client that dials out
(TRD.md §1: all WAN reach is via MQTT). So "accessing the gateway
remotely" really means "making the broker reachable from wherever you
are" — a port-forward + TLS, a cloud-hosted broker, or a VPN back to your
LAN, entirely outside PulseLink's scope (CLAUDE.md rules out a
hosted/SaaS gateway by design).

What *is* in scope, once the broker stops being LAN-only: `kMqttUsername`/
`kMqttPassword` (empty = anonymous connect, fine for a trusted LAN broker)
and an opt-in `PULSELINK_MQTT_TLS` toggle (off by default — TRD.md §6
flags TLS as the dominant heap cost if enabled, ~40 KB+ during the
handshake, so it stays a deliberate choice rather than the default). See
D-017, DECISIONS.md, for the full reasoning.

## What's verified vs. what isn't

Flagship article artifact for Part 4 (PRD.md §4.1). **Not compiled in this
repo's CI, and not compiled by me at all** — there's no ESP32 toolchain in
this host-native test environment, only a plain Linux g++. What *is*
verified:

- Every piece of protocol/orchestration logic this file calls (registry,
  join handler, command table, dedupe, loss tracker, gateway HSM, topic
  builder/parser) is covered by 130 host-native tests in `test/` against
  `transport/fake/` — that's real coverage, not a claim.
- `gateway.ino` itself was syntax-checked with `g++ -std=c++11 -Wall
  -Wextra -fsyntax-only` against hand-written stub headers standing in for
  `WiFi.h`, `esp_now.h`, `esp_wifi.h`, `Preferences.h`, and
  `PubSubClient.h` — it compiles clean with zero warnings. That catches
  syntax errors and type mismatches against *our own* headers, but the
  stubs are approximations of the real Arduino/ESP-IDF APIs (best-effort,
  matching current arduino-esp32 3.x signatures), not the real thing —
  they can't catch a wrong real API signature or runtime/hardware
  behavior.
- The topic scheme and `MqttClient` interface itself were validated
  against a genuine Mosquitto broker in `examples/part4-gateway/`
  (independently cross-checked with `mosquitto_sub`) — but that used
  `libmosquitto` on host, not `PubSubClient` on an ESP32.

What's genuinely unverified: `EspNowTransport` against real ESP-NOW
hardware, `NvsRegistryStorage` against real flash, `PubSubClientMqttClient`
against `PubSubClient`'s actual API, and the whole file under a real
Arduino-ESP32 toolchain. One design decision worth knowing about before
you flash this: `EspNowTransport::send_unicast()` blocks for up to 50 ms
turning ESP-NOW's async send callback into the synchronous MAC-ack every
host-tested caller expects (D-014, DECISIONS.md) — reasonable on paper,
unverified on a real radio.

This is where the hardware rig, a Wokwi project, and real devkits take
over (PLAN.md Phase 4) — resources this environment doesn't have. One bug
already turned up this way before ever touching hardware: writing
`wokwi/single-board/combined.ino` alongside this file surfaced a missing
`set_field_name()` call that meant no DATA field would ever have actually
been published to MQTT — see `wokwi/single-board/README.md` for the story.
Fixed here too. Worth remembering: orchestration glue like this file isn't
unit-testable, so re-reading it end to end (or writing a sibling copy, as
happened here) is sometimes the only way a bug like that surfaces.

## Two more bugs, this time from an independent review (2026-07-14)

Both fixed here and in `wokwi/single-board/combined/combined.ino`:

- **`g_uplink_dedupe`/`g_loss_tracker` weren't reset on rejoin.** Both are
  indexed by `device_id`, which a rejoining node gets back unchanged (same
  MAC, same registry slot) — but the node's own `seq` counter is plain RAM
  with no NVS persistence (`node/node.ino`), so a node reboot restarts it
  at 0 while the gateway's tracker still remembers a `last_seq_` from
  before the reboot. That one-time "gap" got counted as real loss and,
  since `loss_rate_percent()` is a lifetime cumulative ratio with no decay,
  permanently inflated that device's published `loss_rate` for the rest of
  its uptime. Fixed in `handle_join_req()`: both trackers are reset to
  fresh instances on every successful join, first or repeat.
- **The command downlink trusted any sender.** See `node/README.md`'s
  security section — this file's half of that fix is D-015's JOIN_ACK
  token echo (`handle_join_req` fills it from the already-validated
  token) and D-016's random `cmd_id` seed (`esp_random()` at boot instead
  of a fixed `0`, so a post-reboot `cmd_id` can't collide with a value a
  node still has cached as its last-executed one and get silently
  skipped instead of run).

See `DECISIONS.md` D-015/D-016 for the full rationale on both.
