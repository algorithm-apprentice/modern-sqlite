#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "storage/btree/writer_internal.hpp"
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

template <typename Runner>
[[nodiscard]] bool ExhaustAllocations(Runner&& runner) {
  const Outcome baseline = runner(std::nullopt);
  if (!baseline.succeeded || !baseline.invariant_holds || baseline.allocations == 0U) {
    return false;
  }
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const Outcome outcome = runner(failure);
    if (outcome.succeeded || outcome.error != modern_sqlite::ErrorCode::kOutOfMemory ||
        !outcome.invariant_holds) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Outcome RunOverflowSeek(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }

  const std::array<modern_sqlite::IndexColumnOrder, 1> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  modern_sqlite::PageNumber root_page;
  std::array<modern_sqlite::SqlValue, 1> key{
      modern_sqlite::SqlValue::Blob(modern_sqlite::ByteBuffer{modern_sqlite::ByteCount{2'000}}),
  };
  if (!modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                   pager->header()->usable_size());
    auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
    if (!geometry.has_value() || !workspace.has_value()) {
      return {};
    }
    const auto root = modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    const auto record = modern_sqlite::EncodeRecord(key);
    if (!root.has_value() || !record.has_value()) {
      return {};
    }
    root_page = root->page_number;
    auto page = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, root->owner_slot, *geometry, modern_sqlite::BtreePageType::kLeafIndex);
    const auto cell = modern_sqlite::btree_internal::FillIndexCell(
        owner, *geometry, *workspace, record->view(), modern_sqlite::BtreePageType::kLeafIndex,
        std::nullopt);
    if (!page.has_value() || !cell.has_value() ||
        !page->InsertCell(0U, cell->bytes, std::nullopt, {}, *workspace).has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool invariant = false;
  std::size_t allocations = 0;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(owner, root_page, false);
    if (!cursor.has_value()) {
      return {};
    }
    std::vector<std::byte> scratch;
    Arm(failure);
    const auto found =
        cursor->SeekIndex(key, columns,
                          modern_sqlite::RecordCodecOptions{
                              .schema_format = modern_sqlite::RecordSchemaFormat::kFour,
                          },
                          scratch);
    allocations = Disarm();
    succeeded = found.has_value();
    error = found.has_value() ? modern_sqlite::ErrorCode::kGeneric : found.error().code();
    invariant =
        found.has_value()
            ? found->exact
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault &&
                  owner.size() == 1U;
  }
  invariant = invariant && pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = invariant,
  };
}

[[nodiscard]] Outcome RunOverflowFormat(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  if (!modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value()) {
    return {};
  }
  const std::vector<std::byte> original{vfs.database_bytes().begin(), vfs.database_bytes().end()};
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  auto workspace =
      modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(modern_sqlite::ByteCount{512});
  if (!workspace.has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  if (!geometry.has_value()) {
    return {};
  }
  const std::vector<std::byte> payload(2'000U, std::byte{0x6a});

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    Arm(failure);
    const auto formatted =
        modern_sqlite::btree_internal::FillTableLeafCell(owner, *geometry, *workspace, 7, payload);
    allocations = Disarm();
    succeeded = formatted.has_value();
    error = formatted.has_value() ? modern_sqlite::ErrorCode::kGeneric : formatted.error().code();
  }
  const bool rolled_back = pager->Rollback().has_value();
  const modern_sqlite::ByteView restored = vfs.database_bytes();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = rolled_back && std::ranges::equal(original, restored),
  };
}

[[nodiscard]] Outcome RunFreelistLeafMark(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  if (!modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value()) {
    return {};
  }
  const std::vector<std::byte> original{vfs.database_bytes().begin(), vfs.database_bytes().end()};
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  if (!geometry.has_value()) {
    return {};
  }

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto second = modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    auto third = modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    if (!second.has_value() || !third.has_value() ||
        !modern_sqlite::btree_internal::FreeBtreePage(owner, *geometry, second->page_number)
             .has_value()) {
      return {};
    }

    Arm(failure);
    const auto freed =
        modern_sqlite::btree_internal::FreeBtreePage(owner, *geometry, third->page_number);
    allocations = Disarm();
    succeeded = freed.has_value();
    error = freed.has_value() ? modern_sqlite::ErrorCode::kGeneric : freed.error().code();
    failure_latched =
        freed.has_value() || pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds =
          failure_latched && rolled_back && std::ranges::equal(original, vfs.database_bytes()),
  };
}

[[nodiscard]] bool CellArrayPageEditingAllocatesNothing() {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!geometry.has_value() || !workspace.has_value()) {
    return false;
  }
  auto cells = modern_sqlite::btree_internal::CellArray::Create(*geometry);
  if (!cells.has_value()) {
    return false;
  }

  bool succeeded = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto slot = owner.AcquireWrite(modern_sqlite::PageNumber{1});
    if (!slot.has_value()) {
      return false;
    }
    auto page = modern_sqlite::btree_internal::MutableBtreePage::Open(owner, *slot, *geometry);
    const std::array<std::byte, 4> payload{
        std::byte{0x10},
        std::byte{0x20},
        std::byte{0x30},
        std::byte{0x40},
    };
    const auto formatted =
        modern_sqlite::btree_internal::FillTableLeafCell(owner, *geometry, *workspace, 1, payload);
    if (!page.has_value() || !formatted.has_value() ||
        !page->InsertCell(0U, formatted->bytes, std::nullopt, {}, *workspace).has_value()) {
      return false;
    }

    Arm(std::nullopt);
    const auto appended = cells->AppendPage(*page);
    bool edited = false;
    if (appended.has_value()) {
      edited = page->Edit(*cells, 0U, 0U, cells->size(), *workspace).has_value();
    }
    bool copied = false;
    if (edited) {
      copied = cells->AppendCopied(formatted->bytes).has_value();
    }
    bool rebuilt = false;
    if (copied) {
      rebuilt = page->Rebuild(*cells, 0U, cells->size(), *workspace).has_value();
    }
    allocations = Disarm();
    succeeded = appended.has_value() && copied && rebuilt && edited;
  }
  return succeeded && allocations == 0U && pager->Rollback().has_value();
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
  Arm(std::nullopt);
  const auto baseline_workspace =
      modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(modern_sqlite::ByteCount{512});
  const std::size_t workspace_allocations = Disarm();
  if (!baseline_workspace.has_value() || workspace_allocations == 0U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < workspace_allocations; ++failure) {
    Arm(failure);
    const auto workspace =
        modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(modern_sqlite::ByteCount{512});
    (void)Disarm();
    if (workspace.has_value() ||
        workspace.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return 2;
    }
  }

  const auto geometry = modern_sqlite::BtreePageGeometry::Create(modern_sqlite::ByteCount{512},
                                                                 modern_sqlite::ByteCount{512});
  if (!geometry.has_value()) {
    return 3;
  }
  Arm(std::nullopt);
  const auto baseline_cells = modern_sqlite::btree_internal::CellArray::Create(*geometry);
  const std::size_t cell_array_allocations = Disarm();
  if (!baseline_cells.has_value() || cell_array_allocations == 0U) {
    return 4;
  }
  for (std::size_t failure = 0; failure < cell_array_allocations; ++failure) {
    Arm(failure);
    const auto cells = modern_sqlite::btree_internal::CellArray::Create(*geometry);
    (void)Disarm();
    if (cells.has_value() || cells.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return 5;
    }
  }
  if (!CellArrayPageEditingAllocatesNothing()) {
    return 6;
  }
  if (!ExhaustAllocations(RunOverflowSeek)) {
    return 7;
  }
  if (!ExhaustAllocations(RunOverflowFormat)) {
    return 8;
  }
  if (!ExhaustAllocations(RunFreelistLeafMark)) {
    return 9;
  }
  return 0;
} catch (...) {
  failing_allocation.reset();
  return 10;
}
