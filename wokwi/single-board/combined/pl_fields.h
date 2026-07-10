#ifndef PULSELINK_CORE_PL_FIELDS_H
#define PULSELINK_CORE_PL_FIELDS_H

#include <cstdint>
#include <cstring>

#include "pl_config.h"

// Self-describing DATA payload codec: a sequence of (field_id, type, value)
// tuples (TRD.md §3.3, D-004). The gateway maps field_id -> MQTT field name
// via per-node registry metadata, so introducing another node type never
// requires a gateway reflash.
//
// This stands in for the real Structa library (not vendored into this repo
// — see DECISIONS.md D-011); the wire format is fixed regardless of which
// encoder produces it, so swapping the implementation later is transparent.

namespace pulselink {

enum class FieldType : unsigned char {
  kU8 = 0,
  kBool,
  kU16,
  kI16,
  kU32,
  kI32,
  kF32,
};

inline uint8_t field_type_size(FieldType type) {
  switch (type) {
    case FieldType::kU8:
    case FieldType::kBool:
      return 1;
    case FieldType::kU16:
    case FieldType::kI16:
      return 2;
    case FieldType::kU32:
    case FieldType::kI32:
    case FieldType::kF32:
      return 4;
  }
  return 0;
}

struct FieldValue {
  uint8_t field_id;
  FieldType type;
  union {
    uint8_t u8;
    bool b;
    uint16_t u16;
    int16_t i16;
    uint32_t u32;
    int32_t i32;
    float f32;
  } value;
};

// Appends (field_id, type, value) tuples into a fixed-capacity buffer.
// Every write_* call fails closed (returns false, buffer unchanged in
// effect) rather than overflowing the caller's payload buffer.
class FieldWriter {
 public:
  FieldWriter(uint8_t* buf, uint8_t capacity)
      : buf_(buf), capacity_(capacity), len_(0) {}

  bool write_u8(uint8_t field_id, uint8_t v) {
    uint8_t bytes[1] = {v};
    return write_raw(field_id, FieldType::kU8, bytes, 1);
  }

  bool write_bool(uint8_t field_id, bool v) {
    uint8_t bytes[1] = {static_cast<uint8_t>(v ? 1 : 0)};
    return write_raw(field_id, FieldType::kBool, bytes, 1);
  }

  bool write_u16(uint8_t field_id, uint16_t v) {
    return write_u16_typed(field_id, FieldType::kU16, v);
  }

  bool write_i16(uint8_t field_id, int16_t v) {
    return write_u16_typed(field_id, FieldType::kI16,
                            static_cast<uint16_t>(v));
  }

  bool write_u32(uint8_t field_id, uint32_t v) {
    return write_u32_typed(field_id, FieldType::kU32, v);
  }

  bool write_i32(uint8_t field_id, int32_t v) {
    return write_u32_typed(field_id, FieldType::kI32,
                            static_cast<uint32_t>(v));
  }

  bool write_f32(uint8_t field_id, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return write_u32_typed(field_id, FieldType::kF32, bits);
  }

  uint8_t length() const { return len_; }

 private:
  bool write_raw(uint8_t field_id, FieldType type, const uint8_t* value,
                 uint8_t size) {
    uint16_t needed = static_cast<uint16_t>(2 + size);
    if (static_cast<uint16_t>(len_) + needed > capacity_) return false;
    buf_[len_++] = field_id;
    buf_[len_++] = static_cast<uint8_t>(type);
    for (uint8_t i = 0; i < size; ++i) buf_[len_++] = value[i];
    return true;
  }

  bool write_u16_typed(uint8_t field_id, FieldType type, uint16_t v) {
    uint8_t bytes[2] = {static_cast<uint8_t>(v & 0xFF),
                         static_cast<uint8_t>((v >> 8) & 0xFF)};
    return write_raw(field_id, type, bytes, 2);
  }

  bool write_u32_typed(uint8_t field_id, FieldType type, uint32_t v) {
    uint8_t bytes[4] = {
        static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF),
        static_cast<uint8_t>((v >> 24) & 0xFF)};
    return write_raw(field_id, type, bytes, 4);
  }

  uint8_t* buf_;
  uint8_t capacity_;
  uint8_t len_;
};

// Iterates tuples out of a DATA payload. next() returns false both at
// end-of-buffer and on a corrupt/truncated tuple (e.g. a type tag whose
// declared size runs past the buffer) — callers can't tell those apart from
// the return value alone, which is fine: either way there's nothing more to
// safely read.
class FieldReader {
 public:
  FieldReader(const uint8_t* buf, uint8_t len) : buf_(buf), len_(len), pos_(0) {}

  bool next(FieldValue* out) {
    if (static_cast<uint16_t>(pos_) + 2 > len_) return false;

    uint8_t field_id = buf_[pos_];
    FieldType type = static_cast<FieldType>(buf_[pos_ + 1]);
    uint8_t size = field_type_size(type);
    if (static_cast<uint16_t>(pos_) + 2 + size > len_) return false;

    const uint8_t* v = buf_ + pos_ + 2;
    out->field_id = field_id;
    out->type = type;
    switch (type) {
      case FieldType::kU8:
        out->value.u8 = v[0];
        break;
      case FieldType::kBool:
        out->value.b = v[0] != 0;
        break;
      case FieldType::kU16:
        out->value.u16 = static_cast<uint16_t>(
            static_cast<uint16_t>(v[0]) | (static_cast<uint16_t>(v[1]) << 8));
        break;
      case FieldType::kI16:
        out->value.i16 = static_cast<int16_t>(
            static_cast<uint16_t>(v[0]) | (static_cast<uint16_t>(v[1]) << 8));
        break;
      case FieldType::kU32:
        out->value.u32 = read_u32(v);
        break;
      case FieldType::kI32:
        out->value.i32 = static_cast<int32_t>(read_u32(v));
        break;
      case FieldType::kF32: {
        uint32_t bits = read_u32(v);
        memcpy(&out->value.f32, &bits, sizeof(bits));
        break;
      }
    }
    pos_ = static_cast<uint8_t>(pos_ + 2 + size);
    return true;
  }

 private:
  static uint32_t read_u32(const uint8_t* v) {
    return static_cast<uint32_t>(v[0]) | (static_cast<uint32_t>(v[1]) << 8) |
           (static_cast<uint32_t>(v[2]) << 16) |
           (static_cast<uint32_t>(v[3]) << 24);
  }

  const uint8_t* buf_;
  uint8_t len_;
  uint8_t pos_;
};

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_FIELDS_H
