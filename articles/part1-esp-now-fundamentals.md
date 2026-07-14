---
title: "ESP-NOW Fundamentals: When to Reach for It (and When Not To)"
series: "Production-Grade ESP-NOW"
part: 1
status: draft
---

# Part 1 — ESP-NOW Fundamentals & When to Use It

Every ESP-NOW tutorial on the internet teaches you the same trick: two boards,
one `esp_now_send()`, a payload that shows up on the other side. It works,
it's satisfying, and it tells you almost nothing about what happens when you
try to run twenty of these nodes for six months in a building with a router
that changes channels on you, a gateway that reboots mid-command, and a radio
that lies to you about what it actually delivered.

This series builds that version instead — a real ESP-NOW-to-MQTT gateway,
[PulseLink](https://github.com/pulsecoreengineering/pulselink), with the
kind of failure-mode handling production firmware needs and none of the
"it worked once on my desk" tutorials bother with. Part 1 is scope-setting:
what ESP-NOW actually is at the protocol level, and — more usefully — when
it's the wrong tool.

## What ESP-NOW actually is

ESP-NOW is a connectionless, link-layer protocol Espressif built on top of
the 802.11 physical layer. That one sentence carries three consequences
worth unpacking, because each one shapes every design decision in this
series.

**Connectionless** means there's no association, no handshake with an
access point, no DHCP lease, no IP stack involved at all. A frame goes out
addressed to a MAC address and either a radio hears it or it doesn't. This
is why ESP-NOW boots and sends its first frame in milliseconds where WiFi
association can take seconds — there's nothing to negotiate.

**Link-layer** means ESP-NOW frames don't route. There's no concept of a
gateway, a subnet, or a next hop baked into the protocol. Every "node" in an
ESP-NOW cluster is one radio hop from every other node it talks to, full
stop. If you need a frame to leave the local radio neighborhood — reach the
internet, cross a building, hit a database — something else has to carry it
the rest of the way. That's the entire reason this series' reference
architecture exists: ESP-NOW gets telemetry from a sensor to a gateway
that's in radio range; MQTT gets it everywhere after that. Conflating the
two — expecting ESP-NOW itself to somehow "get to the cloud" — is the most
common source of confused first attempts.

**Built on 802.11** means ESP-NOW frames are unicast or broadcast 802.11
action frames, capped by the same payload ceiling: **250 bytes** per frame
in the v1 API (ESP-NOW v2 raises this to 1470 bytes on newer chips, but v1
is the portable baseline this series targets — see the frame-size trade-off
below). It also means ESP-NOW shares the radio with WiFi. On a single-radio
chip like the ESP32, ESP-NOW and a WiFi station connection are the same
antenna, the same channel, and — critically — the same power-management
state. Get that last part wrong and frames vanish silently; Part 4 covers
exactly how and why when we build the gateway's WiFi+ESP-NOW coexistence.

## MAC addressing: your only namespace

There's no broker, no service discovery, no DNS. The only identity primitive
ESP-NOW gives you is the 6-byte MAC address of the sending or receiving
radio. Every other layer of identity — "this is temperature sensor 7 in the
warehouse, tenant acme-corp" — is something *you* build on top, by
maintaining a MAC-to-identity mapping somewhere. That mapping is exactly
what PulseLink's gateway-side registry is (Part 3), and it's worth
internalizing now: ESP-NOW hands you a MAC address and a payload, nothing
else. Sender identity above the MAC layer is not present on the wire and
must never be trusted from payload content either — a detail that matters
once you start thinking about a node spoofing another node's traffic.

Unicast requires a peer table entry before you can send — you register the
peer's MAC via `esp_now_add_peer()` first. Broadcast (`FF:FF:FF:FF:FF:FF`)
needs no peer registration, which is exactly why it's the *only* legitimate
use of broadcast in this series' protocol: discovering a gateway's MAC
address in the first place, before a peer entry can exist. Every other
message type is unicast. More on why broadcast-only designs are tempting
and wrong for anything past a handful of nodes in the sidebar below.

## The 250-byte ceiling, and why we don't just use v2

250 bytes sounds small until you've actually designed a frame format inside
it. PulseLink's header is 7 bytes (magic, version, msg_type, seq, a 2-byte
cmd_id, payload_len), leaving 243 bytes for the body — plenty for a handful
of self-describing sensor fields (Part 2), tight but workable for a batched
uplink. ESP-NOW v2 lifts the cap to 1470 bytes on chips that support it, and
if you're targeting only newer silicon, it's a reasonable trade to make.
This series deliberately stays on v1 framing: the smaller ceiling forces the
frame-design discipline the series is actually trying to teach (self-describing
fields instead of "just add another struct member"), and it keeps the
reference implementation portable to the widest range of ESP32 variants
instead of chip-specific. That's a scope decision, not a technical
limitation of v2 — noted here so you know it's a choice.

## What "no routing" costs you

This is the part most tutorials skip, because it only bites at scale. Since
ESP-NOW doesn't route, every node you add is a peer relationship someone has
to manage — a peer table entry, a registry record, a place in whatever
liveness/retry bookkeeping your application maintains. Espressif's official
ceiling is 20 encrypted peers or a larger number of unencrypted ones, and in
practice a single gateway radio starts feeling that pressure well before
you'd hit a hard limit: airtime is shared, and every node's uplink,
retries, and command traffic all contend for the same channel. PulseLink's
whole architecture — one gateway, ≤20 nodes, no mesh, no multi-hop — is
built around treating that ceiling as a real constraint rather than
something to engineer around. If you need hundreds of nodes or multi-hop
reach, ESP-NOW's mesh variant (ESP-MESH) or a different radio entirely is
the honest answer, not stacking gateways yourself. That's explicitly out of
scope for this series (see the repo's `DECISIONS.md` and `PLAN.md` scope
guards) — not because it's impossible, but because it's a different set of
problems than the ones worth teaching here.

## ESP-NOW vs. WiFi vs. BLE vs. LoRa — an honest comparison

Every one of these gets pitched as "the IoT radio." They solve different
problems.

| | ESP-NOW | Plain WiFi (TCP/MQTT direct) | BLE | LoRa |
|---|---|---|---|---|
| Setup latency | ~ms, no association | Seconds (assoc + DHCP + TCP) | ~100ms–1s (advertise/connect) | N/A (also connectionless) |
| Range | Same as WiFi, ~tens of meters indoors | Same as WiFi | Shorter than WiFi, ~10s of meters | Kilometers |
| Throughput | Small frames, low overhead | High (full TCP/IP stack) | Low-moderate | Very low (bytes/sec at long range) |
| Power | Good — no assoc/keepalive overhead | Poor unless you fight power-save | Excellent (designed for it) | Excellent |
| Needs an AP/router? | No (but shares radio with one if present) | Yes | No | No |
| Internet reach | None — link-layer only | Direct | None — needs a phone/gateway | None — needs a gateway |
| Best at | Many battery/low-latency nodes, one hop to a gateway | A single device that genuinely needs the internet | Phone-paired wearables/peripherals | Long-range, very-low-bandwidth, wide-area sensors |

The honest takeaway: if a single ESP32 just needs to talk to the internet
and power isn't tight, skip this whole series and connect it to WiFi
directly with a normal MQTT or HTTP client — that's simpler and every step
of this gateway architecture exists to solve a problem you don't have. ESP-NOW
earns its place specifically when you have *several to twenty* nodes, want
sub-second wake-send-sleep cycles, can tolerate "link-layer only, gateway
required," and are willing to build (or borrow — that's what this series
hands you) the gateway that bridges to everything past the radio's reach.

### Sidebar: why not just broadcast everything?

It's tempting to skip pairing entirely — every node broadcasts its
telemetry, every gateway listens promiscuously, done. No peer table to
manage, no join handshake to design. It falls apart for three reasons
that matter in production: broadcast frames get no MAC-layer
acknowledgment from the radio (ESP-NOW's send-callback *always* reports
success for broadcast, whether or not anything heard it — Part 5 covers
why that makes broadcast fundamentally unsuitable for anything you need
delivery confidence on), broadcast traffic costs airtime for every device
on the channel whether or not they care about the message, and there's no
natural place to hang per-node state like sleep profile or last-seen
liveness. PulseLink uses broadcast for exactly one thing — the initial
`JOIN_REQ` that bootstraps a MAC address into a unicast peer relationship
(Part 3) — and unicast for everything after. If your use case is genuinely
"a handful of nodes, no delivery guarantees needed, don't care who's
listening," broadcast-and-filter is a legitimate simpler pattern; it's just
not the one this series is teaching, and we won't pretend otherwise.

## What's next

Part 2 gets hands-on: two boards, a real frame codec, and the RX-callback
discipline that determines whether your gateway survives a burst of traffic
or silently corrupts state under load. Part 3 covers the channel problem —
what happens when your router changes channels out from under twenty nodes
that have no idea it happened — and the pairing handshake that recovers
from it. Part 4 is the flagship: the full MQTT gateway, wired into
[PulseDash](https://github.com/pulsecoreengineering) with zero changes on
the dashboard side. Part 5 closes the loop on reliability — the two-loop
ack model, command retries, and sleep profiles that make the difference
between a demo and something you'd trust in a wall for a year.

The full reference implementation, including everything demonstrated in
this series, is at
[github.com/pulsecoreengineering/pulselink](https://github.com/pulsecoreengineering/pulselink).
