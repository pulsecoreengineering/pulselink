// Sanity checks for the still-stub core headers (registry, command table —
// implemented in later phases per PLAN.md) plus config bounds shared by
// everything else. Frame codec, field tuples, dedupe, ring buffer, and the
// fake transport each have their own dedicated test files as of Phase 1.

#include "pl_test.h"

#include "../core/pl_cmdtable.h"
#include "../core/pl_config.h"
#include "../core/pl_registry.h"

PL_TEST_CASE(config_bounds_are_positive_and_fit_esp_now_v1) {
  PL_ASSERT(PULSELINK_MAX_NODES > 0);
  PL_ASSERT(PULSELINK_FRAME_HEADER_SIZE == 7);
  PL_ASSERT(PULSELINK_MAX_FRAME_PAYLOAD > 0);
  // ESP-NOW v1 caps the whole payload at 250 B (TRD.md §3.1, D-007).
  PL_ASSERT(PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD <= 250);
  PL_ASSERT(PULSELINK_MAX_RING_DEPTH > 0);
  PL_ASSERT(PULSELINK_MAX_CMD_SLOTS == PULSELINK_MAX_NODES);
}

PL_TEST_CASE(cmd_result_enum_matches_trd_order) {
  // TRD.md §3.4 / D-010: OK, UNKNOWN_CMD, BUSY, INVALID_PARAM, HW_FAULT.
  PL_ASSERT(static_cast<unsigned char>(pulselink::CmdResult::kOk) == 0);
  PL_ASSERT(static_cast<unsigned char>(pulselink::CmdResult::kUnknownCmd) == 1);
  PL_ASSERT(static_cast<unsigned char>(pulselink::CmdResult::kBusy) == 2);
  PL_ASSERT(static_cast<unsigned char>(pulselink::CmdResult::kInvalidParam) == 3);
  PL_ASSERT(static_cast<unsigned char>(pulselink::CmdResult::kHwFault) == 4);
}

PL_TEST_CASE(sleep_profile_enum_compiles) {
  pulselink::SleepProfile p = pulselink::SleepProfile::kWakeAndPoll;
  PL_ASSERT(p != pulselink::SleepProfile::kAlwaysOn);
}
