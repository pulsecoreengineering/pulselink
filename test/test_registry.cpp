#include "pl_test.h"

#include <cstring>

#include "../core/pl_registry.h"

using namespace pulselink;  // NOLINT

namespace {
const uint8_t kMacA[6] = {1, 1, 1, 1, 1, 1};
const uint8_t kMacB[6] = {2, 2, 2, 2, 2, 2};
}  // namespace

PL_TEST_CASE(fresh_registry_is_empty) {
  Registry reg;
  PL_ASSERT(reg.size() == 0);
  PL_ASSERT(reg.find_by_mac(kMacA) == nullptr);
}

PL_TEST_CASE(add_or_update_assigns_a_device_id_and_is_findable) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, /*now=*/100);
  PL_ASSERT(id >= 0);
  PL_ASSERT(reg.size() == 1);

  RegistryEntry* e = reg.find_by_mac(kMacA);
  PL_ASSERT(e != nullptr);
  PL_ASSERT(e->device_id == static_cast<uint8_t>(id));
  PL_ASSERT(e->sleep_profile == SleepProfile::kAlwaysOn);
  PL_ASSERT(e->last_seen_ticks == 100);
}

PL_TEST_CASE(rejoin_of_known_mac_keeps_same_device_id) {
  Registry reg;
  int id1 = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, 100);
  int id2 = reg.add_or_update(kMacA, SleepProfile::kWakeAndPoll, 200);
  PL_ASSERT(id1 == id2);
  PL_ASSERT(reg.size() == 1);

  RegistryEntry* e = reg.find_by_mac(kMacA);
  PL_ASSERT(e->sleep_profile == SleepProfile::kWakeAndPoll);  // updated
  PL_ASSERT(e->last_seen_ticks == 200);
}

PL_TEST_CASE(distinct_macs_get_distinct_device_ids) {
  Registry reg;
  int id_a = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, 1);
  int id_b = reg.add_or_update(kMacB, SleepProfile::kAlwaysOn, 1);
  PL_ASSERT(id_a != id_b);
  PL_ASSERT(reg.size() == 2);
}

PL_TEST_CASE(registry_full_rejects_new_macs_but_still_updates_known_ones) {
  Registry reg;
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < PULSELINK_MAX_NODES; ++i) {
    mac[5] = static_cast<uint8_t>(i);
    int id = reg.add_or_update(mac, SleepProfile::kAlwaysOn, 1);
    PL_ASSERT(id >= 0);
  }
  PL_ASSERT(reg.size() == PULSELINK_MAX_NODES);

  uint8_t one_too_many[6] = {9, 9, 9, 9, 9, 9};
  PL_ASSERT(reg.add_or_update(one_too_many, SleepProfile::kAlwaysOn, 1) == -1);

  mac[5] = 0;  // an already-registered mac still updates fine
  PL_ASSERT(reg.add_or_update(mac, SleepProfile::kWakeAndPoll, 2) >= 0);
}

PL_TEST_CASE(ram_storage_survives_across_registry_instances) {
  RamRegistryStorage storage;
  {
    Registry reg(&storage);
    reg.add_or_update(kMacA, SleepProfile::kWakeAndPoll, 42);
  }
  // Simulates a reboot: a fresh Registry loads from the same backend.
  Registry reg2(&storage);
  PL_ASSERT(reg2.size() == 1);
  RegistryEntry* e = reg2.find_by_mac(kMacA);
  PL_ASSERT(e != nullptr);
  PL_ASSERT(e->sleep_profile == SleepProfile::kWakeAndPoll);
  PL_ASSERT(e->last_seen_ticks == 42);
}

PL_TEST_CASE(unmapped_field_name_is_not_found) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, 0);
  PL_ASSERT(reg.find_field_name(static_cast<uint8_t>(id), 1) == nullptr);
}

PL_TEST_CASE(set_field_name_provisions_and_finds_it) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, 0);
  uint8_t device_id = static_cast<uint8_t>(id);
  PL_ASSERT(reg.set_field_name(device_id, 1, "temperature"));

  const char* name = reg.find_field_name(device_id, 1);
  PL_ASSERT(name != nullptr);
  PL_ASSERT(strcmp(name, "temperature") == 0);
}

PL_TEST_CASE(set_field_name_updates_an_existing_mapping) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, 0);
  uint8_t device_id = static_cast<uint8_t>(id);
  reg.set_field_name(device_id, 1, "temp_c");
  reg.set_field_name(device_id, 1, "temperature");

  PL_ASSERT(strcmp(reg.find_field_name(device_id, 1), "temperature") == 0);
}

PL_TEST_CASE(set_field_name_truncates_names_longer_than_the_buffer) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, 0);
  uint8_t device_id = static_cast<uint8_t>(id);
  // 20 chars, longer than PULSELINK_MAX_FIELD_NAME_LEN (16) can hold with
  // a NUL terminator — must truncate cleanly, not overflow.
  reg.set_field_name(device_id, 1, "temperature_celsius");

  const char* name = reg.find_field_name(device_id, 1);
  PL_ASSERT(strlen(name) == PULSELINK_MAX_FIELD_NAME_LEN - 1);
  PL_ASSERT(strncmp(name, "temperature_celsius", PULSELINK_MAX_FIELD_NAME_LEN - 1) ==
            0);
}

PL_TEST_CASE(set_field_name_fails_for_unknown_device_id) {
  Registry reg;
  PL_ASSERT(!reg.set_field_name(0, 1, "x"));
}

PL_TEST_CASE(node_within_timeout_is_not_reported_offline) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, /*now=*/0);
  PL_ASSERT(!reg.check_offline_transition(static_cast<uint8_t>(id),
                                           PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS - 1));
}

PL_TEST_CASE(node_past_timeout_is_reported_offline_exactly_once) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, /*now=*/0);
  uint8_t device_id = static_cast<uint8_t>(id);
  uint32_t stale = PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS;

  PL_ASSERT(reg.check_offline_transition(device_id, stale));
  PL_ASSERT(!reg.check_offline_transition(device_id, stale + 1));  // already reported
}

PL_TEST_CASE(touch_after_offline_report_signals_back_online) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, 0);
  uint8_t device_id = static_cast<uint8_t>(id);
  reg.check_offline_transition(device_id, PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS);

  PL_ASSERT(reg.touch(kMacA, PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS + 10));
  PL_ASSERT(!reg.touch(kMacA, PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS + 11));  // no repeat
}

PL_TEST_CASE(rejoin_also_clears_the_offline_flag) {
  Registry reg;
  int id = reg.add_or_update(kMacA, SleepProfile::kAlwaysOn, 0);
  uint8_t device_id = static_cast<uint8_t>(id);
  reg.check_offline_transition(device_id, PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS);

  reg.add_or_update(kMacA, SleepProfile::kAlwaysOn,
                     PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS + 10);
  PL_ASSERT(!reg.touch(kMacA, PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS + 11));
}
