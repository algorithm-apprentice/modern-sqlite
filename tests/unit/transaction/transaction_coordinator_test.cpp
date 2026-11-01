#include "modern_sqlite/transaction/transaction_coordinator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <compare>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "modern_sqlite/vm/vm.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"
#include "tests/unit/storage/btree/database_test_support.hpp"

namespace modern_sqlite {
namespace {

constexpr int kSqliteAbortRollback = 516;

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

[[nodiscard]] TransactionCoordinator OpenCoordinator(test::WritePagerFixedVfs& vfs,
                                                     std::size_t cache_pages = 64U) {
  std::unique_ptr<BtreeDatabase> pager_database =
      test::OpenWriteDatabase<BtreeDatabase>(vfs, cache_pages);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  if (pager == nullptr) {
    throw std::runtime_error("failed to open transaction test pager");
  }
  return TakeValue(TransactionCoordinator::Open(std::move(pager_database)));
}

[[nodiscard]] ByteBuffer MakeTransactionFixture(test::WritePagerFixedVfs& vfs) {
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(statement.writer()->InitializeDatabase());
  TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
  const std::array payload{std::byte{0x01}};
  RequireStatus(table.Insert(1, payload));
  RequireStatus(statement.Succeed());
  return ByteBuffer::CopyOf(vfs.database_bytes());
}

[[nodiscard]] TransactionCoordinator PrepareNamedMutation(test::WritePagerFixedVfs& vfs,
                                                          ByteView fixture) {
  vfs.LoadDatabase(fixture);
  TransactionCoordinator coordinator = OpenCoordinator(vfs, 1U);
  RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
  RequireStatus(coordinator.Savepoint(Utf8View{"s"}));
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
  ByteBuffer payload{ByteCount{2'000}};
  std::ranges::fill(payload.mutable_view(), std::byte{0x02});
  RequireStatus(table.Insert(2, payload.view()));
  RequireStatus(statement.Succeed());
  return coordinator;
}

enum class ModelActionKind : std::uint8_t {
  kBeginDeferred,
  kBeginImmediate,
  kBeginRead,
  kBeginWrite,
  kSucceedStatement,
  kRollbackStatement,
  kSavepoint,
  kRelease,
  kRollbackTo,
  kCommit,
  kRollback,
};

struct ModelAction {
  constexpr ModelAction(ModelActionKind action_kind, std::string_view action_name) noexcept
      : kind(action_kind), name(action_name) {}

  ModelActionKind kind;
  std::string_view name;
};

// Admitting a write statement does not itself mutate a database page.
struct ReferenceTransactionModel {
  TransactionState transaction = TransactionState::kAutocommit;
  PagerState pager = PagerState::kOpen;
  std::optional<StatementAccess> statement;
  std::vector<std::string> savepoints;

  [[nodiscard]] static bool EqualName(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
      const auto left_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(left[index]));
      const auto right_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(right[index]));
      if (SqliteToLower(left_byte) != SqliteToLower(right_byte)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::optional<std::size_t> Find(std::string_view name) const noexcept {
    for (std::size_t index = savepoints.size(); index > 0U; --index) {
      if (EqualName(savepoints[index - 1U], name)) {
        return index - 1U;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<ErrorCode> Apply(const ModelAction& action) {
    switch (action.kind) {
      case ModelActionKind::kBeginDeferred:
      case ModelActionKind::kBeginImmediate:
        if (statement.has_value()) {
          return ErrorCode::kBusy;
        }
        if (transaction != TransactionState::kAutocommit) {
          return ErrorCode::kMisuse;
        }
        transaction = TransactionState::kExplicit;
        if (action.kind == ModelActionKind::kBeginImmediate) {
          pager = PagerState::kWriterLocked;
        }
        return std::nullopt;
      case ModelActionKind::kBeginRead:
      case ModelActionKind::kBeginWrite:
        if (statement.has_value()) {
          return ErrorCode::kBusy;
        }
        statement = action.kind == ModelActionKind::kBeginRead ? StatementAccess::kRead
                                                               : StatementAccess::kWrite;
        if (pager == PagerState::kOpen) {
          pager = action.kind == ModelActionKind::kBeginRead ? PagerState::kReader
                                                             : PagerState::kWriterLocked;
        } else if (action.kind == ModelActionKind::kBeginWrite && pager == PagerState::kReader) {
          pager = PagerState::kWriterLocked;
        }
        return std::nullopt;
      case ModelActionKind::kSucceedStatement:
      case ModelActionKind::kRollbackStatement:
        if (!statement.has_value()) {
          return ErrorCode::kSchemaChanged;
        }
        if (transaction == TransactionState::kAutocommit) {
          pager = PagerState::kOpen;
          savepoints.clear();
        }
        statement.reset();
        return std::nullopt;
      case ModelActionKind::kSavepoint:
        if (statement.has_value()) {
          return ErrorCode::kBusy;
        }
        savepoints.emplace_back(action.name);
        if (transaction == TransactionState::kAutocommit) {
          transaction = TransactionState::kSavepoint;
        }
        return std::nullopt;
      case ModelActionKind::kRelease: {
        if (statement.has_value()) {
          return ErrorCode::kBusy;
        }
        const auto found = Find(action.name);
        if (!found.has_value()) {
          return ErrorCode::kGeneric;
        }
        if (transaction == TransactionState::kSavepoint && *found == 0U) {
          transaction = TransactionState::kAutocommit;
          pager = PagerState::kOpen;
          savepoints.clear();
          return std::nullopt;
        }
        savepoints.erase(savepoints.begin() + static_cast<std::ptrdiff_t>(*found),
                         savepoints.end());
        return std::nullopt;
      }
      case ModelActionKind::kRollbackTo: {
        if (statement.has_value()) {
          return ErrorCode::kBusy;
        }
        const auto found = Find(action.name);
        if (!found.has_value()) {
          return ErrorCode::kGeneric;
        }
        savepoints.erase(savepoints.begin() + static_cast<std::ptrdiff_t>(*found + 1U),
                         savepoints.end());
        return std::nullopt;
      }
      case ModelActionKind::kCommit:
        if (statement.has_value()) {
          return ErrorCode::kBusy;
        }
        if (transaction == TransactionState::kAutocommit) {
          return ErrorCode::kMisuse;
        }
        transaction = TransactionState::kAutocommit;
        pager = PagerState::kOpen;
        savepoints.clear();
        return std::nullopt;
      case ModelActionKind::kRollback:
        if (transaction == TransactionState::kAutocommit && !statement.has_value() &&
            pager == PagerState::kOpen) {
          return ErrorCode::kMisuse;
        }
        transaction = TransactionState::kAutocommit;
        pager = PagerState::kOpen;
        statement.reset();
        savepoints.clear();
        return std::nullopt;
    }
    return ErrorCode::kInternal;
  }
};

template <typename T>
[[nodiscard]] std::optional<ErrorCode> ResultCode(const Result<T>& result) noexcept {
  return result.has_value() ? std::nullopt : std::optional<ErrorCode>{result.error().code()};
}

[[nodiscard]] std::optional<ErrorCode> StatusCode(const Status& status) noexcept {
  return status.has_value() ? std::nullopt : std::optional<ErrorCode>{status.error().code()};
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

TEST(TransactionCoordinator, SuppliesStatementScopedVmExecutionContext) {
  test::WritePagerFixedVfs vfs{false};
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  TransactionStatement statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  ASSERT_NE(nullptr, statement.writer());

  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 5};
  input.statement_kind = ProgramStatementKind::kUpdate;
  input.transaction_access = ProgramTransactionAccess::kWrite;
  input.mutation_result.publishes_changes = true;
  input.requires_database_snapshot = true;
  input.instructions.emplace_back(HaltInstruction{});
  auto program = BytecodeProgram::Create(input);
  ASSERT_TRUE(program.has_value());
  Vm vm = TakeValue(Vm::Create(*program, VmEnvironment::Core()));

  RequireStatus(vm.AttachExecutionContext(VmExecutionContext{*statement.writer(), 5}));
  EXPECT_EQ(VmStep::kDone, TakeValue(vm.Step()));
  RequireStatus(vm.DetachExecutionContext());
  RequireStatus(statement.Rollback());
}

TEST(TransactionCoordinator, RejectsInvalidPagerOwnershipAndStatementOptions) {
  const auto null_pager = TransactionCoordinator::Open(std::unique_ptr<BtreeDatabase>{});
  ASSERT_FALSE(null_pager.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, null_pager.error().code());

  {
    test::WritePagerFixedVfs vfs;
    auto read_only = BtreeDatabase::Open(vfs, test::kWritePagerInputPath);
    ASSERT_TRUE(read_only.has_value());
    const auto rejected = TransactionCoordinator::Open(std::move(*read_only));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
  }
  {
    test::WritePagerFixedVfs vfs;
    std::unique_ptr<BtreeDatabase> active_database =
        test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    Pager* active = active_database == nullptr ? nullptr : &active_database->pager();
    ASSERT_NE(nullptr, active);
    RequireStatus(active->BeginRead());
    const auto rejected = TransactionCoordinator::Open(std::move(active_database));
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
    const auto read_only_writer = coordinator.BeginStatement(TransactionStatementOptions{
        .access = StatementAccess::kWrite,
        .read_only = true,
    });
    ASSERT_FALSE(read_only_writer.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, read_only_writer.error().code());
    EXPECT_FALSE(coordinator.statement_active());
    EXPECT_EQ(0U, coordinator.work_counters().pager_write_begins);
    EXPECT_EQ(0U, coordinator.work_counters().btree_writer_opens);
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
  std::optional<IndexBtreeWriter> stale_index;
  const std::array payload{std::byte{0x41}, std::byte{0x42}};
  const std::array<IndexColumnOrder, 1> columns{
      IndexColumnOrder{BinaryCollation()},
  };
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    ASSERT_NE(nullptr, statement.writer());
    RequireStatus(statement.writer()->InitializeDatabase());
    stale.emplace(TakeValue(statement.writer()->OpenTableBtree(PageNumber{1})));
    stale_index.emplace(TakeValue(modern_sqlite::test::CreateIndexBtree(
        *(statement.writer()), columns,
        modern_sqlite::IndexKeyShape{.key_field_count = (columns).size(),
                                     .all_field_count = (columns).size()})));
    RequireStatus(stale->Insert(1, payload));
    RequireStatus(statement.Succeed());
    EXPECT_FALSE(statement.valid());
    EXPECT_TRUE(coordinator.autocommit());
  }
  const auto stale_insert = stale->Insert(2, payload);
  ASSERT_FALSE(stale_insert.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale_insert.error().code());
  std::array<SqlValue, 1> stale_key{SqlValue::Integer(1)};
  const auto stale_index_insert = stale_index->Insert(stale_key);
  ASSERT_FALSE(stale_index_insert.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, stale_index_insert.error().code());

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
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

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
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

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    EXPECT_FALSE(TakeValue(cursor.First()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, CommitsAndRollsBackSchemaCookieIncrements) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    TransactionStatement initialize = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(initialize.writer()->InitializeDatabase());
    RequireStatus(initialize.Succeed());

    TransactionStatement rolled_back =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    EXPECT_EQ(1U, TakeValue(rolled_back.writer()->IncrementSchemaCookie()));
    RequireStatus(rolled_back.Rollback());

    TransactionStatement committed =
        TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = StatementRollbackMode::kStatement,
        }));
    EXPECT_EQ(1U, TakeValue(committed.writer()->IncrementSchemaCookie()));
    RequireStatus(committed.Succeed());
  }

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  ASSERT_NE(nullptr, pager->header());
  EXPECT_EQ(1U, pager->header()->schema_cookie());
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, DeliversSchemaResetRequestsForFullAndNamedRollback) {
  const std::array cases{
      std::pair{false, false},
      std::pair{true, false},
      std::pair{true, true},
  };
  for (const auto& [named, save_after_schema_change] : cases) {
    SCOPED_TRACE(named);
    SCOPED_TRACE(save_after_schema_change);
    test::WritePagerFixedVfs vfs{false};
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    auto initialize = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(initialize.writer()->InitializeDatabase());
    RequireStatus(initialize.Succeed());
    RequireStatus(coordinator.Begin());
    if (named && !save_after_schema_change) {
      RequireStatus(coordinator.Savepoint(Utf8View{"s"}));
    }
    auto changed = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    EXPECT_EQ(1U, TakeValue(changed.writer()->IncrementSchemaCookie()));
    RequireStatus(changed.Succeed());
    if (named && save_after_schema_change) {
      RequireStatus(coordinator.Savepoint(Utf8View{"s"}));
    }
    EXPECT_FALSE(coordinator.TakeSchemaResetRequest());
    RequireStatus(named ? coordinator.RollbackTo(Utf8View{"s"}) : coordinator.Rollback());
    EXPECT_TRUE(coordinator.TakeSchemaResetRequest());
    EXPECT_FALSE(coordinator.TakeSchemaResetRequest());
    if (named) {
      RequireStatus(coordinator.Rollback());
      EXPECT_TRUE(coordinator.TakeSchemaResetRequest());
    }
  }
}

TEST(TransactionCoordinator, RetainsSchemaResetRequestsAcrossTerminalCleanup) {
  test::WritePagerFixedVfs vfs{false};
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  auto initialize = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(initialize.writer()->InitializeDatabase());
  RequireStatus(initialize.Succeed());
  RequireStatus(coordinator.Begin());
  auto changed = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  EXPECT_EQ(1U, TakeValue(changed.writer()->IncrementSchemaCookie()));
  RequireStatus(changed.Succeed());
  vfs.FailNextDatabaseUnlock(DatabaseLock::kNone, ErrorCode::kIo);
  const auto failed = coordinator.Rollback();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  RequireStatus(coordinator.Rollback());
  EXPECT_TRUE(coordinator.autocommit());
  EXPECT_TRUE(coordinator.TakeSchemaResetRequest());
  EXPECT_FALSE(coordinator.TakeSchemaResetRequest());
}

TEST(TransactionCoordinator, DoesNotResetSchemasForStatementAbortCommitOrDmlRollback) {
  test::WritePagerFixedVfs vfs{false};
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  auto initialize = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(initialize.writer()->InitializeDatabase());
  RequireStatus(initialize.Succeed());
  RequireStatus(coordinator.Begin());
  auto changed = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  EXPECT_EQ(1U, TakeValue(changed.writer()->IncrementSchemaCookie()));
  RequireStatus(changed.Succeed());
  auto aborted = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(aborted.Rollback(ErrorCode::kConstraint));
  EXPECT_FALSE(coordinator.TakeSchemaResetRequest());
  RequireStatus(coordinator.Commit());
  EXPECT_FALSE(coordinator.TakeSchemaResetRequest());
  RequireStatus(coordinator.Begin());
  auto unchanged = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  RequireStatus(unchanged.Succeed());
  RequireStatus(coordinator.Rollback());
  EXPECT_FALSE(coordinator.TakeSchemaResetRequest());
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

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(1, TakeValue(cursor.rowid()));
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, JournalFreeWritesKeepEpochsAndNamedSavepoints) {
  test::WritePagerFixedVfs vfs{false};
  const ByteBuffer fixture = MakeTransactionFixture(vfs);
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  RequireStatus(coordinator.Begin());
  const TransactionStatementOptions options{
      .access = StatementAccess::kWrite,
      .uses_statement_journal = false,
  };
  const std::array payload{std::byte{0x62}};
  TransactionStatement first = TakeValue(coordinator.BeginStatement(options));
  TableBtreeWriter stale = TakeValue(first.writer()->OpenTableBtree(PageNumber{1}));
  RequireStatus(stale.Insert(2, payload));
  RequireStatus(first.Succeed());
  EXPECT_EQ(0U, coordinator.work_counters().pager_savepoint_creates);
  const auto escaped = stale.Insert(3, payload);
  ASSERT_FALSE(escaped.has_value());
  EXPECT_EQ(ErrorCode::kSchemaChanged, escaped.error().code());

  RequireStatus(coordinator.Savepoint(Utf8View{"kept"}));
  TransactionStatement second = TakeValue(coordinator.BeginStatement(options));
  TableBtreeWriter table = TakeValue(second.writer()->OpenTableBtree(PageNumber{1}));
  RequireStatus(table.Insert(3, payload));
  RequireStatus(second.Succeed());
  EXPECT_EQ(1U, coordinator.work_counters().pager_savepoint_creates);
  EXPECT_EQ(1U, coordinator.work_counters().btree_writer_opens);
  RequireStatus(coordinator.RollbackTo(Utf8View{"kept"}));
  RequireStatus(coordinator.Rollback());
  EXPECT_TRUE(std::ranges::equal(fixture.view(), vfs.database_bytes()));
}

TEST(TransactionCoordinator, SelectsNativeErrorCompletionFromJournalAdmission) {
  for (const bool journal : {false, true}) {
    for (const ErrorCode cause : {ErrorCode::kConstraint, ErrorCode::kIo, ErrorCode::kInterrupted,
                                  ErrorCode::kOutOfMemory, ErrorCode::kFull}) {
      SCOPED_TRACE(journal);
      SCOPED_TRACE(static_cast<int>(cause));
      test::WritePagerFixedVfs vfs{false};
      const ByteBuffer fixture = MakeTransactionFixture(vfs);
      TransactionCoordinator coordinator = OpenCoordinator(vfs);
      RequireStatus(coordinator.Begin());
      TransactionStatement statement =
          TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
              .access = StatementAccess::kWrite,
              .uses_statement_journal = journal,
          }));
      const bool full_rollback =
          cause == ErrorCode::kIo || cause == ErrorCode::kInterrupted ||
          (!journal && (cause == ErrorCode::kOutOfMemory || cause == ErrorCode::kFull));
      RequireStatus(statement.Rollback(cause));
      EXPECT_EQ(full_rollback, coordinator.autocommit());
      EXPECT_FALSE(statement.valid());
      EXPECT_EQ(journal ? 1U : 0U, coordinator.work_counters().pager_savepoint_creates);
      if (!coordinator.autocommit()) {
        RequireStatus(coordinator.Rollback());
      }
      EXPECT_TRUE(std::ranges::equal(fixture.view(), vfs.database_bytes()));
    }
  }
}

TEST(TransactionCoordinator, ReadResourceErrorsRetireTheOuterWriteTransaction) {
  for (const bool read_only : {false, true}) {
    for (const ErrorCode cause : {ErrorCode::kGeneric, ErrorCode::kInterrupted,
                                  ErrorCode::kOutOfMemory, ErrorCode::kIo, ErrorCode::kFull}) {
      SCOPED_TRACE(read_only);
      SCOPED_TRACE(static_cast<int>(cause));
      test::WritePagerFixedVfs vfs{false};
      const ByteBuffer fixture = MakeTransactionFixture(vfs);
      auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
      ASSERT_NE(nullptr, database);
      auto& borrowed = *database;
      TransactionCoordinator coordinator =
          TakeValue(TransactionCoordinator::Open(std::move(database)));
      RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
      {
        auto prior = TakeValue(coordinator.BeginStatement({.access = StatementAccess::kWrite}));
        auto writer = TakeValue(prior.writer()->OpenTableBtree(PageNumber{1}));
        const std::array payload{std::byte{0x02}};
        RequireStatus(writer.Insert(2, payload));
        RequireStatus(prior.Succeed());
      }
      RequireStatus(coordinator.Savepoint(Utf8View{"kept"}));
      const auto before = coordinator.work_counters();
      TransactionStatement statement =
          TakeValue(coordinator.BeginStatement({.read_only = read_only}));
      EXPECT_EQ(nullptr, statement.writer());
      EXPECT_EQ(before.pager_write_begins, coordinator.work_counters().pager_write_begins);
      EXPECT_EQ(before.btree_writer_opens, coordinator.work_counters().btree_writer_opens);
      EXPECT_EQ(before.pager_savepoint_creates,
                coordinator.work_counters().pager_savepoint_creates);
      RequireStatus(statement.Rollback(cause));
      const bool full =
          cause != ErrorCode::kGeneric && (cause != ErrorCode::kInterrupted || !read_only);
      EXPECT_EQ(full, coordinator.autocommit());
      EXPECT_EQ(full ? 0U : 1U, coordinator.savepoint_count());
      EXPECT_FALSE(coordinator.pending_completion().has_value());
      {
        auto read = TakeValue(coordinator.BeginStatement());
        {
          auto cursor = TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1}));
          EXPECT_TRUE(TakeValue(cursor.First()));
          EXPECT_EQ(1, TakeValue(cursor.rowid()));
          EXPECT_EQ(!full, TakeValue(cursor.Next()));
          if (!full) {
            EXPECT_EQ(2, TakeValue(cursor.rowid()));
            EXPECT_FALSE(TakeValue(cursor.Next()));
          }
        }
        RequireStatus(read.Succeed());
      }
      if (!coordinator.autocommit()) {
        RequireStatus(coordinator.RollbackTo(Utf8View{"kept"}));
        RequireStatus(coordinator.Rollback());
      }
      EXPECT_TRUE(std::ranges::equal(fixture.view(), vfs.database_bytes()));
    }
  }
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

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
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
  vfs.LoadDatabase(fixture.view());
  TransactionCoordinator coordinator = OpenCoordinator(vfs, 1U);
  RequireStatus(coordinator.Begin());
  TransactionStatement statement = TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
      .access = StatementAccess::kWrite,
      .rollback = StatementRollbackMode::kTransaction,
  }));
  TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
  ByteBuffer payload{ByteCount{6'000}};
  std::ranges::fill(payload.mutable_view(), std::byte{0x32});
  vfs.FailDatabaseWriteAfter(1U, ErrorCode::kIo);
  const auto mutation = table.Insert(2, payload.view());
  ASSERT_FALSE(mutation.has_value());
  EXPECT_EQ(ErrorCode::kIo, mutation.error().code());
  EXPECT_GT(vfs.database_write_count(), 0U);

  const auto finish = statement.Succeed();
  ASSERT_FALSE(finish.has_value());
  EXPECT_EQ(ErrorCode::kIo, finish.error().code());
  EXPECT_FALSE(statement.valid());
  EXPECT_TRUE(coordinator.autocommit());
}

TEST(TransactionCoordinator, NamedSavepointsUseNewestCaseInsensitiveMatch) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Savepoint(Utf8View{"Outer"}));
    EXPECT_EQ(TransactionState::kSavepoint, coordinator.state());
    EXPECT_FALSE(coordinator.autocommit());

    TransactionStatement first = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(first.writer()->InitializeDatabase());
    TableBtreeWriter first_table = TakeValue(first.writer()->OpenTableBtree(PageNumber{1}));
    const std::array first_payload{std::byte{0x11}};
    RequireStatus(first_table.Insert(1, first_payload));
    RequireStatus(first.Succeed());

    RequireStatus(coordinator.Savepoint(Utf8View{"dup"}));
    TransactionStatement second = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    TableBtreeWriter second_table = TakeValue(second.writer()->OpenTableBtree(PageNumber{1}));
    const std::array second_payload{std::byte{0x22}};
    RequireStatus(second_table.Insert(2, second_payload));
    RequireStatus(second.Succeed());

    RequireStatus(coordinator.Savepoint(Utf8View{"DUP"}));
    TransactionStatement third = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    TableBtreeWriter third_table = TakeValue(third.writer()->OpenTableBtree(PageNumber{1}));
    const std::array third_payload{std::byte{0x33}};
    RequireStatus(third_table.Insert(3, third_payload));
    RequireStatus(third.Succeed());

    RequireStatus(coordinator.RollbackTo(Utf8View{"DuP"}));
    RequireStatus(coordinator.Release(Utf8View{"dUp"}));
    RequireStatus(coordinator.RollbackTo(Utf8View{"OUTER"}));

    TransactionStatement replacement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(replacement.writer()->InitializeDatabase());
    TableBtreeWriter replacement_table =
        TakeValue(replacement.writer()->OpenTableBtree(PageNumber{1}));
    const std::array replacement_payload{std::byte{0x44}};
    RequireStatus(replacement_table.Insert(4, replacement_payload));
    RequireStatus(replacement.Succeed());

    RequireStatus(coordinator.Release(Utf8View{"outer"}));
    EXPECT_TRUE(coordinator.autocommit());
  }

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(4, TakeValue(cursor.rowid()));
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, FullRollbackReleasesTableAndIndexPeersBeforePagerPlayback) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(MakeTransactionFixture(vfs));
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  BtreeDatabase& borrowed = *database;
  auto coordinator = TakeValue(TransactionCoordinator::Open(std::move(database)));
  const std::array columns{IndexColumnOrder{}, IndexColumnOrder{}};
  const auto info = TakeValue(IndexComparisonInfo::CopyOf(columns));
  auto initial = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  auto table = TakeValue(initial.writer()->OpenTableBtree(PageNumber{1}));
  constexpr std::array payload{std::byte{0x01}};
  RequireStatus(table.Insert(3, payload));
  auto index = TakeValue(initial.writer()->CreateIndexBtree(info));
  for (const std::int64_t rowid : {1, 3}) {
    RequireStatus(index.Insert(std::array{SqlValue::Integer(rowid), SqlValue::Integer(rowid)}));
  }
  RequireStatus(initial.Succeed());
  RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
  auto changed = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  auto changed_table = TakeValue(changed.writer()->OpenTableBtree(PageNumber{1}));
  auto changed_index = TakeValue(changed.writer()->OpenIndexBtree(index.root_page(), info));
  RequireStatus(changed_table.Insert(2, payload));
  RequireStatus(changed_index.Insert(std::array{SqlValue::Integer(2), SqlValue::Integer(2)}));
  RequireStatus(changed.Succeed());
  std::optional<TransactionStatement> resumed_read;
  {
    auto table_reader = TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1}));
    auto index_reader = TakeValue(IndexBtreeCursor::Open(borrowed, index.root_page(), info));
    ASSERT_TRUE(TakeValue(table_reader.Seek(2, BtreeSeekMode::kEqual)));
    ASSERT_TRUE(TakeValue(index_reader.Seek(std::array{SqlValue::Integer(2), SqlValue::Integer(2)},
                                            BtreeSeekMode::kEqual)));

    const auto rolled_back = coordinator.Rollback();

    ASSERT_TRUE(rolled_back.has_value())
        << (rolled_back.has_value() ? "" : rolled_back.error().ToString());
    EXPECT_TRUE(coordinator.autocommit());
    EXPECT_EQ(1U, coordinator.work_counters().pager_rollbacks);
    EXPECT_EQ(0U, borrowed.pager().cache_pin_count());
    resumed_read.emplace(TakeValue(coordinator.BeginStatement()));
    ASSERT_FALSE(TakeValue(table_reader.RestorePosition()));
    ASSERT_FALSE(TakeValue(index_reader.RestorePosition()));
    ASSERT_TRUE(TakeValue(table_reader.Next()));
    ASSERT_TRUE(TakeValue(index_reader.Next()));
    EXPECT_EQ(3, TakeValue(table_reader.rowid()));
    EXPECT_EQ(std::weak_ordering::equivalent, TakeValue(index_reader.CompareCurrent(std::array{
                                                  SqlValue::Integer(3), SqlValue::Integer(3)})));
  }
  RequireStatus(resumed_read->Succeed());
}

TEST(TransactionCoordinator, MatchesNativeFullRollbackPolicyAcrossJournalFlagsAndScopes) {
  enum class Scope { Implicit, Explicit, Schema };
  for (const auto scope : {Scope::Implicit, Scope::Explicit, Scope::Schema}) {
    for (const bool journal : {false, true}) {
      for (const auto cause :
           {ErrorCode::kGeneric, ErrorCode::kConstraint, ErrorCode::kIo, ErrorCode::kInterrupted,
            ErrorCode::kOutOfMemory, ErrorCode::kFull}) {
        SCOPED_TRACE(static_cast<int>(scope));
        SCOPED_TRACE(journal);
        SCOPED_TRACE(static_cast<int>(cause));
        test::WritePagerFixedVfs vfs{false};
        static_cast<void>(MakeTransactionFixture(vfs));
        auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
        ASSERT_NE(nullptr, database);
        BtreeDatabase& borrowed = *database;
        auto coordinator = TakeValue(TransactionCoordinator::Open(std::move(database)));
        if (scope != Scope::Implicit) {
          RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
        }
        auto statement = TakeValue(coordinator.BeginStatement(TransactionStatementOptions{
            .access = StatementAccess::kWrite,
            .rollback = scope == Scope::Implicit ? StatementRollbackMode::kStatement
                                                 : StatementRollbackMode::kTransaction,
            .uses_statement_journal = journal,
        }));
        if (scope == Scope::Schema) {
          static_cast<void>(TakeValue(statement.writer()->IncrementSchemaCookie()));
        }
        auto table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
        {
          auto writer = TakeValue(table.OpenMutationCursor());
          auto reader = TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1}));
          ASSERT_TRUE(TakeValue(writer.First()));
          ASSERT_TRUE(TakeValue(reader.First()));

          RequireStatus(statement.Rollback(cause));

          const bool resource = cause == ErrorCode::kOutOfMemory || cause == ErrorCode::kFull;
          const bool trips = scope != Scope::Implicit || cause == ErrorCode::kIo ||
                             cause == ErrorCode::kInterrupted || (resource && !journal);
          for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(borrowed)) {
            const bool fault = scope == Scope::Schema ||
                               (trips && cursor.access == btree_internal::CursorAccess::Write);
            EXPECT_EQ(fault ? btree_internal::BtreeCursorPhase::Fault
                            : btree_internal::BtreeCursorPhase::RequireSeek,
                      cursor.phase);
            EXPECT_EQ(fault ? std::optional<int>{kSqliteAbortRollback} : std::nullopt,
                      BtreeDatabaseTestPeer::TripCode(*cursor.identity));
          }
          EXPECT_TRUE(coordinator.autocommit());
          EXPECT_FALSE(statement.valid());
          EXPECT_FALSE(coordinator.pending_completion().has_value());
          EXPECT_EQ(1U, coordinator.work_counters().pager_rollbacks);
          EXPECT_EQ(0U, borrowed.pager().cache_pin_count());
        }
      }
    }
  }
}

TEST(TransactionCoordinator, PreservesImplicitSavePolicyAcrossAnExplicitRollbackRetry) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(MakeTransactionFixture(vfs));
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  BtreeDatabase& borrowed = *database;
  auto coordinator = TakeValue(TransactionCoordinator::Open(std::move(database)));
  auto statement = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  auto table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
  {
    auto writer = TakeValue(table.OpenMutationCursor());
    auto reader = TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1}));
    ASSERT_TRUE(TakeValue(writer.First()));
    ASSERT_TRUE(TakeValue(reader.First()));
    {
      const auto raw_pin = TakeValue(borrowed.pager().ReadPage(PageNumber{1}));

      const auto blocked = statement.Rollback(ErrorCode::kConstraint);

      ASSERT_FALSE(blocked.has_value());
      EXPECT_EQ(ErrorCode::kBusy, blocked.error().code());
      EXPECT_EQ(TransactionCompletionKind::Rollback, coordinator.pending_completion());
      EXPECT_EQ(0U, coordinator.work_counters().pager_rollbacks);
      EXPECT_EQ(PageNumber{1}, raw_pin.frame().page_number());
    }

    RequireStatus(coordinator.Rollback());

    EXPECT_TRUE(coordinator.autocommit());
    EXPECT_FALSE(coordinator.pending_completion().has_value());
    EXPECT_EQ(1U, coordinator.work_counters().pager_rollbacks);
    for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(borrowed)) {
      EXPECT_EQ(btree_internal::BtreeCursorPhase::RequireSeek, cursor.phase);
      EXPECT_FALSE(BtreeDatabaseTestPeer::TripCode(*cursor.identity).has_value());
    }
  }
}

TEST(TransactionCoordinator, FullRollbackFencesReadOnlyAndDeferredTransactionsWithoutPlayback) {
  for (const bool reading : {false, true}) {
    SCOPED_TRACE(reading);
    test::WritePagerFixedVfs vfs{false};
    static_cast<void>(MakeTransactionFixture(vfs));
    auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    ASSERT_NE(nullptr, database);
    BtreeDatabase& borrowed = *database;
    auto coordinator = TakeValue(TransactionCoordinator::Open(std::move(database)));
    RequireStatus(coordinator.Begin());
    std::optional<TableBtreeCursor> reader;
    if (reading) {
      auto statement = TakeValue(coordinator.BeginStatement());
      RequireStatus(statement.Succeed());
      reader.emplace(TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1})));
      ASSERT_TRUE(TakeValue(reader->First()));
    }

    RequireStatus(coordinator.Rollback());

    EXPECT_TRUE(coordinator.autocommit());
    EXPECT_FALSE(coordinator.pending_completion().has_value());
    EXPECT_EQ(0U, coordinator.work_counters().pager_rollbacks);
    EXPECT_EQ(0U, borrowed.pager().cache_pin_count());
    if (reader.has_value()) {
      EXPECT_TRUE(reader->HasMoved());
      const auto cursors = BtreeDatabaseTestPeer::Cursors(borrowed);
      ASSERT_EQ(1U, cursors.size());
      ASSERT_NE(nullptr, cursors.data());
      EXPECT_EQ(btree_internal::BtreeCursorPhase::RequireSeek, cursors.front().phase);
    }
  }
}

TEST(TransactionCoordinator, SavepointRollbackSavesTableAndIndexReadersBeforePlayback) {
  for (const bool anonymous : {false, true}) {
    SCOPED_TRACE(anonymous);
    test::WritePagerFixedVfs vfs{false};
    auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    ASSERT_NE(nullptr, database);
    BtreeDatabase& borrowed = *database;
    auto coordinator = TakeValue(TransactionCoordinator::Open(std::move(database)));
    RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
    const std::array columns{IndexColumnOrder{}, IndexColumnOrder{}};
    const auto comparison_info = TakeValue(IndexComparisonInfo::CopyOf(columns));
    TransactionStatement initial = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(initial.writer()->InitializeDatabase());
    auto first_table = TakeValue(initial.writer()->CreateTableBtree());
    auto other_table = TakeValue(initial.writer()->CreateTableBtree());
    auto index = TakeValue(initial.writer()->CreateIndexBtree(comparison_info));
    constexpr std::array payload{std::byte{0x42}};
    for (const std::int64_t rowid : {1, 3}) {
      RequireStatus(first_table.Insert(rowid, payload));
      RequireStatus(other_table.Insert(rowid, payload));
      RequireStatus(index.Insert(std::array{SqlValue::Integer(rowid), SqlValue::Integer(rowid)}));
    }
    RequireStatus(initial.Succeed());
    RequireStatus(coordinator.Savepoint(Utf8View{"kept"}));
    TransactionStatement changed = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    auto changed_first = TakeValue(changed.writer()->OpenTableBtree(first_table.root_page()));
    auto changed_other = TakeValue(changed.writer()->OpenTableBtree(other_table.root_page()));
    auto changed_index =
        TakeValue(changed.writer()->OpenIndexBtree(index.root_page(), comparison_info));
    RequireStatus(changed_first.Insert(2, payload));
    RequireStatus(changed_other.Insert(2, payload));
    RequireStatus(changed_index.Insert(std::array{SqlValue::Integer(2), SqlValue::Integer(2)}));
    if (!anonymous) {
      RequireStatus(changed.Succeed());
    }
    {
      auto first = TakeValue(TableBtreeCursor::Open(borrowed, first_table.root_page()));
      auto other = TakeValue(TableBtreeCursor::Open(borrowed, other_table.root_page()));
      auto index_reader =
          TakeValue(IndexBtreeCursor::Open(borrowed, index.root_page(), comparison_info));
      ASSERT_TRUE(TakeValue(first.Seek(2, BtreeSeekMode::kEqual)));
      ASSERT_TRUE(TakeValue(other.Seek(2, BtreeSeekMode::kEqual)));
      ASSERT_TRUE(TakeValue(index_reader.Seek(
          std::array{SqlValue::Integer(2), SqlValue::Integer(2)}, BtreeSeekMode::kEqual)));
      const auto generation = borrowed.pager().write_transaction_generation();

      const auto rolled_back = anonymous ? changed.Rollback(ErrorCode::kConstraint)
                                         : coordinator.RollbackTo(Utf8View{"kept"});

      ASSERT_TRUE(rolled_back.has_value())
          << (rolled_back.has_value() ? "" : rolled_back.error().ToString());
      EXPECT_FALSE(coordinator.autocommit());
      EXPECT_EQ(generation + 1U, borrowed.pager().write_transaction_generation());
      EXPECT_EQ(0U, borrowed.pager().cache_pin_count());
      for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(borrowed)) {
        EXPECT_EQ(btree_internal::BtreeCursorPhase::RequireSeek, cursor.phase);
      }
      ASSERT_FALSE(TakeValue(first.RestorePosition()));
      ASSERT_FALSE(TakeValue(other.RestorePosition()));
      ASSERT_FALSE(TakeValue(index_reader.RestorePosition()));
      ASSERT_TRUE(TakeValue(first.Next()));
      ASSERT_TRUE(TakeValue(other.Next()));
      ASSERT_TRUE(TakeValue(index_reader.Next()));
      EXPECT_EQ(3, TakeValue(first.rowid()));
      EXPECT_EQ(3, TakeValue(other.rowid()));
      EXPECT_EQ(std::weak_ordering::equivalent, TakeValue(index_reader.CompareCurrent(std::array{
                                                    SqlValue::Integer(3), SqlValue::Integer(3)})));
      EXPECT_EQ(1U, coordinator.savepoint_count());
    }
    RequireStatus(coordinator.Rollback());
  }
}

TEST(TransactionCoordinator, SchemaSavepointRollbackTripsReadersAndRetainedWritersBeforePlayback) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(MakeTransactionFixture(vfs));
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  BtreeDatabase& borrowed = *database;
  auto coordinator = TakeValue(TransactionCoordinator::Open(std::move(database)));
  RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
  RequireStatus(coordinator.Savepoint(Utf8View{"schema"}));
  auto changed = TakeValue(
      coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
  const std::array columns{IndexColumnOrder{}, IndexColumnOrder{}};
  const auto info = TakeValue(IndexComparisonInfo::CopyOf(columns));
  auto index = TakeValue(changed.writer()->CreateIndexBtree(info));
  RequireStatus(index.Insert(std::array{SqlValue::Integer(10), SqlValue::Integer(1)}));
  static_cast<void>(TakeValue(changed.writer()->IncrementSchemaCookie()));
  auto table = TakeValue(changed.writer()->OpenTableBtree(PageNumber{1}));
  {
    auto writer = TakeValue(table.OpenMutationCursor());
    ASSERT_TRUE(TakeValue(writer.First()));
    RequireStatus(changed.Succeed());
    auto table_reader = TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1}));
    auto index_reader = TakeValue(IndexBtreeCursor::Open(borrowed, index.root_page(), info));
    ASSERT_TRUE(TakeValue(table_reader.First()));
    ASSERT_TRUE(TakeValue(index_reader.First()));
    RequireStatus(coordinator.Savepoint(Utf8View{"released"}));
    RequireStatus(coordinator.Release(Utf8View{"released"}));
    EXPECT_FALSE(table_reader.HasMoved());
    EXPECT_FALSE(index_reader.HasMoved());
    EXPECT_FALSE(writer.HasMoved());

    RequireStatus(coordinator.RollbackTo(Utf8View{"schema"}));

    EXPECT_EQ(0U, borrowed.pager().cache_pin_count());
    for (const auto& cursor : BtreeDatabaseTestPeer::Cursors(borrowed)) {
      EXPECT_EQ(btree_internal::BtreeCursorPhase::Fault, cursor.phase);
    }
    const auto table_fault = table_reader.First();
    ASSERT_FALSE(table_fault.has_value());
    EXPECT_EQ(ErrorCode::kAborted, table_fault.error().code());
    EXPECT_EQ(kSqliteAbortRollback, table_fault.error().sqlite_code());
    const auto index_fault = index_reader.First();
    ASSERT_FALSE(index_fault.has_value());
    EXPECT_EQ(ErrorCode::kAborted, index_fault.error().code());
    EXPECT_EQ(kSqliteAbortRollback, index_fault.error().sqlite_code());
    const auto writer_fault = writer.Next();
    ASSERT_FALSE(writer_fault.has_value());
    EXPECT_EQ(ErrorCode::kAborted, writer_fault.error().code());
    EXPECT_EQ(kSqliteAbortRollback, writer_fault.error().sqlite_code());
    EXPECT_FALSE(coordinator.autocommit());
    EXPECT_EQ(1U, coordinator.savepoint_count());
  }
  RequireStatus(coordinator.Rollback());
}

TEST(TransactionCoordinator, CookieAttemptTripPolicyStaysAcrossNamedRollbackAndClearsAtCommit) {
  for (const bool failed_cookie : {false, true}) {
    SCOPED_TRACE(failed_cookie);
    test::WritePagerFixedVfs vfs{false};
    static_cast<void>(MakeTransactionFixture(vfs));
    auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
    ASSERT_NE(nullptr, database);
    BtreeDatabase& borrowed = *database;
    auto coordinator = TakeValue(TransactionCoordinator::Open(std::move(database)));
    RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
    RequireStatus(coordinator.Savepoint(Utf8View{"schema"}));
    auto changed = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    if (failed_cookie) {
      {
        const auto pin = TakeValue(borrowed.pager().ReadPage(PageNumber{1}));
        const auto rejected = changed.writer()->IncrementSchemaCookie();
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(ErrorCode::kBusy, rejected.error().code());
      }
      RequireStatus(changed.Rollback(ErrorCode::kBusy));
    } else {
      static_cast<void>(TakeValue(changed.writer()->IncrementSchemaCookie()));
      RequireStatus(changed.Succeed());
    }
    for (const bool again : {false, true}) {
      SCOPED_TRACE(again);
      auto reader = TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1}));
      ASSERT_TRUE(TakeValue(reader.First()));

      RequireStatus(coordinator.RollbackTo(Utf8View{"schema"}));

      const auto fault = reader.First();
      ASSERT_FALSE(fault.has_value());
      EXPECT_EQ(kSqliteAbortRollback, fault.error().sqlite_code());
    }
    RequireStatus(coordinator.Commit());
    RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
    RequireStatus(coordinator.Savepoint(Utf8View{"data"}));
    {
      auto reader = TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1}));
      ASSERT_TRUE(TakeValue(reader.First()));

      RequireStatus(coordinator.RollbackTo(Utf8View{"data"}));

      ASSERT_TRUE(TakeValue(reader.RestorePosition()));
      EXPECT_EQ(1, TakeValue(reader.rowid()));
    }
    RequireStatus(coordinator.Rollback());
  }
}

TEST(TransactionCoordinator, NamedReadOnlyRollbackTripsBeforeCheckingForAPagerSavepoint) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(MakeTransactionFixture(vfs));
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  ASSERT_NE(nullptr, database);
  BtreeDatabase& borrowed = *database;
  auto coordinator = TakeValue(TransactionCoordinator::Open(std::move(database)));
  RequireStatus(coordinator.Begin());
  RequireStatus(coordinator.Savepoint(Utf8View{"read"}));
  auto statement = TakeValue(coordinator.BeginStatement());
  RequireStatus(statement.Succeed());
  {
    auto reader = TakeValue(TableBtreeCursor::Open(borrowed, PageNumber{1}));
    ASSERT_TRUE(TakeValue(reader.First()));
    const auto missing = coordinator.RollbackTo(Utf8View{"absent"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, missing.error().code());
    EXPECT_FALSE(reader.HasMoved());
    const auto generation = borrowed.pager().write_transaction_generation();

    RequireStatus(coordinator.RollbackTo(Utf8View{"read"}));

    EXPECT_TRUE(reader.HasMoved());
    EXPECT_EQ(0U, borrowed.pager().cache_pin_count());
    EXPECT_EQ(generation, borrowed.pager().write_transaction_generation());
    EXPECT_EQ(0U, coordinator.work_counters().pager_savepoint_creates);
    EXPECT_EQ(0U, coordinator.work_counters().pager_savepoint_rollbacks);
    ASSERT_TRUE(TakeValue(reader.RestorePosition()));
    EXPECT_EQ(1, TakeValue(reader.rowid()));
  }
  RequireStatus(coordinator.Rollback());
}

TEST(TransactionCoordinator, TransactionSavepointReleaseRetriesCommittedCleanup) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Savepoint(Utf8View{"outer"}));
    TransactionStatement statement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(statement.writer()->InitializeDatabase());
    TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
    const std::array payload{std::byte{0x45}};
    RequireStatus(table.Insert(1, payload));
    RequireStatus(statement.Succeed());

    vfs.FailNextDatabaseUnlock(DatabaseLock::kNone, ErrorCode::kIo);
    const auto cleanup_failure = coordinator.Release(Utf8View{"OUTER"});
    ASSERT_FALSE(cleanup_failure.has_value());
    EXPECT_EQ(ErrorCode::kIo, cleanup_failure.error().code());
    EXPECT_EQ(TransactionState::kSavepoint, coordinator.state());

    const auto forbidden_rollback = coordinator.RollbackTo(Utf8View{"outer"});
    ASSERT_FALSE(forbidden_rollback.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, forbidden_rollback.error().code());

    RequireStatus(coordinator.Release(Utf8View{"outer"}));
    EXPECT_TRUE(coordinator.autocommit());
  }

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(1, TakeValue(cursor.rowid()));
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, RollbackToRecoversRetryableTransactionSavepointCommitFailure) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Savepoint(Utf8View{"outer"}));
    TransactionStatement first = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(first.writer()->InitializeDatabase());
    TableBtreeWriter first_table = TakeValue(first.writer()->OpenTableBtree(PageNumber{1}));
    const std::array first_payload{std::byte{0x51}};
    RequireStatus(first_table.Insert(1, first_payload));
    RequireStatus(first.Succeed());

    vfs.FailNextDatabaseLock(DatabaseLock::kExclusive, ErrorCode::kBusy);
    const auto commit_failure = coordinator.Release(Utf8View{"outer"});
    ASSERT_FALSE(commit_failure.has_value());
    EXPECT_EQ(ErrorCode::kBusy, commit_failure.error().code());
    RequireStatus(coordinator.RollbackTo(Utf8View{"OUTER"}));

    TransactionStatement replacement = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(replacement.writer()->InitializeDatabase());
    TableBtreeWriter replacement_table =
        TakeValue(replacement.writer()->OpenTableBtree(PageNumber{1}));
    const std::array replacement_payload{std::byte{0x52}};
    RequireStatus(replacement_table.Insert(2, replacement_payload));
    RequireStatus(replacement.Succeed());
    RequireStatus(coordinator.Release(Utf8View{"outer"}));
  }

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(2, TakeValue(cursor.rowid()));
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, ReleaseRemovesMatchedAndNewerLogicalSavepoints) {
  test::WritePagerFixedVfs vfs{false};
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  RequireStatus(coordinator.Begin());
  RequireStatus(coordinator.Savepoint(Utf8View{"a"}));
  RequireStatus(coordinator.Savepoint(Utf8View{"b"}));
  RequireStatus(coordinator.Release(Utf8View{"A"}));

  const auto missing = coordinator.RollbackTo(Utf8View{"b"});
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, missing.error().code());
  RequireStatus(coordinator.Rollback());
}

TEST(TransactionCoordinator, ReleaseRemovesMatchedAndNewerMaterializedSavepoints) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin());
    TransactionStatement first = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(first.writer()->InitializeDatabase());
    TableBtreeWriter first_table = TakeValue(first.writer()->OpenTableBtree(PageNumber{1}));
    const std::array payload{std::byte{0x55}};
    RequireStatus(first_table.Insert(1, payload));
    RequireStatus(first.Succeed());

    RequireStatus(coordinator.Savepoint(Utf8View{"a"}));
    TransactionStatement second = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    TableBtreeWriter second_table = TakeValue(second.writer()->OpenTableBtree(PageNumber{1}));
    RequireStatus(second_table.Insert(2, payload));
    RequireStatus(second.Succeed());

    RequireStatus(coordinator.Savepoint(Utf8View{"b"}));
    TransactionStatement third = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    TableBtreeWriter third_table = TakeValue(third.writer()->OpenTableBtree(PageNumber{1}));
    RequireStatus(third_table.Insert(3, payload));
    RequireStatus(third.Succeed());

    RequireStatus(coordinator.Release(Utf8View{"A"}));
    const auto missing = coordinator.RollbackTo(Utf8View{"b"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, missing.error().code());
    RequireStatus(coordinator.Commit());
  }

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    for (std::int64_t expected = 1; expected <= 3; ++expected) {
      const bool has_row = expected == 1 ? TakeValue(cursor.First()) : TakeValue(cursor.Next());
      ASSERT_TRUE(has_row);
      EXPECT_EQ(expected, TakeValue(cursor.rowid()));
    }
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinator, RejectsMalformedSavepointNameBeforeStateChange) {
  test::WritePagerFixedVfs vfs{false};
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  const std::string malformed{"\xc0", 1U};
  const auto rejected = coordinator.Savepoint(Utf8View{malformed});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kFormat, rejected.error().code());
  EXPECT_TRUE(coordinator.autocommit());

  RequireStatus(coordinator.Savepoint(Utf8View{""}));
  RequireStatus(coordinator.Release(Utf8View{""}));
  EXPECT_TRUE(coordinator.autocommit());
}

TEST(TransactionCoordinator, SavepointPublicationFailureFullyRollsBackTransaction) {
  test::WritePagerFixedVfs vfs{false};
  const ByteBuffer fixture = MakeTransactionFixture(vfs);

  std::size_t mutation_count = 0U;
  {
    vfs.LoadDatabase(fixture.view());
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
    vfs.ArmCrashCut(std::nullopt);
    RequireStatus(coordinator.Savepoint(Utf8View{"s"}));
    mutation_count = vfs.mutation_count();
    RequireStatus(coordinator.Rollback());
  }
  ASSERT_GT(mutation_count, 0U);

  for (std::size_t cut = 1U; cut <= mutation_count; ++cut) {
    vfs.LoadDatabase(fixture.view());
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin(TransactionMode::kImmediate));
    vfs.ArmCrashCut(cut);
    const auto saved = coordinator.Savepoint(Utf8View{"s"});
    ASSERT_FALSE(saved.has_value());
    EXPECT_EQ(ErrorCode::kIo, saved.error().code());
    EXPECT_TRUE(coordinator.autocommit());
    EXPECT_TRUE(std::ranges::equal(vfs.database_bytes(), fixture.view()));
  }
}

TEST(TransactionCoordinator, SavepointReleaseAfterMutationPerformsNoPersistentIo) {
  test::WritePagerFixedVfs vfs{false};
  const ByteBuffer fixture = MakeTransactionFixture(vfs);

  TransactionCoordinator coordinator = PrepareNamedMutation(vfs, fixture.view());
  vfs.ArmCrashCut(std::nullopt);
  RequireStatus(coordinator.Release(Utf8View{"s"}));
  EXPECT_EQ(0U, vfs.mutation_count());
  RequireStatus(coordinator.Rollback());
}

TEST(TransactionCoordinator, RecoverableSavepointRollbackFailureFullyRollsBackTransaction) {
  test::WritePagerFixedVfs vfs{false};
  const ByteBuffer fixture = MakeTransactionFixture(vfs);

  TransactionCoordinator coordinator = PrepareNamedMutation(vfs, fixture.view());
  EXPECT_GT(vfs.database_write_count(), 0U);
  vfs.FailDatabaseWriteAfter(0U, ErrorCode::kIo);
  const auto rolled_back = coordinator.RollbackTo(Utf8View{"s"});
  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(ErrorCode::kIo, rolled_back.error().code());
  EXPECT_TRUE(coordinator.autocommit());
  EXPECT_TRUE(std::ranges::equal(vfs.database_bytes(), fixture.view()));
}

TEST(TransactionCoordinator, NamedSavepointControlRejectsActiveStatement) {
  test::WritePagerFixedVfs vfs{false};
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  TransactionStatement statement = TakeValue(coordinator.BeginStatement());
  const auto savepoint = coordinator.Savepoint(Utf8View{"blocked"});
  const auto release = coordinator.Release(Utf8View{"blocked"});
  const auto rollback_to = coordinator.RollbackTo(Utf8View{"blocked"});
  ASSERT_FALSE(savepoint.has_value());
  ASSERT_FALSE(release.has_value());
  ASSERT_FALSE(rollback_to.has_value());
  EXPECT_EQ(ErrorCode::kBusy, savepoint.error().code());
  EXPECT_EQ(ErrorCode::kBusy, release.error().code());
  EXPECT_EQ(ErrorCode::kBusy, rollback_to.error().code());
  RequireStatus(statement.Rollback());
}

TEST(TransactionCoordinatorModel, MixedTransactionsMatchReferenceMap) {
  test::WritePagerFixedVfs vfs{false};
  {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    TransactionStatement initialize = TakeValue(
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite}));
    RequireStatus(initialize.writer()->InitializeDatabase());
    RequireStatus(initialize.Succeed());
  }

  std::map<std::int64_t, std::byte> durable;
  for (std::size_t cycle = 0U; cycle < 120U; ++cycle) {
    TransactionCoordinator coordinator = OpenCoordinator(vfs);
    RequireStatus(coordinator.Begin());
    std::map<std::int64_t, std::byte> working = durable;

    const std::int64_t first_key = static_cast<std::int64_t>((cycle * 7U) % 31U) + 1;
    const auto first_value = static_cast<std::byte>((cycle + 1U) & 0xffU);
    {
      TransactionStatement statement = TakeValue(coordinator.BeginStatement(
          TransactionStatementOptions{.access = StatementAccess::kWrite}));
      TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
      std::array<std::byte, 8> payload{};
      std::ranges::fill(payload, first_value);
      RequireStatus(table.Insert(first_key, payload, BtreeInsertMode::kReplace));
      RequireStatus(statement.Succeed());
      working.insert_or_assign(first_key, first_value);
    }

    RequireStatus(coordinator.Savepoint(Utf8View{"model"}));
    const std::map<std::int64_t, std::byte> savepoint_model = working;
    const std::int64_t second_key = static_cast<std::int64_t>((cycle * 11U + 3U) % 31U) + 1;
    const auto second_value = static_cast<std::byte>((cycle + 101U) & 0xffU);
    {
      TransactionStatement statement = TakeValue(coordinator.BeginStatement(
          TransactionStatementOptions{.access = StatementAccess::kWrite}));
      TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
      std::array<std::byte, 8> payload{};
      std::ranges::fill(payload, second_value);
      RequireStatus(table.Insert(second_key, payload, BtreeInsertMode::kReplace));
      if (cycle % 3U == 0U) {
        RequireStatus(statement.Rollback());
      } else {
        RequireStatus(statement.Succeed());
        working.insert_or_assign(second_key, second_value);
      }
    }

    if (cycle % 4U == 0U) {
      RequireStatus(coordinator.RollbackTo(Utf8View{"MODEL"}));
      working = savepoint_model;
      RequireStatus(coordinator.Release(Utf8View{"model"}));
    } else {
      RequireStatus(coordinator.Release(Utf8View{"MODEL"}));
    }

    const std::int64_t delete_key = static_cast<std::int64_t>((cycle * 13U + 5U) % 31U) + 1;
    {
      TransactionStatement statement = TakeValue(coordinator.BeginStatement(
          TransactionStatementOptions{.access = StatementAccess::kWrite}));
      TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
      EXPECT_EQ(working.erase(delete_key) != 0U, TakeValue(table.Delete(delete_key)));
      RequireStatus(statement.Succeed());
    }

    if (cycle % 5U == 0U) {
      RequireStatus(coordinator.Rollback());
    } else {
      RequireStatus(coordinator.Commit());
      durable = std::move(working);
    }
  }

  std::unique_ptr<BtreeDatabase> pager_database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager_database, PageNumber{1}));
    auto expected = durable.begin();
    bool has_row = TakeValue(cursor.First());
    while (has_row) {
      ASSERT_NE(durable.end(), expected);
      EXPECT_EQ(expected->first, TakeValue(cursor.rowid()));
      const ByteBuffer payload = TakeValue(cursor.CopyPayload());
      ASSERT_EQ(ByteCount{8}, payload.size());
      EXPECT_TRUE(std::ranges::all_of(
          payload.view(), [value = expected->second](std::byte byte) { return byte == value; }));
      ++expected;
      has_row = TakeValue(cursor.Next());
    }
    EXPECT_EQ(durable.end(), expected);
  }
  RequireStatus(pager->EndRead());
}

TEST(TransactionCoordinatorModel, StateMachineMatchesValidAndInvalidActions) {
  test::WritePagerFixedVfs vfs{false};
  static_cast<void>(MakeTransactionFixture(vfs));
  TransactionCoordinator coordinator = OpenCoordinator(vfs);
  ReferenceTransactionModel model;
  std::optional<TransactionStatement> statement;
  const std::vector<ModelAction> actions{
      {ModelActionKind::kCommit, {}},
      {ModelActionKind::kBeginDeferred, {}},
      {ModelActionKind::kBeginDeferred, {}},
      {ModelActionKind::kSavepoint, "A"},
      {ModelActionKind::kSavepoint, "a"},
      {ModelActionKind::kBeginRead, {}},
      {ModelActionKind::kSavepoint, "blocked"},
      {ModelActionKind::kCommit, {}},
      {ModelActionKind::kSucceedStatement, {}},
      {ModelActionKind::kRollbackTo, "A"},
      {ModelActionKind::kRelease, "a"},
      {ModelActionKind::kBeginWrite, {}},
      {ModelActionKind::kRelease, "A"},
      {ModelActionKind::kSucceedStatement, {}},
      {ModelActionKind::kSavepoint, "B"},
      {ModelActionKind::kRollbackTo, "a"},
      {ModelActionKind::kRelease, "A"},
      {ModelActionKind::kCommit, {}},
      {ModelActionKind::kSavepoint, "outer"},
      {ModelActionKind::kBeginWrite, {}},
      {ModelActionKind::kSucceedStatement, {}},
      {ModelActionKind::kRelease, "OUTER"},
      {ModelActionKind::kBeginRead, {}},
      {ModelActionKind::kRollback, {}},
      {ModelActionKind::kBeginImmediate, {}},
      {ModelActionKind::kSavepoint, "s"},
      {ModelActionKind::kBeginWrite, {}},
      {ModelActionKind::kRollbackStatement, {}},
      {ModelActionKind::kRollbackTo, "missing"},
      {ModelActionKind::kRollback, {}},
  };

  for (std::size_t index = 0U; index < actions.size(); ++index) {
    SCOPED_TRACE(index);
    const ModelAction& action = actions[index];
    const std::optional<ErrorCode> expected = model.Apply(action);
    std::optional<ErrorCode> actual;
    switch (action.kind) {
      case ModelActionKind::kBeginDeferred:
        actual = StatusCode(coordinator.Begin());
        break;
      case ModelActionKind::kBeginImmediate:
        actual = StatusCode(coordinator.Begin(TransactionMode::kImmediate));
        break;
      case ModelActionKind::kBeginRead: {
        auto begun = coordinator.BeginStatement();
        actual = ResultCode(begun);
        if (begun.has_value()) {
          statement = std::move(*begun);
        }
        break;
      }
      case ModelActionKind::kBeginWrite: {
        auto begun = coordinator.BeginStatement(
            TransactionStatementOptions{.access = StatementAccess::kWrite});
        actual = ResultCode(begun);
        if (begun.has_value()) {
          statement = std::move(*begun);
        }
        break;
      }
      case ModelActionKind::kSucceedStatement:
        actual = statement.has_value() ? StatusCode(statement->Succeed())
                                       : std::optional<ErrorCode>{ErrorCode::kSchemaChanged};
        if (!actual.has_value()) {
          statement.reset();
        }
        break;
      case ModelActionKind::kRollbackStatement:
        actual = statement.has_value() ? StatusCode(statement->Rollback())
                                       : std::optional<ErrorCode>{ErrorCode::kSchemaChanged};
        if (!actual.has_value()) {
          statement.reset();
        }
        break;
      case ModelActionKind::kSavepoint:
        actual = StatusCode(coordinator.Savepoint(Utf8View{action.name}));
        break;
      case ModelActionKind::kRelease:
        actual = StatusCode(coordinator.Release(Utf8View{action.name}));
        break;
      case ModelActionKind::kRollbackTo:
        actual = StatusCode(coordinator.RollbackTo(Utf8View{action.name}));
        break;
      case ModelActionKind::kCommit:
        actual = StatusCode(coordinator.Commit());
        break;
      case ModelActionKind::kRollback:
        actual = StatusCode(coordinator.Rollback());
        if (!actual.has_value()) {
          statement.reset();
        }
        break;
    }

    EXPECT_EQ(expected, actual);
    EXPECT_EQ(model.transaction, coordinator.state());
    EXPECT_EQ(model.transaction == TransactionState::kAutocommit, coordinator.autocommit());
    EXPECT_EQ(model.statement.has_value(), coordinator.statement_active());
    EXPECT_EQ(model.savepoints.size(), coordinator.savepoint_count());
    ASSERT_TRUE(coordinator.pager_state().has_value());
    EXPECT_EQ(model.pager, *coordinator.pager_state());
  }
}

TEST(TransactionCoordinatorBaseline, RecordsImplicitAndExplicitFixedWork) {
  test::WritePagerFixedVfs fixture_vfs{false};
  const ByteBuffer fixture = MakeTransactionFixture(fixture_vfs);

  test::WritePagerFixedVfs implicit_vfs{false};
  implicit_vfs.LoadDatabase(fixture.view());
  TransactionWorkCounters implicit_work;
  {
    TransactionCoordinator coordinator = OpenCoordinator(implicit_vfs);
    for (std::int64_t rowid = 2; rowid <= 4; ++rowid) {
      TransactionStatement statement = TakeValue(coordinator.BeginStatement(
          TransactionStatementOptions{.access = StatementAccess::kWrite}));
      TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
      std::array<std::byte, 8> payload{};
      std::ranges::fill(payload, static_cast<std::byte>(rowid));
      RequireStatus(table.Insert(rowid, payload));
      RequireStatus(statement.Succeed());
    }
    implicit_work = coordinator.work_counters();
  }

  test::WritePagerFixedVfs explicit_vfs{false};
  explicit_vfs.LoadDatabase(fixture.view());
  TransactionWorkCounters explicit_work;
  {
    TransactionCoordinator coordinator = OpenCoordinator(explicit_vfs);
    RequireStatus(coordinator.Begin());
    for (std::int64_t rowid = 2; rowid <= 4; ++rowid) {
      TransactionStatement statement = TakeValue(coordinator.BeginStatement(
          TransactionStatementOptions{.access = StatementAccess::kWrite}));
      TableBtreeWriter table = TakeValue(statement.writer()->OpenTableBtree(PageNumber{1}));
      std::array<std::byte, 8> payload{};
      std::ranges::fill(payload, static_cast<std::byte>(rowid));
      RequireStatus(table.Insert(rowid, payload));
      RequireStatus(statement.Succeed());
    }
    RequireStatus(coordinator.Commit());
    explicit_work = coordinator.work_counters();
  }

  EXPECT_EQ(3U, implicit_work.pager_read_begins);
  EXPECT_EQ(3U, implicit_work.pager_write_begins);
  EXPECT_EQ(0U, implicit_work.pager_savepoint_creates);
  EXPECT_EQ(0U, implicit_work.pager_savepoint_releases);
  EXPECT_EQ(3U, implicit_work.pager_commits);
  EXPECT_EQ(3U, implicit_work.btree_writer_opens);

  EXPECT_EQ(1U, explicit_work.pager_read_begins);
  EXPECT_EQ(1U, explicit_work.pager_write_begins);
  EXPECT_EQ(3U, explicit_work.pager_savepoint_creates);
  EXPECT_EQ(3U, explicit_work.pager_savepoint_releases);
  EXPECT_EQ(1U, explicit_work.pager_commits);
  EXPECT_EQ(1U, explicit_work.btree_writer_opens);

  EXPECT_EQ(3U, implicit_vfs.total_database_writes());
  EXPECT_EQ(3U, implicit_vfs.total_database_syncs());
  EXPECT_EQ(9U, implicit_vfs.total_journal_writes());
  EXPECT_EQ(6U, implicit_vfs.total_journal_syncs());
  EXPECT_EQ(0U, implicit_vfs.total_subjournal_writes());
  EXPECT_EQ(3U, implicit_vfs.total_journal_deletes());

  EXPECT_EQ(1U, explicit_vfs.total_database_writes());
  EXPECT_EQ(1U, explicit_vfs.total_database_syncs());
  EXPECT_EQ(3U, explicit_vfs.total_journal_writes());
  EXPECT_EQ(2U, explicit_vfs.total_journal_syncs());
  EXPECT_EQ(0U, explicit_vfs.total_subjournal_writes());
  EXPECT_EQ(0U, explicit_vfs.pathless_open_count());
  EXPECT_EQ(1U, explicit_vfs.total_journal_deletes());
}

}  // namespace
}  // namespace modern_sqlite
