#ifndef PULSELINK_CORE_PL_CMDTABLE_H
#define PULSELINK_CORE_PL_CMDTABLE_H

#include <cstdint>
#include <cstring>

#include "pl_config.h"

// Downlink command table: per-node slots {cmd_id, state, retry_count,
// deadline, payload} (TRD.md §4.2). One outstanding command per node at a
// time — matches TRD's "per-node slots" design and keeps this tutorial's
// scope tight; a node's next command simply waits for the slot to clear.
//
// Retry counters live on the slot itself — the calling/sending state's own
// bookkeeping — and are never reset by re-entering some "waiting for ack"
// destination state (Pattern 8, CLAUDE.md). poll()/try_deliver() below only
// ever increment retry_count; nothing resets it except a fresh enqueue().
//
// No persistence here by design (D-006): a gateway reboot loses whatever
// was in flight, and PulseDash learns the outcome via cmd_status timeout.

namespace pulselink {

enum class CmdState : unsigned char {
  kPending = 0,
  kSent,
  kAcked,
  kFailed,
};

// CMD_ACK result codes, fixed from day one (D-010, TRD.md §3.4).
enum class CmdResult : unsigned char {
  kOk = 0,
  kUnknownCmd,
  kBusy,
  kInvalidParam,
  kHwFault,
};

// CMD_ACK payload is just the result byte — cmd_id is already in the frame
// header (TRD.md §3.1: "cmd_id... meaningful only for CMD / CMD_ACK").
inline uint8_t encode_cmd_ack(CmdResult result, uint8_t* out) {
  out[0] = static_cast<uint8_t>(result);
  return 1;
}

inline bool decode_cmd_ack(const uint8_t* buf, uint8_t len, CmdResult* out) {
  if (len != 1) return false;
  *out = static_cast<CmdResult>(buf[0]);
  return true;
}

struct CmdSlot {
  bool used;
  uint8_t device_id;
  uint16_t cmd_id;
  CmdState state;
  uint8_t retry_count;
  uint32_t deadline_ticks;
  uint8_t payload[PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t payload_len;
};

class CmdTable {
 public:
  CmdTable() {
    for (int i = 0; i < PULSELINK_MAX_CMD_SLOTS; ++i) slots_[i].used = false;
  }

  // Enqueues a command for device_id. Returns false if this node
  // already has one in flight, or the table is full — caller decides
  // whether that's an error or "wait and retry the enqueue."
  bool enqueue(uint8_t device_id, uint16_t cmd_id, const uint8_t* payload,
               uint8_t payload_len, uint32_t now_ticks) {
    if (find(device_id)) return false;
    if (payload_len > PULSELINK_MAX_FRAME_PAYLOAD) return false;

    int slot = -1;
    for (int i = 0; i < PULSELINK_MAX_CMD_SLOTS; ++i) {
      if (!slots_[i].used) {
        slot = i;
        break;
      }
    }
    if (slot < 0) return false;

    slots_[slot].used = true;
    slots_[slot].device_id = device_id;
    slots_[slot].cmd_id = cmd_id;
    slots_[slot].state = CmdState::kPending;
    slots_[slot].retry_count = 0;
    slots_[slot].deadline_ticks = now_ticks;  // due immediately
    if (payload_len > 0) memcpy(slots_[slot].payload, payload, payload_len);
    slots_[slot].payload_len = payload_len;
    return true;
  }

  CmdSlot* find(uint8_t device_id) {
    for (int i = 0; i < PULSELINK_MAX_CMD_SLOTS; ++i) {
      if (slots_[i].used && slots_[i].device_id == device_id) {
        return &slots_[i];
      }
    }
    return nullptr;
  }

  // Attempts delivery right now, unconditionally — for when a real
  // opportunity to transmit exists: an ALWAYS_ON node's periodic retry
  // tick, or a WAKE_AND_POLL node's post-uplink listen window. Returns
  // true if the caller should transmit the slot's payload immediately.
  bool try_deliver(uint8_t device_id, uint32_t now_ticks) {
    CmdSlot* s = find(device_id);
    if (!s) return false;
    if (s->state == CmdState::kAcked || s->state == CmdState::kFailed) {
      return false;
    }

    if (s->state == CmdState::kSent) {
      if (s->retry_count >= PULSELINK_MAX_CMD_RETRIES) {
        s->state = CmdState::kFailed;
        return false;
      }
      ++s->retry_count;
    }
    s->state = CmdState::kSent;
    s->deadline_ticks = now_ticks + PULSELINK_CMD_RETRY_TIMEOUT_TICKS;
    return true;
  }

  // Timer-driven variant for ALWAYS_ON nodes: only calls try_deliver()
  // once the slot's own deadline has elapsed, so an ack already in flight
  // isn't preempted by an early retry.
  bool poll(uint8_t device_id, uint32_t now_ticks) {
    CmdSlot* s = find(device_id);
    if (!s) return false;
    if (now_ticks < s->deadline_ticks) return false;
    return try_deliver(device_id, now_ticks);
  }

  // On a matching CMD_ACK: marks the slot ACKED. Returns false if there's
  // no in-flight command for this device_id/cmd_id pair (stale or
  // mismatched ack — nothing to do).
  bool on_ack(uint8_t device_id, uint16_t cmd_id) {
    CmdSlot* s = find(device_id);
    if (!s || s->cmd_id != cmd_id) return false;
    s->state = CmdState::kAcked;
    return true;
  }

  // Frees a finished (ACKED or FAILED) slot so the node can receive its
  // next command. Separate from on_ack()/try_deliver() so callers can
  // publish cmd_status upstream before the record disappears.
  void clear_if_finished(uint8_t device_id) {
    CmdSlot* s = find(device_id);
    if (s && (s->state == CmdState::kAcked || s->state == CmdState::kFailed)) {
      s->used = false;
    }
  }

 private:
  CmdSlot slots_[PULSELINK_MAX_CMD_SLOTS];
};

// Node-side: remembers the last cmd_id actually executed, so a retried CMD
// with the same id is re-acked without re-running the command (TRD.md
// §5.3). Distinct from CmdTable, which is a gateway-side concept.
class NodeCmdDedupe {
 public:
  NodeCmdDedupe() : has_executed_(false), last_cmd_id_(0), last_result_(CmdResult::kOk) {}

  bool already_executed(uint16_t cmd_id) const {
    return has_executed_ && cmd_id == last_cmd_id_;
  }

  void record_execution(uint16_t cmd_id, CmdResult result) {
    has_executed_ = true;
    last_cmd_id_ = cmd_id;
    last_result_ = result;
  }

  // Valid only when already_executed() is true for the cmd_id in question.
  CmdResult last_result() const { return last_result_; }

 private:
  bool has_executed_;
  uint16_t last_cmd_id_;
  CmdResult last_result_;
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_CMDTABLE_H
