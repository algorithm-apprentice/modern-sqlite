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

inline constexpr std::size_t kProbeTagCount = 512;

enum class ProbeCollectionKind : std::uint8_t {
  RowEvaluation,
  AggregateWork,
};

enum class AggregateWorkMetric : std::uint8_t {
  Open,
  Step,
  Finalize,
  Reset,
  Close,
  RetainedBytesPeak,
  WorkingSetBytesPeak,
  LiveFilesPeak,
  FileAcquisitions,
  SorterSpillWrites,
};

inline constexpr std::size_t kAggregateWorkMetricCount =
    static_cast<std::size_t>(AggregateWorkMetric::SorterSpillWrites) + 1U;
inline constexpr std::size_t kAggregateWorkFirstTag = kProbeTagCount - kAggregateWorkMetricCount;

[[nodiscard]] constexpr std::string_view AggregateWorkMetricName(
    AggregateWorkMetric metric) noexcept {
  switch (metric) {
    case AggregateWorkMetric::Open:
      return "open";
    case AggregateWorkMetric::Step:
      return "step";
    case AggregateWorkMetric::Finalize:
      return "finalize";
    case AggregateWorkMetric::Reset:
      return "reset";
    case AggregateWorkMetric::Close:
      return "close";
    case AggregateWorkMetric::RetainedBytesPeak:
      return "retained_bytes_peak";
    case AggregateWorkMetric::WorkingSetBytesPeak:
      return "working_set_bytes_peak";
    case AggregateWorkMetric::LiveFilesPeak:
      return "live_files_peak";
    case AggregateWorkMetric::FileAcquisitions:
      return "file_acquisitions";
    case AggregateWorkMetric::SorterSpillWrites:
      return "sorter_spill_writes";
  }
  return "unknown";
}

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
void RecordProbeCall(std::size_t tag) noexcept;
void RecordAggregateWork(AggregateWorkMetric metric, std::uint64_t amount, bool maximum) noexcept;

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

class ProbeCounterCollection final {
 public:
  explicit ProbeCounterCollection(
      ProbeCollectionKind kind = ProbeCollectionKind::RowEvaluation) noexcept
      : kind_(kind) {}

  [[nodiscard]] std::uint64_t Value(std::size_t tag) const noexcept {
    return tag < values_.size() ? values_[tag] : 0;
  }
  [[nodiscard]] std::uint64_t Value(AggregateWorkMetric metric) const noexcept {
    return Value(kAggregateWorkFirstTag + static_cast<std::size_t>(metric));
  }
  void Reset() noexcept { values_.fill(0); }

 private:
  friend void detail::RecordProbeCall(std::size_t tag) noexcept;
  friend void detail::RecordAggregateWork(AggregateWorkMetric metric, std::uint64_t amount,
                                          bool maximum) noexcept;

  void Add(std::size_t tag) noexcept {
    if (tag < values_.size() &&
        (kind_ != ProbeCollectionKind::AggregateWork || tag < kAggregateWorkFirstTag)) {
      ++values_[tag];
    }
  }

  void Record(AggregateWorkMetric metric, std::uint64_t amount, bool maximum) noexcept {
    const std::size_t tag = kAggregateWorkFirstTag + static_cast<std::size_t>(metric);
    if (kind_ != ProbeCollectionKind::AggregateWork || tag >= values_.size()) {
      return;
    }
    if (maximum) {
      if (amount > values_[tag]) {
        values_[tag] = amount;
      }
    } else {
      values_[tag] += amount;
    }
  }

  ProbeCollectionKind kind_;
  std::array<std::uint64_t, kProbeTagCount> values_{};
};

class ScopedProbeCounterCollection final {
 public:
  explicit ScopedProbeCounterCollection(ProbeCounterCollection& collection) noexcept;
  ~ScopedProbeCounterCollection() noexcept;

  ScopedProbeCounterCollection(const ScopedProbeCounterCollection&) = delete;
  ScopedProbeCounterCollection& operator=(const ScopedProbeCounterCollection&) = delete;
  ScopedProbeCounterCollection(ScopedProbeCounterCollection&&) = delete;
  ScopedProbeCounterCollection& operator=(ScopedProbeCounterCollection&&) = delete;

 private:
  ProbeCounterCollection* previous_;
};

}  // namespace modern_sqlite::instrumentation

#if MODERN_SQLITE_ENABLE_INSTRUMENTATION
#define MODERN_SQLITE_RECORD_COUNTER(counter, amount)                                            \
  do {                                                                                           \
    ::modern_sqlite::instrumentation::detail::RecordCounter((counter),                           \
                                                            static_cast<std::uint64_t>(amount)); \
  } while (false)
#define MODERN_SQLITE_RECORD_PROBE_CALL(tag)                                                  \
  do {                                                                                        \
    ::modern_sqlite::instrumentation::detail::RecordProbeCall(static_cast<std::size_t>(tag)); \
  } while (false)
#define MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(metric)                                \
  do {                                                                                  \
    ::modern_sqlite::instrumentation::detail::RecordAggregateWork((metric), 1U, false); \
  } while (false)
#define MODERN_SQLITE_RECORD_AGGREGATE_MAXIMUM(metric, amount)     \
  do {                                                             \
    ::modern_sqlite::instrumentation::detail::RecordAggregateWork( \
        (metric), static_cast<std::uint64_t>(amount), true);       \
  } while (false)
#else
#define MODERN_SQLITE_RECORD_COUNTER(counter, amount)                                              \
  do {                                                                                             \
    if constexpr (false) {                                                                         \
      ::modern_sqlite::instrumentation::detail::RecordCounter((counter),                           \
                                                              static_cast<std::uint64_t>(amount)); \
    }                                                                                              \
  } while (false)
#define MODERN_SQLITE_RECORD_PROBE_CALL(tag)                                                    \
  do {                                                                                          \
    if constexpr (false) {                                                                      \
      ::modern_sqlite::instrumentation::detail::RecordProbeCall(static_cast<std::size_t>(tag)); \
    }                                                                                           \
  } while (false)
#define MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(metric)                                  \
  do {                                                                                    \
    if constexpr (false) {                                                                \
      ::modern_sqlite::instrumentation::detail::RecordAggregateWork((metric), 1U, false); \
    }                                                                                     \
  } while (false)
#define MODERN_SQLITE_RECORD_AGGREGATE_MAXIMUM(metric, amount)       \
  do {                                                               \
    if constexpr (false) {                                           \
      ::modern_sqlite::instrumentation::detail::RecordAggregateWork( \
          (metric), static_cast<std::uint64_t>(amount), true);       \
    }                                                                \
  } while (false)
#endif

#endif  // MODERN_SQLITE_INSTRUMENTATION_COUNTERS_HPP_
