#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "modern_sqlite/storage/journal/rollback_journal.hpp"
#include "rollback_journal_test_support.hpp"

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
  if (!baseline.succeeded || !baseline.invariant_holds || baseline.allocations == 0) {
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

[[nodiscard]] ScenarioOutcome RunCreate(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::FixedMemoryVfs vfs;
  Arm(failure);
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error =
      created.has_value() ? modern_sqlite::ErrorCode::kGeneric : created.error().code();
  bool reusable = true;
  if (!created.has_value()) {
    reusable =
        modern_sqlite::RollbackJournal::Create(
            vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties())
            .has_value();
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = created.has_value(),
      .error = error,
      .invariant_holds = reusable,
  };
}

[[nodiscard]] ScenarioOutcome RunBegin(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::FixedMemoryVfs vfs;
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return {};
  }
  Arm(failure);
  auto begun = modern_sqlite::JournalTransaction::Begin(
      **created, modern_sqlite::test::FixedMemoryVfs::TransactionInfo());
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error =
      begun.has_value() ? modern_sqlite::ErrorCode::kGeneric : begun.error().code();
  bool reusable = true;
  if (!begun.has_value()) {
    reusable = modern_sqlite::JournalTransaction::Begin(
                   **created, modern_sqlite::test::FixedMemoryVfs::TransactionInfo())
                   .has_value();
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = begun.has_value(),
      .error = error,
      .invariant_holds = reusable,
  };
}

[[nodiscard]] std::unique_ptr<modern_sqlite::JournalTransaction> BeginTransaction(
    modern_sqlite::RollbackJournal& journal) {
  auto begun = modern_sqlite::JournalTransaction::Begin(
      journal, modern_sqlite::test::FixedMemoryVfs::TransactionInfo());
  return begun.has_value() ? std::move(*begun) : nullptr;
}

[[nodiscard]] bool Capture(modern_sqlite::JournalTransaction& transaction,
                           std::uint32_t page_number) {
  const std::array<std::byte, modern_sqlite::test::kTestPageSize> page{
      static_cast<std::byte>(page_number & 0xffU)};
  return transaction
      .CapturePage(modern_sqlite::JournalPageImage{
          .page_number = modern_sqlite::PageNumber{page_number},
          .bytes = page,
      })
      .has_value();
}

[[nodiscard]] ScenarioOutcome RunSavepointGrowth(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::FixedMemoryVfs vfs;
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return {};
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> transaction = BeginTransaction(**created);
  if (transaction == nullptr) {
    return {};
  }
  for (std::size_t index = 0; index < 4; ++index) {
    if (!transaction->CreateSavepoint(64).has_value()) {
      return {};
    }
  }

  Arm(failure);
  const auto savepoint = transaction->CreateSavepoint(64);
  const std::size_t allocations = Disarm();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = savepoint.has_value(),
      .error =
          savepoint.has_value() ? modern_sqlite::ErrorCode::kGeneric : savepoint.error().code(),
      .invariant_holds = savepoint.has_value() ||
                         transaction->state() == modern_sqlite::JournalTransactionState::kFailed,
  };
}

[[nodiscard]] ScenarioOutcome RunSubjournalSetup(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::FixedMemoryVfs vfs;
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return {};
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> transaction = BeginTransaction(**created);
  if (transaction == nullptr || !Capture(*transaction, 1) ||
      !transaction->CreateSavepoint(64).has_value()) {
    return {};
  }

  Arm(failure);
  const bool captured = Capture(*transaction, 1);
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error =
      captured ? modern_sqlite::ErrorCode::kGeneric
               : transaction->failure_code().value_or(modern_sqlite::ErrorCode::kGeneric);
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = captured,
      .error = error,
      .invariant_holds =
          captured || transaction->state() == modern_sqlite::JournalTransactionState::kFailed,
  };
}

[[nodiscard]] ScenarioOutcome RunCohortGrowth(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::FixedMemoryVfs vfs;
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return {};
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> transaction = BeginTransaction(**created);
  if (transaction == nullptr) {
    return {};
  }
  for (std::uint32_t page_number = 1; page_number <= 4; ++page_number) {
    if (!Capture(*transaction, page_number) || !transaction->SyncJournal().has_value()) {
      return {};
    }
  }

  Arm(failure);
  const bool captured = Capture(*transaction, 5);
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error =
      captured ? modern_sqlite::ErrorCode::kGeneric
               : transaction->failure_code().value_or(modern_sqlite::ErrorCode::kGeneric);
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = captured,
      .error = error,
      .invariant_holds =
          captured || transaction->state() == modern_sqlite::JournalTransactionState::kFailed,
  };
}

[[nodiscard]] ScenarioOutcome RunSavepointPlayback(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::FixedMemoryVfs vfs;
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return {};
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> transaction = BeginTransaction(**created);
  if (transaction == nullptr || !Capture(*transaction, 1)) {
    return {};
  }
  const auto savepoint = transaction->CreateSavepoint(64);
  if (!savepoint.has_value() || !Capture(*transaction, 2)) {
    return {};
  }
  modern_sqlite::test::CountingRecoveryTarget target;

  Arm(failure);
  const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, target);
  const std::size_t allocations = Disarm();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = rolled_back.has_value(),
      .error =
          rolled_back.has_value() ? modern_sqlite::ErrorCode::kGeneric : rolled_back.error().code(),
      .invariant_holds = rolled_back.has_value() ||
                         transaction->state() == modern_sqlite::JournalTransactionState::kFailed,
  };
}

[[nodiscard]] ScenarioOutcome RunTransactionPlayback(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::FixedMemoryVfs vfs;
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return {};
  }
  std::unique_ptr<modern_sqlite::JournalTransaction> transaction = BeginTransaction(**created);
  if (transaction == nullptr || !Capture(*transaction, 1)) {
    return {};
  }
  modern_sqlite::test::CountingRecoveryTarget target;

  Arm(failure);
  const auto rolled_back = transaction->Rollback(target);
  const std::size_t allocations = Disarm();
  bool invariant = rolled_back.has_value();
  if (!rolled_back.has_value()) {
    modern_sqlite::test::CountingRecoveryTarget retry_target;
    invariant = transaction->state() == modern_sqlite::JournalTransactionState::kFailed &&
                transaction->Rollback(retry_target).has_value();
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = rolled_back.has_value(),
      .error =
          rolled_back.has_value() ? modern_sqlite::ErrorCode::kGeneric : rolled_back.error().code(),
      .invariant_holds = invariant,
  };
}

[[nodiscard]] ScenarioOutcome RunHotPlayback(std::optional<std::size_t> failure,
                                             std::string_view super_journal = {}) {
  failing_allocation.reset();
  modern_sqlite::test::FixedMemoryVfs vfs;
  if (!vfs.LoadHotJournal(8, super_journal)) {
    return {};
  }
  auto created = modern_sqlite::RollbackJournal::Create(
      vfs, "database.sqlite", modern_sqlite::test::FixedMemoryVfs::DatabaseProperties());
  if (!created.has_value()) {
    return {};
  }
  modern_sqlite::test::CountingRecoveryTarget target;

  Arm(failure);
  const auto recovered = modern_sqlite::RecoverHotJournal(**created, target);
  const std::size_t allocations = Disarm();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = recovered.has_value(),
      .error =
          recovered.has_value() ? modern_sqlite::ErrorCode::kGeneric : recovered.error().code(),
      .invariant_holds = recovered.has_value() ? !vfs.journal_present() : vfs.journal_present(),
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

int main() {
  if (!ExhaustAllocations(RunCreate)) {
    return 1;
  }
  if (!ExhaustAllocations(RunBegin)) {
    return 2;
  }
  if (!ExhaustAllocations(RunSavepointGrowth)) {
    return 3;
  }
  if (!ExhaustAllocations(RunSubjournalSetup)) {
    return 4;
  }
  if (!ExhaustAllocations(RunCohortGrowth)) {
    return 5;
  }
  if (!ExhaustAllocations(RunSavepointPlayback)) {
    return 6;
  }
  if (!ExhaustAllocations(RunTransactionPlayback)) {
    return 7;
  }
  if (!ExhaustAllocations(
          [](std::optional<std::size_t> failure) { return RunHotPlayback(failure); })) {
    return 8;
  }
  constexpr std::string_view kSuperJournal =
      "/allocation-tests/modern-sqlite/super-journal/database.sqlite-mj000000900";
  if (!ExhaustAllocations([kSuperJournal](std::optional<std::size_t> failure) {
        return RunHotPlayback(failure, kSuperJournal);
      })) {
    return 9;
  }
  return 0;
}
