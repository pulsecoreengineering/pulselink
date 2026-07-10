#include "pl_test.h"

#include "../transport/fake/pl_fake_transport.h"

using namespace pulselink;       // NOLINT
using namespace pulselink::fake; // NOLINT

namespace {
const uint8_t kMacA[6] = {1, 1, 1, 1, 1, 1};
const uint8_t kMacB[6] = {2, 2, 2, 2, 2, 2};
const uint8_t kMacC[6] = {3, 3, 3, 3, 3, 3};
const uint8_t kMacGhost[6] = {9, 9, 9, 9, 9, 9};
}  // namespace

PL_TEST_CASE(unicast_delivers_only_to_the_named_peer) {
  FakeMedium medium;
  FakeTransport a(&medium, kMacA);
  FakeTransport b(&medium, kMacB);
  FakeTransport c(&medium, kMacC);

  uint8_t payload[3] = {9, 8, 7};
  PL_ASSERT(a.send_unicast(kMacB, payload, sizeof(payload)));

  uint8_t src[6], buf[16], len;
  PL_ASSERT(b.receive(src, buf, &len));
  PL_ASSERT(len == 3 && buf[0] == 9);
  PL_ASSERT(mac_equal(src, kMacA));
  PL_ASSERT(!c.receive(src, buf, &len));
}

PL_TEST_CASE(broadcast_delivers_to_every_other_peer) {
  FakeMedium medium;
  FakeTransport a(&medium, kMacA);
  FakeTransport b(&medium, kMacB);
  FakeTransport c(&medium, kMacC);

  uint8_t payload[1] = {42};
  // MAC ack always "succeeds" for broadcast, even though delivery below is
  // what we're actually asserting on (TRD.md §3.5).
  PL_ASSERT(a.send_broadcast(payload, 1));

  uint8_t src[6], buf[16], len;
  PL_ASSERT(b.receive(src, buf, &len));
  PL_ASSERT(c.receive(src, buf, &len));
  PL_ASSERT(!a.receive(src, buf, &len));  // sender doesn't receive its own
}

PL_TEST_CASE(unicast_to_unregistered_peer_reports_mac_failure) {
  FakeMedium medium;
  FakeTransport a(&medium, kMacA);

  uint8_t payload[1] = {1};
  PL_ASSERT(!a.send_unicast(kMacGhost, payload, 1));
}

PL_TEST_CASE(full_drop_rate_drops_every_unicast_frame) {
  FakeMedium medium;
  medium.set_fault_injection(/*drop_permille=*/1000, 0, 0);
  FakeTransport a(&medium, kMacA);
  FakeTransport b(&medium, kMacB);

  uint8_t payload[1] = {1};
  PL_ASSERT(!a.send_unicast(kMacB, payload, 1));

  uint8_t src[6], buf[16], len;
  PL_ASSERT(!b.receive(src, buf, &len));
}

PL_TEST_CASE(full_duplicate_rate_delivers_frame_twice) {
  FakeMedium medium;
  medium.set_fault_injection(0, /*duplicate_permille=*/1000, 0);
  FakeTransport a(&medium, kMacA);
  FakeTransport b(&medium, kMacB);

  uint8_t payload[1] = {5};
  a.send_unicast(kMacB, payload, 1);

  uint8_t src[6], buf[16], len;
  PL_ASSERT(b.receive(src, buf, &len));
  PL_ASSERT(b.receive(src, buf, &len));   // the duplicate
  PL_ASSERT(!b.receive(src, buf, &len));  // nothing more
}

PL_TEST_CASE(delay_injection_holds_frame_until_ticks_elapse) {
  FakeMedium medium;
  medium.set_fault_injection(0, 0, /*delay_ticks=*/2);
  FakeTransport a(&medium, kMacA);
  FakeTransport b(&medium, kMacB);

  uint8_t payload[1] = {5};
  a.send_unicast(kMacB, payload, 1);

  uint8_t src[6], buf[16], len;
  PL_ASSERT(!b.receive(src, buf, &len));  // not yet
  medium.tick();
  PL_ASSERT(!b.receive(src, buf, &len));  // still not yet
  medium.tick();
  PL_ASSERT(b.receive(src, buf, &len));   // delivered after 2 ticks
}
