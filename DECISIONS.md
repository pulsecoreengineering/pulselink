# DECISIONS.md — PulseLink

Append-only log. New entries at the top: date, decision, rationale, alternatives rejected.

---

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
