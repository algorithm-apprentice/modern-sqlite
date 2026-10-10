#include "modern_sqlite/instrumentation/counters.hpp"

#include <utility>

namespace modern_sqlite::instrumentation {
namespace {

thread_local CounterCollection* active_collection = nullptr;
thread_local ProbeCounterCollection* active_probe_collection = nullptr;

[[nodiscard]] constexpr std::size_t CounterIndex(Counter counter) noexcept {
  return static_cast<std::size_t>(counter);
}

}  // namespace

std::uint64_t CounterCollection::Value(Counter counter) const noexcept {
  return values_[CounterIndex(counter)];
}

void CounterCollection::Reset() noexcept { values_.fill(0); }

void CounterCollection::Add(Counter counter, std::uint64_t amount) noexcept {
  values_[CounterIndex(counter)] += amount;
}

ScopedCounterCollection::ScopedCounterCollection(CounterCollection& collection) noexcept
    : previous_(std::exchange(active_collection, &collection)) {}

ScopedCounterCollection::~ScopedCounterCollection() noexcept { active_collection = previous_; }

ScopedProbeCounterCollection::ScopedProbeCounterCollection(
    ProbeCounterCollection& collection) noexcept
    : previous_(std::exchange(active_probe_collection, &collection)) {}

ScopedProbeCounterCollection::~ScopedProbeCounterCollection() noexcept {
  active_probe_collection = previous_;
}

namespace detail {

void RecordCounter(Counter counter, std::uint64_t amount) noexcept {
  if (active_collection != nullptr) {
    active_collection->Add(counter, amount);
  }
}

void RecordProbeCall(std::size_t tag) noexcept {
  if (active_probe_collection != nullptr) {
    active_probe_collection->Add(tag);
  }
}

}  // namespace detail
}  // namespace modern_sqlite::instrumentation
