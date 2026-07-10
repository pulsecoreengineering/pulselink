#ifndef PULSELINK_CORE_PL_CMD_STATUS_H
#define PULSELINK_CORE_PL_CMD_STATUS_H

#include <cstdint>
#include <cstring>

#include "pl_cmdtable.h"

// cmd_status payload formatting (PRD.md FR-3: "gateway publishes
// cmd_status (delivered/failed + result code)"). PulseDash never assumes
// delivery from a MAC-layer ack or a queued send — this topic, built from
// an actual CMD_ACK or a command table slot hitting FAILED, is the only
// source of truth for what happened to a command.

namespace pulselink {

inline const char* cmd_result_name(CmdResult result) {
  switch (result) {
    case CmdResult::kOk:
      return "ok";
    case CmdResult::kUnknownCmd:
      return "unknown_cmd";
    case CmdResult::kBusy:
      return "busy";
    case CmdResult::kInvalidParam:
      return "invalid_param";
    case CmdResult::kHwFault:
      return "hw_fault";
  }
  return "unknown";
}

// `delivered` distinguishes a real CMD_ACK outcome from a command table
// slot that hit FAILED without ever being acked (retries exhausted, or a
// gateway reboot dropped it — D-006). `result` is only meaningful when
// delivered is true.
inline uint16_t format_cmd_status(bool delivered, CmdResult result,
                                   uint8_t* out, uint16_t cap) {
  const char* text = delivered ? cmd_result_name(result) : "failed";
  uint16_t len = static_cast<uint16_t>(strlen(text));
  if (len > cap) return 0;
  memcpy(out, text, len);
  return len;
}

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_CMD_STATUS_H
