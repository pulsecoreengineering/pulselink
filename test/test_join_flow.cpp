// End-to-end join flow over the fake transport: a real JOIN_REQ frame goes
// out on the wire, the gateway-side handler decides on it, a real JOIN_ACK
// frame comes back — exercising the frame codec, the join payload codec,
// the registry, and the rate limiter together, the way it actually happens
// on the wire (PLAN.md Phase 2: "JOIN_REQ/JOIN_ACK flow over fake
// transport; provisioning token check").

#include "pl_test.h"

#include <cstring>

#include "../core/pl_frame.h"
#include "../core/pl_join.h"
#include "../core/pl_registry.h"
#include "../transport/fake/pl_fake_transport.h"

using namespace pulselink;        // NOLINT
using namespace pulselink::fake;  // NOLINT

namespace {
const uint8_t kToken[PULSELINK_PROVISIONING_TOKEN_SIZE] = {1, 2, 3, 4};
const uint8_t kWrongToken[PULSELINK_PROVISIONING_TOKEN_SIZE] = {9, 9, 9, 9};
const uint8_t kNodeMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
const uint8_t kGatewayMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
const uint8_t kGatewayChannel = 6;

void send_join_request(FakeTransport* node, const uint8_t token[4],
                        SleepProfile profile, uint8_t seq) {
  JoinRequestPayload req;
  memcpy(req.token, token, PULSELINK_PROVISIONING_TOKEN_SIZE);
  req.sleep_profile = profile;

  uint8_t payload[16];
  uint8_t payload_len = encode_join_request(req, payload);

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + 16];
  uint8_t frame_len = 0;
  encode_frame(MsgType::kJoinReq, seq, /*cmd_id=*/0, payload, payload_len,
               frame, &frame_len);
  node->send_broadcast(frame, frame_len);
}

// Pops one frame off `node`'s ring; true and *out_ack filled if it decodes
// as a JOIN_ACK.
bool receive_join_ack(FakeTransport* node, JoinAckPayload* out_ack) {
  uint8_t src[6], buf[PULSELINK_FRAME_HEADER_SIZE + 16], len;
  if (!node->receive(src, buf, &len)) return false;

  FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  if (decode_frame(buf, len, &header, &payload, &payload_len) !=
      FrameError::kOk) {
    return false;
  }
  if (header.msg_type != MsgType::kJoinAck) return false;
  return decode_join_ack(payload, payload_len, out_ack);
}

// Drains a gateway's inbound ring, runs each JOIN_REQ through the handler,
// and unicasts a JOIN_ACK back when the handler approves it.
void service_join_requests(FakeTransport* gateway, GatewayJoinHandler* handler,
                            uint32_t now_ticks) {
  uint8_t src[6], buf[PULSELINK_FRAME_HEADER_SIZE + 16], len;
  while (gateway->receive(src, buf, &len)) {
    FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    if (decode_frame(buf, len, &header, &payload, &payload_len) !=
        FrameError::kOk) {
      continue;
    }
    if (header.msg_type != MsgType::kJoinReq) continue;

    JoinRequestPayload req;
    if (!decode_join_request(payload, payload_len, &req)) continue;

    JoinAckPayload ack;
    if (!handler->handle(src, req, kGatewayChannel, now_ticks, &ack)) {
      continue;  // silently ignored: bad token, rate-limited, or full
    }

    uint8_t ack_payload[2 + PULSELINK_PROVISIONING_TOKEN_SIZE];
    uint8_t ack_payload_len = encode_join_ack(ack, ack_payload);
    uint8_t ack_frame[PULSELINK_FRAME_HEADER_SIZE + 2 +
                       PULSELINK_PROVISIONING_TOKEN_SIZE];
    uint8_t ack_frame_len = 0;
    encode_frame(MsgType::kJoinAck, 0, 0, ack_payload, ack_payload_len,
                 ack_frame, &ack_frame_len);
    gateway->send_unicast(src, ack_frame, ack_frame_len);
  }
}
}  // namespace

PL_TEST_CASE(fresh_join_succeeds_end_to_end_over_fake_transport) {
  FakeMedium medium;
  FakeTransport node(&medium, kNodeMac);
  FakeTransport gateway(&medium, kGatewayMac);

  Registry registry;
  JoinRateLimiter limiter;
  GatewayJoinHandler handler(kToken, &registry, &limiter);

  send_join_request(&node, kToken, SleepProfile::kAlwaysOn, /*seq=*/0);
  service_join_requests(&gateway, &handler, /*now=*/1);

  JoinAckPayload ack;
  PL_ASSERT(receive_join_ack(&node, &ack));
  PL_ASSERT(ack.channel == kGatewayChannel);
  PL_ASSERT(memcmp(ack.token, kToken, sizeof(kToken)) == 0);
  PL_ASSERT(join_ack_is_authentic(ack, kToken));
  PL_ASSERT(registry.size() == 1);
}

PL_TEST_CASE(wrong_token_join_request_is_ignored) {
  FakeMedium medium;
  FakeTransport node(&medium, kNodeMac);
  FakeTransport gateway(&medium, kGatewayMac);

  Registry registry;
  JoinRateLimiter limiter;
  GatewayJoinHandler handler(kToken, &registry, &limiter);

  send_join_request(&node, kWrongToken, SleepProfile::kAlwaysOn, /*seq=*/0);
  service_join_requests(&gateway, &handler, /*now=*/1);

  JoinAckPayload ack;
  PL_ASSERT(!receive_join_ack(&node, &ack));  // no JOIN_ACK came back
  PL_ASSERT(registry.size() == 0);
}

PL_TEST_CASE(join_spam_beyond_rate_limit_is_ignored) {
  FakeMedium medium;
  FakeTransport node(&medium, kNodeMac);
  FakeTransport gateway(&medium, kGatewayMac);

  Registry registry;
  JoinRateLimiter limiter;
  GatewayJoinHandler handler(kToken, &registry, &limiter);

  // A crash-looping node hammering JOIN_REQ, all in the same tick.
  for (int i = 0; i < PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW + 2; ++i) {
    send_join_request(&node, kToken, SleepProfile::kAlwaysOn,
                       static_cast<uint8_t>(i));
  }
  service_join_requests(&gateway, &handler, /*now=*/0);

  int acks_received = 0;
  JoinAckPayload ack;
  while (receive_join_ack(&node, &ack)) ++acks_received;

  PL_ASSERT(acks_received == PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW);
  PL_ASSERT(registry.size() == 1);  // still only joined once
}

PL_TEST_CASE(channel_change_recovery_rejoins_after_gateway_moves) {
  // Node is paired to a MAC that, from its perspective, has gone silent
  // (simulates a router channel change: the real gateway is on the medium,
  // but unreachable at the node's stale assumed address). After enough
  // consecutive failures the node wipes its pairing and re-broadcasts
  // JOIN_REQ, successfully rejoining the real gateway.
  FakeMedium medium;
  FakeTransport node(&medium, kNodeMac);
  FakeTransport gateway(&medium, kGatewayMac);

  Registry registry;
  JoinRateLimiter limiter;
  GatewayJoinHandler handler(kToken, &registry, &limiter);

  NodePairingState pairing;
  uint8_t stale_gateway_mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  pairing.on_join_ack(stale_gateway_mac, /*channel=*/1);
  PL_ASSERT(pairing.paired());

  bool rediscovery_triggered = false;
  for (uint8_t i = 0; i < PULSELINK_MAX_UNICAST_FAILURES; ++i) {
    uint8_t dummy[1] = {0};
    bool acked = node.send_unicast(stale_gateway_mac, dummy, 1);
    PL_ASSERT(!acked);  // stale_gateway_mac isn't a registered peer
    rediscovery_triggered = pairing.on_send_result(acked);
  }
  PL_ASSERT(rediscovery_triggered);
  PL_ASSERT(!pairing.paired());

  // Node falls back to broadcast discovery and finds the real gateway.
  send_join_request(&node, kToken, SleepProfile::kAlwaysOn, /*seq=*/0);
  service_join_requests(&gateway, &handler, /*now=*/100);

  JoinAckPayload ack;
  PL_ASSERT(receive_join_ack(&node, &ack));

  pairing.on_join_ack(kGatewayMac, ack.channel);
  PL_ASSERT(pairing.paired());
  PL_ASSERT(memcmp(pairing.gateway_mac(), kGatewayMac, 6) == 0);
}
