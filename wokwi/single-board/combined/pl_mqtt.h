#ifndef PULSELINK_CORE_PL_MQTT_H
#define PULSELINK_CORE_PL_MQTT_H

#include <cstdint>

#include "pl_config.h"

// MQTT client interface: the gateway's only, and only the gateway's — no
// node ever speaks MQTT (TRD.md §1). Implemented by transport/fake/
// (host tests) and, on hardware, a real client (PubSubClient or
// ESP-IDF's esp-mqtt); see gateway/ once that firmware lands.
//
// Same zero-heap-friendly shape as Transport: pointers + lengths, no
// std::string, fixed caller-owned buffers on the receive side.

namespace pulselink {

class MqttClient {
 public:
  virtual ~MqttClient() = default;

  virtual bool connected() const = 0;

  virtual bool publish(const char* topic, const uint8_t* payload,
                        uint16_t payload_len) = 0;

  virtual bool subscribe(const char* topic) = 0;

  // Non-blocking poll of the subscribed-message queue. Returns false when
  // empty. out_topic/out_payload are caller-owned buffers of the given
  // capacities; a message that doesn't fit is dropped rather than
  // truncated silently — callers should size buffers generously
  // (PULSELINK_MAX_TOPIC_LEN / PULSELINK_MAX_MQTT_PAYLOAD are the gateway's
  // own bounds).
  virtual bool receive(char* out_topic, uint16_t topic_cap,
                        uint8_t* out_payload, uint16_t payload_cap,
                        uint16_t* out_payload_len) = 0;
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_MQTT_H
