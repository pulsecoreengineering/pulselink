#include "pl_test.h"

#include <cstring>

#include "../core/pl_cmd_status.h"

using namespace pulselink;  // NOLINT

PL_TEST_CASE(delivered_status_uses_the_result_name) {
  uint8_t buf[32];
  uint16_t len = format_cmd_status(true, CmdResult::kBusy, buf, sizeof(buf));
  PL_ASSERT(len == strlen("busy"));
  PL_ASSERT(memcmp(buf, "busy", len) == 0);
}

PL_TEST_CASE(undelivered_status_is_always_failed_regardless_of_result) {
  uint8_t buf[32];
  uint16_t len = format_cmd_status(false, CmdResult::kOk, buf, sizeof(buf));
  PL_ASSERT(len == strlen("failed"));
  PL_ASSERT(memcmp(buf, "failed", len) == 0);
}

PL_TEST_CASE(every_result_code_has_a_name) {
  const CmdResult results[] = {CmdResult::kOk, CmdResult::kUnknownCmd,
                                CmdResult::kBusy, CmdResult::kInvalidParam,
                                CmdResult::kHwFault};
  for (CmdResult r : results) {
    const char* name = cmd_result_name(r);
    PL_ASSERT(name != nullptr);
    PL_ASSERT(strcmp(name, "unknown") != 0);
  }
}
