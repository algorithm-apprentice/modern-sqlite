#include "modern_sqlite/storage/btree/database.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "storage/btree/cursor_state.hpp"
#include "storage/btree/writer_internal.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite {

class BtreeDatabaseTestPeer final {
 public:
  struct Cursor {
    const btree_internal::CursorState* identity;
    PageNumber root;
    btree_internal::CursorAccess access;
    btree_internal::BtreeCursorPhase phase;
    bool multiple;
  };

  [[nodiscard]] static std::vector<Cursor> Cursors(const BtreeDatabase& database) {
    std::vector<Cursor> result;
    for (const auto* cursor = database.cursors_; cursor != nullptr; cursor = cursor->next_) {
      result.push_back(Cursor{
          .identity = cursor,
          .root = cursor->root_page(),
          .access = cursor->access(),
          .phase = cursor->state(),
          .multiple = cursor->multiple(),
      });
    }
    return result;
  }
};

namespace {

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

TEST_F(BtreeCursorOwnership, RelocatesAValueCursorWithoutReorderingOrDuplicatingItsNode) {
  using namespace btree_internal;
  const PageNumber root = CreatePopulatedTable();
  {
    MutationPageOwner first_owner{database_->pager()};
    MutationPageOwner middle_owner{database_->pager()};
    MutationPageOwner last_owner{database_->pager()};
    WritableCursor first = TakeValue(WritableCursor::Open(*database_, first_owner, root, true));
    WritableCursor middle = TakeValue(WritableCursor::Open(*database_, middle_owner, root, true));
    WritableCursor head = TakeValue(WritableCursor::Open(*database_, last_owner, root, true));
    ASSERT_TRUE(TakeValue(middle.FirstTable()));
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
    const auto detached = middle.CheckActive();
    ASSERT_FALSE(detached.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, detached.error().code());
    // BYTE_VECTOR_RESIZABLE_SCRATCH: the writable payload API resizes overflow storage.
    std::vector<std::byte> scratch;
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
      btree_internal::WritableCursor::Open(*database_, foreign_owner, PageNumber{1}, true);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  EXPECT_TRUE(BtreeDatabaseTestPeer::Cursors(*database_).empty());
  EXPECT_TRUE(BtreeDatabaseTestPeer::Cursors(*foreign).empty());
  EXPECT_EQ(0U, database_->pager().cache_pin_count());
  EXPECT_EQ(0U, foreign->pager().cache_pin_count());
  RequireStatus(foreign->pager().Rollback());
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
