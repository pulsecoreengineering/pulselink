# wokwi

Wokwi web-simulator projects. Radio is idealized here — not an RF testbed
(TRD.md §9, D-009).

- `single-board/` — gateway + node combined on one simulated ESP32,
  connected by the same in-process fake ESP-NOW medium the host-native
  tests use (real ESP-NOW isn't simulated — see that project's README for
  why and what this does/doesn't prove). WiFi and MQTT are real.

An earlier `gateway-node/` attempt assumed Wokwi supported different
firmware on two boards in one project; it doesn't (confirmed, not
assumed), so that project was removed in favor of `single-board/`.

Built in Phase 4 (PLAN.md).
