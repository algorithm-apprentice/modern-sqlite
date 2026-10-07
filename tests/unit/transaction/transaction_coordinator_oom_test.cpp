#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::optional<std::size_t> failing_allocation;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (failing_allocation == index) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
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
  if (posix_memalign(&memory, alignment, size == 0U ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

void Arm(std::optional<std::size_t> failure) noexcept {
  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = failure;
}

[[nodiscard]] std::size_t Disarm() noexcept {
  failing_allocation.reset();
  return allocation_index.load(std::memory_order_relaxed);
}

struct Outcome {
  std::size_t allocations = 0;
  bool succeeded = false;
  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool invariant_holds = false;
};

template <typename T>
[[nodiscard]] modern_sqlite::ErrorCode ErrorCodeOf(
    const modern_sqlite::Result<T>& result) noexcept {
  return result.has_value() ? modern_sqlite::ErrorCode::kGeneric : result.error().code();
}

template <typename Runner>
[[nodiscard]] bool ExhaustAllocations(Runner&& runner) {
  const Outcome baseline = runner(std::nullopt);
  if (!baseline.succeeded || !baseline.invariant_holds || baseline.allocations == 0U ||
      baseline.allocations > 256U) {
    return false;
  }
  for (std::size_t failure = 0U; failure < baseline.allocations; ++failure) {
    const Outcome outcome = runner(failure);
    if (outcome.succeeded || outcome.error != modern_sqlite::ErrorCode::kOutOfMemory ||
        !outcome.invariant_holds) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] modern_sqlite::Result<modern_sqlite::TransactionCoordinator> OpenCoordinator(
    modern_sqlite::test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    return std::unexpected(modern_sqlite::Error::Create(modern_sqlite::ErrorCode::kInternal,
                                                        "failed to open transaction OOM pager"));
  }
  return modern_sqlite::TransactionCoordinator::Open(std::move(pager));
}

[[nodiscard]] Outcome RunOpen(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    return {};
  }
  Arm(failure);
  const auto opened = modern_sqlite::TransactionCoordinator::Open(std::move(pager));
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(opened);
  bool invariant = opened.has_value();
  if (!opened.has_value()) {
    invariant = modern_sqlite::test::OpenWritePager(vfs, 64U) != nullptr;
  }
  return Outcome{
      .allocations = allocations,
      .succeeded = opened.has_value(),
      .error = error,
      .invariant_holds = invariant,
  };
}

[[nodiscard]] Outcome RunReadStatement(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  auto opened = OpenCoordinator(vfs);
  if (!opened.has_value()) {
    return {};
  }
  modern_sqlite::TransactionCoordinator coordinator = std::move(*opened);
  Arm(failure);
  auto statement = coordinator.BeginStatement();
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(statement);
  bool invariant = false;
  if (statement.has_value()) {
    invariant = statement->Rollback().has_value() && coordinator.autocommit();
  } else {
    const bool clean_before_retry = !coordinator.statement_active() && coordinator.autocommit();
    auto retried = coordinator.BeginStatement();
    invariant = clean_before_retry && retried.has_value() && retried->Rollback().has_value();
  }
  return Outcome{
      .allocations = allocations,
      .succeeded = statement.has_value(),
      .error = error,
      .invariant_holds = invariant,
  };
}

[[nodiscard]] Outcome RunWriteStatement(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  auto opened = OpenCoordinator(vfs);
  if (!opened.has_value() || !opened->Begin().has_value()) {
    return {};
  }
  modern_sqlite::TransactionCoordinator coordinator = std::move(*opened);
  Arm(failure);
  auto statement = coordinator.BeginStatement(modern_sqlite::TransactionStatementOptions{
      .access = modern_sqlite::StatementAccess::kWrite,
  });
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(statement);
  bool invariant = false;
  if (statement.has_value()) {
    invariant = statement->Rollback().has_value() && coordinator.Rollback().has_value();
  } else if (coordinator.autocommit()) {
    invariant = !coordinator.statement_active();
  } else {
    invariant = !coordinator.statement_active() && coordinator.Rollback().has_value();
  }
  return Outcome{
      .allocations = allocations,
      .succeeded = statement.has_value(),
      .error = error,
      .invariant_holds = invariant,
  };
}

[[nodiscard]] Outcome RunLogicalSavepoint(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  auto opened = OpenCoordinator(vfs);
  if (!opened.has_value() || !opened->Begin().has_value()) {
    return {};
  }
  modern_sqlite::TransactionCoordinator coordinator = std::move(*opened);
  const std::string name(128U, 's');
  Arm(failure);
  const auto saved = coordinator.Savepoint(modern_sqlite::Utf8View{name});
  const std::size_t allocations = Disarm();
  bool invariant = false;
  if (saved.has_value()) {
    invariant = coordinator.Release(modern_sqlite::Utf8View{name}).has_value() &&
                coordinator.Rollback().has_value();
  } else {
    invariant = coordinator.state() == modern_sqlite::TransactionState::kExplicit &&
                coordinator.Rollback().has_value();
  }
  return Outcome{
      .allocations = allocations,
      .succeeded = saved.has_value(),
      .error = saved.has_value() ? modern_sqlite::ErrorCode::kGeneric : saved.error().code(),
      .invariant_holds = invariant,
  };
}

[[nodiscard]] Outcome RunMaterializedSavepoint(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  auto opened = OpenCoordinator(vfs);
  if (!opened.has_value() ||
      !opened->Begin(modern_sqlite::TransactionMode::kImmediate).has_value()) {
    return {};
  }
  modern_sqlite::TransactionCoordinator coordinator = std::move(*opened);
  const std::string name(128U, 'm');
  Arm(failure);
  const auto saved = coordinator.Savepoint(modern_sqlite::Utf8View{name});
  const std::size_t allocations = Disarm();
  bool invariant = false;
  if (saved.has_value()) {
    invariant = coordinator.Release(modern_sqlite::Utf8View{name}).has_value() &&
                coordinator.Rollback().has_value();
  } else if (coordinator.autocommit()) {
    invariant = !coordinator.statement_active();
  } else {
    invariant = coordinator.state() == modern_sqlite::TransactionState::kExplicit &&
                coordinator.Rollback().has_value();
  }
  return Outcome{
      .allocations = allocations,
      .succeeded = saved.has_value(),
      .error = saved.has_value() ? modern_sqlite::ErrorCode::kGeneric : saved.error().code(),
      .invariant_holds = invariant,
  };
}

[[nodiscard]] std::optional<modern_sqlite::ByteBuffer> MakeFixture() {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  auto opened = OpenCoordinator(vfs);
  if (!opened.has_value()) {
    return std::nullopt;
  }
  auto statement = opened->BeginStatement(modern_sqlite::TransactionStatementOptions{
      .access = modern_sqlite::StatementAccess::kWrite,
  });
  if (!statement.has_value() || statement->writer() == nullptr ||
      !statement->writer()->InitializeDatabase().has_value() || !statement->Succeed().has_value()) {
    return std::nullopt;
  }
  return modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
}

[[nodiscard]] std::optional<std::size_t> MeasureBatchAllocations(modern_sqlite::ByteView fixture,
                                                                 bool explicit_transaction) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  vfs.LoadDatabase(fixture);
  auto opened = OpenCoordinator(vfs);
  if (!opened.has_value() || (explicit_transaction && !opened->Begin().has_value())) {
    return std::nullopt;
  }

  Arm(std::nullopt);
  for (std::int64_t rowid = 2; rowid <= 4; ++rowid) {
    auto statement = opened->BeginStatement(modern_sqlite::TransactionStatementOptions{
        .access = modern_sqlite::StatementAccess::kWrite,
    });
    if (!statement.has_value() || statement->writer() == nullptr) {
      static_cast<void>(Disarm());
      return std::nullopt;
    }
    auto table = statement->writer()->OpenTableBtree(modern_sqlite::PageNumber{1});
    std::array<std::byte, 8> payload{};
    std::ranges::fill(payload, static_cast<std::byte>(rowid));
    if (!table.has_value() || !table->Insert(rowid, payload).has_value() ||
        !statement->Succeed().has_value()) {
      static_cast<void>(Disarm());
      return std::nullopt;
    }
  }
  if (explicit_transaction && !opened->Commit().has_value()) {
    static_cast<void>(Disarm());
    return std::nullopt;
  }
  return Disarm();
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
  if (!ExhaustAllocations(RunReadStatement)) {
    return 2;
  }
  if (!ExhaustAllocations(RunWriteStatement)) {
    return 3;
  }
  if (!ExhaustAllocations(RunLogicalSavepoint)) {
    return 4;
  }
  if (!ExhaustAllocations(RunMaterializedSavepoint)) {
    return 5;
  }
  const auto fixture = MakeFixture();
  if (!fixture.has_value()) {
    return 6;
  }
  const auto implicit_allocations = MeasureBatchAllocations(fixture->view(), false);
  const auto explicit_allocations = MeasureBatchAllocations(fixture->view(), true);
  if (!implicit_allocations.has_value() || !explicit_allocations.has_value() ||
      *implicit_allocations == 0U || *explicit_allocations == 0U ||
      *explicit_allocations >= *implicit_allocations) {
    return 7;
  }
  return 0;
} catch (...) {
  failing_allocation.reset();
  return 8;
}
