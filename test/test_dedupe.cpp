#include "pl_test.h"

#include "../core/pl_dedupe.h"

using pulselink::LastSeenSeq;

PL_TEST_CASE(first_frame_is_never_a_duplicate) {
  LastSeenSeq d;
  PL_ASSERT(!d.is_duplicate(5));
}

PL_TEST_CASE(retransmit_of_last_accepted_seq_is_a_duplicate) {
  LastSeenSeq d;
  d.accept(5);
  PL_ASSERT(d.is_duplicate(5));
}

PL_TEST_CASE(a_new_seq_is_not_a_duplicate) {
  LastSeenSeq d;
  d.accept(5);
  PL_ASSERT(!d.is_duplicate(6));
}

PL_TEST_CASE(seq_wraparound_is_handled) {
  LastSeenSeq d;
  d.accept(255);
  PL_ASSERT(!d.is_duplicate(0));
  d.accept(0);
  PL_ASSERT(d.is_duplicate(0));
  PL_ASSERT(!d.is_duplicate(1));
}
