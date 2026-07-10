#include "pl_test.h"

#include <cstring>

#include "../core/pl_topics.h"

using namespace pulselink;  // NOLINT

PL_TEST_CASE(data_topic_matches_the_trd_scheme) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  uint8_t len = build_data_topic("acme", 7, "temperature", buf, sizeof(buf));
  PL_ASSERT(len > 0);
  PL_ASSERT(strcmp(buf, "pulsecore/acme/7/temperature") == 0);
  PL_ASSERT(len == strlen(buf));
}

PL_TEST_CASE(cmd_topic_matches_the_trd_scheme) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  uint8_t len = build_cmd_topic("acme", 7, buf, sizeof(buf));
  PL_ASSERT(len > 0);
  PL_ASSERT(strcmp(buf, "pulsecore/acme/7/cmd") == 0);
}

PL_TEST_CASE(cmd_status_topic_matches_the_trd_scheme) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  uint8_t len = build_cmd_status_topic("acme", 7, buf, sizeof(buf));
  PL_ASSERT(len > 0);
  PL_ASSERT(strcmp(buf, "pulsecore/acme/7/cmd_status") == 0);
}

PL_TEST_CASE(device_id_zero_formats_correctly) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  build_data_topic("t", 0, "f", buf, sizeof(buf));
  PL_ASSERT(strcmp(buf, "pulsecore/t/0/f") == 0);
}

PL_TEST_CASE(device_id_multi_digit_formats_correctly) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  build_data_topic("t", 19, "f", buf, sizeof(buf));
  PL_ASSERT(strcmp(buf, "pulsecore/t/19/f") == 0);
}

PL_TEST_CASE(topic_builder_fails_closed_on_overflow) {
  char buf[8];  // far too small for "pulsecore/..."
  uint8_t len = build_data_topic("acme", 7, "temperature", buf, sizeof(buf));
  PL_ASSERT(len == 0);
}

PL_TEST_CASE(gateway_topic_uses_gateway_pseudo_segment_not_a_device_id) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  uint8_t len = build_gateway_topic("acme", "ring_overflow", buf, sizeof(buf));
  PL_ASSERT(len > 0);
  PL_ASSERT(strcmp(buf, "pulsecore/acme/gateway/ring_overflow") == 0);
}

PL_TEST_CASE(cmd_topic_device_id_round_trips_through_build_and_parse) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  build_cmd_topic("acme", 19, buf, sizeof(buf));
  PL_ASSERT(parse_cmd_topic_device_id("acme", buf) == 19);
}

PL_TEST_CASE(cmd_topic_device_id_zero_round_trips) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  build_cmd_topic("acme", 0, buf, sizeof(buf));
  PL_ASSERT(parse_cmd_topic_device_id("acme", buf) == 0);
}

PL_TEST_CASE(parse_rejects_wrong_tenant) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  build_cmd_topic("acme", 7, buf, sizeof(buf));
  PL_ASSERT(parse_cmd_topic_device_id("other", buf) == -1);
}

PL_TEST_CASE(parse_rejects_non_cmd_topics) {
  char buf[PULSELINK_MAX_TOPIC_LEN];
  build_data_topic("acme", 7, "temperature", buf, sizeof(buf));
  PL_ASSERT(parse_cmd_topic_device_id("acme", buf) == -1);

  build_cmd_status_topic("acme", 7, buf, sizeof(buf));
  PL_ASSERT(parse_cmd_topic_device_id("acme", buf) == -1);
}

PL_TEST_CASE(parse_rejects_non_numeric_device_id) {
  PL_ASSERT(parse_cmd_topic_device_id("acme", "pulsecore/acme/gateway/cmd") ==
            -1);
}

PL_TEST_CASE(parse_rejects_device_id_out_of_uint8_range) {
  PL_ASSERT(parse_cmd_topic_device_id("acme", "pulsecore/acme/999/cmd") == -1);
}
