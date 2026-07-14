# TRD — PulseLink Protocol & Gateway Architecture

**Status:** Approved. Decisions here are settled unless a blocking issue is found during implementation — record any deviation in `DECISIONS.md` with rationale.

---

## 1. System topology

```
PulseDash ↔ MQTT broker (Mosquitto) ↔ Gateway ESP32 ↔ ESP-NOW (unicast) ↔ Nodes (≤20/gateway)
```

- ESP-NOW is link-layer only (MAC-addressed, no IP/routing). All WAN/cross-network reach comes from the MQTT backhaul. One gateway per site; multiple sites converge on the same broker.
- PulseDash requires **zero changes** — the gateway speaks the existing topic convention.

## 2. Radio coexistence rules (gateway)

- Gateway runs WiFi-STA + ESP-NOW on one radio; **the router dictates the channel** for the entire ESP-NOW cluster.
- `esp_wifi_set_ps(WIFI_PS_NONE)` is **mandatory** on the gateway — default power-save duty-cycles the radio and drops ESP-NOW frames.
- Router channel change is a first-class event: nodes recover via re-discovery (see §5.1), not via any gateway-side mechanism.

## 3. Wire protocol

### 3.1 Frame layout (inside the 250-byte ESP-NOW v1 payload)

Header is serialized **field-by-field** (no raw packed-struct casts across the wire — padding/endianness). Little-endian for multi-byte fields.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | magic | Constant signature byte; discard foreign traffic on mismatch |
| 1 | 1 | version | High nibble = protocol version (start at 1); low nibble reserved |
| 2 | 1 | msg_type | See §3.2 |
| 3 | 1 | seq | Per-node, wrapping; uplink dedupe + loss-rate measurement |
| 4 | 2 | cmd_id | Meaningful only for CMD / CMD_ACK; 0 otherwise |
| 6 | 1 | payload_len | Explicit; truncated frames are detectable |
| 7 | ≤243 | payload | Structa-packed body |

Deliberately absent from the frame: sender MAC (provided out-of-band by the RX callback) and tenant/device identity (resolved gateway-side via registry). This keeps device identity out of attacker-controlled payload bytes, but it is *not* a spoofing defense on its own — the MAC itself is still just a radio-reported address, forgeable by any ESP32 that sets its own MAC before `esp_now_init()`. The real trust boundary is the shared provisioning token (§5.1, D-015): a MAC-spoofing attacker who also has the token can still impersonate a node's uplink. LMK-level protection against that is explicitly out of scope (CLAUDE.md).

### 3.2 Message types

| Type | Direction | Purpose |
|---|---|---|
| `JOIN_REQ` | node → broadcast | Discovery + pairing; carries provisioning token. The only legitimate broadcast frame. |
| `JOIN_ACK` | gateway → node | Assigned/confirmed device_id + current channel + the provisioning token echoed back (authenticity check, D-015 — a node refuses to trust a JOIN_ACK, and refuses to re-pair at all once already paired, unless the token matches) |
| `DATA` | node → gateway | Telemetry (self-describing fields, §3.3) |
| `CMD` | gateway → node | Downlink command; carries cmd_id |
| `CMD_ACK` | node → gateway | App-level ack; cmd_id + result code (§3.4) |
| `PING` / `PONG` | both | Liveness |
| `NACK` | either | "Received but cannot process" (bad version, unknown command). Distinct from silence, which means retry. |

### 3.3 DATA payload: self-describing fields

- Body = sequence of (field_id: u8, type tag, value) tuples, Structa-packed.
- Gateway maps field_id → MQTT field name via per-node registry metadata (delivered at join or provisioned).
- Rationale: new node types must never require a gateway reflash.

### 3.4 CMD_ACK result codes

`OK`, `UNKNOWN_CMD`, `BUSY`, `INVALID_PARAM`, `HW_FAULT`. Enum fixed from day one; nodes must say *why* they refused.

### 3.5 Reliability model — two independent ack loops

1. **MAC-layer ack** (ESP-NOW send callback): "a radio received the frame." Drives fast retransmit (×2, short backoff). **Lies for broadcast** (always success) — never trust it for JOIN_REQ.
2. **App-level ack** (`CMD_ACK`): "the application executed the command." Drives command-table state. These two must never be conflated.

Delivery semantics: at-least-once on the wire; dedupe (seq for uplink, last-executed cmd_id on nodes for downlink) yields effectively exactly-once execution.

## 4. Gateway architecture

### 4.1 Uplink path (high-volume, loss-tolerable)

```
RX callback (WiFi task ctx): memcpy frame + sender MAC → ring buffer, return immediately
Main loop: drain ring → seq dedupe → MAC→device_id lookup → Structa unpack → MQTT publish
```

- Ring buffer sized for synchronized wake bursts: depth ≈ node_count × frames_per_wake × safety factor (initial: 32 × 260 B ≈ 8 KB).
- Overflow policy: **drop-oldest + increment overflow counter**; counter published as gateway health metric (congestion visible in PulseDash).
- Same discipline as the PulseCore ISR rule: no work, no allocation, no `bus.emit()`-equivalents in the callback.

### 4.2 Downlink path (low-volume, loss-intolerable)

**Command table** — per-node slots:

```
{cmd_id, state: PENDING|SENT|ACKED|FAILED, retry_count, deadline, payload}
```

- MQTT `.../cmd` arrives → stamp cmd_id → slot.
- Always-on node: transmit immediately. Wake-and-poll node: hold as mailbox; transmit within the node's ~100 ms post-uplink listen window.
- Retry counter lives **in the gateway's sending logic** (calling state — Pattern 8), never in a destination state's entry.
- On `CMD_ACK`: clear slot, publish `cmd_status` upstream. On retries exhausted / deadline: mark FAILED, publish failure. PulseDash renders delivery from actual acks only.
- Gateway reboot loses in-flight commands by design (no NVS persistence of the table; flash wear not justified). Documented behavior.

### 4.3 Registry

`MAC ↔ device_id ↔ {sleep_profile: ALWAYS_ON|WAKE_AND_POLL, field_map, last_seen, pending_cmds}`

- Persisted to NVS (reboot must not orphan the fleet).
- Populated by join flow. `last_seen` drives liveness: miss N expected check-ins → publish offline event.

### 4.4 Gateway state machine (PulseHSM)

```
Root
├── Connected            (WiFi up + MQTT session live)
│   ├── Bridging         (normal operation)
│   └── Draining         (flushing spool after reconnect)
└── Degraded             (WiFi or broker down)
    ├── ESP-NOW side KEEPS OPERATING
    ├── uplink → bounded spool (drop-oldest when full)
    └── downlink refused with reason
```

Key invariant: **nodes must not care that the backhaul died.** The Connected/Degraded superstate split is the series' flagship HSM demonstration.

## 5. Node behavior

### 5.1 Join / discovery / channel recovery

1. First boot: broadcast `JOIN_REQ` scanning channels 1–13 with provisioning token.
2. `JOIN_ACK` → persist gateway MAC + channel to NVS → unicast thereafter.
3. Subsequent boots: skip scan, go straight to unicast.
4. After **N consecutive unicast send failures** → wipe channel assumption, re-run discovery. This *is* the channel-change recovery; no separate mechanism.

### 5.2 Sleep profiles

- `ALWAYS_ON`: receives CMD any time.
- `WAKE_AND_POLL`: deep sleep → wake → send `DATA` → hold ~100 ms RX listen window → receive queued CMD if any → `CMD_ACK` → sleep.
- Profile is per-device registry metadata, declared at join.

### 5.3 Command dedupe

Node stores last-executed cmd_id; a retried CMD with the same id is re-acked but not re-executed. Before any of that: a node only accepts a CMD frame from its paired gateway's MAC (D-015) — ESP-NOW has no other sender authentication, so skipping this check would let any radio on the channel command the node directly.

## 6. Memory budget (WROOM-class gateway, rough)

| Component | Estimate |
|---|---|
| Uplink ring buffer (32 × 260 B) | ~8 KB |
| Command table (20 × ~300 B) | ~6 KB |
| Registry | ~2 KB |
| MQTT client | modest |
| TLS (if enabled) | ~40 KB+ heap during handshake — the elephant; optional |
| WiFi stack | platform overhead |

Fits WROOM **only because of** the zero-heap / no-String / no-JSON-on-wire contracts. Any JSON or Arduino String in the hot path blows the budget.

## 7. Failure modes (each = a test scenario + Part 5 section)

1. Router channel change mid-operation → nodes recover via §5.1 fallback.
2. Broker down while nodes keep sending → Degraded spool, flush on reconnect.
3. Node crash-looping, spamming JOIN_REQ → gateway rate-limits joins per MAC.
4. Duplicate CMD delivery → node dedupe by cmd_id.
5. Gateway reboot with in-flight commands → documented loss.
6. Ring overflow under burst → drop-oldest + visible counter.
7. Foreign ESP-NOW traffic on channel → magic-byte discard.
8. Truncated/corrupt frame → payload_len mismatch discard.

## 8. Design contracts (inherited from PulseCore, non-negotiable)

- Zero heap allocation in library code; no malloc/new/Arduino String.
- C++11; header-only where possible; AVR-friendly patterns where cheap.
- No `<type_traits>` dependency in shared cores (PulseVars precedent).
- Callback/ISR discipline: copy + flag/ring, process in loop. Never emit/dispatch from callback context.
- No raw packed-struct casts on any wire — everything serialized explicitly (Structa or field-by-field).
- Macro namespace: `PULSELINK_MAX_*` compile-time config (mirroring `PULSEHSM_MAX_*` convention). Never use deprecated `TINY_FSM_*`-style names.

## 9. Test strategy (three layers)

1. **Host-native (g++, CI, no hardware):** frame codec, dedupe, command table FSM, ring buffer + overflow, registry — driven through a **fake transport** (in-process simulated gateway + N nodes) enabling deterministic fault injection (frame drops, duplicates, broker kill).
2. **Wokwi web simulator:** multi-ESP32 project (gateway + 2 nodes), simulated ESP-NOW + WiFi→public/local MQTT. Validates integration logic (join, uplink→publish, downlink→ack). Radio is idealized — not an RF testbed. (Web simulator only; the Wokwi API was previously evaluated and rejected for Pulse Compiler.)
3. **Hardware:** minimum 3 × ESP32 devkits (gateway + 2 nodes), ideal 4–5 (one plays sleeping battery node). Irreplaceable for: channel-change under real RF, `WIFI_PS_NONE` frame-loss demonstration, send-callback timing under load, deep-sleep wake timing / listen-window sizing, range/attenuation. Backhaul: Mosquitto in Docker + local PulseDash (existing LAN deployment stack).

Hardware is required only from Phase 4 onward (see PLAN.md).
