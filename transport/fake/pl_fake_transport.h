#ifndef PULSELINK_TRANSPORT_FAKE_PL_FAKE_TRANSPORT_H
#define PULSELINK_TRANSPORT_FAKE_PL_FAKE_TRANSPORT_H

#include <cstdint>
#include <cstring>

#include "../../core/pl_config.h"
#include "../../core/pl_mac.h"
#include "../../core/pl_ring.h"
#include "../pl_transport.h"

// In-process fake transport: simulates a shared ESP-NOW-like medium between
// a gateway + N nodes within one test binary, with injectable frame drops,
// duplicates, and delay (TRD.md §9, D-009). This is where most protocol
// logic coverage lives — no hardware involved.
//
// Zero-heap like everything else under transport/: fixed-capacity arrays
// only, no dynamic allocation or Arduino string type (CLAUDE.md — CI's
// tripwire grep covers this directory too, fake/ included, on purpose).

namespace pulselink {
namespace fake {

using pulselink::mac_equal;

// Shared medium: owns fault-injection policy and routes frames between
// endpoints registered on it. One FakeMedium per simulated cluster.
class FakeMedium {
 public:
  static const int kMaxEndpoints = PULSELINK_MAX_NODES + 1;  // +1 gateway
  static const int kMaxPending = PULSELINK_MAX_RING_DEPTH * 2;
  static const int kMaxFrameBytes =
      PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD;

  FakeMedium()
      : endpoint_count_(0),
        drop_permille_(0),
        duplicate_permille_(0),
        delay_ticks_(0),
        rng_state_(0x2545F491u),
        pending_count_(0) {}

  // permille = parts-per-thousand (0-1000), so fault rates can express
  // sub-percent probabilities without floating point.
  void set_fault_injection(uint16_t drop_permille, uint16_t duplicate_permille,
                            uint16_t delay_ticks) {
    drop_permille_ = drop_permille;
    duplicate_permille_ = duplicate_permille;
    delay_ticks_ = delay_ticks;
  }

  void set_seed(uint32_t seed) { rng_state_ = seed ? seed : 1u; }

  int register_endpoint(const uint8_t mac[6],
                         FrameRing<PULSELINK_MAX_RING_DEPTH>* rx_ring) {
    if (endpoint_count_ >= kMaxEndpoints) return -1;
    int idx = endpoint_count_++;
    memcpy(endpoints_[idx].mac, mac, 6);
    endpoints_[idx].rx_ring = rx_ring;
    return idx;
  }

  // Returns the MAC-layer ack: false if the peer isn't registered on this
  // medium, or if the fault-injection drop roll ate the frame.
  bool send_unicast(int src_idx, const uint8_t dest_mac[6],
                     const uint8_t* data, uint8_t len) {
    int dest_idx = find_endpoint(dest_mac);
    if (dest_idx < 0) return false;
    return try_deliver(src_idx, dest_idx, data, len);
  }

  // No return value: MAC-layer ack always reports success for broadcast
  // regardless of what actually got through (TRD.md §3.5) — callers must
  // get that "lie" from Transport::send_broadcast, not from here.
  void send_broadcast(int src_idx, const uint8_t* data, uint8_t len) {
    for (int i = 0; i < endpoint_count_; ++i) {
      if (i == src_idx) continue;
      try_deliver(src_idx, i, data, len);
    }
  }

  // Advances simulated time by one tick, delivering any delayed frames
  // whose countdown has elapsed. Only meaningful when delay_ticks > 0.
  void tick() {
    int i = 0;
    while (i < pending_count_) {
      if (pending_[i].ticks_remaining > 0) --pending_[i].ticks_remaining;
      if (pending_[i].ticks_remaining == 0) {
        endpoints_[pending_[i].dest_idx].rx_ring->push(
            endpoints_[pending_[i].src_idx].mac, pending_[i].data,
            pending_[i].len);
        pending_[i] = pending_[pending_count_ - 1];
        --pending_count_;
      } else {
        ++i;
      }
    }
  }

 private:
  struct Endpoint {
    uint8_t mac[6];
    FrameRing<PULSELINK_MAX_RING_DEPTH>* rx_ring;
  };

  struct Pending {
    int src_idx;
    int dest_idx;
    uint8_t data[kMaxFrameBytes];
    uint8_t len;
    uint16_t ticks_remaining;
  };

  int find_endpoint(const uint8_t mac[6]) const {
    for (int i = 0; i < endpoint_count_; ++i) {
      if (mac_equal(endpoints_[i].mac, mac)) return i;
    }
    return -1;
  }

  // xorshift32: deterministic given a seed, no heap, no <random>.
  uint32_t next_rand() {
    uint32_t x = rng_state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state_ = x;
    return x;
  }

  bool roll_permille(uint16_t permille) {
    if (permille == 0) return false;
    if (permille >= 1000) return true;
    return (next_rand() % 1000) < permille;
  }

  bool try_deliver(int src_idx, int dest_idx, const uint8_t* data,
                    uint8_t len) {
    if (roll_permille(drop_permille_)) return false;
    enqueue_or_deliver(src_idx, dest_idx, data, len);
    if (roll_permille(duplicate_permille_)) {
      enqueue_or_deliver(src_idx, dest_idx, data, len);
    }
    return true;
  }

  void enqueue_or_deliver(int src_idx, int dest_idx, const uint8_t* data,
                           uint8_t len) {
    if (delay_ticks_ == 0) {
      endpoints_[dest_idx].rx_ring->push(endpoints_[src_idx].mac, data, len);
      return;
    }
    if (pending_count_ >= kMaxPending) return;  // full: drop silently
    Pending& p = pending_[pending_count_++];
    p.src_idx = src_idx;
    p.dest_idx = dest_idx;
    memcpy(p.data, data, len);
    p.len = len;
    p.ticks_remaining = delay_ticks_;
  }

  Endpoint endpoints_[kMaxEndpoints];
  int endpoint_count_;
  uint16_t drop_permille_;
  uint16_t duplicate_permille_;
  uint16_t delay_ticks_;
  uint32_t rng_state_;
  Pending pending_[kMaxPending];
  int pending_count_;
};

// One endpoint's handle onto a FakeMedium — this is what test code hands to
// gateway/node logic in place of the real ESP-NOW transport.
class FakeTransport : public pulselink::Transport {
 public:
  FakeTransport(FakeMedium* medium, const uint8_t mac[6]) : medium_(medium) {
    memcpy(mac_, mac, 6);
    endpoint_idx_ = medium_->register_endpoint(mac_, &rx_ring_);
  }

  bool send_unicast(const uint8_t dest_mac[6], const uint8_t* data,
                     uint8_t len) override {
    return medium_->send_unicast(endpoint_idx_, dest_mac, data, len);
  }

  bool send_broadcast(const uint8_t* data, uint8_t len) override {
    medium_->send_broadcast(endpoint_idx_, data, len);
    return true;  // MAC ack always reports success for broadcast.
  }

  bool receive(uint8_t out_src_mac[6], uint8_t* out_buf,
               uint8_t* out_len) override {
    return rx_ring_.pop(out_src_mac, out_buf, out_len);
  }

  void local_mac(uint8_t out_mac[6]) const override {
    memcpy(out_mac, mac_, 6);
  }

  unsigned long rx_overflow_count() const {
    return rx_ring_.overflow_count();
  }

 private:
  FakeMedium* medium_;
  uint8_t mac_[6];
  int endpoint_idx_;
  FrameRing<PULSELINK_MAX_RING_DEPTH> rx_ring_;
};

}  // namespace fake
}  // namespace pulselink

#endif  // PULSELINK_TRANSPORT_FAKE_PL_FAKE_TRANSPORT_H
