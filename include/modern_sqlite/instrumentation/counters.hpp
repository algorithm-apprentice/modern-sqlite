#ifndef MODERN_SQLITE_INSTRUMENTATION_COUNTERS_HPP_
#define MODERN_SQLITE_INSTRUMENTATION_COUNTERS_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#ifndef MODERN_SQLITE_ENABLE_INSTRUMENTATION
#define MODERN_SQLITE_ENABLE_INSTRUMENTATION 0
#endif

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION != 0 && MODERN_SQLITE_ENABLE_INSTRUMENTATION != 1
#error "MODERN_SQLITE_ENABLE_INSTRUMENTATION must be 0 or 1"
#endif

namespace modern_sqlite::instrumentation {

enum class Counter : std::uint8_t {
  kAllocations,
  kBytesCopied,
  kVfsCalls,
  kPagesRead,
  kPagesWritten,
  kCacheHits,
  kCacheMisses,
  kBtreeComparisons,
  kVmInstructions,
  kPlannerWork,
};

inline constexpr std::size_t kCounterCount = static_cast<std::size_t>(Counter::kPlannerWork) + 1U;

[[nodiscard]] constexpr std::string_view CounterName(Counter counter) noexcept {
  switch (counter) {
    case Counter::kAllocations:
      return "allocations";
    case Counter::kBytesCopied:
      return "bytes_copied";
    case Counter::kVfsCalls:
      return "vfs_calls";
    case Counter::kPagesRead:
      return "pages_read";
    case Counter::kPagesWritten:
      return "pages_written";
    case Counter::kCacheHits:
      return "cache_hits";
    case Counter::kCacheMisses:
      return "cache_misses";
    case Counter::kBtreeComparisons:
      return "btree_comparisons";
    case Counter::kVmInstructions:
      return "vm_instructions";
    case Counter::kPlannerWork:
      return "planner_work";
  }
  return "unknown";
}

class CounterCollection;

namespace detail {

void RecordCounter(Counter counter, std::uint64_t amount) noexcept;

}  // namespace detail

class CounterCollection final {
 public:
  [[nodiscard]] std::uint64_t Value(Counter counter) const noexcept;
  void Reset() noexcept;

 private:
  friend void detail::RecordCounter(Counter counter, std::uint64_t amount) noexcept;

  void Add(Counter counter, std::uint64_t amount) noexcept;

  std::array<std::uint64_t, kCounterCount> values_{};
};

class ScopedCounterCollection final {
 public:
  explicit ScopedCounterCollection(CounterCollection& collection) noexcept;
  ~ScopedCounterCollection() noexcept;

  ScopedCounterCollection(const ScopedCounterCollection&) = delete;
  ScopedCounterCollection& operator=(const ScopedCounterCollection&) = delete;
  ScopedCounterCollection(ScopedCounterCollection&&) = delete;
  ScopedCounterCollection& operator=(ScopedCounterCollection&&) = delete;

 private:
  CounterCollection* previous_;
};

}  // namespace modern_sqlite::instrumentation

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
#define MODERN_SQLITE_RECORD_COUNTER(counter, amount)                                            \
  do {                                                                                           \
    ::modern_sqlite::instrumentation::detail::RecordCounter((counter),                           \
                                                            static_cast<std::uint64_t>(amount)); \
  } while (false)
#else
#define MODERN_SQLITE_RECORD_COUNTER(counter, amount)                                              \
  do {                                                                                             \
    if constexpr (false) {                                                                         \
      ::modern_sqlite::instrumentation::detail::RecordCounter((counter),                           \
                                                              static_cast<std::uint64_t>(amount)); \
    }                                                                                              \
  } while (false)
#endif

#endif  // MODERN_SQLITE_INSTRUMENTATION_COUNTERS_HPP_
