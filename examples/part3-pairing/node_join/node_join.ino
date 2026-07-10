// Part 3 example: node-side join flow. First boot broadcasts JOIN_REQ; a
// successful JOIN_ACK is persisted (gateway MAC + channel) so every later
// boot skips straight to unicast. Enough consecutive unicast send failures
// wipes that pairing and falls back to broadcast discovery again — that
// fallback *is* the channel-change recovery mechanism (TRD.md §5.1 point
// 4, D-008); there's no separate gateway-side trigger for it.
//
// LogicFrenzy series, Part 3 — "The channel problem & pairing."
//
// NOT compiled against the real ESP32 Arduino core in this repo's CI — see
// part2-node-to-node/sender.ino's header comment for why. All the join
// logic this sketch calls (NodePairingState, the JOIN_REQ/JOIN_ACK codec)
// is covered by test/test_join.cpp and test/test_join_flow.cpp against the
// fake transport. NVS read/write below is pseudocode illustrating the
// shape of persistence, not a compiled implementation — that lands with
// the NVS registry backend in Phase 4.

#include <cstring>

#include <esp_now.h>
#include <WiFi.h>

#include "../../../core/pl_frame.h"
#include "../../../core/pl_join.h"

static const uint8_t kProvisioningToken[PULSELINK_PROVISIONING_TOKEN_SIZE] = {
    1, 2, 3, 4};

static pulselink::NodePairingState g_pairing;

static bool load_pairing_from_nvs() {
  uint8_t bytes[pulselink::NodePairingState::kSerializedSize];
  // if (!nvs_read("pairing", bytes, sizeof(bytes))) return false;
  // g_pairing.deserialize(bytes);
  // return g_pairing.paired();
  (void)bytes;
  return false;  // pseudocode stub — see header comment
}

static void save_pairing_to_nvs() {
  uint8_t bytes[pulselink::NodePairingState::kSerializedSize];
  g_pairing.serialize(bytes);
  // nvs_write("pairing", bytes, sizeof(bytes));
}

static void broadcast_join_request() {
  pulselink::JoinRequestPayload req;
  memcpy(req.token, kProvisioningToken, sizeof(kProvisioningToken));
  req.sleep_profile = pulselink::SleepProfile::kAlwaysOn;

  uint8_t payload[16];
  uint8_t payload_len = pulselink::encode_join_request(req, payload);

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + 16];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kJoinReq, /*seq=*/0,
                           /*cmd_id=*/0, payload, payload_len, frame,
                           &frame_len);

  uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcast_mac, frame, frame_len);
  // Real discovery scans channels 1-13 broadcasting on each in turn
  // (TRD.md §5.1 point 1) — omitted here to keep the example minimal.
}

void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 0 || len > 255) return;

  pulselink::FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  if (pulselink::decode_frame(data, static_cast<uint8_t>(len), &header,
                               &payload, &payload_len) !=
      pulselink::FrameError::kOk) {
    return;
  }
  if (header.msg_type != pulselink::MsgType::kJoinAck) return;

  pulselink::JoinAckPayload ack;
  if (!pulselink::decode_join_ack(payload, payload_len, &ack)) return;

  g_pairing.on_join_ack(info->src_addr, ack.channel);
  save_pairing_to_nvs();
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  esp_now_init();
  esp_now_register_recv_cb(on_recv);

  if (!load_pairing_from_nvs()) {
    broadcast_join_request();
  }
}

void loop() {
  if (!g_pairing.paired()) {
    delay(1000);
    broadcast_join_request();
    return;
  }

  // Simplification for readability: esp_now_send()'s return value only
  // means "queued for send," not "a radio received it" — the real
  // MAC-layer ack arrives asynchronously via esp_now_register_send_cb().
  // A production node feeds that callback's result into on_send_result(),
  // not this call's return value.
  uint8_t dummy_data[1] = {0};
  bool queued = esp_now_send(g_pairing.gateway_mac(), dummy_data, 1) == ESP_OK;
  if (g_pairing.on_send_result(queued)) {
    Serial.println("channel assumption invalidated, re-discovering");
  }
  delay(1000);
}
