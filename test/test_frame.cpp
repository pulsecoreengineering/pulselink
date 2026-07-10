#include "pl_test.h"

#include "../core/pl_frame.h"

using pulselink::FrameError;
using pulselink::FrameHeader;
using pulselink::MsgType;

PL_TEST_CASE(frame_round_trips_every_msg_type) {
  const MsgType types[] = {
      MsgType::kJoinReq, MsgType::kJoinAck, MsgType::kData, MsgType::kCmd,
      MsgType::kCmdAck,  MsgType::kPing,    MsgType::kPong, MsgType::kNack,
  };
  const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};

  for (MsgType type : types) {
    uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD];
    uint8_t frame_len = 0;
    FrameError enc_err =
        pulselink::encode_frame(type, /*seq=*/7, /*cmd_id=*/0x1234, payload,
                                 sizeof(payload), frame, &frame_len);
    PL_ASSERT(enc_err == FrameError::kOk);
    PL_ASSERT(frame_len == PULSELINK_FRAME_HEADER_SIZE + sizeof(payload));

    FrameHeader header;
    const uint8_t* out_payload = nullptr;
    uint8_t out_payload_len = 0;
    FrameError dec_err = pulselink::decode_frame(
        frame, frame_len, &header, &out_payload, &out_payload_len);
    PL_ASSERT(dec_err == FrameError::kOk);
    PL_ASSERT(header.msg_type == type);
    PL_ASSERT(header.seq == 7);
    PL_ASSERT(header.cmd_id == 0x1234);
    PL_ASSERT(out_payload_len == sizeof(payload));
    for (uint8_t i = 0; i < sizeof(payload); ++i) {
      PL_ASSERT(out_payload[i] == payload[i]);
    }
  }
}

PL_TEST_CASE(foreign_traffic_is_discarded_on_bad_magic) {
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE] = {0xFF, 0x10, 0x02, 0, 0, 0, 0};
  FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  FrameError err = pulselink::decode_frame(frame, sizeof(frame), &header,
                                            &payload, &payload_len);
  PL_ASSERT(err == FrameError::kBadMagic);
}

PL_TEST_CASE(unsupported_version_is_rejected) {
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE] = {
      pulselink::kFrameMagic, 0x20 /* version 2 in the high nibble */, 0x02,
      0, 0, 0, 0};
  FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  FrameError err = pulselink::decode_frame(frame, sizeof(frame), &header,
                                            &payload, &payload_len);
  PL_ASSERT(err == FrameError::kBadVersion);
}

PL_TEST_CASE(buffer_shorter_than_header_is_rejected) {
  uint8_t frame[3] = {pulselink::kFrameMagic, 0x10, 0x02};
  FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  FrameError err = pulselink::decode_frame(frame, sizeof(frame), &header,
                                            &payload, &payload_len);
  PL_ASSERT(err == FrameError::kTooShort);
}

PL_TEST_CASE(truncated_payload_is_rejected) {
  uint8_t payload[10] = {0};
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + 10];
  uint8_t frame_len = 0;
  pulselink::encode_frame(MsgType::kData, 1, 0, payload, sizeof(payload),
                           frame, &frame_len);

  // header still claims payload_len == 10, but hand decode_frame fewer
  // bytes than that — simulates a truncated/corrupt frame on the wire.
  FrameHeader header;
  const uint8_t* out_payload = nullptr;
  uint8_t out_payload_len = 0;
  FrameError err = pulselink::decode_frame(
      frame, static_cast<uint8_t>(frame_len - 3), &header, &out_payload,
      &out_payload_len);
  PL_ASSERT(err == FrameError::kLengthMismatch);
}

PL_TEST_CASE(oversized_payload_is_rejected_at_encode) {
  uint8_t oversized[PULSELINK_MAX_FRAME_PAYLOAD + 1] = {0};
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE + PULSELINK_MAX_FRAME_PAYLOAD + 1];
  uint8_t frame_len = 0;
  FrameError err = pulselink::encode_frame(
      MsgType::kData, 0, 0, oversized, sizeof(oversized), frame, &frame_len);
  PL_ASSERT(err == FrameError::kPayloadTooLarge);
}

PL_TEST_CASE(cmd_id_round_trips_little_endian) {
  uint8_t frame[PULSELINK_FRAME_HEADER_SIZE];
  uint8_t frame_len = 0;
  pulselink::encode_frame(MsgType::kCmd, 0, /*cmd_id=*/0xBEEF, nullptr, 0,
                           frame, &frame_len);
  PL_ASSERT(frame[4] == 0xEF);  // low byte first on the wire
  PL_ASSERT(frame[5] == 0xBE);

  FrameHeader header;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  pulselink::decode_frame(frame, frame_len, &header, &payload, &payload_len);
  PL_ASSERT(header.cmd_id == 0xBEEF);
}
