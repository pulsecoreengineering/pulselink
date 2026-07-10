# PLAN — PulseLink Implementation Phases

Phases map to series parts. Each phase ends with: tests green, the matching article draftable, and a demo artifact. Hardware is a blocker only from Phase 4.

---

## Phase 0 — Repo skeleton & tooling (no hardware)

- [ ] Create `pulselink` repo with structure below
- [ ] Host-native build: plain `g++`/CMake target for `core/` + `test/`
- [ ] CI: build + run host-native tests on push
- [ ] `CLAUDE.md`, `PRD.md`, `TRD.md`, `DECISIONS.md` committed at root

```
pulselink/
├── CLAUDE.md              # Claude Code context (conventions, contracts, decisions)
├── PRD.md  TRD.md  PLAN.md  DECISIONS.md
├── core/                  # Platform-agnostic, header-only, zero-heap
│   ├── pl_frame.h         # Frame codec (header + Structa body)
│   ├── pl_ring.h          # Ring buffer + overflow policy
│   ├── pl_cmdtable.h      # Command table state machine
│   ├── pl_registry.h      # MAC↔device_id registry (storage-backend agnostic)
│   └── pl_config.h        # PULSELINK_MAX_* macros
├── transport/
│   ├── pl_transport.h     # Transport interface (send/recv/peer mgmt)
│   ├── espnow/            # Real ESP-NOW backend (ESP32 only)
│   └── fake/              # In-process fake transport w/ fault injection
├── gateway/               # Gateway firmware (ESP32) — PulseHSM state machine
├── node/                  # Node firmware (ESP32) — always-on & wake-and-poll
├── test/                  # Host-native tests against fake transport
├── examples/
│   ├── part2-node-to-node/
│   └── part3-pairing/
└── wokwi/                 # Wokwi multi-board projects
```

**Exit:** empty-but-compiling skeleton, CI green.

## Phase 1 — Frame codec + fake transport (→ enables Part 2 article)

- [x] `pl_frame.h`: serialize/parse header field-by-field; magic/version rejection; payload_len truncation detection
- [x] Self-describing DATA field tuples via Structa (`pl_fields.h` stands in for Structa itself — not vendored into this repo; see DECISIONS.md D-011)
- [x] `fake/` transport: in-process delivery between simulated gateway + N nodes; injectable frame drop rate, duplication, delay
- [x] Tests: round-trip all msg types; corrupt/truncated/foreign-frame rejection; seq dedupe
- [x] `examples/part2-node-to-node`: minimal two-board sketch using codec + ring discipline (logic verified via fake transport; real ESP32 compilation deferred to Phase 4 hardware bring-up)

**Exit:** Part 2 code artifact done. Part 1 (no code) and Part 2 articles can be written.

## Phase 2 — Join/pairing + registry (→ Part 3)

- [x] JOIN_REQ/JOIN_ACK flow over fake transport; provisioning token check
- [x] Registry with pluggable storage (RAM backend for host tests; NVS backend is pseudocode in the Part 3 `.ino` files for now — a real ESP32 implementation lands in Phase 4 hardware bring-up)
- [x] Node-side: persist gateway MAC + channel; N-failure → re-discovery fallback
- [x] Gateway-side: join rate-limit per MAC
- [x] Tests: fresh join, rejoin from persistence, channel-change recovery (fake transport simulates channel mismatch), join-spam rate limiting
- [x] `examples/part3-pairing`

**Exit:** Part 3 draftable. Order/locate 3–5 ESP32 devkits now if not on hand.

## Phase 3 — Command table + reliability core (→ Part 5 logic, tested early)

- [ ] `pl_cmdtable.h`: PENDING→SENT→ACKED/FAILED, retries in calling state (Pattern 8), deadlines
- [ ] Two-loop ack model: fake MAC-ack vs app-ack, including "MAC-ack lies for broadcast" case
- [ ] Node cmd_id dedupe (re-ack without re-execute)
- [ ] Sleep-profile mailbox: hold CMD for WAKE_AND_POLL node, deliver in listen window (simulated timing)
- [ ] CMD_ACK result-code enum end to end; NACK path
- [ ] Tests: every failure mode in TRD §7 that the fake transport can express (1–8 except real-RF aspects)

**Exit:** all protocol logic proven off-target. This is the series' "we test embedded protocol logic on the desktop" showpiece.

## Phase 4 — Gateway firmware + Wokwi + hardware bring-up (→ Part 4) **[hardware starts]**

- [ ] `espnow/` transport backend; `WIFI_PS_NONE`; unicast peer management
- [ ] Gateway PulseHSM machine: Connected{Bridging, Draining} / Degraded superstates; bounded spool; refuse-with-reason
- [ ] MQTT integration: publish `pulsecore/{tenant}/{device_id}/{field}`, subscribe `.../cmd`, publish `cmd_status` + health metrics (overflow counter, per-node loss rate, offline events)
- [ ] NVS registry backend
- [ ] Wokwi project: gateway + 2 nodes end-to-end against local Mosquitto
- [ ] Hardware rig: 3 devkits + Mosquitto (Docker) + PulseDash pointed at it
- [ ] Record Part 4 demo: live nodes on a PulseDash dashboard

**Exit:** end-to-end demo on real boards. Part 4 draftable.

## Phase 5 — Real-RF validation + sleep (→ Part 5 complete)

- [ ] Node firmware: deep sleep, wake→DATA→100 ms listen window→sleep; tune window empirically
- [ ] Real-RF tests: router channel change mid-run; power-save loss demonstration (PS on vs `WIFI_PS_NONE`); burst load (2 physical nodes generating 20-node synthetic traffic); range/attenuation notes
- [ ] Degraded-mode soak: kill broker under live node traffic, verify spool + flush
- [ ] Finalize Part 5; publish series per LogicFrenzy content calendar

**Exit:** series complete, repo public, demo video recorded.

---

## Standing rules for every phase

- Any deviation from TRD → entry in `DECISIONS.md` (date, what, why).
- No heap in `core/` or `transport/` — CI can grep for `new `/`malloc`/`String` as a tripwire.
- Article draft for part N starts as soon as phase N exits — don't batch all writing to the end.
- Timebox: this project must not cannibalize the PulseDash P1 widget backlog; if a phase balloons, cut scope (broadcast sidebar, LMK encryption, etc. are already out).
