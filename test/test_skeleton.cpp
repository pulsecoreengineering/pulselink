// Phase 0 placeholder: proves the skeleton compiles and wires together —
// config macros are sane and the core headers' enums match the TRD.
// Real protocol-logic tests (frame codec, ring buffer, dedupe, ...) land in
// Phase 1 onward (PLAN.md) alongside their implementations.

#include "pl_test.h"

#include "../core/pl_cmdtable.h"
#include "../core/pl_config.h"
#include "../core/pl_frame.h"
#include "../core/pl_registry.h"
#include "../core/pl_ring.h"
#include "../transport/pl_transport.h"

PL_TEST_CASE(config_bounds_are_positive_and_fit_esp_now_v1) {
  PL_ASSERT(PULSELINK_MAX_NODES > 0);
  PL_ASSERT(PULSELINK_FRAME_HEADER_SIZE == 7);
  PL_ASSERT(PULSELINK_MAX_FRAME_PAYLOAD > 0);
  // ESP-NOW v1 caps the whole payload at 250 B (TRD.md §3.1, D-007).
  PL_ASSERT(PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD <= 250);
  PL_ASSERT(PULSELINK_MAX_RING_DEPTH > 0);
  PL_ASSERT(PULSELINK_MAX_CMD_SLOTS == PULSELINK_MAX_NODES);
}

PL_TEST_CASE(msg_type_enum_compiles) {
  pulselink::MsgType t = pulselink::MsgType::kData;
  PL_ASSERT(static_cast<unsigned char>(t) == 2);
  PL_ASSERT(static_cast<unsigned char>(pulselink::MsgType::kJoinReq) == 0);
  PL_ASSERT(static_cast<unsigned char>(pulselink::MsgType::kNack) == 7);
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

PL_TEST_CASE(transport_interface_is_polymorphic) {
  class NullTransport : public pulselink::Transport {};
  NullTransport t;
  pulselink::Transport* base = &t;
  PL_ASSERT(base == static_cast<pulselink::Transport*>(&t));
}
