#include "pl_test.h"

#include "../core/pl_ring.h"

using pulselink::FrameRing;

PL_TEST_CASE(ring_push_pop_preserves_order_and_content) {
  FrameRing<4> ring;
  uint8_t mac1[6] = {1, 2, 3, 4, 5, 6};
  uint8_t mac2[6] = {6, 5, 4, 3, 2, 1};
  uint8_t data1[3] = {0xAA, 0xBB, 0xCC};
  uint8_t data2[2] = {0x11, 0x22};

  PL_ASSERT(ring.push(mac1, data1, 3));
  PL_ASSERT(ring.push(mac2, data2, 2));
  PL_ASSERT(ring.size() == 2);

  uint8_t out_mac[6];
  uint8_t out_data[16];
  uint8_t out_len;

  PL_ASSERT(ring.pop(out_mac, out_data, &out_len));
  PL_ASSERT(out_len == 3);
  PL_ASSERT(out_mac[0] == 1);
  PL_ASSERT(out_data[0] == 0xAA);

  PL_ASSERT(ring.pop(out_mac, out_data, &out_len));
  PL_ASSERT(out_len == 2);
  PL_ASSERT(out_mac[0] == 6);

  PL_ASSERT(!ring.pop(out_mac, out_data, &out_len));
  PL_ASSERT(ring.empty());
}

PL_TEST_CASE(ring_overflow_drops_oldest_and_counts) {
  FrameRing<2> ring;
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};

  for (uint8_t i = 0; i < 4; ++i) {
    uint8_t d = i;
    ring.push(mac, &d, 1);
  }
  PL_ASSERT(ring.size() == 2);
  PL_ASSERT(ring.full());
  PL_ASSERT(ring.overflow_count() == 2);

  uint8_t out_mac[6];
  uint8_t out_data[16];
  uint8_t out_len;

  ring.pop(out_mac, out_data, &out_len);
  PL_ASSERT(out_data[0] == 2);  // frames 0 and 1 were dropped

  ring.pop(out_mac, out_data, &out_len);
  PL_ASSERT(out_data[0] == 3);
}
