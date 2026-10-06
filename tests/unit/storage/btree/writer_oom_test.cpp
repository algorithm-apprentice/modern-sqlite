#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
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

void Arm(std::optional<std::size_t> failure) noexcept {
  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = failure;
}

[[nodiscard]] std::size_t Disarm() noexcept {
  failing_allocation.reset();
  return allocation_index.load(std::memory_order_relaxed);
}

struct ScenarioOutcome {
  std::size_t allocations = 0;
  bool succeeded = false;
  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool invariant_holds = false;
  bool observed_mutation = false;
  bool observed_no_mutation = false;
};

template <typename Result>
[[nodiscard]] modern_sqlite::ErrorCode ErrorCodeOf(const Result& result) noexcept {
  return result.has_value() ? modern_sqlite::ErrorCode::kGeneric : result.error().code();
}

template <typename Runner>
[[nodiscard]] bool ExhaustAllocations(Runner&& runner, bool require_boundary_coverage = false) {
  const ScenarioOutcome baseline = runner(std::nullopt);
  if (!baseline.succeeded || !baseline.invariant_holds || baseline.allocations == 0U) {
    return false;
  }
  bool observed_mutation = false;
  bool observed_no_mutation = false;
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const ScenarioOutcome outcome = runner(failure);
    if (outcome.succeeded || outcome.error != modern_sqlite::ErrorCode::kOutOfMemory ||
        !outcome.invariant_holds) {
      return false;
    }
    observed_mutation = observed_mutation || outcome.observed_mutation;
    observed_no_mutation = observed_no_mutation || outcome.observed_no_mutation;
  }
  if (require_boundary_coverage && !(observed_mutation && observed_no_mutation)) {
    return false;
  }
  return true;
}

[[nodiscard]] std::unique_ptr<modern_sqlite::Pager> OpenEmpty(
    modern_sqlite::test::WritePagerFixedVfs& vfs, std::size_t cache_pages = 8U) {
  return modern_sqlite::test::OpenWritePager(vfs, cache_pages);
}

[[nodiscard]] bool Initialize(modern_sqlite::Pager& pager) {
  if (!pager.BeginRead().has_value() || !pager.BeginWrite().has_value()) {
    return false;
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(pager);
  return session.has_value() && session->InitializeDatabase().has_value() &&
         pager.Commit().has_value();
}

[[nodiscard]] bool ValidateStaleWriterErrorBoundary() {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !Initialize(*pager) || !pager->BeginWrite().has_value()) {
    return false;
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return false;
  }
  auto writer = session->OpenTableBtree(modern_sqlite::PageNumber{1});
  if (!writer.has_value() || !pager->Rollback().has_value()) {
    return false;
  }

  Arm(std::nullopt);
  const auto baseline = writer->Clear();
  const std::size_t allocations = Disarm();
  if (baseline.has_value() || baseline.error().code() != modern_sqlite::ErrorCode::kSchemaChanged ||
      allocations == 0U) {
    return false;
  }
  for (std::size_t failure = 0; failure < allocations; ++failure) {
    Arm(failure);
    const auto failed = writer->Clear();
    static_cast<void>(Disarm());
    if (failed.has_value() || failed.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool ValidateCoordinatorClaimErrorBoundary() {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs;
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !pager->ClaimWriteCoordinator().has_value()) {
    return false;
  }

  Arm(std::nullopt);
  const auto baseline = pager->ClaimWriteCoordinator();
  const std::size_t allocations = Disarm();
  if (baseline.has_value() || baseline.error().code() != modern_sqlite::ErrorCode::kLocked ||
      allocations == 0U) {
    return false;
  }
  for (std::size_t failure = 0; failure < allocations; ++failure) {
    Arm(failure);
    const auto failed = pager->ClaimWriteCoordinator();
    static_cast<void>(Disarm());
    if (failed.has_value() || failed.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return false;
    }
  }
  return pager->Rollback().has_value();
}

[[nodiscard]] bool ValidateRecordDestinationErrorBoundary() {
  const std::array<modern_sqlite::SqlValue, 1> values{
      modern_sqlite::SqlValue::Integer(1),
  };
  std::array<std::byte, 1> destination{};

  Arm(std::nullopt);
  const auto baseline = modern_sqlite::EncodeRecordInto(values, destination);
  const std::size_t allocations = Disarm();
  if (baseline.has_value() || baseline.error().code() != modern_sqlite::ErrorCode::kMisuse ||
      allocations == 0U) {
    return false;
  }
  for (std::size_t failure = 0; failure < allocations; ++failure) {
    Arm(failure);
    const auto failed = modern_sqlite::EncodeRecordInto(values, destination);
    static_cast<void>(Disarm());
    if (failed.has_value() || failed.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool ValidateInitializationGeometryErrorBoundary() {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return false;
  }
  const modern_sqlite::BtreeDatabaseOptions options{
      .reserved_bytes = modern_sqlite::ByteCount{33},
  };

  Arm(std::nullopt);
  const auto baseline = session->InitializeDatabase(options);
  const std::size_t allocations = Disarm();
  if (baseline.has_value() || baseline.error().code() != modern_sqlite::ErrorCode::kMisuse ||
      allocations == 0U) {
    return false;
  }
  for (std::size_t failure = 0; failure < allocations; ++failure) {
    Arm(failure);
    const auto failed = session->InitializeDatabase(options);
    static_cast<void>(Disarm());
    if (failed.has_value() || failed.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return false;
    }
  }
  return pager->Rollback().has_value();
}

[[nodiscard]] bool ValidateInvalidRootErrorBoundary() {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !Initialize(*pager) || !pager->BeginWrite().has_value()) {
    return false;
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return false;
  }
  const modern_sqlite::PageNumber invalid_root{pager->page_count() + 1U};

  Arm(std::nullopt);
  const auto baseline = session->OpenTableBtree(invalid_root);
  const std::size_t allocations = Disarm();
  if (baseline.has_value() || baseline.error().code() == modern_sqlite::ErrorCode::kOutOfMemory ||
      allocations == 0U) {
    return false;
  }
  for (std::size_t failure = 0; failure < allocations; ++failure) {
    Arm(failure);
    const auto failed = session->OpenTableBtree(invalid_root);
    static_cast<void>(Disarm());
    if (failed.has_value() || failed.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return false;
    }
  }
  return pager->Rollback().has_value();
}

[[nodiscard]] bool Matches(const modern_sqlite::test::WritePagerFixedVfs& vfs,
                           const std::vector<std::byte>& expected) {
  const modern_sqlite::ByteView actual = vfs.database_bytes();
  return actual.size() == expected.size() && std::ranges::equal(actual, expected);
}

[[nodiscard]] bool PagerImageMatches(modern_sqlite::Pager& pager,
                                     const std::vector<std::byte>& expected) {
  constexpr std::size_t kPageSize = modern_sqlite::test::kWritePagerPageSize;
  if (expected.size() != static_cast<std::size_t>(pager.page_count()) * kPageSize) {
    return false;
  }
  for (std::uint32_t page = 1; page <= pager.page_count(); ++page) {
    auto pin = pager.ReadPage(modern_sqlite::PageNumber{page});
    if (!pin.has_value()) {
      return false;
    }
    const auto expected_page =
        std::span{expected}.subspan(static_cast<std::size_t>(page - 1U) * kPageSize, kPageSize);
    if (!std::ranges::equal(pin->frame().bytes(), expected_page)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] ScenarioOutcome RunSessionOpen(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !Initialize(*pager) || !pager->BeginWrite().has_value()) {
    return {};
  }

  Arm(failure);
  const auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(session);
  bool invariant = session.has_value();
  if (!session.has_value()) {
    invariant = modern_sqlite::BtreeWriteSession::Open(*pager).has_value();
  }
  invariant = invariant && pager->Rollback().has_value();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = session.has_value(),
      .error = error,
      .invariant_holds = invariant,
  };
}

[[nodiscard]] ScenarioOutcome RunRootOpen(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !Initialize(*pager) || !pager->BeginWrite().has_value()) {
    return {};
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return {};
  }

  Arm(failure);
  const auto writer = session->OpenTableBtree(modern_sqlite::PageNumber{1});
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(writer);
  bool invariant = writer.has_value();
  if (!writer.has_value()) {
    invariant = session->OpenTableBtree(modern_sqlite::PageNumber{1}).has_value();
  }
  invariant = invariant && pager->Rollback().has_value();
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = writer.has_value(),
      .error = error,
      .invariant_holds = invariant,
  };
}

[[nodiscard]] ScenarioOutcome RunCreateRoot(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !Initialize(*pager)) {
    return {};
  }
  const modern_sqlite::ByteView original = vfs.database_bytes();
  const std::vector<std::byte> expected{original.begin(), original.end()};
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return {};
  }

  Arm(failure);
  const auto writer = session->CreateTableBtree();
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(writer);
  bool invariant = true;
  if (!writer.has_value() && !session->requires_rollback()) {
    invariant = session->CreateTableBtree().has_value();
  }
  invariant = invariant && pager->Rollback().has_value() && Matches(vfs, expected);
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = writer.has_value(),
      .error = error,
      .invariant_holds = invariant,
  };
}

[[nodiscard]] ScenarioOutcome RunOverflowInsert(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs, 2U);
  if (pager == nullptr || !Initialize(*pager)) {
    return {};
  }
  const modern_sqlite::ByteView original = vfs.database_bytes();
  const std::vector<std::byte> expected{original.begin(), original.end()};
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return {};
  }
  auto writer = session->OpenTableBtree(modern_sqlite::PageNumber{1});
  if (!writer.has_value()) {
    return {};
  }
  const auto savepoint = pager->CreateSavepoint();
  if (!savepoint.has_value()) {
    return {};
  }
  std::vector<std::byte> payload(6'000U, std::byte{0x5a});

  Arm(failure);
  const auto inserted = writer->Insert(1, payload);
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(inserted);
  bool invariant = true;
  bool observed_mutation = false;
  bool observed_no_mutation = false;
  bool observed_pager_failure = false;
  if (!inserted.has_value()) {
    const bool image_changed = !PagerImageMatches(*pager, expected);
    const std::optional<modern_sqlite::ErrorCode> pager_failure = pager->write_failure_code();
    observed_mutation = image_changed;
    observed_no_mutation = !image_changed && !pager_failure.has_value();
    observed_pager_failure = pager_failure.has_value();
    if (image_changed || pager_failure.has_value()) {
      const auto blocked = writer->Insert(999, payload);
      const auto created = pager->CreateSavepoint();
      const auto committed = pager->Commit();
      invariant = session->requires_rollback() && !blocked.has_value() &&
                  blocked.error().code() == modern_sqlite::ErrorCode::kOutOfMemory &&
                  !created.has_value() &&
                  created.error().code() == modern_sqlite::ErrorCode::kOutOfMemory &&
                  !committed.has_value() &&
                  committed.error().code() == modern_sqlite::ErrorCode::kOutOfMemory;
    } else {
      invariant = !session->requires_rollback() && writer->Insert(999, payload).has_value();
    }
  }
  if (!inserted.has_value() && !observed_pager_failure) {
    const auto restored = pager->RollbackToSavepoint(*savepoint);
    bool reopened = false;
    if (restored.has_value() && !pager->write_failure_code().has_value()) {
      const auto restored_session = modern_sqlite::BtreeWriteSession::Open(*pager);
      reopened = restored_session.has_value();
    }
    const auto rolled_back = pager->Rollback();
    const bool matches = Matches(vfs, expected);
    invariant =
        invariant && restored.has_value() && reopened && rolled_back.has_value() && matches;
  } else {
    invariant = invariant && pager->Rollback().has_value() && Matches(vfs, expected);
  }
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = inserted.has_value(),
      .error = error,
      .invariant_holds = invariant,
      .observed_mutation = observed_mutation,
      .observed_no_mutation = observed_no_mutation,
  };
}

[[nodiscard]] ScenarioOutcome RunIndexInsert(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !Initialize(*pager) || !pager->BeginWrite().has_value()) {
    return {};
  }
  const std::array<modern_sqlite::IndexColumnOrder, 1> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  auto setup_session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!setup_session.has_value()) {
    return {};
  }
  auto setup_writer = setup_session->CreateIndexBtree(columns);
  if (!setup_writer.has_value()) {
    return {};
  }
  const modern_sqlite::PageNumber root_page = setup_writer->root_page();
  if (!pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteView original = vfs.database_bytes();
  const std::vector<std::byte> expected{original.begin(), original.end()};
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return {};
  }
  auto writer = session->OpenIndexBtree(root_page, columns);
  if (!writer.has_value()) {
    return {};
  }
  const std::array<modern_sqlite::SqlValue, 1> values{
      modern_sqlite::SqlValue::Text(std::string(1'000U, 'x')),
  };

  Arm(failure);
  const auto inserted = writer->Insert(values);
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(inserted);
  bool invariant = true;
  bool observed_mutation = false;
  bool observed_no_mutation = false;
  if (!inserted.has_value()) {
    const bool image_changed = !PagerImageMatches(*pager, expected);
    const std::optional<modern_sqlite::ErrorCode> pager_failure = pager->write_failure_code();
    observed_mutation = image_changed;
    observed_no_mutation = !image_changed && !pager_failure.has_value();
    if (image_changed || pager_failure.has_value()) {
      const auto blocked = writer->Insert(values);
      invariant = session->requires_rollback() && !blocked.has_value() &&
                  blocked.error().code() == modern_sqlite::ErrorCode::kOutOfMemory;
    } else {
      invariant = !session->requires_rollback() && writer->Insert(values).has_value();
    }
  }
  invariant = invariant && pager->Rollback().has_value() && Matches(vfs, expected);
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = inserted.has_value(),
      .error = error,
      .invariant_holds = invariant,
      .observed_mutation = observed_mutation,
      .observed_no_mutation = observed_no_mutation,
  };
}

[[nodiscard]] ScenarioOutcome RunTableNonRightmostSplit(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !Initialize(*pager) || !pager->BeginWrite().has_value()) {
    return {};
  }
  auto setup_session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!setup_session.has_value()) {
    return {};
  }
  auto setup_writer = setup_session->OpenTableBtree(modern_sqlite::PageNumber{1});
  if (!setup_writer.has_value()) {
    return {};
  }
  const std::array<std::byte, 20> setup_payload{};
  for (std::int64_t rowid = 1; rowid <= 100; ++rowid) {
    if (!setup_writer->Insert(rowid, setup_payload).has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }

  const modern_sqlite::ByteView original = vfs.database_bytes();
  const std::vector<std::byte> expected{original.begin(), original.end()};
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return {};
  }
  auto writer = session->OpenTableBtree(modern_sqlite::PageNumber{1});
  if (!writer.has_value()) {
    return {};
  }
  const std::vector<std::byte> payload(470U, std::byte{0x6b});

  Arm(failure);
  const auto inserted = writer->Insert((std::numeric_limits<std::int64_t>::min)(), payload);
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(inserted);
  bool invariant = true;
  bool observed_mutation = false;
  bool observed_no_mutation = false;
  if (!inserted.has_value()) {
    const bool image_changed = !PagerImageMatches(*pager, expected);
    const std::optional<modern_sqlite::ErrorCode> pager_failure = pager->write_failure_code();
    observed_mutation = image_changed;
    observed_no_mutation = !image_changed && !pager_failure.has_value();
    if (image_changed || pager_failure.has_value()) {
      const auto blocked = writer->Insert((std::numeric_limits<std::int64_t>::min)(), payload);
      invariant = session->requires_rollback() && !blocked.has_value() &&
                  blocked.error().code() == modern_sqlite::ErrorCode::kOutOfMemory;
    } else {
      invariant = !session->requires_rollback() &&
                  writer->Insert((std::numeric_limits<std::int64_t>::min)(), payload).has_value();
    }
  }
  invariant = invariant && pager->Rollback().has_value() && Matches(vfs, expected);
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = inserted.has_value(),
      .error = error,
      .invariant_holds = invariant,
      .observed_mutation = observed_mutation,
      .observed_no_mutation = observed_no_mutation,
  };
}

[[nodiscard]] ScenarioOutcome RunIndexInteriorDelete(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = OpenEmpty(vfs);
  if (pager == nullptr || !Initialize(*pager) || !pager->BeginWrite().has_value()) {
    return {};
  }
  const std::array<modern_sqlite::IndexColumnOrder, 2> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  auto setup_session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!setup_session.has_value()) {
    return {};
  }
  auto setup_writer = setup_session->CreateIndexBtree(columns);
  if (!setup_writer.has_value()) {
    return {};
  }
  const modern_sqlite::PageNumber root_page = setup_writer->root_page();
  for (std::int64_t value = 1; value <= 100; ++value) {
    const std::array<modern_sqlite::SqlValue, 2> record{
        modern_sqlite::SqlValue::Integer(value),
        modern_sqlite::SqlValue::Integer(value),
    };
    if (!setup_writer->Insert(record).has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }

  std::vector<modern_sqlite::SqlValue> target;
  {
    const modern_sqlite::DatabaseHeader* header = pager->header();
    if (header == nullptr) {
      return {};
    }
    auto geometry =
        modern_sqlite::BtreePageGeometry::Create(header->page_size(), header->usable_size());
    auto root_pin = pager->ReadPage(root_page);
    if (!geometry.has_value() || !root_pin.has_value()) {
      return {};
    }
    auto root =
        modern_sqlite::BtreePageView::Parse(root_pin->frame().bytes(), root_page, *geometry);
    if (!root.has_value() || root->type() != modern_sqlite::BtreePageType::kInteriorIndex ||
        root->cell_count() == 0U) {
      return {};
    }
    auto cell = root->cell(root->cell_count() / 2U);
    if (!cell.has_value()) {
      return {};
    }
    auto decoded = modern_sqlite::DecodeRecord(cell->local_payload());
    if (!decoded.has_value()) {
      return {};
    }
    target = std::move(*decoded);
  }

  const modern_sqlite::ByteView original = vfs.database_bytes();
  const std::vector<std::byte> expected{original.begin(), original.end()};
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  auto session = modern_sqlite::BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return {};
  }
  auto writer = session->OpenIndexBtree(root_page, columns);
  if (!writer.has_value()) {
    return {};
  }

  Arm(failure);
  const auto deleted = writer->Delete(target);
  const std::size_t allocations = Disarm();
  const modern_sqlite::ErrorCode error = ErrorCodeOf(deleted);
  bool invariant = !deleted.has_value() || *deleted;
  bool observed_mutation = false;
  bool observed_no_mutation = false;
  if (!deleted.has_value()) {
    const bool image_changed = !PagerImageMatches(*pager, expected);
    const std::optional<modern_sqlite::ErrorCode> pager_failure = pager->write_failure_code();
    observed_mutation = image_changed;
    observed_no_mutation = !image_changed && !pager_failure.has_value();
    if (image_changed || pager_failure.has_value()) {
      const auto blocked = writer->Delete(target);
      invariant = session->requires_rollback() && !blocked.has_value() &&
                  blocked.error().code() == modern_sqlite::ErrorCode::kOutOfMemory;
    } else {
      const auto retry = writer->Delete(target);
      invariant = !session->requires_rollback() && retry.has_value() && *retry;
    }
  }
  invariant = invariant && pager->Rollback().has_value() && Matches(vfs, expected);
  return ScenarioOutcome{
      .allocations = allocations,
      .succeeded = deleted.has_value() && *deleted,
      .error = error,
      .invariant_holds = invariant,
      .observed_mutation = observed_mutation,
      .observed_no_mutation = observed_no_mutation,
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
  if (!ExhaustAllocations(RunSessionOpen)) {
    return 1;
  }
  if (!ExhaustAllocations(RunRootOpen)) {
    return 2;
  }
  if (!ExhaustAllocations(RunCreateRoot)) {
    return 3;
  }
  if (!ExhaustAllocations(RunOverflowInsert, true)) {
    return 4;
  }
  if (!ExhaustAllocations(RunIndexInsert, true)) {
    return 5;
  }
  if (!ExhaustAllocations(RunTableNonRightmostSplit, true)) {
    return 6;
  }
  if (!ExhaustAllocations(RunIndexInteriorDelete, true)) {
    return 7;
  }
  if (!ValidateStaleWriterErrorBoundary()) {
    return 8;
  }
  if (!ValidateCoordinatorClaimErrorBoundary()) {
    return 9;
  }
  if (!ValidateRecordDestinationErrorBoundary()) {
    return 10;
  }
  if (!ValidateInitializationGeometryErrorBoundary()) {
    return 11;
  }
  if (!ValidateInvalidRootErrorBoundary()) {
    return 12;
  }
  return 0;
} catch (...) {
  failing_allocation.reset();
  return 13;
}
