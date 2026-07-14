---
title: "Building the MQTT Gateway"
series: "Production-Grade ESP-NOW"
part: 4
status: draft
---

# Part 4 — Building the MQTT Gateway

Everything so far has stayed on one radio hop: two boards, then a node and
a gateway that can find each other and stay paired through a channel
change. None of it has left the room. This part is where that changes —
the flagship article of the series, and the piece that turns "an ESP-NOW
cluster" into "a fleet PulseDash can see."

The code is [`gateway/gateway.ino`](https://github.com/pulsecoreengineering/pulselink/tree/main/gateway),
~440 lines that wire together everything Parts 1-3 built plus four new
pieces: MQTT publish/subscribe, a topic-mapping registry, health metrics,
and a small state machine that decides what "gateway" even means when the
internet connection it depends on goes away.

## The one rule that makes this whole design possible

PulseDash already has an MQTT topic convention:
`pulsecore/{tenant_id}/{device_id}/{field}`, with commands on `.../cmd` and
results on `.../cmd_status`. The gateway's entire job is to make an
ESP-NOW cluster look like it's already speaking that convention — **zero
changes on the PulseDash side.** Every design decision in this file
traces back to that one constraint: the gateway is a translator, not a new
kind of thing PulseDash has to learn about.

`core/pl_topics.h` builds every topic string with zero heap — no
`std::string`, no `snprintf`-into-a-`String`, just fixed buffers and
explicit length tracking:

```cpp
inline uint8_t build_data_topic(const char* tenant_id, uint8_t device_id,
                                 const char* field, char* out,
                                 uint8_t out_cap) {
  uint8_t pos = 0;
  if (!detail::append(out, out_cap, &pos, "pulsecore/")) return 0;
  if (!detail::append(out, out_cap, &pos, tenant_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/")) return 0;
  if (!detail::append_device_id(out, out_cap, &pos, device_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/")) return 0;
  if (!detail::append(out, out_cap, &pos, field)) return 0;
  out[pos] = '\0';
  return pos;
}
```

Every `append` call fails closed — returns 0 on overflow rather than
silently truncating a topic string, which would be a far worse bug (a
truncated topic is still a valid-looking topic, just the wrong one). And
there's an inverse, `parse_cmd_topic_device_id()`, which the downlink path
needs: the gateway subscribes to a wildcard, `pulsecore/{tenant_id}/+/cmd`,
and has to recover which device_id a given message was actually meant for.
TRD.md didn't originally name this function — it fell out of actually
writing the subscribe side, which is a useful reminder that a protocol
doc's happy-path description ("gateway maps field_id to topic name") always
turns out to need an inverse somewhere once you implement both directions.

## Uplink: dedupe, map, publish — in that order

`handle_data()` is the uplink path's core, and the order of its checks
matters:

```cpp
void handle_data(const uint8_t src[6], const pulselink::FrameHeader& header,
                  const uint8_t* payload, uint8_t payload_len) {
  pulselink::RegistryEntry* node = g_registry.find_by_mac(src);
  if (!node) return;  // not joined — ignore

  if (g_uplink_dedupe[node->device_id].is_duplicate(header.seq)) return;
  g_uplink_dedupe[node->device_id].accept(header.seq);
  g_loss_tracker[node->device_id].record(header.seq);
  ...
```

Unregistered senders are dropped before anything else runs — a node that
hasn't joined has no device_id, and there's nothing to bridge it to.
Dedupe runs before loss-tracking, not after: `LastSeenSeq` decides whether
this frame is a retransmit of one already accepted (Part 1's at-least-once
wire + dedupe = effectively-exactly-once), and only a frame that clears
that check gets counted by `SeqLossTracker`, whose whole job is turning
gaps between *accepted* sequence numbers into a loss-rate metric. Feed it
duplicates and it'd measure something meaningless.

From there, the DATA payload's self-describing fields (Part 2) get mapped
to topic names via the registry and published:

```cpp
pulselink::FieldReader fields(payload, payload_len);
pulselink::FieldValue field;
while (fields.next(&field)) {
  const char* field_name =
      g_registry.find_field_name(node->device_id, field.field_id);
  if (!field_name) continue;  // unmapped: nothing to publish as
  ...
  g_hsm.publish_or_spool(&g_mqtt, topic, text, text_len);
}
```

That `find_field_name()` lookup is the entire payoff of Part 2's
self-describing wire format: the gateway never hardcodes "field 1 is
temperature" anywhere near the parsing logic. It asks the registry, and
the registry's mapping is populated once, at join time — TRD.md leaves
open whether that mapping is "delivered at join or provisioned"; this
tutorial's fleet is one node type sending one field, so it's provisioned:
`handle_join_req()` calls `g_registry.set_field_name(ack.device_id,
kFieldIdTemperatureC10, "temperature")` right after a successful join. A
fleet with multiple node types would provision per device type instead of
the same map for everyone, but the mechanism — ask the registry, don't
hardcode — is what lets a new node type ship without a gateway reflash.

That one `set_field_name()` call is also this file's cautionary tale: it
was missing entirely in an earlier version of this sketch, silently
meaning no DATA field would ever have been published — join worked, DATA
frames arrived, dedupe and loss-tracking ran correctly, and nothing ever
showed up on `pulsecore/.../temperature`. It surfaced while writing a
second, independent copy of this logic for the Wokwi simulator (more on
that below) and re-reading the two side by side. Every piece of *logic*
this file calls is covered by host-native tests, but this specific bug
lived in orchestration glue — the wiring between tested pieces — which
by definition those tests can't see, because they call the tested pieces
directly and never exercise the wiring itself. Worth remembering before
trusting "127 tests pass" as a substitute for reading the glue code.

## The state machine that decides what "gateway" means when MQTT is down

This is TRD §4.4's flagship demonstration, and it's the part of this file
that has nothing to do with ESP-NOW at all: what happens when the
*backhaul* — WiFi, or the broker, or both — goes away while nodes keep
transmitting.

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

The key invariant, stated directly in the TRD: **nodes must not care that
the backhaul died.** ESP-NOW is a completely separate radio concern from
WiFi's association state — there's no reason a lost MQTT session should
mean the gateway stops receiving DATA frames or drop dedupe/loss-tracking
state. `GatewayHsm` enforces this by construction: it only ever touches
the MQTT-facing half of the pipeline.

```cpp
void publish_or_spool(MqttClient* client, const char* topic,
                       const uint8_t* payload, uint16_t payload_len) {
  if (state_ == GatewayState::kConnectedBridging) {
    client->publish(topic, payload, payload_len);
    return;
  }
  spool_.push(topic, payload, payload_len);
}
```

While `Bridging`, publishes go straight through. Anywhere else —
`Degraded`, or `Draining` right after reconnect — they queue into a
bounded, drop-oldest spool instead of being lost outright or blocking the
loop waiting on a dead connection. `drain_step()` flushes that spool one
entry per main-loop tick after reconnect (not all at once — a slow broker
shouldn't stall everything else the loop needs to do) and transitions back
to `Bridging` once it's empty.

Downlink gets the harder rule. A command the gateway can't currently
service — and, just as important, can't reliably report the outcome of —
isn't attempted at all:

```cpp
bool accept_downlink(DownlinkRefuseReason* out_reason) {
  if (state_ == GatewayState::kConnectedBridging) {
    *out_reason = DownlinkRefuseReason::kNone;
    return true;
  }
  *out_reason = (state_ == GatewayState::kConnectedDraining)
                    ? DownlinkRefuseReason::kDraining
                    : DownlinkRefuseReason::kBackhaulDegraded;
  return false;
}
```

This connects directly to PRD FR-3's rule for PulseDash: it never assumes
delivery, it renders outcomes from actual `cmd_status` acks. A refused
downlink here produces no `cmd_status` at all — which is exactly the
signal PulseDash needs (silence, same as it already treats "never got an
ack" from Part 5's retry logic), rather than a fabricated
"queued-but-who-knows" status that would need its own special handling
downstream. Refusing loudly at the gateway and refusing by omission
downstream turn out to be the same design principle applied twice.

## MQTT as just another pluggable transport

The gateway never talks to `PubSubClient` directly outside of
`gateway/pl_pubsubclient_mqtt.h` — everything above goes through
`core/pl_mqtt.h`'s `MqttClient` interface, the same pattern Parts 1-3 used
for `Transport`. That's what makes two very different kinds of testing
possible for the same logic: `transport/fake/pl_fake_mqtt.h`'s
`FakeMqttClient` carries the host-native test suite (`test_gateway_bridge.cpp`,
`test_health_metrics.cpp`, and the HSM's own tests all run against it, no
network required), while `examples/part4-gateway/host_bridge_demo.cpp`
swaps in a real `libmosquitto`-backed client and validates the exact same
topic-building/publish logic against an actual Mosquitto broker,
independently cross-checked with a separate `mosquitto_sub` process
watching the wire.

That real-broker validation matters more than it might look: a fake
transport can prove your *logic* is correct, but it can't prove your
topic strings are what you think they are, or that a real broker accepts
them the way you expect. Both checks are real, and they're proving
different things — worth keeping straight rather than treating "tests
pass" as one undifferentiated fact.

## Health metrics: making congestion and liveness visible, not just logged

Two numbers this file publishes exist specifically because a fleet you
can't see the health of is a fleet you find out about too late. Ring
overflow — `core/pl_ring.h`'s drop-oldest counter from Part 2 — gets
published under a gateway-wide pseudo-topic (`pulsecore/{tenant}/gateway/{field}`,
since it has no single device_id to attach to):

```cpp
uint16_t gw_len = format_u32(g_espnow.rx_overflow_count(), gw_payload,
                              sizeof(gw_payload));
g_hsm.publish_or_spool(&g_mqtt, gw_topic, gw_payload, gw_len);
```

Per-node loss rate and liveness (`online`/`offline` events from
`Registry::check_offline_transition()`) publish as ordinary per-device
fields instead, since they do have a device_id and read naturally
alongside `temperature` in PulseDash. None of this is exotic — it's
`format_u32()` into a fixed buffer and a call to the same
`publish_or_spool()` every other topic in this file uses — but it's the
difference between "the gateway is silently dropping frames under load"
being a mystery you debug after the fact and a chart PulseDash can put in
front of you before it's a problem.

## What's proven, and what's still ahead

Every piece of protocol logic this file calls — registry, join handler,
dedupe, loss tracker, gateway HSM, topic builder/parser — is covered by
host-native tests against the fake transport and fake MQTT client. The
file itself syntax-checks clean against stub Arduino/ESP-NOW/PubSubClient
headers, and its logic (minus the real ESP-NOW radio) runs for real,
compiled by a real ESP32 toolchain, in the Wokwi web simulator: WiFi
connects, MQTT connects, a node joins, and `pulsecore/{tenant}/{device_id}/temperature`
updates live on the wire, independently confirmed via `mosquitto_sub` —
not just a Serial log claiming success, an actual subscriber watching the
broker.

What's still ahead, and it's the reason this article's demo section is a
placeholder rather than a screenshot: the real ESP-NOW radio, real NVS
flash, and `PubSubClient`'s actual API are all genuinely unverified
against real hardware as of this writing — Wokwi's fake-transport-in-place-of-ESP-NOW
setup proves the *gateway/MQTT* half for real and leaves the *radio* half
exactly where host-native tests already had it. Closing that gap needs
physical devkits, which is where this part's demo — three real nodes,
Mosquitto in Docker, a live PulseDash dashboard — actually gets recorded.

## What's next

Part 5 finishes the reliability story: the two-loop ack model this
article only touched in passing (MAC-layer ack vs. `CMD_ACK`), the command
table's retry-in-the-calling-state discipline, sleep profiles for battery
nodes, and the TRD's full list of failure modes — channel changes, broker
outages, crash-looping nodes, gateway reboots — each proven as an actual
test scenario, not just a design doc claim.
