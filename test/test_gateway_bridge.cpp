// End-to-end uplink bridge: a real DATA frame goes out over the fake
// ESP-NOW transport, the gateway decodes it, maps field_id -> MQTT field
// name via the registry, builds the pulsecore/{tenant}/{device}/{field}
// topic, and publishes — or, if the backhaul is down, spools it and
// publishes later once reconnected. This is the whole point of PulseLink:
// bridging an ESP-NOW cluster into the existing PulseCore MQTT tree.

#include "pl_test.h"

#include <cstdio>
#include <cstring>

#include "../core/pl_fields.h"
#include "../core/pl_frame.h"
#include "../core/pl_gateway_hsm.h"
#include "../core/pl_registry.h"
#include "../core/pl_topics.h"
#include "../transport/fake/pl_fake_mqtt.h"
#include "../transport/fake/pl_fake_transport.h"

using namespace pulselink;        // NOLINT
using namespace pulselink::fake;  // NOLINT

namespace {
const uint8_t kNodeMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
const uint8_t kGatewayMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
const char kTenantId[] = "acme";
const uint8_t kFieldIdTemperature = 1;

// Formats a decimal integer as text for the MQTT payload — deliberately
// not part of core/: number formatting for human-readable MQTT payloads is
// gateway presentation logic, not wire protocol (unlike pl_topics.h's
// device_id formatting, which the topic string itself needs).
uint16_t format_i16(int16_t v, uint8_t* out, uint16_t cap) {
  char buf[8];
  int n = snprintf(buf, sizeof(buf), "%d", v);
  if (n <= 0 || static_cast<uint16_t>(n) > cap) return 0;
  memcpy(out, buf, n);
  return static_cast<uint16_t>(n);
}

// Drains the gateway's ESP-NOW ring; for each DATA frame, maps every field
// through the registry and routes it through the HSM (publish or spool).
void bridge_uplink(FakeTransport* gateway, Registry* registry, GatewayHsm* hsm,
                    MqttClient* mqtt) {
  uint8_t src[6], buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD],
      len;
  while (gateway->receive(src, buf, &len)) {
    FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    if (decode_frame(buf, len, &header, &payload, &payload_len) !=
        FrameError::kOk) {
      continue;
    }
    if (header.msg_type != MsgType::kData) continue;

    RegistryEntry* node = registry->find_by_mac(src);
    if (!node) continue;  // unknown sender: not joined, ignore

    FieldReader fields(payload, payload_len);
    FieldValue field;
    while (fields.next(&field)) {
      const char* field_name =
          registry->find_field_name(node->device_id, field.field_id);
      if (!field_name) continue;  // unmapped field: nothing to publish as

      char topic[PULSELINK_MAX_TOPIC_LEN];
      if (build_data_topic(kTenantId, node->device_id, field_name, topic,
                            sizeof(topic)) == 0) {
        continue;
      }

      uint8_t text_payload[PULSELINK_MAX_MQTT_PAYLOAD];
      uint16_t text_len = 0;
      if (field.type == FieldType::kI16) {
        text_len = format_i16(field.value.i16, text_payload,
                               sizeof(text_payload));
      }
      if (text_len == 0) continue;

      hsm->publish_or_spool(mqtt, topic, text_payload, text_len);
    }
  }
}

void send_temperature(FakeTransport* node, int16_t value_c10, uint8_t seq) {
  uint8_t payload[PULSELINK_MAX_FRAME_PAYLOAD];
  FieldWriter fields(payload, sizeof(payload));
  fields.write_i16(kFieldIdTemperature, value_c10);

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t frame_len = 0;
  encode_frame(MsgType::kData, seq, 0, payload, fields.length(), frame,
               &frame_len);
  node->send_unicast(kGatewayMac, frame, frame_len);
}
}  // namespace

PL_TEST_CASE(uplink_data_bridges_to_the_expected_mqtt_topic_when_bridging) {
  FakeMedium medium;
  FakeTransport node(&medium, kNodeMac);
  FakeTransport gateway(&medium, kGatewayMac);

  Registry registry;
  int device_id = registry.add_or_update(kNodeMac, SleepProfile::kAlwaysOn, 0);
  registry.set_field_name(static_cast<uint8_t>(device_id), kFieldIdTemperature,
                           "temperature");

  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  send_temperature(&node, 215, /*seq=*/0);
  bridge_uplink(&gateway, &registry, &hsm, &mqtt);

  PL_ASSERT(mqtt.publish_count() == 1);
  PL_ASSERT(strcmp(mqtt.published(0).topic, "pulsecore/acme/0/temperature") ==
            0);
  PL_ASSERT(mqtt.published(0).payload_len == 3);  // "215"
  PL_ASSERT(memcmp(mqtt.published(0).payload, "215", 3) == 0);
}

PL_TEST_CASE(uplink_data_spools_when_degraded_and_flushes_on_reconnect) {
  FakeMedium medium;
  FakeTransport node(&medium, kNodeMac);
  FakeTransport gateway(&medium, kGatewayMac);

  Registry registry;
  int device_id = registry.add_or_update(kNodeMac, SleepProfile::kAlwaysOn, 0);
  registry.set_field_name(static_cast<uint8_t>(device_id), kFieldIdTemperature,
                           "temperature");

  GatewayHsm hsm;  // starts Degraded — broker is down
  FakeMqttClient mqtt;

  send_temperature(&node, 215, 0);
  bridge_uplink(&gateway, &registry, &hsm, &mqtt);

  PL_ASSERT(mqtt.publish_count() == 0);  // nothing published while Degraded
  PL_ASSERT(hsm.spool().size() == 1);    // held instead

  hsm.on_backhaul_up();
  hsm.drain_step(&mqtt);

  PL_ASSERT(mqtt.publish_count() == 1);
  PL_ASSERT(strcmp(mqtt.published(0).topic, "pulsecore/acme/0/temperature") ==
            0);
  PL_ASSERT(hsm.state() == GatewayState::kConnectedBridging);
}

PL_TEST_CASE(uplink_from_unjoined_node_is_ignored) {
  FakeMedium medium;
  FakeTransport node(&medium, kNodeMac);
  FakeTransport gateway(&medium, kGatewayMac);

  Registry registry;  // node never joined
  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  send_temperature(&node, 215, 0);
  bridge_uplink(&gateway, &registry, &hsm, &mqtt);

  PL_ASSERT(mqtt.publish_count() == 0);
}

PL_TEST_CASE(unmapped_field_is_not_published) {
  FakeMedium medium;
  FakeTransport node(&medium, kNodeMac);
  FakeTransport gateway(&medium, kGatewayMac);

  Registry registry;
  registry.add_or_update(kNodeMac, SleepProfile::kAlwaysOn, 0);
  // Deliberately no set_field_name() call — field_id 1 stays unmapped.

  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  send_temperature(&node, 215, 0);
  bridge_uplink(&gateway, &registry, &hsm, &mqtt);

  PL_ASSERT(mqtt.publish_count() == 0);
}
