#include "modern_sqlite/instrumentation/counters.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <thread>
#include <utility>

namespace modern_sqlite::instrumentation {
namespace {

static_assert(MODERN_SQLITE_ENABLE_INSTRUMENTATION == 1);

constexpr std::array<std::pair<Counter, std::string_view>, 10> kCounters{{
    {Counter::kAllocations, "allocations"},
    {Counter::kBytesCopied, "bytes_copied"},
    {Counter::kVfsCalls, "vfs_calls"},
    {Counter::kPagesRead, "pages_read"},
    {Counter::kPagesWritten, "pages_written"},
    {Counter::kCacheHits, "cache_hits"},
    {Counter::kCacheMisses, "cache_misses"},
    {Counter::kBtreeComparisons, "btree_comparisons"},
    {Counter::kVmInstructions, "vm_instructions"},
    {Counter::kPlannerWork, "planner_work"},
}};

TEST(Instrumentation, CollectionsStartAtZeroWithStableCounterNames) {
  const CounterCollection counters;

  for (const auto& [counter, name] : kCounters) {
    EXPECT_EQ(name, CounterName(counter));
    EXPECT_EQ(0U, counters.Value(counter));
  }
}

TEST(Instrumentation, RecordsCountersOnlyInsideActiveScope) {
  CounterCollection counters;

  MODERN_SQLITE_RECORD_COUNTER(Counter::kVfsCalls, 100);
  {
    const ScopedCounterCollection scope{counters};
    MODERN_SQLITE_RECORD_COUNTER(Counter::kVfsCalls, 2);
    MODERN_SQLITE_RECORD_COUNTER(Counter::kBytesCopied, 4096);
  }
  MODERN_SQLITE_RECORD_COUNTER(Counter::kVfsCalls, 100);

  EXPECT_EQ(2U, counters.Value(Counter::kVfsCalls));
  EXPECT_EQ(4096U, counters.Value(Counter::kBytesCopied));
}

TEST(Instrumentation, RecordsProbeCallsOnlyInsideActiveScope) {
  ProbeCounterCollection probes;

  MODERN_SQLITE_RECORD_PROBE_CALL(1);
  {
    const ScopedProbeCounterCollection scope{probes};
    MODERN_SQLITE_RECORD_PROBE_CALL(1);
    MODERN_SQLITE_RECORD_PROBE_CALL(2);
    MODERN_SQLITE_RECORD_PROBE_CALL(2);
  }
  MODERN_SQLITE_RECORD_PROBE_CALL(2);

  EXPECT_EQ(1U, probes.Value(1));
  EXPECT_EQ(2U, probes.Value(2));
  EXPECT_EQ(0U, probes.Value(kProbeTagCount));
  probes.Reset();
  EXPECT_EQ(0U, probes.Value(1));
  EXPECT_EQ(0U, probes.Value(2));
}

TEST(Instrumentation, RestoresNestedCollection) {
  CounterCollection outer;
  CounterCollection inner;

  {
    const ScopedCounterCollection outer_scope{outer};
    MODERN_SQLITE_RECORD_COUNTER(Counter::kPlannerWork, 1);
    {
      const ScopedCounterCollection inner_scope{inner};
      MODERN_SQLITE_RECORD_COUNTER(Counter::kPlannerWork, 2);
    }
    MODERN_SQLITE_RECORD_COUNTER(Counter::kPlannerWork, 3);
  }

  EXPECT_EQ(4U, outer.Value(Counter::kPlannerWork));
  EXPECT_EQ(2U, inner.Value(Counter::kPlannerWork));
}

TEST(Instrumentation, DoesNotLeakAcrossSequentialOperations) {
  CounterCollection first;
  CounterCollection second;

  {
    const ScopedCounterCollection scope{first};
    MODERN_SQLITE_RECORD_COUNTER(Counter::kPagesRead, 3);
  }
  MODERN_SQLITE_RECORD_COUNTER(Counter::kPagesRead, 100);
  {
    const ScopedCounterCollection scope{second};
    MODERN_SQLITE_RECORD_COUNTER(Counter::kPagesRead, 5);
  }

  EXPECT_EQ(3U, first.Value(Counter::kPagesRead));
  EXPECT_EQ(5U, second.Value(Counter::kPagesRead));
}

TEST(Instrumentation, KeepsActiveCollectionsThreadLocal) {
  CounterCollection main_thread;
  CounterCollection worker_thread;

  {
    const ScopedCounterCollection main_scope{main_thread};
    MODERN_SQLITE_RECORD_COUNTER(Counter::kVmInstructions, 1);

    std::thread worker{[&worker_thread] {
      MODERN_SQLITE_RECORD_COUNTER(Counter::kVmInstructions, 100);
      const ScopedCounterCollection worker_scope{worker_thread};
      MODERN_SQLITE_RECORD_COUNTER(Counter::kVmInstructions, 2);
    }};
    worker.join();
  }

  EXPECT_EQ(1U, main_thread.Value(Counter::kVmInstructions));
  EXPECT_EQ(2U, worker_thread.Value(Counter::kVmInstructions));
}

TEST(Instrumentation, ResetsCollectionExplicitly) {
  CounterCollection counters;
  {
    const ScopedCounterCollection scope{counters};
    MODERN_SQLITE_RECORD_COUNTER(Counter::kCacheHits, 7);
    MODERN_SQLITE_RECORD_COUNTER(Counter::kCacheMisses, 2);
  }

  counters.Reset();

  for (const auto& [counter, name] : kCounters) {
    static_cast<void>(name);
    EXPECT_EQ(0U, counters.Value(counter));
  }
}

}  // namespace
}  // namespace modern_sqlite::instrumentation
