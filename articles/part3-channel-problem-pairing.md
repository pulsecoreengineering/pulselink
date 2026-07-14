---
title: "The Channel Problem & Pairing"
series: "Production-Grade ESP-NOW"
part: 3
status: draft
---

# Part 3 — The Channel Problem & Pairing

Part 2's sender had the receiver's MAC address hardcoded and never
mentioned a WiFi channel. That's fine for two boards on a desk. It falls
apart the first time this runs somewhere with a real router in the room,
for a reason that has nothing to do with ESP-NOW's protocol design and
everything to do with how the radio underneath it is shared — and it's the
single most common "it worked yesterday, it's broken today" bug reports
you'll get once this is running in the field.

The code for this part is
[`examples/part3-pairing`](https://github.com/pulsecoreengineering/pulselink/tree/main/examples/part3-pairing).

## Why a WiFi router gets a vote in your ESP-NOW cluster

ESP-NOW rides on the same radio as WiFi. If your gateway is *also* a WiFi
station — connected to your router so it can reach MQTT, which every
gateway in this series is — then the gateway's radio is locked to whatever
channel that router assigned it. ESP-NOW frames on that gateway go out and
come in on that same channel, because there's only one radio and it's only
tuned to one channel at a time.

Nodes don't get a vote. A node that hardcodes "channel 6" because that's
what the gateway happened to be on during setup will silently stop hearing
anything the moment the router — responding to interference, a firmware
update, or just its own DFS/auto-channel logic — reassigns itself to
channel 11. Nothing crashes. Nothing errors. Frames just stop arriving, on
both ends, and there is no notification anywhere in the 802.11 or ESP-NOW
stack that this happened. Your node is still transmitting perfectly good
frames into a channel nobody's listening on.

This is why TRD.md puts it plainly: **the router dictates the channel for
the entire ESP-NOW cluster**, and channel-change recovery has to be
something nodes detect and fix themselves, because there's no gateway-side
mechanism that can push a channel change to a node that can no longer hear
it. If the node can't hear the gateway, the gateway telling it "hey, we
moved" is exactly the message that can't get through.

## The handshake: broadcast once, unicast forever after

Everything else in this protocol is unicast, and Part 1's sidebar already
covered why: MAC-layer acks lie for broadcast, so nothing that needs
delivery confidence can use it. But unicast requires knowing the peer's MAC
address ahead of time via `esp_now_add_peer()` — which is exactly the
chicken-and-egg problem a brand-new node has on first boot. It doesn't know
the gateway's MAC yet. So `JOIN_REQ` is the one deliberate exception: a
broadcast frame, carrying a provisioning token, sent while scanning
channels, whose entire purpose is bootstrapping enough information to
switch to unicast for everything after.

```
node: broadcast JOIN_REQ (token, sleep_profile) on channel N
gateway: token check + rate limit → assign/confirm device_id
gateway: unicast JOIN_ACK (device_id, channel, token echo) back to the node's MAC
node: verify the token echo → persist {gateway_mac, channel} → unicast from here on
```

`core/pl_join.h` implements both bodies as fixed layouts, not
self-describing tuples — Part 2's field codec is deliberately scoped to
DATA payloads (D-004); JOIN_REQ/JOIN_ACK are small, fixed, and known at
compile time, so there's no reason to pay tuple-tagging overhead on them:

```cpp
struct JoinRequestPayload {
  uint8_t token[PULSELINK_PROVISIONING_TOKEN_SIZE];
  SleepProfile sleep_profile;
};

struct JoinAckPayload {
  uint8_t device_id;
  uint8_t channel;
  uint8_t token[PULSELINK_PROVISIONING_TOKEN_SIZE];  // see the callout below
};
```

Same rule as the frame header, though: still no raw struct cast onto the
wire. `encode_join_request`/`decode_join_request` write and read each
field explicitly, same discipline Part 2 covered for the frame header
itself.

## Gateway side: don't trust a token, and don't let one node starve everyone

The gateway's job on receiving a JOIN_REQ is threefold, in this order, and
the order matters: rate-limit first, then check the token, then touch the
registry. `GatewayJoinHandler::handle()`:

```cpp
bool handle(const uint8_t sender_mac[6], const JoinRequestPayload& req,
            uint8_t gateway_channel, uint32_t now_ticks,
            JoinAckPayload* out_ack) {
  if (!rate_limiter_->allow(sender_mac, now_ticks)) return false;
  if (!token_matches(req.token)) return false;

  int device_id =
      registry_->add_or_update(sender_mac, req.sleep_profile, now_ticks);
  if (device_id < 0) return false;
  ...
```

The provisioning token exists because broadcast is, by definition, audible
to every ESP-NOW radio on the channel — anyone can send a frame shaped like
a JOIN_REQ. The token is the gate that keeps a stray or malicious broadcast
from getting a device_id assigned; a request with the wrong token is
silently ignored, not NACKed, because in this protocol silence already
means "retry," and telling an attacker "wrong token, try again" is
strictly worse than telling them nothing.

`JoinRateLimiter` is keyed **per sender MAC**, not globally — a node
crash-looping and spamming JOIN_REQ (TRD.md §7 failure mode 3: a real
scenario, not a hypothetical, since a node that can't get a valid
JOIN_ACK for any reason will just keep retrying) burns through its own
budget within `PULSELINK_JOIN_RATE_WINDOW_TICKS` and gets silently dropped,
without touching any other node's ability to join. That per-MAC keying is
the detail that makes this a rate limiter and not just a shared bucket a
noisy neighbor can drain for everyone else.

## Why JOIN_ACK carries a token too

JOIN_REQ's token answers "should the gateway trust this join attempt."
There's a symmetric question the node has to answer that's easy to miss:
"should I trust this JOIN_ACK." ESP-NOW gives a receiver no way to verify
a frame actually came from the radio it claims to — a source MAC is just
a field the sender's radio reported, and unicast delivery doesn't imply
authentication, only addressing. Left unchecked, a node would accept
*any* frame shaped like a JOIN_ACK — broadcast or unicast — and
re-point its trusted gateway MAC at whoever sent it. That's not a
hypothetical: it's a full node hijack with zero effort, and because it
works on a broadcast frame, one forged packet can hijack every node in
range simultaneously, including ones that are already paired and working
fine.

The fix is two independent checks, deliberately not just one. First,
`JoinAckPayload` now echoes the same provisioning token JOIN_REQ required
— a node checks it with `join_ack_is_authentic()` before trusting
anything else in the frame. Second, and this one matters even if a token
ever leaked: `NodePairingState::on_join_ack()` flatly refuses to re-pair a
node that's already paired —

```cpp
bool on_join_ack(const uint8_t gateway_mac[6], uint8_t channel) {
  if (paired_) return false;
  paired_ = true;
  ...
```

— because a legitimate node only ever broadcasts JOIN_REQ, and therefore
only expects a JOIN_ACK, while it's unpaired in the first place. Once
you're paired, there's no legitimate reason for a second JOIN_ACK to
arrive at all; refusing it outright closes the worse half of this bug (an
already-working node getting silently hijacked) independent of whether
the token check even runs. The companion fix on the command path is the
same idea applied downstream: a node now checks that every `CMD` frame's
sender matches its paired gateway's MAC before executing anything, since
nothing upstream of that point authenticates the sender either.

## Node side: persist once, skip the scan forever after

A `JOIN_ACK` gives the node two things worth writing to flash immediately:
the gateway's MAC address, and the channel it's currently on. `NodePairingState`
holds this plus one more field that does the real work — a consecutive
unicast-failure counter:

```cpp
bool on_send_result(bool mac_acked) {
  if (mac_acked) {
    consecutive_failures_ = 0;
    return false;
  }
  ++consecutive_failures_;
  if (consecutive_failures_ >= PULSELINK_MAX_UNICAST_FAILURES) {
    paired_ = false;
    consecutive_failures_ = 0;
    return true;
  }
  return false;
}
```

Every unicast send result — success or failure — feeds this function.
Success resets the counter to zero; nothing to see, business as usual. But
`PULSELINK_MAX_UNICAST_FAILURES` consecutive failures flips `paired_` back
to false and tells the caller to re-broadcast JOIN_REQ from scratch. That
return value **is** channel-change recovery, in its entirety. There's no
separate "detect a channel change" routine anywhere in this codebase — a
run of unicast failures is indistinguishable from a channel change from
the node's point of view, so the fix is the same either way: forget what
you thought you knew and rediscover it.

This is also why the failure count lives where it lives — in
`NodePairingState`, the node's own long-lived pairing/sending state — and
not inside some destination handler that resets every time it's entered.
That's Pattern 8 (from the *Taming the Loop* material this series leans
on): a retry/failure counter belongs in the state that's doing the sending
and tracking the outcome, never in a destination state's `entry()`, or
every re-entry silently resets progress you were trying to measure.
Command retries in Part 5 follow the identical rule for exactly the same
reason.

`NodePairingState::serialize()`/`deserialize()` are a fixed 8-byte layout
— `paired(1) + gateway_mac(6) + channel(1)` — meant for NVS. In this
example the actual flash read/write calls are commented-out pseudocode;
the byte layout itself is what's under test, round-tripped through a plain
buffer in `test/test_join.cpp` to simulate surviving a reboot without
needing real flash to prove it. (Real NVS I/O for the registry landed on
the gateway side in Phase 4 — `gateway/pl_nvs_registry_storage.h` — node-side
persistence is still pseudocode here because full node firmware is a later
phase; see the repo's `examples/part3-pairing/README.md` for the exact
status.)

## Proving the recovery works without a real router

You can't casually reproduce "the office router changed channels" on
demand, which makes this exact failure mode a good example of something a
fake transport can express and a hardware test can't schedule. `host_demo.cpp`
runs the whole thing over `transport/fake/`: a fresh join, then a simulated
channel change (the node's unicasts to its paired gateway start failing on
purpose), and it watches `PULSELINK_MAX_UNICAST_FAILURES` consecutive
failures trigger exactly the re-discovery path above, ending in a
successful rejoin:

```
cmake --build build --target part3_host_demo
./build/examples/part3-pairing/part3_host_demo
```

This is deliberately the same fake transport from Part 2, the same one
carrying `test/test_join.cpp`, `test/test_registry.cpp`, and
`test/test_join_flow.cpp` — which is where the provisioning-token check,
join-spam rate limiting, and this channel-change scenario are actually
proven, not just demonstrated. Real hardware will still be needed
eventually to confirm the actual RF behavior — real channel-change timing,
real send-callback latency under a live router — but the *logic* of
recovery doesn't need a radio to verify, and that's the whole point of
building it this way.

## What's next

Pairing gets a node a device_id and a working unicast link. It doesn't yet
get any of its data anywhere useful — Part 4 is the flagship: the full
gateway, bridging everything this and Part 2 built into MQTT, so PulseDash
sees real nodes without a single line of dashboard code changing.
