#ifndef PULSELINK_CORE_PL_NACK_H
#define PULSELINK_CORE_PL_NACK_H

#include <cstdint>

// NACK: "received but cannot process" — distinct from silence, which means
// retry (TRD.md §3.2). This is a protocol-dispatch-level rejection, not an
// application-level outcome: it fires when a frame decodes cleanly (right
// magic, right version) but names a msg_type the receiver has no handler
// for, or was built against an incompatible payload shape for that type.
// CMD_ACK's CmdResult (pl_cmdtable.h) is the app-level counterpart — "I
// understood you, and here's what happened when I tried" — used once a
// CMD has actually been dispatched to a command handler.

namespace pulselink {

enum class NackReason : unsigned char {
  kBadVersion = 0,
  kUnknownMsgType,
};

inline uint8_t encode_nack(NackReason reason, uint8_t* out) {
  out[0] = static_cast<uint8_t>(reason);
  return 1;
}

inline bool decode_nack(const uint8_t* buf, uint8_t len, NackReason* out) {
  if (len != 1) return false;
  *out = static_cast<NackReason>(buf[0]);
  return true;
}

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_NACK_H
