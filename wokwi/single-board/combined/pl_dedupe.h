#ifndef PULSELINK_CORE_PL_DEDUPE_H
#define PULSELINK_CORE_PL_DEDUPE_H

#include <cstdint>

// Uplink dedupe building block: at-least-once wire delivery + this =
// effectively exactly-once execution (TRD.md §3.5). A retried DATA frame
// carries the same seq as the original; a distinct frame carries a
// different one. One instance tracks one sender.

namespace pulselink {

class LastSeenSeq {
 public:
  LastSeenSeq() : has_seen_(false), last_seq_(0) {}

  // True if `seq` is a retransmit of the last frame already accepted.
  bool is_duplicate(uint8_t seq) const {
    return has_seen_ && seq == last_seq_;
  }

  void accept(uint8_t seq) {
    last_seq_ = seq;
    has_seen_ = true;
  }

 private:
  bool has_seen_;
  uint8_t last_seq_;
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_DEDUPE_H
