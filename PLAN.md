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

- [x] `pl_cmdtable.h`: PENDING→SENT→ACKED/FAILED, retries in calling state (Pattern 8), deadlines
- [x] Two-loop ack model: fake MAC-ack vs app-ack, including "MAC-ack lies for broadcast" case (broadcast case covered in Phase 1's `test_fake_transport.cpp`; Phase 3 adds the CMD/CMD_ACK-specific MAC-ack-vs-app-ack test)
- [x] Node cmd_id dedupe (re-ack without re-execute)
- [x] Sleep-profile mailbox: hold CMD for WAKE_AND_POLL node, deliver in listen window (simulated timing)
- [x] CMD_ACK result-code enum end to end; NACK path
- [x] Tests: TRD §7 failure modes 1, 3, 4, 5, 6, 7, 8 (modes covered across Phases 1-3). Mode 2 (broker down → Degraded spool) needs the MQTT/HSM gateway machinery and is deferred to Phase 4; it isn't a real-RF gap, just a not-yet-built-one.

**Exit:** all protocol logic proven off-target. This is the series' "we test embedded protocol logic on the desktop" showpiece.

## Phase 4 — Gateway firmware + Wokwi + hardware bring-up (→ Part 4) **[hardware starts]**

- [x] `espnow/` transport backend; `WIFI_PS_NONE`; unicast peer management (`transport/espnow/pl_espnow_transport.h` — written and syntax-checked against stub headers, D-014's bounded-wait MAC-ack design; genuinely unverified against real hardware, no ESP32 toolchain in this environment)
- [x] Gateway HSM: Connected{Bridging, Draining} / Degraded superstates; bounded spool; refuse-with-reason (`core/pl_gateway_hsm.h` — hand-rolled stand-in for PulseHSM, D-013)
- [x] MQTT integration, publish side: `pulsecore/{tenant_id}/{device_id}/{field}` topic mapping (`core/pl_topics.h`), pluggable `MqttClient` (`core/pl_mqtt.h`) with a fake backend for host tests (`transport/fake/pl_fake_mqtt.h`) and validated against a real Mosquitto broker (`examples/part4-gateway/host_bridge_demo.cpp`)
- [x] MQTT integration, subscribe side (basic): `.../cmd` subscribe + receive proven against a real broker in the same demo
- [x] `cmd_status` publish (`core/pl_cmd_status.h`: `format_cmd_status()` — delivered result code or "failed", the same `.../cmd_status` topic PulseDash already reads from)
- [x] Health metrics: per-node loss rate from seq gaps (`core/pl_loss_tracker.h`'s `SeqLossTracker`, published as a `loss_rate` field), node liveness / offline+online events (`pl_registry.h`'s `check_offline_transition()`/`touch()`, published as an `online` field), gateway-wide ring overflow counter (`build_gateway_topic()` — TRD.md doesn't name a topic for gateway-wide metrics, since they have no device_id; this fills that gap with a `pulsecore/{tenant}/gateway/{field}` convention)
- [x] NVS registry backend (`gateway/pl_nvs_registry_storage.h` via Arduino `Preferences`; RAM backend is still what host tests exercise directly). Writing this surfaced a real bug worth flagging: `Registry`'s constructor eagerly loads from storage, but as a global object it would construct *before* `setup()` runs — i.e. before `Preferences::begin()`. Fixed with an explicit `Registry::reload()`, called from `gateway.ino`'s `setup()` after `g_prefs.begin()`; covered by a new host test.
- [x] `gateway/gateway.ino`: the full orchestration sketch — join handling, uplink dedupe/loss-tracking/field-bridging, downlink routing (new `parse_cmd_topic_device_id()` recovers device_id from a subscribed `.../cmd` topic), ALWAYS_ON retry polling vs. WAKE_AND_POLL listen-window delivery, health metrics, HSM-gated publish/spool throughout. `gateway/pl_pubsubclient_mqtt.h` is the real `MqttClient` backend (`PubSubClient`). All four `.ino` files in the repo (this one plus Parts 2-3's) syntax-check clean (`g++ -fsyntax-only`, zero warnings) against hand-written Arduino/ESP-NOW stub headers — real, but bounded: catches syntax/type errors against our own headers, not wrong real API signatures or hardware behavior. See `gateway/README.md` for exactly what is and isn't verified.
- [x] `node/node.ino`: the ALWAYS_ON node counterpart to `gateway.ino` — join, periodic DATA send, CMD receive/execute/ack with dedupe, real NVS pairing persistence. Uses `EspNowTransport` for a correct MAC-ack, fixing `examples/part3-pairing/node_join.ino`'s earlier flagged simplification now that D-014's real fix exists.
- [x] Wokwi project — `wokwi/single-board/`: confirmed (by the user, running Wokwi directly) that Wokwi does not support different firmware on multiple boards in one project, so the original two-board `gateway-node/` attempt was removed. Rebuilt as gateway + node combined on **one** simulated ESP32, connected by `transport/fake/pl_fake_transport.h`'s `FakeMedium`/`FakeTransport` — the same in-process fake medium 127 host-native tests already use — in place of real ESP-NOW, since `Transport` is an interface and swapping backends is a type change, not a logic change. WiFi and MQTT stay real (`Wokwi-GUEST` + `test.mosquitto.org`), which is the part this setup can actually add beyond what host-native tests already prove. Writing it surfaced a real bug in the already-pushed `gateway.ino`: nothing ever called `Registry::set_field_name()`, so DATA fields would never actually have been published — 127 tests missed it because they provision the field map directly in test setup, bypassing the join path. Fixed in both `gateway.ino` and the combined sketch; see `wokwi/single-board/README.md`. **Run by the user on real Wokwi** (2026-07-10): compiled clean and produced exactly the expected boot sequence — WiFi connect, MQTT connect, JOIN_REQ → joined → paired, repeated `publishing pulsecore/acme/0/temperature`. First real compilation and execution this code has ever seen, and it worked first try. **Independently confirmed via `mosquitto_sub` from the user's own machine** (not just the Serial log, which only proves the code ran, not that the broker received anything): `pulsecore/acme/0/temperature` updating every ~5s with real incrementing values, plus `pulsecore/acme/0/loss_rate` and `pulsecore/acme/gateway/ring_overflow` both landing on the ~30s health-metrics cycle exactly as designed. **Downlink confirmed too**: `mosquitto_pub` to `pulsecore/acme/0/cmd` produced `node: executing cmd_id=0` in Wokwi's Serial log and `pulsecore/acme/0/cmd_status ok` on the subscriber — the full MQTT → gateway → (fake ESP-NOW) → node → CMD_ACK → gateway → `cmd_status` round trip, for real. All three legs (uplink, health metrics, downlink) are now independently verified, not just predicted.
- [ ] Hardware rig: 3 devkits + Mosquitto (Docker) + PulseDash pointed at it (needs physical boards + your local network)
- [ ] Record Part 4 demo: live nodes on a PulseDash dashboard (depends on the hardware rig above)

**Exit:** end-to-end demo on real boards. Part 4 draftable. The Wokwi simulation is now confirmed working end-to-end; what remains is exclusively the physical hardware rig and PulseDash, which need devkits and your local network — resources this environment doesn't have.

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
