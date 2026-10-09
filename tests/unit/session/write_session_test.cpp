#include "modern_sqlite/session/write_session.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/catalog/catalog_loader.hpp"
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

template <typename T>
[[nodiscard]] T TakeOptional(std::optional<T> value, std::string_view message) {
  if (!value.has_value()) {
    throw std::runtime_error{std::string{message}};
  }
  return *value;
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

[[nodiscard]] std::filesystem::path IndexFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "index_performance" / "indexed.db";
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-write-session-test-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path DatabasePath() const { return path_ / "test.sqlite"; }

 private:
  std::filesystem::path path_;
};

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

TEST(WriteSession, AcceptsTemporaryStorageOptionsAndRejectsInvalidConfiguration) {
  const WriteSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
              .sorter_memory_threshold = ByteCount{8192},
          },
  };
  WriteSession session = TakeValue(WriteSession::Open(
      std::make_unique<test::WritePagerFixedVfs>(false), test::kWritePagerDatabasePath, options));
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
  ExecuteDone(session, "INSERT INTO Items VALUES(2,'beta')");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'alpha')");
  ExecuteDone(session, "INSERT INTO Items VALUES(3,'gamma')");
  const auto rows = QueryRows(session, "SELECT id, Name FROM Items ORDER BY Name DESC LIMIT 2");
  ASSERT_EQ(2U, rows.size());
  EXPECT_EQ(3, rows[0][0].integer_value());
  EXPECT_EQ("gamma", Text(rows[0][1]));
  EXPECT_EQ(2, rows[1][0].integer_value());
  EXPECT_EQ("beta", Text(rows[1][1]));

  WriteSessionOptions invalid = options;
  invalid.temporary_storage.sorter_memory_threshold = ByteCount{0};
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.DatabasePath();
  const auto path_rejected = WriteSession::Open(path.string(), invalid);
  ASSERT_FALSE(path_rejected.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, path_rejected.error().code());
  EXPECT_FALSE(std::filesystem::exists(path));

  const auto rejected = WriteSession::Open(std::make_unique<test::WritePagerFixedVfs>(false),
                                           test::kWritePagerDatabasePath, invalid);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, rejected.error().code());
}

TEST(WriteSession, MaintainsIndexesForDmlStatements) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.DatabasePath();
  std::filesystem::copy_file(IndexFixturePath(), path);
  WriteSession session = TakeValue(WriteSession::Open(path.string()));

  ExecuteDone(session,
              "INSERT INTO items(id,category,score,flag,payload) "
              "VALUES(5000,'category-new',42,0,x'00')");
  EXPECT_EQ(1U, session.changes());
  EXPECT_EQ(5000, session.last_insert_rowid());

  const auto category_rows =
      QueryRows(session, "SELECT id,score FROM items WHERE category='category-new'");
  ASSERT_EQ(1U, category_rows.size());
  EXPECT_EQ(5000, category_rows[0][0].integer_value());
  EXPECT_EQ(42, category_rows[0][1].integer_value());

  const auto score_rows = QueryRows(session, "SELECT id FROM items WHERE score=42 AND id=5000");
  ASSERT_EQ(1U, score_rows.size());
  EXPECT_EQ(5000, score_rows[0][0].integer_value());

  const auto flag_rows = QueryRows(session, "SELECT id FROM items WHERE flag=0 AND id=5000");
  ASSERT_EQ(1U, flag_rows.size());
  EXPECT_EQ(5000, flag_rows[0][0].integer_value());

  WriteStatement duplicate = PrepareOne(session,
                                        "INSERT INTO items(id,category,score,flag,payload) "
                                        "VALUES(5000,'category-other',7,1,x'01')");
  const auto duplicate_step = duplicate.Step();
  ASSERT_FALSE(duplicate_step.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate_step.error().code());
  EXPECT_EQ(0U, session.changes());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM items WHERE category='category-other'").empty());

  ExecuteDone(session,
              "UPDATE items "
              "SET id=5001,category='category-updated',score=43,flag=7 "
              "WHERE id=5000");
  EXPECT_EQ(1U, session.changes());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM items WHERE category='category-new'").empty());
  const auto updated_category =
      QueryRows(session, "SELECT id,score FROM items WHERE category='category-updated'");
  ASSERT_EQ(1U, updated_category.size());
  EXPECT_EQ(5001, updated_category[0][0].integer_value());
  EXPECT_EQ(43, updated_category[0][1].integer_value());
  ASSERT_EQ(1U, QueryRows(session, "SELECT id FROM items WHERE score=43 AND id=5001").size());
  ASSERT_EQ(1U, QueryRows(session, "SELECT id FROM items WHERE flag=7 AND id=5001").size());

  ExecuteDone(session, "DELETE FROM items WHERE category='category-updated'");
  EXPECT_EQ(1U, session.changes());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM items WHERE category='category-updated'").empty());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM items WHERE score=43 AND id=5001").empty());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM items WHERE flag=7 AND id=5001").empty());
}

TEST(WriteSession, CreatesPopulatesAndPublishesIndexes) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT, Score REAL)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'alpha',7)");
  ExecuteDone(session, "INSERT INTO Items VALUES(2,'beta',8)");
  ExecuteDone(session, "INSERT INTO Items VALUES(3,NULL,9)");
  ExecuteDone(session, "INSERT INTO Items VALUES(4,NULL,9)");
  WriteStatement prepared = PrepareOne(
      session, "SELECT id,Score FROM Items WHERE Name COLLATE NOCASE='ALPHA' AND Score=7");

  ExecuteDone(session,
              "CREATE UNIQUE INDEX IF NOT EXISTS main.items_name_score "
              "ON Items(Name COLLATE NOCASE DESC,Score)");
  EXPECT_EQ(1U, session.changes());
  EXPECT_EQ(4, session.last_insert_rowid());

  const auto schema = QueryRows(
      session, "SELECT sql FROM sqlite_schema WHERE name='items_name_score' AND type='index'");
  ASSERT_EQ(1U, schema.size());
  EXPECT_EQ("CREATE UNIQUE INDEX items_name_score ON Items(Name COLLATE NOCASE DESC,Score)",
            Text(schema[0][0]));

  ASSERT_EQ(WriteStep::kRow, TakeValue(prepared.Step()));
  ASSERT_EQ(2U, prepared.row().size());
  EXPECT_EQ(1, prepared.row()[0].integer_value());
  EXPECT_EQ(7.0, prepared.row()[1].real_value());
  EXPECT_EQ(WriteStep::kDone, TakeValue(prepared.Step()));

  ExecuteDone(session,
              "CREATE UNIQUE INDEX IF NOT EXISTS items_name_score "
              "ON Items(no_such_column)");
  EXPECT_EQ(1U, session.changes());

  WriteStatement duplicate = PrepareOne(session, "INSERT INTO Items VALUES(5,'ALPHA',7)");
  const auto duplicate_step = duplicate.Step();
  ASSERT_FALSE(duplicate_step.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, duplicate_step.error().code());
  EXPECT_EQ(0U, session.changes());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM Items WHERE id=5").empty());
}

TEST(WriteSession, RollsBackCreatedIndexesAndCatalogs) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'same')");

  ExecuteDone(session, "BEGIN");
  ExecuteDone(session, "CREATE UNIQUE INDEX items_name ON Items(Name)");
  ASSERT_EQ(1U,
            QueryRows(session, "SELECT name FROM sqlite_schema WHERE name='items_name'").size());
  ExecuteDone(session, "ROLLBACK");
  EXPECT_TRUE(QueryRows(session, "SELECT name FROM sqlite_schema WHERE name='items_name'").empty());
  ExecuteDone(session, "INSERT INTO Items VALUES(2,'same')");
  EXPECT_EQ(1U, session.changes());

  ExecuteDone(session, "SAVEPOINT s");
  ExecuteDone(session, "CREATE INDEX items_name ON Items(Name DESC)");
  ASSERT_EQ(1U,
            QueryRows(session, "SELECT name FROM sqlite_schema WHERE name='items_name'").size());
  ExecuteDone(session, "ROLLBACK TO s");
  ExecuteDone(session, "RELEASE s");
  EXPECT_TRUE(QueryRows(session, "SELECT name FROM sqlite_schema WHERE name='items_name'").empty());
}

TEST(WriteSession, RollsBackFailedUniqueIndexPopulation) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'same')");
  ExecuteDone(session, "INSERT INTO Items VALUES(2,'same')");

  WriteStatement unique = PrepareOne(session, "CREATE UNIQUE INDEX items_name ON Items(Name)");
  const auto failed = unique.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, failed.error().code());
  EXPECT_EQ(1U, session.changes());
  EXPECT_EQ(2, session.last_insert_rowid());
  EXPECT_TRUE(QueryRows(session, "SELECT name FROM sqlite_schema WHERE name='items_name'").empty());
  EXPECT_EQ(2U, QueryRows(session, "SELECT id FROM Items").size());

  ExecuteDone(session, "CREATE INDEX items_name ON Items(Name)");
  EXPECT_EQ(1U,
            QueryRows(session, "SELECT name FROM sqlite_schema WHERE name='items_name'").size());
}

TEST(WriteSession, AnalyzesIndexPrefixesAndPublishesStatistics) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT, Score INT)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'a',1)");
  ExecuteDone(session, "INSERT INTO Items VALUES(2,'A',2)");
  ExecuteDone(session, "INSERT INTO Items VALUES(3,'b',3)");
  ExecuteDone(session, "INSERT INTO Items VALUES(4,NULL,4)");
  ExecuteDone(session, "INSERT INTO Items VALUES(5,NULL,5)");
  ExecuteDone(session, "CREATE INDEX items_name_score ON Items(Name COLLATE NOCASE DESC,Score)");

  ExecuteDone(session, "ANALYZE items_name_score");
  EXPECT_EQ(1U, session.changes());
  const auto rows =
      QueryRows(session, "SELECT tbl,idx,stat FROM sqlite_stat1 WHERE idx='items_name_score'");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("Items", Text(rows[0][0]));
  EXPECT_EQ("items_name_score", Text(rows[0][1]));
  EXPECT_EQ("5 2 1", Text(rows[0][2]));

  ExecuteDone(session, "CREATE INDEX items_score ON Items(Score DESC)");
  ExecuteDone(session, "INSERT INTO Items VALUES(6,'b',4)");
  ExecuteDone(session, "ANALYZE Items");
  const auto table_rows = QueryRows(session, "SELECT idx,stat FROM sqlite_stat1 WHERE tbl='Items'");
  ASSERT_EQ(2U, table_rows.size());
  EXPECT_EQ("items_name_score", Text(table_rows[0][0]));
  EXPECT_EQ("6 2 1", Text(table_rows[0][1]));
  EXPECT_EQ("items_score", Text(table_rows[1][0]));
  EXPECT_EQ("6 2", Text(table_rows[1][1]));

  ExecuteDone(session, "CREATE TABLE Plain(id INTEGER PRIMARY KEY, Value TEXT)");
  ExecuteDone(session, "CREATE TABLE Empty(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "CREATE INDEX empty_id ON Empty(id)");
  ExecuteDone(session, "INSERT INTO Plain VALUES(1,'one')");
  ExecuteDone(session, "INSERT INTO Plain VALUES(2,'two')");
  ExecuteDone(session, "INSERT INTO Plain VALUES(3,'three')");
  ExecuteDone(session, "ANALYZE main");
  const auto plain = QueryRows(session, "SELECT idx,stat FROM sqlite_stat1 WHERE tbl='Plain'");
  ASSERT_EQ(1U, plain.size());
  EXPECT_EQ(SqlValueType::kNull, plain[0][0].type());
  EXPECT_EQ("3", Text(plain[0][1]));
  EXPECT_TRUE(QueryRows(session, "SELECT stat FROM sqlite_stat1 WHERE tbl='Empty'").empty());

  ExecuteDone(session, "DELETE FROM Items WHERE id=6");
  ExecuteDone(session, "ANALYZE main.items_score");
  const auto score = QueryRows(session, "SELECT stat FROM sqlite_stat1 WHERE idx='items_score'");
  ASSERT_EQ(1U, score.size());
  EXPECT_EQ("5 1", Text(score[0][0]));

  std::unique_ptr<Pager> pager = test::OpenWritePager(*fixture.vfs, 64U);
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager, CatalogLoadOptions{.generation = 101}));
  const IndexId name_index =
      TakeOptional(catalog->FindIndex("items_name_score"), "missing analyzed name index");
  const IndexId score_index =
      TakeOptional(catalog->FindIndex("items_score"), "missing analyzed score index");
  EXPECT_EQ((std::vector<std::uint64_t>{6, 2, 1}),
            catalog->index(name_index).statistics.rows_per_prefix);
  EXPECT_EQ((std::vector<std::uint64_t>{5, 1}),
            catalog->index(score_index).statistics.rows_per_prefix);
  const TableId plain_table =
      TakeOptional(catalog->FindTable("Plain"), "missing analyzed plain table");
  EXPECT_EQ(std::optional<std::uint64_t>{3}, catalog->table(plain_table).statistics.estimated_rows);
  RequireStatus(pager->EndRead());
}

TEST(WriteSession, NormalizesNearUniqueAnalyzeEstimate) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Value INT)");
  for (std::int64_t rowid = 1; rowid <= 11; ++rowid) {
    const std::int64_t value = rowid == 11 ? 10 : rowid;
    ExecuteDone(session, "INSERT INTO Items VALUES(" + std::to_string(rowid) + "," +
                             std::to_string(value) + ")");
  }
  ExecuteDone(session, "CREATE INDEX items_value ON Items(Value)");
  ExecuteDone(session, "ANALYZE items_value");
  const auto rows = QueryRows(session, "SELECT stat FROM sqlite_stat1 WHERE idx='items_value'");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("11 1", Text(rows[0][0]));
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

TEST(WriteSession, RollsBackAbandonedTransactionWhenTheFinalOwnerIsDestroyed) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.DatabasePath();
  {
    WriteSession session = TakeValue(WriteSession::Open(path.string()));
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "BEGIN");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    EXPECT_FALSE(session.autocommit());
  }

  WriteSession reopened = TakeValue(WriteSession::Open(path.string()));
  EXPECT_TRUE(reopened.autocommit());
  EXPECT_TRUE(QueryRows(reopened, "SELECT id FROM Items").empty());
}

}  // namespace
}  // namespace modern_sqlite
