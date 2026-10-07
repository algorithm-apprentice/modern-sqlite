#include <gtest/gtest.h>

#include "tests/compatibility/write_session_crash_harness.hpp"

namespace modern_sqlite::test {
namespace {

TEST(WriteSessionCrash, RecoversEveryImplicitInsertCut) {
  EXPECT_NO_THROW(RunWriteSessionCrashHarness());
}

TEST(WriteSessionCrash, RecoversTransactionAndSchemaCuts) {
  EXPECT_NO_THROW(RunWriteSessionTransactionCrashHarness());
}

}  // namespace
}  // namespace modern_sqlite::test
