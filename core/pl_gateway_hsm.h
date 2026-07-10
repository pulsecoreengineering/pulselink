#ifndef PULSELINK_CORE_PL_GATEWAY_HSM_H
#define PULSELINK_CORE_PL_GATEWAY_HSM_H

#include <cstdint>

#include "pl_config.h"
#include "pl_mqtt.h"
#include "pl_spool.h"

// Gateway backhaul state machine (TRD.md §4.4, D-013 — see DECISIONS.md):
//
//   Connected            (WiFi up + MQTT session live)
//   |- Bridging          (normal operation: publish straight through)
//   \- Draining          (flushing the spool after reconnect)
//   Degraded             (WiFi or broker down)
//   \- ESP-NOW side KEEPS OPERATING; uplink -> bounded spool; downlink
//      refused with reason
//
// Key invariant: nodes must not care that the backhaul died. The ESP-NOW
// ring buffer (pl_ring.h) and the command table (pl_cmdtable.h) run
// completely independently of this state machine — this class only
// governs what happens to MQTT-bound uplink and MQTT-sourced downlink.

namespace pulselink {

enum class GatewayState : unsigned char {
  kDegraded = 0,
  kConnectedBridging,
  kConnectedDraining,
};

enum class DownlinkRefuseReason : unsigned char {
  kNone = 0,
  kBackhaulDegraded,  // WiFi or broker down — can't confirm/report delivery
  kDraining,          // reconnected but still flushing the spool
};

class GatewayHsm {
 public:
  GatewayHsm() : state_(GatewayState::kDegraded) {}

  GatewayState state() const { return state_; }

  // Call when WiFi association + MQTT session are both confirmed live.
  // Enters Draining if the spool has anything queued, else goes straight
  // to Bridging.
  void on_backhaul_up() {
    state_ = spool_.empty() ? GatewayState::kConnectedBridging
                             : GatewayState::kConnectedDraining;
  }

  // Call when WiFi drops or the MQTT session is lost.
  void on_backhaul_down() { state_ = GatewayState::kDegraded; }

  // Uplink path: publishes immediately while Bridging; while Degraded or
  // Draining, spools instead (bounded, drop-oldest — TRD.md §4.4).
  // `client` is only touched (and may be null) when state_ is Bridging.
  void publish_or_spool(MqttClient* client, const char* topic,
                         const uint8_t* payload, uint16_t payload_len) {
    if (state_ == GatewayState::kConnectedBridging) {
      client->publish(topic, payload, payload_len);
      return;
    }
    spool_.push(topic, payload, payload_len);
  }

  // Call once per main-loop tick while Draining. Flushes one spooled
  // entry per call (not all at once, so a slow broker doesn't stall the
  // rest of the loop) and transitions to Bridging once the spool empties.
  void drain_step(MqttClient* client) {
    if (state_ != GatewayState::kConnectedDraining) return;

    char topic[PULSELINK_MAX_TOPIC_LEN];
    uint8_t payload[PULSELINK_MAX_MQTT_PAYLOAD];
    uint16_t payload_len = 0;
    if (spool_.pop(topic, payload, &payload_len)) {
      client->publish(topic, payload, payload_len);
    }
    if (spool_.empty()) state_ = GatewayState::kConnectedBridging;
  }

  // Downlink path: PulseDash never assumes delivery (PRD.md §4.2), so a
  // command the gateway can't currently service — and can't reliably
  // report the outcome of — gets refused with a reason rather than
  // silently attempted.
  bool accept_downlink(DownlinkRefuseReason* out_reason) {
    if (state_ == GatewayState::kConnectedBridging) {
      *out_reason = DownlinkRefuseReason::kNone;
      return true;
    }
    *out_reason = (state_ == GatewayState::kConnectedDraining)
                      ? DownlinkRefuseReason::kDraining
                      : DownlinkRefuseReason::kBackhaulDegraded;
    return false;
  }

  const MqttSpool& spool() const { return spool_; }

 private:
  GatewayState state_;
  MqttSpool spool_;
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_GATEWAY_HSM_H
