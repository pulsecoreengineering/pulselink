// Part 2 example: receiver node. This is the callback-discipline sketch —
// the RX callback runs in WiFi task context, so it does nothing but memcpy
// the frame + sender MAC into a ring buffer and return. Parsing, printing,
// and any future MQTT publish all happen in loop(), never in the callback
// (CLAUDE.md; same rule as the PulseCore ISR discipline).
//
// LogicFrenzy series, Part 2 — "Two boards talking, done right."
//
// NOT compiled against the real ESP32 Arduino core in this repo's CI — see
// sender.ino's header comment. The esp_now_register_recv_cb() signature
// below matches current (ESP-IDF v5 / arduino-esp32 3.x) cores, which pass
// an esp_now_recv_info_t*; older cores instead pass a plain
// `const uint8_t *mac_addr`. Confirm against your installed core version.

#include <esp_now.h>
#include <WiFi.h>

#include "../../../core/pl_config.h"
#include "../../../core/pl_fields.h"
#include "../../../core/pl_frame.h"
#include "../../../core/pl_ring.h"

static pulselink::UplinkRing g_rx_ring;

void on_data_recv(const esp_now_recv_info_t* info, const uint8_t* data,
                   int len) {
  // No parsing, no Serial.print, no dispatch here — copy and return.
  if (len < 0 || len > 255) return;
  g_rx_ring.push(info->src_addr, data, static_cast<uint8_t>(len));
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init failed");
    return;
  }
  esp_now_register_recv_cb(on_data_recv);
}

void loop() {
  uint8_t mac[6];
  uint8_t buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t len = 0;

  while (g_rx_ring.pop(mac, buf, &len)) {
    pulselink::FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    pulselink::FrameError err =
        pulselink::decode_frame(buf, len, &header, &payload, &payload_len);
    if (err != pulselink::FrameError::kOk) {
      continue;  // foreign traffic or a corrupt frame — silently discard
    }
    if (header.msg_type != pulselink::MsgType::kData) continue;

    pulselink::FieldReader fields(payload, payload_len);
    pulselink::FieldValue field;
    while (fields.next(&field)) {
      if (field.field_id == 1 && field.type == pulselink::FieldType::kI16) {
        Serial.printf("temperature: %.1f C\n", field.value.i16 / 10.0);
      }
    }
  }

  if (g_rx_ring.overflow_count() > 0) {
    Serial.printf("ring overflow: %lu\n", g_rx_ring.overflow_count());
  }
}
