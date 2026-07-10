# CLAUDE.md — PulseLink

Context for Claude Code sessions. Read PRD.md (what/why), TRD.md (protocol + architecture), PLAN.md (current phase) before making changes. Settled decisions live in TRD; deviations go in DECISIONS.md — do not silently diverge.

## What this project is

PulseLink: ESP-NOW → MQTT gateway (protocol library + gateway/node firmware) built as the reference implementation for a five-part LogicFrenzy article series. **Content is the product** — code must be readable teaching material first, cleverness second. Bridges ESP-NOW node clusters into the PulseCore MQTT topic tree so PulseDash works unchanged.

## Topology

`PulseDash ↔ MQTT broker ↔ Gateway ESP32 ↔ ESP-NOW unicast ↔ ≤20 nodes`

ESP-NOW is link-layer only (no IP/routing); all WAN reach is via MQTT. Topics: `pulsecore/{tenant_id}/{device_id}/{field}`, commands on `.../cmd`, results on `.../cmd_status`.

## Non-negotiable design contracts

- **Zero heap** in `core/` and `transport/`: no `malloc`, no `new`, no Arduino `String`, no JSON on the wire. Static/stack only, `PULSELINK_MAX_*` macros bound everything.
- **C++11**, header-only where possible, no `<type_traits>` in shared cores.
- **Callback discipline:** ESP-NOW RX callback runs in WiFi task context — memcpy frame + MAC into ring buffer and return. Never parse, never publish, never dispatch from callback context. (Same rule as the PulseCore ISR rule: no `bus.emit()` from ISRs.)
- **No raw packed-struct casts across the wire.** Header serialized field-by-field (little-endian); body is Structa-packed.
- **Retry counters live in the calling/sending state**, never in a destination state's `entry()` (Pattern 8 from Taming the Loop).
- Macro naming: `PULSELINK_MAX_*`. Never `TINY_FSM_*`-style names anywhere.

## Protocol quick reference (full spec: TRD §3)

Frame header (7 B): `magic(1) | version(1) | msg_type(1) | seq(1) | cmd_id(2 LE) | payload_len(1)` then ≤243 B Structa body.
Msg types: JOIN_REQ (only legit broadcast), JOIN_ACK, DATA, CMD, CMD_ACK, PING, PONG, NACK.
CMD_ACK result codes: OK, UNKNOWN_CMD, BUSY, INVALID_PARAM, HW_FAULT.
DATA payload: self-describing `(field_id:u8, type, value)` tuples — gateway maps field_id→topic via registry; new node types must never require gateway reflash.

**Two ack loops, never conflated:** MAC-layer ack (send callback; drives fast retransmit ×2; **always reports success for broadcast — never trust it for JOIN_REQ**) vs app-level CMD_ACK (drives command table). At-least-once wire + dedupe (seq uplink / last cmd_id on node) = exactly-once execution.

## Architecture invariants

- Gateway: `esp_wifi_set_ps(WIFI_PS_NONE)` mandatory. Router dictates the channel for the whole cluster.
- Channel-change recovery = node-side only: N consecutive unicast failures → re-run broadcast discovery. No gateway mechanism.
- Uplink ring buffer: drop-oldest on overflow + overflow counter published as health metric.
- Gateway HSM (PulseHSM): `Connected{Bridging, Draining}` / `Degraded`. **ESP-NOW keeps working when MQTT is down** — spool uplink (bounded), refuse downlink with reason, flush on reconnect.
- Sleep profiles: ALWAYS_ON vs WAKE_AND_POLL (deep sleep → send DATA → ~100 ms listen window → sleep). Command table doubles as mailbox for sleepers.
- Gateway reboot drops in-flight commands **by design** (no NVS for command table).
- Registry (MAC↔device_id↔metadata) persists to NVS; storage backend is pluggable (RAM backend for host tests).

## Testing (three layers — most work happens in layer 1)

1. **Host-native** (`test/` via g++/CMake, runs in CI): all protocol logic through `transport/fake/` — an in-process fake transport with injectable drops/dupes/delays. Prefer adding tests here; new core logic without a host-native test is incomplete.
2. **Wokwi** (`wokwi/`): multi-board integration checks. Logic testbed only — its radio is idealized.
3. **Hardware** (3–5 devkits): real-RF only (channel change, power-save loss, burst timing, sleep windows). Don't design tests that need hardware if the fake transport can express them.

Run host tests before claiming anything works. CI greps `core/` and `transport/` for `new `/`malloc`/`String` as a heap tripwire — don't "fix" that grep.

## Repo layout

`core/` platform-agnostic headers · `transport/` interface + `espnow/` + `fake/` backends · `gateway/` + `node/` firmware · `test/` host-native · `examples/part2-node-to-node`, `examples/part3-pairing` (article artifacts — keep them minimal and readable, they appear in print) · `wokwi/` sim projects.

## Related PulseCore context

- **Structa** (v3.0): binary struct serialization — used for frame bodies. No DynamicJsonDocument, no String members.
- **PulseHSM**: gateway state machine. Config macros `PULSEHSM_MAX_STATES/EVENTS/DEPTH`.
- **PulseTrace**: optional debug tracing; HSM mode *or* signal-slot mode, never both.
- **AdvancedSignalSlot**: NOT used together with PulseHSM (mutually exclusive frameworks).
- **PulseDash**: consumes the MQTT tree unchanged; renders cmd delivery from `cmd_status` acks only, never assumes.

## Scope guards

Out of scope (do not build even if it seems helpful): broadcast-and-filter mode, LMK encryption, ESP-NOW v2 long frames, >20-node scaling, mesh/multi-hop, OTA-via-gateway, PulseDash provisioning UI, hosted/SaaS gateway. Broadcast pattern gets an article sidebar only.
