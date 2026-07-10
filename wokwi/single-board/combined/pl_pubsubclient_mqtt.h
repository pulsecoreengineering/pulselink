#ifndef PULSELINK_GATEWAY_PL_PUBSUBCLIENT_MQTT_H
#define PULSELINK_GATEWAY_PL_PUBSUBCLIENT_MQTT_H

// Real MqttClient backend over PubSubClient (the standard Arduino MQTT
// library). NOT compiled in this repo's CI — no ESP32 toolchain in the
// host-native test environment (see gateway/README.md).
//
// PubSubClient delivers inbound messages via a callback (setCallback in
// setup()), not a poll — this class's callback pushes into a small
// fixed-capacity ring buffer that receive() drains, so callers see the
// same poll-based MqttClient contract as transport/fake/pl_fake_mqtt.h
// (host tests) and
// examples/part4-gateway/host_bridge_demo.cpp's libmosquitto backend
// (real-broker validation). Same reason for the static self-pointer
// trampoline as transport/espnow/pl_espnow_transport.h: only one
// PubSubClientMqttClient should exist at a time.

#include <cstring>

#include <PubSubClient.h>

#include "pl_config.h"
#include "pl_mqtt.h"

namespace pulselink {
namespace gateway {

class PubSubClientMqttClient : public pulselink::MqttClient {
 public:
  static const int kMaxQueuedInbound = 8;

  explicit PubSubClientMqttClient(PubSubClient* client) : client_(client) {
    instance_ptr() = this;
    client_->setCallback(&PubSubClientMqttClient::on_message_trampoline);
  }

  bool connected() const override { return client_->connected(); }

  bool publish(const char* topic, const uint8_t* payload,
               uint16_t payload_len) override {
    return client_->publish(topic, payload, payload_len);
  }

  bool subscribe(const char* topic) override {
    return client_->subscribe(topic);
  }

  bool receive(char* out_topic, uint16_t topic_cap, uint8_t* out_payload,
               uint16_t payload_cap, uint16_t* out_payload_len) override {
    if (inbound_count_ == 0) return false;
    const Queued& q = inbound_[inbound_head_];
    inbound_head_ = (inbound_head_ + 1) % kMaxQueuedInbound;
    --inbound_count_;

    size_t topic_len = strlen(q.topic);
    if (topic_len >= topic_cap || q.payload_len > payload_cap) return false;

    memcpy(out_topic, q.topic, topic_len + 1);
    memcpy(out_payload, q.payload, q.payload_len);
    *out_payload_len = q.payload_len;
    return true;
  }

 private:
  struct Queued {
    char topic[PULSELINK_MAX_TOPIC_LEN];
    uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
    uint16_t payload_len;
  };

  static void on_message_trampoline(char* topic, uint8_t* payload,
                                     unsigned int length) {
    PubSubClientMqttClient* self = instance_ptr();
    if (!self) return;
    self->on_message(topic, payload, length);
  }

  void on_message(const char* topic, const uint8_t* payload,
                   unsigned int length) {
    if (inbound_count_ >= kMaxQueuedInbound) return;  // drop: queue full
    size_t topic_len = strlen(topic);
    if (topic_len >= PULSELINK_MAX_TOPIC_LEN ||
        length > PULSELINK_MAX_MQTT_PAYLOAD) {
      return;  // drop: doesn't fit — don't truncate silently
    }

    Queued& q = inbound_[inbound_tail_];
    inbound_tail_ = (inbound_tail_ + 1) % kMaxQueuedInbound;
    ++inbound_count_;
    memcpy(q.topic, topic, topic_len + 1);
    memcpy(q.payload, payload, length);
    q.payload_len = static_cast<uint16_t>(length);
  }

  static PubSubClientMqttClient*& instance_ptr() {
    static PubSubClientMqttClient* p = nullptr;
    return p;
  }

  PubSubClient* client_;
  Queued inbound_[kMaxQueuedInbound];
  int inbound_count_ = 0;
  int inbound_head_ = 0;
  int inbound_tail_ = 0;
};

}  // namespace gateway
}  // namespace pulselink

#endif  // PULSELINK_GATEWAY_PL_PUBSUBCLIENT_MQTT_H
