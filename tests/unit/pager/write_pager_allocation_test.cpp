#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "modern_sqlite/pager/pager.hpp"
#include "write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;

[[nodiscard]] void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] std::optional<std::size_t> RepeatedWriteAllocations() {
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 4);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return std::nullopt;
  }
  {
    const auto page = pager->WritePage(modern_sqlite::PageNumber{2});
    if (!page.has_value()) {
      return std::nullopt;
    }
  }

  allocation_count.store(0, std::memory_order_relaxed);
  {
    const auto page = pager->WritePage(modern_sqlite::PageNumber{2});
    if (!page.has_value()) {
      return std::nullopt;
    }
  }
  return allocation_count.load(std::memory_order_relaxed);
}

[[nodiscard]] std::optional<std::size_t> PressureSpillAllocations() {
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 1);
  if (pager == nullptr || !pager->BeginRead().has_value()) {
    return std::nullopt;
  }
  auto retained_result = pager->ReadPage(modern_sqlite::PageNumber{1});
  if (!retained_result.has_value()) {
    return std::nullopt;
  }
  std::optional<modern_sqlite::ReadPagePin> retained;
  retained.emplace(std::move(*retained_result));
  if (!pager->BeginWrite().has_value()) {
    return std::nullopt;
  }
  {
    auto page = pager->WritePage(modern_sqlite::PageNumber{2});
    if (!page.has_value()) {
      return std::nullopt;
    }
    page->mutable_bytes()[100] = std::byte{0x7f};
  }

  allocation_count.store(0, std::memory_order_relaxed);
  {
    const auto page = pager->ReadPage(modern_sqlite::PageNumber{1});
    if (!page.has_value()) {
      return std::nullopt;
    }
  }
  return allocation_count.load(std::memory_order_relaxed);
}

[[nodiscard]] std::optional<std::size_t> CommitAllocations() {
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 2);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return std::nullopt;
  }
  {
    auto page = pager->WritePage(modern_sqlite::PageNumber{2});
    if (!page.has_value()) {
      return std::nullopt;
    }
    page->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    const auto page = pager->WritePage(modern_sqlite::PageNumber{1});
    if (!page.has_value()) {
      return std::nullopt;
    }
  }

  allocation_count.store(0, std::memory_order_relaxed);
  const auto committed = pager->Commit();
  const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
  return committed.has_value() ? std::optional<std::size_t>{allocations} : std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> EphemeralPressureSpillAllocations() {
  modern_sqlite::test::WritePagerFixedVfs vfs;
  auto opened = modern_sqlite::Pager::OpenEphemeral(
      vfs, modern_sqlite::PagerOptions{
               .empty_database_page_size =
                   modern_sqlite::ByteCount{modern_sqlite::test::kWritePagerPageSize},
               .cache_capacity_pages = 1,
           });
  if (!opened.has_value()) {
    return std::nullopt;
  }
  std::unique_ptr<modern_sqlite::Pager> pager = std::move(*opened);
  if (!pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return std::nullopt;
  }
  {
    auto page = pager->AllocatePage();
    if (!page.has_value()) {
      return std::nullopt;
    }
    page->mutable_bytes()[100] = std::byte{0x11};
  }
  auto retained_result = pager->ReadPage(modern_sqlite::PageNumber{1});
  if (!retained_result.has_value()) {
    return std::nullopt;
  }
  std::optional<modern_sqlite::ReadPagePin> retained;
  retained.emplace(std::move(*retained_result));
  {
    auto page = pager->AllocatePage();
    if (!page.has_value()) {
      return std::nullopt;
    }
    page->mutable_bytes()[100] = std::byte{0x22};
  }

  allocation_count.store(0, std::memory_order_relaxed);
  const auto page = pager->ReadPage(modern_sqlite::PageNumber{1});
  const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
  return page.has_value() && page->frame().bytes()[100] == std::byte{0x11}
             ? std::optional<std::size_t>{allocations}
             : std::nullopt;
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
  const std::optional<std::size_t> repeated = RepeatedWriteAllocations();
  if (!repeated.has_value() || *repeated != 0U) {
    return 1;
  }
  const std::optional<std::size_t> spill = PressureSpillAllocations();
  if (!spill.has_value() || *spill != 0U) {
    return 2;
  }
  const std::optional<std::size_t> commit = CommitAllocations();
  if (!commit.has_value() || *commit != 0U) {
    return 3;
  }
  const std::optional<std::size_t> ephemeral_spill = EphemeralPressureSpillAllocations();
  if (!ephemeral_spill.has_value() || *ephemeral_spill != 0U) {
    return 4;
  }
  return 0;
}
