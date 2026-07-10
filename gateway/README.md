# gateway

Gateway firmware (ESP32). Bridges the ESP-NOW node cluster to MQTT so
PulseDash sees the standard `pulsecore/{tenant_id}/{device_id}/{field}`
topic tree unchanged.

- `gateway.ino` — the whole thing wired together: join handling, uplink
  dedupe/loss-tracking/field-bridging, downlink command routing, the
  two-loop ack model, the WAKE_AND_POLL mailbox, health metrics, and the
  `GatewayHsm` backhaul state machine (TRD.md §4.4, D-013):
  ```
  Root
  ├── Connected            (WiFi up + MQTT session live)
  │   ├── Bridging         (normal operation)
  │   └── Draining         (flushing spool after reconnect)
  └── Degraded             (WiFi or broker down)
      ├── ESP-NOW side KEEPS OPERATING
      ├── uplink -> bounded spool (drop-oldest when full)
      └── downlink refused with reason
  ```
- `pl_nvs_registry_storage.h` — `RegistryStorage` backed by Arduino's
  `Preferences` (NVS). The RAM backend (`core/pl_registry.h`) is what
  `test/test_registry.cpp` exercises; this is the same interface,
  persisted.
- `pl_pubsubclient_mqtt.h` — `MqttClient` backed by `PubSubClient`, the
  standard Arduino MQTT library.

Edit the constants at the top of `gateway.ino` (WiFi SSID/password, MQTT
host/port, tenant_id, provisioning token) before flashing.

## What's verified vs. what isn't

Flagship article artifact for Part 4 (PRD.md §4.1). **Not compiled in this
repo's CI, and not compiled by me at all** — there's no ESP32 toolchain in
this host-native test environment, only a plain Linux g++. What *is*
verified:

- Every piece of protocol/orchestration logic this file calls (registry,
  join handler, command table, dedupe, loss tracker, gateway HSM, topic
  builder/parser) is covered by 126 host-native tests in `test/` against
  `transport/fake/` — that's real coverage, not a claim.
- `gateway.ino` itself was syntax-checked with `g++ -std=c++11 -Wall
  -Wextra -fsyntax-only` against hand-written stub headers standing in for
  `WiFi.h`, `esp_now.h`, `esp_wifi.h`, `Preferences.h`, and
  `PubSubClient.h` — it compiles clean with zero warnings. That catches
  syntax errors and type mismatches against *our own* headers, but the
  stubs are approximations of the real Arduino/ESP-IDF APIs (best-effort,
  matching current arduino-esp32 3.x signatures), not the real thing —
  they can't catch a wrong real API signature or runtime/hardware
  behavior.
- The topic scheme and `MqttClient` interface itself were validated
  against a genuine Mosquitto broker in `examples/part4-gateway/`
  (independently cross-checked with `mosquitto_sub`) — but that used
  `libmosquitto` on host, not `PubSubClient` on an ESP32.

What's genuinely unverified: `EspNowTransport` against real ESP-NOW
hardware, `NvsRegistryStorage` against real flash, `PubSubClientMqttClient`
against `PubSubClient`'s actual API, and the whole file under a real
Arduino-ESP32 toolchain. One design decision worth knowing about before
you flash this: `EspNowTransport::send_unicast()` blocks for up to 50 ms
turning ESP-NOW's async send callback into the synchronous MAC-ack every
host-tested caller expects (D-014, DECISIONS.md) — reasonable on paper,
unverified on a real radio.

This is where the hardware rig, a Wokwi project, and real devkits take
over (PLAN.md Phase 4) — resources this environment doesn't have.
