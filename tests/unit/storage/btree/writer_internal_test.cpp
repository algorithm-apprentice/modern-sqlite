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
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/page_number.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite::btree_internal {
namespace {

// BYTE_VECTOR_RESIZABLE_SCRATCH: WritableCursor grows decoded overflow keys in place.
using ResizablePayloadScratch = std::vector<std::byte>;

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

[[nodiscard]] std::uint32_t Load32(ByteView bytes, std::size_t offset) {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + offset, sizeof(std::uint32_t)});
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

[[nodiscard]] ByteBuffer FilledBuffer(std::size_t size, std::byte value) {
  ByteBuffer buffer{ByteCount{size}};
  std::ranges::fill(buffer.mutable_view(), value);
  return buffer;
}

[[nodiscard]] ByteBuffer TableLeafCell(std::int64_t rowid, std::size_t payload_size) {
  std::array<std::byte, 9> encoded_payload{};
  std::array<std::byte, 9> encoded_rowid{};
  const auto payload_header = EncodeSqliteVarint(payload_size, MutableByteView{encoded_payload});
  const auto rowid_header =
      EncodeSqliteVarint(std::bit_cast<std::uint64_t>(rowid), MutableByteView{encoded_rowid});
  if (!payload_header.has_value() || !rowid_header.has_value()) {
    throw std::runtime_error("failed to encode test table cell");
  }
  const std::size_t encoded_size =
      std::max<std::size_t>(4U, payload_header->value() + rowid_header->value() + payload_size);
  ByteBuffer cell{ByteCount{encoded_size}};
  const MutableByteView bytes = cell.mutable_view();
  std::ranges::copy(std::span{encoded_payload}.first(payload_header->value()), bytes.begin());
  std::ranges::copy(std::span{encoded_rowid}.first(rowid_header->value()),
                    bytes.begin() + static_cast<std::ptrdiff_t>(payload_header->value()));
  std::ranges::fill(bytes.subspan(payload_header->value() + rowid_header->value(), payload_size),
                    static_cast<std::byte>(static_cast<std::uint64_t>(rowid) & 0xffU));
  return cell;
}

struct TableLeafSpec {
  std::int64_t rowid;
  std::size_t payload_size;
};

[[nodiscard]] Status InsertTableLeafCell(MutableBtreePage& page, std::size_t index,
                                         TableLeafSpec spec, MutableByteView staged_copy,
                                         BtreeWriteWorkspace& workspace) {
  const ByteBuffer cell = TableLeafCell(spec.rowid, spec.payload_size);
  return page.InsertCell(index, cell.view(), std::nullopt, staged_copy, workspace);
}

[[nodiscard]] Status AppendTableLeafCell(CellArray& cells, TableLeafSpec spec) {
  const ByteBuffer cell = TableLeafCell(spec.rowid, spec.payload_size);
  return cells.AppendCopied(cell.view());
}

[[nodiscard]] ByteBuffer TableLeafCellWithRowid(ByteView encoded_rowid, std::size_t payload_size) {
  std::array<std::byte, 9> encoded_payload{};
  const auto payload_header = EncodeSqliteVarint(payload_size, MutableByteView{encoded_payload});
  if (!payload_header.has_value() || encoded_rowid.empty()) {
    throw std::runtime_error("failed to encode test table cell");
  }
  const std::size_t encoded_size =
      std::max<std::size_t>(4U, payload_header->value() + encoded_rowid.size() + payload_size);
  ByteBuffer cell{ByteCount{encoded_size}};
  const MutableByteView bytes = cell.mutable_view();
  std::ranges::copy(std::span{encoded_payload}.first(payload_header->value()), bytes.begin());
  std::ranges::copy(encoded_rowid,
                    bytes.begin() + static_cast<std::ptrdiff_t>(payload_header->value()));
  std::ranges::fill(bytes.subspan(payload_header->value() + encoded_rowid.size(), payload_size),
                    std::byte{0x5a});
  return cell;
}

[[nodiscard]] ByteBuffer TableInteriorCell(PageNumber left_child, std::int64_t rowid) {
  std::array<std::byte, 9> encoded_rowid{};
  const auto rowid_header =
      EncodeSqliteVarint(std::bit_cast<std::uint64_t>(rowid), MutableByteView{encoded_rowid});
  if (!rowid_header.has_value()) {
    throw std::runtime_error("failed to encode test table divider");
  }
  ByteBuffer cell{ByteCount{sizeof(std::uint32_t) + rowid_header->value()}};
  const MutableByteView bytes = cell.mutable_view();
  Store32(bytes, 0U, left_child.value());
  std::ranges::copy(std::span{encoded_rowid}.first(rowid_header->value()),
                    bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
  return cell;
}

[[nodiscard, maybe_unused]] ByteBuffer IndexCell(ByteView payload,
                                                 std::optional<PageNumber> left_child) {
  std::array<std::byte, 9> encoded_payload{};
  const auto payload_header = EncodeSqliteVarint(payload.size(), MutableByteView{encoded_payload});
  if (!payload_header.has_value()) {
    throw std::runtime_error("failed to encode test index cell");
  }
  const std::size_t child_bytes = left_child.has_value() ? sizeof(std::uint32_t) : 0U;
  const std::size_t encoded_size =
      std::max<std::size_t>(4U, child_bytes + payload_header->value() + payload.size());
  ByteBuffer cell{ByteCount{encoded_size}};
  const MutableByteView bytes = cell.mutable_view();
  std::ranges::fill(bytes, std::byte{0});
  if (left_child.has_value()) {
    Store32(bytes, 0U, left_child->value());
  }
  std::ranges::copy(std::span{encoded_payload}.first(payload_header->value()),
                    bytes.begin() + static_cast<std::ptrdiff_t>(child_bytes));
  std::ranges::copy(
      payload, bytes.begin() + static_cast<std::ptrdiff_t>(child_bytes + payload_header->value()));
  return cell;
}

[[nodiscard]] ByteBuffer CellImagePage(ByteView cell, BtreePageType type,
                                       BtreePageGeometry geometry) {
  ByteBuffer page{geometry.page_size()};
  const MutableByteView bytes = page.mutable_view();
  if (bytes.size() < 12U || cell.size() > geometry.usable_size().value() - 12U) {
    throw std::runtime_error("test cell image does not fit its page");
  }
  bytes[0] = static_cast<std::byte>(type);
  Store16(bytes, 3U, 1U);
  const bool leaf = type == BtreePageType::kLeafIndex || type == BtreePageType::kLeafTable;
  const std::size_t header_size = leaf ? 8U : 12U;
  if (!leaf) {
    Store32(bytes, 8U, 99U);
  }
  const std::size_t offset = geometry.usable_size().value() - cell.size();
  Store16(bytes, 5U, static_cast<std::uint16_t>(offset));
  Store16(bytes, header_size, static_cast<std::uint16_t>(offset));
  std::ranges::copy(cell, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  return page;
}

[[nodiscard]] std::unique_ptr<Pager> OpenInitialized(test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return nullptr;
  }
  if (!test::InitializeEmptyBtreeImage(*pager).has_value() || !pager->Commit().has_value()) {
    return nullptr;
  }
  return pager;
}

[[nodiscard]] ByteBuffer SnapshotPage(Pager& pager, const MutationPageOwner& owner,
                                      PageNumber page_number) {
  if (const std::optional<std::size_t> slot = owner.Find(page_number); slot.has_value()) {
    return ByteBuffer::CopyOf(TakeValue(owner.Frame(*slot)).get().bytes());
  }
  return ByteBuffer::CopyOf(TakeValue(pager.ReadPage(page_number)).frame().bytes());
}

[[nodiscard]] ByteBuffer SnapshotPage(Pager& pager, PageNumber page_number) {
  return ByteBuffer::CopyOf(TakeValue(pager.ReadPage(page_number)).frame().bytes());
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

TEST(BtreeFreelist, AppendsThenReusesAnEmptyTrunk) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage appended = TakeValue(AllocateBtreePage(owner, geometry));
    EXPECT_EQ(PageNumber{2}, appended.page_number);
    EXPECT_FALSE(appended.reused_freelist);
    const ByteView page_one =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{1})))).get().bytes();
    EXPECT_EQ(2U, LoadBigEndian<std::uint32_t>(page_one.subspan<28U, 4U>()));
    EXPECT_EQ(0U, LoadBigEndian<std::uint32_t>(page_one.subspan<36U, 4U>()));
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    RequireStatus(FreeBtreePage(owner, geometry, PageNumber{2}));
    const std::size_t page_one_slot = TakeValue(owner.Borrow(PageNumber{1}));
    const ByteView page_one = TakeValue(owner.Frame(page_one_slot)).get().bytes();
    EXPECT_EQ(2U, LoadBigEndian<std::uint32_t>(page_one.subspan<32U, 4U>()));
    EXPECT_EQ(1U, LoadBigEndian<std::uint32_t>(page_one.subspan<36U, 4U>()));
    const std::size_t trunk_slot = TakeValue(owner.Borrow(PageNumber{2}));
    const ByteView trunk = TakeValue(owner.Frame(trunk_slot)).get().bytes();
    EXPECT_EQ(0U, LoadBigEndian<std::uint32_t>(trunk.first<4U>()));
    EXPECT_EQ(0U, LoadBigEndian<std::uint32_t>(trunk.subspan<4U, 4U>()));
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage reused = TakeValue(AllocateBtreePage(owner, geometry));
    EXPECT_EQ(PageNumber{2}, reused.page_number);
    EXPECT_TRUE(reused.reused_freelist);
    const ByteView page_one =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{1})))).get().bytes();
    EXPECT_EQ(0U, LoadBigEndian<std::uint32_t>(page_one.subspan<32U, 4U>()));
    EXPECT_EQ(0U, LoadBigEndian<std::uint32_t>(page_one.subspan<36U, 4U>()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeFreelist, AppendsAndExtractsLeavesInSQLiteOrder) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    EXPECT_EQ(PageNumber{2}, TakeValue(AllocateBtreePage(owner, geometry)).page_number);
    EXPECT_EQ(PageNumber{3}, TakeValue(AllocateBtreePage(owner, geometry)).page_number);
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    RequireStatus(FreeBtreePage(owner, geometry, PageNumber{2}));
    RequireStatus(FreeBtreePage(owner, geometry, PageNumber{3}));
    EXPECT_TRUE(pager->PageContentRequired(PageNumber{3}));
    const ByteView trunk =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{2})))).get().bytes();
    EXPECT_EQ(1U, LoadBigEndian<std::uint32_t>(trunk.subspan<4U, 4U>()));
    EXPECT_EQ(3U, LoadBigEndian<std::uint32_t>(trunk.subspan<8U, 4U>()));
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage leaf = TakeValue(AllocateBtreePage(owner, geometry));
    EXPECT_EQ(PageNumber{3}, leaf.page_number);
    EXPECT_TRUE(leaf.reused_freelist);
    EXPECT_FALSE(owner.Find(PageNumber{2}).has_value());
    {
      const auto trunk = TakeValue(pager->ReadPage(PageNumber{2}));
      EXPECT_EQ(0U, LoadBigEndian<std::uint32_t>(trunk.frame().bytes().subspan<4U, 4U>()));
    }
    const AllocatedBtreePage trunk_page = TakeValue(AllocateBtreePage(owner, geometry));
    EXPECT_EQ(PageNumber{2}, trunk_page.page_number);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeFreelist, UsesTheHistoricalTrunkLeafLimit) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  constexpr std::uint32_t kLeafCount = 512U / 4U - 8U;
  RequireStatus(pager->BeginWrite());
  {
    for (std::uint32_t page = 2U; page <= kLeafCount + 3U; ++page) {
      const auto allocated = TakeValue(pager->AllocatePage());
      EXPECT_EQ(PageNumber{page}, allocated.frame().page_number());
    }
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    Store32(page_one.mutable_bytes(), 28U, kLeafCount + 3U);
    Store32(page_one.mutable_bytes(), 32U, 2U);
    Store32(page_one.mutable_bytes(), 36U, kLeafCount + 1U);
    auto trunk = TakeValue(pager->WritePage(PageNumber{2}));
    Store32(trunk.mutable_bytes(), 0U, 0U);
    Store32(trunk.mutable_bytes(), 4U, kLeafCount);
    for (std::uint32_t index = 0U; index < kLeafCount; ++index) {
      Store32(trunk.mutable_bytes(), 8U + index * 4U, 3U + index);
    }
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const PageNumber freed{kLeafCount + 3U};
    RequireStatus(FreeBtreePage(owner, geometry, freed));
    const ByteView page_one =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{1})))).get().bytes();
    EXPECT_EQ(freed.value(), LoadBigEndian<std::uint32_t>(page_one.subspan<32U, 4U>()));
    const ByteView new_trunk = TakeValue(owner.Frame(TakeValue(owner.Borrow(freed)))).get().bytes();
    EXPECT_EQ(2U, LoadBigEndian<std::uint32_t>(new_trunk.first<4U>()));
    EXPECT_EQ(0U, LoadBigEndian<std::uint32_t>(new_trunk.subspan<4U, 4U>()));
    EXPECT_FALSE(owner.Find(PageNumber{2}).has_value());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeFreelist, RejectsImpossibleHeaderCountsBeforeMutation) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  RequireStatus(pager->BeginWrite());
  {
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    Store32(page_one.mutable_bytes(), 32U, 1U);
    Store32(page_one.mutable_bytes(), 36U, 1U);
  }
  ByteBuffer before;
  {
    const auto page_one = TakeValue(pager->ReadPage(PageNumber{1}));
    before = ByteBuffer::CopyOf(page_one.frame().bytes());
  }
  {
    MutationPageOwner owner{*pager};
    const auto allocated = AllocateBtreePage(owner, geometry);
    ASSERT_FALSE(allocated.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, allocated.error().code());
  }
  {
    const auto after = TakeValue(pager->ReadPage(PageNumber{1}));
    EXPECT_TRUE(std::ranges::equal(before.view(), after.frame().bytes()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeFreelist, RejectsAppendBeyondSqlitesMaximumPageNumber) {
  test::WritePagerFixedVfs vfs;
  vfs.SetReportedPageCount(0xfffffffeU);
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 4U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  {
    MutationPageOwner owner{*pager};
    const auto appended = AllocateBtreePage(owner, geometry);
    ASSERT_FALSE(appended.has_value());
    EXPECT_EQ(ErrorCode::kTooLarge, appended.error().code());
    EXPECT_EQ(0xfffffffeU, pager->page_count());
    EXPECT_EQ(0U, owner.mutation_sequence());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeFreelist, LatchesRollbackAfterPostHeaderCorruption) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  {
    auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
    Store32(page_one.mutable_bytes(), 28U, 2U);
    Store32(page_one.mutable_bytes(), 32U, 2U);
    Store32(page_one.mutable_bytes(), 36U, 1U);
  }
  {
    auto trunk = TakeValue(pager->WritePage(PageNumber{2}));
    Store32(trunk.mutable_bytes(), 0U, 0U);
    Store32(trunk.mutable_bytes(), 4U, 127U);
  }
  RequireStatus(pager->Commit());

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const auto allocated = AllocateBtreePage(owner, geometry);
    ASSERT_FALSE(allocated.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, allocated.error().code());
  }
  ASSERT_EQ(ErrorCode::kCorruption, pager->write_failure_code());
  const auto blocked = pager->AllocatePage();
  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, blocked.error().code());
  RequireStatus(pager->Rollback());
}

TEST(BtreeFreelist, ContentHistorySurvivesIntegratedSavepointRollback) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    EXPECT_EQ(PageNumber{2}, TakeValue(AllocateBtreePage(owner, geometry)).page_number);
    EXPECT_EQ(PageNumber{3}, TakeValue(AllocateBtreePage(owner, geometry)).page_number);
  }
  RequireStatus(pager->Commit());
  const ByteBuffer original = ByteBuffer::CopyOf(vfs.database_bytes());

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    RequireStatus(FreeBtreePage(owner, geometry, PageNumber{2}));
    RequireStatus(FreeBtreePage(owner, geometry, PageNumber{3}));
  }
  const auto savepoint = TakeValue(pager->CreateSavepoint());
  {
    MutationPageOwner owner{*pager};
    EXPECT_EQ(PageNumber{3}, TakeValue(AllocateBtreePage(owner, geometry)).page_number);
  }
  RequireStatus(pager->RollbackToSavepoint(savepoint));
  EXPECT_TRUE(pager->PageContentRequired(PageNumber{3}));
  RequireStatus(pager->Rollback());
  EXPECT_TRUE(std::ranges::equal(original.view(), vfs.database_bytes()));
}

TEST(BtreeOverflow, FormatsLocalTableAndInteriorIndexCellsInRetainedScratch) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const std::array<std::byte, 3> payload{
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
    };
    const FormattedCell table =
        TakeValue(FillTableLeafCell(owner, geometry, workspace, -7, payload));
    EXPECT_EQ(0U, table.first_overflow_page.has_value());
    EXPECT_GE(table.bytes.size(), 4U);
    const ByteBuffer table_page = CellImagePage(table.bytes, BtreePageType::kLeafTable, geometry);
    const auto parsed_table =
        TakeValue(BtreePageView::Parse(table_page.view(), PageNumber{98}, geometry));
    const BtreeCellView table_cell = TakeValue(parsed_table.cell(0U));
    EXPECT_EQ(-7, table_cell.rowid().value_or(0));
    EXPECT_TRUE(std::ranges::equal(payload, table_cell.local_payload()));

    const ByteBuffer record = TakeValue(EncodeRecord(std::array{SqlValue::Integer(42)}));
    const FormattedCell index = TakeValue(FillIndexCell(
        owner, geometry, workspace, record.view(), BtreePageType::kInteriorIndex, PageNumber{7}));
    const ByteBuffer index_page =
        CellImagePage(index.bytes, BtreePageType::kInteriorIndex, geometry);
    const auto parsed_index =
        TakeValue(BtreePageView::Parse(index_page.view(), PageNumber{98}, geometry));
    const BtreeCellView index_cell = TakeValue(parsed_index.cell(0U));
    EXPECT_EQ(PageNumber{7}, index_cell.left_child().value_or(PageNumber{}));
    EXPECT_TRUE(std::ranges::equal(record.view(), index_cell.local_payload()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeOverflow, WritesAndClearsAReferenceOrderedOverflowChain) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer payload = FilledBuffer(2'000U, std::byte{0x6a});
  payload.mutable_view().front() = std::byte{0x11};
  payload.mutable_view().back() = std::byte{0x22};

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const FormattedCell formatted =
        TakeValue(FillTableLeafCell(owner, geometry, workspace, 9, payload.view()));
    ASSERT_TRUE(formatted.first_overflow_page.has_value());
    const ByteBuffer cell_page =
        CellImagePage(formatted.bytes, BtreePageType::kLeafTable, geometry);
    const auto parsed_cell =
        TakeValue(BtreePageView::Parse(cell_page.view(), PageNumber{98}, geometry));
    const BtreeCellView cell = TakeValue(parsed_cell.cell(0U));
    ASSERT_TRUE(cell.first_overflow_page().has_value());
    EXPECT_EQ(payload.size().value(), cell.payload_size().value());
    EXPECT_TRUE(std::ranges::equal(payload.view().first(cell.local_payload().size()),
                                   cell.local_payload()));

    std::size_t offset = cell.local_payload().size();
    std::optional<PageNumber> current = cell.first_overflow_page();
    while (offset < payload.size().value()) {
      ASSERT_TRUE(current.has_value());
      const auto pin = TakeValue(pager->ReadPage(*current));
      const auto overflow = TakeValue(OverflowPageView::Parse(pin.frame().bytes(), geometry));
      const std::size_t count =
          std::min(overflow.payload().size(), payload.size().value() - offset);
      EXPECT_TRUE(std::ranges::equal(payload.view().subspan(offset, count),
                                     overflow.payload().first(count)));
      offset += count;
      current = overflow.next_page();
    }
    EXPECT_FALSE(current.has_value());

    RequireStatus(ClearCellOverflow(owner, geometry, cell));
    const ByteView page_one =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{1})))).get().bytes();
    EXPECT_EQ(pager->page_count() - 1U, LoadBigEndian<std::uint32_t>(page_one.subspan<36U, 4U>()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeOverflow, ClearsMoreThanThirtyFreelistTrunksWithBoundedPins) {
  auto vfs = std::make_unique<test::WritePagerMemoryVfs<std::size_t{4U} * 1024U * 1024U>>(false);
  std::unique_ptr<Pager> pager = test::OpenWritePager(*vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  RequireStatus(test::InitializeEmptyBtreeImage(*pager));
  RequireStatus(pager->Commit());
  const auto geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(pager->page_size()));
  const ByteBuffer payload = FilledBuffer(std::size_t{2U} * 1024U * 1024U, std::byte{0x6a});
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const auto formatted =
        TakeValue(FillTableLeafCell(owner, geometry, workspace, 1, payload.view()));
    const ByteBuffer cell_page =
        CellImagePage(formatted.bytes, BtreePageType::kLeafTable, geometry);
    const auto parsed = TakeValue(BtreePageView::Parse(cell_page.view(), PageNumber{98}, geometry));
    const auto cell = TakeValue(parsed.cell(0U));

    RequireStatus(ClearCellOverflow(owner, geometry, cell));

    EXPECT_EQ(1U, owner.size());
    const ByteView page_one =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{1})))).get().bytes();
    EXPECT_EQ(pager->page_count() - 1U, LoadBigEndian<std::uint32_t>(page_one.subspan<36U, 4U>()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeOverflow, OverwritesEqualPayloadsWithoutChangingOverflowOwnership) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const ByteBuffer original = FilledBuffer(2'000U, std::byte{0x31});
  ByteBuffer replacement = FilledBuffer(2'000U, std::byte{0x72});
  replacement.mutable_view().front() = std::byte{0x11};
  replacement.mutable_view().back() = std::byte{0x22};

  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const FormattedCell formatted =
        TakeValue(FillTableLeafCell(owner, geometry, workspace, 12, original.view()));
    const AllocatedBtreePage table_page = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage page = TakeValue(MutableBtreePage::Initialize(
        owner, table_page.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(page.InsertCell(0U, formatted.bytes, std::nullopt, {}, workspace));
    const std::uint32_t page_count = pager->page_count();
    const ByteView before = TakeValue(owner.Frame(table_page.owner_slot)).get().bytes();
    const auto parsed = TakeValue(BtreePageView::Parse(before, table_page.page_number, geometry));
    const BtreeCellView cell = TakeValue(parsed.cell(0U));
    ASSERT_TRUE(cell.first_overflow_page().has_value());
    std::vector<PageNumber> overflow_pages;
    std::optional<PageNumber> current = cell.first_overflow_page();
    std::size_t remaining = replacement.size().value() - cell.local_payload().size();
    while (remaining != 0U) {
      ASSERT_TRUE(current.has_value());
      overflow_pages.push_back(*current);
      const auto pin = TakeValue(pager->ReadPage(*current));
      const auto overflow = TakeValue(OverflowPageView::Parse(pin.frame().bytes(), geometry));
      const std::size_t count = std::min(geometry.overflow_payload_capacity().value(), remaining);
      remaining -= count;
      current = overflow.next_page();
    }

    RequireStatus(page.OverwritePayload(0U, replacement.view(), workspace));

    EXPECT_EQ(page_count, pager->page_count());
    const auto rewritten =
        TakeValue(BtreePageView::Parse(TakeValue(owner.Frame(table_page.owner_slot)).get().bytes(),
                                       table_page.page_number, geometry));
    const BtreeCellView rewritten_cell = TakeValue(rewritten.cell(0U));
    EXPECT_EQ(cell.first_overflow_page(), rewritten_cell.first_overflow_page());
    EXPECT_TRUE(std::ranges::equal(replacement.view().first(rewritten_cell.local_payload().size()),
                                   rewritten_cell.local_payload()));
    EXPECT_EQ(0U, pager->header()->freelist_page_count());
    EXPECT_FALSE(overflow_pages.empty());
    std::size_t offset = rewritten_cell.local_payload().size();
    for (std::size_t index = 0U; index < overflow_pages.size(); ++index) {
      const auto pin = TakeValue(pager->ReadPage(overflow_pages[index]));
      const auto overflow = TakeValue(OverflowPageView::Parse(pin.frame().bytes(), geometry));
      const std::size_t count =
          std::min(overflow.payload().size(), replacement.size().value() - offset);
      EXPECT_TRUE(std::ranges::equal(replacement.view().subspan(offset, count),
                                     overflow.payload().first(count)));
      offset += count;
      if (index + 1U < overflow_pages.size()) {
        EXPECT_EQ(overflow_pages[index + 1U], overflow.next_page().value_or(PageNumber{}));
      } else {
        EXPECT_FALSE(overflow.next_page().has_value());
      }
    }
    EXPECT_EQ(replacement.size().value(), offset);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeOverflow, OverwritesAnOverlappingLocalPayloadSafely) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage table_page = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage page = TakeValue(MutableBtreePage::Initialize(
        owner, table_page.owner_slot, geometry, BtreePageType::kLeafTable));
    const std::array<std::byte, 5> payload{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}, std::byte{0x50},
    };
    const FormattedCell formatted =
        TakeValue(FillTableLeafCell(owner, geometry, workspace, 1, payload));
    RequireStatus(page.InsertCell(0U, formatted.bytes, std::nullopt, {}, workspace));
    const ByteView bytes = TakeValue(owner.Frame(table_page.owner_slot)).get().bytes();
    const auto parsed = TakeValue(BtreePageView::Parse(bytes, table_page.page_number, geometry));
    const BtreeCellView cell = TakeValue(parsed.cell(0U));
    const auto offset = static_cast<std::size_t>(cell.local_payload().data() - bytes.data());
    ASSERT_GT(offset, 0U);
    const ByteView overlapping = bytes.subspan(offset - 1U, payload.size());
    std::array<std::byte, 5> expected{};
    std::ranges::copy(overlapping, expected.begin());

    RequireStatus(page.OverwritePayload(0U, overlapping, workspace));

    const auto rewritten =
        TakeValue(BtreePageView::Parse(TakeValue(owner.Frame(table_page.owner_slot)).get().bytes(),
                                       table_page.page_number, geometry));
    EXPECT_TRUE(std::ranges::equal(expected, TakeValue(rewritten.cell(0U)).local_payload()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeOverflow, StagesPageAliasedOverflowPayloadBeforeTheFirstCopy) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  RequireStatus(pager->BeginWrite());
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage table_page = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage page = TakeValue(MutableBtreePage::Initialize(
        owner, table_page.owner_slot, geometry, BtreePageType::kLeafTable));
    const ByteBuffer initial = FilledBuffer(500U, std::byte{0x6a});
    const FormattedCell formatted =
        TakeValue(FillTableLeafCell(owner, geometry, workspace, 1, initial.view()));
    RequireStatus(page.InsertCell(0U, formatted.bytes, std::nullopt, {}, workspace));
    const ByteView aliased =
        TakeValue(owner.Frame(table_page.owner_slot)).get().bytes().first(initial.size().value());
    const ByteBuffer expected = ByteBuffer::CopyOf(aliased);

    RequireStatus(page.OverwritePayload(0U, aliased, workspace));

    const auto rewritten =
        TakeValue(BtreePageView::Parse(TakeValue(owner.Frame(table_page.owner_slot)).get().bytes(),
                                       table_page.page_number, geometry));
    const BtreeCellView cell = TakeValue(rewritten.cell(0U));
    ASSERT_TRUE(cell.first_overflow_page().has_value());
    EXPECT_TRUE(std::ranges::equal(expected.view().first(cell.local_payload().size()),
                                   cell.local_payload()));
    const auto overflow_pin = TakeValue(pager->ReadPage(*cell.first_overflow_page()));
    const auto overflow =
        TakeValue(OverflowPageView::Parse(overflow_pin.frame().bytes(), geometry));
    const auto remaining = expected.view().subspan(cell.local_payload().size());
    EXPECT_TRUE(std::ranges::equal(remaining, overflow.payload().first(remaining.size())));
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, AllocatesRetainedSQLiteScratchWithTheFourBytePrefix) {
  static_assert(!std::is_copy_constructible_v<MutableBtreePage>);
  static_assert(!std::is_copy_assignable_v<MutableBtreePage>);

  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));

  ASSERT_EQ(516U, workspace.cell_scratch_with_prefix().size());
  ASSERT_EQ(512U, workspace.cell_scratch().size());
  ASSERT_EQ(512U, workspace.rebuild_scratch().size());
  EXPECT_EQ(workspace.cell_scratch_with_prefix().data() + 4U, workspace.cell_scratch().data());
  EXPECT_TRUE(std::ranges::all_of(workspace.cell_scratch_with_prefix().first(4U),
                                  [](std::byte value) { return value == std::byte{0}; }));
}

TEST(CellArray, EnforcesTheReferenceMaximumWithoutGrowingAfterCreation) {
  static_assert(sizeof(CellLocator) == 8U);
  static_assert(!std::is_copy_constructible_v<CellArray>);
  static_assert(!std::is_copy_assignable_v<CellArray>);
  static_assert(std::is_nothrow_move_constructible_v<CellArray>);

  const BtreePageGeometry geometry =
      TakeValue(BtreePageGeometry::Create(ByteCount{512}, ByteCount{512}));
  CellArray cells = TakeValue(CellArray::Create(geometry));
  const ByteBuffer cell = TableLeafCell(1, 0U);
  const std::size_t maximum_page_cells = (geometry.page_size().value() - 8U) / 6U;
  const std::size_t maximum_cells =
      (3U * (maximum_page_cells + kStagedCellSlots) + 3U) & ~std::size_t{3U};

  for (std::size_t index = 0U; index < maximum_cells; ++index) {
    RequireStatus(cells.AppendBorrowed(cell.view(), 0U, cell.size().value()));
  }

  const auto overflow = cells.AppendBorrowed(cell.view(), 0U, cell.size().value());
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, overflow.error().code());
  EXPECT_EQ(maximum_cells, cells.size());
}

TEST(MutableBtreePage, RejectsRebuildCellsFromDifferentPageGeometryWithoutMutation) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry page_geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const BtreePageGeometry other_geometry =
      TakeValue(BtreePageGeometry::Create(ByteCount{1024}, ByteCount{1024}));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{1}));
    MutableBtreePage page = TakeValue(MutableBtreePage::Open(owner, slot, page_geometry));
    CellArray cells = TakeValue(CellArray::Create(other_geometry));
    const ByteBuffer cell = TableLeafCell(1, 0U);
    RequireStatus(cells.AppendCopied(cell.view()));
    const ByteBuffer before = ByteBuffer::CopyOf(TakeValue(owner.Frame(slot)).get().bytes());

    const auto rebuilt = page.Rebuild(cells, 0U, 1U, workspace);

    ASSERT_FALSE(rebuilt.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, rebuilt.error().code());
    EXPECT_TRUE(std::ranges::equal(before.view(), TakeValue(owner.Frame(slot)).get().bytes()));
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, RebuildsAliasedPageOneCellsWithoutChangingTheDatabaseHeader) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{1}));
    MutableBtreePage page = TakeValue(MutableBtreePage::Open(owner, slot, geometry));
    const ByteBuffer one = TableLeafCell(1, 20U);
    const ByteBuffer two = TableLeafCell(2, 24U);
    const ByteBuffer three = TableLeafCell(3, 28U);
    RequireStatus(page.InsertCell(0U, one.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(1U, three.view(), std::nullopt, {}, workspace));

    const ByteView before = TakeValue(owner.Frame(slot)).get().bytes();
    std::array<std::byte, 100> database_header{};
    std::ranges::copy(before.first(database_header.size()), database_header.begin());
    const BtreePageView parsed = TakeValue(BtreePageView::Parse(before, PageNumber{1}, geometry));
    const ByteCount first_offset = TakeValue(parsed.cell_offset(0U));
    const ByteCount third_offset = TakeValue(parsed.cell_offset(1U));
    const BtreeCellView first_cell = TakeValue(parsed.cell(0U));
    const BtreeCellView third_cell = TakeValue(parsed.cell(1U));
    CellArray cells = TakeValue(CellArray::Create(geometry));
    RequireStatus(
        cells.AppendBorrowed(before, first_offset.value(), first_cell.encoded_size().value()));
    RequireStatus(cells.AppendCopied(two.view()));
    RequireStatus(
        cells.AppendBorrowed(before, third_offset.value(), third_cell.encoded_size().value()));

    RequireStatus(page.Rebuild(cells, 0U, 3U, workspace));

    const ByteView rebuilt = TakeValue(owner.Frame(slot)).get().bytes();
    EXPECT_TRUE(std::ranges::equal(database_header, rebuilt.first(database_header.size())));
    const BtreePageView result = TakeValue(BtreePageView::Parse(rebuilt, PageNumber{1}, geometry));
    ASSERT_EQ(3U, result.cell_count());
    EXPECT_EQ(1, TakeValue(result.cell(0U)).rowid());
    EXPECT_EQ(2, TakeValue(result.cell(1U)).rowid());
    EXPECT_EQ(3, TakeValue(result.cell(2U)).rowid());
    EXPECT_EQ(ByteCount{0}, result.fragmented_free_bytes());
    EXPECT_FALSE(TakeValue(result.AnalyzeFreeSpace()).freeblocks().Next().has_value());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, RejectsPartiallyOverlappingRebuildSourcesWithoutMutation) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(InsertTableLeafCell(page, 0U, TableLeafSpec{.rowid = 1, .payload_size = 20U}, {},
                                      workspace));
    const ByteView bytes = TakeValue(owner.Frame(slot)).get().bytes();
    const BtreePageView parsed = TakeValue(BtreePageView::Parse(bytes, PageNumber{2}, geometry));
    const std::size_t offset = TakeValue(parsed.cell_offset(0U)).value();
    const std::size_t size = TakeValue(parsed.cell(0U)).encoded_size().value();
    ASSERT_GT(offset, 0U);
    ASSERT_LE(offset + size, bytes.size());
    CellArray cells = TakeValue(CellArray::Create(geometry));
    RequireStatus(cells.AppendBorrowed(bytes.subspan(offset - 1U, size + 1U), 1U, size));
    const ByteBuffer before = ByteBuffer::CopyOf(bytes);

    const auto rebuilt = page.Rebuild(cells, 0U, 1U, workspace);

    ASSERT_FALSE(rebuilt.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, rebuilt.error().code());
    EXPECT_TRUE(std::ranges::equal(before.view(), TakeValue(owner.Frame(slot)).get().bytes()));
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, RebuildsPaddedFourByteLeafIndexCells) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafIndex));
    const std::array<std::byte, 4> tiny_cell{
        std::byte{0x02},
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x00},
    };
    CellArray cells = TakeValue(CellArray::Create(geometry));
    RequireStatus(cells.AppendCopied(tiny_cell));

    RequireStatus(page.Rebuild(cells, 0U, 1U, workspace));

    const BtreePageView result = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    const BtreeCellView cell = TakeValue(result.cell(0U));
    EXPECT_EQ(ByteCount{2}, cell.payload_size());
    EXPECT_EQ(ByteCount{4}, cell.encoded_size());
    EXPECT_TRUE(std::ranges::equal(std::span{tiny_cell}.subspan<1U, 2U>(), cell.local_payload()));
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, EditsAnOverfullPageByMaterializingAStagedCell) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    const ByteBuffer one = TableLeafCell(1, 230U);
    const ByteBuffer two = TableLeafCell(2, 11U);
    const ByteBuffer three = TableLeafCell(3, 250U);
    std::array<std::byte, 32> staged_copy{};
    RequireStatus(page.InsertCell(0U, one.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(1U, three.view(), std::nullopt, {}, workspace));
    RequireStatus(
        page.InsertCell(1U, two.view(), std::nullopt, MutableByteView{staged_copy}, workspace));
    ASSERT_EQ(1U, page.staged_count());

    CellArray cells = TakeValue(CellArray::Create(geometry));
    RequireStatus(cells.AppendPage(page));
    ASSERT_EQ(3U, cells.size());

    RequireStatus(page.Edit(cells, 0U, 0U, 2U, workspace));

    EXPECT_EQ(0U, page.staged_count());
    const BtreePageView result = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    ASSERT_EQ(2U, result.cell_count());
    EXPECT_EQ(1, TakeValue(result.cell(0U)).rowid());
    EXPECT_EQ(2, TakeValue(result.cell(1U)).rowid());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, PrependsCellsThroughTheEditFastPath) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(InsertTableLeafCell(page, 0U, TableLeafSpec{.rowid = 2, .payload_size = 20U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(page, 1U, TableLeafSpec{.rowid = 3, .payload_size = 24U}, {},
                                      workspace));
    CellArray cells = TakeValue(CellArray::Create(geometry));
    RequireStatus(AppendTableLeafCell(cells, TableLeafSpec{.rowid = 1, .payload_size = 16U}));
    RequireStatus(cells.AppendPage(page));

    RequireStatus(page.Edit(cells, 1U, 0U, 3U, workspace));

    const BtreePageView result = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    ASSERT_EQ(3U, result.cell_count());
    EXPECT_EQ(1, TakeValue(result.cell(0U)).rowid());
    EXPECT_EQ(2, TakeValue(result.cell(1U)).rowid());
    EXPECT_EQ(3, TakeValue(result.cell(2U)).rowid());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, DropsPrefixCellsThroughTheEditFastPath) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(InsertTableLeafCell(page, 0U, TableLeafSpec{.rowid = 1, .payload_size = 16U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(page, 1U, TableLeafSpec{.rowid = 2, .payload_size = 20U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(page, 2U, TableLeafSpec{.rowid = 3, .payload_size = 24U}, {},
                                      workspace));
    CellArray cells = TakeValue(CellArray::Create(geometry));
    RequireStatus(cells.AppendPage(page));

    RequireStatus(page.Edit(cells, 0U, 1U, 2U, workspace));

    const BtreePageView result = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    ASSERT_EQ(2U, result.cell_count());
    EXPECT_EQ(2, TakeValue(result.cell(0U)).rowid());
    EXPECT_EQ(3, TakeValue(result.cell(1U)).rowid());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, PreservesTheEditFastPathByteForByteWhenTheRangeIsUnchanged) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    const ByteBuffer one = TableLeafCell(1, 80U);
    const ByteBuffer two = TableLeafCell(2, 60U);
    const ByteBuffer three = TableLeafCell(3, 40U);
    RequireStatus(page.InsertCell(0U, one.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(1U, two.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(2U, three.view(), std::nullopt, {}, workspace));
    RequireStatus(page.DropCell(1U));
    const BtreePageView fragmented = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    EXPECT_TRUE(TakeValue(fragmented.AnalyzeFreeSpace()).freeblocks().Next().has_value());
    CellArray cells = TakeValue(CellArray::Create(geometry));
    RequireStatus(cells.AppendPage(page));
    const ByteBuffer before = ByteBuffer::CopyOf(TakeValue(owner.Frame(slot)).get().bytes());

    RequireStatus(page.Edit(cells, 0U, 0U, cells.size(), workspace));

    EXPECT_TRUE(std::ranges::equal(before.view(), TakeValue(owner.Frame(slot)).get().bytes()));
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, RebuildsWhenTheEditFastPathCannotExtendThePointerArray) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(InsertTableLeafCell(page, 0U, TableLeafSpec{.rowid = 1, .payload_size = 170U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(page, 1U, TableLeafSpec{.rowid = 2, .payload_size = 170U}, {},
                                      workspace));
    CellArray cells = TakeValue(CellArray::Create(geometry));
    RequireStatus(cells.AppendPage(page));
    constexpr std::size_t kNewCellCount = 80U;
    for (std::int64_t rowid = 10; rowid < 90; ++rowid) {
      RequireStatus(AppendTableLeafCell(cells, TableLeafSpec{.rowid = rowid, .payload_size = 0U}));
    }

    RequireStatus(page.Edit(cells, 0U, 2U, kNewCellCount, workspace));

    const BtreePageView result = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    ASSERT_EQ(kNewCellCount, result.cell_count());
    EXPECT_EQ(10, TakeValue(result.cell(0U)).rowid());
    EXPECT_EQ(89, TakeValue(result.cell(kNewCellCount - 1U)).rowid());
    EXPECT_EQ(ByteCount{0}, result.fragmented_free_bytes());
    EXPECT_FALSE(TakeValue(result.AnalyzeFreeSpace()).freeblocks().Next().has_value());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, PreservesFragmentsAcrossTheFastDefragmentPath) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    const ByteBuffer larger = TableLeafCell(1, 20U);
    const ByteBuffer smaller = TableLeafCell(1, 18U);
    const ByteBuffer middle = TableLeafCell(2, 30U);
    const ByteBuffer tail = TableLeafCell(3, 30U);
    RequireStatus(page.InsertCell(0U, larger.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(1U, middle.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(2U, tail.view(), std::nullopt, {}, workspace));
    RequireStatus(page.DropCell(0U));
    RequireStatus(page.InsertCell(0U, smaller.view(), std::nullopt, {}, workspace));
    RequireStatus(page.DropCell(1U));
    const std::size_t free_before = page.free_bytes();

    RequireStatus(page.Defragment(2U, workspace));

    const auto parsed = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    EXPECT_EQ(ByteCount{2}, parsed.fragmented_free_bytes());
    EXPECT_EQ(free_before, TakeValue(parsed.AnalyzeFreeSpace()).total().value());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, MovesWithoutDuplicatingTheFacade) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage source =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    const MutableBtreePage destination{std::move(source)};
    EXPECT_EQ(BtreePageType::kLeafTable, destination.type());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, InitializesAndZerosOrdinaryAndPageOneHeaders) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  {
    MutationPageOwner owner{*pager};
    const std::size_t page_two_slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    const MutableBtreePage page = TakeValue(
        MutableBtreePage::Initialize(owner, page_two_slot, geometry, BtreePageType::kLeafIndex));
    EXPECT_EQ(BtreePageType::kLeafIndex, page.type());
    EXPECT_EQ(0U, page.header_offset());
    EXPECT_EQ(8U, page.cell_pointer_offset());
    EXPECT_EQ(504U, page.free_bytes());
    const auto parsed = TakeValue(BtreePageView::Parse(
        TakeValue(owner.Frame(page_two_slot)).get().bytes(), PageNumber{2}, geometry));
    EXPECT_EQ(BtreePageType::kLeafIndex, parsed.type());

    const std::size_t page_one_slot = TakeValue(owner.AcquireWrite(PageNumber{1}));
    const ByteView before = TakeValue(owner.Frame(page_one_slot)).get().bytes();
    std::array<std::byte, 100> header{};
    std::ranges::copy(before.first(header.size()), header.begin());
    MutableBtreePage page_one = TakeValue(MutableBtreePage::Open(owner, page_one_slot, geometry));
    RequireStatus(page_one.Zero(BtreePageType::kLeafTable));
    EXPECT_EQ(100U, page_one.header_offset());
    EXPECT_TRUE(std::ranges::equal(
        header, TakeValue(owner.Frame(page_one_slot)).get().bytes().first<100>()));
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, InsertsDropsCoalescesAndResetsAnEmptyLeaf) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    const ByteBuffer one = TableLeafCell(1, 20U);
    const ByteBuffer two = TableLeafCell(2, 24U);
    const ByteBuffer three = TableLeafCell(3, 28U);
    RequireStatus(page.InsertCell(0U, one.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(1U, two.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(2U, three.view(), std::nullopt, {}, workspace));
    EXPECT_EQ(3U, page.cell_count());
    auto parsed = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    const std::size_t freed_offset = TakeValue(parsed.cell_offset(1U)).value();

    RequireStatus(page.DropCell(1U));
    EXPECT_EQ(2U, page.cell_count());
    parsed = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    const auto free = TakeValue(parsed.AnalyzeFreeSpace());
    EXPECT_TRUE(free.freeblocks().Next().has_value());
    EXPECT_EQ(1, TakeValue(parsed.cell(0)).rowid());
    EXPECT_EQ(3, TakeValue(parsed.cell(1)).rowid());

    RequireStatus(page.InsertCell(1U, two.view(), std::nullopt, {}, workspace));
    parsed = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    EXPECT_EQ(freed_offset, TakeValue(parsed.cell_offset(1U)).value());
    EXPECT_EQ(2, TakeValue(parsed.cell(1U)).rowid());

    RequireStatus(page.DropCell(2U));
    RequireStatus(page.DropCell(1U));
    RequireStatus(page.DropCell(0U));
    EXPECT_EQ(0U, page.cell_count());
    EXPECT_EQ(geometry.usable_size().value() - 8U, page.free_bytes());
    parsed = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    EXPECT_EQ(geometry.usable_size(), parsed.cell_content_offset());
    EXPECT_FALSE(TakeValue(parsed.AnalyzeFreeSpace()).freeblocks().Next().has_value());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, DefragmentsFreeblocksIntoOneUnallocatedRegion) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    const ByteBuffer one = TableLeafCell(1, 90U);
    const ByteBuffer two = TableLeafCell(2, 90U);
    const ByteBuffer three = TableLeafCell(3, 90U);
    RequireStatus(page.InsertCell(0U, one.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(1U, two.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(2U, three.view(), std::nullopt, {}, workspace));
    RequireStatus(page.DropCell(1U));
    RequireStatus(page.Defragment(0U, workspace));

    const auto parsed = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(slot)).get().bytes(), PageNumber{2}, geometry));
    EXPECT_FALSE(TakeValue(parsed.AnalyzeFreeSpace()).freeblocks().Next().has_value());
    EXPECT_EQ(ByteCount{0}, parsed.fragmented_free_bytes());
    EXPECT_EQ(1, TakeValue(parsed.cell(0)).rowid());
    EXPECT_EQ(3, TakeValue(parsed.cell(1)).rowid());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, StagesThreeSequentialOverflowCellsAndProtectsTheFourthSlot) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    MutableBtreePage page =
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable));
    const ByteBuffer first = TableLeafCell(1, 230U);
    const ByteBuffer second = TableLeafCell(2, 250U);
    RequireStatus(page.InsertCell(0U, first.view(), std::nullopt, {}, workspace));
    RequireStatus(page.InsertCell(1U, second.view(), std::nullopt, {}, workspace));
    const ByteBuffer staged = TableLeafCell(3, 11U);
    std::array<std::array<std::byte, 16>, 4> copies{};
    const ByteBuffer before = ByteBuffer::CopyOf(TakeValue(owner.Frame(slot)).get().bytes());
    for (std::size_t index = 0; index < 3U; ++index) {
      RequireStatus(page.InsertCell(2U + index, staged.view(), std::nullopt,
                                    MutableByteView{copies[index]}, workspace));
    }
    const auto fourth =
        page.InsertCell(5U, staged.view(), std::nullopt, MutableByteView{copies[3]}, workspace);
    ASSERT_FALSE(fourth.has_value());
    EXPECT_EQ(ErrorCode::kTooLarge, fourth.error().code());
    EXPECT_EQ(3U, page.staged_count());
    EXPECT_EQ(2U, page.cell_count());
    EXPECT_TRUE(std::ranges::equal(before.view(), TakeValue(owner.Frame(slot)).get().bytes()));
    for (std::size_t index = 0; index < 3U; ++index) {
      const auto cell = page.staged_cell(index);
      ASSERT_TRUE(cell.has_value());
      EXPECT_EQ(2U + index, cell->index);
      EXPECT_TRUE(std::ranges::equal(staged.view(), cell->bytes));
    }
    page.ClearStagedCells();
    EXPECT_EQ(0U, page.staged_count());
  }
  RequireStatus(pager->Rollback());
}

TEST(MutableBtreePage, RejectsMalformedFreeblocksWithoutMutation) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  {
    const auto allocated = TakeValue(pager->AllocatePage());
    EXPECT_EQ(PageNumber{2}, allocated.frame().page_number());
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  {
    MutationPageOwner owner{*pager};
    const std::size_t slot = TakeValue(owner.AcquireWrite(PageNumber{2}));
    static_cast<void>(
        TakeValue(MutableBtreePage::Initialize(owner, slot, geometry, BtreePageType::kLeafTable)));
    const MutableByteView bytes = TakeValue(owner.MutableBytes(slot));
    Store16(bytes, 1U, 200U);
    const ByteBuffer before = ByteBuffer::CopyOf(bytes);

    const auto malformed = MutableBtreePage::Open(owner, slot, geometry);

    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, malformed.error().code());
    EXPECT_TRUE(std::ranges::equal(before.view(), TakeValue(owner.Frame(slot)).get().bytes()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, QuickBalanceMovesTheRightmostOverflowCellToANewSibling) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage leaf_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    ASSERT_EQ(PageNumber{2}, parent_allocation.page_number);
    ASSERT_EQ(PageNumber{3}, leaf_allocation.page_number);
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage leaf = TakeValue(MutableBtreePage::Initialize(
        owner, leaf_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(parent.SetRightmostChild(leaf.page_number()));
    RequireStatus(InsertTableLeafCell(leaf, 0U, TableLeafSpec{.rowid = 1, .payload_size = 477U}, {},
                                      workspace));
    std::array<std::byte, 32> staged_copy{};
    RequireStatus(InsertTableLeafCell(leaf, 1U, TableLeafSpec{.rowid = 2, .payload_size = 20U},
                                      MutableByteView{staged_copy}, workspace));
    ASSERT_EQ(1U, leaf.staged_count());

    RequireStatus(MutableBtreePage::BalanceQuick(parent, leaf, workspace));

    EXPECT_EQ(4U, pager->page_count());
    EXPECT_EQ(1U, leaf.cell_count());
    EXPECT_EQ(1U, leaf.staged_count());
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    ASSERT_EQ(1U, parent_view.cell_count());
    EXPECT_EQ(PageNumber{4}, parent_view.rightmost_child().value_or(PageNumber{}));
    const BtreeCellView divider = TakeValue(parent_view.cell(0U));
    EXPECT_EQ(leaf.page_number(), divider.left_child().value_or(PageNumber{}));
    EXPECT_EQ(1, divider.rowid());
    const auto new_page_pin = TakeValue(pager->ReadPage(PageNumber{4}));
    const BtreePageView new_page =
        TakeValue(BtreePageView::Parse(new_page_pin.frame().bytes(), PageNumber{4}, geometry));
    ASSERT_EQ(BtreePageType::kLeafTable, new_page.type());
    ASSERT_EQ(1U, new_page.cell_count());
    EXPECT_EQ(2, TakeValue(new_page.cell(0U)).rowid());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, QuickBalancePreservesTheRawDividerRowidEncoding) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage leaf_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage leaf = TakeValue(MutableBtreePage::Initialize(
        owner, leaf_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(parent.SetRightmostChild(leaf.page_number()));
    const std::array<std::byte, 9> overlong_rowid{
        std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
        std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x01},
    };
    const ByteBuffer old_cell = TableLeafCellWithRowid(overlong_rowid, 468U);
    RequireStatus(leaf.InsertCell(0U, old_cell.view(), std::nullopt, {}, workspace));
    std::array<std::byte, 32> staged_copy{};
    RequireStatus(InsertTableLeafCell(leaf, 1U, TableLeafSpec{.rowid = 2, .payload_size = 20U},
                                      MutableByteView{staged_copy}, workspace));

    RequireStatus(MutableBtreePage::BalanceQuick(parent, leaf, workspace));

    const ByteView parent_bytes =
        TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes();
    const BtreePageView parent_view =
        TakeValue(BtreePageView::Parse(parent_bytes, parent.page_number(), geometry));
    const std::size_t divider_offset = TakeValue(parent_view.cell_offset(0U)).value();
    const BtreeCellView divider = TakeValue(parent_view.cell(0U));
    ASSERT_EQ(13U, divider.encoded_size().value());
    EXPECT_EQ(1, divider.rowid());
    EXPECT_TRUE(std::ranges::equal(
        overlong_rowid,
        parent_bytes.subspan(divider_offset + sizeof(std::uint32_t), overlong_rowid.size())));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, QuickBalanceStagesAParentDividerForTheNextUpwardBalance) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage leaf_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage leaf = TakeValue(MutableBtreePage::Initialize(
        owner, leaf_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(parent.SetRightmostChild(leaf.page_number()));
    const ByteBuffer large_divider = TableInteriorCell(leaf.page_number(), -1);
    ASSERT_EQ(ByteCount{13}, large_divider.size());
    for (std::size_t index = 0U; index < 33U; ++index) {
      RequireStatus(
          parent.InsertCell(index, large_divider.view(), leaf.page_number(), {}, workspace));
    }
    ASSERT_EQ(5U, parent.free_bytes());
    RequireStatus(InsertTableLeafCell(leaf, 0U, TableLeafSpec{.rowid = 1, .payload_size = 477U}, {},
                                      workspace));
    std::array<std::byte, 32> staged_copy{};
    RequireStatus(InsertTableLeafCell(leaf, 1U, TableLeafSpec{.rowid = 2, .payload_size = 20U},
                                      MutableByteView{staged_copy}, workspace));

    RequireStatus(MutableBtreePage::BalanceQuick(parent, leaf, workspace));

    EXPECT_EQ(33U, parent.cell_count());
    ASSERT_EQ(1U, parent.staged_count());
    const std::optional<StagedCell> staged = parent.staged_cell(0U);
    ASSERT_TRUE(staged.has_value());
    EXPECT_EQ(33U, staged->index);
    const ByteBuffer divider_page =
        CellImagePage(staged->bytes, BtreePageType::kInteriorTable, geometry);
    const BtreeCellView divider = TakeValue(
        TakeValue(BtreePageView::Parse(divider_page.view(), PageNumber{98}, geometry)).cell(0U));
    EXPECT_EQ(leaf.page_number(), divider.left_child().value_or(PageNumber{}));
    EXPECT_EQ(1, divider.rowid());
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    EXPECT_EQ(PageNumber{4}, parent_view.rightmost_child().value_or(PageNumber{}));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, QuickBalanceRejectsANonIncreasingOverflowBeforeAllocation) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage leaf_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage leaf = TakeValue(MutableBtreePage::Initialize(
        owner, leaf_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(parent.SetRightmostChild(leaf.page_number()));
    RequireStatus(InsertTableLeafCell(leaf, 0U, TableLeafSpec{.rowid = 2, .payload_size = 477U}, {},
                                      workspace));
    std::array<std::byte, 32> staged_copy{};
    RequireStatus(InsertTableLeafCell(leaf, 1U, TableLeafSpec{.rowid = 1, .payload_size = 20U},
                                      MutableByteView{staged_copy}, workspace));
    const ByteBuffer parent_before =
        ByteBuffer::CopyOf(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes());
    const ByteBuffer leaf_before =
        ByteBuffer::CopyOf(TakeValue(owner.Frame(leaf_allocation.owner_slot)).get().bytes());

    const auto balanced = MutableBtreePage::BalanceQuick(parent, leaf, workspace);

    ASSERT_FALSE(balanced.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, balanced.error().code());
    EXPECT_EQ(3U, pager->page_count());
    EXPECT_TRUE(std::ranges::equal(
        parent_before.view(), TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes()));
    EXPECT_TRUE(std::ranges::equal(
        leaf_before.view(), TakeValue(owner.Frame(leaf_allocation.owner_slot)).get().bytes()));
    EXPECT_FALSE(pager->write_failure_code().has_value());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, QuickBalanceRejectsPageOneAsTheParent) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage leaf_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const std::size_t parent_slot = TakeValue(owner.Borrow(PageNumber{1}));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Open(owner, parent_slot, geometry));
    RequireStatus(parent.Zero(BtreePageType::kInteriorTable));
    MutableBtreePage leaf = TakeValue(MutableBtreePage::Initialize(
        owner, leaf_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(parent.SetRightmostChild(leaf.page_number()));
    RequireStatus(InsertTableLeafCell(leaf, 0U, TableLeafSpec{.rowid = 1, .payload_size = 477U}, {},
                                      workspace));
    std::array<std::byte, 32> staged_copy{};
    RequireStatus(InsertTableLeafCell(leaf, 1U, TableLeafSpec{.rowid = 2, .payload_size = 20U},
                                      MutableByteView{staged_copy}, workspace));

    const auto balanced = MutableBtreePage::BalanceQuick(parent, leaf, workspace);

    ASSERT_FALSE(balanced.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, balanced.error().code());
    EXPECT_EQ(2U, pager->page_count());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceRedistributesThreeTableLeavesAndFreesTheSurplusPage) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    ASSERT_EQ(PageNumber{2}, parent_allocation.page_number);
    ASSERT_EQ(PageNumber{3}, left_allocation.page_number);
    ASSERT_EQ(PageNumber{4}, middle_allocation.page_number);
    ASSERT_EQ(PageNumber{5}, right_allocation.page_number);

    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafTable));

    RequireStatus(InsertTableLeafCell(left, 0U, TableLeafSpec{.rowid = 1, .payload_size = 160U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(left, 1U, TableLeafSpec{.rowid = 2, .payload_size = 160U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(middle, 0U, TableLeafSpec{.rowid = 3, .payload_size = 160U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(middle, 1U, TableLeafSpec{.rowid = 4, .payload_size = 160U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(right, 0U, TableLeafSpec{.rowid = 5, .payload_size = 160U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(right, 1U, TableLeafSpec{.rowid = 6, .payload_size = 160U},
                                      {}, workspace));

    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), 2);
    const ByteBuffer middle_divider = TableInteriorCell(middle.page_number(), 4);
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    ASSERT_EQ(1U, parent_view.cell_count());
    const BtreeCellView divider = TakeValue(parent_view.cell(0U));
    EXPECT_EQ(PageNumber{3}, divider.left_child().value_or(PageNumber{}));
    EXPECT_EQ(3, divider.rowid());
    EXPECT_EQ(PageNumber{4}, parent_view.rightmost_child().value_or(PageNumber{}));

    for (std::uint32_t page_number = 3U; page_number <= 4U; ++page_number) {
      ByteBuffer page_image;
      if (const std::optional<std::size_t> slot = owner.Find(PageNumber{page_number});
          slot.has_value()) {
        page_image = ByteBuffer::CopyOf(TakeValue(owner.Frame(*slot)).get().bytes());
      } else {
        page_image =
            ByteBuffer::CopyOf(TakeValue(pager->ReadPage(PageNumber{page_number})).frame().bytes());
      }
      const BtreePageView page =
          TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{page_number}, geometry));
      ASSERT_EQ(3U, page.cell_count());
      for (std::size_t index = 0U; index < page.cell_count(); ++index) {
        const auto expected =
            static_cast<std::int64_t>(page_number - 3U) * 3 + static_cast<std::int64_t>(index) + 1;
        EXPECT_EQ(expected, TakeValue(page.cell(index)).rowid());
      }
    }

    const ByteView page_one =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{1})))).get().bytes();
    EXPECT_EQ(1U, Load32(page_one, 36U));
    EXPECT_EQ(PageNumber{5}, PageNumber{Load32(page_one, 32U)});
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceKeepsThreeFullTableLeafSiblings) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(InsertTableLeafCell(left, 0U, TableLeafSpec{.rowid = 1, .payload_size = 477U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(middle, 0U, TableLeafSpec{.rowid = 2, .payload_size = 477U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(right, 0U, TableLeafSpec{.rowid = 3, .payload_size = 477U},
                                      {}, workspace));
    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), 1);
    const ByteBuffer middle_divider = TableInteriorCell(middle.page_number(), 2);
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    EXPECT_EQ(5U, pager->page_count());
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    ASSERT_EQ(2U, parent_view.cell_count());
    EXPECT_EQ(PageNumber{5}, parent_view.rightmost_child().value_or(PageNumber{}));
    for (std::uint32_t page_number = 3U; page_number <= 5U; ++page_number) {
      const ByteBuffer page_image = SnapshotPage(*pager, owner, PageNumber{page_number});
      const BtreePageView leaf =
          TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{page_number}, geometry));
      ASSERT_EQ(1U, leaf.cell_count());
      EXPECT_EQ(static_cast<std::int64_t>(page_number - 2U), TakeValue(leaf.cell(0U)).rowid());
    }
    const ByteView page_one =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{1})))).get().bytes();
    EXPECT_EQ(0U, Load32(page_one, 36U));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceCanonicalizesTableDividerRowids) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    const std::array<std::byte, 9> overlong_rowid{
        std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
        std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x01},
    };
    const ByteBuffer left_cell = TableLeafCellWithRowid(overlong_rowid, 468U);
    RequireStatus(left.InsertCell(0U, left_cell.view(), std::nullopt, {}, workspace));
    RequireStatus(InsertTableLeafCell(middle, 0U, TableLeafSpec{.rowid = 2, .payload_size = 477U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(right, 0U, TableLeafSpec{.rowid = 3, .payload_size = 477U},
                                      {}, workspace));
    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), 1);
    const ByteBuffer middle_divider = TableInteriorCell(middle.page_number(), 2);
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    const ByteView parent_bytes =
        TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes();
    const BtreePageView parent_view =
        TakeValue(BtreePageView::Parse(parent_bytes, parent.page_number(), geometry));
    ASSERT_EQ(2U, parent_view.cell_count());
    const BtreeCellView divider = TakeValue(parent_view.cell(0U));
    ASSERT_EQ(ByteCount{5}, divider.encoded_size());
    EXPECT_EQ(1, divider.rowid());
    const std::size_t divider_offset = TakeValue(parent_view.cell_offset(0U)).value();
    EXPECT_EQ(std::byte{0x01}, parent_bytes[divider_offset + sizeof(std::uint32_t)]);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceSelectsLeftMiddleAndRightThreeSiblingWindows) {
  struct Case {
    std::size_t selected_child;
    PageNumber combined_page;
    std::array<std::int64_t, 2> parent_rowids;
    std::array<PageNumber, 2> parent_left_children;
    PageNumber parent_right_child;
    std::array<std::int64_t, 3> combined_rowids;
  };
  const std::array cases{
      Case{
          .selected_child = 0U,
          .combined_page = PageNumber{3},
          .parent_rowids = {30, 40},
          .parent_left_children = {PageNumber{3}, PageNumber{6}},
          .parent_right_child = PageNumber{7},
          .combined_rowids = {10, 20, 30},
      },
      Case{
          .selected_child = 2U,
          .combined_page = PageNumber{4},
          .parent_rowids = {10, 40},
          .parent_left_children = {PageNumber{3}, PageNumber{4}},
          .parent_right_child = PageNumber{7},
          .combined_rowids = {20, 30, 40},
      },
      Case{
          .selected_child = 4U,
          .combined_page = PageNumber{5},
          .parent_rowids = {10, 20},
          .parent_left_children = {PageNumber{3}, PageNumber{4}},
          .parent_right_child = PageNumber{5},
          .combined_rowids = {30, 40, 50},
      },
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.selected_child);
    test::WritePagerFixedVfs vfs{false};
    std::unique_ptr<Pager> pager = OpenInitialized(vfs);
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginWrite());
    const BtreePageGeometry geometry = TakeValue(
        BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
    BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
    ByteBuffer parent_overflow{geometry.page_size()};
    {
      MutationPageOwner owner{*pager};
      const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
      MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
          owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
      std::array<AllocatedBtreePage, 5> allocations{};
      std::array<std::optional<MutableBtreePage>, 5> leaves{};
      for (std::size_t index = 0U; index < leaves.size(); ++index) {
        allocations[index] = TakeValue(AllocateBtreePage(owner, geometry));
        leaves[index].emplace(TakeValue(MutableBtreePage::Initialize(
            owner, allocations[index].owner_slot, geometry, BtreePageType::kLeafTable)));
        RequireStatus(
            InsertTableLeafCell(*leaves[index], 0U,
                                TableLeafSpec{
                                    .rowid = static_cast<std::int64_t>((index + 1U) * 10U),
                                    .payload_size = 0U,
                                },
                                {}, workspace));
      }
      for (std::size_t index = 0U; index + 1U < leaves.size(); ++index) {
        const auto rowid = static_cast<std::int64_t>(index + 1U) * 10;
        const ByteBuffer divider = TableInteriorCell(leaves[index]->page_number(), rowid);
        RequireStatus(
            parent.InsertCell(index, divider.view(), leaves[index]->page_number(), {}, workspace));
      }
      RequireStatus(parent.SetRightmostChild(leaves.back()->page_number()));
      for (std::size_t index = 0U; index < leaves.size(); ++index) {
        if (index != test_case.selected_child) {
          owner.Release(allocations[index].owner_slot);
        }
      }

      RequireStatus(MutableBtreePage::BalanceNonroot(
          parent, *leaves[test_case.selected_child], test_case.selected_child,
          parent_overflow.mutable_view(), workspace, false));

      const BtreePageView parent_view = TakeValue(
          BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                               parent.page_number(), geometry));
      ASSERT_EQ(2U, parent_view.cell_count());
      for (std::size_t index = 0U; index < parent_view.cell_count(); ++index) {
        const BtreeCellView divider = TakeValue(parent_view.cell(index));
        EXPECT_EQ(test_case.parent_rowids[index], divider.rowid());
        EXPECT_EQ(test_case.parent_left_children[index],
                  divider.left_child().value_or(PageNumber{}));
      }
      EXPECT_EQ(test_case.parent_right_child, parent_view.rightmost_child().value_or(PageNumber{}));

      const ByteBuffer combined_image = SnapshotPage(*pager, owner, test_case.combined_page);
      const BtreePageView combined =
          TakeValue(BtreePageView::Parse(combined_image.view(), test_case.combined_page, geometry));
      ASSERT_EQ(3U, combined.cell_count());
      for (std::size_t index = 0U; index < combined.cell_count(); ++index) {
        EXPECT_EQ(test_case.combined_rowids[index], TakeValue(combined.cell(index)).rowid());
      }
    }
    RequireStatus(pager->Rollback());
  }
}

TEST(BtreeBalance, NonrootBalanceRejectsMismatchedSiblingKindsBeforeMutation) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    RequireStatus(InsertTableLeafCell(left, 0U, TableLeafSpec{.rowid = 1, .payload_size = 0U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(middle, 0U, TableLeafSpec{.rowid = 2, .payload_size = 0U}, {},
                                      workspace));
    const std::array<std::byte, 2> record{std::byte{0x02}, std::byte{0x08}};
    const ByteBuffer index_cell = IndexCell(record, std::nullopt);
    RequireStatus(right.InsertCell(0U, index_cell.view(), std::nullopt, {}, workspace));
    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), 1);
    const ByteBuffer middle_divider = TableInteriorCell(middle.page_number(), 2);
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    const ByteBuffer parent_before =
        ByteBuffer::CopyOf(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes());
    const ByteBuffer middle_before =
        ByteBuffer::CopyOf(TakeValue(owner.Frame(middle_allocation.owner_slot)).get().bytes());
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    const auto balanced = MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false);

    ASSERT_FALSE(balanced.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, balanced.error().code());
    EXPECT_TRUE(std::ranges::equal(
        parent_before.view(), TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes()));
    EXPECT_TRUE(std::ranges::equal(
        middle_before.view(), TakeValue(owner.Frame(middle_allocation.owner_slot)).get().bytes()));
    EXPECT_FALSE(pager->write_failure_code().has_value());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceConsumesAParentDividerStagedAtTheSelectedChild) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  ByteBuffer prior_parent_overflow{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(InsertTableLeafCell(left, 0U, TableLeafSpec{.rowid = 1, .payload_size = 0U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(middle, 0U, TableLeafSpec{.rowid = 2, .payload_size = 0U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(right, 0U, TableLeafSpec{.rowid = 3, .payload_size = 0U}, {},
                                      workspace));

    const ByteBuffer large_divider = TableInteriorCell(PageNumber{100}, -1);
    ASSERT_EQ(ByteCount{13}, large_divider.size());
    for (std::size_t index = 0U; index < 32U; ++index) {
      RequireStatus(parent.InsertCell(index, large_divider.view(), PageNumber{100}, {}, workspace));
    }
    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), -1);
    RequireStatus(parent.InsertCell(32U, left_divider.view(), left.page_number(), {}, workspace));
    const ByteBuffer staged_divider = TableInteriorCell(middle.page_number(), 1);
    RequireStatus(parent.InsertCell(33U, staged_divider.view(), middle.page_number(),
                                    prior_parent_overflow.mutable_view(), workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    ASSERT_EQ(33U, parent.cell_count());
    ASSERT_EQ(1U, parent.staged_count());
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 33U, parent_overflow.mutable_view(), workspace, false));

    EXPECT_EQ(32U, parent.cell_count());
    EXPECT_EQ(0U, parent.staged_count());
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    EXPECT_EQ(PageNumber{3}, parent_view.rightmost_child().value_or(PageNumber{}));
    const ByteBuffer combined_image = SnapshotPage(*pager, owner, PageNumber{3});
    const BtreePageView combined =
        TakeValue(BtreePageView::Parse(combined_image.view(), PageNumber{3}, geometry));
    ASSERT_EQ(3U, combined.cell_count());
    for (std::size_t index = 0U; index < combined.cell_count(); ++index) {
      EXPECT_EQ(static_cast<std::int64_t>(index + 1U), TakeValue(combined.cell(index)).rowid());
    }
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceRetainsAStagedParentDividerInCallerStorage) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  ByteBuffer staged_cell{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(InsertTableLeafCell(left, 0U, TableLeafSpec{.rowid = -4, .payload_size = 477U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(middle, 0U, TableLeafSpec{.rowid = -3, .payload_size = 477U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(middle, 1U, TableLeafSpec{.rowid = -2, .payload_size = 477U},
                                      staged_cell.mutable_view(), workspace));
    RequireStatus(InsertTableLeafCell(right, 0U, TableLeafSpec{.rowid = -1, .payload_size = 477U},
                                      {}, workspace));

    const ByteBuffer large_divider = TableInteriorCell(PageNumber{100}, -1);
    for (std::size_t index = 0U; index < 31U; ++index) {
      RequireStatus(parent.InsertCell(index, large_divider.view(), PageNumber{100}, {}, workspace));
    }
    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), -4);
    const ByteBuffer middle_divider = TableInteriorCell(middle.page_number(), -3);
    RequireStatus(parent.InsertCell(31U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(32U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    ASSERT_EQ(33U, parent.cell_count());
    ASSERT_EQ(5U, parent.free_bytes());
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 32U, parent_overflow.mutable_view(), workspace, false));

    EXPECT_EQ(33U, parent.cell_count());
    ASSERT_EQ(1U, parent.staged_count());
    const std::optional<StagedCell> staged = parent.staged_cell(0U);
    ASSERT_TRUE(staged.has_value());
    EXPECT_EQ(33U, staged->index);
    const ByteBuffer staged_page =
        CellImagePage(staged->bytes, BtreePageType::kInteriorTable, geometry);
    const BtreeCellView divider = TakeValue(
        TakeValue(BtreePageView::Parse(staged_page.view(), PageNumber{98}, geometry)).cell(0U));
    EXPECT_EQ(PageNumber{5}, divider.left_child().value_or(PageNumber{}));
    EXPECT_EQ(-2, divider.rowid());
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    EXPECT_EQ(PageNumber{6}, parent_view.rightmost_child().value_or(PageNumber{}));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceAllocatesAndRekeysAscendingPagesForAStagedTableCell) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  ByteBuffer staged_cell{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));

    RequireStatus(InsertTableLeafCell(left, 0U, TableLeafSpec{.rowid = 1, .payload_size = 477U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(middle, 0U, TableLeafSpec{.rowid = 2, .payload_size = 477U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(middle, 1U, TableLeafSpec{.rowid = 3, .payload_size = 477U},
                                      staged_cell.mutable_view(), workspace));
    RequireStatus(InsertTableLeafCell(right, 0U, TableLeafSpec{.rowid = 4, .payload_size = 477U},
                                      {}, workspace));
    ASSERT_EQ(1U, middle.staged_count());

    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), 1);
    const ByteBuffer middle_divider = TableInteriorCell(middle.page_number(), 2);
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    EXPECT_EQ(6U, pager->page_count());
    EXPECT_EQ(PageNumber{4}, middle.page_number());
    EXPECT_EQ(0U, middle.staged_count());
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    ASSERT_EQ(3U, parent_view.cell_count());
    for (std::size_t index = 0U; index < parent_view.cell_count(); ++index) {
      const BtreeCellView divider = TakeValue(parent_view.cell(index));
      EXPECT_EQ(PageNumber{static_cast<std::uint32_t>(index + 3U)},
                divider.left_child().value_or(PageNumber{}));
      EXPECT_EQ(static_cast<std::int64_t>(index + 1U), divider.rowid());
    }
    EXPECT_EQ(PageNumber{6}, parent_view.rightmost_child().value_or(PageNumber{}));

    for (std::uint32_t page_number = 3U; page_number <= 6U; ++page_number) {
      const ByteBuffer page_image = SnapshotPage(*pager, owner, PageNumber{page_number});
      const BtreePageView leaf =
          TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{page_number}, geometry));
      ASSERT_EQ(1U, leaf.cell_count());
      EXPECT_EQ(static_cast<std::int64_t>(page_number - 2U), TakeValue(leaf.cell(0U)).rowid());
    }
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceProducesFiveSiblingsAtTheReferenceBound) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  ByteBuffer staged_cells{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafTable));

    for (std::size_t index = 0U; index < 84U; ++index) {
      RequireStatus(InsertTableLeafCell(left, index,
                                        TableLeafSpec{
                                            .rowid = static_cast<std::int64_t>(index + 1U),
                                            .payload_size = 1U,
                                        },
                                        {}, workspace));
      RequireStatus(InsertTableLeafCell(middle, index,
                                        TableLeafSpec{
                                            .rowid = static_cast<std::int64_t>(index + 85U),
                                            .payload_size = 1U,
                                        },
                                        {}, workspace));
      RequireStatus(InsertTableLeafCell(right, index,
                                        TableLeafSpec{
                                            .rowid = static_cast<std::int64_t>(index + 171U),
                                            .payload_size = 1U,
                                        },
                                        {}, workspace));
    }
    ASSERT_EQ(0U, left.free_bytes());
    ASSERT_EQ(0U, middle.free_bytes());
    ASSERT_EQ(0U, right.free_bytes());
    RequireStatus(InsertTableLeafCell(middle, 84U,
                                      TableLeafSpec{.rowid = 169, .payload_size = 252U},
                                      staged_cells.mutable_view().first(256U), workspace));
    RequireStatus(InsertTableLeafCell(middle, 85U,
                                      TableLeafSpec{.rowid = 170, .payload_size = 252U},
                                      staged_cells.mutable_view().subspan(256U, 256U), workspace));
    ASSERT_EQ(2U, middle.staged_count());

    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), 84);
    const ByteBuffer middle_divider = TableInteriorCell(middle.page_number(), 168);
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    EXPECT_EQ(7U, pager->page_count());
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    ASSERT_EQ(4U, parent_view.cell_count());
    EXPECT_EQ(PageNumber{7}, parent_view.rightmost_child().value_or(PageNumber{}));

    std::int64_t expected_rowid = 1;
    std::size_t total_cells = 0U;
    for (std::uint32_t page_number = 3U; page_number <= 7U; ++page_number) {
      const ByteBuffer page_image = SnapshotPage(*pager, owner, PageNumber{page_number});
      const BtreePageView leaf =
          TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{page_number}, geometry));
      ASSERT_EQ(BtreePageType::kLeafTable, leaf.type());
      ASSERT_GT(leaf.cell_count(), 0U);
      total_cells += leaf.cell_count();
      for (std::size_t index = 0U; index < leaf.cell_count(); ++index) {
        EXPECT_EQ(expected_rowid, TakeValue(leaf.cell(index)).rowid());
        ++expected_rowid;
      }
    }
    EXPECT_EQ(254U, total_cells);
    EXPECT_EQ(255, expected_rowid);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceOrdersInteriorTableCellsAndPreservesTheFinalChild) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));

    const auto insert = [&workspace](MutableBtreePage& target, std::size_t index, PageNumber child,
                                     std::int64_t rowid) {
      const ByteBuffer cell = TableInteriorCell(child, rowid);
      return target.InsertCell(index, cell.view(), child, {}, workspace);
    };
    RequireStatus(insert(left, 0U, PageNumber{10}, 1));
    RequireStatus(insert(left, 1U, PageNumber{11}, 2));
    RequireStatus(left.SetRightmostChild(PageNumber{12}));
    RequireStatus(insert(middle, 0U, PageNumber{20}, 4));
    RequireStatus(insert(middle, 1U, PageNumber{21}, 5));
    RequireStatus(middle.SetRightmostChild(PageNumber{22}));
    RequireStatus(insert(right, 0U, PageNumber{30}, 7));
    RequireStatus(insert(right, 1U, PageNumber{31}, 8));
    RequireStatus(right.SetRightmostChild(PageNumber{32}));

    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), 3);
    const ByteBuffer middle_divider = TableInteriorCell(middle.page_number(), 6);
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    EXPECT_EQ(0U, parent_view.cell_count());
    EXPECT_EQ(PageNumber{3}, parent_view.rightmost_child().value_or(PageNumber{}));

    const ByteBuffer page_image = SnapshotPage(*pager, owner, PageNumber{3});
    const BtreePageView combined =
        TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{3}, geometry));
    ASSERT_EQ(BtreePageType::kInteriorTable, combined.type());
    ASSERT_EQ(8U, combined.cell_count());
    const std::array<PageNumber, 8> expected_children{
        PageNumber{10}, PageNumber{11}, PageNumber{12}, PageNumber{20},
        PageNumber{21}, PageNumber{22}, PageNumber{30}, PageNumber{31},
    };
    for (std::size_t index = 0U; index < combined.cell_count(); ++index) {
      const BtreeCellView cell = TakeValue(combined.cell(index));
      EXPECT_EQ(static_cast<std::int64_t>(index + 1U), cell.rowid());
      EXPECT_EQ(expected_children[index], cell.left_child().value_or(PageNumber{}));
    }
    EXPECT_EQ(PageNumber{32}, combined.rightmost_child().value_or(PageNumber{}));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalancePadsTinyIndexLeafDividersWithoutChangingTheirParentSize) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  const std::array<std::byte, 2> record{std::byte{0x02}, std::byte{0x08}};
  const ByteBuffer leaf_cell = IndexCell(record, std::nullopt);
  ASSERT_EQ(ByteCount{4}, leaf_cell.size());
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));

    for (std::size_t index = 0U; index < 30U; ++index) {
      RequireStatus(left.InsertCell(index, leaf_cell.view(), std::nullopt, {}, workspace));
      RequireStatus(middle.InsertCell(index, leaf_cell.view(), std::nullopt, {}, workspace));
      RequireStatus(right.InsertCell(index, leaf_cell.view(), std::nullopt, {}, workspace));
    }
    const ByteBuffer left_divider = IndexCell(record, left.page_number());
    const ByteBuffer middle_divider = IndexCell(record, middle.page_number());
    ASSERT_EQ(ByteCount{7}, left_divider.size());
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    ASSERT_EQ(1U, parent_view.cell_count());
    const BtreeCellView divider = TakeValue(parent_view.cell(0U));
    EXPECT_EQ(ByteCount{7}, divider.encoded_size());
    EXPECT_EQ(ByteCount{2}, divider.payload_size());
    EXPECT_EQ(PageNumber{3}, divider.left_child().value_or(PageNumber{}));
    EXPECT_EQ(PageNumber{4}, parent_view.rightmost_child().value_or(PageNumber{}));

    std::size_t leaf_cells = 0U;
    for (std::uint32_t page_number = 3U; page_number <= 4U; ++page_number) {
      const ByteBuffer page_image = SnapshotPage(*pager, owner, PageNumber{page_number});
      const BtreePageView leaf =
          TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{page_number}, geometry));
      ASSERT_EQ(BtreePageType::kLeafIndex, leaf.type());
      leaf_cells += leaf.cell_count();
      for (std::size_t index = 0U; index < leaf.cell_count(); ++index) {
        EXPECT_EQ(ByteCount{4}, TakeValue(leaf.cell(index)).encoded_size());
      }
    }
    EXPECT_EQ(92U, leaf_cells + parent_view.cell_count());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceKeepsStagedIndexCellsOutsideDividerScratch) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  ByteBuffer staged_cells{geometry.page_size()};
  const ByteBuffer payload = FilledBuffer(610U, std::byte{0x5a});
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));

    const auto insert_leaf = [&](MutableBtreePage& target, std::size_t index,
                                 MutableByteView staged_copy) {
      const FormattedCell cell = TakeValue(FillIndexCell(owner, geometry, workspace, payload.view(),
                                                         BtreePageType::kLeafIndex, std::nullopt));
      EXPECT_EQ(108U, cell.bytes.size());
      return target.InsertCell(index, cell.bytes, std::nullopt, staged_copy, workspace);
    };
    for (std::size_t index = 0U; index < 4U; ++index) {
      RequireStatus(insert_leaf(left, index, {}));
      RequireStatus(insert_leaf(middle, index, {}));
      RequireStatus(insert_leaf(right, index, {}));
    }
    for (std::size_t index = 0U; index < 3U; ++index) {
      RequireStatus(
          insert_leaf(middle, index + 4U, staged_cells.mutable_view().subspan(index * 108U, 108U)));
    }
    ASSERT_EQ(3U, middle.staged_count());

    const auto divider = [&](PageNumber child) {
      return TakeValue(FillIndexCell(owner, geometry, workspace, payload.view(),
                                     BtreePageType::kInteriorIndex, child));
    };
    const FormattedCell left_divider = divider(left.page_number());
    ASSERT_EQ(112U, left_divider.bytes.size());
    RequireStatus(parent.InsertCell(0U, left_divider.bytes, left.page_number(), {}, workspace));
    const FormattedCell middle_divider = divider(middle.page_number());
    ASSERT_EQ(112U, middle_divider.bytes.size());
    RequireStatus(parent.InsertCell(1U, middle_divider.bytes, middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    EXPECT_EQ(0U, middle.staged_count());
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    ASSERT_EQ(3U, parent_view.cell_count());
    std::array<PageNumber, 4> child_pages{};
    for (std::size_t index = 0U; index < parent_view.cell_count(); ++index) {
      child_pages[index] = TakeValue(parent_view.cell(index)).left_child().value_or(PageNumber{});
    }
    child_pages.back() = parent_view.rightmost_child().value_or(PageNumber{});

    std::size_t leaf_cells = 0U;
    for (const PageNumber child : child_pages) {
      const ByteBuffer page_image = SnapshotPage(*pager, owner, child);
      const BtreePageView leaf =
          TakeValue(BtreePageView::Parse(page_image.view(), child, geometry));
      ASSERT_EQ(BtreePageType::kLeafIndex, leaf.type());
      leaf_cells += leaf.cell_count();
    }
    EXPECT_EQ(17U, leaf_cells + parent_view.cell_count());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceOrdersInteriorIndexCellsAndPreservesTheFinalChild) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage middle_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    MutableBtreePage middle = TakeValue(MutableBtreePage::Initialize(
        owner, middle_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));

    const auto insert = [&workspace](MutableBtreePage& target, std::size_t index, PageNumber child,
                                     std::uint8_t marker) {
      const std::array<std::byte, 1> record{static_cast<std::byte>(marker)};
      const ByteBuffer cell = IndexCell(record, child);
      return target.InsertCell(index, cell.view(), child, {}, workspace);
    };
    RequireStatus(insert(left, 0U, PageNumber{10}, 1U));
    RequireStatus(insert(left, 1U, PageNumber{11}, 2U));
    RequireStatus(left.SetRightmostChild(PageNumber{12}));
    RequireStatus(insert(middle, 0U, PageNumber{20}, 4U));
    RequireStatus(insert(middle, 1U, PageNumber{21}, 5U));
    RequireStatus(middle.SetRightmostChild(PageNumber{22}));
    RequireStatus(insert(right, 0U, PageNumber{30}, 7U));
    RequireStatus(insert(right, 1U, PageNumber{31}, 8U));
    RequireStatus(right.SetRightmostChild(PageNumber{32}));

    const std::array<std::byte, 1> left_marker{std::byte{3}};
    const std::array<std::byte, 1> middle_marker{std::byte{6}};
    const ByteBuffer left_divider = IndexCell(left_marker, left.page_number());
    const ByteBuffer middle_divider = IndexCell(middle_marker, middle.page_number());
    RequireStatus(parent.InsertCell(0U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(
        parent.InsertCell(1U, middle_divider.view(), middle.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(right.page_number()));
    owner.Release(left_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);

    RequireStatus(MutableBtreePage::BalanceNonroot(
        parent, middle, 1U, parent_overflow.mutable_view(), workspace, false));

    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(parent_allocation.owner_slot)).get().bytes(),
                             parent.page_number(), geometry));
    EXPECT_EQ(0U, parent_view.cell_count());
    EXPECT_EQ(PageNumber{3}, parent_view.rightmost_child().value_or(PageNumber{}));

    const ByteBuffer page_image = SnapshotPage(*pager, owner, PageNumber{3});
    const BtreePageView combined =
        TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{3}, geometry));
    ASSERT_EQ(BtreePageType::kInteriorIndex, combined.type());
    ASSERT_EQ(8U, combined.cell_count());
    const std::array<PageNumber, 8> expected_children{
        PageNumber{10}, PageNumber{11}, PageNumber{12}, PageNumber{20},
        PageNumber{21}, PageNumber{22}, PageNumber{30}, PageNumber{31},
    };
    for (std::size_t index = 0U; index < combined.cell_count(); ++index) {
      const BtreeCellView cell = TakeValue(combined.cell(index));
      ASSERT_EQ(1U, cell.local_payload().size());
      EXPECT_EQ(static_cast<std::byte>(index + 1U), cell.local_payload()[0]);
      EXPECT_EQ(expected_children[index], cell.left_child().value_or(PageNumber{}));
    }
    EXPECT_EQ(PageNumber{32}, combined.rightmost_child().value_or(PageNumber{}));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, NonrootBalanceShallowsAnEmptyPageOneRoot) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer parent_overflow{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage child_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    ASSERT_EQ(PageNumber{2}, child_allocation.page_number);
    const std::size_t root_slot = TakeValue(owner.Borrow(PageNumber{1}));
    MutableBtreePage root = TakeValue(MutableBtreePage::Open(owner, root_slot, geometry));
    RequireStatus(root.Zero(BtreePageType::kInteriorTable));
    MutableBtreePage child = TakeValue(MutableBtreePage::Initialize(
        owner, child_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(root.SetRightmostChild(child.page_number()));
    const ByteBuffer header_before =
        ByteBuffer::CopyOf(TakeValue(owner.Frame(root_slot)).get().bytes().first(100U));

    RequireStatus(MutableBtreePage::BalanceNonroot(root, child, 0U, parent_overflow.mutable_view(),
                                                   workspace, true));

    const ByteView root_bytes = TakeValue(owner.Frame(root_slot)).get().bytes();
    EXPECT_TRUE(std::ranges::equal(header_before.view().first(28U), root_bytes.first(28U)));
    EXPECT_TRUE(
        std::ranges::equal(header_before.view().subspan(40U), root_bytes.subspan(40U, 60U)));
    EXPECT_EQ(2U, Load32(root_bytes, 28U));
    EXPECT_EQ(PageNumber{2}, PageNumber{Load32(root_bytes, 32U)});
    EXPECT_EQ(1U, Load32(root_bytes, 36U));
    const BtreePageView root_view =
        TakeValue(BtreePageView::Parse(root_bytes, PageNumber{1}, geometry));
    EXPECT_EQ(BtreePageType::kLeafTable, root_view.type());
    EXPECT_EQ(0U, root_view.cell_count());
    EXPECT_FALSE(root_view.rightmost_child().has_value());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, CursorBalanceDeepensAndSplitsAnOverfullRoot) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer staged_cell{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.PromoteCurrent());
    MutableBtreePage root =
        TakeValue(MutableBtreePage::Open(owner, cursor.current_owner_slot(), geometry));
    RequireStatus(InsertTableLeafCell(root, 0U, TableLeafSpec{.rowid = 1, .payload_size = 380U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(root, 1U, TableLeafSpec{.rowid = 2, .payload_size = 380U},
                                      staged_cell.mutable_view(), workspace));
    ASSERT_EQ(1U, root.staged_count());

    RequireStatus(cursor.Balance(std::move(root), workspace));

    EXPECT_EQ(WritableCursorState::kInvalid, cursor.state());
    EXPECT_EQ(1U, cursor.depth());
    EXPECT_EQ(3U, pager->page_count());
    const ByteView root_bytes =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(PageNumber{1})))).get().bytes();
    const BtreePageView root_view =
        TakeValue(BtreePageView::Parse(root_bytes, PageNumber{1}, geometry));
    ASSERT_EQ(BtreePageType::kInteriorTable, root_view.type());
    ASSERT_EQ(1U, root_view.cell_count());
    const BtreeCellView divider = TakeValue(root_view.cell(0U));
    EXPECT_EQ(PageNumber{2}, divider.left_child().value_or(PageNumber{}));
    EXPECT_EQ(1, divider.rowid());
    EXPECT_EQ(PageNumber{3}, root_view.rightmost_child().value_or(PageNumber{}));
    for (std::uint32_t page_number = 2U; page_number <= 3U; ++page_number) {
      const ByteBuffer page_image = SnapshotPage(*pager, owner, PageNumber{page_number});
      const BtreePageView leaf =
          TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{page_number}, geometry));
      ASSERT_EQ(1U, leaf.cell_count());
      EXPECT_EQ(static_cast<std::int64_t>(page_number - 1U), TakeValue(leaf.cell(0U)).rowid());
    }
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, CursorBalancePropagatesQuickBalanceIntoTheRoot) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  std::array<std::byte, 32> staged_cell{};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage leaf_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const std::size_t root_slot = TakeValue(owner.Borrow(PageNumber{1}));
    MutableBtreePage root = TakeValue(MutableBtreePage::Open(owner, root_slot, geometry));
    RequireStatus(root.Zero(BtreePageType::kInteriorTable));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage leaf = TakeValue(MutableBtreePage::Initialize(
        owner, leaf_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(root.SetRightmostChild(parent.page_number()));
    const ByteBuffer large_divider = TableInteriorCell(PageNumber{100}, -1);
    for (std::size_t index = 0U; index < 33U; ++index) {
      RequireStatus(parent.InsertCell(index, large_divider.view(), PageNumber{100}, {}, workspace));
    }
    RequireStatus(parent.SetRightmostChild(leaf.page_number()));
    RequireStatus(InsertTableLeafCell(leaf, 0U, TableLeafSpec{.rowid = 1, .payload_size = 477U}, {},
                                      workspace));
    owner.Release(parent_allocation.owner_slot);
    owner.Release(leaf_allocation.owner_slot);

    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const TableSeekResult found = TakeValue(cursor.SeekTable(2));
    ASSERT_EQ(3U, found.tree_depth);
    RequireStatus(cursor.PromoteCurrent());
    MutableBtreePage current =
        TakeValue(MutableBtreePage::Open(owner, cursor.current_owner_slot(), geometry));
    RequireStatus(InsertTableLeafCell(current, 1U, TableLeafSpec{.rowid = 2, .payload_size = 20U},
                                      MutableByteView{staged_cell}, workspace));

    RequireStatus(cursor.Balance(std::move(current), workspace));

    EXPECT_EQ(WritableCursorState::kInvalid, cursor.state());
    EXPECT_EQ(1U, cursor.depth());
    EXPECT_EQ(5U, pager->page_count());
    const ByteView root_bytes = TakeValue(owner.Frame(root_slot)).get().bytes();
    const BtreePageView root_view =
        TakeValue(BtreePageView::Parse(root_bytes, PageNumber{1}, geometry));
    ASSERT_EQ(BtreePageType::kInteriorTable, root_view.type());
    ASSERT_EQ(1U, root_view.cell_count());
    EXPECT_EQ(PageNumber{2}, TakeValue(root_view.cell(0U)).left_child().value_or(PageNumber{}));
    EXPECT_EQ(PageNumber{5}, root_view.rightmost_child().value_or(PageNumber{}));

    for (std::uint32_t page_number = 3U; page_number <= 4U; ++page_number) {
      const ByteBuffer page_image = SnapshotPage(*pager, owner, PageNumber{page_number});
      const BtreePageView leaf_view =
          TakeValue(BtreePageView::Parse(page_image.view(), PageNumber{page_number}, geometry));
      ASSERT_EQ(BtreePageType::kLeafTable, leaf_view.type());
      ASSERT_EQ(1U, leaf_view.cell_count());
      EXPECT_EQ(static_cast<std::int64_t>(page_number - 2U), TakeValue(leaf_view.cell(0U)).rowid());
    }
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, CursorBalancePreservesAStagedAncestorDivider) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  ByteBuffer staged_divider_storage{geometry.page_size()};
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage parent_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage current_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const std::size_t root_slot = TakeValue(owner.Borrow(PageNumber{1}));
    MutableBtreePage root = TakeValue(MutableBtreePage::Open(owner, root_slot, geometry));
    RequireStatus(root.Zero(BtreePageType::kInteriorTable));
    MutableBtreePage parent = TakeValue(MutableBtreePage::Initialize(
        owner, parent_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    MutableBtreePage current = TakeValue(MutableBtreePage::Initialize(
        owner, current_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(root.SetRightmostChild(parent.page_number()));
    const ByteBuffer large_divider = TableInteriorCell(PageNumber{100}, -1);
    for (std::size_t index = 0U; index < 32U; ++index) {
      RequireStatus(parent.InsertCell(index, large_divider.view(), PageNumber{100}, {}, workspace));
    }
    const ByteBuffer left_divider = TableInteriorCell(left.page_number(), -1);
    RequireStatus(parent.InsertCell(32U, left_divider.view(), left.page_number(), {}, workspace));
    RequireStatus(parent.SetRightmostChild(current.page_number()));
    RequireStatus(InsertTableLeafCell(left, 0U, TableLeafSpec{.rowid = 1, .payload_size = 0U}, {},
                                      workspace));
    RequireStatus(InsertTableLeafCell(current, 0U, TableLeafSpec{.rowid = 2, .payload_size = 0U},
                                      {}, workspace));
    RequireStatus(InsertTableLeafCell(right, 0U, TableLeafSpec{.rowid = 3, .payload_size = 0U}, {},
                                      workspace));
    owner.Release(parent_allocation.owner_slot);
    owner.Release(current_allocation.owner_slot);
    owner.Release(right_allocation.owner_slot);
    owner.Release(left_allocation.owner_slot);

    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const TableSeekResult found = TakeValue(cursor.SeekTable(2));
    ASSERT_EQ(3U, found.tree_depth);
    RequireStatus(cursor.PromoteCurrent());
    const std::size_t parent_slot = TakeValue(owner.Borrow(parent_allocation.page_number));
    RequireStatus(owner.Promote(parent_slot));
    MutableBtreePage live_parent = TakeValue(MutableBtreePage::Open(owner, parent_slot, geometry));
    const ByteBuffer staged_divider = TableInteriorCell(current_allocation.page_number, 1);
    RequireStatus(live_parent.InsertCell(33U, staged_divider.view(), current_allocation.page_number,
                                         staged_divider_storage.mutable_view(), workspace));
    RequireStatus(live_parent.SetRightmostChild(right_allocation.page_number));
    ASSERT_EQ(1U, live_parent.staged_count());
    MutableBtreePage live_current =
        TakeValue(MutableBtreePage::Open(owner, cursor.current_owner_slot(), geometry));

    RequireStatus(cursor.Balance(std::move(live_current), workspace));

    EXPECT_EQ(WritableCursorState::kInvalid, cursor.state());
    EXPECT_EQ(2U, cursor.depth());
    const ByteBuffer parent_image = SnapshotPage(*pager, owner, parent_allocation.page_number);
    const BtreePageView parent_view = TakeValue(
        BtreePageView::Parse(parent_image.view(), parent_allocation.page_number, geometry));
    EXPECT_EQ(32U, parent_view.cell_count());
    EXPECT_EQ(left_allocation.page_number, parent_view.rightmost_child().value_or(PageNumber{}));
    const ByteBuffer combined_image = SnapshotPage(*pager, owner, left_allocation.page_number);
    const BtreePageView combined = TakeValue(
        BtreePageView::Parse(combined_image.view(), left_allocation.page_number, geometry));
    ASSERT_EQ(3U, combined.cell_count());
    for (std::size_t index = 0U; index < combined.cell_count(); ++index) {
      EXPECT_EQ(static_cast<std::int64_t>(index + 1U), TakeValue(combined.cell(index)).rowid());
    }
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, CursorBalanceShallowsAnUnderfullRootChild) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage child_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const std::size_t root_slot = TakeValue(owner.Borrow(PageNumber{1}));
    MutableBtreePage root = TakeValue(MutableBtreePage::Open(owner, root_slot, geometry));
    RequireStatus(root.Zero(BtreePageType::kInteriorTable));
    MutableBtreePage child = TakeValue(MutableBtreePage::Initialize(
        owner, child_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(root.SetRightmostChild(child.page_number()));
    RequireStatus(InsertTableLeafCell(child, 0U, TableLeafSpec{.rowid = 1, .payload_size = 0U}, {},
                                      workspace));
    owner.Release(child_allocation.owner_slot);

    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const TableSeekResult found = TakeValue(cursor.SeekTable(1));
    ASSERT_EQ(2U, found.tree_depth);
    RequireStatus(cursor.PromoteCurrent());
    MutableBtreePage current =
        TakeValue(MutableBtreePage::Open(owner, cursor.current_owner_slot(), geometry));

    RequireStatus(cursor.Balance(std::move(current), workspace));

    EXPECT_EQ(WritableCursorState::kInvalid, cursor.state());
    EXPECT_EQ(1U, cursor.depth());
    const ByteView root_bytes = TakeValue(owner.Frame(root_slot)).get().bytes();
    const BtreePageView root_view =
        TakeValue(BtreePageView::Parse(root_bytes, PageNumber{1}, geometry));
    ASSERT_EQ(BtreePageType::kLeafTable, root_view.type());
    ASSERT_EQ(1U, root_view.cell_count());
    EXPECT_EQ(1, TakeValue(root_view.cell(0U)).rowid());
    EXPECT_EQ(PageNumber{2}, PageNumber{Load32(root_bytes, 32U)});
    EXPECT_EQ(1U, Load32(root_bytes, 36U));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, CursorBalanceStopsAtTheReferenceFreeSpaceThreshold) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage child_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const std::size_t root_slot = TakeValue(owner.Borrow(PageNumber{1}));
    MutableBtreePage root = TakeValue(MutableBtreePage::Open(owner, root_slot, geometry));
    RequireStatus(root.Zero(BtreePageType::kInteriorTable));
    MutableBtreePage child = TakeValue(MutableBtreePage::Initialize(
        owner, child_allocation.owner_slot, geometry, BtreePageType::kLeafTable));
    RequireStatus(root.SetRightmostChild(child.page_number()));
    RequireStatus(InsertTableLeafCell(child, 0U, TableLeafSpec{.rowid = 1, .payload_size = 200U},
                                      {}, workspace));
    owner.Release(child_allocation.owner_slot);

    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const TableSeekResult found = TakeValue(cursor.SeekTable(1));
    ASSERT_EQ(2U, found.tree_depth);
    RequireStatus(cursor.PromoteCurrent());
    MutableBtreePage current =
        TakeValue(MutableBtreePage::Open(owner, cursor.current_owner_slot(), geometry));
    const ByteBuffer root_before =
        ByteBuffer::CopyOf(TakeValue(owner.Frame(root_slot)).get().bytes());
    const ByteBuffer child_before =
        ByteBuffer::CopyOf(TakeValue(owner.Frame(cursor.current_owner_slot())).get().bytes());

    RequireStatus(cursor.Balance(std::move(current), workspace));

    EXPECT_EQ(WritableCursorState::kInvalid, cursor.state());
    EXPECT_EQ(2U, cursor.depth());
    EXPECT_TRUE(
        std::ranges::equal(root_before.view(), TakeValue(owner.Frame(root_slot)).get().bytes()));
    EXPECT_TRUE(std::ranges::equal(
        child_before.view(), TakeValue(owner.Frame(cursor.current_owner_slot())).get().bytes()));
    EXPECT_EQ(0U, Load32(TakeValue(owner.Frame(root_slot)).get().bytes(), 36U));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, DeepensAnOverfullLeafRootAndTransfersItsStagedCell) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t root_slot = TakeValue(owner.AcquireWrite(PageNumber{1}));
    MutableBtreePage root = TakeValue(MutableBtreePage::Open(owner, root_slot, geometry));
    RequireStatus(InsertTableLeafCell(root, 0U, TableLeafSpec{.rowid = 1, .payload_size = 380U}, {},
                                      workspace));
    std::array<std::byte, 32> staged_copy{};
    RequireStatus(InsertTableLeafCell(root, 1U, TableLeafSpec{.rowid = 2, .payload_size = 20U},
                                      MutableByteView{staged_copy}, workspace));
    const ByteView root_before = TakeValue(owner.Frame(root_slot)).get().bytes();
    const BtreePageView root_before_view =
        TakeValue(BtreePageView::Parse(root_before, PageNumber{1}, geometry));
    const std::size_t copied_prefix_size = 108U + root_before_view.cell_count() * 2U;
    const ByteBuffer copied_prefix =
        ByteBuffer::CopyOf(root_before.subspan(100U, copied_prefix_size));
    const ByteBuffer cell_content =
        ByteBuffer::CopyOf(root_before.subspan(root_before_view.cell_content_offset().value()));
    std::array<std::byte, 100> expected_header{};
    std::ranges::copy(root_before.first<100>(), expected_header.begin());
    Store32(MutableByteView{expected_header}, 28U, 2U);

    const MutableBtreePage child = TakeValue(MutableBtreePage::BalanceDeeper(root));

    EXPECT_EQ(PageNumber{2}, child.page_number());
    EXPECT_EQ(2U, owner.size());
    EXPECT_EQ(0U, root.staged_count());
    const ByteView root_bytes = TakeValue(owner.Frame(root_slot)).get().bytes();
    EXPECT_TRUE(std::ranges::equal(expected_header, root_bytes.first<100>()));
    const BtreePageView root_view =
        TakeValue(BtreePageView::Parse(root_bytes, PageNumber{1}, geometry));
    EXPECT_EQ(BtreePageType::kInteriorTable, root_view.type());
    EXPECT_EQ(0U, root_view.cell_count());
    EXPECT_EQ(child.page_number(), root_view.rightmost_child().value_or(PageNumber{}));

    const BtreePageView child_view = TakeValue(BtreePageView::Parse(
        TakeValue(owner.Frame(TakeValue(owner.Borrow(child.page_number())))).get().bytes(),
        child.page_number(), geometry));
    EXPECT_EQ(BtreePageType::kLeafTable, child_view.type());
    ASSERT_EQ(1U, child_view.cell_count());
    EXPECT_EQ(1, TakeValue(child_view.cell(0U)).rowid());
    const ByteView child_bytes =
        TakeValue(owner.Frame(TakeValue(owner.Borrow(child.page_number())))).get().bytes();
    EXPECT_TRUE(
        std::ranges::equal(copied_prefix.view(), child_bytes.first(copied_prefix.size().value())));
    EXPECT_TRUE(std::ranges::equal(cell_content.view(),
                                   child_bytes.subspan(child_view.cell_content_offset().value())));
    ASSERT_EQ(1U, child.staged_count());
    const std::optional<StagedCell> staged = child.staged_cell(0U);
    ASSERT_TRUE(staged.has_value());
    EXPECT_EQ(1U, staged->index);
    const ByteBuffer staged_page =
        CellImagePage(staged->bytes, BtreePageType::kLeafTable, geometry);
    EXPECT_EQ(
        2,
        TakeValue(
            TakeValue(BtreePageView::Parse(staged_page.view(), PageNumber{98}, geometry)).cell(0U))
            .rowid());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, DeepensAnInteriorRootWithoutChangingTheChildPageKind) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage existing_child = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage root = TakeValue(MutableBtreePage::Initialize(
        owner, root_allocation.owner_slot, geometry, BtreePageType::kInteriorTable));
    RequireStatus(root.SetRightmostChild(existing_child.page_number));
    const ByteBuffer large_divider = TableInteriorCell(existing_child.page_number, -1);
    for (std::size_t index = 0U; index < 33U; ++index) {
      RequireStatus(
          root.InsertCell(index, large_divider.view(), existing_child.page_number, {}, workspace));
    }
    std::array<std::byte, 16> staged_copy{};
    const ByteBuffer staged_divider = TableInteriorCell(existing_child.page_number, 1);
    RequireStatus(root.InsertCell(33U, staged_divider.view(), existing_child.page_number,
                                  MutableByteView{staged_copy}, workspace));
    ASSERT_EQ(1U, root.staged_count());

    const MutableBtreePage child = TakeValue(MutableBtreePage::BalanceDeeper(root));

    EXPECT_EQ(BtreePageType::kInteriorTable, root.type());
    EXPECT_EQ(BtreePageType::kInteriorTable, child.type());
    EXPECT_EQ(0U, root.cell_count());
    EXPECT_EQ(0U, root.staged_count());
    EXPECT_EQ(33U, child.cell_count());
    EXPECT_EQ(1U, child.staged_count());
    const BtreePageView root_view = TakeValue(
        BtreePageView::Parse(TakeValue(owner.Frame(root_allocation.owner_slot)).get().bytes(),
                             root.page_number(), geometry));
    EXPECT_EQ(child.page_number(), root_view.rightmost_child().value_or(PageNumber{}));
    const BtreePageView child_view = TakeValue(BtreePageView::Parse(
        TakeValue(owner.Frame(TakeValue(owner.Borrow(child.page_number())))).get().bytes(),
        child.page_number(), geometry));
    EXPECT_EQ(existing_child.page_number, child_view.rightmost_child().value_or(PageNumber{}));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeBalance, RejectsRootDeepeningWhenAStagedCellAliasesTheRootPage) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    const std::size_t root_slot = TakeValue(owner.AcquireWrite(PageNumber{1}));
    MutableBtreePage root = TakeValue(MutableBtreePage::Open(owner, root_slot, geometry));
    RequireStatus(InsertTableLeafCell(root, 0U, TableLeafSpec{.rowid = 1, .payload_size = 380U}, {},
                                      workspace));
    const ByteView root_bytes = TakeValue(owner.Frame(root_slot)).get().bytes();
    const BtreePageView root_view =
        TakeValue(BtreePageView::Parse(root_bytes, PageNumber{1}, geometry));
    const std::size_t cell_offset = TakeValue(root_view.cell_offset(0U)).value();
    const std::size_t cell_size = TakeValue(root_view.cell(0U)).encoded_size().value();
    RequireStatus(root.InsertCell(1U, root_bytes.subspan(cell_offset, cell_size), std::nullopt, {},
                                  workspace));
    ASSERT_EQ(1U, root.staged_count());
    const ByteBuffer before = ByteBuffer::CopyOf(root_bytes);

    const auto deepened = MutableBtreePage::BalanceDeeper(root);

    ASSERT_FALSE(deepened.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, deepened.error().code());
    EXPECT_EQ(1U, pager->page_count());
    EXPECT_TRUE(std::ranges::equal(before.view(), TakeValue(owner.Frame(root_slot)).get().bytes()));
    EXPECT_FALSE(pager->write_failure_code().has_value());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, InsertsTableRowsInOrderAndRejectsInsertOnlyDuplicates) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const auto insert = [&pager, &workspace](std::int64_t rowid, std::byte value,
                                           BtreeInsertMode mode) {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const std::array payload{value};
    return cursor.InsertTable(rowid, payload, mode, workspace);
  };

  RequireStatus(insert(2, std::byte{0x22}, BtreeInsertMode::kInsertOnly));
  RequireStatus(insert(1, std::byte{0x11}, BtreeInsertMode::kInsertOnly));
  RequireStatus(insert(3, std::byte{0x33}, BtreeInsertMode::kInsertOnly));

  const ByteBuffer before =
      ByteBuffer::CopyOf(TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes());
  const auto duplicate = insert(2, std::byte{0x7f}, BtreeInsertMode::kInsertOnly);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate.error().code());
  EXPECT_TRUE(
      std::ranges::equal(before.view(), TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes()));
  EXPECT_FALSE(pager->write_failure_code().has_value());

  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const ByteBuffer root_image = SnapshotPage(*pager, PageNumber{1});
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), PageNumber{1}, geometry));
  ASSERT_EQ(BtreePageType::kLeafTable, root.type());
  ASSERT_EQ(3U, root.cell_count());
  for (std::size_t index = 0U; index < root.cell_count(); ++index) {
    const BtreeCellView cell = TakeValue(root.cell(index));
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), cell.rowid());
    ASSERT_EQ(1U, cell.local_payload().size());
    EXPECT_EQ(static_cast<std::byte>((index + 1U) * 0x11U), cell.local_payload()[0]);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, BalancesASequenceOfLargeTableRows) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const std::array<std::int64_t, 6> insertion_order{3, 1, 5, 2, 4, 6};
  for (const std::int64_t rowid : insertion_order) {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const ByteBuffer payload =
        FilledBuffer(380U, static_cast<std::byte>(static_cast<std::uint8_t>(rowid)));
    RequireStatus(
        cursor.InsertTable(rowid, payload.view(), BtreeInsertMode::kInsertOnly, workspace));
  }

  EXPECT_GT(pager->page_count(), 1U);
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const ByteBuffer root_image = SnapshotPage(*pager, PageNumber{1});
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), PageNumber{1}, geometry));
  EXPECT_EQ(BtreePageType::kInteriorTable, root.type());
  for (std::int64_t rowid = 1; rowid <= 6; ++rowid) {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const TableSeekResult found = TakeValue(cursor.SeekTable(rowid));
    ASSERT_TRUE(found.exact);
    const BtreePageView page = TakeValue(cursor.CurrentPage());
    const BtreeCellView cell = TakeValue(page.cell(cursor.current_index()));
    ASSERT_EQ(380U, cell.local_payload().size());
    EXPECT_EQ(static_cast<std::byte>(static_cast<std::uint8_t>(rowid)), cell.local_payload()[0]);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, SameSizeReplacementRetainsOverflowPages) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const ByteBuffer original = FilledBuffer(2'000U, std::byte{0x11});
  const ByteBuffer replacement = FilledBuffer(2'000U, std::byte{0x22});
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.InsertTable(7, original.view(), BtreeInsertMode::kInsertOnly, workspace));
  }

  PageNumber first_overflow;
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    ASSERT_TRUE(TakeValue(cursor.SeekTable(7)).exact);
    const BtreeCellView cell =
        TakeValue(TakeValue(cursor.CurrentPage()).cell(cursor.current_index()));
    first_overflow = cell.first_overflow_page().value_or(PageNumber{});
    ASSERT_NE(PageNumber{}, first_overflow);
  }
  const std::uint32_t page_count_before = pager->page_count();
  const std::uint32_t free_count_before =
      Load32(TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes(), 36U);

  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.InsertTable(7, replacement.view(), BtreeInsertMode::kReplace, workspace));
  }

  EXPECT_EQ(page_count_before, pager->page_count());
  EXPECT_EQ(free_count_before,
            Load32(TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes(), 36U));
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    ASSERT_TRUE(TakeValue(cursor.SeekTable(7)).exact);
    const BtreeCellView cell =
        TakeValue(TakeValue(cursor.CurrentPage()).cell(cursor.current_index()));
    EXPECT_EQ(first_overflow, cell.first_overflow_page().value_or(PageNumber{}));
    EXPECT_TRUE(std::ranges::all_of(cell.local_payload(),
                                    [](std::byte value) { return value == std::byte{0x22}; }));
  }
  {
    const auto overflow_pin = TakeValue(pager->ReadPage(first_overflow));
    const OverflowPageView overflow = TakeValue(OverflowPageView::Parse(
        overflow_pin.frame().bytes(),
        TakeValue(BtreePageGeometry::Create(pager->header()->page_size(),
                                            pager->header()->usable_size()))));
    EXPECT_TRUE(std::ranges::all_of(overflow.payload(),
                                    [](std::byte value) { return value == std::byte{0x22}; }));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, DifferentSizeReplacementReleasesTheOldOverflowChain) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const ByteBuffer original = FilledBuffer(2'000U, std::byte{0x11});
  const ByteBuffer replacement = FilledBuffer(10U, std::byte{0x33});
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.InsertTable(9, original.view(), BtreeInsertMode::kInsertOnly, workspace));
  }
  const std::uint32_t free_count_before =
      Load32(TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes(), 36U);

  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.InsertTable(9, replacement.view(), BtreeInsertMode::kReplace, workspace));
  }

  EXPECT_GT(Load32(TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes(), 36U),
            free_count_before);
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    ASSERT_TRUE(TakeValue(cursor.SeekTable(9)).exact);
    const BtreeCellView cell =
        TakeValue(TakeValue(cursor.CurrentPage()).cell(cursor.current_index()));
    EXPECT_EQ(ByteCount{10}, cell.payload_size());
    EXPECT_FALSE(cell.first_overflow_page().has_value());
    EXPECT_TRUE(std::ranges::all_of(cell.local_payload(),
                                    [](std::byte value) { return value == std::byte{0x33}; }));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, SameLocalCellSizeReplacementKeepsTheCellOffset) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.InsertTable(1, {}, BtreeInsertMode::kInsertOnly, workspace));
  }
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const ByteBuffer before_image = SnapshotPage(*pager, PageNumber{1});
  const BtreePageView before =
      TakeValue(BtreePageView::Parse(before_image.view(), PageNumber{1}, geometry));
  const ByteCount offset_before = TakeValue(before.cell_offset(0U));
  const ByteCount free_before = TakeValue(before.AnalyzeFreeSpace()).total();

  const std::array payload{std::byte{0x44}};
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.InsertTable(1, payload, BtreeInsertMode::kReplace, workspace));
  }

  const ByteBuffer after_image = SnapshotPage(*pager, PageNumber{1});
  const BtreePageView after =
      TakeValue(BtreePageView::Parse(after_image.view(), PageNumber{1}, geometry));
  EXPECT_EQ(offset_before, TakeValue(after.cell_offset(0U)));
  EXPECT_EQ(free_before, TakeValue(after.AnalyzeFreeSpace()).total());
  const BtreeCellView cell = TakeValue(after.cell(0U));
  EXPECT_EQ(ByteCount{1}, cell.payload_size());
  ASSERT_EQ(1U, cell.local_payload().size());
  EXPECT_EQ(std::byte{0x44}, cell.local_payload()[0]);
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, InsertsIndexRecordsInOrderAndRejectsDuplicates) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root = TakeValue(AllocateBtreePage(owner, geometry));
    static_cast<void>(TakeValue(
        MutableBtreePage::Initialize(owner, root.owner_slot, geometry, BtreePageType::kLeafIndex)));
    root_page = root.page_number;
  }
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  const auto insert = [&pager, &workspace, &columns, options, root_page](std::int64_t value,
                                                                         BtreeInsertMode mode) {
    std::array<SqlValue, 1> key{SqlValue::Integer(value)};
    const ByteBuffer record = TakeValue(EncodeRecord(key, options));
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    return cursor.InsertIndex(record.view(), key, columns, options, mode, scratch, workspace);
  };

  RequireStatus(insert(2, BtreeInsertMode::kInsertOnly));
  RequireStatus(insert(1, BtreeInsertMode::kInsertOnly));
  RequireStatus(insert(3, BtreeInsertMode::kInsertOnly));
  const ByteBuffer before = SnapshotPage(*pager, root_page);
  const auto duplicate = insert(2, BtreeInsertMode::kInsertOnly);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate.error().code());
  EXPECT_TRUE(std::ranges::equal(before.view(), SnapshotPage(*pager, root_page).view()));
  EXPECT_FALSE(pager->write_failure_code().has_value());
  {
    std::array<SqlValue, 1> stored_key{SqlValue::Integer(1)};
    const ByteBuffer mismatched_record = TakeValue(EncodeRecord(stored_key, options));
    std::array<SqlValue, 1> comparison_key{SqlValue::Integer(9)};
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    const auto mismatched =
        cursor.InsertIndex(mismatched_record.view(), comparison_key, columns, options,
                           BtreeInsertMode::kInsertOnly, scratch, workspace);
    ASSERT_FALSE(mismatched.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, mismatched.error().code());
  }
  EXPECT_TRUE(std::ranges::equal(before.view(), SnapshotPage(*pager, root_page).view()));
  {
    std::array<SqlValue, 2> stored_key{SqlValue::Integer(4), SqlValue::Integer(5)};
    const ByteBuffer record = TakeValue(EncodeRecord(stored_key, options));
    std::array<SqlValue, 1> prefix{SqlValue::Integer(4)};
    const std::array<IndexColumnOrder, 2> two_columns{
        IndexColumnOrder{BinaryCollation()},
        IndexColumnOrder{BinaryCollation()},
    };
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    const auto incomplete = cursor.InsertIndex(record.view(), prefix, two_columns, options,
                                               BtreeInsertMode::kInsertOnly, scratch, workspace);
    ASSERT_FALSE(incomplete.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, incomplete.error().code());
  }
  EXPECT_TRUE(std::ranges::equal(before.view(), SnapshotPage(*pager, root_page).view()));

  const ByteBuffer root_image = SnapshotPage(*pager, root_page);
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), root_page, geometry));
  ASSERT_EQ(BtreePageType::kLeafIndex, root.type());
  ASSERT_EQ(3U, root.cell_count());
  for (std::size_t index = 0U; index < root.cell_count(); ++index) {
    const std::array<SqlValue, 1> key{
        SqlValue::Integer(static_cast<std::int64_t>(index) + 1),
    };
    const ByteBuffer expected = TakeValue(EncodeRecord(key, options));
    EXPECT_TRUE(std::ranges::equal(expected.view(), TakeValue(root.cell(index)).local_payload()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, BalancesLargeIndexRecordsInsertedOutOfOrder) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root = TakeValue(AllocateBtreePage(owner, geometry));
    static_cast<void>(TakeValue(
        MutableBtreePage::Initialize(owner, root.owner_slot, geometry, BtreePageType::kLeafIndex)));
    root_page = root.page_number;
  }
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  const auto make_key = [](std::uint8_t marker) {
    ByteBuffer blob{ByteCount{90}};
    std::ranges::fill(blob.mutable_view(), static_cast<std::byte>(marker));
    return std::array<SqlValue, 1>{SqlValue::Blob(std::move(blob))};
  };
  const auto insert = [&pager, &workspace, &columns, &make_key, options,
                       root_page](std::uint8_t marker) {
    std::array<SqlValue, 1> key = make_key(marker);
    const ByteBuffer record = TakeValue(EncodeRecord(key, options));
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    return cursor.InsertIndex(record.view(), key, columns, options, BtreeInsertMode::kInsertOnly,
                              scratch, workspace);
  };
  const std::array<std::uint8_t, 12> insertion_order{7U, 1U, 10U, 4U, 12U, 2U,
                                                     9U, 5U, 11U, 3U, 8U,  6U};
  for (const std::uint8_t marker : insertion_order) {
    RequireStatus(insert(marker));
  }

  EXPECT_GT(pager->page_count(), root_page.value());
  const ByteBuffer root_image = SnapshotPage(*pager, root_page);
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), root_page, geometry));
  EXPECT_EQ(BtreePageType::kInteriorIndex, root.type());
  for (std::uint8_t marker = 1U; marker <= 12U; ++marker) {
    std::array<SqlValue, 1> key = make_key(marker);
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    EXPECT_TRUE(TakeValue(cursor.SeekIndex(key, columns, options, scratch)).exact);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, ReplacesAnInteriorIndexRecordWithoutChangingItsLeftChild) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const ByteBuffer ten = TakeValue(EncodeRecord(std::array{SqlValue::Integer(10)}, options));
  const ByteBuffer forty_two = TakeValue(EncodeRecord(std::array{SqlValue::Integer(42)}, options));
  const ByteBuffer seventy = TakeValue(EncodeRecord(std::array{SqlValue::Integer(70)}, options));
  const std::array<std::byte, 4> wide_forty_two{
      std::byte{0x02},
      std::byte{0x02},
      std::byte{0x00},
      std::byte{0x2a},
  };

  RequireStatus(pager->BeginWrite());
  for (std::uint32_t page = 2U; page <= 4U; ++page) {
    EXPECT_EQ(PageNumber{page}, TakeValue(pager->AllocatePage()).frame().page_number());
  }
  {
    auto root = TakeValue(pager->WritePage(PageNumber{2}));
    WriteIndexPage(root.mutable_bytes(), BtreePageType::kInteriorIndex, forty_two.view(),
                   PageNumber{3}, PageNumber{4});
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

  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  std::array<SqlValue, 1> key{SqlValue::Integer(42)};
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{2}, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.InsertIndex(wide_forty_two, key, columns, options,
                                     BtreeInsertMode::kReplace, scratch, workspace));
  }

  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const ByteBuffer root_image = SnapshotPage(*pager, PageNumber{2});
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), PageNumber{2}, geometry));
  ASSERT_EQ(1U, root.cell_count());
  const BtreeCellView cell = TakeValue(root.cell(0U));
  EXPECT_EQ(PageNumber{3}, cell.left_child().value_or(PageNumber{}));
  EXPECT_TRUE(std::ranges::equal(wide_forty_two, cell.local_payload()));
  EXPECT_EQ(PageNumber{4}, root.rightmost_child().value_or(PageNumber{}));
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, GrowingInteriorIndexReplacementStagesAndBalances) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  PageNumber root_page;
  constexpr std::size_t kCellCount = 50U;
  constexpr std::int64_t kFirstMarker = 10;
  constexpr std::int64_t kTargetMarker = 26;
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage root = TakeValue(MutableBtreePage::Initialize(
        owner, root_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    root_page = root.page_number();
    for (std::size_t index = 0U; index < kCellCount; ++index) {
      const ByteBuffer record = TakeValue(EncodeRecord(
          std::array{SqlValue::Integer(kFirstMarker + static_cast<std::int64_t>(index))}, options));
      const PageNumber child{static_cast<std::uint32_t>(100U + index)};
      const FormattedCell cell = TakeValue(FillIndexCell(owner, geometry, workspace, record.view(),
                                                         BtreePageType::kInteriorIndex, child));
      ASSERT_EQ(8U, cell.bytes.size());
      RequireStatus(root.InsertCell(index, cell.bytes, child, {}, workspace));
    }
    RequireStatus(root.SetRightmostChild(PageNumber{500}));
    ASSERT_EQ(0U, root.free_bytes());
  }

  const std::array<std::byte, 10> replacement{
      std::byte{0x02}, std::byte{0x06}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x1a},
  };
  std::array<SqlValue, 1> key{SqlValue::Integer(kTargetMarker)};
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.InsertIndex(replacement, key, columns, options, BtreeInsertMode::kReplace,
                                     scratch, workspace));
  }

  EXPECT_GT(pager->page_count(), root_page.value());
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    ASSERT_TRUE(TakeValue(cursor.SeekIndex(key, columns, options, scratch)).exact);
    const BtreeCellView cell =
        TakeValue(TakeValue(cursor.CurrentPage()).cell(cursor.current_index()));
    EXPECT_TRUE(std::ranges::equal(replacement, cell.local_payload()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeInsert, SameSizeIndexReplacementRetainsOverflowPages) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root = TakeValue(AllocateBtreePage(owner, geometry));
    static_cast<void>(TakeValue(
        MutableBtreePage::Initialize(owner, root.owner_slot, geometry, BtreePageType::kLeafIndex)));
    root_page = root.page_number;
  }
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  ByteBuffer blob{ByteCount{700}};
  std::ranges::fill(blob.mutable_view(), std::byte{0x5a});
  std::array<SqlValue, 1> key{SqlValue::Blob(std::move(blob))};
  const ByteBuffer record = TakeValue(EncodeRecord(key, options));
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.InsertIndex(record.view(), key, columns, options,
                                     BtreeInsertMode::kInsertOnly, scratch, workspace));
  }

  PageNumber first_overflow;
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    ASSERT_TRUE(TakeValue(cursor.SeekIndex(key, columns, options, scratch)).exact);
    first_overflow = TakeValue(TakeValue(cursor.CurrentPage()).cell(cursor.current_index()))
                         .first_overflow_page()
                         .value_or(PageNumber{});
    ASSERT_NE(PageNumber{}, first_overflow);
  }
  const std::uint32_t page_count_before = pager->page_count();
  const std::uint32_t free_count_before =
      Load32(TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes(), 36U);
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.InsertIndex(record.view(), key, columns, options,
                                     BtreeInsertMode::kReplace, scratch, workspace));
  }
  EXPECT_EQ(page_count_before, pager->page_count());
  EXPECT_EQ(free_count_before,
            Load32(TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes(), 36U));
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    ASSERT_TRUE(TakeValue(cursor.SeekIndex(key, columns, options, scratch)).exact);
    EXPECT_EQ(first_overflow,
              TakeValue(TakeValue(cursor.CurrentPage()).cell(cursor.current_index()))
                  .first_overflow_page()
                  .value_or(PageNumber{}));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeDelete, DeletesTableRowsAndRejectsMissingRowidsBeforeMutation) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  for (std::int64_t rowid = 1; rowid <= 5; ++rowid) {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const std::array payload{static_cast<std::byte>(static_cast<std::uint8_t>(rowid))};
    RequireStatus(cursor.InsertTable(rowid, payload, BtreeInsertMode::kInsertOnly, workspace));
  }

  const ByteBuffer before = SnapshotPage(*pager, PageNumber{1});
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const auto missing = cursor.DeleteTable(99, workspace);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(ErrorCode::kNotFound, missing.error().code());
  }
  EXPECT_TRUE(std::ranges::equal(before.view(), SnapshotPage(*pager, PageNumber{1}).view()));
  EXPECT_FALSE(pager->write_failure_code().has_value());

  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.DeleteTable(3, workspace));
  }
  for (std::int64_t rowid = 1; rowid <= 5; ++rowid) {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    EXPECT_EQ(rowid != 3, TakeValue(cursor.SeekTable(rowid)).exact);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeDelete, TableDeleteBalancesAndShallowsTheRoot) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  for (std::int64_t rowid = 1; rowid <= 2; ++rowid) {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    const ByteBuffer payload =
        FilledBuffer(380U, static_cast<std::byte>(static_cast<std::uint8_t>(rowid)));
    RequireStatus(
        cursor.InsertTable(rowid, payload.view(), BtreeInsertMode::kInsertOnly, workspace));
  }
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.DeleteTable(2, workspace));
  }

  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const ByteBuffer root_image = SnapshotPage(*pager, PageNumber{1});
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), PageNumber{1}, geometry));
  ASSERT_EQ(BtreePageType::kLeafTable, root.type());
  ASSERT_EQ(1U, root.cell_count());
  EXPECT_EQ(1, TakeValue(root.cell(0U)).rowid());
  EXPECT_GT(Load32(root_image.view(), 36U), 0U);
  RequireStatus(pager->Rollback());
}

TEST(BtreeDelete, TableDeleteReleasesOverflowAndLeavesAnEmptyLeafRoot) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const ByteBuffer payload = FilledBuffer(2'000U, std::byte{0x5a});
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.InsertTable(7, payload.view(), BtreeInsertMode::kInsertOnly, workspace));
  }
  const std::uint32_t free_before =
      Load32(TakeValue(pager->ReadPage(PageNumber{1})).frame().bytes(), 36U);
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, PageNumber{1}, true));
    RequireStatus(cursor.DeleteTable(7, workspace));
  }

  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  const ByteBuffer root_image = SnapshotPage(*pager, PageNumber{1});
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), PageNumber{1}, geometry));
  EXPECT_EQ(BtreePageType::kLeafTable, root.type());
  EXPECT_EQ(0U, root.cell_count());
  EXPECT_GT(Load32(root_image.view(), 36U), free_before);
  RequireStatus(pager->Rollback());
}

TEST(BtreeDelete, DeletesIndexLeafRecordsAndRejectsMissingKeys) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root = TakeValue(AllocateBtreePage(owner, geometry));
    static_cast<void>(TakeValue(
        MutableBtreePage::Initialize(owner, root.owner_slot, geometry, BtreePageType::kLeafIndex)));
    root_page = root.page_number;
  }
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  for (std::int64_t value = 1; value <= 3; ++value) {
    std::array<SqlValue, 1> key{SqlValue::Integer(value)};
    const ByteBuffer record = TakeValue(EncodeRecord(key, options));
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.InsertIndex(record.view(), key, columns, options,
                                     BtreeInsertMode::kInsertOnly, scratch, workspace));
  }

  {
    std::array<SqlValue, 1> key{SqlValue::Integer(9)};
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    const ByteBuffer before = SnapshotPage(*pager, root_page);
    const auto missing = cursor.DeleteIndex(key, columns, options, scratch, workspace);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(ErrorCode::kNotFound, missing.error().code());
    EXPECT_TRUE(std::ranges::equal(before.view(), SnapshotPage(*pager, root_page).view()));
  }
  {
    std::array<SqlValue, 1> key{SqlValue::Integer(2)};
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.DeleteIndex(key, columns, options, scratch, workspace));
  }
  for (std::int64_t value = 1; value <= 3; ++value) {
    std::array<SqlValue, 1> key{SqlValue::Integer(value)};
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    EXPECT_EQ(value != 2, TakeValue(cursor.SeekIndex(key, columns, options, scratch)).exact);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeDelete, InteriorIndexDeleteMovesThePredecessorWithoutASecondSeek) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto make_record = [options](std::int64_t marker) {
    ByteBuffer blob{ByteCount{90}};
    std::ranges::fill(blob.mutable_view(), std::byte{0x5a});
    std::array<SqlValue, 2> values{
        SqlValue::Integer(marker),
        SqlValue::Blob(std::move(blob)),
    };
    return TakeValue(EncodeRecord(values, options));
  };

  PageNumber root_page;
  PageNumber left_page;
  PageNumber right_page;
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage root = TakeValue(MutableBtreePage::Initialize(
        owner, root_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    root_page = root.page_number();
    left_page = left.page_number();
    right_page = right.page_number();
    const std::array<std::int64_t, 4> left_markers{10, 20, 30, 40};
    for (std::size_t index = 0U; index < left_markers.size(); ++index) {
      const ByteBuffer record = make_record(left_markers[index]);
      const FormattedCell cell = TakeValue(FillIndexCell(owner, geometry, workspace, record.view(),
                                                         BtreePageType::kLeafIndex, std::nullopt));
      RequireStatus(left.InsertCell(index, cell.bytes, std::nullopt, {}, workspace));
    }
    const ByteBuffer seventy = make_record(70);
    const FormattedCell right_cell = TakeValue(FillIndexCell(
        owner, geometry, workspace, seventy.view(), BtreePageType::kLeafIndex, std::nullopt));
    RequireStatus(right.InsertCell(0U, right_cell.bytes, std::nullopt, {}, workspace));
    const ByteBuffer fifty = make_record(50);
    const FormattedCell divider = TakeValue(FillIndexCell(
        owner, geometry, workspace, fifty.view(), BtreePageType::kInteriorIndex, left_page));
    RequireStatus(root.InsertCell(0U, divider.bytes, left_page, {}, workspace));
    RequireStatus(root.SetRightmostChild(right_page));
  }

  ByteBuffer target_blob{ByteCount{90}};
  std::ranges::fill(target_blob.mutable_view(), std::byte{0x5a});
  std::array<SqlValue, 2> key{
      SqlValue::Integer(50),
      SqlValue::Blob(std::move(target_blob)),
  };
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.DeleteIndex(key, columns, options, scratch, workspace));
  }

  const ByteBuffer root_image = SnapshotPage(*pager, root_page);
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), root_page, geometry));
  ASSERT_EQ(BtreePageType::kInteriorIndex, root.type());
  ASSERT_EQ(1U, root.cell_count());
  const BtreeCellView divider = TakeValue(root.cell(0U));
  EXPECT_EQ(left_page, divider.left_child().value_or(PageNumber{}));
  EXPECT_TRUE(std::ranges::equal(make_record(40).view(), divider.local_payload()));
  EXPECT_EQ(right_page, root.rightmost_child().value_or(PageNumber{}));

  const ByteBuffer left_image = SnapshotPage(*pager, left_page);
  const BtreePageView left =
      TakeValue(BtreePageView::Parse(left_image.view(), left_page, geometry));
  ASSERT_EQ(3U, left.cell_count());
  const std::array<std::int64_t, 3> remaining{10, 20, 30};
  for (std::size_t index = 0U; index < remaining.size(); ++index) {
    EXPECT_TRUE(std::ranges::equal(make_record(remaining[index]).view(),
                                   TakeValue(left.cell(index)).local_payload()));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeDelete, InteriorIndexDeleteStripsTinyLeafPaddingFromThePredecessor) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage root = TakeValue(MutableBtreePage::Initialize(
        owner, root_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    root_page = root.page_number();
    const ByteBuffer one = TakeValue(EncodeRecord(std::array{SqlValue::Integer(1)}, options));
    const FormattedCell one_cell = TakeValue(FillIndexCell(
        owner, geometry, workspace, one.view(), BtreePageType::kLeafIndex, std::nullopt));
    ASSERT_EQ(4U, one_cell.bytes.size());
    RequireStatus(left.InsertCell(0U, one_cell.bytes, std::nullopt, {}, workspace));
    const ByteBuffer three = TakeValue(EncodeRecord(std::array{SqlValue::Integer(3)}, options));
    const FormattedCell three_cell = TakeValue(FillIndexCell(
        owner, geometry, workspace, three.view(), BtreePageType::kLeafIndex, std::nullopt));
    RequireStatus(right.InsertCell(0U, three_cell.bytes, std::nullopt, {}, workspace));
    const ByteBuffer two = TakeValue(EncodeRecord(std::array{SqlValue::Integer(2)}, options));
    const FormattedCell divider = TakeValue(FillIndexCell(
        owner, geometry, workspace, two.view(), BtreePageType::kInteriorIndex, left.page_number()));
    ASSERT_EQ(8U, divider.bytes.size());
    RequireStatus(root.InsertCell(0U, divider.bytes, left.page_number(), {}, workspace));
    RequireStatus(root.SetRightmostChild(right.page_number()));
  }

  std::array<SqlValue, 1> key{SqlValue::Integer(2)};
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.DeleteIndex(key, columns, options, scratch, workspace));
  }
  for (std::int64_t value = 1; value <= 3; ++value) {
    std::array<SqlValue, 1> search{SqlValue::Integer(value)};
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    EXPECT_EQ(value != 2, TakeValue(cursor.SeekIndex(search, columns, options, scratch)).exact);
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeDelete, InteriorIndexDeleteTransfersPredecessorOverflowOwnership) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 2> columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  const auto make_record = [options](std::int64_t marker, std::size_t blob_size) {
    ByteBuffer blob{ByteCount{blob_size}};
    std::ranges::fill(blob.mutable_view(), std::byte{0x6b});
    std::array<SqlValue, 2> values{
        SqlValue::Integer(marker),
        SqlValue::Blob(std::move(blob)),
    };
    return TakeValue(EncodeRecord(values, options));
  };

  PageNumber root_page;
  PageNumber predecessor_overflow;
  {
    MutationPageOwner owner{*pager};
    const AllocatedBtreePage root_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage left_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    const AllocatedBtreePage right_allocation = TakeValue(AllocateBtreePage(owner, geometry));
    MutableBtreePage root = TakeValue(MutableBtreePage::Initialize(
        owner, root_allocation.owner_slot, geometry, BtreePageType::kInteriorIndex));
    MutableBtreePage left = TakeValue(MutableBtreePage::Initialize(
        owner, left_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    MutableBtreePage right = TakeValue(MutableBtreePage::Initialize(
        owner, right_allocation.owner_slot, geometry, BtreePageType::kLeafIndex));
    root_page = root.page_number();
    const ByteBuffer ten = make_record(10, 4U);
    const FormattedCell ten_cell = TakeValue(FillIndexCell(
        owner, geometry, workspace, ten.view(), BtreePageType::kLeafIndex, std::nullopt));
    RequireStatus(left.InsertCell(0U, ten_cell.bytes, std::nullopt, {}, workspace));
    const ByteBuffer forty = make_record(40, 700U);
    const FormattedCell forty_cell = TakeValue(FillIndexCell(
        owner, geometry, workspace, forty.view(), BtreePageType::kLeafIndex, std::nullopt));
    predecessor_overflow = forty_cell.first_overflow_page.value_or(PageNumber{});
    ASSERT_NE(PageNumber{}, predecessor_overflow);
    RequireStatus(left.InsertCell(1U, forty_cell.bytes, std::nullopt, {}, workspace));
    const ByteBuffer seventy = make_record(70, 4U);
    const FormattedCell seventy_cell = TakeValue(FillIndexCell(
        owner, geometry, workspace, seventy.view(), BtreePageType::kLeafIndex, std::nullopt));
    RequireStatus(right.InsertCell(0U, seventy_cell.bytes, std::nullopt, {}, workspace));
    const ByteBuffer fifty = make_record(50, 4U);
    const FormattedCell divider =
        TakeValue(FillIndexCell(owner, geometry, workspace, fifty.view(),
                                BtreePageType::kInteriorIndex, left.page_number()));
    RequireStatus(root.InsertCell(0U, divider.bytes, left.page_number(), {}, workspace));
    RequireStatus(root.SetRightmostChild(right.page_number()));
  }

  ByteBuffer target_blob{ByteCount{4}};
  std::ranges::fill(target_blob.mutable_view(), std::byte{0x6b});
  std::array<SqlValue, 2> key{
      SqlValue::Integer(50),
      SqlValue::Blob(std::move(target_blob)),
  };
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.DeleteIndex(key, columns, options, scratch, workspace));
  }

  ByteBuffer predecessor_blob{ByteCount{700}};
  std::ranges::fill(predecessor_blob.mutable_view(), std::byte{0x6b});
  std::array<SqlValue, 2> predecessor_key{
      SqlValue::Integer(40),
      SqlValue::Blob(std::move(predecessor_blob)),
  };
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, false));
    ResizablePayloadScratch scratch;
    ASSERT_TRUE(TakeValue(cursor.SeekIndex(predecessor_key, columns, options, scratch)).exact);
    const BtreeCellView predecessor =
        TakeValue(TakeValue(cursor.CurrentPage()).cell(cursor.current_index()));
    EXPECT_EQ(predecessor_overflow, predecessor.first_overflow_page().value_or(PageNumber{}));
  }
  {
    const auto overflow_pin = TakeValue(pager->ReadPage(predecessor_overflow));
    const OverflowPageView overflow =
        TakeValue(OverflowPageView::Parse(overflow_pin.frame().bytes(), geometry));
    EXPECT_TRUE(std::ranges::all_of(overflow.payload(),
                                    [](std::byte value) { return value == std::byte{0x6b}; }));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeLifecycle, CreatesTableAndIndexRoots) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  {
    MutationPageOwner owner{*pager};
    const PageNumber table_root = TakeValue(CreateBtreeRoot(owner, geometry, true));
    const PageNumber index_root = TakeValue(CreateBtreeRoot(owner, geometry, false));
    EXPECT_EQ(PageNumber{2}, table_root);
    EXPECT_EQ(PageNumber{3}, index_root);
    const BtreePageView table = TakeValue(BtreePageView::Parse(
        TakeValue(owner.Frame(TakeValue(owner.Borrow(table_root)))).get().bytes(), table_root,
        geometry));
    const BtreePageView index = TakeValue(BtreePageView::Parse(
        TakeValue(owner.Frame(TakeValue(owner.Borrow(index_root)))).get().bytes(), index_root,
        geometry));
    EXPECT_EQ(BtreePageType::kLeafTable, table.type());
    EXPECT_EQ(BtreePageType::kLeafIndex, index.type());
    EXPECT_EQ(0U, table.cell_count());
    EXPECT_EQ(0U, index.cell_count());
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeLifecycle, ClearsTableAndIndexTreesWhileRetainingTheirRoots) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber table_root;
  PageNumber index_root;
  {
    MutationPageOwner owner{*pager};
    table_root = TakeValue(CreateBtreeRoot(owner, geometry, true));
    index_root = TakeValue(CreateBtreeRoot(owner, geometry, false));
  }
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  for (std::int64_t rowid = 1; rowid <= 6; ++rowid) {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, table_root, true));
    const ByteBuffer payload =
        FilledBuffer(380U, static_cast<std::byte>(static_cast<std::uint8_t>(rowid)));
    RequireStatus(
        cursor.InsertTable(rowid, payload.view(), BtreeInsertMode::kInsertOnly, workspace));
  }
  const RecordCodecOptions options{.schema_format = RecordSchemaFormat::kFour};
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  for (std::uint8_t marker = 1U; marker <= 12U; ++marker) {
    ByteBuffer blob{ByteCount{90}};
    std::ranges::fill(blob.mutable_view(), static_cast<std::byte>(marker));
    std::array<SqlValue, 1> key{SqlValue::Blob(std::move(blob))};
    const ByteBuffer record = TakeValue(EncodeRecord(key, options));
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, index_root, false));
    ResizablePayloadScratch scratch;
    RequireStatus(cursor.InsertIndex(record.view(), key, columns, options,
                                     BtreeInsertMode::kInsertOnly, scratch, workspace));
  }

  {
    MutationPageOwner owner{*pager};
    EXPECT_EQ(6U, TakeValue(ClearBtree(owner, geometry, table_root)));
  }
  {
    MutationPageOwner owner{*pager};
    EXPECT_EQ(12U, TakeValue(ClearBtree(owner, geometry, index_root)));
  }
  const ByteBuffer table_image = SnapshotPage(*pager, table_root);
  const ByteBuffer index_image = SnapshotPage(*pager, index_root);
  const BtreePageView table =
      TakeValue(BtreePageView::Parse(table_image.view(), table_root, geometry));
  const BtreePageView index =
      TakeValue(BtreePageView::Parse(index_image.view(), index_root, geometry));
  EXPECT_EQ(BtreePageType::kLeafTable, table.type());
  EXPECT_EQ(BtreePageType::kLeafIndex, index.type());
  EXPECT_EQ(0U, table.cell_count());
  EXPECT_EQ(0U, index.cell_count());
  EXPECT_GT(Load32(SnapshotPage(*pager, PageNumber{1}).view(), 36U), 0U);
  RequireStatus(pager->Rollback());
}

TEST(BtreeLifecycle, DropsAndReusesANonPageOneRoot) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    root_page = TakeValue(CreateBtreeRoot(owner, geometry, true));
  }
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  for (std::int64_t rowid = 1; rowid <= 2; ++rowid) {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, true));
    const ByteBuffer payload =
        FilledBuffer(380U, static_cast<std::byte>(static_cast<std::uint8_t>(rowid)));
    RequireStatus(
        cursor.InsertTable(rowid, payload.view(), BtreeInsertMode::kInsertOnly, workspace));
  }

  {
    MutationPageOwner owner{*pager};
    RequireStatus(DropBtree(owner, geometry, root_page));
  }
  std::array<PageNumber, 3> reused_roots{};
  {
    MutationPageOwner owner{*pager};
    for (PageNumber& reused : reused_roots) {
      reused = TakeValue(CreateBtreeRoot(owner, geometry, false));
    }
  }
  EXPECT_NE(reused_roots.end(), std::ranges::find(reused_roots, root_page));
  const ByteBuffer root_image = SnapshotPage(*pager, root_page);
  const BtreePageView root =
      TakeValue(BtreePageView::Parse(root_image.view(), root_page, geometry));
  EXPECT_EQ(BtreePageType::kLeafIndex, root.type());
  EXPECT_EQ(0U, root.cell_count());
  RequireStatus(pager->Rollback());
}

TEST(BtreeLifecycle, RejectsAChildCycleBeforeClearingTheRoot) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    root_page = TakeValue(CreateBtreeRoot(owner, geometry, true));
    const std::size_t root_slot = TakeValue(owner.Borrow(root_page));
    MutableBtreePage root = TakeValue(MutableBtreePage::Open(owner, root_slot, geometry));
    RequireStatus(root.Zero(BtreePageType::kInteriorTable));
    RequireStatus(root.SetRightmostChild(root_page));
  }
  const ByteBuffer before = SnapshotPage(*pager, root_page);
  {
    MutationPageOwner owner{*pager};
    const auto cleared = ClearBtree(owner, geometry, root_page);
    ASSERT_FALSE(cleared.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, cleared.error().code());
  }
  EXPECT_TRUE(std::ranges::equal(before.view(), SnapshotPage(*pager, root_page).view()));
  EXPECT_FALSE(pager->write_failure_code().has_value());
  RequireStatus(pager->Rollback());
}

TEST(BtreeLifecycle, RejectsDroppingAnExternallyPinnedRootBeforeFreelistMutation) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    root_page = TakeValue(CreateBtreeRoot(owner, geometry, true));
    const AllocatedBtreePage spare = TakeValue(AllocateBtreePage(owner, geometry));
    RequireStatus(FreeBtreePage(owner, geometry, spare.page_number));
  }
  const ByteBuffer page_one_before = SnapshotPage(*pager, PageNumber{1});
  const ByteBuffer root_before = SnapshotPage(*pager, root_page);
  {
    const auto external = TakeValue(pager->ReadPage(root_page));
    MutationPageOwner owner{*pager};
    const auto dropped = DropBtree(owner, geometry, root_page);
    ASSERT_FALSE(dropped.has_value());
    EXPECT_EQ(ErrorCode::kBusy, dropped.error().code());
    EXPECT_EQ(root_page, external.frame().page_number());
  }
  EXPECT_TRUE(
      std::ranges::equal(page_one_before.view(), SnapshotPage(*pager, PageNumber{1}).view()));
  EXPECT_TRUE(std::ranges::equal(root_before.view(), SnapshotPage(*pager, root_page).view()));
  EXPECT_FALSE(pager->write_failure_code().has_value());
  RequireStatus(pager->Rollback());
}

TEST(BtreeLifecycle, RejectsClearingAnExternallyPinnedOverflowPage) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = OpenInitialized(vfs);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginWrite());
  const BtreePageGeometry geometry = TakeValue(
      BtreePageGeometry::Create(pager->header()->page_size(), pager->header()->usable_size()));
  PageNumber root_page;
  {
    MutationPageOwner owner{*pager};
    root_page = TakeValue(CreateBtreeRoot(owner, geometry, true));
  }
  BtreeWriteWorkspace workspace = TakeValue(BtreeWriteWorkspace::Create(ByteCount{512}));
  const ByteBuffer payload = FilledBuffer(2'000U, std::byte{0x5a});
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, true));
    RequireStatus(cursor.InsertTable(7, payload.view(), BtreeInsertMode::kInsertOnly, workspace));
  }
  PageNumber overflow_page;
  {
    MutationPageOwner owner{*pager};
    WritableCursor cursor = TakeValue(WritableCursor::Open(owner, root_page, true));
    ASSERT_TRUE(TakeValue(cursor.SeekTable(7)).exact);
    overflow_page = TakeValue(TakeValue(cursor.CurrentPage()).cell(cursor.current_index()))
                        .first_overflow_page()
                        .value_or(PageNumber{});
    ASSERT_NE(PageNumber{}, overflow_page);
  }
  const ByteBuffer page_one_before = SnapshotPage(*pager, PageNumber{1});
  const ByteBuffer root_before = SnapshotPage(*pager, root_page);
  {
    const auto external = TakeValue(pager->ReadPage(overflow_page));
    MutationPageOwner owner{*pager};
    const auto cleared = ClearBtree(owner, geometry, root_page);
    ASSERT_FALSE(cleared.has_value());
    EXPECT_EQ(ErrorCode::kBusy, cleared.error().code());
    EXPECT_EQ(overflow_page, external.frame().page_number());
  }
  EXPECT_TRUE(
      std::ranges::equal(page_one_before.view(), SnapshotPage(*pager, PageNumber{1}).view()));
  EXPECT_TRUE(std::ranges::equal(root_before.view(), SnapshotPage(*pager, root_page).view()));
  EXPECT_FALSE(pager->write_failure_code().has_value());
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
    ResizablePayloadScratch scratch;

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
    ResizablePayloadScratch scratch;
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
    ResizablePayloadScratch scratch;
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
