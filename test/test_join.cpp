#include "pl_test.h"

#include <cstring>

#include "../core/pl_join.h"

using namespace pulselink;  // NOLINT

namespace {
const uint8_t kGatewayMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
}  // namespace

PL_TEST_CASE(join_request_round_trips) {
  JoinRequestPayload req;
  req.token[0] = 0xDE;
  req.token[1] = 0xAD;
  req.token[2] = 0xBE;
  req.token[3] = 0xEF;
  req.sleep_profile = SleepProfile::kWakeAndPoll;

  uint8_t buf[16];
  uint8_t len = encode_join_request(req, buf);

  JoinRequestPayload out;
  PL_ASSERT(decode_join_request(buf, len, &out));
  PL_ASSERT(memcmp(out.token, req.token, sizeof(req.token)) == 0);
  PL_ASSERT(out.sleep_profile == SleepProfile::kWakeAndPoll);
}

PL_TEST_CASE(join_request_wrong_length_is_rejected) {
  uint8_t buf[3] = {0, 0, 0};
  JoinRequestPayload out;
  PL_ASSERT(!decode_join_request(buf, sizeof(buf), &out));
}

PL_TEST_CASE(join_ack_round_trips) {
  JoinAckPayload ack{7, 11};
  uint8_t buf[16];
  uint8_t len = encode_join_ack(ack, buf);
  PL_ASSERT(len == 2);

  JoinAckPayload out;
  PL_ASSERT(decode_join_ack(buf, len, &out));
  PL_ASSERT(out.device_id == 7);
  PL_ASSERT(out.channel == 11);
}

PL_TEST_CASE(fresh_node_pairing_state_is_unpaired) {
  NodePairingState state;
  PL_ASSERT(!state.paired());
}

PL_TEST_CASE(join_ack_transitions_to_paired) {
  NodePairingState state;
  state.on_join_ack(kGatewayMac, 6);
  PL_ASSERT(state.paired());
  PL_ASSERT(state.channel() == 6);
  PL_ASSERT(memcmp(state.gateway_mac(), kGatewayMac, 6) == 0);
}

PL_TEST_CASE(consecutive_failures_below_threshold_do_not_trigger_rediscovery) {
  NodePairingState state;
  state.on_join_ack(kGatewayMac, 6);
  for (uint8_t i = 0; i < PULSELINK_MAX_UNICAST_FAILURES - 1; ++i) {
    PL_ASSERT(!state.on_send_result(false));
  }
  PL_ASSERT(state.paired());  // still paired, one shy of the threshold
}

PL_TEST_CASE(reaching_the_failure_threshold_triggers_rediscovery) {
  NodePairingState state;
  state.on_join_ack(kGatewayMac, 6);
  bool triggered = false;
  for (uint8_t i = 0; i < PULSELINK_MAX_UNICAST_FAILURES; ++i) {
    triggered = state.on_send_result(false);
  }
  PL_ASSERT(triggered);
  PL_ASSERT(!state.paired());
}

PL_TEST_CASE(a_successful_send_resets_the_failure_counter) {
  NodePairingState state;
  state.on_join_ack(kGatewayMac, 6);
  for (uint8_t i = 0; i < PULSELINK_MAX_UNICAST_FAILURES - 1; ++i) {
    state.on_send_result(false);
  }
  state.on_send_result(true);  // resets the counter
  for (uint8_t i = 0; i < PULSELINK_MAX_UNICAST_FAILURES - 1; ++i) {
    PL_ASSERT(!state.on_send_result(false));
  }
  PL_ASSERT(state.paired());
}

PL_TEST_CASE(pairing_survives_a_simulated_reboot_via_serialize_deserialize) {
  NodePairingState before;
  before.on_join_ack(kGatewayMac, 9);

  uint8_t bytes[NodePairingState::kSerializedSize];
  before.serialize(bytes);

  NodePairingState after;  // a fresh instance, as if the node just rebooted
  PL_ASSERT(!after.paired());
  after.deserialize(bytes);

  PL_ASSERT(after.paired());
  PL_ASSERT(after.channel() == 9);
  PL_ASSERT(memcmp(after.gateway_mac(), kGatewayMac, 6) == 0);
}

PL_TEST_CASE(rate_limiter_allows_attempts_within_budget) {
  JoinRateLimiter limiter;
  uint8_t mac[6] = {3, 3, 3, 3, 3, 3};
  for (int i = 0; i < PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW; ++i) {
    PL_ASSERT(limiter.allow(mac, /*now=*/i));
  }
}

PL_TEST_CASE(rate_limiter_blocks_spam_beyond_budget) {
  JoinRateLimiter limiter;
  uint8_t mac[6] = {3, 3, 3, 3, 3, 3};
  for (int i = 0; i < PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW; ++i) {
    limiter.allow(mac, /*now=*/0);
  }
  PL_ASSERT(!limiter.allow(mac, /*now=*/0));  // one too many, same window
}

PL_TEST_CASE(rate_limiter_resets_after_the_window_elapses) {
  JoinRateLimiter limiter;
  uint8_t mac[6] = {3, 3, 3, 3, 3, 3};
  for (int i = 0; i < PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW; ++i) {
    limiter.allow(mac, /*now=*/0);
  }
  PL_ASSERT(!limiter.allow(mac, /*now=*/0));

  uint32_t later = PULSELINK_JOIN_RATE_WINDOW_TICKS + 1;
  PL_ASSERT(limiter.allow(mac, later));  // new window, budget refreshed
}

PL_TEST_CASE(rate_limiter_tracks_each_mac_independently) {
  JoinRateLimiter limiter;
  uint8_t mac_a[6] = {1, 1, 1, 1, 1, 1};
  uint8_t mac_b[6] = {2, 2, 2, 2, 2, 2};
  for (int i = 0; i < PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW; ++i) {
    limiter.allow(mac_a, 0);
  }
  PL_ASSERT(!limiter.allow(mac_a, 0));
  PL_ASSERT(limiter.allow(mac_b, 0));  // mac_b's budget is untouched
}
