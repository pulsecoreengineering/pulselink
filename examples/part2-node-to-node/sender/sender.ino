// Part 2 example: sender node. Encodes a DATA frame with the field-tuple
// codec and unicasts it — no pairing, no channel handling, no acks beyond
// what ESP-NOW gives for free. That's deliberate: Part 3 covers discovery
// and channel lock-in, Part 5 covers the app-level ack loop. This sketch
// exists to show the codec and the wire format, nothing else.
//
// LogicFrenzy series, Part 2 — "Two boards talking, done right."
//
// NOT compiled against the real ESP32 Arduino core in this repo's CI —
// that toolchain isn't available in the host-native test environment.
// The codec logic this sketch calls is fully covered by test/test_frame.cpp
// and test/test_fields.cpp against the fake transport; only the ESP-NOW
// glue below is unverified until Phase 4's hardware bring-up (PLAN.md).

#include <esp_now.h>
#include <WiFi.h>

#include "../../../core/pl_config.h"
#include "../../../core/pl_fields.h"
#include "../../../core/pl_frame.h"

// Receiver's MAC address, read off the board and hardcoded here. Real
// pairing (broadcast discovery, NVS-persisted peer) is Part 3's subject.
static uint8_t kReceiverMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

static const uint8_t kFieldIdTemperatureC10 = 1;  // degrees C * 10

static uint8_t g_seq = 0;

static int16_t read_fake_temperature_c10() {
  // Stand-in for a real sensor read — keeps the example self-contained.
  return 215;  // 21.5 C
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init failed");
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kReceiverMac, 6);
  peer.channel = 0;  // current WiFi channel — the router dictates it
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void loop() {
  uint8_t payload[PULSELINK_MAX_FRAME_PAYLOAD];
  pulselink::FieldWriter fields(payload, sizeof(payload));
  fields.write_i16(kFieldIdTemperatureC10, read_fake_temperature_c10());

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kData, g_seq++, /*cmd_id=*/0,
                           payload, fields.length(), frame, &frame_len);

  esp_now_send(kReceiverMac, frame, frame_len);
  delay(1000);
}
