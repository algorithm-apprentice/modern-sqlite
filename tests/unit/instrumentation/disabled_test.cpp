#include <gtest/gtest.h>

#include <cstdint>

#include "modern_sqlite/instrumentation/counters.hpp"

namespace modern_sqlite::instrumentation {
namespace {

static_assert(MODERN_SQLITE_ENABLE_INSTRUMENTATION == 0);

TEST(InstrumentationDisabled, HookArgumentsAreNotEvaluatedOrLinked) {
  std::uint64_t counter_evaluations = 0;
  std::uint64_t amount_evaluations = 0;

  MODERN_SQLITE_RECORD_COUNTER((++counter_evaluations, Counter::kAllocations),
                               ++amount_evaluations);

  EXPECT_EQ(0U, counter_evaluations);
  EXPECT_EQ(0U, amount_evaluations);
}

}  // namespace
}  // namespace modern_sqlite::instrumentation
