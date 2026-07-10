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

#endif  // PULSELINK_CORE_PL_CONFIG_H
