// Part 3 example: gateway-side join handling. RX callback discipline still
// applies (CLAUDE.md) — the callback only copies frame + MAC into the
// ring; GatewayJoinHandler runs from loop(), never from callback context.
//
// LogicFrenzy series, Part 3 — "The channel problem & pairing."
//
// NOT compiled against the real ESP32 Arduino core in this repo's CI — see
// part2-node-to-node/sender.ino's header comment for why. GatewayJoinHandler
// (token check + rate limiting + registry) is covered end-to-end by
// test/test_join_flow.cpp against the fake transport.

#include <esp_now.h>
#include <WiFi.h>

#include "../../../core/pl_frame.h"
#include "../../../core/pl_join.h"
#include "../../../core/pl_registry.h"
#include "../../../core/pl_ring.h"

static const uint8_t kProvisioningToken[PULSELINK_PROVISIONING_TOKEN_SIZE] = {
    1, 2, 3, 4};
static const uint8_t kGatewayChannel = 6;  // router dictates the channel

static pulselink::UplinkRing g_rx_ring;
static pulselink::Registry g_registry;  // no NVS backend wired up yet
static pulselink::JoinRateLimiter g_rate_limiter;
static pulselink::GatewayJoinHandler g_join_handler(kProvisioningToken,
                                                      &g_registry,
                                                      &g_rate_limiter);

void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 0 || len > 255) return;
  g_rx_ring.push(info->src_addr, data, static_cast<uint8_t>(len));
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  esp_now_init();
  esp_now_register_recv_cb(on_recv);
}

void loop() {
  uint8_t src_mac[6];
  uint8_t buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t len = 0;
  uint32_t now = millis() / 1000;

  while (g_rx_ring.pop(src_mac, buf, &len)) {
    pulselink::FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    if (pulselink::decode_frame(buf, len, &header, &payload, &payload_len) !=
        pulselink::FrameError::kOk) {
      continue;
    }
    if (header.msg_type != pulselink::MsgType::kJoinReq) continue;

    pulselink::JoinRequestPayload req;
    if (!pulselink::decode_join_request(payload, payload_len, &req)) continue;

    pulselink::JoinAckPayload ack;
    if (!g_join_handler.handle(src_mac, req, kGatewayChannel, now, &ack)) {
      continue;  // bad token, rate-limited, or registry full: stay silent
    }

    // Node isn't a known ESP-NOW peer yet — add it before unicasting back.
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, src_mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    uint8_t ack_payload[2];
    uint8_t ack_payload_len = pulselink::encode_join_ack(ack, ack_payload);
    uint8_t ack_frame[PULSELINK_FRAME_HEADER_SIZE + 2];
    uint8_t ack_frame_len = 0;
    pulselink::encode_frame(pulselink::MsgType::kJoinAck, 0, 0, ack_payload,
                             ack_payload_len, ack_frame, &ack_frame_len);
    esp_now_send(src_mac, ack_frame, ack_frame_len);

    Serial.printf("joined device_id=%u\n", ack.device_id);
  }
}
