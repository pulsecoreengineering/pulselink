#ifndef PULSELINK_TRANSPORT_PL_TRANSPORT_H
#define PULSELINK_TRANSPORT_PL_TRANSPORT_H

#include <cstdint>

#include "../core/pl_config.h"

// Transport interface: send/recv/peer management, implemented by
// transport/espnow (real ESP-NOW, ESP32-only) and transport/fake
// (in-process, fault-injectable — see PLAN.md Phase 1).
// Zero-heap contract applies here too (CLAUDE.md).

namespace pulselink {

class Transport {
 public:
  virtual ~Transport() = default;

  // Unicast send. Return value is the MAC-layer ack: "a radio received the
  // frame" — drives fast retransmit, nothing more (TRD.md §3.5). It is
  // never a substitute for an app-level CMD_ACK.
  virtual bool send_unicast(const uint8_t dest_mac[6], const uint8_t* data,
                             uint8_t len) = 0;

  // Broadcast send. The MAC-layer ack always lies for broadcast — callers
  // must never trust it for JOIN_REQ delivery confirmation.
  virtual bool send_broadcast(const uint8_t* data, uint8_t len) = 0;

  // Non-blocking poll of this endpoint's inbound queue. Returns false when
  // empty. Safe to call from the main loop only — never from an RX
  // callback (CLAUDE.md callback discipline).
  virtual bool receive(uint8_t out_src_mac[6], uint8_t* out_buf,
                        uint8_t* out_len) = 0;

  virtual void local_mac(uint8_t out_mac[6]) const = 0;
};

}  // namespace pulselink

#endif  // PULSELINK_TRANSPORT_PL_TRANSPORT_H
