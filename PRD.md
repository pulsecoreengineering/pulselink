# PRD — PulseLink: ESP-NOW → MQTT Gateway (LogicFrenzy Series + Reference Implementation)

**Status:** Approved for build
**Owner:** Alex Gabriel / PulseCore Engineering
**Last updated:** 2026-07-10

---

## 1. What this is

Two coupled deliverables:

1. **A five-part LogicFrenzy article series** teaching production-grade ESP-NOW: fundamentals, reliable messaging, channel/pairing management, an MQTT gateway, and reliability/sleep patterns.
2. **PulseLink** — the reference implementation behind the series: a protocol library + gateway firmware bridging ESP-NOW node clusters to MQTT (and therefore to PulseDash).

**Content is the product.** The code exists to make the articles credible and complete. PulseLink may be promoted to a supported PulseCore library later **only if reader/customer demand materializes** — that promotion is explicitly out of scope for this effort.

## 2. Why (and why now)

- SEO gap: existing ESP-NOW tutorials are shallow ("blink a payload across two boards"). Nobody covers channel lock-in, RX callback safety, app-level acks, or sleeping nodes. LogicFrenzy can own the "production-grade ESP-NOW" cluster.
- Funnel: the series naturally showcases PulseHSM, Structa, and PulseDash where they genuinely help — earned placement, not badging.
- Deferred-product validation: if readers ask "can I buy this gateway," that is the signal to productize. Building the tutorial version costs ~10% of productizing.

**Explicit non-goal:** PulseLink is *not* a PulseDash feature commitment. It must not displace the P1 widget backlog beyond the budgeted effort.

## 3. Audience

- Primary: ESP32 developers (hobbyist → semi-pro) searching for ESP-NOW guidance.
- Secondary: industrial IoT engineers evaluating low-power / no-WiFi-coverage sensor options.
- Tertiary: existing PulseCore library users.

## 4. Deliverables

### 4.1 Article series (LogicFrenzy, Astro blog)

| Part | Title (working) | Core content | Code artifact |
|---|---|---|---|
| 1 | ESP-NOW fundamentals & when to use it | MAC addressing, 250-byte frames, no routing, honest WiFi/BLE/LoRa comparison | none / trivial |
| 2 | Two boards talking, done right | RX-callback ring buffer discipline, Structa binary payloads, why raw struct casts are a trap | `examples/part2-node-to-node` |
| 3 | The channel problem & pairing | Router channel lock-in, broadcast discovery handshake, NVS persistence, re-scan fallback | `examples/part3-pairing` |
| 4 | Building the MQTT gateway | Full bridge: registry, uplink path, topic mapping, PulseDash live demo | `gateway/` (flagship) |
| 5 | Reliability & sleep | Two-loop ack model, command table, retry-in-calling-state (Pattern 8), sleep profiles, failure modes | `gateway/` + `node/` complete |

Each part ships a working, self-contained repo folder.

### 4.2 Reference implementation (public GitHub repo)

- `pulselink` repo: protocol library (header-only where possible), gateway firmware, node firmware, host-native test suite, Wokwi projects.
- Library placement: framework-neutral core with PulseHSM/Structa appearing where they genuinely help (per scoping decision: credibility over funnel).

## 5. Functional requirements (reference implementation)

- **FR-1** Nodes join a gateway via broadcast discovery with provisioning token; gateway assigns/confirms device_id; both persist pairing to NVS.
- **FR-2** Uplink: nodes send telemetry as self-describing field/value pairs; gateway publishes to `pulsecore/{tenant_id}/{device_id}/{field}`.
- **FR-3** Downlink: MQTT commands on `pulsecore/{tenant_id}/{device_id}/cmd` are delivered to nodes with app-level ack; gateway publishes `cmd_status` (delivered/failed + result code). PulseDash never assumes delivery.
- **FR-4** Sleeping nodes: gateway queues commands per node; delivers during the node's post-uplink listen window (~100 ms).
- **FR-5** Gateway survives backhaul loss: ESP-NOW side keeps operating while MQTT is down (Degraded superstate); bounded uplink spool; refuse downlink with reason; flush on reconnect.
- **FR-6** Channel-change recovery: nodes fall back to discovery re-scan after N consecutive unicast send failures.
- **FR-7** Health telemetry: gateway publishes ring-buffer overflow counter, per-node loss rate (from sequence gaps), and node liveness (last-seen → offline events).
- **FR-8** Duplicate safety: at-least-once delivery with dedupe (sequence numbers for uplink, command IDs for downlink) → effectively exactly-once execution.

## 6. Constraints & scope decisions (settled — do not relitigate)

- **Unicast-first.** Peer table, MAC-layer acks, ~20-node ceiling. Broadcast-and-filter pattern gets a sidebar in the articles only.
- **Self-describing DATA payloads.** Field IDs in the frame; gateway maps field ID → topic name via registry metadata. New node types must not require gateway reflashes.
- **ESP-NOW v1 framing.** Design for the 250-byte payload cap.
- **One ESP32 gateway** (WROOM-class). Memory budgeted; TLS optional and the dominant heap cost if enabled.
- **PulseCore design contracts apply:** zero heap allocation in library code, no malloc/new/Arduino String, C++11 on ESP32 (AVR-portability where cheap), header-only where possible, ISR/callback discipline (no work in callbacks — flag/ring-buffer and process in loop).
- Gateway reboot with in-flight commands: **accept the loss**, document it. No NVS persistence of the command table (flash wear not justified).

## 7. Success metrics

- Series: 5 parts published; ESP-NOW cluster ranking within 6 months; measurable referral traffic to PulseHSM/Structa repos and PulseDash.
- Code: full host-native test suite green in CI; end-to-end demo (3 boards → Mosquitto → PulseDash) recorded for Part 4.
- Validation signal (for future productization decision): inbound requests for a supported gateway.

## 8. Out of scope

- SaaS/hosted gateway, provisioning UI in PulseDash, OTA update of nodes via gateway, LMK encryption rollout (design leaves room; implementation deferred), ESP-NOW v2 long frames, >20 node scaling work, mesh/multi-hop.
