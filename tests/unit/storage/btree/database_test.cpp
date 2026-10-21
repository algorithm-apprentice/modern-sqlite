#include "modern_sqlite/storage/btree/database.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <type_traits>
#include <utility>

#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite {
namespace {

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

}  // namespace
}  // namespace modern_sqlite
