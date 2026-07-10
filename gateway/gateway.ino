// PulseLink gateway firmware — the Part 4 flagship artifact. Wires
// together everything built and host-tested across Phases 1-4: join
// handling, uplink dedupe/loss-tracking/field-bridging to MQTT, downlink
// command routing, the two-loop ack model, the WAKE_AND_POLL mailbox, the
// Connected/Degraded backhaul state machine, and health metrics.
//
// LogicFrenzy series, Part 4 — "Building the MQTT gateway."
//
// NOT compiled against the real ESP32 Arduino core in this repo's CI — no
// ESP32 toolchain in the host-native test environment (see README.md).
// Every piece of *logic* this file calls (registry, join handler, command
// table, dedupe, loss tracker, gateway HSM, topic builder/parser) is
// covered by the 125 host-native tests in test/ against transport/fake/ —
// this file is the orchestration wiring, not new protocol logic. The
// pieces that ARE new here (EspNowTransport, NvsRegistryStorage,
// PubSubClientMqttClient) are thin, mechanical adapters to real hardware
// APIs behind interfaces the tests already exercise.

#include <cstdio>
#include <cstring>

#include <PubSubClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../core/pl_cmd_status.h"
#include "../core/pl_cmdtable.h"
#include "../core/pl_dedupe.h"
#include "../core/pl_fields.h"
#include "../core/pl_frame.h"
#include "../core/pl_gateway_hsm.h"
#include "../core/pl_join.h"
#include "../core/pl_loss_tracker.h"
#include "../core/pl_registry.h"
#include "../core/pl_topics.h"
#include "../transport/espnow/pl_espnow_transport.h"
#include "pl_nvs_registry_storage.h"
#include "pl_pubsubclient_mqtt.h"

// ---- Deployment config — edit before flashing ----
static const char* kWifiSsid = "YOUR_WIFI_SSID";
static const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
static const char* kMqttHost = "192.168.1.10";
static const int kMqttPort = 1883;
static const char kTenantId[] = "acme";
static const uint8_t kProvisioningToken[PULSELINK_PROVISIONING_TOKEN_SIZE] = {
    1, 2, 3, 4};
// Field schema for this deployment (TRD.md §3.3, §4.3: "delivered at join
// or provisioned" — provisioned here, applied to every joining node).
// This tutorial's fleet is one node type (node/node.ino) sending one
// field; a fleet with multiple node types would provision per device
// type instead of the same map for every join.
static const uint8_t kFieldIdTemperatureC10 = 1;
static const uint32_t kHealthMetricsIntervalTicks = 30;  // seconds
static const uint32_t kMqttReconnectIntervalMs = 5000;
// ---------------------------------------------------

Preferences g_prefs;
pulselink::gateway::NvsRegistryStorage g_registry_storage(&g_prefs);
pulselink::Registry g_registry(&g_registry_storage);
pulselink::JoinRateLimiter g_rate_limiter;
pulselink::GatewayJoinHandler g_join_handler(kProvisioningToken, &g_registry,
                                              &g_rate_limiter);
pulselink::CmdTable g_cmd_table;
pulselink::GatewayHsm g_hsm;

pulselink::espnow::EspNowTransport g_espnow;
WiFiClient g_wifi_client;
PubSubClient g_pubsub(g_wifi_client);
pulselink::gateway::PubSubClientMqttClient g_mqtt(&g_pubsub);

// Indexed by device_id — Registry assigns device_id as a 0-based slot
// index, so these are always in bounds (see core/pl_registry.h).
pulselink::LastSeenSeq g_uplink_dedupe[PULSELINK_MAX_NODES];
pulselink::SeqLossTracker g_loss_tracker[PULSELINK_MAX_NODES];

uint8_t g_gateway_channel = 0;
uint16_t g_next_cmd_id = 0;

// Ticks are seconds throughout this file — matches how PULSELINK_MAX_*
// timeout/window macros are documented (pl_config.h).
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

void publish_online_event(uint8_t device_id, bool online) {
  char topic[PULSELINK_MAX_TOPIC_LEN];
  pulselink::build_data_topic(kTenantId, device_id, "online", topic,
                               sizeof(topic));
  uint8_t payload[1] = {online ? static_cast<uint8_t>('1')
                                : static_cast<uint8_t>('0')};
  g_hsm.publish_or_spool(&g_mqtt, topic, payload, 1);
}

void resend_via_espnow(uint8_t device_id, const pulselink::CmdSlot& slot) {
  pulselink::RegistryEntry* node = g_registry.find_by_device_id(device_id);
  if (!node) return;

  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kCmd, /*seq=*/0, slot.cmd_id,
                           slot.payload, slot.payload_len, frame, &frame_len);
  g_espnow.send_unicast(node->mac, frame, frame_len);
}

// Only meaningful right after a poll()/try_deliver() call that returned
// false because retries were exhausted (state flips to FAILED) — ACKED
// slots publish their cmd_status from handle_cmd_ack() instead, where the
// real result code is known.
void publish_cmd_status_if_failed(uint8_t device_id) {
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

void handle_join_req(const uint8_t src[6], const uint8_t* payload,
                      uint8_t payload_len) {
  pulselink::JoinRequestPayload req;
  if (!pulselink::decode_join_request(payload, payload_len, &req)) return;

  pulselink::JoinAckPayload ack;
  if (!g_join_handler.handle(src, req, g_gateway_channel, now_ticks(),
                              &ack)) {
    return;  // bad token, rate-limited, or registry full — stay silent
  }

  uint8_t ack_payload[2];
  uint8_t ack_payload_len = pulselink::encode_join_ack(ack, ack_payload);
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + 2];
  uint8_t frame_len = 0;
  pulselink::encode_frame(pulselink::MsgType::kJoinAck, 0, 0, ack_payload,
                           ack_payload_len, frame, &frame_len);
  g_espnow.send_unicast(src, frame, frame_len);

  g_registry.set_field_name(ack.device_id, kFieldIdTemperatureC10,
                             "temperature");
  Serial.printf("joined device_id=%u\n", ack.device_id);
}

void handle_data(const uint8_t src[6], const pulselink::FrameHeader& header,
                  const uint8_t* payload, uint8_t payload_len) {
  pulselink::RegistryEntry* node = g_registry.find_by_mac(src);
  if (!node) return;  // not joined — ignore

  if (g_uplink_dedupe[node->device_id].is_duplicate(header.seq)) return;
  g_uplink_dedupe[node->device_id].accept(header.seq);
  g_loss_tracker[node->device_id].record(header.seq);

  if (g_registry.touch(src, now_ticks())) {
    publish_online_event(node->device_id, /*online=*/true);
  }

  pulselink::FieldReader fields(payload, payload_len);
  pulselink::FieldValue field;
  while (fields.next(&field)) {
    const char* field_name =
        g_registry.find_field_name(node->device_id, field.field_id);
    if (!field_name) continue;  // unmapped: nothing to publish as

    char topic[PULSELINK_MAX_TOPIC_LEN];
    pulselink::build_data_topic(kTenantId, node->device_id, field_name, topic,
                                 sizeof(topic));
    uint8_t text[PULSELINK_MAX_MQTT_PAYLOAD];
    uint16_t text_len = format_field_value(field, text, sizeof(text));
    if (text_len == 0) continue;

    g_hsm.publish_or_spool(&g_mqtt, topic, text, text_len);
  }

  // WAKE_AND_POLL: this uplink IS the listen window — deliver any pending
  // command right now. Never on a timer; the node's radio is off between
  // wakes (TRD.md §5.2, §4.2 "sleep-profile mailbox").
  if (node->sleep_profile == pulselink::SleepProfile::kWakeAndPoll) {
    pulselink::CmdSlot* slot = g_cmd_table.find(node->device_id);
    if (slot) {
      if (g_cmd_table.try_deliver(node->device_id, now_ticks())) {
        resend_via_espnow(node->device_id, *slot);
      } else {
        publish_cmd_status_if_failed(node->device_id);
      }
    }
  }
}

void handle_cmd_ack(const uint8_t src[6], const pulselink::FrameHeader& header,
                     const uint8_t* payload, uint8_t payload_len) {
  pulselink::RegistryEntry* node = g_registry.find_by_mac(src);
  if (!node) return;

  pulselink::CmdResult result;
  if (!pulselink::decode_cmd_ack(payload, payload_len, &result)) return;
  if (!g_cmd_table.on_ack(node->device_id, header.cmd_id)) return;

  // Two-loop ack model (TRD.md §3.5): this is the app-level ack, distinct
  // from whatever the earlier send_unicast() MAC-ack said. cmd_status is
  // only ever published from here or publish_cmd_status_if_failed() —
  // never from the MAC-ack alone.
  char topic[PULSELINK_MAX_TOPIC_LEN];
  pulselink::build_cmd_status_topic(kTenantId, node->device_id, topic,
                                     sizeof(topic));
  uint8_t status[PULSELINK_MAX_MQTT_PAYLOAD];
  uint16_t len =
      pulselink::format_cmd_status(/*delivered=*/true, result, status,
                                    sizeof(status));
  g_hsm.publish_or_spool(&g_mqtt, topic, status, len);
  g_cmd_table.clear_if_finished(node->device_id);
}

void service_espnow_uplink() {
  uint8_t src[6];
  uint8_t buf[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
  uint8_t len;

  while (g_espnow.receive(src, buf, &len)) {
    pulselink::FrameHeader header;
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
    if (pulselink::decode_frame(buf, len, &header, &payload, &payload_len) !=
        pulselink::FrameError::kOk) {
      continue;  // foreign traffic or a corrupt frame — silently discard
    }

    switch (header.msg_type) {
      case pulselink::MsgType::kJoinReq:
        handle_join_req(src, payload, payload_len);
        break;
      case pulselink::MsgType::kData:
        handle_data(src, header, payload, payload_len);
        break;
      case pulselink::MsgType::kCmdAck:
        handle_cmd_ack(src, header, payload, payload_len);
        break;
      default:
        break;  // PING/PONG/NACK/JOIN_ACK/CMD aren't expected inbound here
    }
  }
}

// ALWAYS_ON nodes only — WAKE_AND_POLL nodes are serviced from
// handle_data()'s listen-window path above, never from this timer.
void service_cmd_table_retries() {
  for (uint8_t device_id = 0; device_id < PULSELINK_MAX_NODES; ++device_id) {
    pulselink::RegistryEntry* node = g_registry.find_by_device_id(device_id);
    if (!node || node->sleep_profile != pulselink::SleepProfile::kAlwaysOn) {
      continue;
    }

    pulselink::CmdSlot* slot = g_cmd_table.find(device_id);
    if (!slot) continue;

    if (g_cmd_table.poll(device_id, now_ticks())) {
      resend_via_espnow(device_id, *slot);
    } else {
      publish_cmd_status_if_failed(device_id);
    }
  }
}

void service_mqtt_downlink() {
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
    if (!g_hsm.accept_downlink(&reason)) {
      // PulseDash never assumes delivery (PRD.md FR-3) — refusing here,
      // silently from the wire's perspective, is safe precisely because
      // PulseDash only trusts an actual cmd_status, which this command
      // will never produce.
      continue;
    }

    uint16_t cmd_id = g_next_cmd_id++;  // TRD.md §4.2: gateway stamps cmd_id
    if (!g_cmd_table.enqueue(static_cast<uint8_t>(device_id), cmd_id, payload,
                              static_cast<uint8_t>(payload_len),
                              now_ticks())) {
      continue;  // already one in flight for this node, or table full
    }

    if (node->sleep_profile == pulselink::SleepProfile::kAlwaysOn) {
      pulselink::CmdSlot* slot =
          g_cmd_table.find(static_cast<uint8_t>(device_id));
      if (slot &&
          g_cmd_table.try_deliver(static_cast<uint8_t>(device_id),
                                   now_ticks())) {
        resend_via_espnow(static_cast<uint8_t>(device_id), *slot);
      }
    }
    // WAKE_AND_POLL: left PENDING — delivered on the node's next uplink
    // listen window (handle_data()), not blasted out immediately.
  }
}

void service_health_metrics() {
  static uint32_t s_last_run = 0;
  uint32_t now = now_ticks();
  if (now - s_last_run < kHealthMetricsIntervalTicks) return;
  s_last_run = now;

  for (uint8_t device_id = 0; device_id < PULSELINK_MAX_NODES; ++device_id) {
    pulselink::RegistryEntry* node = g_registry.find_by_device_id(device_id);
    if (!node) continue;

    if (g_registry.check_offline_transition(device_id, now)) {
      publish_online_event(device_id, /*online=*/false);
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
  uint16_t gw_len = format_u32(g_espnow.rx_overflow_count(), gw_payload,
                                sizeof(gw_payload));
  g_hsm.publish_or_spool(&g_mqtt, gw_topic, gw_payload, gw_len);
}

void connect_mqtt() {
  if (!g_pubsub.connect("pulselink-gateway")) return;

  g_hsm.on_backhaul_up();
  char wildcard[PULSELINK_MAX_TOPIC_LEN];
  snprintf(wildcard, sizeof(wildcard), "pulsecore/%s/+/cmd", kTenantId);
  g_mqtt.subscribe(wildcard);
  Serial.println("MQTT connected");
}

void setup() {
  Serial.begin(115200);

  // g_registry was constructed as a global — before setup() runs, per
  // ordinary C++ static-init order — so its constructor's load() call
  // found g_prefs not yet begin()'d and loaded nothing. Force a real
  // reload now that NVS is actually ready (core/pl_registry.h's reload()
  // doc comment explains why this hazard exists).
  g_prefs.begin("pulselink", /*readOnly=*/false);
  g_registry.reload();

  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);
  while (WiFi.status() != WL_CONNECTED) delay(200);

  // Mandatory: default power-save duty-cycles the radio and drops
  // ESP-NOW frames (TRD.md §2).
  esp_wifi_set_ps(WIFI_PS_NONE);
  g_gateway_channel = WiFi.channel();

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init failed");
    return;
  }
  g_espnow.begin();

  g_pubsub.setServer(kMqttHost, kMqttPort);
  connect_mqtt();
}

void loop() {
  service_espnow_uplink();
  service_cmd_table_retries();

  if (!g_pubsub.connected()) {
    g_hsm.on_backhaul_down();
    static uint32_t s_last_reconnect_attempt = 0;
    uint32_t now_ms = millis();
    if (now_ms - s_last_reconnect_attempt > kMqttReconnectIntervalMs) {
      s_last_reconnect_attempt = now_ms;
      connect_mqtt();
    }
  } else {
    g_pubsub.loop();  // pumps the network, fires PubSubClient's callback
    service_mqtt_downlink();
    g_hsm.drain_step(&g_mqtt);
  }

  service_health_metrics();
}
