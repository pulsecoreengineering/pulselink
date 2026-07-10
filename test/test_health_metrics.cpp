// End-to-end health metrics: per-node loss rate and liveness, gateway-wide
// ring overflow, and cmd_status — all published through the same
// GatewayHsm publish-or-spool path as regular DATA fields (TRD.md FR-7).

#include "pl_test.h"

#include <cstdio>
#include <cstring>

#include "../core/pl_cmd_status.h"
#include "../core/pl_cmdtable.h"
#include "../core/pl_gateway_hsm.h"
#include "../core/pl_loss_tracker.h"
#include "../core/pl_registry.h"
#include "../core/pl_ring.h"
#include "../core/pl_topics.h"
#include "../transport/fake/pl_fake_mqtt.h"

using namespace pulselink;        // NOLINT
using namespace pulselink::fake;  // NOLINT

namespace {
const char kTenantId[] = "acme";
const uint8_t kMacA[6] = {1, 1, 1, 1, 1, 1};

uint16_t format_u32(uint32_t v, uint8_t* out, uint16_t cap) {
  char buf[12];
  int n = snprintf(buf, sizeof(buf), "%u", v);
  if (n <= 0 || static_cast<uint16_t>(n) > cap) return 0;
  memcpy(out, buf, n);
  return static_cast<uint16_t>(n);
}
}  // namespace

PL_TEST_CASE(loss_rate_is_published_as_a_per_node_field) {
  SeqLossTracker loss;
  loss.record(0);
  loss.record(2);  // one frame lost -> 33%

  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  char topic[PULSELINK_MAX_TOPIC_LEN];
  build_data_topic(kTenantId, /*device_id=*/0, "loss_rate", topic,
                    sizeof(topic));
  uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t len = format_u32(loss.loss_rate_percent(), payload, sizeof(payload));
  hsm.publish_or_spool(&mqtt, topic, payload, len);

  PL_ASSERT(mqtt.publish_count() == 1);
  PL_ASSERT(strcmp(mqtt.published(0).topic, "pulsecore/acme/0/loss_rate") == 0);
  PL_ASSERT(memcmp(mqtt.published(0).payload, "33", 2) == 0);
}

PL_TEST_CASE(offline_transition_publishes_an_online_field_of_zero) {
  Registry registry;
  int id = registry.add_or_update(kMacA, SleepProfile::kAlwaysOn, 0);
  uint8_t device_id = static_cast<uint8_t>(id);

  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  bool went_offline =
      registry.check_offline_transition(device_id, PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS);
  PL_ASSERT(went_offline);

  if (went_offline) {
    char topic[PULSELINK_MAX_TOPIC_LEN];
    build_data_topic(kTenantId, device_id, "online", topic, sizeof(topic));
    uint8_t zero[1] = {'0'};
    hsm.publish_or_spool(&mqtt, topic, zero, 1);
  }

  PL_ASSERT(mqtt.publish_count() == 1);
  PL_ASSERT(strcmp(mqtt.published(0).topic, "pulsecore/acme/0/online") == 0);
  PL_ASSERT(mqtt.published(0).payload[0] == '0');
}

PL_TEST_CASE(coming_back_online_publishes_an_online_field_of_one) {
  Registry registry;
  int id = registry.add_or_update(kMacA, SleepProfile::kAlwaysOn, 0);
  uint8_t device_id = static_cast<uint8_t>(id);
  registry.check_offline_transition(device_id, PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS);

  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  bool back_online =
      registry.touch(kMacA, PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS + 10);
  PL_ASSERT(back_online);

  if (back_online) {
    char topic[PULSELINK_MAX_TOPIC_LEN];
    build_data_topic(kTenantId, device_id, "online", topic, sizeof(topic));
    uint8_t one[1] = {'1'};
    hsm.publish_or_spool(&mqtt, topic, one, 1);
  }

  PL_ASSERT(mqtt.publish_count() == 1);
  PL_ASSERT(mqtt.published(0).payload[0] == '1');
}

PL_TEST_CASE(ring_overflow_counter_is_published_under_the_gateway_topic) {
  UplinkRing ring;
  uint8_t mac[6] = {0};
  uint8_t data[1] = {0};
  for (int i = 0; i < PULSELINK_MAX_RING_DEPTH + 5; ++i) ring.push(mac, data, 1);
  PL_ASSERT(ring.overflow_count() == 5);

  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  char topic[PULSELINK_MAX_TOPIC_LEN];
  build_gateway_topic(kTenantId, "ring_overflow", topic, sizeof(topic));
  uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t len = format_u32(ring.overflow_count(), payload, sizeof(payload));
  hsm.publish_or_spool(&mqtt, topic, payload, len);

  PL_ASSERT(strcmp(mqtt.published(0).topic, "pulsecore/acme/gateway/ring_overflow") ==
            0);
  PL_ASSERT(memcmp(mqtt.published(0).payload, "5", 1) == 0);
}

PL_TEST_CASE(cmd_status_publishes_on_ack) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(0, 1, payload, 1, 0);
  table.try_deliver(0, 0);
  table.on_ack(0, 1);

  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  char topic[PULSELINK_MAX_TOPIC_LEN];
  build_cmd_status_topic(kTenantId, 0, topic, sizeof(topic));
  uint8_t status[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t len = format_cmd_status(/*delivered=*/true, CmdResult::kOk, status,
                                    sizeof(status));
  hsm.publish_or_spool(&mqtt, topic, status, len);

  PL_ASSERT(strcmp(mqtt.published(0).topic, "pulsecore/acme/0/cmd_status") == 0);
  PL_ASSERT(memcmp(mqtt.published(0).payload, "ok", 2) == 0);
}

PL_TEST_CASE(cmd_status_publishes_failed_when_retries_are_exhausted) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(0, 1, payload, 1, 0);

  uint32_t now = 0;
  table.try_deliver(0, now);
  for (uint8_t i = 0; i < PULSELINK_MAX_CMD_RETRIES; ++i) {
    now += PULSELINK_CMD_RETRY_TIMEOUT_TICKS;
    table.poll(0, now);
  }
  now += PULSELINK_CMD_RETRY_TIMEOUT_TICKS;
  table.poll(0, now);
  PL_ASSERT(table.find(0)->state == CmdState::kFailed);

  GatewayHsm hsm;
  hsm.on_backhaul_up();
  FakeMqttClient mqtt;

  char topic[PULSELINK_MAX_TOPIC_LEN];
  build_cmd_status_topic(kTenantId, 0, topic, sizeof(topic));
  uint8_t status[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t len =
      format_cmd_status(/*delivered=*/false, CmdResult::kOk, status,
                         sizeof(status));  // result ignored when undelivered
  hsm.publish_or_spool(&mqtt, topic, status, len);

  PL_ASSERT(memcmp(mqtt.published(0).payload, "failed", 6) == 0);
}
