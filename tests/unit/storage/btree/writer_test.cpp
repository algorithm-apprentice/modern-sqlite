#include "modern_sqlite/storage/btree/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
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

enum class CrashOperation : std::uint8_t {
  kInsert,
  kOverflowInsert,
  kAppendSeven,
  kDelete,
  kClear,
};

struct CrashFixture {
  ByteBuffer image;
  PageNumber root_page;
  struct Row {
    std::int64_t rowid;
    std::size_t payload_size;
    std::byte fill;

    bool operator==(const Row&) const = default;
  };
  std::vector<Row> rows;
};

struct RecoveredCrashState {
  ByteBuffer image;
  std::vector<CrashFixture::Row> rows;
};

[[nodiscard]] CrashFixture MakeCrashFixture(std::size_t row_count) {
  test::WritePagerFixedVfs vfs{false};
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("failed to open crash fixture pager");
  }
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());
  PageNumber root_page;
  {
    BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager));
    RequireStatus(session.InitializeDatabase());
    TableBtreeWriter table = TakeValue(session.CreateTableBtree());
    root_page = table.root_page();
    for (std::size_t index = 0U; index < row_count; ++index) {
      const auto rowid = static_cast<std::int64_t>(index) + 1;
      ByteBuffer payload{ByteCount{380}};
      std::ranges::fill(payload.mutable_view(),
                        static_cast<std::byte>(static_cast<std::uint8_t>(rowid)));
      RequireStatus(table.Insert(rowid, payload.view()));
    }
  }
  RequireStatus(pager->Commit());
  RequireStatus(pager->EndRead());
  std::vector<CrashFixture::Row> rows;
  for (std::size_t index = 0U; index < row_count; ++index) {
    const auto rowid = static_cast<std::int64_t>(index) + 1;
    rows.push_back(CrashFixture::Row{
        .rowid = rowid,
        .payload_size = 380U,
        .fill = static_cast<std::byte>(static_cast<std::uint8_t>(rowid)),
    });
  }
  return CrashFixture{
      .image = ByteBuffer::CopyOf(vfs.database_bytes()),
      .root_page = root_page,
      .rows = std::move(rows),
  };
}

[[nodiscard]] bool ApplyCrashOperation(test::WritePagerFixedVfs& vfs, PageNumber root_page,
                                       CrashOperation operation) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr || !pager->BeginRead().has_value() || !pager->BeginWrite().has_value()) {
    return false;
  }
  auto session = BtreeWriteSession::Open(*pager);
  if (!session.has_value()) {
    return false;
  }
  auto table = session->OpenTableBtree(root_page);
  if (!table.has_value()) {
    return false;
  }
  Status mutation;
  switch (operation) {
    case CrashOperation::kInsert: {
      ByteBuffer payload{ByteCount{380}};
      std::ranges::fill(payload.mutable_view(), std::byte{0x02});
      mutation = table->Insert(2, payload.view());
      break;
    }
    case CrashOperation::kOverflowInsert: {
      ByteBuffer payload{ByteCount{2'000}};
      std::ranges::fill(payload.mutable_view(), std::byte{0x02});
      mutation = table->Insert(2, payload.view());
      break;
    }
    case CrashOperation::kAppendSeven: {
      ByteBuffer payload{ByteCount{380}};
      std::ranges::fill(payload.mutable_view(), std::byte{0x07});
      mutation = table->Insert(7, payload.view());
      break;
    }
    case CrashOperation::kDelete: {
      auto deleted = table->Delete(2);
      mutation = deleted.has_value() && *deleted
                     ? Status{}
                     : Status{std::unexpected(
                           deleted.has_value()
                               ? Error::Create(ErrorCode::kNotFound, "crash delete row is absent")
                               : std::move(deleted.error()))};
      break;
    }
    case CrashOperation::kClear: {
      auto cleared = table->Clear();
      mutation =
          cleared.has_value() ? Status{} : Status{std::unexpected(std::move(cleared.error()))};
      break;
    }
  }
  if (!mutation.has_value() || !pager->Commit().has_value()) {
    return false;
  }
  return pager->EndRead().has_value();
}

[[nodiscard]] std::vector<CrashFixture::Row> ReadCrashRows(test::WritePagerFixedVfs& vfs,
                                                           PageNumber root_page) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("failed to reopen crashed pager");
  }
  RequireStatus(pager->BeginRead());
  std::vector<CrashFixture::Row> rows;
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, root_page));
    if (TakeValue(cursor.First())) {
      do {
        const ByteBuffer payload = TakeValue(cursor.CopyPayload());
        const std::byte fill = payload.empty() ? std::byte{0} : payload.view()[0];
        if (!std::ranges::all_of(payload.view(),
                                 [fill](std::byte value) { return value == fill; })) {
          throw std::runtime_error("crash recovery produced a corrupt payload");
        }
        rows.push_back(CrashFixture::Row{
            .rowid = TakeValue(cursor.rowid()),
            .payload_size = payload.size().value(),
            .fill = fill,
        });
      } while (TakeValue(cursor.Next()));
    }
  }
  RequireStatus(pager->EndRead());
  return rows;
}

[[nodiscard]] RecoveredCrashState RecoverCrashState(test::WritePagerFixedVfs& vfs,
                                                    PageNumber root_page) {
  vfs.Crash();
  const std::vector<CrashFixture::Row> first = ReadCrashRows(vfs, root_page);
  vfs.Crash();
  const std::vector<CrashFixture::Row> second = ReadCrashRows(vfs, root_page);
  if (first != second) {
    throw std::runtime_error("hot recovery was not durable across a second crash");
  }
  return RecoveredCrashState{
      .image = ByteBuffer::CopyOf(vfs.database_bytes()),
      .rows = second,
  };
}

void VerifyCrashCuts(const CrashFixture& fixture, CrashOperation operation,
                     const std::vector<CrashFixture::Row>& committed_rows) {
  for (const bool writes_are_durable : {false, true}) {
    SCOPED_TRACE(writes_are_durable ? "durable-writes" : "sync-only");
    test::WritePagerFixedVfs baseline{false};
    baseline.LoadDatabase(fixture.image.view());
    baseline.SetDatabaseWritesDurable(writes_are_durable);
    baseline.ArmCrashCut(std::nullopt);
    ASSERT_TRUE(ApplyCrashOperation(baseline, fixture.root_page, operation));
    const std::size_t mutation_count = baseline.mutation_count();
    const ByteBuffer committed_image = ByteBuffer::CopyOf(baseline.database_bytes());
    ASSERT_GT(mutation_count, 0U);

    for (std::size_t cut = 1U; cut <= mutation_count; ++cut) {
      SCOPED_TRACE(cut);
      test::WritePagerFixedVfs vfs{false};
      vfs.LoadDatabase(fixture.image.view());
      vfs.SetDatabaseWritesDurable(writes_are_durable);
      vfs.ArmCrashCut(cut);
      static_cast<void>(ApplyCrashOperation(vfs, fixture.root_page, operation));
      const RecoveredCrashState recovered = RecoverCrashState(vfs, fixture.root_page);
      const bool old_state = recovered.rows == fixture.rows &&
                             std::ranges::equal(recovered.image.view(), fixture.image.view());
      const bool committed_state =
          recovered.rows == committed_rows &&
          std::ranges::equal(recovered.image.view(), committed_image.view());
      EXPECT_TRUE(old_state || committed_state);
    }
  }
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

TEST(BtreeWriter, OnePassCursorDeletesCurrentRowsWithoutSkippingSuccessors) {
  test::WritePagerMemoryVfs<test::kWritePagerFileCapacity> vfs{false};
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  RequireStatus(pager->BeginWrite());

  BtreeWriteSession session = TakeValue(BtreeWriteSession::Open(*pager));
  RequireStatus(session.InitializeDatabase());
  TableBtreeWriter table = TakeValue(session.CreateTableBtree());
  for (std::int64_t rowid = 1; rowid <= 128; ++rowid) {
    ByteBuffer payload{ByteCount{380}};
    std::ranges::fill(payload.mutable_view(),
                      static_cast<std::byte>(static_cast<std::uint8_t>(rowid)));
    RequireStatus(table.Insert(rowid, payload.view()));
  }

  std::uint64_t deleted = 0;
  {
    TableBtreeMutationCursor opened = TakeValue(table.OpenMutationCursor());
    TableBtreeMutationCursor cursor = std::move(opened);
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    const auto moved_from = opened.First();
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    ASSERT_FALSE(moved_from.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, moved_from.error().code());
    ASSERT_TRUE(TakeValue(cursor.First()));
    while (cursor.valid()) {
      const TableBtreeMutationRow row = TakeValue(cursor.row());
      const auto expected_rowid = static_cast<std::int64_t>(deleted) + 1;
      EXPECT_EQ(expected_rowid, row.rowid);
      ASSERT_EQ(380U, row.payload.size());
      EXPECT_EQ(static_cast<std::byte>(static_cast<std::uint8_t>(expected_rowid)), row.payload[0]);
      ++deleted;
      const bool has_next = TakeValue(cursor.DeleteAndNext());
      EXPECT_EQ(deleted < 128U, has_next);
    }
  }
  EXPECT_EQ(128U, deleted);
  {
    TableBtreeMutationCursor empty = TakeValue(table.OpenMutationCursor());
    EXPECT_FALSE(TakeValue(empty.First()));
    const auto row = empty.row();
    const auto deleted_empty = empty.DeleteAndNext();
    ASSERT_FALSE(row.has_value());
    ASSERT_FALSE(deleted_empty.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, row.error().code());
    EXPECT_EQ(ErrorCode::kMisuse, deleted_empty.error().code());
  }
  EXPECT_EQ(0U, TakeValue(table.Clear()));
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

TEST(BtreeWriterCrash, EveryStructuralInsertCutRecoversOldOrCommittedRows) {
  const CrashFixture fixture = MakeCrashFixture(1U);
  auto committed = fixture.rows;
  committed.push_back(CrashFixture::Row{
      .rowid = 2,
      .payload_size = 380U,
      .fill = std::byte{0x02},
  });
  VerifyCrashCuts(fixture, CrashOperation::kInsert, committed);
}

TEST(BtreeWriterCrash, EveryOverflowInsertCutRecoversOldOrCommittedRows) {
  const CrashFixture fixture = MakeCrashFixture(1U);
  auto committed = fixture.rows;
  committed.push_back(CrashFixture::Row{
      .rowid = 2,
      .payload_size = 2'000U,
      .fill = std::byte{0x02},
  });
  VerifyCrashCuts(fixture, CrashOperation::kOverflowInsert, committed);
}

TEST(BtreeWriterCrash, EveryQuickBalanceCutRecoversOldOrCommittedRows) {
  const CrashFixture fixture = MakeCrashFixture(6U);
  auto committed = fixture.rows;
  committed.push_back(CrashFixture::Row{
      .rowid = 7,
      .payload_size = 380U,
      .fill = std::byte{0x07},
  });
  VerifyCrashCuts(fixture, CrashOperation::kAppendSeven, committed);
}

TEST(BtreeWriterCrash, EveryDeleteCutRecoversOldOrCommittedRows) {
  const CrashFixture fixture = MakeCrashFixture(2U);
  VerifyCrashCuts(fixture, CrashOperation::kDelete, {fixture.rows.front()});
}

TEST(BtreeWriterCrash, EveryClearCutRecoversOldOrCommittedRows) {
  const CrashFixture fixture = MakeCrashFixture(3U);
  VerifyCrashCuts(fixture, CrashOperation::kClear, {});
}

TEST(BtreeWriterModel, MixedTableAndIndexOperationsMatchReferenceContainers) {
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
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  IndexBtreeWriter index = TakeValue(session.CreateIndexBtree(columns));
  std::map<std::int64_t, std::byte> table_model;
  std::set<std::int64_t> index_model;

  for (std::size_t step = 0U; step < 300U; ++step) {
    const std::int64_t key = static_cast<std::int64_t>((step * 37U) % 97U) + 1;
    const auto value = static_cast<std::byte>(step & 0xffU);
    const std::array payload{value, value, value, value, value, value, value, value};
    switch (step % 4U) {
      case 0U:
        RequireStatus(table.Insert(key, payload, BtreeInsertMode::kReplace));
        table_model.insert_or_assign(key, value);
        break;
      case 1U: {
        const auto inserted = table.Insert(key, payload, BtreeInsertMode::kInsertOnly);
        const bool exists = table_model.contains(key);
        EXPECT_EQ(!exists, inserted.has_value());
        if (exists) {
          EXPECT_EQ(ErrorCode::kConstraint, inserted.error().code());
        } else {
          table_model.emplace(key, value);
        }
        break;
      }
      case 2U:
        EXPECT_EQ(table_model.erase(key) != 0U, TakeValue(table.Delete(key)));
        break;
      case 3U:
        RequireStatus(table.Insert(key, payload, BtreeInsertMode::kReplace));
        table_model.insert_or_assign(key, value);
        break;
      default:
        throw std::runtime_error("mixed table model selected an invalid operation");
    }

    std::array<SqlValue, 1> index_key{SqlValue::Integer(key)};
    if (step % 3U == 0U) {
      const auto inserted = index.Insert(index_key);
      const bool exists = index_model.contains(key);
      EXPECT_EQ(!exists, inserted.has_value());
      if (exists) {
        EXPECT_EQ(ErrorCode::kConstraint, inserted.error().code());
      } else {
        index_model.insert(key);
      }
    } else if (step % 3U == 1U) {
      EXPECT_EQ(index_model.erase(key) != 0U, TakeValue(index.Delete(index_key)));
    }
  }

  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, table.root_page()));
    auto expected = table_model.begin();
    bool has_row = TakeValue(cursor.First());
    while (has_row) {
      ASSERT_NE(table_model.end(), expected);
      EXPECT_EQ(expected->first, TakeValue(cursor.rowid()));
      const ByteBuffer payload = TakeValue(cursor.CopyPayload());
      ASSERT_EQ(ByteCount{8}, payload.size());
      EXPECT_TRUE(std::ranges::all_of(
          payload.view(), [value = expected->second](std::byte byte) { return byte == value; }));
      ++expected;
      has_row = TakeValue(cursor.Next());
    }
    EXPECT_EQ(table_model.end(), expected);
  }
  {
    IndexBtreeCursor cursor = TakeValue(IndexBtreeCursor::Open(*pager, index.root_page(), columns));
    auto expected = index_model.begin();
    bool has_row = TakeValue(cursor.First());
    while (has_row) {
      ASSERT_NE(index_model.end(), expected);
      const ByteBuffer payload = TakeValue(cursor.CopyPayload());
      const RecordView record = TakeValue(RecordView::Parse(payload.view()));
      const RecordFieldView field = TakeValue(record.field(0U));
      EXPECT_EQ(*expected, field.integer_value().value_or(0));
      ++expected;
      has_row = TakeValue(cursor.Next());
    }
    EXPECT_EQ(index_model.end(), expected);
  }

  EXPECT_EQ(73U, table_model.size());
  EXPECT_EQ(32U, index_model.size());
  EXPECT_EQ(table_model.size(), TakeValue(table.Clear()));
  EXPECT_EQ(index_model.size(), TakeValue(index.Clear()));
  RequireStatus(pager->Rollback());
}

}  // namespace
}  // namespace modern_sqlite
