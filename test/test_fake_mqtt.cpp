#include "pl_test.h"

#include <cstring>

#include "../transport/fake/pl_fake_mqtt.h"

using pulselink::fake::FakeMqttClient;

PL_TEST_CASE(fake_mqtt_logs_published_messages) {
  FakeMqttClient client;
  uint8_t payload[3] = {1, 2, 3};
  PL_ASSERT(client.publish("pulsecore/t/0/x", payload, 3));
  PL_ASSERT(client.publish_count() == 1);
  PL_ASSERT(strcmp(client.published(0).topic, "pulsecore/t/0/x") == 0);
  PL_ASSERT(client.published(0).payload_len == 3);
}

PL_TEST_CASE(disconnected_fake_mqtt_rejects_publish) {
  FakeMqttClient client;
  client.set_connected(false);
  uint8_t payload[1] = {0};
  PL_ASSERT(!client.publish("t", payload, 1));
  PL_ASSERT(client.publish_count() == 0);
}

PL_TEST_CASE(injected_inbound_message_is_returned_by_receive) {
  FakeMqttClient client;
  uint8_t payload[2] = {5, 6};
  client.inject_inbound("pulsecore/t/0/cmd", payload, 2);

  char topic[64];
  uint8_t out_payload[64];
  uint16_t out_len;
  PL_ASSERT(client.receive(topic, sizeof(topic), out_payload, sizeof(out_payload),
                            &out_len));
  PL_ASSERT(strcmp(topic, "pulsecore/t/0/cmd") == 0);
  PL_ASSERT(out_len == 2 && out_payload[0] == 5);
  PL_ASSERT(!client.receive(topic, sizeof(topic), out_payload, sizeof(out_payload),
                             &out_len));
}
