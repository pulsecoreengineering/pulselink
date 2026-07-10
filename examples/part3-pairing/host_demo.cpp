// Runnable end-to-end demo: a node joins a gateway, loses its channel
// assumption (simulating a router channel change), and rejoins — all over
// transport/fake/, no hardware required:
//
//   cmake --build build --target part3_host_demo && ./build/examples/part3-pairing/part3_host_demo

#include <cstdio>
#include <cstring>

#include "../../core/pl_frame.h"
#include "../../core/pl_join.h"
#include "../../core/pl_registry.h"
#include "../../transport/fake/pl_fake_transport.h"

using namespace pulselink;
using namespace pulselink::fake;

static const uint8_t kToken[PULSELINK_PROVISIONING_TOKEN_SIZE] = {1, 2, 3, 4};
static const uint8_t kGatewayChannel = 6;

static void broadcast_join_request(FakeTransport* node, uint8_t seq) {
  JoinRequestPayload req;
  memcpy(req.token, kToken, sizeof(kToken));
  req.sleep_profile = SleepProfile::kAlwaysOn;

  uint8_t payload[16];
  uint8_t payload_len = encode_join_request(req, payload);

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + 16];
  uint8_t frame_len = 0;
  encode_frame(MsgType::kJoinReq, seq, 0, payload, payload_len, frame,
               &frame_len);
  node->send_broadcast(frame, frame_len);
  printf("node: broadcasting JOIN_REQ\n");
}

static void service_gateway(FakeTransport* gateway, GatewayJoinHandler* handler,
                             uint32_t now) {
  uint8_t src[6], buf[64], len;
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
    if (!handler->handle(src, req, kGatewayChannel, now, &ack)) {
      printf("gateway: JOIN_REQ ignored (bad token, rate-limited, or full)\n");
      continue;
    }

    uint8_t ack_payload[2];
    uint8_t ack_payload_len = encode_join_ack(ack, ack_payload);
    uint8_t ack_frame[PULSELINK_FRAME_HEADER_SIZE + 2];
    uint8_t ack_frame_len = 0;
    encode_frame(MsgType::kJoinAck, 0, 0, ack_payload, ack_payload_len,
                 ack_frame, &ack_frame_len);
    gateway->send_unicast(src, ack_frame, ack_frame_len);
    printf("gateway: JOIN_ACK sent, device_id=%u channel=%u\n", ack.device_id,
           ack.channel);
  }
}

static bool receive_join_ack(FakeTransport* node, JoinAckPayload* out_ack) {
  uint8_t src[6], buf[64], len;
  if (!node->receive(src, buf, &len)) return false;
  FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  if (decode_frame(buf, len, &header, &payload, &payload_len) !=
          FrameError::kOk ||
      header.msg_type != MsgType::kJoinAck) {
    return false;
  }
  return decode_join_ack(payload, payload_len, out_ack);
}

int main() {
  uint8_t node_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t gateway_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

  FakeMedium medium;
  FakeTransport node(&medium, node_mac);
  FakeTransport gateway(&medium, gateway_mac);

  Registry registry;
  JoinRateLimiter rate_limiter;
  GatewayJoinHandler handler(kToken, &registry, &rate_limiter);
  NodePairingState pairing;

  printf("--- fresh join ---\n");
  broadcast_join_request(&node, 0);
  service_gateway(&gateway, &handler, /*now=*/0);
  JoinAckPayload ack;
  if (receive_join_ack(&node, &ack)) {
    pairing.on_join_ack(gateway_mac, ack.channel);
    printf("node: paired, device_id=%u channel=%u\n\n", ack.device_id,
           ack.channel);
  }

  printf("--- router changes channel; node's unicasts start failing ---\n");
  // Simulate the channel change by unicasting to a MAC that (from the
  // node's perspective) is unreachable — the real gateway's peer registration
  // is now stale on the old channel.
  uint8_t stale_mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  pairing.on_join_ack(stale_mac, pairing.channel());  // repoint at the stale peer
  for (uint8_t i = 0; i < PULSELINK_MAX_UNICAST_FAILURES; ++i) {
    uint8_t dummy[1] = {0};
    bool acked = node.send_unicast(stale_mac, dummy, 1);
    bool rediscover = pairing.on_send_result(acked);
    printf("node: unicast attempt %u acked=%s%s\n", i,
           acked ? "yes" : "no",
           rediscover ? "  -> re-discovery triggered" : "");
  }
  printf("node: paired=%s\n\n", pairing.paired() ? "yes" : "no");

  printf("--- node re-broadcasts JOIN_REQ and rejoins ---\n");
  broadcast_join_request(&node, 1);
  service_gateway(&gateway, &handler, /*now=*/100);
  if (receive_join_ack(&node, &ack)) {
    pairing.on_join_ack(gateway_mac, ack.channel);
    printf("node: paired, device_id=%u channel=%u\n", ack.device_id,
           ack.channel);
  }

  printf("\nregistry size=%d (still just this one node)\n", registry.size());
  return 0;
}
