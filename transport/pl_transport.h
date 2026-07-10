#ifndef PULSELINK_TRANSPORT_PL_TRANSPORT_H
#define PULSELINK_TRANSPORT_PL_TRANSPORT_H

#include "../core/pl_config.h"

// Transport interface: send/recv/peer management, implemented by
// transport/espnow (real ESP-NOW, ESP32-only) and transport/fake
// (in-process, fault-injectable — see PLAN.md Phase 1).
// Zero-heap contract applies here too (CLAUDE.md).

namespace pulselink {

class Transport {
 public:
  virtual ~Transport() = default;
};

}  // namespace pulselink

#endif  // PULSELINK_TRANSPORT_PL_TRANSPORT_H
