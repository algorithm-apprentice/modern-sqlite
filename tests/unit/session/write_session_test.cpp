#include "modern_sqlite/session/write_session.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
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

[[nodiscard]] WriteStatement PrepareOne(WriteSession& session, std::string_view sql) {
  WritePrepareOutput prepared = TakeValue(session.Prepare(Utf8View{sql}));
  if (!prepared.statement.has_value() || prepared.next_offset.value() != sql.size()) {
    throw std::runtime_error{"write-session test failed to prepare exactly one statement"};
  }
  return std::move(*prepared.statement);
}

void ExecuteDone(WriteSession& session, std::string_view sql) {
  WriteStatement statement = PrepareOne(session, sql);
  EXPECT_EQ(WriteStep::kDone, TakeValue(statement.Step()));
}

[[nodiscard]] std::vector<std::vector<SqlValue>> QueryRows(WriteSession& session,
                                                           std::string_view sql) {
  WriteStatement statement = PrepareOne(session, sql);
  std::vector<std::vector<SqlValue>> rows;
  while (true) {
    const WriteStep step = TakeValue(statement.Step());
    if (step == WriteStep::kDone) {
      return rows;
    }
    std::vector<SqlValue> row;
    row.reserve(statement.row().size());
    for (const SqlValue& value : statement.row()) {
      row.push_back(value.Clone());
    }
    rows.push_back(std::move(row));
  }
}

[[nodiscard]] std::string_view Text(const SqlValue& value) {
  const std::optional<Utf8View> text = value.text_value();
  if (!text.has_value()) {
    throw std::runtime_error{"expected text value"};
  }
  return text->bytes();
}

struct SessionFixture {
  SessionFixture() {
    auto owned = std::make_unique<test::WritePagerFixedVfs>(false);
    vfs = owned.get();
    session.emplace(TakeValue(WriteSession::Open(std::move(owned), test::kWritePagerDatabasePath)));
  }

  [[nodiscard]] WriteSession& Get() {
    if (!session.has_value()) {
      throw std::runtime_error{"write session fixture is uninitialized"};
    }
    return *session;
  }

  test::WritePagerFixedVfs* vfs = nullptr;
  std::optional<WriteSession> session;
};

TEST(WriteSession, ExecutesWritablePipelineAndPublishesConnectionState) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  EXPECT_TRUE(session.autocommit());
  EXPECT_EQ(0U, session.changes());
  EXPECT_EQ(0, session.last_insert_rowid());

  ExecuteDone(session,
              "CREATE TABLE Items("
              "id INTEGER PRIMARY KEY, "
              "Name TEXT NOT NULL DEFAULT 'seed', "
              "Score REAL"
              ")");
  EXPECT_EQ(0U, session.changes());

  WriteStatement insert = PrepareOne(session, "INSERT INTO Items(Name,Score) VALUES(?1,?2)");
  EXPECT_EQ(2U, insert.parameter_count());
  RequireStatus(insert.Bind(1, SqlValue::Text("alpha")));
  RequireStatus(insert.Bind(2, SqlValue::Integer(7)));
  EXPECT_EQ(WriteStep::kDone, TakeValue(insert.Step()));
  EXPECT_EQ(1U, session.changes());
  EXPECT_EQ(1, session.last_insert_rowid());

  WriteStatement duplicate =
      PrepareOne(session, "INSERT INTO Items(id,Name) VALUES(1,'duplicate')");
  const auto duplicate_result = duplicate.Step();
  ASSERT_FALSE(duplicate_result.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate_result.error().code());
  EXPECT_EQ(0U, session.changes());
  EXPECT_EQ(1, session.last_insert_rowid());
  const Status duplicate_finalize = duplicate.Finalize();
  ASSERT_FALSE(duplicate_finalize.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate_finalize.error().code());

  auto rows = QueryRows(session, "SELECT id,Name,Score FROM Items");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ("alpha", Text(rows[0][1]));
  EXPECT_EQ(7.0, rows[0][2].real_value());

  ExecuteDone(session, "UPDATE Items SET Name='updated' WHERE id=1");
  EXPECT_EQ(1U, session.changes());
  rows = QueryRows(session, "SELECT Name FROM Items WHERE id=1");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("updated", Text(rows[0][0]));

  ExecuteDone(session, "DELETE FROM Items WHERE id=1");
  EXPECT_EQ(1U, session.changes());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM Items").empty());

  ExecuteDone(session, "CREATE TABLE IF NOT EXISTS Items(a UNIQUE)");
  EXPECT_EQ(1U, session.changes());
}

TEST(WriteSession, MapsTransactionsSavepointsAndRollbackVisibility) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");

  ExecuteDone(session, "BEGIN");
  ExecuteDone(session, "INSERT INTO Items VALUES(9,'kept')");
  WriteStatement duplicate = PrepareOne(session, "INSERT INTO Items VALUES(9,'duplicate')");
  const auto duplicate_result = duplicate.Step();
  ASSERT_FALSE(duplicate_result.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate_result.error().code());
  EXPECT_EQ(0U, session.changes());
  EXPECT_FALSE(session.autocommit());
  auto retained = QueryRows(session, "SELECT Name FROM Items WHERE id=9");
  ASSERT_EQ(1U, retained.size());
  EXPECT_EQ("kept", Text(retained[0][0]));
  ExecuteDone(session, "COMMIT");
  EXPECT_TRUE(session.autocommit());
  ExecuteDone(session, "DELETE FROM Items WHERE id=9");

  ExecuteDone(session, "BEGIN IMMEDIATE");
  EXPECT_FALSE(session.autocommit());
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'rolled back')");
  EXPECT_EQ(1, session.last_insert_rowid());
  ExecuteDone(session, "ROLLBACK");
  EXPECT_TRUE(session.autocommit());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM Items").empty());
  EXPECT_EQ(1U, session.changes());
  EXPECT_EQ(1, session.last_insert_rowid());

  ExecuteDone(session, "SAVEPOINT outer");
  EXPECT_FALSE(session.autocommit());
  ExecuteDone(session, "INSERT INTO Items VALUES(2,'savepoint')");
  ExecuteDone(session, "ROLLBACK TO outer");
  ExecuteDone(session, "RELEASE outer");
  EXPECT_TRUE(session.autocommit());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM Items").empty());

  WriteStatement invalid_commit = PrepareOne(session, "COMMIT");
  const auto failed = invalid_commit.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
}

TEST(WriteSession, EnforcesStatementLifecycleResetAndBusyRules) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'one')");
  ExecuteDone(session, "INSERT INTO Items VALUES(2,'two')");

  WriteStatement select = PrepareOne(session, "SELECT id FROM Items");
  EXPECT_EQ(WriteStep::kRow, TakeValue(select.Step()));
  WriteStatement blocked = PrepareOne(session, "DELETE FROM Items WHERE id=2");
  const auto busy = blocked.Step();
  ASSERT_FALSE(busy.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy.error().code());
  RequireStatus(select.Reset());
  EXPECT_EQ(WriteStep::kDone, TakeValue(blocked.Step()));

  WriteStatement reusable = PrepareOne(session, "INSERT INTO Items VALUES(?1,?2)");
  RequireStatus(reusable.Bind(1, SqlValue::Integer(3)));
  RequireStatus(reusable.Bind(2, SqlValue::Text("three")));
  EXPECT_EQ(WriteStep::kDone, TakeValue(reusable.Step()));
  RequireStatus(reusable.Reset());
  RequireStatus(reusable.Bind(1, SqlValue::Integer(4)));
  RequireStatus(reusable.Bind(2, SqlValue::Text("four")));
  EXPECT_EQ(WriteStep::kDone, TakeValue(reusable.Step()));

  const auto rows = QueryRows(session, "SELECT id FROM Items");
  ASSERT_EQ(3U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(3, rows[1][0].integer_value());
  EXPECT_EQ(4, rows[2][0].integer_value());
}

TEST(WriteSession, RepreparesAcrossSchemaChangesAndReloadsRolledBackCatalogs) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'one')");
  WriteStatement prepared = PrepareOne(session, "SELECT Name FROM Items WHERE id=1");

  ExecuteDone(session, "BEGIN");
  ExecuteDone(session, "CREATE TABLE Temp(id INTEGER PRIMARY KEY)");
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM Temp").empty());
  EXPECT_EQ(WriteStep::kRow, TakeValue(prepared.Step()));
  EXPECT_EQ("one", Text(prepared.row()[0]));
  EXPECT_EQ(WriteStep::kDone, TakeValue(prepared.Step()));
  ExecuteDone(session, "ROLLBACK");

  const auto rows = QueryRows(session, "SELECT Name FROM Items WHERE id=1");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("one", Text(rows[0][0]));
  const auto missing = session.Prepare(Utf8View{"SELECT id FROM Temp"});
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, missing.error().code());
}

TEST(WriteSession, PreparedStatementsRetainZombieSessionState) {
  std::optional<WriteStatement> survivor;
  {
    auto vfs = std::make_unique<test::WritePagerFixedVfs>(false);
    WriteSession session =
        TakeValue(WriteSession::Open(std::move(vfs), test::kWritePagerDatabasePath));
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1,'one')");
    survivor.emplace(PrepareOne(session, "SELECT Name FROM Items"));
  }

  ASSERT_TRUE(survivor.has_value());
  EXPECT_EQ(WriteStep::kRow, TakeValue(survivor->Step()));
  EXPECT_EQ("one", Text(survivor->row()[0]));
  EXPECT_EQ(WriteStep::kDone, TakeValue(survivor->Step()));
  RequireStatus(survivor->Finalize());
}

TEST(WriteSession, RetriesTransactionControlCleanupBeforeCatalogRefresh) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "BEGIN");
  ExecuteDone(session, "INSERT INTO Items VALUES(1)");

  WriteStatement commit = PrepareOne(session, "COMMIT");
  fixture.vfs->FailNextDatabaseUnlock(DatabaseLock::kNone, ErrorCode::kIo);
  const auto failed = commit.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_EQ(WriteStep::kDone, TakeValue(commit.Step()));
  EXPECT_TRUE(session.autocommit());

  const auto rows = QueryRows(session, "SELECT id FROM Items");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
}

}  // namespace
}  // namespace modern_sqlite
