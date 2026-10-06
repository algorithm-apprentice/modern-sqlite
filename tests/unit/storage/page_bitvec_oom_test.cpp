#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <utility>

#include "modern_sqlite/storage/page_bitvec.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;

constexpr std::size_t kNodeBytes = 512U;
constexpr std::size_t kHeaderBytes = 3U * sizeof(std::uint32_t);
constexpr std::size_t kUnionBytes = ((kNodeBytes - kHeaderBytes) / sizeof(void*)) * sizeof(void*);
constexpr std::size_t kHashSlots = kUnionBytes / sizeof(std::uint32_t);
constexpr std::size_t kMaximumHashEntries = kHashSlots / 2U;
constexpr modern_sqlite::PageNumber kCollidingPage{static_cast<std::uint32_t>(kHashSlots + 1U)};

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0U ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

void Arm(std::optional<std::size_t> failure) noexcept {
  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = failure;
}

[[nodiscard]] std::size_t Disarm() noexcept {
  failing_allocation.reset();
  return allocation_index.load(std::memory_order_relaxed);
}

[[nodiscard]] modern_sqlite::Result<modern_sqlite::PageBitvec> CreateSeeded() {
  auto created = modern_sqlite::PageBitvec::Create(1'000'000U);
  if (!created.has_value()) {
    return std::unexpected(std::move(created.error()));
  }
  modern_sqlite::PageBitvec bits = std::move(*created);
  for (std::uint32_t index = 0U; index < static_cast<std::uint32_t>(kMaximumHashEntries); ++index) {
    const std::uint32_t page = 1U + index * 2U;
    auto set = bits.Set(modern_sqlite::PageNumber{page});
    if (!set.has_value()) {
      return std::unexpected(std::move(set.error()));
    }
  }
  return bits;
}

[[nodiscard]] bool SeedBitsRemainSet(const modern_sqlite::PageBitvec& bits) noexcept {
  for (std::uint32_t index = 0U; index < static_cast<std::uint32_t>(kMaximumHashEntries); ++index) {
    const std::uint32_t page = 1U + index * 2U;
    if (!bits.Test(modern_sqlite::PageNumber{page})) {
      return false;
    }
  }
  return true;
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
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

int main() {
  Arm(std::nullopt);
  const auto baseline_create = modern_sqlite::PageBitvec::Create(1'000'000U);
  const std::size_t create_allocations = Disarm();
  if (!baseline_create.has_value() || create_allocations == 0U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < create_allocations; ++failure) {
    Arm(failure);
    const auto created = modern_sqlite::PageBitvec::Create(1'000'000U);
    (void)Disarm();
    if (created.has_value() || created.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return 2;
    }
  }

  auto measured = CreateSeeded();
  if (!measured.has_value()) {
    return 3;
  }
  Arm(std::nullopt);
  const auto baseline_set = measured->Set(kCollidingPage);
  const std::size_t subdivision_allocations = Disarm();
  if (!baseline_set.has_value() || subdivision_allocations == 0U) {
    return 4;
  }

  for (std::size_t failure = 0; failure < subdivision_allocations; ++failure) {
    auto seeded = CreateSeeded();
    if (!seeded.has_value()) {
      return 5;
    }
    Arm(failure);
    const auto set = seeded->Set(kCollidingPage);
    (void)Disarm();
    if (set.has_value() || set.error().code() != modern_sqlite::ErrorCode::kOutOfMemory ||
        !SeedBitsRemainSet(*seeded)) {
      return 6;
    }
    if (!seeded->Set(kCollidingPage).has_value() || !SeedBitsRemainSet(*seeded) ||
        !seeded->Test(kCollidingPage)) {
      return 7;
    }
  }
  return 0;
}
