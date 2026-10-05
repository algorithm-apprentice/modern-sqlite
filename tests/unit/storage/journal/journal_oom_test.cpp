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

std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation.has_value() && index == *failing_allocation) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation.has_value() && index == *failing_allocation) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

class FixedPlayback final : public modern_sqlite::JournalPlayback {
 public:
  FixedPlayback(modern_sqlite::JournalPlaybackKind kind, const std::array<std::byte, 512>& page,
                modern_sqlite::PageNumber page_number = modern_sqlite::PageNumber{1}) noexcept
      : info_{
            .kind = kind,
            .page_size = modern_sqlite::ByteCount{512},
            .original_page_count = 8,
        },
        page_(page),
        page_number_(page_number) {}

  [[nodiscard]] std::optional<modern_sqlite::JournalPlaybackInfo> info() const noexcept override {
    return info_;
  }

  [[nodiscard]] modern_sqlite::Result<std::optional<modern_sqlite::JournalPageImage>> Next()
      override {
    if (emitted_) {
      return std::optional<modern_sqlite::JournalPageImage>{};
    }
    emitted_ = true;
    return std::optional<modern_sqlite::JournalPageImage>{modern_sqlite::JournalPageImage{
        .page_number = page_number_,
        .bytes = page_,
    }};
  }

 private:
  modern_sqlite::JournalPlaybackInfo info_;
  modern_sqlite::ByteView page_;
  modern_sqlite::PageNumber page_number_;
  bool emitted_ = false;
};

class NoopTarget final : public modern_sqlite::JournalRecoveryTarget {
 public:
  [[nodiscard]] modern_sqlite::Status PreparePlayback(modern_sqlite::JournalPlaybackInfo) override {
    ++prepare_count;
    return {};
  }

  [[nodiscard]] modern_sqlite::Status ResizeDatabase(std::uint32_t) override {
    ++resize_count;
    return {};
  }

  [[nodiscard]] modern_sqlite::Status RestorePage(modern_sqlite::JournalPageImage) override {
    ++restore_count;
    return {};
  }

  [[nodiscard]] modern_sqlite::Status SyncDatabase() override {
    ++sync_count;
    return {};
  }

  [[nodiscard]] modern_sqlite::Status CompletePlayback(
      modern_sqlite::JournalPlaybackInfo) override {
    ++complete_count;
    return {};
  }

  int prepare_count = 0;
  int resize_count = 0;
  int restore_count = 0;
  int sync_count = 0;
  int complete_count = 0;
};

class NoopBackend final : public modern_sqlite::JournalBackend {
 public:
  void SetPlayback(std::unique_ptr<modern_sqlite::JournalPlayback> playback) noexcept {
    playback_ = std::move(playback);
  }

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
    if (playback_ != nullptr) {
      return std::move(playback_);
    }
    return std::unexpected(modern_sqlite::Error::OutOfMemory());
  }

  [[nodiscard]] modern_sqlite::Status DoCompleteSavepointPlayback(
      modern_sqlite::JournalSavepoint) override {
    return {};
  }

  [[nodiscard]] modern_sqlite::Status DoSync() override { return {}; }

  [[nodiscard]] modern_sqlite::Result<std::unique_ptr<modern_sqlite::JournalPlayback>>
  DoOpenTransactionPlayback() override {
    if (playback_ != nullptr) {
      return std::move(playback_);
    }
    return std::unexpected(modern_sqlite::Error::OutOfMemory());
  }

  [[nodiscard]] modern_sqlite::Status DoPrepareHotRecovery() override { return {}; }

  [[nodiscard]] modern_sqlite::Result<std::unique_ptr<modern_sqlite::JournalPlayback>>
  DoOpenHotPlayback() override {
    if (playback_ != nullptr) {
      return std::move(playback_);
    }
    return std::unexpected(modern_sqlite::Error::OutOfMemory());
  }

  [[nodiscard]] modern_sqlite::Status DoFinalizeCommit() override { return {}; }
  [[nodiscard]] modern_sqlite::Status DoFinalizeRollback() override { return {}; }

 private:
  std::unique_ptr<modern_sqlite::JournalPlayback> playback_;
};

[[nodiscard]] modern_sqlite::Result<std::unique_ptr<modern_sqlite::JournalTransaction>> Begin(
    NoopBackend& backend) {
  return modern_sqlite::JournalTransaction::Begin(backend,
                                                  modern_sqlite::JournalTransactionInfo{
                                                      .page_size = modern_sqlite::ByteCount{512},
                                                      .sector_size = modern_sqlite::ByteCount{512},
                                                      .original_page_count = 8,
                                                  });
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
  {
    NoopBackend backend;
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    const auto begun = Begin(backend);
    failing_allocation.reset();
    if (begun.has_value() || begun.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return 1;
    }
  }

  NoopBackend measuring_backend;
  auto measured_begin = Begin(measuring_backend);
  if (!measured_begin.has_value()) {
    return 1;
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> measured = std::move(*measured_begin);
  const std::array<std::byte, 512> page{};
  allocation_index.store(0, std::memory_order_relaxed);
  const auto baseline_capture = measured->CapturePage(modern_sqlite::JournalPageImage{
      .page_number = modern_sqlite::PageNumber{1},
      .bytes = page,
  });
  if (!baseline_capture.has_value()) {
    return 1;
  }
  const std::size_t capture_allocations = allocation_index.load(std::memory_order_relaxed);
  if (capture_allocations == 0) {
    return 1;
  }

  for (std::size_t failure = 0; failure < capture_allocations; ++failure) {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 1;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    const auto captured = transaction->CapturePage(modern_sqlite::JournalPageImage{
        .page_number = modern_sqlite::PageNumber{1},
        .bytes = page,
    });
    failing_allocation.reset();
    if (captured.has_value() || captured.error().code() != modern_sqlite::ErrorCode::kOutOfMemory ||
        transaction->state() != modern_sqlite::JournalTransactionState::kFailed) {
      return 1;
    }
  }

  NoopBackend savepoint_backend;
  auto savepoint_begin = Begin(savepoint_backend);
  if (!savepoint_begin.has_value()) {
    return 1;
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> savepoint_transaction =
      std::move(*savepoint_begin);
  allocation_index.store(0, std::memory_order_relaxed);
  const auto baseline_savepoint = savepoint_transaction->CreateSavepoint(8);
  if (!baseline_savepoint.has_value()) {
    return 1;
  }
  const std::size_t savepoint_allocations = allocation_index.load(std::memory_order_relaxed);
  if (savepoint_allocations == 0) {
    return 1;
  }

  for (std::size_t failure = 0; failure < savepoint_allocations; ++failure) {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 1;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    const auto savepoint = transaction->CreateSavepoint(8);
    failing_allocation.reset();
    if (savepoint.has_value() ||
        savepoint.error().code() != modern_sqlite::ErrorCode::kOutOfMemory ||
        transaction->state() != modern_sqlite::JournalTransactionState::kFailed) {
      return 1;
    }
  }

  {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 1;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    const auto savepoint = transaction->CreateSavepoint(8);
    if (!savepoint.has_value()) {
      return 1;
    }
    backend.SetPlayback(std::make_unique<FixedPlayback>(
        modern_sqlite::JournalPlaybackKind::kSavepointRollback, page));
    NoopTarget target;
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, target);
    failing_allocation.reset();
    if (rolled_back.has_value() ||
        rolled_back.error().code() != modern_sqlite::ErrorCode::kOutOfMemory ||
        transaction->state() != modern_sqlite::JournalTransactionState::kFailed ||
        target.restore_count != 0) {
      return 1;
    }
  }

  {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 1;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    const auto savepoint = transaction->CreateSavepoint(8);
    if (!savepoint.has_value()) {
      return 1;
    }
    backend.SetPlayback(
        std::make_unique<FixedPlayback>(modern_sqlite::JournalPlaybackKind::kSavepointRollback,
                                        page, modern_sqlite::PageNumber{9}));
    NoopTarget target;
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, target);
    failing_allocation.reset();
    if (!rolled_back.has_value() || allocation_index.load(std::memory_order_relaxed) != 0 ||
        target.restore_count != 0 || target.complete_count != 1) {
      return 1;
    }
  }

  {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 1;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    backend.SetPlayback(std::make_unique<FixedPlayback>(
        modern_sqlite::JournalPlaybackKind::kTransactionRollback, page));
    NoopTarget target;
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    const auto rolled_back = transaction->Rollback(target);
    failing_allocation.reset();
    if (!rolled_back.has_value() || allocation_index.load(std::memory_order_relaxed) != 0 ||
        target.restore_count != 1 || target.sync_count != 1 || target.complete_count != 1) {
      return 1;
    }
  }

  {
    NoopBackend backend;
    backend.SetPlayback(
        std::make_unique<FixedPlayback>(modern_sqlite::JournalPlaybackKind::kHotRecovery, page));
    NoopTarget target;
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    const auto recovered = modern_sqlite::RecoverHotJournal(backend, target);
    failing_allocation.reset();
    if (!recovered.has_value() || allocation_index.load(std::memory_order_relaxed) != 0 ||
        target.restore_count != 1 || target.sync_count != 1 || target.complete_count != 1) {
      return 1;
    }
  }
  return 0;
}
