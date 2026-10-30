#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>

#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/native_vfs.hpp"
#include "modern_sqlite/storage/page_number.hpp"
#include "tests/support/aligned_allocation.hpp"
#include "tests/support/page_cache_test_peer.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;
bool count_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (count_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* memory = std::malloc(size == 0 ? 1U : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (count_allocations) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
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

int main() try {
  using namespace modern_sqlite;

  const std::filesystem::path fixture =
      std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path().parent_path() /
      "fixtures/read_compatibility/sqlite-3.54.0-read-compatibility.db";
  NativeVfs vfs;
  auto opened = Pager::Open(vfs, fixture.string(), PagerOptions{.cache_capacity_pages = 2});
  if (!opened.has_value() || !(*opened)->BeginRead().has_value() || (*opened)->page_count() < 4U) {
    return 1;
  }
  for (const PageNumber page : std::array{PageNumber{1}, PageNumber{2}}) {
    auto pin = (*opened)->ReadPage(page);
    if (!pin.has_value()) {
      return 1;
    }
  }

  auto replay_cache = PageCache::Create(PageCacheOptions{
      .page_size = ByteCount{8},
      .capacity_pages = 1,
  });
  if (!replay_cache.has_value()) {
    return 1;
  }
  auto replay = (*replay_cache)->InsertExclusive(PageNumber{1}, ByteBuffer{ByteCount{8}});
  if (!replay.has_value() || !replay->MarkDirty().has_value() ||
      !replay->MarkNeedsSync().has_value() || !replay->MarkDontWrite().has_value()) {
    return 1;
  }
  ByteBuffer image{ByteCount{8}};
  image.mutable_view().front() = std::byte{0x33};
  image.mutable_view().back() = std::byte{0x77};

  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations = true;
  std::uint64_t digest = 0;
  constexpr std::array kPages{PageNumber{3}, PageNumber{4}, PageNumber{1}, PageNumber{2}};
  for (std::size_t repetition = 0; repetition < 8U; ++repetition) {
    if (!PageCacheTestPeer::RestoreBytes(**replay_cache, *replay, image.view()).has_value()) {
      count_allocations = false;
      return 1;
    }
    for (const PageNumber page : kPages) {
      auto pin = (*opened)->ReadPage(page);
      if (!pin.has_value()) {
        count_allocations = false;
        return 1;
      }
      digest += std::to_integer<std::uint8_t>(pin->frame().bytes().front());
    }
  }
  count_allocations = false;

  const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
  if (allocations != 0U) {
    std::fprintf(stderr, "page-cache recycle allocations: %zu\n", allocations);
  }
  if (allocations != 0U || digest == 0U || !replay->frame().dirty() || !replay->needs_sync() ||
      !replay->dont_write() || replay->frame().bytes().front() != std::byte{0x33} ||
      replay->frame().bytes().back() != std::byte{0x77}) {
    return 1;
  }
  return (*opened)->EndRead().has_value() ? 0 : 1;
} catch (...) {
  count_allocations = false;
  return 1;
}
