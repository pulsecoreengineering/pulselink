#ifndef PULSELINK_CORE_PL_CMDTABLE_H
#define PULSELINK_CORE_PL_CMDTABLE_H

#include "pl_config.h"

// Downlink command table: per-node slots {cmd_id, state, retry_count,
// deadline, payload}. Retry counters live in the calling/sending state,
// never in a destination state's entry() (Pattern 8, CLAUDE.md).
// Implemented in Phase 3 (PLAN.md).

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

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_CMDTABLE_H
