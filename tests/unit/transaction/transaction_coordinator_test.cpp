#include "modern_sqlite/transaction/transaction_coordinator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

[[nodiscard]] TransactionCoordinator OpenCoordinator(test::WritePagerFixedVfs& vfs,
                                                     std::size_t cache_pages = 64U) {
  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, cache_pages);
  if (pager == nullptr) {
    throw std::runtime_error("failed to open transaction test pager");
  }
  return TakeValue(TransactionCoordinator::Open(std::move(pager)));
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
        } else if (*statement == StatementAccess::kWrite) {
          if (action.kind == ModelActionKind::kRollbackStatement) {
            pager = PagerState::kWriterCacheModified;
          }
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
        if (pager == PagerState::kWriterLocked || pager == PagerState::kWriterCacheModified ||
            pager == PagerState::kWriterDatabaseModified) {
          pager = PagerState::kWriterCacheModified;
        }
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
    stale_index.emplace(TakeValue(statement.writer()->CreateIndexBtree(columns)));
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

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  ASSERT_NE(nullptr, pager->header());
  EXPECT_EQ(1U, pager->header()->schema_cookie());
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

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
    ASSERT_TRUE(TakeValue(cursor.First()));
    EXPECT_EQ(4, TakeValue(cursor.rowid()));
    EXPECT_FALSE(TakeValue(cursor.Next()));
  }
  RequireStatus(pager->EndRead());
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

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
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

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
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

  std::unique_ptr<Pager> pager = test::OpenWritePager(vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  {
    TableBtreeCursor cursor = TakeValue(TableBtreeCursor::Open(*pager, PageNumber{1}));
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
  EXPECT_EQ(2U, explicit_vfs.total_subjournal_writes());
  EXPECT_EQ(1U, explicit_vfs.total_journal_deletes());
}

}  // namespace
}  // namespace modern_sqlite
