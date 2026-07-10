#ifndef PULSELINK_CORE_PL_CONFIG_H
#define PULSELINK_CORE_PL_CONFIG_H

// Compile-time configuration for core/ and transport/.
// Every bound here exists so core/ and transport/ can stay zero-heap
// (static/stack allocation only). See CLAUDE.md and TRD.md §6.
//
// Override any of these before including core headers to retune for a
// specific deployment; defaults match the numbers in TRD.md §1, §4, §6.

// Max ESP-NOW nodes per gateway (PRD.md §6: unicast-first, ~20-node ceiling).
#ifndef PULSELINK_MAX_NODES
#define PULSELINK_MAX_NODES 20
#endif

// Frame header is a fixed 7 bytes (TRD.md §3.1), serialized field-by-field.
#ifndef PULSELINK_FRAME_HEADER_SIZE
#define PULSELINK_FRAME_HEADER_SIZE 7
#endif

// ESP-NOW v1 payload cap is 250 B; header takes 7, leaving <=243 for the
// Structa-packed body (TRD.md §3.1, D-007).
#ifndef PULSELINK_MAX_FRAME_PAYLOAD
#define PULSELINK_MAX_FRAME_PAYLOAD 243
#endif

// Uplink ring buffer depth. TRD.md §4.1: "depth ~= node_count *
// frames_per_wake * safety factor (initial: 32 x 260 B ~= 8 KB)".
#ifndef PULSELINK_MAX_RING_DEPTH
#define PULSELINK_MAX_RING_DEPTH 32
#endif

// Command table slots: one per node (TRD.md §4.2, §6: "20 x ~300 B").
#ifndef PULSELINK_MAX_CMD_SLOTS
#define PULSELINK_MAX_CMD_SLOTS PULSELINK_MAX_NODES
#endif

// Shared-secret provisioning token carried in JOIN_REQ (TRD.md §5.1).
// Size is a tutorial-scale default, not a TRD-mandated number.
#ifndef PULSELINK_PROVISIONING_TOKEN_SIZE
#define PULSELINK_PROVISIONING_TOKEN_SIZE 4
#endif

// Consecutive unicast send failures before a node wipes its channel
// assumption and re-runs discovery — this *is* the channel-change recovery
// mechanism (TRD.md §5.1 point 4, D-008). Tutorial-scale default.
#ifndef PULSELINK_MAX_UNICAST_FAILURES
#define PULSELINK_MAX_UNICAST_FAILURES 5
#endif

// Gateway-side join rate limiting (TRD.md §7 failure mode 3: a
// crash-looping node spamming JOIN_REQ). At most this many attempts per
// MAC are allowed inside a window of this many simulated ticks; the rest
// are silently ignored (silence means retry, per the protocol's own
// semantics — no NACK needed). Tutorial-scale defaults, not TRD numbers.
#ifndef PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW
#define PULSELINK_MAX_JOIN_ATTEMPTS_PER_WINDOW 3
#endif

#ifndef PULSELINK_JOIN_RATE_WINDOW_TICKS
#define PULSELINK_JOIN_RATE_WINDOW_TICKS 60
#endif

// Downlink command retry policy (TRD.md §4.2). Retries live in the calling
// state's own counter (Pattern 8, CLAUDE.md) — these bound how many
// retransmissions a command table slot gets before it's marked FAILED, and
// how long to wait between them. Tutorial-scale defaults, not TRD numbers.
#ifndef PULSELINK_MAX_CMD_RETRIES
#define PULSELINK_MAX_CMD_RETRIES 3
#endif

#ifndef PULSELINK_CMD_RETRY_TIMEOUT_TICKS
#define PULSELINK_CMD_RETRY_TIMEOUT_TICKS 5
#endif

// MQTT topic scheme: pulsecore/{tenant_id}/{device_id}/{field} (TRD.md §1).
// device_id is formatted as decimal digits (registry's device_id is a
// uint8_t, so max 3 digits); tenant_id and field are short fixed strings.
#ifndef PULSELINK_MAX_TENANT_ID_LEN
#define PULSELINK_MAX_TENANT_ID_LEN 16
#endif

#ifndef PULSELINK_MAX_FIELD_NAME_LEN
#define PULSELINK_MAX_FIELD_NAME_LEN 16
#endif

// Per-node field_id -> MQTT field name map (TRD.md §3.3, §4.3): "gateway
// maps field_id -> topic name via registry metadata... delivered at join
// or provisioned." This tutorial's gateway example provisions it directly
// rather than extending JOIN_REQ's wire format — both are valid per TRD.
#ifndef PULSELINK_MAX_FIELDS_PER_NODE
#define PULSELINK_MAX_FIELDS_PER_NODE 8
#endif

#ifndef PULSELINK_MAX_TOPIC_LEN
#define PULSELINK_MAX_TOPIC_LEN 64
#endif

// MQTT payload cap: a single field's value as compact text, not JSON (no
// JSON-on-the-wire applies to MQTT payloads too — CLAUDE.md's zero-heap
// contract is ESP-NOW-side, but the "no JSON" spirit carries over: one
// topic, one scalar value, human-readable for a `mosquitto_sub` to show).
#ifndef PULSELINK_MAX_MQTT_PAYLOAD
#define PULSELINK_MAX_MQTT_PAYLOAD 32
#endif

// Bounded uplink spool while the backhaul is down (TRD.md §4.4 Degraded
// superstate): drop-oldest when full, same policy as the ESP-NOW ring.
#ifndef PULSELINK_MAX_MQTT_SPOOL_DEPTH
#define PULSELINK_MAX_MQTT_SPOOL_DEPTH 32
#endif

// Node liveness (TRD.md §4.3, FR-7): miss this many simulated ticks of
// silence since last_seen and the node is reported offline. Tutorial-scale
// default — real deployments size this off each node's actual check-in
// cadence (sleep profile dependent).
#ifndef PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS
#define PULSELINK_NODE_OFFLINE_TIMEOUT_TICKS 120
#endif

#endif  // PULSELINK_CORE_PL_CONFIG_H
