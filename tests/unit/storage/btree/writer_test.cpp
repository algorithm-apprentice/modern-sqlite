#include "modern_sqlite/storage/btree/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite {
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

[[nodiscard]] std::uint32_t Load32(ByteView bytes, std::size_t offset) {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(std::uint32_t)});
}

void Store32(MutableByteView bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{
          bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(std::uint32_t)},
      value);
}

TEST(BtreeWriter, InitializesDatabaseAndCoordinatesTypedWriters) {
  test::WritePagerMemoryVfs<test::kWritePagerFileCapacity> vfs{false};
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());

  BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager));
  const auto duplicate_session = BtreeWriteSession::Open(*pager);
  ASSERT_FALSE(duplicate_session.has_value());
  EXPECT_EQ(ErrorCode::kLocked, duplicate_session.error().code());
  RequireStatus(session.InitializeDatabase(BtreeDatabaseOptions{
      .reserved_bytes = ByteCount{8},
      .schema_format = DatabaseSchemaFormat::kFour,
      .text_encoding = DatabaseTextEncoding::kUtf8,
  }));

  {
    const auto page_one = TakeValue(pager->ReadPage(PageNumber{1}));
    const ByteView bytes = page_one.frame().bytes();
    EXPECT_EQ(std::byte{8}, bytes[20]);
    EXPECT_EQ(1U, Load32(bytes, 28U));
    EXPECT_EQ(4U, Load32(bytes, 44U));
    EXPECT_EQ(1U, Load32(bytes, 56U));
    const BtreePageGeometry geometry =
        TakeValue(BtreePageGeometry::Create(ByteCount{512}, ByteCount{504}));
    const BtreePageView root = TakeValue(BtreePageView::Parse(bytes, PageNumber{1}, geometry));
    EXPECT_EQ(BtreePageType::kLeafTable, root.type());
  }

  TableBtreeWriter table = TakeValue(session.CreateTableBtree());
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  const std::uint32_t pages_before_invalid_metadata = pager->page_count();
  const auto invalid_index = session.CreateIndexBtree(std::span<const IndexColumnOrder>{});
  ASSERT_FALSE(invalid_index.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_index.error().code());
  EXPECT_EQ(pages_before_invalid_metadata, pager->page_count());
  IndexBtreeWriter index = TakeValue(session.CreateIndexBtree(columns));
  const PageNumber table_root = table.root_page();
  const PageNumber index_root = index.root_page();

  const std::array payload_one{std::byte{0x11}, std::byte{0x12}};
  const std::array payload_two{std::byte{0x21}, std::byte{0x22}};
  RequireStatus(table.Insert(2, payload_two));
  RequireStatus(table.Insert(1, payload_one));
  EXPECT_TRUE(TakeValue(table.Delete(2)));
  EXPECT_FALSE(TakeValue(table.Delete(2)));

  std::array<SqlValue, 1> key_one{SqlValue::Integer(1)};
  std::array<SqlValue, 1> key_two{SqlValue::Integer(2)};
  RequireStatus(index.Insert(key_two));
  RequireStatus(index.Insert(key_one));
  const auto duplicate = index.Insert(key_one);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate.error().code());
  EXPECT_TRUE(TakeValue(index.Delete(key_two)));
  EXPECT_FALSE(TakeValue(index.Delete(key_two)));

  EXPECT_EQ(1U, TakeValue(table.Clear()));
  EXPECT_EQ(1U, TakeValue(index.Clear()));
  EXPECT_FALSE(session.requires_rollback());
  EXPECT_FALSE(table.requires_rollback());
  EXPECT_FALSE(index.requires_rollback());

  TableBtreeWriter stale_table = TakeValue(session.OpenTableBtree(table_root));
  IndexBtreeWriter stale_index = TakeValue(session.OpenIndexBtree(index_root, columns));
  RequireStatus(table.Drop());
  RequireStatus(index.Drop());
  const auto stale_table_clear = stale_table.Clear();
  const auto stale_index_clear = stale_index.Clear();
  ASSERT_FALSE(stale_table_clear.has_value());
  ASSERT_FALSE(stale_index_clear.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale_table_clear.error().code());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale_index_clear.error().code());

  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, OpensExistingRootsAndRejectsFreelistRootsOrWrongKinds) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  RequireStatus(test::InitializeEmptyBtreeImage(*pager));
  RequireStatus(pager->Commit());
  RequireStatus(pager->BeginWrite());

  BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager));
  TableBtreeWriter page_one = TakeValue(session.OpenTableBtree(PageNumber{1}));
  const std::array payload{std::byte{0x41}};
  RequireStatus(page_one.Insert(1, payload));

  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  const auto wrong_kind = session.OpenIndexBtree(PageNumber{1}, columns);
  ASSERT_FALSE(wrong_kind.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, wrong_kind.error().code());

  TableBtreeWriter dropped = TakeValue(session.CreateTableBtree());
  const PageNumber dropped_root = dropped.root_page();
  RequireStatus(dropped.Drop());
  const auto freelist_root = session.OpenTableBtree(dropped_root);
  ASSERT_FALSE(freelist_root.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, freelist_root.error().code());

  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, CommitsAnInitializedDatabaseAndReopensItsRoots) {
  test::WritePagerMemoryVfs<test::kWritePagerFileCapacity> vfs{false};
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  PageNumber root_page;
  {
    BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager));
    RequireStatus(session.InitializeDatabase());
    TableBtreeWriter table = TakeValue(session.CreateTableBtree());
    root_page = table.root_page();
    const std::array payload{std::byte{0x31}, std::byte{0x32}};
    RequireStatus(table.Insert(7, payload));
  }
  RequireStatus(pager->Commit());
  ASSERT_NE(nullptr, pager->header());
  EXPECT_EQ(1U, pager->header()->text_encoding());
  EXPECT_EQ(4U, pager->header()->schema_format());

  RequireStatus(pager->BeginWrite());
  {
    BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager));
    TableBtreeWriter table = TakeValue(session.OpenTableBtree(root_page));
    EXPECT_TRUE(TakeValue(table.Delete(7)));
    EXPECT_FALSE(TakeValue(table.Delete(7)));
  }
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, ReportsRollbackRequiredAndRejectsStaleTransactionHandles) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  RequireStatus(test::InitializeEmptyBtreeImage(*pager));
  RequireStatus(pager->Commit());
  RequireStatus(pager->BeginWrite());

  BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager));
  TableBtreeWriter table = TakeValue(session.OpenTableBtree(PageNumber{1}));
  pager->ReportWriteCoordinatorFailure(ErrorCode::kIo);
  EXPECT_TRUE(session.requires_rollback());
  EXPECT_TRUE(table.requires_rollback());
  const std::array payload{std::byte{0x51}};
  const auto blocked = table.Insert(1, payload);
  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(ErrorCode::kIo, blocked.error().code());

  RequireStatus(pager->Rollback());
  RequireStatus(pager->BeginWrite());
  const auto stale = table.Insert(1, payload);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale.error().code());
  RequireStatus(pager->Rollback());
}

TEST(BtreeWriter, RejectsUnsupportedOrMalformedDatabaseFormatsBeforeClaiming) {
  {
    test::WritePagerFixedVfs vfs{false};
    std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    RequireStatus(pager->BeginWrite());
    RequireStatus(test::InitializeEmptyBtreeImage(*pager));
    {
      auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
      Store32(page_one.mutable_bytes(), 52U, 2U);
    }
    RequireStatus(pager->Commit());
    RequireStatus(pager->BeginWrite());
    const auto session = BtreeWriteSession::Open(*pager);
    ASSERT_FALSE(session.has_value());
    EXPECT_EQ(ErrorCode::kProtocol, session.error().code());
    RequireStatus(pager->ClaimWriteCoordinator());
    RequireStatus(pager->Rollback());
  }
  {
    test::WritePagerFixedVfs vfs{false};
    std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    RequireStatus(pager->BeginWrite());
    RequireStatus(test::InitializeEmptyBtreeImage(*pager));
    {
      auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
      Store32(page_one.mutable_bytes(), 56U, 2U);
    }
    RequireStatus(pager->Commit());
    RequireStatus(pager->BeginWrite());
    const auto session = BtreeWriteSession::Open(*pager);
    ASSERT_FALSE(session.has_value());
    EXPECT_EQ(ErrorCode::kProtocol, session.error().code());
    RequireStatus(pager->ClaimWriteCoordinator());
    RequireStatus(pager->Rollback());
  }
  {
    test::WritePagerFixedVfs vfs{false};
    std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
    ASSERT_NE(nullptr, pager);
    RequireStatus(pager->BeginRead());
    RequireStatus(pager->BeginWrite());
    RequireStatus(test::InitializeEmptyBtreeImage(*pager));
    {
      auto page_one = TakeValue(pager->WritePage(PageNumber{1}));
      page_one.mutable_bytes()[100] = static_cast<std::byte>(BtreePageType::kLeafIndex);
    }
    RequireStatus(pager->Commit());
    RequireStatus(pager->BeginWrite());
    const auto session = BtreeWriteSession::Open(*pager);
    ASSERT_FALSE(session.has_value());
    EXPECT_EQ(ErrorCode::kCorruption, session.error().code());
    RequireStatus(pager->ClaimWriteCoordinator());
    RequireStatus(pager->Rollback());
  }
}

}  // namespace
}  // namespace modern_sqlite
