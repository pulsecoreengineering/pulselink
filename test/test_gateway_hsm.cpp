#include "pl_test.h"

#include "../core/pl_gateway_hsm.h"
#include "../transport/fake/pl_fake_mqtt.h"

using namespace pulselink;        // NOLINT
using namespace pulselink::fake;  // NOLINT

PL_TEST_CASE(fresh_hsm_starts_degraded) {
  GatewayHsm hsm;
  PL_ASSERT(hsm.state() == GatewayState::kDegraded);
}

PL_TEST_CASE(publish_or_spool_publishes_directly_when_bridging) {
  GatewayHsm hsm;
  hsm.on_backhaul_up();
  PL_ASSERT(hsm.state() == GatewayState::kConnectedBridging);

  FakeMqttClient client;
  uint8_t payload[1] = {42};
  hsm.publish_or_spool(&client, "pulsecore/t/0/x", payload, 1);
  PL_ASSERT(client.publish_count() == 1);
  PL_ASSERT(hsm.spool().empty());
}

PL_TEST_CASE(publish_or_spool_spools_when_degraded) {
  // TRD.md §4.4: nodes must not care that the backhaul died — uplink just
  // gets spooled instead of published.
  GatewayHsm hsm;
  uint8_t payload[1] = {42};
  hsm.publish_or_spool(nullptr, "pulsecore/t/0/x", payload, 1);
  PL_ASSERT(hsm.spool().size() == 1);
}

PL_TEST_CASE(backhaul_up_with_empty_spool_goes_straight_to_bridging) {
  GatewayHsm hsm;
  hsm.on_backhaul_up();
  PL_ASSERT(hsm.state() == GatewayState::kConnectedBridging);
}

PL_TEST_CASE(backhaul_up_with_queued_spool_enters_draining_not_bridging) {
  GatewayHsm hsm;
  uint8_t payload[1] = {1};
  hsm.publish_or_spool(nullptr, "pulsecore/t/0/x", payload, 1);  // Degraded: spools

  hsm.on_backhaul_up();
  PL_ASSERT(hsm.state() == GatewayState::kConnectedDraining);
}

PL_TEST_CASE(drain_step_flushes_one_entry_and_transitions_when_empty) {
  GatewayHsm hsm;
  uint8_t p1[1] = {1};
  uint8_t p2[1] = {2};
  hsm.publish_or_spool(nullptr, "pulsecore/t/0/a", p1, 1);
  hsm.publish_or_spool(nullptr, "pulsecore/t/0/b", p2, 1);
  hsm.on_backhaul_up();
  PL_ASSERT(hsm.state() == GatewayState::kConnectedDraining);

  FakeMqttClient client;
  hsm.drain_step(&client);
  PL_ASSERT(client.publish_count() == 1);
  PL_ASSERT(hsm.state() == GatewayState::kConnectedDraining);  // one left

  hsm.drain_step(&client);
  PL_ASSERT(client.publish_count() == 2);
  PL_ASSERT(hsm.state() == GatewayState::kConnectedBridging);  // spool empty
}

PL_TEST_CASE(backhaul_down_mid_bridging_returns_to_degraded) {
  GatewayHsm hsm;
  hsm.on_backhaul_up();
  hsm.on_backhaul_down();
  PL_ASSERT(hsm.state() == GatewayState::kDegraded);
}

PL_TEST_CASE(accept_downlink_allows_only_while_bridging) {
  GatewayHsm hsm;
  DownlinkRefuseReason reason;

  PL_ASSERT(!hsm.accept_downlink(&reason));  // Degraded
  PL_ASSERT(reason == DownlinkRefuseReason::kBackhaulDegraded);

  hsm.on_backhaul_up();
  PL_ASSERT(hsm.accept_downlink(&reason));
  PL_ASSERT(reason == DownlinkRefuseReason::kNone);
}

PL_TEST_CASE(accept_downlink_refuses_with_draining_reason_while_draining) {
  GatewayHsm hsm;
  uint8_t payload[1] = {1};
  hsm.publish_or_spool(nullptr, "pulsecore/t/0/x", payload, 1);
  hsm.on_backhaul_up();
  PL_ASSERT(hsm.state() == GatewayState::kConnectedDraining);

  DownlinkRefuseReason reason;
  PL_ASSERT(!hsm.accept_downlink(&reason));
  PL_ASSERT(reason == DownlinkRefuseReason::kDraining);
}
