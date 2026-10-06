#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

#include "modern_sqlite/storage/page_bitvec.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;

constexpr std::size_t kNodeBytes = 512U;
constexpr std::size_t kHeaderBytes = 3U * sizeof(std::uint32_t);
constexpr std::size_t kUnionBytes = ((kNodeBytes - kHeaderBytes) / sizeof(void*)) * sizeof(void*);
constexpr std::size_t kHashSlots = kUnionBytes / sizeof(std::uint32_t);
constexpr std::size_t kMaximumHashEntries = kHashSlots / 2U;

[[nodiscard]] void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0U ? alignment : size) == 0) {
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
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

int main() {
  auto created = modern_sqlite::PageBitvec::Create(1'000'000U);
  if (!created.has_value()) {
    return 1;
  }
  modern_sqlite::PageBitvec bits = std::move(*created);

  const std::size_t sparse_before = allocation_count.load(std::memory_order_relaxed);
  for (std::uint32_t index = 0U; index < static_cast<std::uint32_t>(kMaximumHashEntries); ++index) {
    const std::uint32_t page = 1U + index * 2U;
    if (!bits.Set(modern_sqlite::PageNumber{page}).has_value()) {
      return 2;
    }
  }
  const std::size_t sparse_after = allocation_count.load(std::memory_order_relaxed);
  if (sparse_after != sparse_before) {
    return 3;
  }

  const modern_sqlite::PageNumber colliding_page{static_cast<std::uint32_t>(kHashSlots + 1U)};
  const std::size_t subdivision_before = allocation_count.load(std::memory_order_relaxed);
  if (!bits.Set(colliding_page).has_value()) {
    return 4;
  }
  const std::size_t subdivision_after = allocation_count.load(std::memory_order_relaxed);
  if (subdivision_after - subdivision_before != 1U) {
    return 5;
  }

  for (std::uint32_t index = 0U; index < static_cast<std::uint32_t>(kMaximumHashEntries); ++index) {
    const std::uint32_t page = 1U + index * 2U;
    if (!bits.Test(modern_sqlite::PageNumber{page})) {
      return 6;
    }
  }
  return bits.Test(colliding_page) ? 0 : 7;
}
