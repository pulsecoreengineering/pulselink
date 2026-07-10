#ifndef PULSELINK_CORE_PL_MAC_H
#define PULSELINK_CORE_PL_MAC_H

#include <cstdint>

// Tiny shared helper — MAC comparison shows up in the registry, the join
// rate limiter, and the fake transport alike.

namespace pulselink {

inline bool mac_equal(const uint8_t a[6], const uint8_t b[6]) {
  for (int i = 0; i < 6; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_MAC_H
