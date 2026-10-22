#include "modern_sqlite/storage/btree/database.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "storage/btree/cursor_state.hpp"
#include "storage/btree/writer_internal.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"
#include "tests/unit/storage/btree/database_test_support.hpp"

namespace modern_sqlite {
namespace {

constexpr int kSqliteAbortRollback = 516;

template <typename Value>
[[nodiscard]] Value TakeValue(Result<Value> result) {
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

template <typename Database>
concept AllowsEmptyConstructionKey = requires { Database({}); };

static_assert(!AllowsEmptyConstructionKey<BtreeDatabase>);
static_assert(!std::is_default_constructible_v<BtreeDatabase>);
static_assert(!std::is_copy_constructible_v<BtreeDatabase>);
static_assert(!std::is_copy_assignable_v<BtreeDatabase>);
static_assert(!std::is_move_constructible_v<BtreeDatabase>);
static_assert(!std::is_move_assignable_v<BtreeDatabase>);

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path().parent_path() /
         "fixtures" / "btree_read" / "sqlite-3.54.0-btree-read.db";
}

TEST(BtreeDatabase, KeepsDatabaseAndPagerAddressesStableWhenTheOwnerMoves) {
  PosixVfs vfs;
  auto opened = BtreeDatabase::Open(vfs, FixturePath().string());
  ASSERT_TRUE(opened.has_value());
  ASSERT_NE(nullptr, *opened);
  BtreeDatabase* const database_address = opened->get();
  Pager* const pager_address = &(*opened)->pager();
  EXPECT_FALSE(pager_address->writable());
  ASSERT_TRUE(pager_address->BeginRead().has_value());
  EXPECT_GT(pager_address->page_count(), 0U);

  std::unique_ptr<BtreeDatabase> moved = std::move(*opened);
  EXPECT_EQ(nullptr, *opened);
  EXPECT_EQ(database_address, moved.get());
  EXPECT_EQ(pager_address, &moved->pager());
  ASSERT_TRUE(moved->pager().EndRead().has_value());
}

TEST(BtreeDatabase, OpensTheWritablePagerWithTheSameCapabilities) {
  test::WritePagerFixedVfs vfs;
  auto opened = BtreeDatabase::OpenWritable(vfs, test::kWritePagerDatabasePath);
  ASSERT_TRUE(opened.has_value());
  Pager& pager = (*opened)->pager();
  EXPECT_TRUE(pager.writable());
  ASSERT_TRUE(pager.BeginRead().has_value());
  ASSERT_TRUE(pager.BeginWrite().has_value());
  ASSERT_TRUE(pager.Rollback().has_value());
}

TEST(BtreeDatabase, OwnsBothEphemeralPagerModesAndClosesTheirResources) {
  for (const bool memory_only : {false, true}) {
    SCOPED_TRACE(memory_only);
    test::WritePagerFixedVfs vfs{false};
    {
      auto opened =
          memory_only ? BtreeDatabase::OpenEphemeralMemory(vfs) : BtreeDatabase::OpenEphemeral(vfs);
      ASSERT_TRUE(opened.has_value());
      Pager& pager = (*opened)->pager();
      EXPECT_TRUE(pager.writable());
      ASSERT_TRUE(pager.BeginRead().has_value());
      ASSERT_TRUE(pager.BeginWrite().has_value());
      EXPECT_EQ(memory_only ? 0U : 1U, vfs.pathless_open_count());
      const auto rolled_back = pager.Rollback();
      ASSERT_FALSE(rolled_back.has_value());
      EXPECT_EQ(ErrorCode::kMisuse, rolled_back.error().code());
    }
    EXPECT_FALSE(vfs.pathless_file_present());
  }
}

TEST(BtreeDatabase, ReturnsTheUnderlyingOpenErrorWithoutLeavingAFile) {
  test::WritePagerFixedVfs vfs{false};
  vfs.FailNextPathlessOpen(ErrorCode::kIo);
  const auto rejected = BtreeDatabase::OpenEphemeral(vfs);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kIo, rejected.error().code());
  EXPECT_FALSE(vfs.pathless_file_present());
  const auto retried = BtreeDatabase::OpenEphemeral(vfs);
  ASSERT_TRUE(retried.has_value());
}

class BtreeCursorOwnership : public ::testing::Test {
 protected:
  void SetUp() override {
    database_ = test::OpenWriteDatabase<BtreeDatabase>(vfs_, 64U);
    ASSERT_NE(nullptr, database_);
    RequireStatus(database_->pager().BeginRead());
    RequireStatus(database_->pager().BeginWrite());
    session_.emplace(TakeValue(BtreeWriteSession::Open(*database_)));
    RequireStatus(session_->InitializeDatabase());
  }

  void TearDown() override {
    session_.reset();
    if (database_ != nullptr && database_->pager().in_write_transaction()) {
      EXPECT_TRUE(database_->pager().Rollback().has_value());
    }
  }

  [[nodiscard]] PageNumber CreatePopulatedTable() {
    TableBtreeWriter table = TakeValue(session_->CreateTableBtree());
    constexpr std::array payload{std::byte{0x41}};
    RequireStatus(table.Insert(1, payload));
    return table.root_page();
  }

  [[nodiscard]] PageNumber CreateTableWithMalformedCell(std::size_t cell_index) {
    const PageNumber root = CreatePopulatedTable();
    {
      TableBtreeWriter table = TakeValue(session_->OpenTableBtree(root));
      constexpr std::array payload{std::byte{0x41}};
      RequireStatus(table.Insert(3, payload));
      RequireStatus(table.Insert(5, payload));
    }
    session_.reset();
    RequireStatus(database_->pager().Commit());
    ByteBuffer image = ByteBuffer::CopyOf(vfs_.database_bytes());
    const std::size_t root_offset = (root.value() - 1U) * test::kWritePagerPageSize;
    const MutableByteView page =
        image.mutable_view().subspan(root_offset, test::kWritePagerPageSize);
    StoreBigEndian<std::uint16_t>(
        std::span<std::byte, sizeof(std::uint16_t)>{
            page.data() + 8U + cell_index * sizeof(std::uint16_t), sizeof(std::uint16_t)},
        static_cast<std::uint16_t>(page.size() - 1U));
    page.back() = std::byte{0x80};
    RequireStatus(database_->pager().EndRead());
    database_.reset();
    vfs_.LoadDatabase(image.view());
    database_ = test::OpenWriteDatabase<BtreeDatabase>(vfs_, 64U);
    if (database_ == nullptr) {
      throw std::runtime_error("failed to open malformed cursor fixture");
    }
    RequireStatus(database_->pager().BeginRead());
    RequireStatus(database_->pager().BeginWrite());
    session_.emplace(TakeValue(BtreeWriteSession::Open(*database_)));
    return root;
  }

  test::WritePagerFixedVfs vfs_{false};
  std::unique_ptr<BtreeDatabase> database_;
  std::optional<BtreeWriteSession> session_;
};

TEST_F(BtreeCursorOwnership, TracksNativeRootOrderFlagsAndStablePublicMoves) {
  using btree_internal::BtreeCursorPhase;
  using btree_internal::CursorAccess;
  const PageNumber first_root = CreatePopulatedTable();
  const PageNumber other_root = CreatePopulatedTable();
  EXPECT_TRUE(BtreeDatabaseTestPeer::Cursors(*database_).empty());
  std::optional<TableBtreeCursor> first{TakeValue(TableBtreeCursor::Open(*database_, first_root))};
  auto authority = TakeValue(session_->OpenTableBtree(first_root));
  std::optional<TableBtreeMutationCursor> middle{TakeValue(authority.OpenMutationCursor())};
  std::optional<TableBtreeCursor> head{TakeValue(TableBtreeCursor::Open(*database_, other_root))};
  ASSERT_TRUE(TakeValue(first->First()));
  ASSERT_TRUE(TakeValue(middle->First()));
  auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(3U, cursors.size());
  EXPECT_EQ(other_root, cursors[0].root);
  EXPECT_EQ(CursorAccess::Read, cursors[0].access);
  EXPECT_EQ(BtreeCursorPhase::Invalid, cursors[0].phase);
  EXPECT_FALSE(cursors[0].multiple);
  EXPECT_EQ(first_root, cursors[1].root);
  EXPECT_EQ(CursorAccess::Write, cursors[1].access);
  EXPECT_EQ(BtreeCursorPhase::Valid, cursors[1].phase);
  EXPECT_TRUE(cursors[1].multiple);
  EXPECT_EQ(first_root, cursors[2].root);
  EXPECT_EQ(CursorAccess::Read, cursors[2].access);
  EXPECT_EQ(BtreeCursorPhase::Valid, cursors[2].phase);
  EXPECT_TRUE(cursors[2].multiple);
  const auto* const first_identity = cursors[2].identity;
  const std::size_t pins = database_->pager().cache_pin_count();

  std::optional<TableBtreeCursor> moved{std::move(*first)};
  EXPECT_FALSE(first->valid());
  first.reset();
  EXPECT_EQ(first_identity, BtreeDatabaseTestPeer::Cursors(*database_).back().identity);
  EXPECT_EQ(pins, database_->pager().cache_pin_count());

  middle.reset();
  cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(2U, cursors.size());
  EXPECT_EQ(first_identity, cursors.back().identity);
  EXPECT_TRUE(cursors.back().multiple);

  std::optional<TableBtreeCursor> new_head{
      TakeValue(TableBtreeCursor::Open(*database_, other_root))};
  cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(3U, cursors.size());
  EXPECT_TRUE(cursors[0].multiple);
  EXPECT_TRUE(cursors[1].multiple);
  const auto* const transferred_identity = cursors[0].identity;
  moved.reset();
  ASSERT_EQ(2U, BtreeDatabaseTestPeer::Cursors(*database_).size());

  *head = std::move(*new_head);
  EXPECT_FALSE(new_head->valid());
  new_head.reset();
  cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(1U, cursors.size());
  EXPECT_EQ(transferred_identity, cursors[0].identity);
  EXPECT_TRUE(cursors[0].multiple);
  head.reset();
  EXPECT_TRUE(BtreeDatabaseTestPeer::Cursors(*database_).empty());
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
}

TEST_F(BtreeCursorOwnership, SavesAllRootsForTheNativeGlobalSentinel) {
  const PageNumber first_root = CreatePopulatedTable();
  const PageNumber other_root = CreatePopulatedTable();
  TableBtreeCursor first = TakeValue(TableBtreeCursor::Open(*database_, first_root));
  TableBtreeCursor other = TakeValue(TableBtreeCursor::Open(*database_, other_root));
  auto authority = TakeValue(session_->OpenTableBtree(first_root));
  TableBtreeMutationCursor writer = TakeValue(authority.OpenMutationCursor());
  ASSERT_TRUE(TakeValue(first.First()));
  ASSERT_TRUE(TakeValue(other.First()));
  ASSERT_TRUE(TakeValue(writer.First()));
  ASSERT_GT(database_->pager().cache_pin_count(), 0U);

  RequireStatus(BtreeDatabaseTestPeer::SaveCursors(*database_, PageNumber{}));

  for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(*database_)) {
    EXPECT_EQ(btree_internal::BtreeCursorPhase::RequireSeek, cursor.phase);
  }
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
  ASSERT_TRUE(TakeValue(first.RestorePosition()));
  ASSERT_TRUE(TakeValue(other.RestorePosition()));
  EXPECT_EQ(1, TakeValue(first.rowid()));
  EXPECT_EQ(1, TakeValue(other.rowid()));
}

TEST_F(BtreeCursorOwnership, GlobalSaveReleasesInvalidPathsWithoutCreatingPositions) {
  const PageNumber root = CreatePopulatedTable();
  auto cursor = TakeValue(TableBtreeCursor::Open(*database_, root));
  ASSERT_TRUE(TakeValue(cursor.First()));
  ASSERT_FALSE(TakeValue(cursor.Next()));
  ASSERT_GT(database_->pager().cache_pin_count(), 0U);

  RequireStatus(BtreeDatabaseTestPeer::SaveCursors(*database_, PageNumber{}));

  const auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(1U, cursors.size());
  EXPECT_EQ(btree_internal::BtreeCursorPhase::Invalid, cursors.front().phase);
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
  EXPECT_FALSE(TakeValue(cursor.RestorePosition()));
}

TEST_F(BtreeCursorOwnership, SavepointSaveFailureKeepsEarlierSavesWithoutPlayingBackTheJournal) {
  const PageNumber malformed_root = CreateTableWithMalformedCell(0);
  const PageNumber other_root = CreatePopulatedTable();
  const auto savepoint = TakeValue(database_->pager().CreateSavepoint());
  auto tail = TakeValue(TableBtreeCursor::Open(*database_, other_root));
  auto middle = TakeValue(TableBtreeCursor::Open(*database_, malformed_root));
  auto head = TakeValue(TableBtreeCursor::Open(*database_, other_root));
  ASSERT_TRUE(TakeValue(tail.First()));
  ASSERT_TRUE(TakeValue(middle.First()));
  ASSERT_TRUE(TakeValue(head.First()));
  const auto pins = database_->pager().cache_pin_count();
  const auto generation = database_->pager().write_transaction_generation();
  const auto state = database_->pager().state();
  const auto writes = vfs_.total_journal_writes();

  const auto rejected = database_->RollbackToSavepoint(savepoint);

  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, rejected.error().code());
  const auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(3U, cursors.size());
  EXPECT_EQ(btree_internal::BtreeCursorPhase::RequireSeek, cursors[0].phase);
  EXPECT_EQ(btree_internal::BtreeCursorPhase::Valid, cursors[1].phase);
  EXPECT_EQ(btree_internal::BtreeCursorPhase::Valid, cursors[2].phase);
  EXPECT_EQ(pins - 1U, database_->pager().cache_pin_count());
  EXPECT_EQ(generation, database_->pager().write_transaction_generation());
  EXPECT_EQ(state, database_->pager().state());
  EXPECT_EQ(writes, vfs_.total_journal_writes());
  EXPECT_EQ(1, TakeValue(tail.rowid()));
}

TEST_F(BtreeCursorOwnership, SavepointFencePreservesRawPinAdmissionAndAnAlreadySavedKey) {
  const PageNumber root = CreatePopulatedTable();
  const auto savepoint = TakeValue(database_->pager().CreateSavepoint());
  auto table = TakeValue(session_->OpenTableBtree(root));
  constexpr std::array payload{std::byte{0x42}};
  RequireStatus(table.Insert(3, payload));
  auto cursor = TakeValue(TableBtreeCursor::Open(*database_, root));
  ASSERT_TRUE(TakeValue(cursor.Last()));
  const auto generation = database_->pager().write_transaction_generation();
  {
    const auto raw_pin = TakeValue(database_->pager().ReadPage(PageNumber{1}));
    const auto writes = vfs_.total_journal_writes();

    const auto rejected = database_->RollbackToSavepoint(savepoint);

    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kBusy, rejected.error().code());
    EXPECT_TRUE(cursor.HasMoved());
    EXPECT_EQ(1U, database_->pager().cache_pin_count());
    EXPECT_EQ(generation, database_->pager().write_transaction_generation());
    EXPECT_EQ(writes, vfs_.total_journal_writes());
    EXPECT_EQ(PageNumber{1}, raw_pin.frame().page_number());
  }

  RequireStatus(database_->RollbackToSavepoint(savepoint));

  EXPECT_EQ(generation + 1U, database_->pager().write_transaction_generation());
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
  ASSERT_FALSE(TakeValue(cursor.RestorePosition()));
  ASSERT_TRUE(TakeValue(cursor.Previous()));
  EXPECT_EQ(1, TakeValue(cursor.rowid()));
}

TEST_F(BtreeCursorOwnership, RejectsSavepointRollbackOutsideAWriteWithoutMovingReaders) {
  const PageNumber root = CreatePopulatedTable();
  const auto savepoint = TakeValue(database_->pager().CreateSavepoint());
  session_.reset();
  RequireStatus(database_->pager().Commit());
  auto cursor = TakeValue(TableBtreeCursor::Open(*database_, root));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const auto pins = database_->pager().cache_pin_count();

  const auto rejected = database_->RollbackToSavepoint(savepoint);

  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  EXPECT_FALSE(cursor.HasMoved());
  EXPECT_EQ(pins, database_->pager().cache_pin_count());
  EXPECT_EQ(1, TakeValue(cursor.rowid()));
}

TEST_F(BtreeCursorOwnership, TripsTableAndAppendWritersWhileSavingReadersAcrossRoots) {
  const PageNumber first_root = CreatePopulatedTable();
  const PageNumber other_root = CreatePopulatedTable();
  const std::array columns{IndexColumnOrder{}, IndexColumnOrder{}};
  const auto info = TakeValue(IndexComparisonInfo::CopyOf(columns));
  auto index = TakeValue(session_->CreateIndexBtree(info));
  auto append = TakeValue(index.OpenAppendCursor());
  const auto first_record =
      TakeValue(EncodeRecord(std::array{SqlValue::Integer(1), SqlValue::Integer(1)}));
  const auto next_record =
      TakeValue(EncodeRecord(std::array{SqlValue::Integer(2), SqlValue::Integer(2)}));
  RequireStatus(append.AppendSorted(TakeValue(RecordView::Parse(first_record.view()))));
  auto authority = TakeValue(session_->OpenTableBtree(first_root));
  auto writer = TakeValue(authority.OpenMutationCursor());
  auto first = TakeValue(TableBtreeCursor::Open(*database_, first_root));
  auto other = TakeValue(TableBtreeCursor::Open(*database_, other_root));
  auto index_reader = TakeValue(IndexBtreeCursor::Open(*database_, index.root_page(), info));
  ASSERT_TRUE(TakeValue(writer.First()));
  ASSERT_TRUE(TakeValue(first.First()));
  ASSERT_TRUE(TakeValue(other.First()));
  ASSERT_TRUE(TakeValue(index_reader.First()));

  RequireStatus(database_->TripCursors(BtreeCursorTripMode::WriteOnly));

  EXPECT_EQ(0U, database_->pager().cache_pin_count());
  for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(*database_)) {
    EXPECT_EQ(cursor.access == btree_internal::CursorAccess::Read
                  ? btree_internal::BtreeCursorPhase::RequireSeek
                  : btree_internal::BtreeCursorPhase::Fault,
              cursor.phase);
  }
  const auto writer_fault = writer.Next();
  ASSERT_FALSE(writer_fault.has_value());
  EXPECT_EQ(kSqliteAbortRollback, writer_fault.error().sqlite_code());
  const auto append_fault = append.AppendSorted(TakeValue(RecordView::Parse(next_record.view())));
  ASSERT_FALSE(append_fault.has_value());
  EXPECT_EQ(kSqliteAbortRollback, append_fault.error().sqlite_code());
  ASSERT_TRUE(TakeValue(first.RestorePosition()));
  ASSERT_TRUE(TakeValue(other.RestorePosition()));
  ASSERT_TRUE(TakeValue(index_reader.RestorePosition()));
  EXPECT_EQ(1, TakeValue(first.rowid()));
  EXPECT_EQ(1, TakeValue(other.rowid()));
}

TEST_F(BtreeCursorOwnership,
       CapturesHeldLocalKeysDuringFailedWriteCleanupWithoutRevivingAuthority) {
  const PageNumber root = CreatePopulatedTable();
  const std::array columns{IndexColumnOrder{}, IndexColumnOrder{}};
  const auto info = TakeValue(IndexComparisonInfo::CopyOf(columns));
  auto index = TakeValue(session_->CreateIndexBtree(info));
  auto append = TakeValue(index.OpenAppendCursor());
  const auto record =
      TakeValue(EncodeRecord(std::array{SqlValue::Integer(1), SqlValue::Integer(1)}));
  RequireStatus(append.AppendSorted(TakeValue(RecordView::Parse(record.view()))));
  auto table = TakeValue(session_->OpenTableBtree(root));
  auto writer = TakeValue(table.OpenMutationCursor());
  auto reader = TakeValue(TableBtreeCursor::Open(*database_, root));
  auto index_reader = TakeValue(IndexBtreeCursor::Open(*database_, index.root_page(), info));
  ASSERT_TRUE(TakeValue(writer.First()));
  ASSERT_TRUE(TakeValue(reader.First()));
  ASSERT_TRUE(TakeValue(index_reader.First()));
  database_->pager().ReportWriteCoordinatorFailure(ErrorCode::kIo);
  const auto sealed = writer.Next();
  ASSERT_FALSE(sealed.has_value());
  EXPECT_EQ(ErrorCode::kIo, sealed.error().code());
  const auto reads = vfs_.total_database_reads();

  RequireStatus(database_->PrepareRollbackCursors(std::nullopt));

  EXPECT_EQ(0U, database_->pager().cache_pin_count());
  EXPECT_EQ(reads, vfs_.total_database_reads());
  for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(*database_)) {
    EXPECT_EQ(btree_internal::BtreeCursorPhase::RequireSeek, cursor.phase);
    EXPECT_FALSE(BtreeDatabaseTestPeer::TripCode(*cursor.identity).has_value());
  }
  const auto still_sealed = writer.RestorePosition();
  ASSERT_FALSE(still_sealed.has_value());
  EXPECT_EQ(ErrorCode::kIo, still_sealed.error().code());
}

TEST_F(BtreeCursorOwnership, TripCauseSurvivesClearCountExplicitPositioningAndRepeatedAccess) {
  const PageNumber root = CreatePopulatedTable();
  const std::array columns{IndexColumnOrder{}, IndexColumnOrder{}};
  const auto info = TakeValue(IndexComparisonInfo::CopyOf(columns));
  auto index = TakeValue(session_->CreateIndexBtree(info));
  const std::array key{SqlValue::Integer(1), SqlValue::Integer(1)};
  RequireStatus(index.Insert(key));
  auto table_reader = TakeValue(TableBtreeCursor::Open(*database_, root));
  auto index_reader = TakeValue(IndexBtreeCursor::Open(*database_, index.root_page(), info));
  ASSERT_TRUE(TakeValue(table_reader.First()));
  ASSERT_TRUE(TakeValue(index_reader.First()));
  RequireStatus(BtreeDatabaseTestPeer::SaveCursors(*database_, index.root_page()));
  const auto saved = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_TRUE(BtreeDatabaseTestPeer::SavedIndexKey(*saved.front().identity).has_value());
  const auto reads = vfs_.total_database_reads();

  RequireStatus(database_->TripCursors(BtreeCursorTripMode::All));

  const auto expect_trip = [](const auto& result) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ErrorCode::kAborted, result.error().code());
    EXPECT_EQ(kSqliteAbortRollback, result.error().sqlite_code());
  };
  RequireStatus(table_reader.ClearPosition());
  RequireStatus(index_reader.ClearPosition());
  auto moved_table = std::move(table_reader);
  auto moved_index = std::move(index_reader);
  for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(*database_)) {
    EXPECT_EQ(btree_internal::BtreeCursorPhase::Fault, cursor.phase);
    EXPECT_EQ(kSqliteAbortRollback, BtreeDatabaseTestPeer::TripCode(*cursor.identity));
    EXPECT_FALSE(BtreeDatabaseTestPeer::SavedIndexKey(*cursor.identity).has_value());
  }
  for (const bool again : {false, true}) {
    SCOPED_TRACE(again);
    expect_trip(moved_table.CountEntries());
    expect_trip(moved_index.CountEntries());
    expect_trip(moved_table.First());
    expect_trip(moved_index.First());
    expect_trip(moved_table.Last());
    expect_trip(moved_index.Last());
    expect_trip(moved_table.Seek(1, BtreeSeekMode::kEqual));
    expect_trip(moved_index.Seek(key, BtreeSeekMode::kEqual));
    expect_trip(moved_table.RestorePosition());
    expect_trip(moved_index.RestorePosition());
    expect_trip(moved_table.rowid());
    expect_trip(moved_table.payload());
    expect_trip(moved_index.payload());
  }
  EXPECT_EQ(reads, vfs_.total_database_reads());
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
}

TEST_F(BtreeCursorOwnership, MovesATrippedPhysicalCursorWithoutRevivingItsCauseOrOldIdentity) {
  const PageNumber root = CreatePopulatedTable();
  btree_internal::MutationPageOwner owner{database_->pager()};
  auto cursor = TakeValue(btree_internal::WritableCursor::OpenTable(*database_, owner, root));
  ASSERT_TRUE(TakeValue(cursor.FirstTable()));
  RequireStatus(database_->TripCursors(BtreeCursorTripMode::All));

  auto moved = std::move(cursor);

  const auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(1U, cursors.size());
  EXPECT_EQ(&moved, cursors.front().identity);
  EXPECT_EQ(kSqliteAbortRollback, BtreeDatabaseTestPeer::TripCode(*cursors.front().identity));
  const auto fault = moved.FirstTable();
  ASSERT_FALSE(fault.has_value());
  EXPECT_EQ(kSqliteAbortRollback, fault.error().sqlite_code());
  const auto moved_from = cursor.CheckActive();
  ASSERT_FALSE(moved_from.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, moved_from.error().code());
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
}

TEST_F(BtreeCursorOwnership, RejectsAnInvalidTripModeWithoutMovingAnyCursor) {
  const PageNumber root = CreatePopulatedTable();
  auto cursor = TakeValue(TableBtreeCursor::Open(*database_, root));
  ASSERT_TRUE(TakeValue(cursor.First()));
  const auto pins = database_->pager().cache_pin_count();
  const auto reads = vfs_.total_database_reads();
  constexpr auto invalid_mode =
      static_cast<BtreeCursorTripMode>(std::numeric_limits<std::uint8_t>::max());

  const auto rejected = database_->TripCursors(invalid_mode);

  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  EXPECT_FALSE(cursor.HasMoved());
  EXPECT_EQ(1, TakeValue(cursor.rowid()));
  EXPECT_EQ(pins, database_->pager().cache_pin_count());
  EXPECT_EQ(reads, vfs_.total_database_reads());
  const auto invalid_rollback = database_->PrepareRollbackCursors(invalid_mode);
  ASSERT_FALSE(invalid_rollback.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_rollback.error().code());
  EXPECT_FALSE(cursor.HasMoved());
  EXPECT_EQ(pins, database_->pager().cache_pin_count());
}

TEST_F(BtreeCursorOwnership, ReadSaveFailureOverwritesEarlierWriterFaultsAndSavedReaders) {
  const PageNumber malformed_root = CreateTableWithMalformedCell(0);
  const PageNumber other_root = CreatePopulatedTable();
  auto tail = TakeValue(TableBtreeCursor::Open(*database_, malformed_root));
  auto authority = TakeValue(session_->OpenTableBtree(other_root));
  auto middle = TakeValue(authority.OpenMutationCursor());
  auto head = TakeValue(TableBtreeCursor::Open(*database_, other_root));
  ASSERT_TRUE(TakeValue(tail.First()));
  ASSERT_TRUE(TakeValue(middle.First()));
  ASSERT_TRUE(TakeValue(head.First()));
  const auto generation = database_->pager().write_transaction_generation();
  const auto writes = vfs_.total_journal_writes();

  const auto failed = database_->TripCursors(BtreeCursorTripMode::WriteOnly);

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, failed.error().code());
  for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(*database_)) {
    EXPECT_EQ(btree_internal::BtreeCursorPhase::Fault, cursor.phase);
    EXPECT_EQ(ToSqlitePrimaryCode(ErrorCode::kCorruption),
              BtreeDatabaseTestPeer::TripCode(*cursor.identity));
  }
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
  EXPECT_EQ(generation, database_->pager().write_transaction_generation());
  EXPECT_EQ(writes, vfs_.total_journal_writes());
}

TEST_F(BtreeCursorOwnership, SharesOverflowAcquisitionWithoutSharingListsOrInvalidatingOtherRoots) {
  auto table = TakeValue(session_->CreateTableBtree());
  ByteBuffer first{ByteCount{test::kWritePagerPageSize * 3U}};
  ByteBuffer second{ByteCount{test::kWritePagerPageSize * 4U}};
  std::ranges::fill(first.mutable_view(), std::byte{0x41});
  std::ranges::fill(second.mutable_view(), std::byte{0x42});
  RequireStatus(table.Insert(1, first.view()));
  RequireStatus(table.Insert(2, second.view()));
  auto reader = TakeValue(TableBtreeCursor::Open(*database_, table.root_page()));
  auto writer = TakeValue(table.OpenMutationCursor());
  ASSERT_TRUE(TakeValue(reader.First()));
  ASSERT_TRUE(TakeValue(writer.First()));
  EXPECT_TRUE(std::ranges::equal(first.view(), TakeValue(reader.CopyPayload()).view()));
  EXPECT_TRUE(std::ranges::equal(first.view(), TakeValue(writer.row()).payload));
  const auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(2U, cursors.size());
  const auto& write_state = *cursors[0].identity;
  const auto& read_state = *cursors[1].identity;
  const auto write_list = [&] { return BtreeDatabaseTestPeer::OverflowPages(write_state); };
  const auto read_list = [&] { return BtreeDatabaseTestPeer::OverflowPages(read_state); };
  ASSERT_TRUE(write_list().valid);
  ASSERT_TRUE(read_list().valid);
  EXPECT_NE(write_list().pages.data(), read_list().pages.data());
  const auto capacity = write_list().capacity;
  const auto* const storage = write_list().pages.data();

  static_cast<void>(CreatePopulatedTable());
  EXPECT_TRUE(write_list().valid);
  EXPECT_TRUE(read_list().valid);
  ASSERT_TRUE(TakeValue(writer.Next()));
  EXPECT_FALSE(write_list().valid);
  EXPECT_TRUE(read_list().valid);
  EXPECT_TRUE(std::ranges::equal(second.view(), TakeValue(writer.row()).payload));
  EXPECT_EQ(capacity, write_list().capacity);
  EXPECT_EQ(storage, write_list().pages.data());
  RequireStatus(writer.ReplaceCurrent(first.view()));
  EXPECT_FALSE(write_list().valid);
  EXPECT_FALSE(read_list().valid);
  EXPECT_TRUE(std::ranges::equal(first.view(), TakeValue(writer.row()).payload));
  EXPECT_TRUE(std::ranges::equal(first.view(), TakeValue(reader.CopyPayload()).view()));

  EXPECT_EQ(2U, TakeValue(table.Clear()));
  EXPECT_FALSE(write_list().valid);
  EXPECT_FALSE(read_list().valid);
  EXPECT_FALSE(TakeValue(reader.RestorePosition()));
  RequireStatus(table.Insert(1, second.view()));
  ASSERT_TRUE(TakeValue(reader.First()));
  ASSERT_TRUE(TakeValue(writer.First()));
  EXPECT_TRUE(std::ranges::equal(second.view(), TakeValue(reader.CopyPayload()).view()));
  EXPECT_TRUE(std::ranges::equal(second.view(), TakeValue(writer.row()).payload));
  RequireStatus(database_->TripCursors(BtreeCursorTripMode::WriteOnly));
  EXPECT_FALSE(write_list().valid);
  EXPECT_FALSE(read_list().valid);
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
}

TEST_F(BtreeCursorOwnership, RelocatesAValueCursorWithoutReorderingOrDuplicatingItsNode) {
  using namespace btree_internal;
  const PageNumber root = CreatePopulatedTable();
  auto authority = TakeValue(session_->OpenTableBtree(root));
  ByteBuffer payload{ByteCount{test::kWritePagerPageSize * 3U}};
  RequireStatus(authority.Insert(1, payload.view(), modern_sqlite::BtreeInsertMode::kReplace));
  {
    MutationPageOwner first_owner{database_->pager()};
    MutationPageOwner middle_owner{database_->pager()};
    MutationPageOwner last_owner{database_->pager()};
    WritableCursor first = TakeValue(WritableCursor::OpenTable(*database_, first_owner, root));
    WritableCursor middle = TakeValue(WritableCursor::OpenTable(*database_, middle_owner, root));
    WritableCursor head = TakeValue(WritableCursor::OpenTable(*database_, last_owner, root));
    ASSERT_TRUE(TakeValue(middle.FirstTable()));
    // BYTE_VECTOR_RESIZABLE_SCRATCH: the writable payload API resizes overflow storage.
    std::vector<std::byte> scratch;
    EXPECT_EQ(1, TakeValue(middle.CurrentTableRow(scratch)).rowid);
    const auto overflow = BtreeDatabaseTestPeer::OverflowPages(middle);
    ASSERT_TRUE(overflow.valid);
    const std::size_t pins = database_->pager().cache_pin_count();

    WritableCursor moved = std::move(middle);

    const auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
    ASSERT_EQ(3U, cursors.size());
    EXPECT_EQ(static_cast<const CursorState*>(&head), cursors[0].identity);
    EXPECT_EQ(static_cast<const CursorState*>(&moved), cursors[1].identity);
    EXPECT_EQ(static_cast<const CursorState*>(&first), cursors[2].identity);
    EXPECT_EQ(BtreeCursorPhase::Valid, cursors[1].phase);
    EXPECT_EQ(BtreeCursorPhase::Invalid, middle.state());
    EXPECT_EQ(pins, database_->pager().cache_pin_count());
    EXPECT_EQ(overflow.pages.data(), BtreeDatabaseTestPeer::OverflowPages(moved).pages.data());
    EXPECT_TRUE(BtreeDatabaseTestPeer::OverflowPages(moved).valid);
    EXPECT_TRUE(BtreeDatabaseTestPeer::OverflowPages(middle).pages.empty());
    EXPECT_FALSE(BtreeDatabaseTestPeer::OverflowPages(middle).valid);
    const auto detached = middle.CheckActive();
    ASSERT_FALSE(detached.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, detached.error().code());
    EXPECT_EQ(1, TakeValue(moved.CurrentTableRow(scratch)).rowid);
  }
  EXPECT_TRUE(BtreeDatabaseTestPeer::Cursors(*database_).empty());
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
}

TEST_F(BtreeCursorOwnership, RejectsAWritePageOwnerFromAnotherDatabaseBeforeAcquisition) {
  test::WritePagerFixedVfs foreign_vfs{false};
  auto foreign = test::OpenWriteDatabase<BtreeDatabase>(foreign_vfs, 64U);
  ASSERT_NE(nullptr, foreign);
  RequireStatus(foreign->pager().BeginRead());
  RequireStatus(foreign->pager().BeginWrite());
  BtreeWriteSession foreign_session = TakeValue(BtreeWriteSession::Open(*foreign));
  RequireStatus(foreign_session.InitializeDatabase());
  btree_internal::MutationPageOwner foreign_owner{foreign->pager()};
  const auto rejected =
      btree_internal::WritableCursor::OpenTable(*database_, foreign_owner, PageNumber{1});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  EXPECT_TRUE(BtreeDatabaseTestPeer::Cursors(*database_).empty());
  EXPECT_TRUE(BtreeDatabaseTestPeer::Cursors(*foreign).empty());
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
  EXPECT_EQ(0U, foreign->pager().cache_pin_count());
  RequireStatus(foreign->pager().Rollback());
}

TEST_F(BtreeCursorOwnership, PreservesNativeListOrderOnAPreMutationSaveError) {
  using btree_internal::BtreeCursorPhase;
  const PageNumber root = CreateTableWithMalformedCell(0);
  TableBtreeWriter table = TakeValue(session_->OpenTableBtree(root));
  TableBtreeCursor tail = TakeValue(TableBtreeCursor::Open(*database_, root));
  TableBtreeCursor middle = TakeValue(TableBtreeCursor::Open(*database_, root));
  TableBtreeCursor head = TakeValue(TableBtreeCursor::Open(*database_, root));
  ASSERT_TRUE(TakeValue(tail.Last()));
  ASSERT_TRUE(TakeValue(middle.First()));
  ASSERT_TRUE(TakeValue(head.Last()));
  const std::size_t pins = database_->pager().cache_pin_count();
  const PagerState state = database_->pager().state();
  const std::size_t journal_writes = vfs_.total_journal_writes();
  constexpr std::array payload{std::byte{0x42}};

  const auto rejected = table.Insert(7, payload);

  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, rejected.error().code());
  const auto cursors = BtreeDatabaseTestPeer::Cursors(*database_);
  ASSERT_EQ(3U, cursors.size());
  EXPECT_EQ(BtreeCursorPhase::RequireSeek, cursors[0].phase);
  EXPECT_EQ(BtreeCursorPhase::Valid, cursors[1].phase);
  EXPECT_EQ(BtreeCursorPhase::Valid, cursors[2].phase);
  EXPECT_EQ(pins - 1U, database_->pager().cache_pin_count());
  EXPECT_EQ(state, database_->pager().state());
  EXPECT_EQ(journal_writes, vfs_.total_journal_writes());
  EXPECT_FALSE(table.requires_rollback());
  EXPECT_EQ(5, TakeValue(tail.rowid()));
}

TEST_F(BtreeCursorOwnership, DoesNotRetryASavedNearestMoveAfterMalformedData) {
  const PageNumber root = CreateTableWithMalformedCell(1);
  TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*database_, root));
  ASSERT_TRUE(TakeValue(cursor.Last()));
  RequireStatus(BtreeDatabaseTestPeer::SaveCursors(*database_, root));
  EXPECT_EQ(0U, database_->pager().cache_pin_count());

  const auto failed = cursor.RestorePosition();

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, failed.error().code());
  EXPECT_FALSE(cursor.HasPosition());
  EXPECT_FALSE(TakeValue(cursor.RestorePosition()));
  ASSERT_TRUE(TakeValue(cursor.First()));
  EXPECT_EQ(1, TakeValue(cursor.rowid()));
}

TEST(BtreeDatabase, RegistersEmptySnapshotCursorsWithTheNativeZeroRoot) {
  test::WritePagerFixedVfs vfs{false};
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  RequireStatus(database->pager().BeginRead());
  {
    TableBtreeCursor first = TakeValue(TableBtreeCursor::Open(*database, PageNumber{1}));
    TableBtreeCursor second = TakeValue(TableBtreeCursor::Open(*database, PageNumber{1}));
    const auto cursors = BtreeDatabaseTestPeer::Cursors(*database);
    ASSERT_EQ(2U, cursors.size());
    for (const auto& cursor : cursors) {
      EXPECT_EQ(PageNumber{}, cursor.root);
      EXPECT_EQ(btree_internal::CursorAccess::Read, cursor.access);
      EXPECT_EQ(btree_internal::BtreeCursorPhase::Invalid, cursor.phase);
      EXPECT_TRUE(cursor.multiple);
    }
    EXPECT_FALSE(TakeValue(first.First()));
    EXPECT_FALSE(TakeValue(second.First()));
    EXPECT_EQ(0U, database->pager().cache_pin_count());
  }
  EXPECT_TRUE(BtreeDatabaseTestPeer::Cursors(*database).empty());
  RequireStatus(database->pager().EndRead());
}

}  // namespace
}  // namespace modern_sqlite
