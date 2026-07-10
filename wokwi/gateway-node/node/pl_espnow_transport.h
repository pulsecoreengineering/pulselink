#ifndef PULSELINK_TRANSPORT_ESPNOW_PL_ESPNOW_TRANSPORT_H
#define PULSELINK_TRANSPORT_ESPNOW_PL_ESPNOW_TRANSPORT_H

// Real ESP-NOW transport backend (ESP32-only). NOT compiled in this
// repo's CI — no ESP32 toolchain in the host-native test environment (see
// gateway/README.md). Every method here implements the Transport contract
// that core/ and the fake backend already exercise 124 host-native tests
// against, so the logic riding on top of Transport (join, cmd table,
// dedupe, bridging...) needs no changes to run on hardware — this file,
// node/'s equivalent, and the two NVS/MQTT backends under gateway/ are
// what Phase 4 hardware bring-up actually adds.

#include <cstring>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "pl_config.h"
#include "pl_ring.h"
#include "pl_transport.h"

namespace pulselink {
namespace espnow {

class EspNowTransport : public pulselink::Transport {
 public:
  // How long send_unicast() will wait for ESP-NOW's send callback before
  // giving up and reporting failure. ESP-NOW's own ack normally lands
  // within a few ms on a quiet channel; this just bounds the worst case.
  static const uint32_t kSendAckTimeoutMs = 50;

  EspNowTransport() : send_pending_(false), send_acked_(false) {
    instance_ptr() = this;
  }

  // Call once from setup(), after esp_now_init(). Registers this
  // instance's callbacks with the ESP-NOW driver — only one
  // EspNowTransport should exist at a time (the driver only supports one
  // recv/send callback pair globally, hence the static self-pointer).
  bool begin() {
    WiFi.macAddress(local_mac_);
    bool recv_ok =
        esp_now_register_recv_cb(&EspNowTransport::on_recv_trampoline) ==
        ESP_OK;
    bool send_ok =
        esp_now_register_send_cb(&EspNowTransport::on_send_trampoline) ==
        ESP_OK;
    return recv_ok && send_ok;
  }

  // D-014 (DECISIONS.md): every host-tested caller of Transport was
  // written against the fake transport's synchronous "true MAC-ack"
  // contract. Real ESP-NOW's ack is asynchronous (arrives via the send
  // callback some time after esp_now_send() returns), so this method
  // turns it back into a synchronous answer with a short bounded wait —
  // keeping the interface contract intact on hardware rather than
  // quietly changing what the return value means. The wait happens here,
  // in loop()-context code, never inside on_recv_trampoline (that stays
  // callback-safe per CLAUDE.md's discipline regardless).
  bool send_unicast(const uint8_t dest_mac[6], const uint8_t* data,
                     uint8_t len) override {
    ensure_peer(dest_mac);

    send_pending_ = true;
    send_acked_ = false;
    if (esp_now_send(dest_mac, data, len) != ESP_OK) {
      send_pending_ = false;
      return false;
    }

    uint32_t start = millis();
    while (send_pending_ && (millis() - start) < kSendAckTimeoutMs) {
      delay(1);
    }
    return send_acked_;
  }

  bool send_broadcast(const uint8_t* data, uint8_t len) override {
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ensure_peer(broadcast_mac);
    esp_now_send(broadcast_mac, data, len);
    // MAC ack always "succeeds" for broadcast — never trust it for
    // JOIN_REQ delivery confirmation (TRD.md §3.5).
    return true;
  }

  bool receive(uint8_t out_src_mac[6], uint8_t* out_buf,
               uint8_t* out_len) override {
    return rx_ring_.pop(out_src_mac, out_buf, out_len);
  }

  void local_mac(uint8_t out_mac[6]) const override {
    memcpy(out_mac, local_mac_, 6);
  }

  unsigned long rx_overflow_count() const { return rx_ring_.overflow_count(); }

 private:
  void ensure_peer(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;  // current channel — the router dictates it (TRD.md §2)
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }

  // WiFi task context: memcpy into the ring and return, nothing else
  // (CLAUDE.md callback discipline — same rule as PulseCore's ISR rule).
  static void on_recv_trampoline(const esp_now_recv_info_t* info,
                                  const uint8_t* data, int len) {
    EspNowTransport* self = instance_ptr();
    if (!self || len < 0 || len > 255) return;
    self->rx_ring_.push(info->src_addr, data, static_cast<uint8_t>(len));
  }

  static void on_send_trampoline(const uint8_t* /*mac*/,
                                  esp_now_send_status_t status) {
    EspNowTransport* self = instance_ptr();
    if (!self) return;
    self->send_acked_ = (status == ESP_NOW_SEND_SUCCESS);
    self->send_pending_ = false;
  }

  // C++11-safe header-only singleton pointer: a static local inside an
  // inline (implicitly, since it's defined in-class) function has exactly
  // one definition across translation units, unlike a plain out-of-class
  // static data member definition (which needs C++17's `inline` to be
  // header-safe — this project targets C++11, CLAUDE.md).
  static EspNowTransport*& instance_ptr() {
    static EspNowTransport* p = nullptr;
    return p;
  }

  UplinkRing rx_ring_;
  uint8_t local_mac_[6];
  volatile bool send_pending_;
  volatile bool send_acked_;
};

}  // namespace espnow
}  // namespace pulselink

#endif  // PULSELINK_TRANSPORT_ESPNOW_PL_ESPNOW_TRANSPORT_H
