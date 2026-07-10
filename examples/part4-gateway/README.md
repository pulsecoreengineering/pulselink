# examples/part4-gateway

Flagship code artifact for LogicFrenzy series Part 4 ("Building the MQTT
gateway"): the ESP-NOW-to-MQTT bridge itself.

- `host_bridge_demo.cpp` — the same bridging logic as
  `test/test_gateway_bridge.cpp` (decode DATA, map field_id -> name via the
  registry, build the `pulsecore/{tenant}/{device}/{field}` topic, publish
  or spool depending on `GatewayHsm` state), but backed by a **real**
  `libmosquitto` connection instead of the fake — proof the topic scheme
  and the `MqttClient` interface actually work over the wire, not just
  against our own test double. It also proves the downlink direction:
  subscribes to `.../cmd` and receives a real published command.

  Optional target — only builds when `libmosquitto` is found on the host
  (`find_library` in this dir's `CMakeLists.txt`); CI doesn't have it
  installed, so it's silently skipped there. Point it at any broker:
  ```
  MQTT_HOST=127.0.0.1 MQTT_PORT=1883 ./build/examples/part4-gateway/part4_host_bridge_demo
  ```
  (a typical `docker run -p 1883:1883 eclipse-mosquitto` uses the default
  host/port already, so `MQTT_HOST`/`MQTT_PORT` are only needed if yours
  differs). Verified during development against a local Mosquitto instance
  and independently cross-checked with a plain `mosquitto_sub`.

## What's built vs. what's still ahead

This covers the MQTT integration half of Phase 4 (PLAN.md): topic mapping,
the pluggable `MqttClient` interface, the bounded spool, and the gateway
HSM (`core/pl_gateway_hsm.h`, `core/pl_spool.h`, `core/pl_topics.h`,
`core/pl_mqtt.h`), all host-tested via `transport/fake/pl_fake_mqtt.h` plus
this real-broker demo.

Still ahead, and all needing resources this environment doesn't have (real
hardware, an ESP32 toolchain):

- `transport/espnow/` — the real ESP-NOW backend (`WIFI_PS_NONE`, unicast
  peer management)
- NVS-backed `RegistryStorage` (the RAM backend is host-test-only)
- A Wokwi project (gateway + 2 simulated nodes against local Mosquitto)
- The hardware rig itself: 3+ ESP32 devkits, this bridge logic actually
  flashed and running, PulseDash pointed at the same broker
- The Part 4 demo recording (live nodes on a PulseDash dashboard)

Article artifact — keep it minimal and readable, it appears in print
(CLAUDE.md).
