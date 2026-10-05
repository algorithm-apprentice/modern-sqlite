#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "modern_sqlite/storage/journal/rollback_journal.hpp"
#include "rollback_journal_test_support.hpp"

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

class NoopBackend final : public modern_sqlite::JournalBackend {
 protected:
  [[nodiscard]] modern_sqlite::Status DoBegin(modern_sqlite::JournalTransactionInfo) override {
    return {};
  }

  [[nodiscard]] modern_sqlite::Status DoAppendTransactionPage(
      modern_sqlite::JournalPageImage) override {
    return {};
  }

  [[nodiscard]] modern_sqlite::Status DoAppendSavepointPage(
      modern_sqlite::JournalPageImage) override {
    return {};
  }

  [[nodiscard]] modern_sqlite::Status DoCreateSavepoint(modern_sqlite::JournalSavepoint) override {
    return {};
  }

  [[nodiscard]] modern_sqlite::Status DoReleaseSavepoint(modern_sqlite::JournalSavepointId,
                                                         bool) override {
    return {};
  }

  [[nodiscard]] modern_sqlite::Result<std::unique_ptr<modern_sqlite::JournalPlayback>>
  DoOpenSavepointPlayback(modern_sqlite::JournalSavepoint) override {
    return std::unexpected(modern_sqlite::Error::OutOfMemory());
  }

  [[nodiscard]] modern_sqlite::Status DoCompleteSavepointPlayback(
      modern_sqlite::JournalSavepoint) override {
    return {};
  }

  [[nodiscard]] modern_sqlite::Status DoSync() override { return {}; }

  [[nodiscard]] modern_sqlite::Result<std::unique_ptr<modern_sqlite::JournalPlayback>>
  DoOpenTransactionPlayback() override {
    return std::unexpected(modern_sqlite::Error::OutOfMemory());
  }

  [[nodiscard]] modern_sqlite::Status DoPrepareHotRecovery() override { return {}; }

  [[nodiscard]] modern_sqlite::Result<std::unique_ptr<modern_sqlite::JournalPlayback>>
  DoOpenHotPlayback() override {
    return std::unexpected(modern_sqlite::Error::OutOfMemory());
  }

  [[nodiscard]] modern_sqlite::Status DoFinalizeCommit() override { return {}; }
  [[nodiscard]] modern_sqlite::Status DoFinalizeRollback() override { return {}; }
};

[[nodiscard]] std::optional<std::size_t> FirstCaptureAllocations(
    modern_sqlite::JournalBackend& backend) {
  auto begun = modern_sqlite::JournalTransaction::Begin(
      backend, modern_sqlite::test::FixedMemoryVfs::TransactionInfo());
  if (!begun.has_value()) {
    return std::nullopt;
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
  const std::array<std::byte, modern_sqlite::test::kTestPageSize> page{};
  allocation_count.store(0, std::memory_order_relaxed);
  const auto captured = transaction->CapturePage(modern_sqlite::JournalPageImage{
      .page_number = modern_sqlite::PageNumber{1},
      .bytes = page,
  });
  const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
  if (!captured.has_value()) {
    return std::nullopt;
  }
  return allocations;
}

[[nodiscard]] std::optional<std::size_t> HotPlaybackAllocations(std::uint32_t record_count) {
  modern_sqlite::test::FixedMemoryVfs vfs;
  if (!vfs.LoadHotJournal(record_count)) {
    return std::nullopt;
  }
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return std::nullopt;
  }
  modern_sqlite::test::CountingRecoveryTarget target;
  allocation_count.store(0, std::memory_order_relaxed);
  const auto recovered = modern_sqlite::RecoverHotJournal(**created, target);
  const std::size_t allocations = allocation_count.load(std::memory_order_relaxed);
  if (!recovered.has_value() || target.restore_count != record_count) {
    return std::nullopt;
  }
  return allocations;
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
  NoopBackend noop_backend;
  const std::optional<std::size_t> noop_capture = FirstCaptureAllocations(noop_backend);
  if (!noop_capture.has_value()) {
    return 1;
  }

  modern_sqlite::test::FixedMemoryVfs vfs;
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return 2;
  }
  const std::optional<std::size_t> rollback_capture = FirstCaptureAllocations(**created);
  if (!rollback_capture.has_value() || *rollback_capture != *noop_capture) {
    return 3;
  }

  const std::optional<std::size_t> one_record = HotPlaybackAllocations(1);
  const std::optional<std::size_t> many_records = HotPlaybackAllocations(64);
  if (!one_record.has_value() || !many_records.has_value() || *one_record != *many_records) {
    return 4;
  }
  return 0;
}
