#ifndef PULSELINK_TRANSPORT_FAKE_PL_FAKE_MQTT_H
#define PULSELINK_TRANSPORT_FAKE_PL_FAKE_MQTT_H

#include <cstdint>
#include <cstring>

#include "../../core/pl_config.h"
#include "../../core/pl_mqtt.h"

// In-process fake MQTT client: no network, no broker — publishes land in a
// fixed-capacity log tests can inspect, and tests can inject "arrived"
// messages for receive() to hand back (simulating a subscribed topic
// getting a message). Same role for gateway/HSM tests that
// transport/fake/pl_fake_transport.h plays for ESP-NOW tests.
//
// Zero-heap like everything else under transport/ (CLAUDE.md's tripwire
// covers this file too).

namespace pulselink {
namespace fake {

class FakeMqttClient : public MqttClient {
 public:
  static const int kMaxLoggedPublishes = 32;
  static const int kMaxQueuedInbound = 8;

  struct Published {
    char topic[PULSELINK_MAX_TOPIC_LEN];
    uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
    uint16_t payload_len;
  };

  FakeMqttClient()
      : connected_(true),
        publish_count_(0),
        inbound_head_(0),
        inbound_tail_(0),
        inbound_count_(0) {}

  void set_connected(bool connected) { connected_ = connected; }

  bool connected() const override { return connected_; }

  bool publish(const char* topic, const uint8_t* payload,
               uint16_t payload_len) override {
    if (!connected_) return false;
    if (publish_count_ >= kMaxLoggedPublishes) return false;
    if (payload_len > PULSELINK_MAX_MQTT_PAYLOAD) return false;

    Published& p = log_[publish_count_++];
    strncpy(p.topic, topic, PULSELINK_MAX_TOPIC_LEN - 1);
    p.topic[PULSELINK_MAX_TOPIC_LEN - 1] = '\0';
    memcpy(p.payload, payload, payload_len);
    p.payload_len = payload_len;
    return true;
  }

  bool subscribe(const char* /*topic*/) override { return connected_; }

  bool receive(char* out_topic, uint16_t topic_cap, uint8_t* out_payload,
               uint16_t payload_cap, uint16_t* out_payload_len) override {
    if (inbound_count_ == 0) return false;
    const Published& p = inbound_[inbound_head_];
    inbound_head_ = (inbound_head_ + 1) % kMaxQueuedInbound;
    --inbound_count_;

    uint16_t topic_len = static_cast<uint16_t>(strlen(p.topic));
    if (topic_len >= topic_cap || p.payload_len > payload_cap) return false;

    memcpy(out_topic, p.topic, topic_len + 1);
    memcpy(out_payload, p.payload, p.payload_len);
    *out_payload_len = p.payload_len;
    return true;
  }

  // Test-only helper: queues a message as if it had arrived on a
  // subscribed topic, for receive() to hand back. Ring buffer, not a
  // flat array — a test (or gateway code exercising this over many
  // iterations) can inject and drain more than kMaxQueuedInbound
  // messages over its lifetime, not just kMaxQueuedInbound ever.
  void inject_inbound(const char* topic, const uint8_t* payload,
                       uint16_t payload_len) {
    if (inbound_count_ >= kMaxQueuedInbound) return;  // drop: queue full
    Published& p = inbound_[inbound_tail_];
    inbound_tail_ = (inbound_tail_ + 1) % kMaxQueuedInbound;
    ++inbound_count_;
    strncpy(p.topic, topic, PULSELINK_MAX_TOPIC_LEN - 1);
    p.topic[PULSELINK_MAX_TOPIC_LEN - 1] = '\0';
    memcpy(p.payload, payload, payload_len);
    p.payload_len = payload_len;
  }

  int publish_count() const { return publish_count_; }
  const Published& published(int i) const { return log_[i]; }

 private:
  bool connected_;
  Published log_[kMaxLoggedPublishes];
  int publish_count_;
  Published inbound_[kMaxQueuedInbound];
  int inbound_head_;
  int inbound_tail_;
  int inbound_count_;
};

}  // namespace fake
}  // namespace pulselink

#endif  // PULSELINK_TRANSPORT_FAKE_PL_FAKE_MQTT_H
