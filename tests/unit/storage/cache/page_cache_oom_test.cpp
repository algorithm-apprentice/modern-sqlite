#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/cache/page_cache.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failure_index;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failure_index.has_value() && index == *failure_index) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1U : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failure_index.has_value() && index == *failure_index) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

void Arm(std::size_t index) {
  allocation_index.store(0, std::memory_order_relaxed);
  failure_index = index;
}

void Disarm() noexcept { failure_index.reset(); }

[[nodiscard]] modern_sqlite::ByteBuffer MakePage(std::uint8_t seed) {
  modern_sqlite::ByteBuffer page{modern_sqlite::ByteCount{8}};
  for (std::size_t index = 0; index < page.size().value(); ++index) {
    page.mutable_view()[index] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
  }
  return page;
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

int main() try {
  using namespace modern_sqlite;

  for (std::size_t failure = 0; failure < 3U; ++failure) {
    Arm(failure);
    auto cache = PageCache::Create(PageCacheOptions{
        .page_size = ByteCount{8},
        .capacity_pages = 4,
    });
    Disarm();
    if (cache.has_value() || cache.error().code() != ErrorCode::kOutOfMemory) {
      return 1;
    }
  }

  auto created = PageCache::Create(PageCacheOptions{
      .page_size = ByteCount{8},
      .capacity_pages = 300,
  });
  if (!created.has_value()) {
    return 1;
  }
  std::unique_ptr<PageCache> cache = std::move(*created);

  ByteBuffer first_page = MakePage(0x10);
  Arm(0);
  auto failed_insert = cache->Insert(PageNumber{1}, std::move(first_page));
  Disarm();
  if (failed_insert.has_value() || failed_insert.error().code() != ErrorCode::kOutOfMemory ||
      cache->page_count() != 0U) {
    return 1;
  }
  auto retried = cache->Insert(PageNumber{1}, MakePage(0x10));
  if (!retried.has_value()) {
    return 1;
  }
  retried = std::unexpected(Error::Create(ErrorCode::kGeneric, "release retry pin"));

  for (std::uint32_t page = 2; page <= 256; ++page) {
    auto inserted = cache->Insert(PageNumber{page}, MakePage(static_cast<std::uint8_t>(page)));
    if (!inserted.has_value()) {
      return 1;
    }
  }
  ByteBuffer growth_page = MakePage(0x30);
  Arm(0);
  auto benign_growth = cache->Insert(PageNumber{257}, std::move(growth_page));
  Disarm();
  if (!benign_growth.has_value() || cache->page_count() != 257U) {
    return 1;
  }
  return 0;
} catch (...) {
  Disarm();
  return 1;
}
