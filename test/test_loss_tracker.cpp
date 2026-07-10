#include "pl_test.h"

#include "../core/pl_loss_tracker.h"

using pulselink::SeqLossTracker;

PL_TEST_CASE(fresh_tracker_reports_zero_loss) {
  SeqLossTracker t;
  PL_ASSERT(t.received_count() == 0);
  PL_ASSERT(t.lost_count() == 0);
  PL_ASSERT(t.loss_rate_percent() == 0);
}

PL_TEST_CASE(consecutive_seq_has_no_loss) {
  SeqLossTracker t;
  for (uint8_t seq = 0; seq < 10; ++seq) t.record(seq);
  PL_ASSERT(t.received_count() == 10);
  PL_ASSERT(t.lost_count() == 0);
  PL_ASSERT(t.loss_rate_percent() == 0);
}

PL_TEST_CASE(a_gap_counts_the_missing_frames_as_lost) {
  SeqLossTracker t;
  t.record(0);
  t.record(3);  // seq 1 and 2 never arrived
  PL_ASSERT(t.received_count() == 2);
  PL_ASSERT(t.lost_count() == 2);
}

PL_TEST_CASE(loss_rate_percent_is_lost_over_total) {
  SeqLossTracker t;
  t.record(0);
  t.record(2);  // 1 lost, 2 received -> 1/3 = 33%
  PL_ASSERT(t.loss_rate_percent() == 33);
}

PL_TEST_CASE(seq_wraparound_is_not_mistaken_for_loss) {
  SeqLossTracker t;
  t.record(254);
  t.record(255);
  t.record(0);  // wraps around; still consecutive
  t.record(1);
  PL_ASSERT(t.received_count() == 4);
  PL_ASSERT(t.lost_count() == 0);
}
