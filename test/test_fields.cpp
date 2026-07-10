#include "pl_test.h"

#include "../core/pl_fields.h"

using namespace pulselink;  // NOLINT

PL_TEST_CASE(field_tuples_round_trip_every_type) {
  uint8_t buf[64];
  FieldWriter w(buf, sizeof(buf));
  PL_ASSERT(w.write_u8(1, 200));
  PL_ASSERT(w.write_bool(2, true));
  PL_ASSERT(w.write_u16(3, 60000));
  PL_ASSERT(w.write_i16(4, -1234));
  PL_ASSERT(w.write_u32(5, 4000000000u));
  PL_ASSERT(w.write_i32(6, -70000));
  PL_ASSERT(w.write_f32(7, 21.5f));

  FieldReader r(buf, w.length());
  FieldValue f;

  PL_ASSERT(r.next(&f));
  PL_ASSERT(f.field_id == 1 && f.type == FieldType::kU8 && f.value.u8 == 200);

  PL_ASSERT(r.next(&f));
  PL_ASSERT(f.field_id == 2 && f.type == FieldType::kBool &&
            f.value.b == true);

  PL_ASSERT(r.next(&f));
  PL_ASSERT(f.field_id == 3 && f.type == FieldType::kU16 &&
            f.value.u16 == 60000);

  PL_ASSERT(r.next(&f));
  PL_ASSERT(f.field_id == 4 && f.type == FieldType::kI16 &&
            f.value.i16 == -1234);

  PL_ASSERT(r.next(&f));
  PL_ASSERT(f.field_id == 5 && f.type == FieldType::kU32 &&
            f.value.u32 == 4000000000u);

  PL_ASSERT(r.next(&f));
  PL_ASSERT(f.field_id == 6 && f.type == FieldType::kI32 &&
            f.value.i32 == -70000);

  PL_ASSERT(r.next(&f));
  PL_ASSERT(f.field_id == 7 && f.type == FieldType::kF32 &&
            f.value.f32 == 21.5f);

  PL_ASSERT(!r.next(&f));  // end of buffer
}

PL_TEST_CASE(field_writer_fails_closed_when_capacity_exceeded) {
  uint8_t buf[3];  // too small for even one u16 field (needs 2 + 2 = 4)
  FieldWriter w(buf, sizeof(buf));
  PL_ASSERT(!w.write_u16(1, 100));
  PL_ASSERT(w.length() == 0);
}

PL_TEST_CASE(field_writer_packs_multiple_fields_tightly) {
  uint8_t buf[16];
  FieldWriter w(buf, sizeof(buf));
  w.write_u8(1, 10);
  w.write_u8(2, 20);
  // 2 fields * (1 id + 1 type + 1 value) = 6 bytes.
  PL_ASSERT(w.length() == 6);
}

PL_TEST_CASE(field_reader_rejects_truncated_tuple) {
  // Declares a u32 (needs 4 value bytes) but only supplies 1.
  uint8_t buf[3] = {9, static_cast<uint8_t>(FieldType::kU32), 0x01};
  FieldReader r(buf, sizeof(buf));
  FieldValue f;
  PL_ASSERT(!r.next(&f));
}
