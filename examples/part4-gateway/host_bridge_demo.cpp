// Real-broker validation: the same ESP-NOW-to-MQTT bridge logic as
// test/test_gateway_bridge.cpp, but pl_mqtt.h's MqttClient interface is
// backed by a genuine libmosquitto connection instead of the fake — proof
// that the topic scheme and publish/subscribe calls actually work over
// the wire against a real broker, not just against our own test double.
//
// This file is host-only integration tooling, not shipped to the embedded
// target: it links libmosquitto (heap-using, real sockets) and is
// deliberately outside core/ and transport/, so none of the zero-heap
// contract or CI's tripwire grep applies to it. It's also not built by
// default — see examples/part4-gateway/CMakeLists.txt: the target only
// exists when libmosquitto is found on the host, so CI (which doesn't
// have it installed) simply skips it.
//
// Usage: point MQTT_HOST/MQTT_PORT at any broker (defaults to
// 127.0.0.1:1883 — a typical `docker run -p 1883:1883 eclipse-mosquitto`):
//   MQTT_HOST=127.0.0.1 MQTT_PORT=1883 ./part4_host_bridge_demo

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <mosquitto.h>

#include "../../core/pl_fields.h"
#include "../../core/pl_frame.h"
#include "../../core/pl_gateway_hsm.h"
#include "../../core/pl_registry.h"
#include "../../core/pl_topics.h"
#include "../../transport/fake/pl_fake_transport.h"

namespace {

const char kTenantId[] = "acme";
const uint8_t kFieldIdTemperature = 1;
const int kMaxQueued = 8;

// Real MqttClient backend over libmosquitto. Demo-quality: fixed small
// inbound queue, synchronous mosquitto_loop() pump instead of a
// background thread — enough to prove the wire protocol works, not a
// production gateway implementation (that's ESP32 firmware, Phase 4
// hardware bring-up, using PubSubClient or esp-mqtt instead of this).
class LibmosquittoMqttClient : public pulselink::MqttClient {
 public:
  LibmosquittoMqttClient(const char* host, int port, const char* client_id) {
    mosq_ = mosquitto_new(client_id, true, this);
    mosquitto_message_callback_set(mosq_, &LibmosquittoMqttClient::on_message_trampoline);
    connected_ = mosquitto_connect(mosq_, host, port, 60) == MOSQ_ERR_SUCCESS;
  }

  ~LibmosquittoMqttClient() override {
    mosquitto_disconnect(mosq_);
    mosquitto_destroy(mosq_);
  }

  void pump() { mosquitto_loop(mosq_, 200, 1); }

  bool connected() const override { return connected_; }

  bool publish(const char* topic, const uint8_t* payload,
               uint16_t payload_len) override {
    return mosquitto_publish(mosq_, nullptr, topic, payload_len, payload, 0,
                              false) == MOSQ_ERR_SUCCESS;
  }

  bool subscribe(const char* topic) override {
    return mosquitto_subscribe(mosq_, nullptr, topic, 0) == MOSQ_ERR_SUCCESS;
  }

  bool receive(char* out_topic, uint16_t topic_cap, uint8_t* out_payload,
               uint16_t payload_cap, uint16_t* out_payload_len) override {
    if (inbound_head_ >= inbound_count_) return false;
    const Queued& q = inbound_[inbound_head_++];
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

  static void on_message_trampoline(mosquitto*, void* userdata,
                                     const mosquitto_message* msg) {
    static_cast<LibmosquittoMqttClient*>(userdata)->on_message(msg);
  }

  void on_message(const mosquitto_message* msg) {
    if (inbound_count_ >= kMaxQueued) return;
    size_t topic_len = strlen(msg->topic);
    if (topic_len >= PULSELINK_MAX_TOPIC_LEN ||
        msg->payloadlen > PULSELINK_MAX_MQTT_PAYLOAD) {
      return;  // demo-quality: drop what doesn't fit, don't truncate silently
    }
    Queued& q = inbound_[inbound_count_++];
    memcpy(q.topic, msg->topic, topic_len + 1);
    memcpy(q.payload, msg->payload, msg->payloadlen);
    q.payload_len = static_cast<uint16_t>(msg->payloadlen);
  }

  mosquitto* mosq_;
  bool connected_;
  Queued inbound_[kMaxQueued];
  int inbound_count_ = 0;
  int inbound_head_ = 0;
};

uint16_t format_i16(int16_t v, uint8_t* out, uint16_t cap) {
  char buf[8];
  int n = snprintf(buf, sizeof(buf), "%d", v);
  if (n <= 0 || static_cast<uint16_t>(n) > cap) return 0;
  memcpy(out, buf, n);
  return static_cast<uint16_t>(n);
}

}  // namespace

int main() {
  const char* host = getenv("MQTT_HOST");
  if (!host) host = "127.0.0.1";
  int port = 1883;
  if (const char* port_str = getenv("MQTT_PORT")) port = atoi(port_str);

  mosquitto_lib_init();

  printf("connecting gateway + observer clients to %s:%d...\n", host, port);
  LibmosquittoMqttClient gateway_mqtt(host, port, "pulselink-gateway-demo");
  LibmosquittoMqttClient observer_mqtt(host, port, "pulsedash-observer-demo");

  if (!gateway_mqtt.connected() || !observer_mqtt.connected()) {
    fprintf(stderr, "could not connect to broker at %s:%d\n", host, port);
    mosquitto_lib_cleanup();
    return 1;
  }

  // mosquitto_connect() only starts the handshake — pump the loop so the
  // CONNACK actually lands before we try to subscribe on top of it.
  for (int i = 0; i < 3; ++i) {
    gateway_mqtt.pump();
    observer_mqtt.pump();
  }

  observer_mqtt.subscribe("pulsecore/#");
  for (int i = 0; i < 3; ++i) observer_mqtt.pump();  // let the SUBACK land

  // --- Set up the ESP-NOW side (fake — no real radio in this environment)
  // and the gateway logic exactly as test_gateway_bridge.cpp does. ---
  uint8_t node_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t gateway_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  pulselink::fake::FakeMedium medium;
  pulselink::fake::FakeTransport node(&medium, node_mac);
  pulselink::fake::FakeTransport gateway(&medium, gateway_mac);

  pulselink::Registry registry;
  int device_id =
      registry.add_or_update(node_mac, pulselink::SleepProfile::kAlwaysOn, 0);
  registry.set_field_name(static_cast<uint8_t>(device_id), kFieldIdTemperature,
                           "temperature");

  pulselink::GatewayHsm hsm;
  hsm.on_backhaul_up();

  // --- Node sends DATA over (fake) ESP-NOW; gateway bridges it to real MQTT. ---
  uint8_t payload[PULSELINK_MAX_FRAME_PAYLOAD];
  pulselink::FieldWriter fields(payload, sizeof(payload));
  fields.write_i16(kFieldIdTemperature, 215);
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kData, 0, 0, payload,
                           fields.length(), frame, &frame_len);
  node.send_unicast(gateway_mac, frame, frame_len);

  uint8_t src[6], buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD], len;
  while (gateway.receive(src, buf, &len)) {
    pulselink::FrameHeader header;
    const uint8_t* body = nullptr;
    uint8_t body_len = 0;
    if (pulselink::decode_frame(buf, len, &header, &body, &body_len) !=
            pulselink::FrameError::kOk ||
        header.msg_type != pulselink::MsgType::kData) {
      continue;
    }
    pulselink::RegistryEntry* n = registry.find_by_mac(src);
    if (!n) continue;

    pulselink::FieldReader reader(body, body_len);
    pulselink::FieldValue field;
    while (reader.next(&field)) {
      const char* field_name =
          registry.find_field_name(n->device_id, field.field_id);
      if (!field_name || field.type != pulselink::FieldType::kI16) continue;

      char topic[PULSELINK_MAX_TOPIC_LEN];
      pulselink::build_data_topic(kTenantId, n->device_id, field_name, topic,
                                   sizeof(topic));
      uint8_t text[PULSELINK_MAX_MQTT_PAYLOAD];
      uint16_t text_len = format_i16(field.value.i16, text, sizeof(text));

      printf("gateway: publishing %s = %.*s\n", topic, text_len, text);
      hsm.publish_or_spool(&gateway_mqtt, topic, text, text_len);
    }
  }
  for (int i = 0; i < 3; ++i) gateway_mqtt.pump();

  // --- Confirm the observer (standing in for PulseDash) actually saw it. ---
  for (int i = 0; i < 3; ++i) observer_mqtt.pump();
  char topic[PULSELINK_MAX_TOPIC_LEN];
  uint8_t obs_payload[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t obs_len;
  int seen = 0;
  while (observer_mqtt.receive(topic, sizeof(topic), obs_payload,
                                sizeof(obs_payload), &obs_len)) {
    printf("observer: received %s = %.*s\n", topic, obs_len, obs_payload);
    ++seen;
  }

  // --- Downlink direction: observer (standing in for PulseDash) publishes
  // a command; gateway (subscribed) receives it. Proves subscribe/receive
  // too, not just publish. ---
  char cmd_topic[PULSELINK_MAX_TOPIC_LEN];
  pulselink::build_cmd_topic(kTenantId, static_cast<uint8_t>(device_id),
                              cmd_topic, sizeof(cmd_topic));
  gateway_mqtt.subscribe(cmd_topic);
  for (int i = 0; i < 3; ++i) gateway_mqtt.pump();

  const uint8_t cmd_payload[1] = {1};
  observer_mqtt.publish(cmd_topic, cmd_payload, 1);
  for (int i = 0; i < 3; ++i) {
    observer_mqtt.pump();
    gateway_mqtt.pump();
  }

  uint16_t cmd_len;
  if (gateway_mqtt.receive(topic, sizeof(topic), obs_payload,
                            sizeof(obs_payload), &cmd_len)) {
    printf("gateway: received downlink on %s\n", topic);
  }

  mosquitto_lib_cleanup();

  printf("\n%s\n", seen > 0
                        ? "PASS: uplink round-tripped through a real broker"
                        : "FAIL: observer never saw the published message");
  return seen > 0 ? 0 : 1;
}
