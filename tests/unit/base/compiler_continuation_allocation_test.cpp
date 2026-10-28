#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

#include "modern_sqlite/base/compiler_continuation.hpp"
#include "tests/support/aligned_allocation.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::size_t failing_allocation = std::numeric_limits<std::size_t>::max();
bool count_allocations = false;
bool inject_failure = false;

void CountAllocation() {
  if (count_allocations || inject_failure) {
    const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
    if (inject_failure && index == failing_allocation) {
      throw std::bad_alloc{};
    }
  }
}

[[nodiscard]] void* Allocate(std::size_t size) {
  CountAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  CountAllocation();
  void* memory = nullptr;
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}

int main() {
  using modern_sqlite::compiler_detail::ContinuationStack;
  ContinuationStack<std::size_t> shallow;
  allocation_index.store(0, std::memory_order_relaxed);
  count_allocations = true;
  shallow.Push(std::size_t{0});
  count_allocations = false;
  if (allocation_index.load(std::memory_order_relaxed) != 0U) {
    return 1;
  }

  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = 0;
  inject_failure = true;
  bool threw = false;
  try {
    shallow.Push(std::size_t{1});
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  inject_failure = false;
  if (!threw || shallow.size() != 1U || shallow.Pop() != 0U) {
    return 1;
  }

  {
    constexpr std::size_t growth_depth = 256;
    constexpr std::size_t maximum_growth_allocations =
        static_cast<std::size_t>(std::bit_width(growth_depth - 2U)) + 1U;
    ContinuationStack<std::size_t> growth;
    allocation_index.store(0, std::memory_order_relaxed);
    count_allocations = true;
    for (std::size_t index = 0; index < growth_depth; ++index) {
      growth.Push(std::size_t{index});
    }
    count_allocations = false;
    const auto growth_allocations = allocation_index.load(std::memory_order_relaxed);
    if (growth_allocations > maximum_growth_allocations) {
      std::fprintf(stderr, "continuation growth allocations=%zu limit=%zu for %zu frames\n",
                   growth_allocations, maximum_growth_allocations, growth_depth);
      return 1;
    }
  }

  constexpr std::size_t depth = 1000;
  std::size_t allocations = 0;
  {
    ContinuationStack<std::size_t> baseline;
    allocation_index.store(0, std::memory_order_relaxed);
    count_allocations = true;
    for (std::size_t index = 0; index < depth; ++index) {
      baseline.Push(std::size_t{index});
    }
    count_allocations = false;
    allocations = allocation_index.load(std::memory_order_relaxed);
    if (allocations == 0U || allocations > 64U || baseline.size() != depth) {
      return 1;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    inject_failure = true;
    for (std::size_t index = depth; index > 0U; --index) {
      if (baseline.Pop() != index - 1U) {
        inject_failure = false;
        return 1;
      }
    }
    inject_failure = false;
    if (!baseline.empty() || allocation_index.load(std::memory_order_relaxed) != 0U) {
      return 1;
    }
  }

  for (std::size_t failure = 0; failure < allocations; ++failure) {
    ContinuationStack<std::size_t> pending;
    pending.Push(std::size_t{0});
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    std::size_t retained = 1;
    inject_failure = true;
    threw = false;
    try {
      for (std::size_t index = 1; index < depth; ++index) {
        pending.Push(std::size_t{index});
        ++retained;
      }
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    inject_failure = false;
    if (!threw || pending.size() != retained) {
      return 1;
    }
    while (!pending.empty()) {
      if (pending.Pop() != --retained) {
        return 1;
      }
    }
    for (std::size_t index = 0; index < depth; ++index) {
      pending.Push(std::size_t{index});
    }
    if (pending.size() != depth) {
      return 1;
    }
  }
  return 0;
}
