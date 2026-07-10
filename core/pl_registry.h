#ifndef PULSELINK_CORE_PL_REGISTRY_H
#define PULSELINK_CORE_PL_REGISTRY_H

#include "pl_config.h"

// MAC <-> device_id <-> {sleep_profile, field_map, last_seen, pending_cmds}
// registry. Storage backend pluggable (RAM for host tests, NVS on-target).
// Implemented in Phase 2 (PLAN.md).

namespace pulselink {

enum class SleepProfile : unsigned char {
  kAlwaysOn = 0,
  kWakeAndPoll,
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_REGISTRY_H
