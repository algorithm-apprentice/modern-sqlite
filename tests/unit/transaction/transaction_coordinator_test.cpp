#include "modern_sqlite/transaction/transaction_coordinator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
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

[[nodiscard]] TransactionCoordinator OpenCoordinator(test::WritePagerFixedVfs& vfs) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  if (pager == nullptr) {
    throw std::runtime_error("failed to open transaction test pager");
  }
  return TakeValue(TransactionCoordinator::Open(std::move(pager)));
}

TEST(TransactionCoordinator, OwnsPagerAndStartsDeferredOrImmediateTransactions) {
  {
    test::WritePagerFixedVfs vfs{false};
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    EXPECT_TRUE(coordinator.autocommit());
    EXPECT_EQ(TransactionState::kAutocommit, coordinator.state());

    RequireStatus(coordinator.Begin(TransactionMode::kDeferred));
    EXPECT_FALSE(coordinator.autocommit());
    EXPECT_EQ(TransactionState::kExplicit, coordinator.state());
    RequireStatus(coordinator.Rollback());
    EXPECT_TRUE(coordinator.autocommit());
  }
  {
    test::WritePagerFixedVfs vfs{false};
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
    EXPECT_EQ(TransactionState::kExplicit, coordinator.state());
    RequireStatus(coordinator.Rollback());
    EXPECT_TRUE(coordinator.autocommit());
  }
}

TEST(TransactionCoordinator, RejectsInvalidPagerOwnershipAndStatementOptions) {
  const auto null_pager = TransactionCoordinator::Open(std::unique_ptr<Pager>{});
  ASSERT_FALSE(null_pager.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, null_pager.error().code());

  {
    test::WritePagerFixedVfs vfs;
    auto read_only = Pager::Open(vfs, test::kWritePagerInputPath);
    ASSERT_TRUE(read_only.has_value());
    const auto rejected = TransactionCoordinator::Open(std::move(*read_only));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  }
  {
    test::WritePagerFixedVfs vfs;
    std::unique_ptr<Pager> active = test::OpenWritePager(vfs, 64U);
    ASSERT_NE(nullptr, active);
    RequireStatus(active->BeginRead());
    const auto rejected = TransactionCoordinator::Open(std::move(active));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  }
  {
    test::WritePagerFixedVfs vfs{false};
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    const auto read_with_transaction_rollback =
        coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kRead,
            .rollback = StatementRollbackMode::kTransaction,
        });
    ASSERT_FALSE(read_with_transaction_rollback.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, read_with_transaction_rollback.error().code());
  }
}

TEST(TransactionCoordinator, CoordinatesImplicitReadAndAbandonedStatementRollback) {
  test::WritePagerFixedVfs vfs{false};
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  {
    TransactionStatement read = TakeValue(coordinator.BeginStatement());
    EXPECT_EQ(StatementAccess::kRead, read.access());
    EXPECT_EQ(nullptr, read.writer());
    EXPECT_TRUE(coordinator.statement_active());
    const auto concurrent = coordinator.BeginStatement();
    ASSERT_FALSE(concurrent.has_value());
    EXPECT_EQ(ErrorCode::kBusy, concurrent.error().code());
    RequireStatus(read.Succeed());
    EXPECT_FALSE(coordinator.statement_active());
  }
  {
    TransactionStatement abandoned = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    ASSERT_NE(nullptr, abandoned.writer());
  }
  EXPECT_TRUE(coordinator.statement_active());
  RequireStatus(coordinator.Rollback());
  EXPECT_FALSE(coordinator.statement_active());
  EXPECT_TRUE(coordinator.autocommit());
}

TEST(TransactionCoordinator, EnforcesExplicitTransactionStateAroundReadStatement) {
  test::WritePagerFixedVfs vfs{false};
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  const auto missing_commit = coordinator.Commit();
  const auto missing_rollback = coordinator.Rollback();
  ASSERT_FALSE(missing_commit.has_value());
  ASSERT_FALSE(missing_rollback.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing_commit.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, missing_rollback.error().code());

  RequireStatus(coordinator.Begin());
  const auto nested = coordinator.Begin();
  ASSERT_FALSE(nested.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, nested.error().code());

  TransactionStatement read = TakeValue(coordinator.BeginStatement());
  const auto busy_commit = coordinator.Commit();
  ASSERT_FALSE(busy_commit.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy_commit.error().code());
  RequireStatus(read.Succeed());
  EXPECT_FALSE(coordinator.autocommit());
  RequireStatus(coordinator.Commit());
  EXPECT_TRUE(coordinator.autocommit());
}

TEST(TransactionCoordinator, CommitsImplicitWriteAndExpiresStatementHandles) {
  test::WritePagerFixedVfs vfs{false};
  std::optional<TableBtreeWriter> stale;
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    ASSERT_NE(nullptr, statement.writer());
    RequireStatus(statement.writer()->InitializeDatabase());
    stale.emplace(TakeValue(statement.writer()->OpenTableBtree(PageNumber{1})));
    const std::array payload{std::byte{0x41}, std::byte{0x42}};
    RequireStatus(stale->Insert(1, payload));
    RequireStatus(statement.Succeed());
    EXPECT_FALSE(statement.valid());
    EXPECT_TRUE(coordinator.autocommit());

    const auto stale_insert = stale->Insert(2, payload);
    ASSERT_FALSE(stale_insert.has_value());
    EXPECT_EQ(ErrorCode::kSchemaChanged, stale_insert.error().code());
  }

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(1, TakeValue(cursor.rowid()));
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, RetriesCommittedImplicitStatementCleanupWithoutRollingBack) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(statement.writer()->InitializeDatabase());
    TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
    const std::array payload{std::byte{0x49}};
    RequireStatus(table.Insert(1, payload));

    vfs.FailNextDatabaseUnlock(DatabaseLock::kNone, ErrorCode::kIo);
    const auto cleanup_failure = statement.Succeed();
    ASSERT_FALSE(cleanup_failure.has_value());
    EXPECT_EQ(ErrorCode::kIo, cleanup_failure.error().code());
    EXPECT_TRUE(statement.valid());
    EXPECT_TRUE(coordinator.statement_active());

    const auto forbidden_rollback = statement.Rollback();
    ASSERT_FALSE(forbidden_rollback.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, forbidden_rollback.error().code());
    EXPECT_TRUE(statement.valid());
    const auto blocked_statement = coordinator.BeginStatement();
    ASSERT_FALSE(blocked_statement.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, blocked_statement.error().code());

    RequireStatus(statement.Succeed());
    EXPECT_FALSE(statement.valid());
    EXPECT_TRUE(coordinator.autocommit());
  }

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(1, TakeValue(cursor.rowid()));
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, RollsBackImplicitWrite) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    TransactionStatement initialize = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(initialize.writer()->InitializeDatabase());
    RequireStatus(initialize.Succeed());

    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
    const std::array payload{std::byte{0x51}};
    RequireStatus(table.Insert(1, payload));
    RequireStatus(statement.Rollback());
    EXPECT_TRUE(coordinator.autocommit());
  }

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    EXPECT_FALSE(TakeValue(cursor.First()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, RollsBackOneStatementInsideExplicitTransaction) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin());

    std::optional<TableBtreeWriter> first_handle;
    TransactionStatement first = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(first.writer()->InitializeDatabase());
    first_handle.emplace(TakeValue(first.writer()->OpenTableBtree(PageNumber{1})));
    const std::array first_payload{std::byte{0x61}};
    RequireStatus(first_handle->Insert(1, first_payload));
    RequireStatus(first.Succeed());
    const auto stale = first_handle->Insert(3, first_payload);
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(ErrorCode::kSchemaChanged, stale.error().code());

    TransactionStatement second = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    TableBtreeWriter second_table = TakeValue(second.writer()->OpenTableBtree(PageNumber{1}));
    const std::array second_payload{std::byte{0x62}};
    RequireStatus(second_table.Insert(2, second_payload));
    RequireStatus(second.Rollback());

    TransactionStatement probe = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    TableBtreeWriter probe_table = TakeValue(probe.writer()->OpenTableBtree(PageNumber{1}));
    const auto duplicate = probe_table.Insert(1, first_payload);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(ErrorCode::kConstraint, duplicate.error().code());
    RequireStatus(probe_table.Insert(2, second_payload));
    RequireStatus(probe.Rollback());

    RequireStatus(coordinator.Commit());
    EXPECT_TRUE(coordinator.autocommit());
  }

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(1, TakeValue(cursor.rowid()));
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, TransactionModeStatementRollbackEndsOuterTransaction) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin());

    TransactionStatement first = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(first.writer()->InitializeDatabase());
    TableBtreeWriter table = TakeValue(first.writer()->OpenTableBtree(PageNumber{1}));
    const std::array payload{std::byte{0x71}};
    RequireStatus(table.Insert(1, payload));
    RequireStatus(first.Succeed());

    TransactionStatement second = TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
        .access = StatementAccess::kWrite,
        .rollback = StatementRollbackMode::kTransaction,
    }));
    TableBtreeWriter second_table = TakeValue(second.writer()->OpenTableBtree(PageNumber{1}));
    RequireStatus(second_table.Insert(2, payload));
    RequireStatus(second.Rollback());
    EXPECT_TRUE(coordinator.autocommit());
  }

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  EXPECT_EQ(nullptr, pager->header());
  EXPECT_EQ(0U, pager->page_count());
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, TransactionModeStatementCannotLaunderRollbackRequiredFailure) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    TransactionStatement initialize = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(initialize.writer()->InitializeDatabase());
    TableBtreeWriter table = TakeValue(initialize.writer()->OpenTableBtree(PageNumber{1}));
    ByteBuffer payload{ByteCount{380}};
    std::ranges::fill(payload.mutable_view(), std::byte{0x31});
    RequireStatus(table.Insert(1, payload.view()));
    RequireStatus(initialize.Succeed());
  }
  const ByteBuffer fixture = ByteBuffer::CopyOf(vfs.database_bytes());

  std::size_t mutation_count = 0U;
  {
    vfs.LoadDatabase(fixture.view());
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin());
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kTransaction,
        }));
    TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
    ByteBuffer payload{ByteCount{2'000}};
    std::ranges::fill(payload.mutable_view(), std::byte{0x32});
    vfs.ArmCrashCut(std::nullopt);
    RequireStatus(table.Insert(2, payload.view()));
    mutation_count = vfs.mutation_count();
    RequireStatus(coordinator.Rollback());
  }
  ASSERT_GT(mutation_count, 0U);

  bool observed_rollback_required = false;
  for (std::size_t cut = 1U; cut <= mutation_count; ++cut) {
    vfs.LoadDatabase(fixture.view());
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin());
    TransactionStatement statement =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kTransaction,
        }));
    TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
    ByteBuffer payload{ByteCount{2'000}};
    std::ranges::fill(payload.mutable_view(), std::byte{0x32});
    vfs.ArmCrashCut(cut);
    const auto mutation = table.Insert(2, payload.view());
    ASSERT_FALSE(mutation.has_value());
    EXPECT_EQ(ErrorCode::kIo, mutation.error().code());

    const auto finish = statement.Succeed();
    if (!finish.has_value() && coordinator.autocommit()) {
      EXPECT_EQ(ErrorCode::kIo, finish.error().code());
      EXPECT_FALSE(statement.valid());
      observed_rollback_required = true;
      break;
    }
    ASSERT_TRUE(finish.has_value());
    RequireStatus(coordinator.Rollback());
  }
  EXPECT_TRUE(observed_rollback_required);
}

}  // namespace
}  // namespace modern_sqlite
