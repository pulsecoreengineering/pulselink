# DECISIONS.md — PulseLink

Append-only log. New entries at the top: date, decision, rationale, alternatives rejected.

---

## 2026-07-14 — Post-Phase-4 hardening: JOIN_ACK authentication + CMD sender verification

**D-015: `JoinAckPayload` (`core/pl_join.h`) now echoes the provisioning
token back to the node, and `NodePairingState::on_join_ack()` refuses to
re-pair a node that's already paired; nodes additionally verify the sender
of every `CMD` frame is their paired gateway's MAC before executing it.**
Found by an independent security review: as originally specified,
JOIN_ACK carried only `device_id` + `channel` with no authentication at
all — since ESP-NOW gives a receiver no way to verify a frame actually
came from the radio it thinks it did, *any* frame shaped like a JOIN_ACK
(broadcast or unicast) could re-point a node's gateway MAC to an attacker,
and `handle_cmd` never checked that an inbound `CMD` came from the paired
gateway at all, so a forged `CMD` from any radio on the channel would be
executed and acked. The fix has two independent layers, deliberately not
just one: (1) JOIN_ACK now must echo the same secret JOIN_REQ already
required (`join_ack_is_authentic()`) — closes the "no token at all" gap
for a mid-join attacker; (2) `on_join_ack()` itself refuses to mutate an
already-paired node's state regardless of the token check's outcome — a
legitimate node only ever expects a JOIN_ACK while unpaired, so this alone
closes the more severe "hijack an already-working node" case even if a
token ever leaked. `handle_cmd`'s MAC check (`node/node.ino`,
`wokwi/single-board/combined/combined.ino`,
`examples/part3-pairing/node_join`'s equivalent doesn't execute commands,
only pairing) is the same principle applied to the command path.
*Rejected:* leaving this for a later "real crypto" pass (LMK encryption is
explicitly out of scope, CLAUDE.md, but *authentication* of which MAC a
node trusts as its gateway doesn't require encryption and was simply
missing — no reason to ship known-hijackable reference firmware in the
meantime). *Also rejected:* per-message nonces/replay windows beyond
this — diminishing returns for a tutorial-scale fleet already bounded by a
shared provisioning token (see the token-size caveat in `pl_config.h`).

**D-016: Gateway's `g_next_cmd_id` counter is seeded from `esp_random()` at
boot, not a fixed `0`.** Found by the same review: the command table is
intentionally RAM-only (D-006 — gateway reboot drops in-flight commands),
but a node's `NodeCmdDedupe` (`core/pl_cmdtable.h`) only remembers a
single last-executed `cmd_id` and *does* survive a gateway reboot (it's
node-local, node reboot only). Restarting the counter at `0` every gateway
boot meant a post-reboot `cmd_id` could collide with a value a node still
had cached from before the reboot, making a genuinely new command look
like a duplicate and get silently re-acked instead of executed — a real
violation of PRD FR-8's "effectively exactly-once execution" with no
error signal anywhere. *Rejected:* persisting the counter to NVS —
that's the exact flash-wear-for-marginal-benefit trade D-006 already
declined for the whole command table; a random seed closes the practical
collision risk (1-in-65536 per reboot, and only against a value the node
happens to still have cached) without reopening that decision.

## 2026-07-10 — Phase 4: EspNowTransport synchronizes ESP-NOW's async MAC-ack

**D-014: `transport/espnow/pl_espnow_transport.h`'s `send_unicast()` blocks
for up to `kSendAckTimeoutMs` (50 ms) waiting on ESP-NOW's send callback
before returning.** Every host-tested caller of `Transport` (join,
channel-change recovery, the command table's retry logic) was written
against the fake transport's synchronous "true MAC-ack" return value —
real ESP-NOW's ack is asynchronous, arriving via `esp_now_register_send_cb`
sometime after `esp_now_send()` returns. Turning it back into a
synchronous answer at the `Transport` boundary means all of that
host-tested logic runs unchanged on hardware. *Rejected:* changing
`send_unicast()`'s return value to mean "queued" instead of "acked" on
real hardware — that would silently break the MAC-ack semantics every
Phase 1-3 test (and the logic it validates) assumes, for the sake of
avoiding a bounded ~50 ms wait in `loop()`-context code, which isn't
callback context and isn't subject to CLAUDE.md's callback-discipline
rule.

## 2026-07-10 — Phase 4: PulseHSM stand-in for the gateway state machine

**D-013: `core/pl_gateway_hsm.h`'s `GatewayHsm` is a hand-rolled state
machine, not the real PulseHSM library.** Same situation as D-011/Structa:
PulseHSM lives in PulseCore and isn't vendored into this repo. `GatewayHsm`
implements exactly the state/event contract TRD.md §4.4 specifies —
`Connected{Bridging, Draining}` / `Degraded`, bounded drop-oldest spool,
refuse-downlink-with-reason — as a plain class rather than
`PULSEHSM_MAX_STATES`-style config macros or a generic reusable framework.
*Rejected:* building a full generic HSM framework clone to host one gateway
state machine (disproportionate to what Phase 4 actually needs; PulseHSM
integration is a later swap behind the same state contract, not a
prerequisite for proving the Degraded/spool/drain logic works).

## 2026-07-10 — Phase 1: Structa stand-in for DATA field tuples

**D-011: `core/pl_fields.h` is a minimal zero-heap (field_id:u8, type, value)
tuple codec, not the real Structa library.** Structa isn't vendored into this
repo (it lives in PulseCore, out of scope to pull in here). The codec matches
the wire format TRD.md §3.3 specifies — a DATA payload built with
`pl_fields.h` today is byte-for-byte what a future Structa-backed encoder
would produce, so swapping the implementation later doesn't change anything
already on the wire or in `test/`. *Rejected:* blocking Phase 1 (and every
phase after it) on vendoring Structa first — the dependency is an
implementation detail behind a fixed wire contract, not a prerequisite for
proving the protocol logic.

## 2026-07-10 — Initial settled decisions (planning phase)

**D-001: Content-first framing.** PulseLink is the reference implementation for a LogicFrenzy series, not a PulseDash product feature. Productization only if reader/customer demand appears. *Rejected:* building it as ecosystem infrastructure now (no customer pull; competes with P1 widget backlog).

**D-002: Framework-neutral code with earned library placement.** Series code built on ESP-IDF/Arduino APIs; PulseHSM/Structa appear where they genuinely help (Parts 2, 4, 5), not badged throughout. *Rejected:* full PulseCore-showcase framing (narrower appeal, less credible).

**D-003: Unicast-first.** Peer table + MAC-layer acks + optional LMK path later; ~20-node ceiling accepted. Broadcast-and-filter documented as an article sidebar only. *Rejected:* dual-mode support from day one (frame format and reliability logic complexity not justified for tutorial audience).

**D-004: Self-describing DATA payloads.** `(field_id, type, value)` tuples; gateway maps via registry metadata. *Rejected:* device-type-implied schemas (couples gateway firmware to every node type; reflash required for new nodes).

**D-005: Header serialized field-by-field; no packed-struct casts on the wire.** Consistency with the series' own teaching (padding/endianness anti-pattern) and PulseCore contracts.

**D-006: Gateway reboot drops in-flight commands.** No NVS persistence of the command table — flash wear for marginal benefit. Documented behavior; PulseDash learns outcome via `cmd_status` timeout.

**D-007: ESP-NOW v1 250-byte framing.** Portability over v2's 1470-byte frames.

**D-008: Channel-change recovery is node-side re-discovery** after N consecutive unicast send failures. No dedicated gateway mechanism; recovery emerges from the join design.

**D-009: Three-layer test strategy.** Host-native fake transport carries most coverage; Wokwi web simulator for integration logic (its radio is idealized; note Wokwi's *API* was separately evaluated and rejected for Pulse Compiler — unrelated to using the web simulator here); hardware (3–5 devkits) reserved for real-RF phenomena from Phase 4.

**D-010: CMD_ACK result-code enum fixed from day one:** OK, UNKNOWN_CMD, BUSY, INVALID_PARAM, HW_FAULT.
