// Runnable end-to-end demo: ties the frame codec, field tuples, ring
// buffer, and fake transport together the way sender.ino/receiver.ino do,
// over a simulated medium with realistic drop/duplicate rates — so the
// whole Phase 1 stack can be seen working together, not just asserted on
// in isolation by test/. No hardware required:
//
//   cmake --build build --target part2_host_demo && ./build/examples/part2-node-to-node/part2_host_demo

#include <cstdio>

#include "../../core/pl_dedupe.h"
#include "../../core/pl_fields.h"
#include "../../core/pl_frame.h"
#include "../../core/pl_ring.h"
#include "../../transport/fake/pl_fake_transport.h"

using namespace pulselink;
using namespace pulselink::fake;

static const uint8_t kFieldIdTemperatureC10 = 1;

int main() {
  FakeMedium medium;
  // A little realism: 5% drop, 2% duplicate, so the run below isn't a
  // frictionless best case.
  medium.set_fault_injection(/*drop_permille=*/50, /*duplicate_permille=*/20,
                              /*delay_ticks=*/0);
  medium.set_seed(42);

  uint8_t sender_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
  uint8_t receiver_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  FakeTransport sender(&medium, sender_mac);
  FakeTransport receiver(&medium, receiver_mac);

  int16_t readings[] = {215, 216, 214, 220, 219, 218, 217, 216, 215, 214};
  uint8_t seq = 0;
  int sent = 0, mac_acked = 0;

  for (int16_t reading : readings) {
    uint8_t payload[PULSELINK_MAX_FRAME_PAYLOAD];
    FieldWriter fields(payload, sizeof(payload));
    fields.write_i16(kFieldIdTemperatureC10, reading);

    uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
    uint8_t frame_len = 0;
    encode_frame(MsgType::kData, seq++, /*cmd_id=*/0, payload,
                 fields.length(), frame, &frame_len);

    ++sent;
    bool acked = sender.send_unicast(receiver_mac, frame, frame_len);
    if (acked) ++mac_acked;
    printf("sender: seq=%u temp=%.1fC frame_len=%u mac_ack=%s\n", seq - 1,
           reading / 10.0, frame_len, acked ? "yes" : "no (dropped)");
  }

  printf("\n--- receiver drains its ring (as loop() would) ---\n");
  uint8_t src[6], buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD], len;
  int received = 0, duplicates = 0;
  LastSeenSeq dedupe;

  while (receiver.receive(src, buf, &len)) {
    FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    FrameError err = decode_frame(buf, len, &header, &payload, &payload_len);
    if (err != FrameError::kOk) {
      printf("receiver: decode error %d, discarding\n",
             static_cast<int>(err));
      continue;
    }

    bool dup = dedupe.is_duplicate(header.seq);
    dedupe.accept(header.seq);
    if (dup) ++duplicates;
    ++received;

    FieldReader fields(payload, payload_len);
    FieldValue f;
    while (fields.next(&f)) {
      if (f.field_id == kFieldIdTemperatureC10 &&
          f.type == FieldType::kI16) {
        printf("receiver: seq=%u temp=%.1fC%s\n", header.seq,
               f.value.i16 / 10.0, dup ? "  [duplicate, ignored]" : "");
      }
    }
  }

  printf("\nsummary: sent=%d mac_acked=%d frames_arrived=%d duplicates=%d "
         "ring_overflow=%lu\n",
         sent, mac_acked, received, duplicates, receiver.rx_overflow_count());
  return 0;
}
