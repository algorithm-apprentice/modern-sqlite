#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "modern_sqlite/storage/journal/journal.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;

constexpr std::size_t kNodeBytes = 512U;
constexpr std::size_t kHeaderBytes = 3U * sizeof(std::uint32_t);
constexpr std::size_t kUnionBytes = ((kNodeBytes - kHeaderBytes) / sizeof(void*)) * sizeof(void*);
constexpr std::size_t kHashSlots = kUnionBytes / sizeof(std::uint32_t);
constexpr std::size_t kMaximumHashEntries = kHashSlots / 2U;
constexpr std::uint32_t kLargePageCount = 1'000'000U;
constexpr modern_sqlite::PageNumber kCollidingPage{static_cast<std::uint32_t>(kHashSlots + 1U)};

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

class SequencePlayback final : public modern_sqlite::JournalPlayback {
 public:
  SequencePlayback(modern_sqlite::JournalPlaybackKind kind, const std::array<std::byte, 512>& page,
                   std::vector<modern_sqlite::PageNumber> page_numbers,
                   std::uint32_t original_page_count) noexcept
      : info_{
            .kind = kind,
            .page_size = modern_sqlite::ByteCount{512},
            .original_page_count = original_page_count,
        },
        page_(page),
        page_numbers_(std::move(page_numbers)) {}

  [[nodiscard]] std::optional<modern_sqlite::JournalPlaybackInfo> info() const noexcept override {
    return info_;
  }

  [[nodiscard]] modern_sqlite::Result<std::optional<modern_sqlite::JournalPageImage>> Next()
      override {
    if (index_ == page_numbers_.size()) {
      return std::optional<modern_sqlite::JournalPageImage>{};
    }
    return std::optional<modern_sqlite::JournalPageImage>{modern_sqlite::JournalPageImage{
        .page_number = page_numbers_[index_++],
        .bytes = page_,
    }};
  }

 private:
  modern_sqlite::JournalPlaybackInfo info_;
  modern_sqlite::ByteView page_;
  std::vector<modern_sqlite::PageNumber> page_numbers_;
  std::size_t index_ = 0;
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

  std::size_t prepare_count = 0;
  std::size_t resize_count = 0;
  std::size_t restore_count = 0;
  std::size_t sync_count = 0;
  std::size_t complete_count = 0;
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

  [[nodiscard]] modern_sqlite::Status DoReleaseSavepoint(modern_sqlite::JournalSavepointId,
                                                         bool) override {
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
    NoopBackend& backend, std::uint32_t original_page_count = 8U) {
  return modern_sqlite::JournalTransaction::Begin(backend,
                                                  modern_sqlite::JournalTransactionInfo{
                                                      .page_size = modern_sqlite::ByteCount{512},
                                                      .sector_size = modern_sqlite::ByteCount{512},
                                                      .original_page_count = original_page_count,
                                                  });
}

[[nodiscard]] bool SeedSparseMembership(modern_sqlite::JournalTransaction& transaction,
                                        const std::array<std::byte, 512>& page) {
  for (std::uint32_t page_number = 1U;
       page_number <= static_cast<std::uint32_t>(kMaximumHashEntries + 1U); ++page_number) {
    if (!transaction
             .CapturePage(modern_sqlite::JournalPageImage{
                 .page_number = modern_sqlite::PageNumber{page_number},
                 .bytes = page,
             })
             .has_value()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::vector<modern_sqlite::PageNumber> SubdivisionPages() {
  std::vector<modern_sqlite::PageNumber> pages;
  pages.reserve(kMaximumHashEntries + 2U);
  for (std::uint32_t page = 1U; page <= static_cast<std::uint32_t>(kMaximumHashEntries + 1U);
       ++page) {
    pages.emplace_back(page);
  }
  pages.push_back(kCollidingPage);
  return pages;
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
  NoopBackend begin_backend;
  allocation_index.store(0, std::memory_order_relaxed);
  auto baseline_begin = Begin(begin_backend);
  if (!baseline_begin.has_value()) {
    return 1;
  }
  const std::size_t begin_allocations = allocation_index.load(std::memory_order_relaxed);
  if (begin_allocations == 0U) {
    return 2;
  }
  baseline_begin->reset();

  for (std::size_t failure = 0; failure < begin_allocations; ++failure) {
    NoopBackend backend;
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    const auto begun = Begin(backend);
    failing_allocation.reset();
    if (begun.has_value() || begun.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return 3;
    }
  }

  NoopBackend measuring_backend;
  auto measured_begin = Begin(measuring_backend);
  if (!measured_begin.has_value()) {
    return 4;
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> measured = std::move(*measured_begin);
  const std::array<std::byte, 512> page{};
  allocation_index.store(0, std::memory_order_relaxed);
  const auto baseline_capture = measured->CapturePage(modern_sqlite::JournalPageImage{
      .page_number = modern_sqlite::PageNumber{1},
      .bytes = page,
  });
  if (!baseline_capture.has_value()) {
    return 5;
  }
  const std::size_t capture_allocations = allocation_index.load(std::memory_order_relaxed);
  if (capture_allocations != 0U) {
    return 6;
  }

  NoopBackend subdivision_backend;
  auto subdivision_begin = Begin(subdivision_backend, kLargePageCount);
  if (!subdivision_begin.has_value() || !SeedSparseMembership(**subdivision_begin, page)) {
    return 26;
  }
  allocation_index.store(0, std::memory_order_relaxed);
  const auto baseline_subdivision = (*subdivision_begin)
                                        ->CapturePage(modern_sqlite::JournalPageImage{
                                            .page_number = kCollidingPage,
                                            .bytes = page,
                                        });
  const std::size_t subdivision_allocations = allocation_index.load(std::memory_order_relaxed);
  if (!baseline_subdivision.has_value() || subdivision_allocations == 0U) {
    return 27;
  }

  for (std::size_t failure = 0; failure < subdivision_allocations; ++failure) {
    NoopBackend backend;
    auto begun = Begin(backend, kLargePageCount);
    if (!begun.has_value() || !SeedSparseMembership(**begun, page)) {
      return 28;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    const auto captured = (*begun)->CapturePage(modern_sqlite::JournalPageImage{
        .page_number = kCollidingPage,
        .bytes = page,
    });
    failing_allocation.reset();
    if (captured.has_value() || captured.error().code() != modern_sqlite::ErrorCode::kOutOfMemory ||
        (*begun)->state() != modern_sqlite::JournalTransactionState::kFailed) {
      return 29;
    }
  }

  NoopBackend savepoint_backend;
  auto savepoint_begin = Begin(savepoint_backend);
  if (!savepoint_begin.has_value()) {
    return 7;
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> savepoint_transaction =
      std::move(*savepoint_begin);
  allocation_index.store(0, std::memory_order_relaxed);
  const auto baseline_savepoint = savepoint_transaction->CreateSavepoint(8);
  if (!baseline_savepoint.has_value()) {
    return 8;
  }
  const std::size_t savepoint_allocations = allocation_index.load(std::memory_order_relaxed);
  if (savepoint_allocations == 0) {
    return 9;
  }

  for (std::size_t failure = 0; failure < savepoint_allocations; ++failure) {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 10;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    const auto savepoint = transaction->CreateSavepoint(8);
    failing_allocation.reset();
    if (savepoint.has_value() ||
        savepoint.error().code() != modern_sqlite::ErrorCode::kOutOfMemory ||
        transaction->state() != modern_sqlite::JournalTransactionState::kFailed) {
      return 11;
    }
  }

  {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 12;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    const auto savepoint = transaction->CreateSavepoint(8);
    if (!savepoint.has_value()) {
      return 13;
    }
    backend.SetPlayback(std::make_unique<FixedPlayback>(
        modern_sqlite::JournalPlaybackKind::kSavepointRollback, page));
    NoopTarget target;
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, target);
    failing_allocation.reset();
    if (rolled_back.has_value()) {
      return 14;
    }
    if (rolled_back.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return 21;
    }
    if (transaction->state() != modern_sqlite::JournalTransactionState::kFailed) {
      return 22;
    }
    if (target.prepare_count != 0) {
      return 23;
    }
    if (target.resize_count != 0) {
      return 24;
    }
    if (target.restore_count != 0) {
      return 25;
    }
  }

  {
    NoopBackend backend;
    auto begun = Begin(backend, kLargePageCount);
    if (!begun.has_value()) {
      return 30;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    const auto savepoint = transaction->CreateSavepoint(kLargePageCount);
    if (!savepoint.has_value()) {
      return 31;
    }
    backend.SetPlayback(
        std::make_unique<SequencePlayback>(modern_sqlite::JournalPlaybackKind::kSavepointRollback,
                                           page, SubdivisionPages(), kLargePageCount));
    NoopTarget target;
    allocation_index.store(0, std::memory_order_relaxed);
    const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, target);
    const std::size_t playback_allocations = allocation_index.load(std::memory_order_relaxed);
    if (!rolled_back.has_value() || playback_allocations == 0U ||
        target.restore_count != kMaximumHashEntries + 2U || target.complete_count != 1) {
      return 32;
    }

    for (std::size_t failure = 0; failure < playback_allocations; ++failure) {
      NoopBackend failing_backend;
      auto failing_begin = Begin(failing_backend, kLargePageCount);
      if (!failing_begin.has_value()) {
        return 33;
      }
      std::unique_ptr<modern_sqlite::JournalTransaction> failing_transaction =
          std::move(*failing_begin);
      const auto failing_savepoint = failing_transaction->CreateSavepoint(kLargePageCount);
      if (!failing_savepoint.has_value()) {
        return 34;
      }
      failing_backend.SetPlayback(
          std::make_unique<SequencePlayback>(modern_sqlite::JournalPlaybackKind::kSavepointRollback,
                                             page, SubdivisionPages(), kLargePageCount));
      NoopTarget failing_target;
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = failure;
      const auto failed =
          failing_transaction->RollbackToSavepoint(*failing_savepoint, failing_target);
      failing_allocation.reset();
      if (failed.has_value() || failed.error().code() != modern_sqlite::ErrorCode::kOutOfMemory ||
          failing_transaction->state() != modern_sqlite::JournalTransactionState::kFailed ||
          failing_target.complete_count != 0) {
        return 35;
      }
    }
  }

  {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 15;
    }
    std::unique_ptr<modern_sqlite::JournalTransaction> transaction = std::move(*begun);
    const auto savepoint = transaction->CreateSavepoint(8);
    if (!savepoint.has_value()) {
      return 16;
    }
    backend.SetPlayback(
        std::make_unique<FixedPlayback>(modern_sqlite::JournalPlaybackKind::kSavepointRollback,
                                        page, modern_sqlite::PageNumber{9}));
    NoopTarget target;
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation.reset();
    const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, target);
    if (!rolled_back.has_value() || allocation_index.load(std::memory_order_relaxed) == 0 ||
        target.restore_count != 0 || target.complete_count != 1) {
      return 17;
    }
  }

  {
    NoopBackend backend;
    auto begun = Begin(backend);
    if (!begun.has_value()) {
      return 18;
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
      return 19;
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
      return 20;
    }
  }
  return 0;
} catch (const std::bad_alloc&) {
  failing_allocation.reset();
  return 36;
} catch (const std::length_error&) {
  failing_allocation.reset();
  return 37;
}
