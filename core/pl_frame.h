#ifndef PULSELINK_CORE_PL_FRAME_H
#define PULSELINK_CORE_PL_FRAME_H

#include "pl_config.h"

// Frame codec: header serialized field-by-field (TRD.md §3.1), Structa-packed
// body. No raw packed-struct casts across the wire (D-005).
// Implemented in Phase 1 (PLAN.md).

namespace pulselink {

enum class MsgType : unsigned char {
  kJoinReq = 0,
  kJoinAck,
  kData,
  kCmd,
  kCmdAck,
  kPing,
  kPong,
  kNack,
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_FRAME_H
