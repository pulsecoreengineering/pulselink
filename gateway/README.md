# gateway

Gateway firmware (ESP32). Bridges the ESP-NOW node cluster to MQTT so
PulseDash sees the standard `pulsecore/{tenant_id}/{device_id}/{field}`
topic tree unchanged.

Owns the PulseHSM state machine described in TRD.md §4.4 and CLAUDE.md:

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

Flagship article artifact for Part 4 (PRD.md §4.1). Built starting Phase 4
(PLAN.md); depends on `core/`, `transport/espnow/`, and the NVS registry
backend.
