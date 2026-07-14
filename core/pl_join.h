#ifndef PULSELINK_CORE_PL_JOIN_H
#define PULSELINK_CORE_PL_JOIN_H

#include <cstdint>
#include <cstring>

#include "pl_config.h"
#include "pl_mac.h"
#include "pl_registry.h"

// Join/pairing flow (TRD.md §5.1, §7 failure mode 3).
//
// JOIN_REQ and JOIN_ACK bodies are fixed layout, serialized field-by-field
// like the frame header (D-005) — self-describing tuples (pl_fields.h) are
// scoped to DATA payloads only (D-004), not every message type.

namespace pulselink {

struct JoinRequestPayload {
  uint8_t token[PULSELINK_PROVISIONING_TOKEN_SIZE];
  SleepProfile sleep_profile;
};

inline uint8_t encode_join_request(const JoinRequestPayload& p, uint8_t* out) {
  memcpy(out, p.token, PULSELINK_PROVISIONING_TOKEN_SIZE);
  out[PULSELINK_PROVISIONING_TOKEN_SIZE] =
      static_cast<uint8_t>(p.sleep_profile);
  return PULSELINK_PROVISIONING_TOKEN_SIZE + 1;
}

inline bool decode_join_request(const uint8_t* buf, uint8_t len,
                                 JoinRequestPayload* out) {
  if (len != PULSELINK_PROVISIONING_TOKEN_SIZE + 1) return false;
  memcpy(out->token, buf, PULSELINK_PROVISIONING_TOKEN_SIZE);
  out->sleep_profile =
      static_cast<SleepProfile>(buf[PULSELINK_PROVISIONING_TOKEN_SIZE]);
  return true;
}

// `token` echoes the gateway's provisioning token back to the node. Unlike
// JOIN_REQ, ESP-NOW's link layer gives a node no way to tell whether a
// received frame actually came from a radio that heard its broadcast, or
// from any other radio on the channel shaped like a reply (see D-015,
// DECISIONS.md) — so JOIN_ACK carries the same secret JOIN_REQ required,
// letting the node confirm this reply came from something that actually
// knows the fleet's provisioning token before it trusts device_id/channel
// and re-points its unicast peer at `src`.
struct JoinAckPayload {
  uint8_t device_id;
  uint8_t channel;
  uint8_t token[PULSELINK_PROVISIONING_TOKEN_SIZE];
};

inline uint8_t encode_join_ack(const JoinAckPayload& p, uint8_t* out) {
  out[0] = p.device_id;
  out[1] = p.channel;
  memcpy(out + 2, p.token, PULSELINK_PROVISIONING_TOKEN_SIZE);
  return 2 + PULSELINK_PROVISIONING_TOKEN_SIZE;
}

inline bool decode_join_ack(const uint8_t* buf, uint8_t len,
                             JoinAckPayload* out) {
  if (len != 2 + PULSELINK_PROVISIONING_TOKEN_SIZE) return false;
  out->device_id = buf[0];
  out->channel = buf[1];
  memcpy(out->token, buf + 2, PULSELINK_PROVISIONING_TOKEN_SIZE);
  return true;
}

// Node-side authenticity check: does a received JOIN_ACK's token echo match
// what this node was provisioned with? Callers must check this — and that
// the node isn't already paired (NodePairingState::on_join_ack enforces the
// latter) — before trusting a JOIN_ACK's device_id/channel at all.
inline bool join_ack_is_authentic(
    const JoinAckPayload& ack,
    const uint8_t expected_token[PULSELINK_PROVISIONING_TOKEN_SIZE]) {
  for (uint8_t i = 0; i < PULSELINK_PROVISIONING_TOKEN_SIZE; ++i) {
    if (ack.token[i] != expected_token[i]) return false;
  }
  return true;
}

// Node-side pairing state: whether we're paired, to which gateway/channel,
// and the consecutive-unicast-failure counter that drives re-discovery.
class NodePairingState {
 public:
  static const uint8_t kSerializedSize = 8;

  NodePairingState() : paired_(false), channel_(0), consecutive_failures_(0) {
    memset(gateway_mac_, 0, sizeof(gateway_mac_));
  }

  bool paired() const { return paired_; }
  const uint8_t* gateway_mac() const { return gateway_mac_; }
  uint8_t channel() const { return channel_; }

  // Returns false (state unchanged) if this node is already paired. A
  // legitimate node only ever broadcasts JOIN_REQ — and therefore only
  // expects a JOIN_ACK — while unpaired (fresh boot, or after
  // on_send_result() triggers re-discovery); an inbound JOIN_ACK arriving
  // while already paired is either a stale retransmit of the one already
  // acted on, or a forged frame attempting to re-point an already-working
  // pairing at a different MAC, and neither should be allowed to mutate
  // state. See JoinAckPayload's doc comment for the companion token check
  // callers should also apply before this — this refusal holds regardless
  // of whether that check even ran.
  bool on_join_ack(const uint8_t gateway_mac[6], uint8_t channel) {
    if (paired_) return false;
    paired_ = true;
    memcpy(gateway_mac_, gateway_mac, 6);
    channel_ = channel;
    consecutive_failures_ = 0;
    return true;
  }

  // Feed the result of every unicast send attempt. Returns true exactly
  // when the failure count just crossed the threshold and the node should
  // wipe its channel assumption and re-broadcast JOIN_REQ — this *is* the
  // channel-change recovery mechanism (TRD.md §5.1 point 4, D-008); there
  // is no separate gateway-side trigger for it.
  bool on_send_result(bool mac_acked) {
    if (mac_acked) {
      consecutive_failures_ = 0;
      return false;
    }
    ++consecutive_failures_;
    if (consecutive_failures_ >= PULSELINK_MAX_UNICAST_FAILURES) {
      paired_ = false;
      consecutive_failures_ = 0;
      return true;
    }
    return false;
  }

  // Fixed 8-byte layout for NVS persistence: paired(1) + gateway_mac(6) +
  // channel(1). Real flash I/O is a Phase 4, ESP32-only concern; this is
  // just the byte layout, testable on host by round-tripping through a
  // buffer to simulate a reboot.
  void serialize(uint8_t out[kSerializedSize]) const {
    out[0] = paired_ ? 1 : 0;
    memcpy(out + 1, gateway_mac_, 6);
    out[7] = channel_;
  }

  void deserialize(const uint8_t in[kSerializedSize]) {
    paired_ = in[0] != 0;
    memcpy(gateway_mac_, in + 1, 6);
    channel_ = in[7];
    consecutive_failures_ = 0;
  }

 private:
  bool paired_;
  uint8_t gateway_mac_[6];
  uint8_t channel_;
  uint8_t consecutive_failures_;
};

// Gateway-side per-MAC join rate limiter (TRD.md §7 failure mode 3: a
// crash-looping node spamming JOIN_REQ). Attempts beyond the window's
// budget are silently dropped — no NACK, because silence already means
// "retry" in this protocol; the limiter just declines to answer as fast.
class JoinRateLimiter {
 public:
  JoinRateLimiter() {
    for (int i = 0; i < PULSELINK_MAX_NODES; ++i) entries_[i].used = false;
  }

  bool allow(const uint8_t mac[6], uint32_t now_ticks) {
    Entry* e = find(mac);
    if (!e) e = claim_slot(mac, now_ticks);

    if (now_ticks - e->window_start_ticks > PULSELINK_JOIN_RATE_WINDOW_TICKS) {
      e->window_start_ticks = now_ticks;
      e->attempts_in_window = 0;
    }
    ++e->attempts_in_window;
    e->last_seen_ticks = now_ticks;
    return e->attempts_in_window <= PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW;
  }

 private:
  struct Entry {
    bool used;
    uint8_t mac[6];
    uint32_t window_start_ticks;
    uint16_t attempts_in_window;
    uint32_t last_seen_ticks;
  };

  Entry* find(const uint8_t mac[6]) {
    for (int i = 0; i < PULSELINK_MAX_NODES; ++i) {
      if (entries_[i].used && mac_equal(entries_[i].mac, mac)) {
        return &entries_[i];
      }
    }
    return nullptr;
  }

  // Evicts the least-recently-seen entry when every slot is taken —
  // acceptable here because a full table of distinct join-spammers is
  // itself the failure mode this limiter exists to contain.
  Entry* claim_slot(const uint8_t mac[6], uint32_t now_ticks) {
    int victim = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < PULSELINK_MAX_NODES; ++i) {
      if (!entries_[i].used) {
        victim = i;
        break;
      }
      if (entries_[i].last_seen_ticks < oldest) {
        oldest = entries_[i].last_seen_ticks;
        victim = i;
      }
    }
    entries_[victim].used = true;
    memcpy(entries_[victim].mac, mac, 6);
    entries_[victim].window_start_ticks = now_ticks;
    entries_[victim].attempts_in_window = 0;
    entries_[victim].last_seen_ticks = now_ticks;
    return &entries_[victim];
  }

  Entry entries_[PULSELINK_MAX_NODES];
};

// Ties token check + rate limiting + registry together into the gateway's
// side of the join flow. One instance per gateway.
class GatewayJoinHandler {
 public:
  GatewayJoinHandler(const uint8_t expected_token[PULSELINK_PROVISIONING_TOKEN_SIZE],
                      Registry* registry, JoinRateLimiter* rate_limiter)
      : registry_(registry), rate_limiter_(rate_limiter) {
    memcpy(expected_token_, expected_token, PULSELINK_PROVISIONING_TOKEN_SIZE);
  }

  // On success, fills *out_ack and returns true — caller sends a JOIN_ACK.
  // Returns false when the request should be silently ignored: rate
  // limited, wrong token (foreign/malicious join attempt), or the
  // registry is full.
  bool handle(const uint8_t sender_mac[6], const JoinRequestPayload& req,
              uint8_t gateway_channel, uint32_t now_ticks,
              JoinAckPayload* out_ack) {
    if (!rate_limiter_->allow(sender_mac, now_ticks)) return false;
    if (!token_matches(req.token)) return false;

    int device_id =
        registry_->add_or_update(sender_mac, req.sleep_profile, now_ticks);
    if (device_id < 0) return false;

    out_ack->device_id = static_cast<uint8_t>(device_id);
    out_ack->channel = gateway_channel;
    memcpy(out_ack->token, expected_token_, PULSELINK_PROVISIONING_TOKEN_SIZE);
    return true;
  }

 private:
  bool token_matches(const uint8_t token[PULSELINK_PROVISIONING_TOKEN_SIZE]) const {
    for (uint8_t i = 0; i < PULSELINK_PROVISIONING_TOKEN_SIZE; ++i) {
      if (token[i] != expected_token_[i]) return false;
    }
    return true;
  }

  uint8_t expected_token_[PULSELINK_PROVISIONING_TOKEN_SIZE];
  Registry* registry_;
  JoinRateLimiter* rate_limiter_;
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_JOIN_H
