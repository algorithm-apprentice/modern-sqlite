#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "storage/btree/writer_internal.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

// BYTE_VECTOR_RESIZABLE_SCRATCH: WritableCursor grows decoded overflow keys in place.
using ResizablePayloadScratch = std::vector<std::byte>;

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

[[nodiscard]] modern_sqlite::ByteBuffer FilledBuffer(std::size_t size, std::byte value) {
  modern_sqlite::ByteBuffer buffer{modern_sqlite::ByteCount{size}};
  std::ranges::fill(buffer.mutable_view(), value);
  return buffer;
}

[[nodiscard]] modern_sqlite::ByteBuffer TableDivider(modern_sqlite::PageNumber left_child,
                                                     std::int64_t rowid) {
  std::array<std::byte, 9> encoded_rowid{};
  const auto encoded = modern_sqlite::EncodeSqliteVarint(
      std::bit_cast<std::uint64_t>(rowid), modern_sqlite::MutableByteView{encoded_rowid});
  if (!encoded.has_value()) {
    return {};
  }
  modern_sqlite::ByteBuffer cell{
      modern_sqlite::ByteCount{sizeof(std::uint32_t) + encoded->value()}};
  modern_sqlite::StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{cell.mutable_view().data(),
                                                  sizeof(std::uint32_t)},
      left_child.value());
  std::ranges::copy(
      std::span{encoded_rowid}.first(encoded->value()),
      cell.mutable_view().begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
  return cell;
}

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
    ResizablePayloadScratch scratch;
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
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
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
  const modern_sqlite::ByteBuffer payload = FilledBuffer(2'000U, std::byte{0x6a});

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    Arm(failure);
    const auto formatted = modern_sqlite::btree_internal::FillTableLeafCell(
        owner, *geometry, *workspace, 7, payload.view());
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
      .invariant_holds = rolled_back && std::ranges::equal(original.view(), restored),
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
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
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
      .invariant_holds = failure_latched && rolled_back &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
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

[[nodiscard]] Outcome RunQuickBalance(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!geometry.has_value() || !workspace.has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer old_payload = FilledBuffer(477U, std::byte{0x31});
  const modern_sqlite::ByteBuffer new_payload = FilledBuffer(20U, std::byte{0x72});

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    const auto parent_allocation =
        modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    const auto leaf_allocation = modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    if (!parent_allocation.has_value() || !leaf_allocation.has_value()) {
      return {};
    }
    auto parent = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, parent_allocation->owner_slot, *geometry,
        modern_sqlite::BtreePageType::kInteriorTable);
    auto leaf = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, leaf_allocation->owner_slot, *geometry, modern_sqlite::BtreePageType::kLeafTable);
    if (!parent.has_value() || !leaf.has_value() ||
        !parent->SetRightmostChild(leaf->page_number()).has_value()) {
      return {};
    }
    const auto old_cell = modern_sqlite::btree_internal::FillTableLeafCell(
        owner, *geometry, *workspace, 1, old_payload.view());
    if (!old_cell.has_value() ||
        !leaf->InsertCell(0U, old_cell->bytes, std::nullopt, {}, *workspace).has_value()) {
      return {};
    }
    const auto new_cell = modern_sqlite::btree_internal::FillTableLeafCell(
        owner, *geometry, *workspace, 2, new_payload.view());
    std::array<std::byte, 32> staged_copy{};
    if (!new_cell.has_value() ||
        !leaf->InsertCell(1U, new_cell->bytes, std::nullopt,
                          modern_sqlite::MutableByteView{staged_copy}, *workspace)
             .has_value()) {
      return {};
    }

    Arm(failure);
    const auto balanced =
        modern_sqlite::btree_internal::MutableBtreePage::BalanceQuick(*parent, *leaf, *workspace);
    allocations = Disarm();
    succeeded = balanced.has_value();
    error = balanced.has_value() ? modern_sqlite::ErrorCode::kGeneric : balanced.error().code();
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = rolled_back && std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunNonrootBalance(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!geometry.has_value() || !workspace.has_value()) {
    return {};
  }
  modern_sqlite::ByteBuffer parent_overflow{geometry->page_size()};
  const modern_sqlite::ByteBuffer payload = FilledBuffer(160U, std::byte{0x3c});

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    const auto parent_allocation =
        modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    const auto left_allocation = modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    const auto middle_allocation =
        modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    const auto right_allocation =
        modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    if (!parent_allocation.has_value() || !left_allocation.has_value() ||
        !middle_allocation.has_value() || !right_allocation.has_value()) {
      return {};
    }
    auto parent = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, parent_allocation->owner_slot, *geometry,
        modern_sqlite::BtreePageType::kInteriorTable);
    auto left = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, left_allocation->owner_slot, *geometry, modern_sqlite::BtreePageType::kLeafTable);
    auto middle = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, middle_allocation->owner_slot, *geometry, modern_sqlite::BtreePageType::kLeafTable);
    auto right = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, right_allocation->owner_slot, *geometry, modern_sqlite::BtreePageType::kLeafTable);
    if (!parent.has_value() || !left.has_value() || !middle.has_value() || !right.has_value()) {
      return {};
    }
    struct CellSpec {
      std::size_t index;
      std::int64_t rowid;
    };
    const auto insert = [&](modern_sqlite::btree_internal::MutableBtreePage& target,
                            CellSpec spec) -> modern_sqlite::Status {
      const auto cell = modern_sqlite::btree_internal::FillTableLeafCell(
          owner, *geometry, *workspace, spec.rowid, payload.view());
      if (!cell.has_value()) {
        return std::unexpected(cell.error());
      }
      return target.InsertCell(spec.index, cell->bytes, std::nullopt, {}, *workspace);
    };
    if (!insert(*left, CellSpec{.index = 0U, .rowid = 1}).has_value() ||
        !insert(*left, CellSpec{.index = 1U, .rowid = 2}).has_value() ||
        !insert(*middle, CellSpec{.index = 0U, .rowid = 3}).has_value() ||
        !insert(*middle, CellSpec{.index = 1U, .rowid = 4}).has_value() ||
        !insert(*right, CellSpec{.index = 0U, .rowid = 5}).has_value() ||
        !insert(*right, CellSpec{.index = 1U, .rowid = 6}).has_value()) {
      return {};
    }
    const modern_sqlite::ByteBuffer left_divider = TableDivider(left->page_number(), 2);
    const modern_sqlite::ByteBuffer middle_divider = TableDivider(middle->page_number(), 4);
    if (left_divider.size().value() == 0U || middle_divider.size().value() == 0U ||
        !parent->InsertCell(0U, left_divider.view(), left->page_number(), {}, *workspace)
             .has_value() ||
        !parent->InsertCell(1U, middle_divider.view(), middle->page_number(), {}, *workspace)
             .has_value() ||
        !parent->SetRightmostChild(right->page_number()).has_value()) {
      return {};
    }
    owner.Release(left_allocation->owner_slot);
    owner.Release(right_allocation->owner_slot);

    Arm(failure);
    const auto balanced = modern_sqlite::btree_internal::MutableBtreePage::BalanceNonroot(
        *parent, *middle, 1U, parent_overflow.mutable_view(), *workspace, false);
    allocations = Disarm();
    succeeded = balanced.has_value();
    error = balanced.has_value() ? modern_sqlite::ErrorCode::kGeneric : balanced.error().code();
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = rolled_back && std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunCursorBalance(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!geometry.has_value() || !workspace.has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer payload = FilledBuffer(380U, std::byte{0x4d});
  modern_sqlite::ByteBuffer staged_cell{geometry->page_size()};

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool cursor_state_valid = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    if (!cursor.has_value() || !cursor->PromoteCurrent().has_value()) {
      return {};
    }
    auto root = modern_sqlite::btree_internal::MutableBtreePage::Open(
        owner, cursor->current_owner_slot(), *geometry);
    const auto old_cell = modern_sqlite::btree_internal::FillTableLeafCell(
        owner, *geometry, *workspace, 1, payload.view());
    if (!root.has_value() || !old_cell.has_value() ||
        !root->InsertCell(0U, old_cell->bytes, std::nullopt, {}, *workspace).has_value()) {
      return {};
    }
    const auto new_cell = modern_sqlite::btree_internal::FillTableLeafCell(
        owner, *geometry, *workspace, 2, payload.view());
    if (!new_cell.has_value() ||
        !root->InsertCell(1U, new_cell->bytes, std::nullopt, staged_cell.mutable_view(), *workspace)
             .has_value()) {
      return {};
    }

    Arm(failure);
    const auto balanced = cursor->Balance(std::move(*root), *workspace);
    allocations = Disarm();
    succeeded = balanced.has_value();
    error = balanced.has_value() ? modern_sqlite::ErrorCode::kGeneric : balanced.error().code();
    cursor_state_valid =
        balanced.has_value()
            ? cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kInvalid
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault;
    failure_latched = balanced.has_value() ||
                      pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = cursor_state_valid && failure_latched && rolled_back &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunTableInsert(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!workspace.has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer first_payload = FilledBuffer(380U, std::byte{0x31});
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    if (!cursor.has_value() ||
        !cursor
             ->InsertTable(1, first_payload.view(),
                           modern_sqlite::btree_internal::BtreeInsertMode::kInsertOnly, *workspace)
             .has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer second_payload = FilledBuffer(380U, std::byte{0x72});

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool cursor_state_valid = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    if (!cursor.has_value()) {
      return {};
    }

    Arm(failure);
    const auto inserted = cursor->InsertTable(
        2, second_payload.view(), modern_sqlite::btree_internal::BtreeInsertMode::kInsertOnly,
        *workspace);
    allocations = Disarm();
    succeeded = inserted.has_value();
    error = inserted.has_value() ? modern_sqlite::ErrorCode::kGeneric : inserted.error().code();
    cursor_state_valid =
        inserted.has_value()
            ? cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kInvalid
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault;
    failure_latched = inserted.has_value() ||
                      owner.mutation_sequence() == owner.operation_checkpoint() ||
                      pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = cursor_state_valid && failure_latched && rolled_back &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunTableReplace(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!workspace.has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original_payload = FilledBuffer(2'000U, std::byte{0x31});
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    if (!cursor.has_value() ||
        !cursor
             ->InsertTable(7, original_payload.view(),
                           modern_sqlite::btree_internal::BtreeInsertMode::kInsertOnly, *workspace)
             .has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer replacement = FilledBuffer(2'500U, std::byte{0x72});

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool cursor_state_valid = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    if (!cursor.has_value()) {
      return {};
    }

    Arm(failure);
    const auto replaced =
        cursor->InsertTable(7, replacement.view(),
                            modern_sqlite::btree_internal::BtreeInsertMode::kReplace, *workspace);
    allocations = Disarm();
    succeeded = replaced.has_value();
    error = replaced.has_value() ? modern_sqlite::ErrorCode::kGeneric : replaced.error().code();
    cursor_state_valid =
        replaced.has_value()
            ? cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kInvalid
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault;
    failure_latched = replaced.has_value() ||
                      owner.mutation_sequence() == owner.operation_checkpoint() ||
                      pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = cursor_state_valid && failure_latched && rolled_back &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunIndexInsert(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!geometry.has_value() || !workspace.has_value()) {
    return {};
  }
  modern_sqlite::PageNumber root_page;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    const auto root = modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    if (!root.has_value() ||
        !modern_sqlite::btree_internal::MutableBtreePage::Initialize(
             owner, root->owner_slot, *geometry, modern_sqlite::BtreePageType::kLeafIndex)
             .has_value()) {
      return {};
    }
    root_page = root->page_number;
  }
  const modern_sqlite::RecordCodecOptions options{
      .schema_format = modern_sqlite::RecordSchemaFormat::kFour,
  };
  const std::array<modern_sqlite::IndexColumnOrder, 1> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  const auto make_key = [](std::uint8_t marker) {
    modern_sqlite::ByteBuffer blob{modern_sqlite::ByteCount{90}};
    std::ranges::fill(blob.mutable_view(), static_cast<std::byte>(marker));
    return std::array<modern_sqlite::SqlValue, 1>{
        modern_sqlite::SqlValue::Blob(std::move(blob)),
    };
  };
  for (std::uint8_t marker = 1U; marker <= 5U; ++marker) {
    std::array<modern_sqlite::SqlValue, 1> key = make_key(marker);
    const auto record = modern_sqlite::EncodeRecord(key, options);
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(owner, root_page, false);
    ResizablePayloadScratch scratch;
    if (!record.has_value() || !cursor.has_value() ||
        !cursor
             ->InsertIndex(record->view(), key, columns, options,
                           modern_sqlite::btree_internal::BtreeInsertMode::kInsertOnly, scratch,
                           *workspace)
             .has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  std::array<modern_sqlite::SqlValue, 1> key = make_key(6U);
  const auto record = modern_sqlite::EncodeRecord(key, options);
  if (!record.has_value()) {
    return {};
  }

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool cursor_state_valid = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(owner, root_page, false);
    if (!cursor.has_value()) {
      return {};
    }
    ResizablePayloadScratch scratch;

    Arm(failure);
    const auto inserted = cursor->InsertIndex(
        record->view(), key, columns, options,
        modern_sqlite::btree_internal::BtreeInsertMode::kInsertOnly, scratch, *workspace);
    allocations = Disarm();
    succeeded = inserted.has_value();
    error = inserted.has_value() ? modern_sqlite::ErrorCode::kGeneric : inserted.error().code();
    cursor_state_valid =
        inserted.has_value()
            ? cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kInvalid
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault;
    failure_latched = inserted.has_value() ||
                      owner.mutation_sequence() == owner.operation_checkpoint() ||
                      pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = cursor_state_valid && failure_latched && rolled_back &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunTableDelete(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!workspace.has_value()) {
    return {};
  }
  for (std::int64_t rowid = 1; rowid <= 2; ++rowid) {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    const modern_sqlite::ByteBuffer payload =
        FilledBuffer(380U, static_cast<std::byte>(static_cast<std::uint8_t>(rowid)));
    if (!cursor.has_value() ||
        !cursor
             ->InsertTable(rowid, payload.view(),
                           modern_sqlite::btree_internal::BtreeInsertMode::kInsertOnly, *workspace)
             .has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool cursor_state_valid = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    if (!cursor.has_value()) {
      return {};
    }

    Arm(failure);
    const auto deleted = cursor->DeleteTable(2, *workspace);
    allocations = Disarm();
    succeeded = deleted.has_value();
    error = deleted.has_value() ? modern_sqlite::ErrorCode::kGeneric : deleted.error().code();
    cursor_state_valid =
        deleted.has_value()
            ? cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kInvalid
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault;
    failure_latched = deleted.has_value() ||
                      owner.mutation_sequence() == owner.operation_checkpoint() ||
                      pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = cursor_state_valid && failure_latched && rolled_back &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunOverflowTableDelete(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!workspace.has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer payload = FilledBuffer(2'000U, std::byte{0x5a});
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    if (!cursor.has_value() ||
        !cursor
             ->InsertTable(7, payload.view(),
                           modern_sqlite::btree_internal::BtreeInsertMode::kInsertOnly, *workspace)
             .has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool cursor_state_valid = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(
        owner, modern_sqlite::PageNumber{1}, true);
    if (!cursor.has_value()) {
      return {};
    }

    Arm(failure);
    const auto deleted = cursor->DeleteTable(7, *workspace);
    allocations = Disarm();
    succeeded = deleted.has_value();
    error = deleted.has_value() ? modern_sqlite::ErrorCode::kGeneric : deleted.error().code();
    cursor_state_valid =
        deleted.has_value()
            ? cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kInvalid
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault;
    failure_latched = deleted.has_value() ||
                      owner.mutation_sequence() == owner.operation_checkpoint() ||
                      pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = cursor_state_valid && failure_latched && rolled_back &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunIndexDelete(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!geometry.has_value() || !workspace.has_value()) {
    return {};
  }
  const modern_sqlite::RecordCodecOptions options{
      .schema_format = modern_sqlite::RecordSchemaFormat::kFour,
  };
  modern_sqlite::PageNumber root_page;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    const auto root_allocation = modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    const auto left_allocation = modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    const auto right_allocation =
        modern_sqlite::btree_internal::AllocateBtreePage(owner, *geometry);
    if (!root_allocation.has_value() || !left_allocation.has_value() ||
        !right_allocation.has_value()) {
      return {};
    }
    auto root = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, root_allocation->owner_slot, *geometry,
        modern_sqlite::BtreePageType::kInteriorIndex);
    auto left = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, left_allocation->owner_slot, *geometry, modern_sqlite::BtreePageType::kLeafIndex);
    auto right = modern_sqlite::btree_internal::MutableBtreePage::Initialize(
        owner, right_allocation->owner_slot, *geometry, modern_sqlite::BtreePageType::kLeafIndex);
    if (!root.has_value() || !left.has_value() || !right.has_value()) {
      return {};
    }
    root_page = root->page_number();
    struct CellSpec {
      std::size_t index;
      std::int64_t value;
    };
    const auto insert_leaf = [&](modern_sqlite::btree_internal::MutableBtreePage& page,
                                 CellSpec spec) {
      const auto record = modern_sqlite::EncodeRecord(
          std::array{modern_sqlite::SqlValue::Integer(spec.value)}, options);
      if (!record.has_value()) {
        return modern_sqlite::Status{std::unexpected(record.error())};
      }
      const auto cell = modern_sqlite::btree_internal::FillIndexCell(
          owner, *geometry, *workspace, record->view(), modern_sqlite::BtreePageType::kLeafIndex,
          std::nullopt);
      return cell.has_value()
                 ? page.InsertCell(spec.index, cell->bytes, std::nullopt, {}, *workspace)
                 : modern_sqlite::Status{std::unexpected(cell.error())};
    };
    if (!insert_leaf(*left, CellSpec{.index = 0U, .value = 10}).has_value() ||
        !insert_leaf(*left, CellSpec{.index = 1U, .value = 40}).has_value() ||
        !insert_leaf(*right, CellSpec{.index = 0U, .value = 70}).has_value()) {
      return {};
    }
    const auto divider_record =
        modern_sqlite::EncodeRecord(std::array{modern_sqlite::SqlValue::Integer(50)}, options);
    if (!divider_record.has_value()) {
      return {};
    }
    const auto divider = modern_sqlite::btree_internal::FillIndexCell(
        owner, *geometry, *workspace, divider_record->view(),
        modern_sqlite::BtreePageType::kInteriorIndex, left->page_number());
    if (!divider.has_value() ||
        !root->InsertCell(0U, divider->bytes, left->page_number(), {}, *workspace).has_value() ||
        !root->SetRightmostChild(right->page_number()).has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  const std::array<modern_sqlite::IndexColumnOrder, 1> columns{
      modern_sqlite::IndexColumnOrder{modern_sqlite::BinaryCollation()},
  };
  std::array<modern_sqlite::SqlValue, 1> key{modern_sqlite::SqlValue::Integer(50)};

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool cursor_state_valid = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(owner, root_page, false);
    if (!cursor.has_value()) {
      return {};
    }
    ResizablePayloadScratch scratch;

    Arm(failure);
    const auto deleted = cursor->DeleteIndex(key, columns, options, scratch, *workspace);
    allocations = Disarm();
    succeeded = deleted.has_value();
    error = deleted.has_value() ? modern_sqlite::ErrorCode::kGeneric : deleted.error().code();
    cursor_state_valid =
        deleted.has_value()
            ? cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kInvalid
            : cursor->state() == modern_sqlite::btree_internal::WritableCursorState::kFault;
    failure_latched = deleted.has_value() ||
                      owner.mutation_sequence() == owner.operation_checkpoint() ||
                      pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = cursor_state_valid && failure_latched && rolled_back &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunBtreeClear(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value() || !pager->BeginWrite().has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!geometry.has_value() || !workspace.has_value()) {
    return {};
  }
  modern_sqlite::PageNumber root_page;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    const auto root = modern_sqlite::btree_internal::CreateBtreeRoot(owner, *geometry, true);
    if (!root.has_value()) {
      return {};
    }
    root_page = *root;
  }
  for (std::int64_t rowid = 1; rowid <= 6; ++rowid) {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto cursor = modern_sqlite::btree_internal::WritableCursor::Open(owner, root_page, true);
    const modern_sqlite::ByteBuffer payload =
        FilledBuffer(380U, static_cast<std::byte>(static_cast<std::uint8_t>(rowid)));
    if (!cursor.has_value() ||
        !cursor
             ->InsertTable(rowid, payload.view(),
                           modern_sqlite::btree_internal::BtreeInsertMode::kInsertOnly, *workspace)
             .has_value()) {
      return {};
    }
  }
  if (!pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  std::vector<modern_sqlite::ByteBuffer> original_pages;
  original_pages.reserve(pager->page_count());
  for (std::uint32_t page = 1U; page <= pager->page_count(); ++page) {
    const auto pin = pager->ReadPage(modern_sqlite::PageNumber{page});
    if (!pin.has_value()) {
      return {};
    }
    original_pages.push_back(modern_sqlite::ByteBuffer::CopyOf(pin->frame().bytes()));
  }
  if (!pager->BeginWrite().has_value()) {
    return {};
  }

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  bool count_matches = false;
  bool failure_latched = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    Arm(failure);
    const auto cleared = modern_sqlite::btree_internal::ClearBtree(owner, *geometry, root_page);
    allocations = Disarm();
    succeeded = cleared.has_value();
    count_matches = !cleared.has_value() || *cleared == 6U;
    error = cleared.has_value() ? modern_sqlite::ErrorCode::kGeneric : cleared.error().code();
    failure_latched = cleared.has_value() ||
                      owner.mutation_sequence() == owner.operation_checkpoint() ||
                      pager->write_failure_code() == modern_sqlite::ErrorCode::kOutOfMemory;
  }
  const bool rolled_back = pager->Rollback().has_value();
  bool cache_restored = rolled_back && pager->page_count() == original_pages.size();
  for (std::size_t index = 0U; cache_restored && index < original_pages.size(); ++index) {
    const auto pin =
        pager->ReadPage(modern_sqlite::PageNumber{static_cast<std::uint32_t>(index + 1U)});
    cache_restored =
        pin.has_value() && std::ranges::equal(original_pages[index].view(), pin->frame().bytes());
  }
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = count_matches && failure_latched && cache_restored &&
                         std::ranges::equal(original.view(), vfs.database_bytes()),
  };
}

[[nodiscard]] Outcome RunRootDeepening(std::optional<std::size_t> failure) {
  failing_allocation.reset();
  modern_sqlite::test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<modern_sqlite::Pager> pager = modern_sqlite::test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value() ||
      !modern_sqlite::test::InitializeEmptyBtreeImage(*pager).has_value() ||
      !pager->Commit().has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer original =
      modern_sqlite::ByteBuffer::CopyOf(vfs.database_bytes());
  if (!pager->BeginWrite().has_value()) {
    return {};
  }
  const auto geometry = modern_sqlite::BtreePageGeometry::Create(pager->header()->page_size(),
                                                                 pager->header()->usable_size());
  auto workspace = modern_sqlite::btree_internal::BtreeWriteWorkspace::Create(pager->page_size());
  if (!geometry.has_value() || !workspace.has_value()) {
    return {};
  }
  const modern_sqlite::ByteBuffer old_payload = FilledBuffer(380U, std::byte{0x31});
  const modern_sqlite::ByteBuffer new_payload = FilledBuffer(20U, std::byte{0x72});

  modern_sqlite::ErrorCode error = modern_sqlite::ErrorCode::kGeneric;
  bool succeeded = false;
  std::size_t allocations = 0U;
  {
    modern_sqlite::btree_internal::MutationPageOwner owner{*pager};
    auto root_slot = owner.AcquireWrite(modern_sqlite::PageNumber{1});
    if (!root_slot.has_value()) {
      return {};
    }
    auto root = modern_sqlite::btree_internal::MutableBtreePage::Open(owner, *root_slot, *geometry);
    if (!root.has_value()) {
      return {};
    }
    const auto old_cell = modern_sqlite::btree_internal::FillTableLeafCell(
        owner, *geometry, *workspace, 1, old_payload.view());
    if (!old_cell.has_value() ||
        !root->InsertCell(0U, old_cell->bytes, std::nullopt, {}, *workspace).has_value()) {
      return {};
    }
    const auto new_cell = modern_sqlite::btree_internal::FillTableLeafCell(
        owner, *geometry, *workspace, 2, new_payload.view());
    std::array<std::byte, 32> staged_copy{};
    if (!new_cell.has_value() ||
        !root->InsertCell(1U, new_cell->bytes, std::nullopt,
                          modern_sqlite::MutableByteView{staged_copy}, *workspace)
             .has_value()) {
      return {};
    }

    Arm(failure);
    const auto deepened = modern_sqlite::btree_internal::MutableBtreePage::BalanceDeeper(*root);
    allocations = Disarm();
    succeeded = deepened.has_value();
    error = deepened.has_value() ? modern_sqlite::ErrorCode::kGeneric : deepened.error().code();
  }
  const bool rolled_back = pager->Rollback().has_value();
  return Outcome{
      .allocations = allocations,
      .succeeded = succeeded,
      .error = error,
      .invariant_holds = rolled_back && std::ranges::equal(original.view(), vfs.database_bytes()),
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
  if (!ExhaustAllocations(RunQuickBalance)) {
    return 7;
  }
  if (!ExhaustAllocations(RunNonrootBalance)) {
    return 8;
  }
  if (!ExhaustAllocations(RunCursorBalance)) {
    return 9;
  }
  if (!ExhaustAllocations(RunTableInsert)) {
    return 10;
  }
  if (!ExhaustAllocations(RunTableReplace)) {
    return 11;
  }
  if (!ExhaustAllocations(RunIndexInsert)) {
    return 12;
  }
  if (!ExhaustAllocations(RunTableDelete)) {
    return 13;
  }
  if (!ExhaustAllocations(RunOverflowTableDelete)) {
    return 14;
  }
  if (!ExhaustAllocations(RunIndexDelete)) {
    return 15;
  }
  if (!ExhaustAllocations(RunBtreeClear)) {
    return 16;
  }
  if (!ExhaustAllocations(RunRootDeepening)) {
    return 17;
  }
  if (!ExhaustAllocations(RunOverflowSeek)) {
    return 18;
  }
  if (!ExhaustAllocations(RunOverflowFormat)) {
    return 19;
  }
  if (!ExhaustAllocations(RunFreelistLeafMark)) {
    return 20;
  }
  return 0;
} catch (...) {
  failing_allocation.reset();
  return 21;
}
