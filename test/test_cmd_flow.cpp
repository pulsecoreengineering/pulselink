// End-to-end downlink scenarios over the fake transport: the two-loop ack
// model (MAC-ack vs app-ack, never conflated), the sleep-profile mailbox,
// node-side cmd_id dedupe, and the gateway-reboot-loses-in-flight-commands
// behavior (D-006). This is PLAN.md Phase 3's "test embedded protocol
// logic on the desktop" showpiece, and covers TRD.md §7 failure modes 4
// and 5 (the modes Phase 1/2 didn't already exercise).

#include "pl_test.h"

#include "../core/pl_cmdtable.h"
#include "../core/pl_frame.h"
#include "../transport/fake/pl_fake_transport.h"

using namespace pulselink;        // NOLINT
using namespace pulselink::fake;  // NOLINT

namespace {
const uint8_t kNodeMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
const uint8_t kGatewayMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

void send_cmd(FakeTransport* gateway, const uint8_t dest_mac[6],
              uint16_t cmd_id, const uint8_t* payload, uint8_t payload_len,
              uint8_t seq) {
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t frame_len = 0;
  encode_frame(MsgType::kCmd, seq, cmd_id, payload, payload_len, frame,
               &frame_len);
  gateway->send_unicast(dest_mac, frame, frame_len);
}

// Node side: pops one frame, and if it's a CMD, "executes" it (deduping by
// cmd_id) and replies CMD_ACK. Returns true if a CMD was handled.
bool node_handle_one_cmd(FakeTransport* node, NodeCmdDedupe* dedupe,
                          CmdResult result_if_new) {
  uint8_t src[6], buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD], len;
  if (!node->receive(src, buf, &len)) return false;

  FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  if (decode_frame(buf, len, &header, &payload, &payload_len) !=
      FrameError::kOk) {
    return false;
  }
  if (header.msg_type != MsgType::kCmd) return false;

  CmdResult result;
  if (dedupe->already_executed(header.cmd_id)) {
    result = dedupe->last_result();  // re-ack without re-executing
  } else {
    result = result_if_new;
    dedupe->record_execution(header.cmd_id, result);
  }

  uint8_t ack_payload[1];
  uint8_t ack_payload_len = encode_cmd_ack(result, ack_payload);
  uint8_t ack_frame[PULSELINK_FRAME_HEADER_SIZE + 1];
  uint8_t ack_frame_len = 0;
  encode_frame(MsgType::kCmdAck, 0, header.cmd_id, ack_payload,
               ack_payload_len, ack_frame, &ack_frame_len);
  node->send_unicast(src, ack_frame, ack_frame_len);
  return true;
}

// Gateway side: pops one frame, and if it's a CMD_ACK, feeds it to the
// command table. Returns true if a CMD_ACK was handled.
bool gateway_handle_one_ack(FakeTransport* gateway, CmdTable* table,
                             uint8_t device_id) {
  uint8_t src[6], buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD], len;
  if (!gateway->receive(src, buf, &len)) return false;

  FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  if (decode_frame(buf, len, &header, &payload, &payload_len) !=
      FrameError::kOk) {
    return false;
  }
  if (header.msg_type != MsgType::kCmdAck) return false;

  CmdResult result;
  if (!decode_cmd_ack(payload, payload_len, &result)) return false;
  return table->on_ack(device_id, header.cmd_id);
}
}  // namespace

PL_TEST_CASE(two_loop_ack_model_mac_ack_does_not_imply_app_ack) {
  // TRD.md §3.5: the MAC-layer ack ("a radio received the frame") and the
  // app-level CMD_ACK ("the application executed the command") are two
  // independent signals. A successful unicast send must not be mistaken
  // for command completion.
  FakeMedium medium;
  FakeTransport gateway(&medium, kGatewayMac);
  FakeTransport node(&medium, kNodeMac);

  CmdTable table;
  uint8_t payload[1] = {0};
  table.enqueue(/*device_id=*/0, /*cmd_id=*/1, payload, 1, /*now=*/0);
  PL_ASSERT(table.try_deliver(0, 0));

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + 1];
  uint8_t frame_len = 0;
  encode_frame(MsgType::kCmd, 0, 1, payload, 1, frame, &frame_len);
  bool mac_acked = gateway.send_unicast(kNodeMac, frame, frame_len);

  PL_ASSERT(mac_acked);  // the radio delivered it...
  PL_ASSERT(table.find(0)->state ==
            CmdState::kSent);  // ...but the command table doesn't know that

  // Only an actual CMD_ACK moves it to ACKED.
  NodeCmdDedupe dedupe;
  PL_ASSERT(node_handle_one_cmd(&node, &dedupe, CmdResult::kOk));
  PL_ASSERT(gateway_handle_one_ack(&gateway, &table, 0));
  PL_ASSERT(table.find(0)->state == CmdState::kAcked);
}

PL_TEST_CASE(always_on_command_delivered_and_acked_end_to_end) {
  FakeMedium medium;
  FakeTransport gateway(&medium, kGatewayMac);
  FakeTransport node(&medium, kNodeMac);

  CmdTable table;
  uint8_t payload[1] = {1};  // e.g. "turn on"
  table.enqueue(0, 55, payload, 1, 0);

  PL_ASSERT(table.try_deliver(0, 0));
  send_cmd(&gateway, kNodeMac, 55, payload, 1, 0);

  NodeCmdDedupe dedupe;
  PL_ASSERT(node_handle_one_cmd(&node, &dedupe, CmdResult::kOk));
  PL_ASSERT(gateway_handle_one_ack(&gateway, &table, 0));
  PL_ASSERT(table.find(0)->state == CmdState::kAcked);
}

PL_TEST_CASE(wake_and_poll_mailbox_holds_until_listen_window) {
  // A WAKE_AND_POLL node's radio is off between wakes — the gateway must
  // not spam retries on a timer while it's asleep. The command just sits
  // PENDING until the node's own uplink opens a listen window.
  FakeMedium medium;
  FakeTransport gateway(&medium, kGatewayMac);
  FakeTransport node(&medium, kNodeMac);

  CmdTable table;
  uint8_t payload[1] = {9};
  table.enqueue(0, 77, payload, 1, /*now=*/0);

  // Node is "asleep": the gateway does not call try_deliver()/poll() at
  // all while there's no uplink from it. Time passes...
  PL_ASSERT(table.find(0)->state == CmdState::kPending);  // still just held

  // Node wakes, sends a DATA uplink (irrelevant to this test beyond being
  // the trigger), and its listen window opens. The gateway's uplink
  // handler sees the pending command and delivers it right now.
  PL_ASSERT(table.try_deliver(0, /*now=*/500));
  send_cmd(&gateway, kNodeMac, 77, payload, 1, 0);

  // All of this happens inside the node's brief listen window.
  NodeCmdDedupe dedupe;
  PL_ASSERT(node_handle_one_cmd(&node, &dedupe, CmdResult::kOk));
  PL_ASSERT(gateway_handle_one_ack(&gateway, &table, 0));
  PL_ASSERT(table.find(0)->state == CmdState::kAcked);
}

PL_TEST_CASE(duplicate_cmd_delivery_is_deduped_by_node) {
  // TRD.md §7 failure mode 4 / §5.3: a retried CMD with the same cmd_id is
  // re-acked, not re-executed. Simulated here by the gateway "retrying" a
  // command whose first CMD_ACK got lost before the gateway saw it.
  FakeMedium medium;
  FakeTransport gateway(&medium, kGatewayMac);
  FakeTransport node(&medium, kNodeMac);
  NodeCmdDedupe dedupe;

  uint8_t payload[1] = {3};
  int execute_count = 0;

  // First delivery: genuinely new, executes once.
  send_cmd(&gateway, kNodeMac, 200, payload, 1, 0);
  bool was_new = !dedupe.already_executed(200);
  PL_ASSERT(node_handle_one_cmd(&node, &dedupe, CmdResult::kOk));
  if (was_new) ++execute_count;
  PL_ASSERT(execute_count == 1);

  // Gateway never saw the ack (lost on the way back) and retries with the
  // same cmd_id.
  send_cmd(&gateway, kNodeMac, 200, payload, 1, 1);
  was_new = !dedupe.already_executed(200);
  PL_ASSERT(!was_new);  // the node recognizes the retry before handling it
  PL_ASSERT(node_handle_one_cmd(&node, &dedupe, CmdResult::kOk));
  if (was_new) ++execute_count;

  PL_ASSERT(execute_count == 1);  // still just once — re-acked, not re-run

  // The gateway does see this ack, and it still completes the command.
  CmdTable table;
  table.enqueue(0, 200, payload, 1, 0);
  table.try_deliver(0, 0);
  PL_ASSERT(gateway_handle_one_ack(&gateway, &table, 0));
  PL_ASSERT(table.find(0)->state == CmdState::kAcked);
}

PL_TEST_CASE(gateway_reboot_drops_in_flight_commands_by_design) {
  // TRD.md §7 failure mode 5 / D-006: no NVS persistence of the command
  // table. A reboot is simply a fresh CmdTable — whatever was in flight is
  // gone, not recovered. This test exists to pin that documented behavior
  // down, not to "fix" it.
  CmdTable before_reboot;
  uint8_t payload[1] = {0};
  before_reboot.enqueue(0, 1, payload, 1, 0);
  before_reboot.try_deliver(0, 0);
  PL_ASSERT(before_reboot.find(0)->state == CmdState::kSent);

  CmdTable after_reboot;  // simulates the gateway restarting
  PL_ASSERT(after_reboot.find(0) == nullptr);  // no memory of the old command
}
