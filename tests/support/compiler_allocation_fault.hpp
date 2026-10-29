#ifndef MODERN_SQLITE_TESTS_SUPPORT_COMPILER_ALLOCATION_FAULT_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_COMPILER_ALLOCATION_FAULT_HPP_

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <new>
#include <thread>

#include "tests/support/aligned_allocation.hpp"

namespace modern_sqlite::test_support::compiler_allocation_fault {

// Allocation-hook diagnostics cannot call potentially allocating C++ formatting.
// NOLINTBEGIN(modernize-use-std-print)

inline thread_local std::atomic<std::size_t> allocation_index = 0;
inline thread_local std::size_t failing_allocation = 0;
inline thread_local bool inject_failure = false;
inline thread_local bool registered_allocation_thread = false;
inline std::atomic<bool> allocation_tracking_active = false;
inline std::atomic<bool> unregistered_allocation = false;
inline const char* owner_name = "compiler";

inline void CountAllocation() {
  if (allocation_tracking_active.load(std::memory_order_relaxed) && !registered_allocation_thread) {
    unregistered_allocation.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "%s OOM allocation from an unregistered thread\n", owner_name);
    throw std::bad_alloc{};
  }
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
}

[[nodiscard]] inline void* Allocate(std::size_t size) {
  CountAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] inline void* AllocateAligned(std::size_t size, std::size_t alignment) {
  CountAllocation();
  void* memory = nullptr;
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

template <typename AdditionalIsolationCheck>
[[nodiscard]] bool VerifyThreadDomains(const AdditionalIsolationCheck& additional_isolation) {
  const auto* main_index = &allocation_index;
  const auto* main_failure = &failing_allocation;
  const auto* main_injection = &inject_failure;
  bool verified = false;
  std::jthread worker([&] {
    if (&allocation_index == main_index || &failing_allocation == main_failure ||
        &inject_failure == main_injection || !additional_isolation()) {
      std::fprintf(stderr, "%s OOM worker shares the main allocation domain\n", owner_name);
      return;
    }
    const auto fails_allocation = [](bool aligned) {
      try {
        void* memory = aligned ? AllocateAligned(32, alignof(std::max_align_t)) : Allocate(1);
        if (aligned) {
          FreeAligned(memory);
        } else {
          std::free(memory);
        }
      } catch (const std::bad_alloc&) {
        return true;
      }
      return false;
    };
    if (!fails_allocation(false) || !fails_allocation(true) ||
        allocation_index.load(std::memory_order_relaxed) != 0U) {
      std::fprintf(stderr, "%s OOM unregistered allocation was not rejected\n", owner_name);
      return;
    }
    registered_allocation_thread = true;
    for (const bool aligned : {false, true}) {
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = 0;
      inject_failure = true;
      const bool failed = fails_allocation(aligned);
      inject_failure = false;
      if (!failed || allocation_index.load(std::memory_order_relaxed) != 1U ||
          fails_allocation(aligned) || allocation_index.load(std::memory_order_relaxed) != 2U) {
        std::fprintf(stderr, "%s OOM registered fault or recovery did not match\n", owner_name);
        return;
      }
    }
    verified = true;
  });
  const std::size_t main_allocations = allocation_index.load(std::memory_order_relaxed);
  worker.join();
  return verified && unregistered_allocation.load(std::memory_order_relaxed) &&
         allocation_index.load(std::memory_order_relaxed) == main_allocations && !inject_failure;
}

[[nodiscard]] inline bool VerifyThreadDomains() {
  return VerifyThreadDomains([] { return true; });
}

// NOLINTEND(modernize-use-std-print)

}  // namespace modern_sqlite::test_support::compiler_allocation_fault

#endif  // MODERN_SQLITE_TESTS_SUPPORT_COMPILER_ALLOCATION_FAULT_HPP_
