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

std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
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
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

struct ScenarioOutcome {
  std::size_t allocations = 0;
  bool succeeded = false;
  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool invariant_holds = false;
};

void Arm(std::optional<std::size_t> failure) noexcept {
  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = failure;
}

[[nodiscard]] std::size_t Disarm() noexcept {
  failing_allocation.reset();
  return allocation_index.load(std::memory_order_relaxed);
}

template <typename Runner>
[[nodiscard]] bool ExhaustAllocations(Runner&& runner) {
  const ScenarioOutcome baseline = runner(std::nullopt);
  if (!baseline.succeeded || !baseline.invariant_holds || baseline.allocations == 0U) {
    return false;
  }
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const ScenarioOutcome outcome = runner(failure);
    if (outcome.succeeded || outcome.error != modern_sqlite::ErrorCode::kOutOfMemory ||
        !outcome.invariant_holds) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] modern_sqlite::ErrorCode ErrorCodeOf(const modern_sqlite::Status& status) noexcept {
  return status.has_value() ? modern_sqlite::ErrorCode::kGeneric : status.error().code();
}

template <typename T>
[[nodiscard]] modern_sqlite::ErrorCode ErrorCodeOf(
    const modern_sqlite::Result<T>& result) noexcept {
  return result.has_value() ? modern_sqlite::ErrorCode::kGeneric : result.error().code();
}

[[nodiscard]] bool HasOriginalDatabase(
    const modern_sqlite::test::WritePagerFixedVfs& vfs) noexcept {
  const modern_sqlite::ByteView bytes = vfs.database_bytes();
  return bytes.size() == modern_sqlite::test::kWritePagerPageSize * 2U &&
         bytes[modern_sqlite::test::kWritePagerPageSize + 100U] == std::byte{2};
}

[[nodiscard]] ScenarioOutcome RunOpen(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  Arm(failure);
  const auto opened = modern_sqlite::Pager::OpenWritable(
      vfs, modern_sqlite::test::kWritePagerInputPath,
      modern_sqlite::WritablePagerOptions{
          .pager =
              modern_sqlite::PagerOptions{
                  .empty_database_page_size =
                      modern_sqlite::ByteCount{modern_sqlite::test::kWritePagerPageSize},
                  .cache_capacity_pages = 4,
              },
          .journal =
              modern_sqlite::RollbackJournalOptions{
                  .legacy_page_size =
                      modern_sqlite::ByteCount{modern_sqlite::test::kWritePagerPageSize},
              },
      });
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(opened);
  bool reusable = true;
  if (!opened.has_value()) {
    reusable = modern_sqlite::test::OpenWritePager(vfs, 4) != nullptr;
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = opened.has_value(),
      .error = error,
      .invariant_holds = reusable,
  };
}

[[nodiscard]] ScenarioOutcome RunBeginRead(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 4);
  if (pager == nullptr) {
    return {};
  }

  Arm(failure);
  const auto begun = pager->BeginRead();
  const std::size_t allocations = Disarm();
  bool retryable = begun.has_value();
  if (!begun.has_value()) {
    retryable = pager->state() == modern_sqlite::PagerState::kOpen &&
                vfs.database_lock() == modern_sqlite::DatabaseLock::kNone &&
                pager->BeginRead().has_value();
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = begun.has_value(),
      .error = ErrorCodeOf(begun),
      .invariant_holds = retryable,
  };
}

[[nodiscard]] ScenarioOutcome RunReadPage(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 4);
  if (pager == nullptr || !pager->BeginRead().has_value()) {
    return {};
  }

  Arm(failure);
  auto page = pager->ReadPage(modern_sqlite::PageNumber{2});
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(page);
  bool retryable = page.has_value() && page->frame().bytes()[100] == std::byte{2};
  page = std::unexpected(
      modern_sqlite::Error::Create(modern_sqlite::ErrorCode::kGeneric, "release test pin"));
  if (error != modern_sqlite::ErrorCode::kGeneric) {
    auto retried = pager->ReadPage(modern_sqlite::PageNumber{2});
    retryable = retried.has_value() && retried->frame().bytes()[100] == std::byte{2};
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = error == modern_sqlite::ErrorCode::kGeneric,
      .error = error,
      .invariant_holds = retryable && pager->state() == modern_sqlite::PagerState::kReader,
  };
}

[[nodiscard]] ScenarioOutcome RunFirstWrite(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 4);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }

  Arm(failure);
  auto page = pager->WritePage(modern_sqlite::PageNumber{2});
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(page);
  page = std::unexpected(
      modern_sqlite::Error::Create(modern_sqlite::ErrorCode::kGeneric, "release test pin"));
  const bool rolled_back = pager->Rollback().has_value();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = error == modern_sqlite::ErrorCode::kGeneric,
      .error = error,
      .invariant_holds = rolled_back && pager->state() == modern_sqlite::PagerState::kReader &&
                         HasOriginalDatabase(vfs),
  };
}

[[nodiscard]] ScenarioOutcome RunCommitWithPageOneMiss(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 1);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  {
    auto page = pager->WritePage(modern_sqlite::PageNumber{2});
    if (!page.has_value()) {
      return {};
    }
    page->mutable_bytes()[100] = std::byte{0x7f};
  }

  Arm(failure);
  const auto committed = pager->Commit();
  const std::size_t allocations = Disarm();
  bool invariant =
      committed.has_value() && pager->state() == modern_sqlite::PagerState::kReader &&
      vfs.database_bytes()[modern_sqlite::test::kWritePagerPageSize + 100U] == std::byte{0x7f} &&
      !vfs.journal_present();
  if (!committed.has_value()) {
    invariant = pager->Rollback().has_value() && HasOriginalDatabase(vfs);
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = committed.has_value(),
      .error = ErrorCodeOf(committed),
      .invariant_holds = invariant,
  };
}

[[nodiscard]] ScenarioOutcome RunAllocatePage(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 4);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }

  Arm(failure);
  auto page = pager->AllocatePage();
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(page);
  page = std::unexpected(
      modern_sqlite::Error::Create(modern_sqlite::ErrorCode::kGeneric, "release test pin"));
  const bool rolled_back = pager->Rollback().has_value();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = error == modern_sqlite::ErrorCode::kGeneric,
      .error = error,
      .invariant_holds = rolled_back && pager->page_count() == 2U && HasOriginalDatabase(vfs),
  };
}

[[nodiscard]] ScenarioOutcome RunCreateSavepoint(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 4);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }

  Arm(failure);
  const auto savepoint = pager->CreateSavepoint();
  const std::size_t allocations = Disarm();
  const bool rolled_back = pager->Rollback().has_value();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = savepoint.has_value(),
      .error = ErrorCodeOf(savepoint),
      .invariant_holds = rolled_back && HasOriginalDatabase(vfs),
  };
}

[[nodiscard]] ScenarioOutcome RunSavepointRollback(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 4);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  {
    auto page = pager->WritePage(modern_sqlite::PageNumber{2});
    if (!page.has_value()) {
      return {};
    }
    page->mutable_bytes()[100] = std::byte{0x33};
  }
  const auto savepoint = pager->CreateSavepoint();
  if (!savepoint.has_value()) {
    return {};
  }
  {
    auto page = pager->WritePage(modern_sqlite::PageNumber{2});
    if (!page.has_value()) {
      return {};
    }
    page->mutable_bytes()[100] = std::byte{0x7f};
  }

  Arm(failure);
  const auto restored = pager->RollbackToSavepoint(*savepoint);
  const std::size_t allocations = Disarm();
  const bool rolled_back = pager->Rollback().has_value();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = restored.has_value(),
      .error = ErrorCodeOf(restored),
      .invariant_holds = rolled_back && HasOriginalDatabase(vfs),
  };
}

[[nodiscard]] bool PrepareSpilledWrite(modern_sqlite::Pager& pager) {
  {
    auto page = pager.WritePage(modern_sqlite::PageNumber{2});
    if (!page.has_value()) {
      return false;
    }
    page->mutable_bytes()[100] = std::byte{0x7f};
  }
  {
    const auto page = pager.WritePage(modern_sqlite::PageNumber{1});
    if (!page.has_value()) {
      return false;
    }
  }
  return pager.ReadPage(modern_sqlite::PageNumber{1}).has_value();
}

[[nodiscard]] ScenarioOutcome RunFullRollback(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 1);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !PrepareSpilledWrite(*pager)) {
    return {};
  }

  Arm(failure);
  const auto restored = pager->Rollback();
  const std::size_t allocations = Disarm();
  bool invariant = restored.has_value() && HasOriginalDatabase(vfs);
  if (!restored.has_value() && pager->state() != modern_sqlite::PagerState::kError) {
    invariant = pager->Rollback().has_value() && HasOriginalDatabase(vfs);
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = restored.has_value(),
      .error = ErrorCodeOf(restored),
      .invariant_holds = invariant,
  };
}

[[nodiscard]] ScenarioOutcome RunHotRecovery(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  {
    std::unique_ptr<modern_sqlite::Pager> crashed = modern_sqlite::test::OpenWritePager(vfs, 1);
    if (crashed == nullptr || !crashed->BeginRead().has_value() ||
        !crashed->BeginWrite().has_value() || !PrepareSpilledWrite(*crashed)) {
      return {};
    }
  }
  if (!vfs.journal_present()) {
    return {};
  }
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 4);
  if (pager == nullptr) {
    return {};
  }

  Arm(failure);
  const auto recovered = pager->BeginRead();
  const std::size_t allocations = Disarm();
  bool invariant = recovered.has_value() && HasOriginalDatabase(vfs) && !vfs.journal_present();
  if (!recovered.has_value()) {
    if (pager->state() == modern_sqlite::PagerState::kOpen) {
      invariant =
          pager->BeginRead().has_value() && HasOriginalDatabase(vfs) && !vfs.journal_present();
    } else {
      pager.reset();
      std::unique_ptr<modern_sqlite::Pager> retry = modern_sqlite::test::OpenWritePager(vfs, 4);
      invariant = retry != nullptr && retry->BeginRead().has_value() && HasOriginalDatabase(vfs) &&
                  !vfs.journal_present();
    }
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = recovered.has_value(),
      .error = ErrorCodeOf(recovered),
      .invariant_holds = invariant,
  };
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
  if (!ExhaustAllocations(RunOpen)) {
    return 1;
  }
  if (!ExhaustAllocations(RunBeginRead)) {
    return 2;
  }
  if (!ExhaustAllocations(RunReadPage)) {
    return 3;
  }
  if (!ExhaustAllocations(RunFirstWrite)) {
    return 4;
  }
  if (!ExhaustAllocations(RunCommitWithPageOneMiss)) {
    return 5;
  }
  if (!ExhaustAllocations(RunAllocatePage)) {
    return 6;
  }
  if (!ExhaustAllocations(RunCreateSavepoint)) {
    return 7;
  }
  if (!ExhaustAllocations(RunSavepointRollback)) {
    return 8;
  }
  if (!ExhaustAllocations(RunFullRollback)) {
    return 9;
  }
  if (!ExhaustAllocations(RunHotRecovery)) {
    return 10;
  }
  return 0;
} catch (...) {
  failing_allocation.reset();
  return 11;
}
