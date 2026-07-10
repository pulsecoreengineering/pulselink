#ifndef PULSELINK_CORE_PL_SPOOL_H
#define PULSELINK_CORE_PL_SPOOL_H

#include <cstdint>
#include <cstring>

#include "pl_config.h"

// Bounded uplink spool for when the MQTT backhaul is down (TRD.md §4.4
// Degraded superstate): fixed capacity, drop-oldest on overflow, overflow
// counter — same policy as the ESP-NOW ring buffer (pl_ring.h), applied to
// topic+payload pairs instead of raw frames.

namespace pulselink {

class MqttSpool {
 public:
  MqttSpool() : head_(0), tail_(0), count_(0), overflow_count_(0) {}

  bool push(const char* topic, const uint8_t* payload, uint16_t payload_len) {
    uint16_t topic_len = static_cast<uint16_t>(strlen(topic));
    if (topic_len >= PULSELINK_MAX_TOPIC_LEN ||
        payload_len > PULSELINK_MAX_MQTT_PAYLOAD) {
      return false;
    }

    Slot& slot = slots_[tail_];
    memcpy(slot.topic, topic, topic_len + 1);
    memcpy(slot.payload, payload, payload_len);
    slot.payload_len = payload_len;
    tail_ = (tail_ + 1) % PULSELINK_MAX_MQTT_SPOOL_DEPTH;

    if (count_ == PULSELINK_MAX_MQTT_SPOOL_DEPTH) {
      head_ = (head_ + 1) % PULSELINK_MAX_MQTT_SPOOL_DEPTH;
      ++overflow_count_;
      return false;
    }
    ++count_;
    return true;
  }

  bool pop(char* out_topic, uint8_t* out_payload, uint16_t* out_payload_len) {
    if (count_ == 0) return false;
    Slot& slot = slots_[head_];
    strcpy(out_topic, slot.topic);  // NOLINT: slot.topic is always NUL-terminated, <= PULSELINK_MAX_TOPIC_LEN
    memcpy(out_payload, slot.payload, slot.payload_len);
    *out_payload_len = slot.payload_len;
    head_ = (head_ + 1) % PULSELINK_MAX_MQTT_SPOOL_DEPTH;
    --count_;
    return true;
  }

  int size() const { return count_; }
  bool empty() const { return count_ == 0; }
  unsigned long overflow_count() const { return overflow_count_; }

 private:
  struct Slot {
    char topic[PULSELINK_MAX_TOPIC_LEN];
    uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
    uint16_t payload_len;
  };

  Slot slots_[PULSELINK_MAX_MQTT_SPOOL_DEPTH];
  int head_;
  int tail_;
  int count_;
  unsigned long overflow_count_;
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_SPOOL_H
