---
title: "Two Boards Talking, Done Right"
series: "Production-Grade ESP-NOW"
part: 2
status: draft
---

# Part 2 — Two Boards Talking, Done Right

Part 1 covered what ESP-NOW is and when to reach for it. This part gets
hands-on with the two decisions that separate a demo from firmware you'd
trust: what you actually put on the wire, and what you're allowed to do
inside the function that receives it. Get either wrong and the bug won't
show up on your desk with two boards three feet apart — it'll show up in
the field, under load, and it'll look like memory corruption because it is.

The code for this part lives in
[`examples/part2-node-to-node`](https://github.com/pulsecoreengineering/pulselink/tree/main/examples/part2-node-to-node):
a sender, a receiver, and a host-native demo that runs the same logic on
your laptop with no hardware at all. We'll walk through all three.

## The trap: casting a struct onto the wire

Here's the version almost every ESP-NOW tutorial reaches for:

```c
struct SensorPacket {
  uint8_t seq;
  int16_t temperature_c10;
};

SensorPacket pkt = {seq++, read_temperature()};
esp_now_send(peer_mac, (uint8_t*)&pkt, sizeof(pkt));
```

It compiles, it runs, it works — right up until it doesn't. The problem is
`sizeof(pkt)` isn't a wire format, it's whatever your compiler's ABI
decided to lay that struct out as, and that's not a stable contract:

- **Padding.** Add a `bool` before that `int16_t` and the compiler is free
  to insert a padding byte to keep the `int16_t` aligned. Your struct
  silently grows, the receiver's identical-looking struct on a *different*
  build (different compiler version, different `-O` flag, a struct member
  reordered six months from now) may not agree on where each field starts.
- **Endianness.** ESP32 is little-endian, so this particular trap won't
  bite you moving data between two ESP32s — but the moment this payload
  needs to be read by anything else (a host-side tool, a different
  architecture, a future chip), a raw multi-byte field is only correct by
  accident.
- **No versioning, no bounds.** There's nothing in the payload that says
  "this is a SensorPacket, v1, 3 bytes." If the sender and receiver ever
  drift — one flashed with a newer firmware than the other — you get
  silent misinterpretation, not a clean error.

None of this shows up with two boards you just flashed from the same
source tree. It shows up eighteen months later when you add a field, flash
half your fleet, and the other half starts printing garbage temperatures.
CLAUDE.md states this as a flat rule for this codebase: **no raw
packed-struct casts across the wire, ever** — every multi-byte field is
serialized explicitly, byte by byte, little-endian, on purpose rather than
by ABI accident.

## What PulseLink puts on the wire instead

Every frame is a 7-byte header, encoded field-by-field, followed by up to
243 bytes of payload (`core/pl_frame.h`):

```
magic(1) | version(1) | msg_type(1) | seq(1) | cmd_id(2, LE) | payload_len(1)
```

`encode_header()` writes each field to an explicit offset — no `struct`,
no cast, no `memcpy` of anything wider than a single documented field:

```cpp
inline void encode_header(const FrameHeader& header,
                           uint8_t out[PULSELINK_FRAME_HEADER_SIZE]) {
  out[0] = header.magic;
  out[1] = static_cast<uint8_t>((header.version & 0x0F) << 4);
  out[2] = static_cast<uint8_t>(header.msg_type);
  out[3] = header.seq;
  out[4] = static_cast<uint8_t>(header.cmd_id & 0xFF);
  out[5] = static_cast<uint8_t>((header.cmd_id >> 8) & 0xFF);
  out[6] = header.payload_len;
}
```

`decode_header()` is the exact inverse, and it does something the raw-cast
version can't: it rejects bad input *before* trusting it. `magic` (a fixed
`0xA5`) has to match, or the frame is foreign traffic on the channel and
gets discarded without going anywhere near the parser. `version`'s high
nibble has to match a protocol version this build understands. `payload_len`
gets cross-checked against the bytes actually received — a truncated frame
is detected, not guessed at. These are TRD.md §7's failure modes 7 and 8
(foreign traffic, truncated/corrupt frames), and they're free once you're
not trusting a cast.

The payload itself is a sequence of self-describing `(field_id, type,
value)` tuples rather than a fixed struct — `core/pl_fields.h`'s
`FieldWriter`/`FieldReader`. That's a second, related decision: instead of
a node type implying a fixed schema the gateway has to know in advance, the
node tags every value with what it *is*. The sender writes one field —
temperature, as a scaled `int16_t`:

```cpp
static const uint8_t kFieldIdTemperatureC10 = 1;  // degrees C * 10
...
pulselink::FieldWriter fields(payload, sizeof(payload));
fields.write_i16(kFieldIdTemperatureC10, read_fake_temperature_c10());
```

and the receiver reads it back by iterating tuples, not by casting a
struct onto the payload bytes:

```cpp
pulselink::FieldReader fields(payload, payload_len);
pulselink::FieldValue field;
while (fields.next(&field)) {
  if (field.field_id == 1 && field.type == pulselink::FieldType::kI16) {
    Serial.printf("temperature: %.1f C\n", field.value.i16 / 10.0);
  }
}
```

The payoff shows up later in the series, not in this file: Part 4's
gateway maps `field_id → MQTT topic name` via a per-node registry, so
adding a new sensor field to a node type never requires reflashing the
gateway. That's only possible because the wire format describes itself
instead of the gateway hardcoding a struct layout per node type.

`FieldWriter` also fails closed — every `write_*` call checks remaining
capacity before it writes a byte, and returns `false` instead of
overflowing the caller's fixed-size buffer if it doesn't fit. On a target
with zero heap and no bounds-checked container types, "fail closed on a
fixed buffer" is doing the job `std::vector` would quietly do for you on a
desktop.

## The trap that actually matters more: what runs in the callback

The wire format bug shows up as bad data. This next one shows up as a
crash, or corrupted unrelated state, and it's the one that actually
justifies this article's title.

`esp_now_register_recv_cb()` registers a function that ESP-NOW's WiFi task
calls directly when a frame arrives — **not** your `loop()`, not a task
you control, a task the WiFi driver owns. Do real work in there — parse a
frame, touch a data structure `loop()` is also touching, call
`Serial.print`, especially allocate anything — and you're one unlucky
timing window away from a race condition or a stack overflow in a task
that wasn't sized for what you just asked it to do. This is the exact same
rule PulseCore's ISR discipline enforces for interrupt handlers, and
ESP-NOW's receive callback is close enough to interrupt context that the
same discipline applies: **copy the data out, flag it, return. Do the
actual work somewhere else.**

Here's the entire body of this project's RX callback:

```cpp
static pulselink::UplinkRing g_rx_ring;

void on_data_recv(const esp_now_recv_info_t* info, const uint8_t* data,
                   int len) {
  // No parsing, no Serial.print, no dispatch here — copy and return.
  if (len < 0 || len > 255) return;
  g_rx_ring.push(info->src_addr, data, static_cast<uint8_t>(len));
}
```

One bounds check, one `memcpy` (inside `push()`), return. `UplinkRing` —
`core/pl_ring.h`'s `FrameRing<PULSELINK_MAX_RING_DEPTH>` — is a
fixed-capacity ring buffer with **no allocation anywhere in its path**:
every slot is pre-sized to the maximum possible frame at construction time,
so `push()` never has to grow anything, it just copies into a slot that
already exists.

All the actual work — decoding the header, checking the magic byte,
reading out DATA fields, deciding what to do with them — happens in
`loop()`, on your own schedule, in a context where a crash is a crash you
can debug instead of a fault in a driver task:

```cpp
void loop() {
  uint8_t mac[6];
  uint8_t buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t len = 0;

  while (g_rx_ring.pop(mac, buf, &len)) {
    pulselink::FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    pulselink::FrameError err =
        pulselink::decode_frame(buf, len, &header, &payload, &payload_len);
    if (err != pulselink::FrameError::kOk) continue;  // discard, don't crash
    ...
```

## What happens when the ring fills up

A fixed-size ring buffer has to have a policy for "more frames arrived
than `loop()` has drained." PulseLink's is drop-oldest: `push()` overwrites
the oldest unread slot and increments an overflow counter rather than
blocking the callback or growing the buffer.

```cpp
if (count_ == Capacity) {
  head_ = (head_ + 1) % Capacity;
  ++overflow_count_;
  return false;
}
```

Dropping the *oldest* frame, not refusing the *newest*, is the right choice
for telemetry: a five-second-old temperature reading is worth less than
the one that just arrived, and refusing new frames would mean the callback
has to make a decision (and potentially block) in the one place it's
least safe to. The overflow counter isn't just a debug aid — Part 4 wires
it straight through as a published health metric, so sustained ring
pressure is something you can actually see on a dashboard instead of
inferring from missing data.

## Proving it without hardware

`host_demo.cpp` runs this exact sender/receiver logic — same frame codec,
same field codec — over `transport/fake/` instead of real ESP-NOW, in one
process, on a laptop:

```
cmake --build build --target part2_host_demo
./build/examples/part2-node-to-node/part2_host_demo
```

It deliberately injects a 5% frame-drop rate and a 2% duplicate rate, so
the output shows something worth noticing: the printed frame count doesn't
quite match the send count, and it's supposed to. That gap is dedupe and
loss working, not a bug — and it's the same fake transport that carries
this repo's full host-native test suite, which is where this codec's
correctness is actually proven (`test/test_frame.cpp`, `test/test_fields.cpp`),
not asserted by eye in a demo. The `.ino` sketches themselves aren't
compiled against a real ESP32 toolchain in this repo's CI — there isn't
one available — so they're syntax-checked against hand-written stub
headers instead. That's a real but bounded check: it catches syntax and
type errors against this project's own headers, not a wrong real Arduino
API signature. Worth being honest about rather than claiming more than
was actually verified.

## What's next

This example deliberately skips two things: how the receiver's MAC address
gets into the sender in the first place (here it's just hardcoded), and
what happens when your router changes channels and every node's cached
assumption about where the gateway lives goes stale. Both are Part 3's
subject — the broadcast discovery handshake, provisioning tokens, and the
NVS-persisted pairing that turns "two boards I flashed by hand" into
something that survives a reboot and a router hiccup.
