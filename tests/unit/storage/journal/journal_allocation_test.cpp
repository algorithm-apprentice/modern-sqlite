#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "modern_sqlite/storage/journal/journal.hpp"

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

  [[nodiscard]] modern_sqlite::Status DoReleaseSavepoint(
      modern_sqlite::JournalSavepointId) override {
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
  NoopBackend backend;
  auto begun = modern_sqlite::JournalTransaction::Begin(
      backend, modern_sqlite::JournalTransactionInfo{
                   .page_size = modern_sqlite::ByteCount{512},
                   .sector_size = modern_sqlite::ByteCount{512},
                   .original_page_count = 1,
               });
  if (!begun.has_value()) {
    return 1;
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
  const std::array<std::byte, 512> page{};
  if (!transaction
           ->CapturePage(modern_sqlite::JournalPageImage{
               .page_number = modern_sqlite::PageNumber{1},
               .bytes = page,
           })
           .has_value() ||
      !transaction->SyncJournal().has_value()) {
    return 1;
  }

  const std::size_t before = allocation_count.load(std::memory_order_relaxed);
  for (std::size_t iteration = 0; iteration < 10'000; ++iteration) {
    if (!transaction
             ->CapturePage(modern_sqlite::JournalPageImage{
                 .page_number = modern_sqlite::PageNumber{1},
                 .bytes = page,
             })
             .has_value() ||
        !transaction->SyncJournal().has_value() ||
        !transaction->AuthorizeDatabaseWrite(modern_sqlite::PageNumber{1}).has_value() ||
        !transaction->MarkDatabaseSynced().has_value()) {
      return 1;
    }
  }
  const std::size_t after = allocation_count.load(std::memory_order_relaxed);
  return after == before ? 0 : 1;
}
