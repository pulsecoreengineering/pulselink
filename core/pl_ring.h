#ifndef PULSELINK_CORE_PL_RING_H
#define PULSELINK_CORE_PL_RING_H

#include "pl_config.h"

// Uplink ring buffer: fixed-capacity, drop-oldest on overflow, overflow
// counter published as a health metric (TRD.md §4.1, CLAUDE.md).
// RX callback discipline: memcpy frame + MAC in, return immediately.
// Implemented in Phase 1 (PLAN.md).

namespace pulselink {

// Placeholder capacity constant so downstream headers/tests have a stable
// symbol to reference before the implementation lands.
constexpr int kRingDepth = PULSELINK_MAX_RING_DEPTH;

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_RING_H
