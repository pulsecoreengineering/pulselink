#ifndef PULSELINK_CORE_PL_LOSS_TRACKER_H
#define PULSELINK_CORE_PL_LOSS_TRACKER_H

#include <cstdint>

// Per-node uplink loss rate from sequence gaps (TRD.md §4.3, FR-7). Feed it
// every accepted (non-duplicate) DATA frame's seq — a gap between
// consecutive accepted values means frames were lost in between, since a
// healthy stream increments seq by exactly 1 each time.
//
// Companion to pl_dedupe.h's LastSeenSeq, not a replacement: dedupe decides
// whether to accept a frame at all; this tracks loss stats for frames
// already accepted. Call record() only on frames LastSeenSeq didn't flag
// as a duplicate.

namespace pulselink {

class SeqLossTracker {
 public:
  SeqLossTracker()
      : has_seen_(false), last_seq_(0), received_count_(0), lost_count_(0) {}

  void record(uint8_t seq) {
    if (has_seen_) {
      // uint8_t arithmetic wraps naturally, so this is correct across the
      // seq-255-to-0 rollover too: 0 immediately after 255 gives gap 0.
      uint8_t gap = static_cast<uint8_t>(seq - last_seq_ - 1);
      lost_count_ += gap;
    }
    ++received_count_;
    last_seq_ = seq;
    has_seen_ = true;
  }

  uint32_t received_count() const { return received_count_; }
  uint32_t lost_count() const { return lost_count_; }

  // 0-100, rounded down. 0 when nothing's been received yet — "no data"
  // and "no loss" are indistinguishable from this class alone; callers
  // that care should also check received_count().
  uint8_t loss_rate_percent() const {
    uint32_t total = received_count_ + lost_count_;
    if (total == 0) return 0;
    return static_cast<uint8_t>((lost_count_ * 100) / total);
  }

 private:
  bool has_seen_;
  uint8_t last_seq_;
  uint32_t received_count_;
  uint32_t lost_count_;
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_LOSS_TRACKER_H
