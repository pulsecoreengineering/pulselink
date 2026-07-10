#include "pl_test.h"

#include <cstring>

#include "../core/pl_spool.h"

using pulselink::MqttSpool;

PL_TEST_CASE(spool_push_pop_preserves_order_and_content) {
  MqttSpool spool;
  uint8_t p1[2] = {1, 2};
  uint8_t p2[1] = {9};
  PL_ASSERT(spool.push("topic/a", p1, 2));
  PL_ASSERT(spool.push("topic/b", p2, 1));
  PL_ASSERT(spool.size() == 2);

  char topic[PULSELINK_MAX_TOPIC_LEN];
  uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t len;

  PL_ASSERT(spool.pop(topic, payload, &len));
  PL_ASSERT(strcmp(topic, "topic/a") == 0);
  PL_ASSERT(len == 2 && payload[0] == 1);

  PL_ASSERT(spool.pop(topic, payload, &len));
  PL_ASSERT(strcmp(topic, "topic/b") == 0);

  PL_ASSERT(!spool.pop(topic, payload, &len));
  PL_ASSERT(spool.empty());
}

PL_TEST_CASE(spool_overflow_drops_oldest_and_counts) {
  MqttSpool spool;
  uint8_t payload[1];
  for (int i = 0; i < PULSELINK_MAX_MQTT_SPOOL_DEPTH + 3; ++i) {
    payload[0] = static_cast<uint8_t>(i);
    spool.push("t", payload, 1);
  }
  PL_ASSERT(spool.size() == PULSELINK_MAX_MQTT_SPOOL_DEPTH);
  PL_ASSERT(spool.overflow_count() == 3);

  char topic[PULSELINK_MAX_TOPIC_LEN];
  uint16_t len;
  spool.pop(topic, payload, &len);
  PL_ASSERT(payload[0] == 3);  // entries 0,1,2 were dropped
}
