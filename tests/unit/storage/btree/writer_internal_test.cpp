#include "storage/btree/writer_internal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "modern_sqlite/storage/page_number.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite::btree_internal {
namespace {

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

void RequireStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

void Store16(MutableByteView bytes, std::size_t offset, std::uint16_t value) {
  StoreBigEndian<std::uint16_t>(
      std::span<std::byte, sizeof(value)>{bytes.data() + offset, sizeof(value)}, value);
}

void Store32(MutableByteView bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(value)>{bytes.data() + offset, sizeof(value)}, value);
}

void WriteTableLeaf(MutableByteView bytes, bool page_one, std::int64_t rowid) {
  const std::size_t header = page_one ? 100U : 0U;
  std::ranges::fill(bytes.subspan(header), std::byte{0});
  bytes[header] = static_cast<std::byte>(BtreePageType::kLeafTable);
  Store16(bytes, header + 3U, 1U);
  const std::size_t cell_offset = bytes.size() - 4U;
  Store16(bytes, header + 5U, static_cast<std::uint16_t>(cell_offset));
  Store16(bytes, header + 8U, static_cast<std::uint16_t>(cell_offset));
  bytes[cell_offset] = std::byte{0};
  std::array<std::byte, 9> encoded{};
  const auto size =
      EncodeSqliteVarint(std::bit_cast<std::uint64_t>(rowid), MutableByteView{encoded});
  ASSERT_TRUE(size.has_value());
  ASSERT_LE(size->value(), 3U);
  std::ranges::copy(std::span{encoded}.first(size->value()),
                    bytes.begin() + static_cast<std::ptrdiff_t>(cell_offset + 1U));
}

void WriteTableInterior(MutableByteView bytes, bool page_one, PageNumber left_child,
                        PageNumber right_child, std::int64_t separator) {
  const std::size_t header = page_one ? 100U : 0U;
  std::ranges::fill(bytes.subspan(header), std::byte{0});
  bytes[header] = static_cast<std::byte>(BtreePageType::kInteriorTable);
  Store16(bytes, header + 3U, 1U);
  Store32(bytes, header + 8U, right_child.value());
  std::array<std::byte, 9> encoded{};
  const auto size =
      EncodeSqliteVarint(std::bit_cast<std::uint64_t>(separator), MutableByteView{encoded});
  ASSERT_TRUE(size.has_value());
  const std::size_t cell_size = 4U + size->value();
  const std::size_t cell_offset = bytes.size() - cell_size;
  Store16(bytes, header + 5U, static_cast<std::uint16_t>(cell_offset));
  Store16(bytes, header + 12U, static_cast<std::uint16_t>(cell_offset));
  Store32(bytes, cell_offset, left_child.value());
  std::ranges::copy(std::span{encoded}.first(size->value()),
                    bytes.begin() + static_cast<std::ptrdiff_t>(cell_offset + 4U));
}

void WriteIndexPage(MutableByteView bytes, BtreePageType type, ByteView record,
                    std::optional<PageNumber> left_child = std::nullopt,
                    std::optional<PageNumber> right_child = std::nullopt) {
  std::ranges::fill(bytes, std::byte{0});
  bytes[0] = static_cast<std::byte>(type);
  Store16(bytes, 3U, 1U);
  const bool leaf = type == BtreePageType::kLeafIndex;
  ASSERT_EQ(leaf, !left_child.has_value());
  if (!leaf) {
    ASSERT_TRUE(right_child.has_value());
    Store32(bytes, 8U, right_child->value());
  }
  std::array<std::byte, 9> encoded_size{};
  const auto size = EncodeSqliteVarint(record.size(), MutableByteView{encoded_size});
  ASSERT_TRUE(size.has_value());
  const std::size_t child_bytes = leaf ? 0U : sizeof(std::uint32_t);
  const std::size_t cell_size = child_bytes + size->value() + record.size();
  const std::size_t cell_offset = bytes.size() - cell_size;
  Store16(bytes, 5U, static_cast<std::uint16_t>(cell_offset));
  Store16(bytes, leaf ? 8U : 12U, static_cast<std::uint16_t>(cell_offset));
  if (left_child.has_value()) {
    Store32(bytes, cell_offset, left_child->value());
  }
  std::ranges::copy(std::span{encoded_size}.first(size->value()),
                    bytes.begin() + static_cast<std::ptrdiff_t>(cell_offset + child_bytes));
  std::ranges::copy(record, bytes.begin() + static_cast<std::ptrdiff_t>(cell_offset + child_bytes +
                                                                        size->value()));
}

[[nodiscard]] std::size_t IndexLocalPayload(std::size_t payload_size, BtreePageGeometry geometry) {
  const std::size_t minimum = geometry.minimum_local_payload().value();
  const std::size_t maximum = geometry.maximum_index_local_payload().value();
  if (payload_size <= maximum) {
    return payload_size;
  }
  const std::size_t candidate =
      minimum + (payload_size - minimum) % geometry.overflow_payload_capacity().value();
  return candidate <= maximum ? candidate : minimum;
}

void WriteOverflowIndexLeaf(MutableByteView bytes, ByteView record, PageNumber first_overflow,
                            BtreePageGeometry geometry) {
  std::ranges::fill(bytes, std::byte{0});
  bytes[0] = static_cast<std::byte>(BtreePageType::kLeafIndex);
  Store16(bytes, 3U, 1U);
  std::array<std::byte, 9> encoded_size{};
  const auto size = EncodeSqliteVarint(record.size(), MutableByteView{encoded_size});
  ASSERT_TRUE(size.has_value());
  const std::size_t local = IndexLocalPayload(record.size(), geometry);
  ASSERT_LT(local, record.size());
  const std::size_t cell_size = size->value() + local + sizeof(std::uint32_t);
  const std::size_t cell_offset = bytes.size() - cell_size;
  Store16(bytes, 5U, static_cast<std::uint16_t>(cell_offset));
  Store16(bytes, 8U, static_cast<std::uint16_t>(cell_offset));
  std::ranges::copy(std::span{encoded_size}.first(size->value()),
                    bytes.begin() + static_cast<std::ptrdiff_t>(cell_offset));
  std::ranges::copy(record.first(local),
                    bytes.begin() + static_cast<std::ptrdiff_t>(cell_offset + size->value()));
  Store32(bytes, cell_offset + size->value() + local, first_overflow.value());
}

[[nodiscard]] std::unique_ptr<Pager> OpenInitialized(test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return nullptr;
  }
  auto session = BtreeWriteSession::Open(*pager);
  if (!session.has_value() || !session->InitializeDatabase().has_value() ||
      !pager->Commit().has_value()) {
    return nullptr;
  }
  return pager;
}

void BuildTableChain(Pager& pager, std::size_t depth) {
  ASSERT_GE(depth, 1U);
  ASSERT_TRUE(pager.BeginWrite().has_value());
  for (std::size_t page = pager.page_count() + 1U; page <= depth; ++page) {
    const auto allocated = pager.AllocatePage();
    ASSERT_TRUE(allocated.has_value());
  }
  for (std::size_t page = 1U; page < depth; ++page) {
    auto pin = pager.WritePage(PageNumber{static_cast<std::uint32_t>(page)});
    ASSERT_TRUE(pin.has_value());
    const PageNumber child{static_cast<std::uint32_t>(page + 1U)};
    WriteTableInterior(pin->mutable_bytes(), page == 1U, child, child, 100);
  }
  {
    auto leaf = pager.WritePage(PageNumber{static_cast<std::uint32_t>(depth)});
    ASSERT_TRUE(leaf.has_value());
    WriteTableLeaf(leaf->mutable_bytes(), depth == 1U, 1);
  }
  {
    auto page_one = pager.WritePage(PageNumber{1});
    ASSERT_TRUE(page_one.has_value());
    Store32(page_one->mutable_bytes(), 28U, static_cast<std::uint32_t>(depth));
  }
  ASSERT_TRUE(pager.Commit().has_value());
}

TEST(MutationPageOwner, BorrowsExplicitlyAndRejectsStructuralAliases) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 4U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  MutationPageOwner owner{*pager};

  const std::size_t slot = TakeValue(owner.AcquireRead(PageNumber{2}));
  EXPECT_EQ(slot, TakeValue(owner.Borrow(PageNumber{2})));
  const auto duplicate = owner.AcquireRead(PageNumber{2});
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, duplicate.error().code());
  EXPECT_EQ(1U, owner.size());

  owner.Release(slot);
  EXPECT_EQ(0U, owner.size());
  RequireStatus(pager->Rollback());
}

TEST(MutationPageOwner, PromotesInPlaceAndRejectsAnExternalAlias) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 4U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  MutationPageOwner owner{*pager};
  {
    const auto external = TakeValue(pager->ReadPage(PageNumber{2}));
    const std::size_t aliased_slot = TakeValue(owner.AcquireRead(PageNumber{2}));

    const auto busy = owner.Promote(aliased_slot);
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(ErrorCode::kBusy, busy.error().code());
    EXPECT_EQ(0U, owner.size());
  }

  const std::size_t slot = TakeValue(owner.AcquireRead(PageNumber{2}));
  const PageFrame* const frame = &TakeValue(owner.Frame(slot)).get();
  RequireStatus(owner.Promote(slot));
  EXPECT_TRUE(owner.IsWritable(slot));
  EXPECT_EQ(frame, &TakeValue(owner.Frame(slot)).get());
  TakeValue(owner.MutableBytes(slot))[100] = std::byte{0x55};
  owner.Release(slot);
  RequireStatus(pager->Rollback());
}

TEST(MutationPageOwner, RejectsUseAfterWriteGenerationChanges) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 4U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  MutationPageOwner owner{*pager};
  const std::size_t slot = TakeValue(owner.AcquireRead(PageNumber{2}));
  owner.Release(slot);
  RequireStatus(pager->Rollback());
  RequireStatus(pager->BeginWrite());

  const auto stale = owner.AcquireRead(PageNumber{2});
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale.error().code());
  RequireStatus(pager->Rollback());
}

TEST(WritableCursor, SeeksTableRowsAndRetainsThePath) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  BuildTableChain(*pager, 3U);
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));

    const TableSeekResult found = TakeValue(cursor.SeekTable(1));

    EXPECT_TRUE(found.exact);
    EXPECT_EQ(0, found.comparison);
    EXPECT_EQ(0U, found.insertion_index);
    EXPECT_EQ(3U, found.tree_depth);
    EXPECT_EQ(3U, cursor.depth());
    EXPECT_EQ(3U, owner.size());
    RequireStatus(cursor.PromoteCurrent());
    EXPECT_TRUE(owner.IsWritable(cursor.current_owner_slot()));
    RequireStatus(cursor.MoveToParent());
    EXPECT_EQ(2U, cursor.depth());
    EXPECT_EQ(2U, owner.size());
  }
  RequireStatus(pager->Rollback());
}

TEST(WritableCursor, AcceptsTwentyLevelsAndRejectsTwentyOne) {
  {
    test::WritePagerFixedVfs vfs{false};
    std::unique_ptr<Pager> pager = OpenInitialized(vfs);
    ASSERT_NE(nullptr, pager);
    BuildTableChain(*pager, kMaximumBtreeDepth);
    RequireStatus(pager->BeginWrite());
    {
      MutationPageOwner owner{*pager};
      WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
      EXPECT_TRUE(TakeValue(cursor.SeekTable(1)).exact);
      EXPECT_EQ(kMaximumBtreeDepth, cursor.depth());
    }
    RequireStatus(pager->Rollback());
  }
  {
    test::WritePagerFixedVfs vfs{false};
    std::unique_ptr<Pager> pager = OpenInitialized(vfs);
    ASSERT_NE(nullptr, pager);
    BuildTableChain(*pager, kMaximumBtreeDepth + 1U);
    RequireStatus(pager->BeginWrite());
    {
      MutationPageOwner owner{*pager};
      WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
      const auto too_deep = cursor.SeekTable(1);
      ASSERT_FALSE(too_deep.has_value());
      EXPECT_EQ(ErrorCode::kCorruption, too_deep.error().code());
      EXPECT_EQ(WritableCursorState::kFault, cursor.state());
    }
    RequireStatus(pager->Rollback());
  }
}

TEST(WritableCursor, SeeksInteriorAndLeafIndexEntriesWithSharedRecordComparison) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  for (std::uint32_t page = 2U; page <= 4U; ++page) {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{page}, allocated.frame().page_number());
  }
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const ByteBuffer ten = TakeValue(EncodeRecord(std::array{SqlValue::Integer(10)}, options));
  const ByteBuffer fifty = TakeValue(EncodeRecord(std::array{SqlValue::Integer(50)}, options));
  const ByteBuffer seventy = TakeValue(EncodeRecord(std::array{SqlValue::Integer(70)}, options));
  {
    auto root = TakeValue(pager->WritePage(PageNumber{2}));
    WriteIndexPage(root.mutable_bytes(), BtreePageType::kInteriorIndex, fifty.view(), PageNumber{3},
                   PageNumber{4});
  }
  {
    auto left = TakeValue(pager->WritePage(PageNumber{3}));
    WriteIndexPage(left.mutable_bytes(), BtreePageType::kLeafIndex, ten.view());
  }
  {
    auto right = TakeValue(pager->WritePage(PageNumber{4}));
    WriteIndexPage(right.mutable_bytes(), BtreePageType::kLeafIndex, seventy.view());
  }
  {
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    Store32(page_one.mutable_bytes(), 28U, 4U);
  }
  RequireStatus(pager->Commit());
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{2}, false));
    const std::array<IndexColumnOrder, 1> columns{
        IndexColumnOrder{BinaryCollation()},
    };
    std::vector<std::byte> scratch;

    const IndexSeekResult interior =
        TakeValue(cursor.SeekIndex(std::array{SqlValue::Integer(50)}, columns, options, scratch));
    EXPECT_TRUE(interior.exact);
    EXPECT_EQ(1U, interior.tree_depth);

    const IndexSeekResult leaf =
        TakeValue(cursor.SeekIndex(std::array{SqlValue::Integer(70)}, columns, options, scratch));
    EXPECT_TRUE(leaf.exact);
    EXPECT_EQ(2U, leaf.tree_depth);

    const IndexSeekResult gap =
        TakeValue(cursor.SeekIndex(std::array{SqlValue::Integer(55)}, columns, options, scratch));
    EXPECT_FALSE(gap.exact);
    EXPECT_EQ(1, gap.comparison);
    EXPECT_EQ(0U, gap.insertion_index);
    EXPECT_EQ(2U, gap.tree_depth);
    EXPECT_EQ(2U, owner.size());
  }
  RequireStatus(pager->Rollback());
}

TEST(WritableCursor, SeeksOverflowIndexEntriesAndRejectsPathAliasing) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  ByteBuffer blob{ByteCount{700}};
  std::ranges::fill(blob.mutable_view(), std::byte{0x5a});
  std::array<SqlValue, 1> key{SqlValue::Blob(std::move(blob))};
  const ByteBuffer record = TakeValue(EncodeRecord(key, options));
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const std::size_t local = IndexLocalPayload(record.size().value(), geometry);

  RequireStatus(pager->BeginWrite());
  for (std::uint32_t page = 2U; page <= 4U; ++page) {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{page}, allocated.frame().page_number());
  }
  {
    auto root = TakeValue(pager->WritePage(PageNumber{2}));
    WriteOverflowIndexLeaf(root.mutable_bytes(), record.view(), PageNumber{3}, geometry);
  }
  {
    auto first = TakeValue(pager->WritePage(PageNumber{3}));
    std::ranges::fill(first.mutable_bytes(), std::byte{0});
    Store32(first.mutable_bytes(), 0U, 4U);
    const std::size_t count =
        std::min(geometry.overflow_payload_capacity().value(), record.size().value() - local);
    std::ranges::copy(
        record.view().subspan(local, count),
        first.mutable_bytes().begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
  }
  {
    auto second = TakeValue(pager->WritePage(PageNumber{4}));
    std::ranges::fill(second.mutable_bytes(), std::byte{0});
    const std::size_t first_count =
        std::min(geometry.overflow_payload_capacity().value(), record.size().value() - local);
    std::ranges::copy(
        record.view().subspan(local + first_count),
        second.mutable_bytes().begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
  }
  {
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    Store32(page_one.mutable_bytes(), 28U, 4U);
  }
  RequireStatus(pager->Commit());

  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{2}, false));
    std::vector<std::byte> scratch;
    const IndexSeekResult found = TakeValue(cursor.SeekIndex(key, columns, options, scratch));
    EXPECT_TRUE(found.exact);
    EXPECT_EQ(record.size().value(), scratch.size());
    EXPECT_EQ(1U, owner.size());
  }
  RequireStatus(pager->Rollback());

  RequireStatus(pager->BeginWrite());
  {
    auto root = TakeValue(pager->WritePage(PageNumber{2}));
    const auto parsed =
        TakeValue(BtreePageView::Parse(root.frame().bytes(), PageNumber{2}, geometry));
    const auto cell = TakeValue(parsed.cell(0));
    ASSERT_TRUE(cell.first_overflow_page().has_value());
    const std::size_t pointer_offset = TakeValue(parsed.cell_offset(0)).value() +
                                       cell.encoded_size().value() - sizeof(std::uint32_t);
    Store32(root.mutable_bytes(), pointer_offset, 2U);
  }
  RequireStatus(pager->Commit());
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{2}, false));
    std::vector<std::byte> scratch;
    const auto aliased = cursor.SeekIndex(key, columns, options, scratch);
    ASSERT_FALSE(aliased.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, aliased.error().code());
    EXPECT_EQ(1U, owner.size());
  }
  RequireStatus(pager->Rollback());
}

TEST(WritableCursor, RejectsAnAncestorCycleWithoutAcquiringAnotherPin) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    auto root = TakeValue(pager->WritePage(PageNumber{1}));
    WriteTableInterior(root.mutable_bytes(), true, PageNumber{1}, PageNumber{1}, 100);
  }
  RequireStatus(pager->Commit());
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));

    const auto cycle = cursor.SeekTable(1);

    ASSERT_FALSE(cycle.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, cycle.error().code());
    EXPECT_EQ(1U, owner.size());
    EXPECT_EQ(WritableCursorState::kFault, cursor.state());
  }
  RequireStatus(pager->Rollback());
}

}  // namespace
}  // namespace modern_sqlite::btree_internal
