#ifndef PULSELINK_CORE_PL_FRAME_H
#define PULSELINK_CORE_PL_FRAME_H

#include <cstdint>
#include <cstring>

#include "pl_config.h"

// Frame codec: header serialized field-by-field (TRD.md §3.1), body is
// opaque bytes here (msg-type-specific bodies — e.g. self-describing DATA
// tuples in pl_fields.h — are built/parsed by their own callers). No raw
// packed-struct casts across the wire (D-005).

namespace pulselink {

enum class MsgType : unsigned char {
  kJoinReq = 0,
  kJoinAck,
  kData,
  kCmd,
  kCmdAck,
  kPing,
  kPong,
  kNack,
};

enum class FrameError : unsigned char {
  kOk = 0,
  kTooShort,        // buffer shorter than the fixed 7-byte header
  kBadMagic,        // foreign/non-PulseLink traffic on the channel
  kBadVersion,      // protocol version this build doesn't speak
  kLengthMismatch,  // payload_len field doesn't match bytes actually given
  kPayloadTooLarge,  // caller tried to encode more than fits in one frame
};

// Constant signature byte; frames failing this check are foreign traffic on
// the channel and must be discarded without further parsing (TRD.md §7).
constexpr uint8_t kFrameMagic = 0xA5;

// High nibble of the version byte; low nibble is reserved (always 0 today).
constexpr uint8_t kProtocolVersion = 1;

struct FrameHeader {
  uint8_t magic;
  uint8_t version;  // decoded version number (0-15), not the packed byte
  MsgType msg_type;
  uint8_t seq;
  uint16_t cmd_id;
  uint8_t payload_len;
};

inline void encode_header(const FrameHeader& header,
                           uint8_t out[PULSELINK_FRAME_HEADER_SIZE]) {
  out[0] = header.magic;
  out[1] = static_cast<uint8_t>((header.version & 0x0F) << 4);
  out[2] = static_cast<uint8_t>(header.msg_type);
  out[3] = header.seq;
  out[4] = static_cast<uint8_t>(header.cmd_id & 0xFF);
  out[5] = static_cast<uint8_t>((header.cmd_id >> 8) & 0xFF);
  out[6] = header.payload_len;
}

inline FrameError decode_header(const uint8_t* buf, uint8_t len,
                                 FrameHeader* out) {
  if (len < PULSELINK_FRAME_HEADER_SIZE) return FrameError::kTooShort;
  if (buf[0] != kFrameMagic) return FrameError::kBadMagic;

  uint8_t version = static_cast<uint8_t>((buf[1] >> 4) & 0x0F);
  if (version != kProtocolVersion) return FrameError::kBadVersion;

  out->magic = buf[0];
  out->version = version;
  out->msg_type = static_cast<MsgType>(buf[2]);
  out->seq = buf[3];
  out->cmd_id = static_cast<uint16_t>(
      static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8));
  out->payload_len = buf[6];
  return FrameError::kOk;
}

// Encodes header + payload into `out` (must be at least
// PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD bytes).
inline FrameError encode_frame(MsgType type, uint8_t seq, uint16_t cmd_id,
                                const uint8_t* payload, uint8_t payload_len,
                                uint8_t* out, uint8_t* out_len) {
  if (payload_len > PULSELINK_MAX_FRAME_PAYLOAD) {
    return FrameError::kPayloadTooLarge;
  }

  FrameHeader header{kFrameMagic, kProtocolVersion, type, seq, cmd_id,
                      payload_len};
  encode_header(header, out);
  if (payload_len > 0) {
    memcpy(out + PULSELINK_FRAME_HEADER_SIZE, payload, payload_len);
  }
  *out_len = static_cast<uint8_t>(PULSELINK_FRAME_HEADER_SIZE + payload_len);
  return FrameError::kOk;
}

// Decodes a frame in place: `out_payload` points into `buf`, no copy.
// Rejects truncated or corrupt frames where payload_len doesn't match the
// bytes actually available (TRD.md §7 failure mode: truncated/corrupt).
inline FrameError decode_frame(const uint8_t* buf, uint8_t len,
                                FrameHeader* out_header,
                                const uint8_t** out_payload,
                                uint8_t* out_payload_len) {
  FrameError err = decode_header(buf, len, out_header);
  if (err != FrameError::kOk) return err;

  uint8_t expected_total = static_cast<uint8_t>(PULSELINK_FRAME_HEADER_SIZE +
                                                 out_header->payload_len);
  if (expected_total != len) return FrameError::kLengthMismatch;

  *out_payload = buf + PULSELINK_FRAME_HEADER_SIZE;
  *out_payload_len = out_header->payload_len;
  return FrameError::kOk;
}

}  // namespace pulselink

#endif  // PULSELINK_CORE_PL_FRAME_H
