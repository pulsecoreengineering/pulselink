#include "pl_test.h"

#include "../core/pl_cmdtable.h"
#include "../core/pl_nack.h"

using namespace pulselink;  // NOLINT

PL_TEST_CASE(cmd_ack_round_trips_every_result_code) {
  const CmdResult results[] = {CmdResult::kOk, CmdResult::kUnknownCmd,
                                CmdResult::kBusy, CmdResult::kInvalidParam,
                                CmdResult::kHwFault};
  for (CmdResult r : results) {
    uint8_t buf[8];
    uint8_t len = encode_cmd_ack(r, buf);
    PL_ASSERT(len == 1);
    CmdResult out;
    PL_ASSERT(decode_cmd_ack(buf, len, &out));
    PL_ASSERT(out == r);
  }
}

PL_TEST_CASE(cmd_ack_wrong_length_is_rejected) {
  uint8_t buf[2] = {0, 0};
  CmdResult out;
  PL_ASSERT(!decode_cmd_ack(buf, 2, &out));
}

PL_TEST_CASE(nack_round_trips) {
  uint8_t buf[8];
  uint8_t len = encode_nack(NackReason::kUnknownMsgType, buf);
  PL_ASSERT(len == 1);
  NackReason out;
  PL_ASSERT(decode_nack(buf, len, &out));
  PL_ASSERT(out == NackReason::kUnknownMsgType);
}

PL_TEST_CASE(enqueue_is_due_immediately) {
  CmdTable table;
  uint8_t payload[2] = {1, 2};
  PL_ASSERT(table.enqueue(/*device_id=*/0, /*cmd_id=*/100, payload, 2,
                           /*now=*/1000));
  CmdSlot* s = table.find(0);
  PL_ASSERT(s != nullptr);
  PL_ASSERT(s->state == CmdState::kPending);
  PL_ASSERT(s->deadline_ticks == 1000);
}

PL_TEST_CASE(second_enqueue_for_same_node_is_rejected_while_one_in_flight) {
  CmdTable table;
  uint8_t payload[1] = {0};
  PL_ASSERT(table.enqueue(0, 1, payload, 1, 0));
  PL_ASSERT(!table.enqueue(0, 2, payload, 1, 0));
}

PL_TEST_CASE(table_full_rejects_new_node) {
  CmdTable table;
  uint8_t payload[1] = {0};
  for (int i = 0; i < PULSELINK_MAX_CMD_SLOTS; ++i) {
    PL_ASSERT(table.enqueue(static_cast<uint8_t>(i), 1, payload, 1, 0));
  }
  PL_ASSERT(!table.enqueue(static_cast<uint8_t>(PULSELINK_MAX_CMD_SLOTS), 1,
                            payload, 1, 0));
}

PL_TEST_CASE(try_deliver_transitions_pending_to_sent_without_counting_a_retry) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(0, 1, payload, 1, 0);
  PL_ASSERT(table.try_deliver(0, 0));
  PL_ASSERT(table.find(0)->state == CmdState::kSent);
  PL_ASSERT(table.find(0)->retry_count == 0);  // first send isn't a retry
}

PL_TEST_CASE(poll_before_deadline_does_not_retransmit) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(0, 1, payload, 1, /*now=*/0);
  table.try_deliver(0, 0);  // first send, deadline now PULSELINK_CMD_RETRY_TIMEOUT_TICKS
  PL_ASSERT(!table.poll(0, /*now=*/1));
  PL_ASSERT(table.find(0)->retry_count == 0);
}

PL_TEST_CASE(poll_after_deadline_retransmits_and_counts_a_retry) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(0, 1, payload, 1, 0);
  table.try_deliver(0, 0);
  PL_ASSERT(table.poll(0, PULSELINK_CMD_RETRY_TIMEOUT_TICKS));
  PL_ASSERT(table.find(0)->retry_count == 1);
}

PL_TEST_CASE(retries_exhausted_marks_the_slot_failed) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(0, 1, payload, 1, 0);

  uint32_t now = 0;
  table.try_deliver(0, now);  // first send
  for (uint8_t i = 0; i < PULSELINK_MAX_CMD_RETRIES; ++i) {
    now += PULSELINK_CMD_RETRY_TIMEOUT_TICKS;
    table.poll(0, now);
  }
  // One more attempt past the retry budget flips it to FAILED.
  now += PULSELINK_CMD_RETRY_TIMEOUT_TICKS;
  PL_ASSERT(!table.poll(0, now));
  PL_ASSERT(table.find(0)->state == CmdState::kFailed);
}

PL_TEST_CASE(matching_ack_marks_the_slot_acked) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(5, 42, payload, 1, 0);
  table.try_deliver(5, 0);
  PL_ASSERT(table.on_ack(5, 42));
  PL_ASSERT(table.find(5)->state == CmdState::kAcked);
}

PL_TEST_CASE(ack_with_wrong_cmd_id_does_not_match) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(5, 42, payload, 1, 0);
  PL_ASSERT(!table.on_ack(5, 999));
  PL_ASSERT(table.find(5)->state != CmdState::kAcked);
}

PL_TEST_CASE(clear_if_finished_only_clears_acked_or_failed) {
  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(0, 1, payload, 1, 0);
  table.clear_if_finished(0);
  PL_ASSERT(table.find(0) != nullptr);  // still PENDING, not cleared

  table.on_ack(0, 1);
  table.clear_if_finished(0);
  PL_ASSERT(table.find(0) == nullptr);  // now gone, slot free for reuse
  PL_ASSERT(table.enqueue(0, 2, payload, 1, 0));
}

PL_TEST_CASE(node_dedupe_does_not_flag_a_first_execution) {
  NodeCmdDedupe dedupe;
  PL_ASSERT(!dedupe.already_executed(7));
}

PL_TEST_CASE(node_dedupe_flags_a_repeat_of_the_same_cmd_id) {
  NodeCmdDedupe dedupe;
  dedupe.record_execution(7, CmdResult::kOk);
  PL_ASSERT(dedupe.already_executed(7));
  PL_ASSERT(dedupe.last_result() == CmdResult::kOk);
}

PL_TEST_CASE(node_dedupe_does_not_flag_a_different_cmd_id) {
  NodeCmdDedupe dedupe;
  dedupe.record_execution(7, CmdResult::kOk);
  PL_ASSERT(!dedupe.already_executed(8));
}
