#ifndef PULSELINK_CORE_PL_RING_H
#define PULSELINK_CORE_PL_RING_H

#include <cstdint>
#include <cstring>

#include "pl_config.h"

// Uplink ring buffer: fixed-capacity, drop-oldest on overflow, overflow
// counter published as a health metric (TRD.md §4.1, CLAUDE.md).
//
// RX callback discipline: the ESP-NOW RX callback (WiFi task context) does
// nothing but memcpy the frame + sender MAC into one of these and return —
// parsing and dispatch happen later, in the main loop.

namespace pulselink {

template <int Capacity>
class FrameRing {
 public:
  static const int kMaxFrameBytes =
      PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD;

  FrameRing() : head_(0), tail_(0), count_(0), overflow_count_(0) {}

  // Pushes a frame. Returns false when the ring was already full — the
  // oldest entry was dropped to make room and overflow_count() advanced.
  bool push(const uint8_t mac[6], const uint8_t* data, uint8_t len) {
    if (len > kMaxFrameBytes) return false;

    Slot& slot = slots_[tail_];
    memcpy(slot.mac, mac, 6);
    memcpy(slot.data, data, len);
    slot.len = len;
    tail_ = (tail_ + 1) % Capacity;

    if (count_ == Capacity) {
      head_ = (head_ + 1) % Capacity;
      ++overflow_count_;
      return false;
    }
    ++count_;
    return true;
  }

  bool pop(uint8_t mac[6], uint8_t* data, uint8_t* len) {
    if (count_ == 0) return false;

    Slot& slot = slots_[head_];
    memcpy(mac, slot.mac, 6);
    memcpy(data, slot.data, slot.len);
    *len = slot.len;
    head_ = (head_ + 1) % Capacity;
    --count_;
    return true;
  }

  int size() const { return count_; }
  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == Capacity; }
  unsigned long overflow_count() const { return overflow_count_; }

 private:
  struct Slot {
    uint8_t mac[6];
    uint8_t data[kMaxFrameBytes];
    uint8_t len;
  };

  Slot slots_[Capacity];
  int head_;
  int tail_;
  int count_;
  unsigned long overflow_count_;
};

using UplinkRing = FrameRing<PULSELINK_MAX_RING_DEPTH>;

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_RING_H
