#ifndef PULSELINK_CORE_PL_TOPICS_H
#define PULSELINK_CORE_PL_TOPICS_H

#include <cstdint>
#include <cstring>

#include "pl_config.h"

// MQTT topic scheme (TRD.md §1): pulsecore/{tenant_id}/{device_id}/{field},
// commands on .../cmd, results on .../cmd_status. Bridging the gateway into
// this tree is the whole point of PulseLink — PulseDash needs zero changes
// because these are the topics it already expects.
//
// Zero-heap: every builder writes into a caller-owned fixed buffer and
// returns the written length, or 0 on overflow. No std::string.

namespace pulselink {

namespace detail {

// Appends `s` to buf[*pos..cap), returns false (leaving *pos unchanged) on
// overflow so callers fail closed instead of silently truncating a topic.
inline bool append(char* buf, uint8_t cap, uint8_t* pos, const char* s) {
  uint8_t len = static_cast<uint8_t>(strlen(s));
  if (static_cast<uint16_t>(*pos) + len >= cap) return false;  // leave room for '\0'
  memcpy(buf + *pos, s, len);
  *pos = static_cast<uint8_t>(*pos + len);
  return true;
}

// Decimal-formats a uint8_t device_id (0-255) without snprintf/heap.
inline bool append_device_id(char* buf, uint8_t cap, uint8_t* pos,
                              uint8_t device_id) {
  char digits[4];
  uint8_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + (device_id % 10));
    device_id /= 10;
  } while (device_id > 0);

  if (static_cast<uint16_t>(*pos) + n >= cap) return false;
  for (uint8_t i = 0; i < n; ++i) buf[(*pos)++] = digits[n - 1 - i];
  return true;
}

}  // namespace detail

// Returns the topic length, or 0 on overflow (buf's contents are undefined
// in that case — check the return value, not buf's contents).
inline uint8_t build_data_topic(const char* tenant_id, uint8_t device_id,
                                 const char* field, char* out,
                                 uint8_t out_cap) {
  uint8_t pos = 0;
  if (!detail::append(out, out_cap, &pos, "pulsecore/")) return 0;
  if (!detail::append(out, out_cap, &pos, tenant_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/")) return 0;
  if (!detail::append_device_id(out, out_cap, &pos, device_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/")) return 0;
  if (!detail::append(out, out_cap, &pos, field)) return 0;
  out[pos] = '\0';
  return pos;
}

inline uint8_t build_cmd_topic(const char* tenant_id, uint8_t device_id,
                                char* out, uint8_t out_cap) {
  uint8_t pos = 0;
  if (!detail::append(out, out_cap, &pos, "pulsecore/")) return 0;
  if (!detail::append(out, out_cap, &pos, tenant_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/")) return 0;
  if (!detail::append_device_id(out, out_cap, &pos, device_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/cmd")) return 0;
  out[pos] = '\0';
  return pos;
}

inline uint8_t build_cmd_status_topic(const char* tenant_id, uint8_t device_id,
                                       char* out, uint8_t out_cap) {
  uint8_t pos = 0;
  if (!detail::append(out, out_cap, &pos, "pulsecore/")) return 0;
  if (!detail::append(out, out_cap, &pos, tenant_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/")) return 0;
  if (!detail::append_device_id(out, out_cap, &pos, device_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/cmd_status")) return 0;
  out[pos] = '\0';
  return pos;
}

// Gateway-wide health metrics (TRD.md FR-7's ring overflow counter) don't
// have a device_id — TRD.md doesn't name a topic for them, so this fills
// that gap with a "gateway" pseudo-segment in place of a device_id:
// pulsecore/{tenant_id}/gateway/{field}. Per-node metrics (loss rate,
// liveness) use build_data_topic() instead, since they *do* have a
// device_id and read naturally as just another field.
inline uint8_t build_gateway_topic(const char* tenant_id, const char* field,
                                    char* out, uint8_t out_cap) {
  uint8_t pos = 0;
  if (!detail::append(out, out_cap, &pos, "pulsecore/")) return 0;
  if (!detail::append(out, out_cap, &pos, tenant_id)) return 0;
  if (!detail::append(out, out_cap, &pos, "/gateway/")) return 0;
  if (!detail::append(out, out_cap, &pos, field)) return 0;
  out[pos] = '\0';
  return pos;
}

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_TOPICS_H
