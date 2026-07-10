// PulseLink Wokwi demo — gateway + node combined on ONE simulated ESP32.
//
// Wokwi (at least on the free/browser tier) doesn't support running
// different firmware on multiple boards in one project, which rules out
// simulating gateway and node as separate real-ESP-NOW chips. This sketch
// takes a different, still-genuine path: it runs BOTH roles' real logic
// in one process, connected by transport/fake/pl_fake_transport.h's
// FakeMedium/FakeTransport — the exact same in-process fake ESP-NOW medium
// 127 host-native tests already exercise (test/test_gateway_bridge.cpp,
// test_join_flow.cpp, test_cmd_flow.cpp, etc.) — instead of a real ESP-NOW
// radio.
//
// This works cleanly because Transport is an interface (core/../pl_transport.h):
// gateway.ino and node.ino's logic only ever call send_unicast() /
// send_broadcast() / receive() / local_mac() on whatever Transport they're
// handed. Swapping EspNowTransport for FakeTransport is a type change at
// the point of construction, not a logic change — everything below is the
// same join/uplink/downlink/HSM/health-metrics code as the real
// gateway.ino + node.ino, just talking to each other in-process instead
// of over a radio.
//
// What this DOES prove that host-native tests can't: real WiFi connection
// behavior, a real MQTT session over real TCP/IP, real millis()-based
// timing, and that the whole codebase actually compiles and runs against
// a real (simulated) ESP32 toolchain — not just my g++ stub-header syntax
// check. What it does NOT prove: anything about the real ESP-NOW radio
// (channel behavior, real MAC-layer ack timing, range) — that's
// EspNowTransport's job, and it's untested here on purpose. See
// gateway/README.md and node/README.md for the equivalent real firmware,
// and this project's README.md for why this shape.

#include <cstdio>
#include <cstring>

#include <PubSubClient.h>
#include <Preferences.h>
#include <WiFi.h>

#include "pl_cmd_status.h"
#include "pl_cmdtable.h"
#include "pl_dedupe.h"
#include "pl_fields.h"
#include "pl_frame.h"
#include "pl_gateway_hsm.h"
#include "pl_join.h"
#include "pl_loss_tracker.h"
#include "pl_registry.h"
#include "pl_topics.h"
#include "pl_fake_transport.h"
#include "pl_nvs_registry_storage.h"
#include "pl_pubsubclient_mqtt.h"

// ---- Config ----
static const char* kWifiSsid = "Wokwi-GUEST";  // Wokwi's built-in open network
static const char* kWifiPassword = "";
static const char* kMqttHost =
    "test.mosquitto.org";  // public test broker — see README.md for why
static const int kMqttPort = 1883;
static const char kTenantId[] = "acme";
static const uint8_t kProvisioningToken[PULSELINK_PROVISIONING_TOKEN_SIZE] = {
    1, 2, 3, 4};
static const uint8_t kGatewayMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static const uint8_t kNodeMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t kFieldIdTemperatureC10 = 1;
static const uint8_t kFakeGatewayChannel = 6;  // no real radio channel here
static const uint32_t kHealthMetricsIntervalTicks = 30;  // seconds
static const uint32_t kMqttReconnectIntervalMs = 5000;
static const uint32_t kDataSendIntervalMs = 5000;
static const uint32_t kJoinRetryIntervalMs = 2000;
// ----------------

// The in-process "radio": both roles below register onto this instead of
// a real ESP-NOW driver.
pulselink::fake::FakeMedium g_medium;
pulselink::fake::FakeTransport g_gateway_link(&g_medium, kGatewayMac);
pulselink::fake::FakeTransport g_node_link(&g_medium, kNodeMac);

// --- Gateway-side state (same roles as gateway/gateway.ino) ---
Preferences g_prefs;
pulselink::gateway::NvsRegistryStorage g_registry_storage(&g_prefs);
pulselink::Registry g_registry(&g_registry_storage);
pulselink::JoinRateLimiter g_rate_limiter;
pulselink::GatewayJoinHandler g_join_handler(kProvisioningToken, &g_registry,
                                              &g_rate_limiter);
pulselink::CmdTable g_cmd_table;
pulselink::GatewayHsm g_hsm;
WiFiClient g_wifi_client;
PubSubClient g_pubsub(g_wifi_client);
pulselink::gateway::PubSubClientMqttClient g_mqtt(&g_pubsub);
pulselink::LastSeenSeq g_uplink_dedupe[PULSELINK_MAX_NODES];
pulselink::SeqLossTracker g_loss_tracker[PULSELINK_MAX_NODES];
uint16_t g_next_cmd_id = 0;

// --- Node-side state (same roles as node/node.ino) ---
pulselink::NodePairingState g_pairing;
pulselink::NodeCmdDedupe g_cmd_dedupe;
uint8_t g_node_seq = 0;

uint32_t now_ticks() { return millis() / 1000; }

uint16_t format_u32(uint32_t v, uint8_t* out, uint16_t cap) {
  char buf[12];
  int n = snprintf(buf, sizeof(buf), "%u", v);
  if (n <= 0 || static_cast<uint16_t>(n) > cap) return 0;
  memcpy(out, buf, n);
  return static_cast<uint16_t>(n);
}

uint16_t format_field_value(const pulselink::FieldValue& f, uint8_t* out,
                             uint16_t cap) {
  char buf[16];
  int n = 0;
  switch (f.type) {
    case pulselink::FieldType::kU8:
      n = snprintf(buf, sizeof(buf), "%u", f.value.u8);
      break;
    case pulselink::FieldType::kBool:
      n = snprintf(buf, sizeof(buf), "%d", f.value.b ? 1 : 0);
      break;
    case pulselink::FieldType::kU16:
      n = snprintf(buf, sizeof(buf), "%u", f.value.u16);
      break;
    case pulselink::FieldType::kI16:
      n = snprintf(buf, sizeof(buf), "%d", f.value.i16);
      break;
    case pulselink::FieldType::kU32:
      n = snprintf(buf, sizeof(buf), "%u", f.value.u32);
      break;
    case pulselink::FieldType::kI32:
      n = snprintf(buf, sizeof(buf), "%d", f.value.i32);
      break;
    case pulselink::FieldType::kF32:
      n = snprintf(buf, sizeof(buf), "%.2f", f.value.f32);
      break;
  }
  if (n <= 0 || static_cast<uint16_t>(n) > cap) return 0;
  memcpy(out, buf, n);
  return static_cast<uint16_t>(n);
}

// ============================== Gateway side ==============================

void gw_publish_online_event(uint8_t device_id, bool online) {
  char topic[PULSELINK_MAX_TOPIC_LEN];
  pulselink::build_data_topic(kTenantId, device_id, "online", topic,
                               sizeof(topic));
  uint8_t payload[1] = {online ? static_cast<uint8_t>('1')
                                : static_cast<uint8_t>('0')};
  g_hsm.publish_or_spool(&g_mqtt, topic, payload, 1);
}

void gw_resend_via_link(uint8_t device_id, const pulselink::CmdSlot& slot) {
  pulselink::RegistryEntry* node = g_registry.find_by_device_id(device_id);
  if (!node) return;

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kCmd, /*seq=*/0, slot.cmd_id,
                           slot.payload, slot.payload_len, frame, &frame_len);
  g_gateway_link.send_unicast(node->mac, frame, frame_len);
}

void gw_publish_cmd_status_if_failed(uint8_t device_id) {
  pulselink::CmdSlot* slot = g_cmd_table.find(device_id);
  if (!slot || slot->state != pulselink::CmdState::kFailed) return;

  char topic[PULSELINK_MAX_TOPIC_LEN];
  pulselink::build_cmd_status_topic(kTenantId, device_id, topic,
                                     sizeof(topic));
  uint8_t status[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t len = pulselink::format_cmd_status(
      /*delivered=*/false, pulselink::CmdResult::kOk, status, sizeof(status));
  g_hsm.publish_or_spool(&g_mqtt, topic, status, len);
  g_cmd_table.clear_if_finished(device_id);
}

void gw_handle_join_req(const uint8_t src[6], const uint8_t* payload,
                         uint8_t payload_len) {
  pulselink::JoinRequestPayload req;
  if (!pulselink::decode_join_request(payload, payload_len, &req)) return;

  pulselink::JoinAckPayload ack;
  if (!g_join_handler.handle(src, req, kFakeGatewayChannel, now_ticks(),
                              &ack)) {
    return;  // bad token, rate-limited, or registry full — stay silent
  }

  uint8_t ack_payload[2];
  uint8_t ack_payload_len = pulselink::encode_join_ack(ack, ack_payload);
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + 2];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kJoinAck, 0, 0, ack_payload,
                           ack_payload_len, frame, &frame_len);
  g_gateway_link.send_unicast(src, frame, frame_len);

  g_registry.set_field_name(ack.device_id, kFieldIdTemperatureC10,
                             "temperature");
  Serial.printf("gateway: joined device_id=%u\n", ack.device_id);
}

void gw_handle_data(const uint8_t src[6], const pulselink::FrameHeader& header,
                     const uint8_t* payload, uint8_t payload_len) {
  pulselink::RegistryEntry* node = g_registry.find_by_mac(src);
  if (!node) return;  // not joined — ignore

  if (g_uplink_dedupe[node->device_id].is_duplicate(header.seq)) return;
  g_uplink_dedupe[node->device_id].accept(header.seq);
  g_loss_tracker[node->device_id].record(header.seq);

  if (g_registry.touch(src, now_ticks())) {
    gw_publish_online_event(node->device_id, /*online=*/true);
  }

  pulselink::FieldReader fields(payload, payload_len);
  pulselink::FieldValue field;
  while (fields.next(&field)) {
    const char* field_name =
        g_registry.find_field_name(node->device_id, field.field_id);
    if (!field_name) continue;

    char topic[PULSELINK_MAX_TOPIC_LEN];
    pulselink::build_data_topic(kTenantId, node->device_id, field_name, topic,
                                 sizeof(topic));
    uint8_t text[PULSELINK_MAX_MQTT_PAYLOAD];
    uint16_t text_len = format_field_value(field, text, sizeof(text));
    if (text_len == 0) continue;

    Serial.printf("gateway: publishing %s\n", topic);
    g_hsm.publish_or_spool(&g_mqtt, topic, text, text_len);
  }

  if (node->sleep_profile == pulselink::SleepProfile::kWakeAndPoll) {
    pulselink::CmdSlot* slot = g_cmd_table.find(node->device_id);
    if (slot) {
      if (g_cmd_table.try_deliver(node->device_id, now_ticks())) {
        gw_resend_via_link(node->device_id, *slot);
      } else {
        gw_publish_cmd_status_if_failed(node->device_id);
      }
    }
  }
}

void gw_handle_cmd_ack(const uint8_t src[6],
                        const pulselink::FrameHeader& header,
                        const uint8_t* payload, uint8_t payload_len) {
  pulselink::RegistryEntry* node = g_registry.find_by_mac(src);
  if (!node) return;

  pulselink::CmdResult result;
  if (!pulselink::decode_cmd_ack(payload, payload_len, &result)) return;
  if (!g_cmd_table.on_ack(node->device_id, header.cmd_id)) return;

  char topic[PULSELINK_MAX_TOPIC_LEN];
  pulselink::build_cmd_status_topic(kTenantId, node->device_id, topic,
                                     sizeof(topic));
  uint8_t status[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t len = pulselink::format_cmd_status(/*delivered=*/true, result,
                                               status, sizeof(status));
  g_hsm.publish_or_spool(&g_mqtt, topic, status, len);
  g_cmd_table.clear_if_finished(node->device_id);
}

void gw_service_uplink() {
  uint8_t src[6];
  uint8_t buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t len;

  while (g_gateway_link.receive(src, buf, &len)) {
    pulselink::FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    if (pulselink::decode_frame(buf, len, &header, &payload, &payload_len) !=
        pulselink::FrameError::kOk) {
      continue;
    }

    switch (header.msg_type) {
      case pulselink::MsgType::kJoinReq:
        gw_handle_join_req(src, payload, payload_len);
        break;
      case pulselink::MsgType::kData:
        gw_handle_data(src, header, payload, payload_len);
        break;
      case pulselink::MsgType::kCmdAck:
        gw_handle_cmd_ack(src, header, payload, payload_len);
        break;
      default:
        break;
    }
  }
}

void gw_service_cmd_table_retries() {
  for (uint8_t device_id = 0; device_id < PULSELINK_MAX_NODES; ++device_id) {
    pulselink::RegistryEntry* node = g_registry.find_by_device_id(device_id);
    if (!node || node->sleep_profile != pulselink::SleepProfile::kAlwaysOn) {
      continue;
    }
    pulselink::CmdSlot* slot = g_cmd_table.find(device_id);
    if (!slot) continue;

    if (g_cmd_table.poll(device_id, now_ticks())) {
      gw_resend_via_link(device_id, *slot);
    } else {
      gw_publish_cmd_status_if_failed(device_id);
    }
  }
}

void gw_service_mqtt_downlink() {
  char topic[PULSELINK_MAX_TOPIC_LEN];
  uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t payload_len;

  while (g_mqtt.receive(topic, sizeof(topic), payload, sizeof(payload),
                         &payload_len)) {
    int device_id = pulselink::parse_cmd_topic_device_id(kTenantId, topic);
    if (device_id < 0) continue;

    pulselink::RegistryEntry* node =
        g_registry.find_by_device_id(static_cast<uint8_t>(device_id));
    if (!node) continue;

    pulselink::DownlinkRefuseReason reason;
    if (!g_hsm.accept_downlink(&reason)) continue;

    uint16_t cmd_id = g_next_cmd_id++;
    if (!g_cmd_table.enqueue(static_cast<uint8_t>(device_id), cmd_id, payload,
                              static_cast<uint8_t>(payload_len),
                              now_ticks())) {
      continue;
    }

    if (node->sleep_profile == pulselink::SleepProfile::kAlwaysOn) {
      pulselink::CmdSlot* slot =
          g_cmd_table.find(static_cast<uint8_t>(device_id));
      if (slot && g_cmd_table.try_deliver(static_cast<uint8_t>(device_id),
                                           now_ticks())) {
        gw_resend_via_link(static_cast<uint8_t>(device_id), *slot);
      }
    }
  }
}

void gw_service_health_metrics() {
  static uint32_t s_last_run = 0;
  uint32_t now = now_ticks();
  if (now - s_last_run < kHealthMetricsIntervalTicks) return;
  s_last_run = now;

  for (uint8_t device_id = 0; device_id < PULSELINK_MAX_NODES; ++device_id) {
    pulselink::RegistryEntry* node = g_registry.find_by_device_id(device_id);
    if (!node) continue;

    if (g_registry.check_offline_transition(device_id, now)) {
      gw_publish_online_event(device_id, /*online=*/false);
    }

    char topic[PULSELINK_MAX_TOPIC_LEN];
    pulselink::build_data_topic(kTenantId, device_id, "loss_rate", topic,
                                 sizeof(topic));
    uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
    uint16_t len = format_u32(g_loss_tracker[device_id].loss_rate_percent(),
                               payload, sizeof(payload));
    g_hsm.publish_or_spool(&g_mqtt, topic, payload, len);
  }

  char gw_topic[PULSELINK_MAX_TOPIC_LEN];
  pulselink::build_gateway_topic(kTenantId, "ring_overflow", gw_topic,
                                  sizeof(gw_topic));
  uint8_t gw_payload[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t gw_len = format_u32(g_gateway_link.rx_overflow_count(), gw_payload,
                                sizeof(gw_payload));
  g_hsm.publish_or_spool(&g_mqtt, gw_topic, gw_payload, gw_len);
}

void gw_connect_mqtt() {
  if (!g_pubsub.connect("pulselink-gateway-wokwi")) return;

  g_hsm.on_backhaul_up();
  char wildcard[PULSELINK_MAX_TOPIC_LEN];
  snprintf(wildcard, sizeof(wildcard), "pulsecore/%s/+/cmd", kTenantId);
  g_mqtt.subscribe(wildcard);
  Serial.println("gateway: MQTT connected");
}

// =============================== Node side =================================

void node_save_pairing() {
  uint8_t bytes[pulselink::NodePairingState::kSerializedSize];
  g_pairing.serialize(bytes);
  g_prefs.putBytes("pairing", bytes, sizeof(bytes));
}

bool node_load_pairing() {
  uint8_t bytes[pulselink::NodePairingState::kSerializedSize];
  size_t got = g_prefs.getBytes("pairing", bytes, sizeof(bytes));
  if (got != sizeof(bytes)) return false;
  g_pairing.deserialize(bytes);
  return g_pairing.paired();
}

void node_broadcast_join_request() {
  pulselink::JoinRequestPayload req;
  memcpy(req.token, kProvisioningToken, sizeof(kProvisioningToken));
  req.sleep_profile = pulselink::SleepProfile::kAlwaysOn;

  uint8_t payload[16];
  uint8_t payload_len = pulselink::encode_join_request(req, payload);
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + 16];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kJoinReq, 0, 0, payload,
                           payload_len, frame, &frame_len);
  g_node_link.send_broadcast(frame, frame_len);
  Serial.println("node: broadcasting JOIN_REQ");
}

int16_t node_read_fake_temperature_c10() {
  return static_cast<int16_t>(200 + (millis() / 1000) % 100);  // 20.0-29.9C
}

void node_send_data() {
  uint8_t payload[PULSELINK_MAX_FRAME_PAYLOAD];
  pulselink::FieldWriter fields(payload, sizeof(payload));
  fields.write_i16(kFieldIdTemperatureC10, node_read_fake_temperature_c10());

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kData, g_node_seq++,
                           /*cmd_id=*/0, payload, fields.length(), frame,
                           &frame_len);

  bool acked =
      g_node_link.send_unicast(g_pairing.gateway_mac(), frame, frame_len);
  if (g_pairing.on_send_result(acked)) {
    Serial.println("node: channel assumption invalidated, re-discovering");
  }
}

void node_handle_join_ack(const uint8_t src[6], const uint8_t* payload,
                           uint8_t payload_len) {
  pulselink::JoinAckPayload ack;
  if (!pulselink::decode_join_ack(payload, payload_len, &ack)) return;
  g_pairing.on_join_ack(src, ack.channel);
  node_save_pairing();
  Serial.printf("node: paired: device_id=%u channel=%u\n", ack.device_id,
                ack.channel);
}

void node_handle_cmd(const uint8_t src[6], const pulselink::FrameHeader& header,
                      const uint8_t* payload, uint8_t payload_len) {
  (void)payload;
  (void)payload_len;

  pulselink::CmdResult result;
  if (g_cmd_dedupe.already_executed(header.cmd_id)) {
    result = g_cmd_dedupe.last_result();
  } else {
    result = pulselink::CmdResult::kOk;
    Serial.printf("node: executing cmd_id=%u\n", header.cmd_id);
    g_cmd_dedupe.record_execution(header.cmd_id, result);
  }

  uint8_t ack_payload[1];
  uint8_t ack_payload_len = pulselink::encode_cmd_ack(result, ack_payload);
  uint8_t ack_frame[PULSELINK_FRAME_HEADER_SIZE + 1];
  uint8_t ack_frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kCmdAck, 0, header.cmd_id,
                           ack_payload, ack_payload_len, ack_frame,
                           &ack_frame_len);
  g_node_link.send_unicast(src, ack_frame, ack_frame_len);
}

void node_service_link() {
  uint8_t src[6];
  uint8_t buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t len;

  while (g_node_link.receive(src, buf, &len)) {
    pulselink::FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    if (pulselink::decode_frame(buf, len, &header, &payload, &payload_len) !=
        pulselink::FrameError::kOk) {
      continue;
    }

    switch (header.msg_type) {
      case pulselink::MsgType::kJoinAck:
        node_handle_join_ack(src, payload, payload_len);
        break;
      case pulselink::MsgType::kCmd:
        node_handle_cmd(src, header, payload, payload_len);
        break;
      default:
        break;
    }
  }
}

// ================================ Arduino ==================================

void setup() {
  Serial.begin(115200);

  g_prefs.begin("pulselink", /*readOnly=*/false);
  g_registry.reload();  // NVS wasn't ready when g_registry's constructor
                         // ran (global init order) — see core/pl_registry.h

  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);
  while (WiFi.status() != WL_CONNECTED) delay(200);
  Serial.println("WiFi connected");

  g_pubsub.setServer(kMqttHost, kMqttPort);
  gw_connect_mqtt();

  if (node_load_pairing()) {
    Serial.printf("node: restored pairing, channel=%u\n", g_pairing.channel());
  } else {
    node_broadcast_join_request();
  }
}

void loop() {
  gw_service_uplink();
  gw_service_cmd_table_retries();

  if (!g_pubsub.connected()) {
    g_hsm.on_backhaul_down();
    static uint32_t s_last_reconnect_attempt = 0;
    uint32_t now_ms = millis();
    if (now_ms - s_last_reconnect_attempt > kMqttReconnectIntervalMs) {
      s_last_reconnect_attempt = now_ms;
      gw_connect_mqtt();
    }
  } else {
    g_pubsub.loop();
    gw_service_mqtt_downlink();
    g_hsm.drain_step(&g_mqtt);
  }
  gw_service_health_metrics();

  node_service_link();
  if (!g_pairing.paired()) {
    static uint32_t s_last_broadcast = 0;
    uint32_t now = millis();
    if (now - s_last_broadcast > kJoinRetryIntervalMs) {
      s_last_broadcast = now;
      node_broadcast_join_request();
    }
  } else {
    static uint32_t s_last_send = 0;
    uint32_t now = millis();
    if (now - s_last_send > kDataSendIntervalMs) {
      s_last_send = now;
      node_send_data();
    }
  }
}
