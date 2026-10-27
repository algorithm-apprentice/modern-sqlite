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
#include "modern_sqlite/syntax/ast.hpp"
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
  explicit SessionFixture(WriteSessionOptions options = {}) {
    auto owned = std::make_unique<test::WritePagerFixedVfs>(false);
    vfs = owned.get();
    session.emplace(
        TakeValue(WriteSession::Open(std::move(owned), test::kWritePagerDatabasePath, options)));
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

TEST(WriteSession, PreservesNativeExpressionConstructorErrorCodesWithoutRelaxingLimits) {
  SessionFixture fixture;
  std::string depth = "SELECT 1";
  for (std::size_t index = 0; index < kMaximumExpressionConstructionDepth; ++index) {
    depth += "+1";
  }
  const auto rejected_depth = fixture.Get().Prepare(Utf8View{depth});
  ASSERT_FALSE(rejected_depth.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, rejected_depth.error().code());
}

TEST(WriteSession, ExecutesNativeInListsInMutationsAndPreservesStatementRollback) {
  for (const auto mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<unsigned>(mode));
    SessionFixture fixture{
        WriteSessionOptions{.temporary_storage = TemporaryStorageOptions{.mode = mode}}};
    auto& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE t(id INTEGER PRIMARY KEY,v INTEGER)");
    ExecuteDone(session, "INSERT INTO t VALUES(1,2 IN (1,2,3))");
    ExecuteDone(session, "INSERT INTO t VALUES(2,5 IN (1,2,3))");
    ExecuteDone(session, "INSERT INTO t VALUES(3,NULL IN (1,2,3))");
    ExecuteDone(session, "UPDATE t SET v=id IN (1,2,3) WHERE id IN (1,3,9)");
    ExecuteDone(session, "DELETE FROM t WHERE id IN (2,4,6)");
    {
      auto failing = PrepareOne(
          session, "UPDATE t SET v=10+(id IN (1,iif(id=3,abs(-9223372036854775808),0),99))");
      const auto result = failing.Step();
      ASSERT_FALSE(result.has_value());
      EXPECT_EQ(ErrorCode::kGeneric, result.error().code());
    }
    const auto rows = QueryRows(session, "SELECT id,v FROM t ORDER BY id");
    ASSERT_EQ(2U, rows.size());
    EXPECT_EQ(1, rows[0][0].integer_value());
    EXPECT_EQ(1, rows[0][1].integer_value());
    EXPECT_EQ(3, rows[1][0].integer_value());
    EXPECT_EQ(1, rows[1][1].integer_value());
    EXPECT_GT(fixture.vfs->total_database_syncs(), 0U);
    EXPECT_GT(fixture.vfs->total_journal_syncs(), 0U);
  }
}

TEST(WriteSession, ExecutesUncorrelatedMutationQueriesThroughSharedReadProducers) {
  for (const auto mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<unsigned>(mode));
    SessionFixture fixture{
        WriteSessionOptions{.temporary_storage = TemporaryStorageOptions{.mode = mode}}};
    auto& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE u(id INTEGER PRIMARY KEY,v INTEGER)");
    ExecuteDone(session, "INSERT INTO u VALUES(1,10)");
    ExecuteDone(session, "INSERT INTO u VALUES(2,20)");
    ExecuteDone(session, "INSERT INTO u VALUES(3,30)");
    ExecuteDone(session, "INSERT INTO u VALUES(4,(SELECT max(v) FROM u))");
    ExecuteDone(session, "UPDATE u SET v=(SELECT max(v) FROM u)+id");
    ExecuteDone(session, "UPDATE u SET v=v+100 WHERE (SELECT count(*) FROM u)=4");
    {
      const auto rows = QueryRows(session, "SELECT id,v FROM u ORDER BY id");
      ASSERT_EQ(4U, rows.size());
      for (std::size_t index = 0; index < rows.size(); ++index) {
        EXPECT_EQ(static_cast<std::int64_t>(index + 1U), rows[index][0].integer_value());
        EXPECT_EQ(static_cast<std::int64_t>(index + 131U), rows[index][1].integer_value());
      }
    }
    ExecuteDone(session, "DELETE FROM u WHERE id<=(SELECT max(id)-2 FROM u)");
    ExecuteDone(session,
                "DELETE FROM u WHERE id=4 AND EXISTS("
                "SELECT abs(-9223372036854775808) FROM u)");
    const auto rows = QueryRows(session, "SELECT id,v FROM u ORDER BY id");
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ(3, rows[0][0].integer_value());
    EXPECT_EQ(133, rows[0][1].integer_value());
    EXPECT_TRUE(session.autocommit());
    EXPECT_GT(fixture.vfs->total_database_syncs(), 0U);
    EXPECT_GT(fixture.vfs->total_journal_syncs(), 0U);
  }
}

TEST(WriteSession, ResetsSharedMutationQueryCachesAndBindsEveryQueryParameter) {
  for (const auto mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<unsigned>(mode));
    SessionFixture fixture{
        WriteSessionOptions{.temporary_storage = TemporaryStorageOptions{.mode = mode}}};
    auto& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE u(id INTEGER PRIMARY KEY,v INTEGER)");
    ExecuteDone(session, "INSERT INTO u VALUES(1,10)");
    ExecuteDone(session, "INSERT INTO u VALUES(2,20)");
    auto statement = PrepareOne(session, "UPDATE u SET v=(SELECT ?1)+(SELECT count(*) FROM u)");
    EXPECT_EQ(1U, statement.parameter_count());
    RequireStatus(statement.Bind(1, SqlValue::Integer(10)));
    EXPECT_EQ(WriteStep::kDone, TakeValue(statement.Step()));
    EXPECT_EQ(2U, session.changes());
    RequireStatus(statement.Reset());
    ExecuteDone(session, "INSERT INTO u VALUES(3,30)");
    RequireStatus(statement.Bind(1, SqlValue::Integer(20)));
    EXPECT_EQ(WriteStep::kDone, TakeValue(statement.Step()));
    EXPECT_EQ(3U, session.changes());
    RequireStatus(statement.Finalize());
    const auto updated = QueryRows(session, "SELECT v FROM u ORDER BY id");
    ASSERT_EQ(3U, updated.size());
    for (const auto& row : updated) {
      EXPECT_EQ(23, row[0].integer_value());
    }
    ExecuteDone(session, "INSERT INTO u VALUES(4,(SELECT v FROM u WHERE 0))");
    const auto empty = QueryRows(session, "SELECT v FROM u WHERE id=4");
    ASSERT_EQ(1U, empty.size());
    EXPECT_EQ(SqlValueType::kNull, empty[0][0].type());
    EXPECT_GT(fixture.vfs->total_database_syncs(), 0U);
    EXPECT_GT(fixture.vfs->total_journal_syncs(), 0U);
  }
}

TEST(WriteSession, RollsBackLatePrivateQueryErrorsAndRetainsEarlierExplicitWrites) {
  for (const auto mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    for (const bool explicit_transaction : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(mode));
      SCOPED_TRACE(explicit_transaction);
      SessionFixture fixture{
          WriteSessionOptions{.temporary_storage = TemporaryStorageOptions{.mode = mode}}};
      auto& session = fixture.Get();
      ExecuteDone(session, "CREATE TABLE u(id INTEGER PRIMARY KEY,v INTEGER)");
      ExecuteDone(session, "INSERT INTO u VALUES(1,10)");
      ExecuteDone(session, "INSERT INTO u VALUES(2,20)");
      ExecuteDone(session, "INSERT INTO u VALUES(3,30)");
      if (explicit_transaction) {
        ExecuteDone(session, "BEGIN");
        ExecuteDone(session, "INSERT INTO u VALUES(9,90)");
      }
      auto failing =
          PrepareOne(session, "UPDATE u SET v=iif(id=3,(SELECT abs(-9223372036854775808)),v+1)");
      const auto failure = failing.Step();
      ASSERT_FALSE(failure.has_value());
      EXPECT_EQ(ErrorCode::kGeneric, failure.error().code());
      EXPECT_EQ(!explicit_transaction, session.autocommit());
      EXPECT_EQ(0U, session.changes());
      const auto rows = QueryRows(session, "SELECT id,v FROM u ORDER BY id");
      ASSERT_EQ(explicit_transaction ? 4U : 3U, rows.size());
      for (const auto& row : rows) {
        ASSERT_TRUE(row[0].integer_value().has_value());
        EXPECT_EQ(*row[0].integer_value() * 10, row[1].integer_value());
      }
      const auto reset = failing.Reset();
      ASSERT_FALSE(reset.has_value());
      EXPECT_EQ(ErrorCode::kGeneric, reset.error().code());
      if (explicit_transaction) {
        ExecuteDone(session, "ROLLBACK");
      }
      EXPECT_TRUE(session.autocommit());
      EXPECT_EQ(3U, QueryRows(session, "SELECT id FROM u").size());
      EXPECT_GT(fixture.vfs->total_database_syncs(), 0U);
      EXPECT_GT(fixture.vfs->total_journal_syncs(), 0U);
    }
  }
}

TEST(WriteSession, ReusesNestedLocalCapturesAggregatesAndCompoundQueryProducersInMutations) {
  for (const auto mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<unsigned>(mode));
    SessionFixture fixture{
        WriteSessionOptions{.temporary_storage = TemporaryStorageOptions{.mode = mode}}};
    auto& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE u(id INTEGER PRIMARY KEY,v INTEGER)");
    ExecuteDone(session, "INSERT INTO u VALUES(1,10)");
    ExecuteDone(session, "INSERT INTO u VALUES(2,20)");
    ExecuteDone(session, "INSERT INTO u VALUES(3,30)");
    ExecuteDone(session, "UPDATE u SET v=(SELECT (SELECT q.v) FROM u q ORDER BY q.id DESC)");
    ExecuteDone(session,
                "UPDATE u SET v=(SELECT sum(a.id) FROM u a JOIN u b ON b.id=a.id "
                "GROUP BY a.v ORDER BY sum(a.id) DESC LIMIT 1)");
    {
      const auto grouped = QueryRows(session, "SELECT v FROM u ORDER BY id");
      ASSERT_EQ(3U, grouped.size());
      for (const auto& row : grouped) {
        EXPECT_EQ(6, row[0].integer_value());
      }
    }
    ExecuteDone(session, "UPDATE u SET v=(SELECT 2 UNION SELECT 9 ORDER BY 1 DESC LIMIT 1)");
    ExecuteDone(session, "INSERT INTO u VALUES(4,(VALUES(7),(8)))");
    ExecuteDone(session,
                "DELETE FROM u WHERE id=2 AND EXISTS("
                "SELECT abs(-9223372036854775808) FROM u ORDER BY abs(-9223372036854775808))");
    const auto rows = QueryRows(session, "SELECT id,v FROM u ORDER BY id");
    ASSERT_EQ(3U, rows.size());
    EXPECT_EQ(1, rows[0][0].integer_value());
    EXPECT_EQ(9, rows[0][1].integer_value());
    EXPECT_EQ(3, rows[1][0].integer_value());
    EXPECT_EQ(9, rows[1][1].integer_value());
    EXPECT_EQ(4, rows[2][0].integer_value());
    EXPECT_EQ(7, rows[2][1].integer_value());
    EXPECT_GT(fixture.vfs->total_database_syncs(), 0U);
    EXPECT_GT(fixture.vfs->total_journal_syncs(), 0U);
  }
}

TEST(WriteSession, RejectsMutationTargetCapturesWithoutPublishingPartialWrites) {
  SessionFixture fixture;
  auto& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE u(id INTEGER PRIMARY KEY,v INTEGER)");
  ExecuteDone(session, "INSERT INTO u VALUES(1,10)");
  for (const std::string_view sql : {
           "UPDATE u SET v=(SELECT u.v)",
           "DELETE FROM u WHERE EXISTS(SELECT u.v FROM u inner_q WHERE 0)",
           "UPDATE u SET v=(SELECT 1,2)",
           "UPDATE u SET v=(SELECT sum(u.v))",
       }) {
    SCOPED_TRACE(sql);
    const auto rejected = session.Prepare(Utf8View{sql});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, rejected.error().code());
    EXPECT_TRUE(session.autocommit());
  }
  const auto rows = QueryRows(session, "SELECT id,v FROM u");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(10, rows[0][1].integer_value());
}

TEST(WriteSession, ChecksAllUpdateNamesButEvaluatesOnlyRightmostValues) {
  for (const auto mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    SCOPED_TRACE(static_cast<unsigned>(mode));
    SessionFixture fixture{
        WriteSessionOptions{.temporary_storage = TemporaryStorageOptions{.mode = mode}}};
    auto& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE u(id INTEGER PRIMARY KEY,v INTEGER)");
    ExecuteDone(session, "INSERT INTO u VALUES(1,10)");
    ExecuteDone(session, "INSERT INTO u VALUES(2,20)");
    for (const std::string_view sql : {
             "UPDATE u SET v=(SELECT abs(-9223372036854775808)),v=7",
             "UPDATE u SET v=abs(-9223372036854775808),v=7",
             "UPDATE u SET rowid=(SELECT abs(-9223372036854775808)),id=id",
             "UPDATE u SET v=(SELECT 1,2),v=7",
             "UPDATE u SET v=(SELECT 1 FROM u ORDER BY v COLLATE missing),v=7",
             "UPDATE u SET v=('a' COLLATE missing)='A',v=7",
         }) {
      SCOPED_TRACE(sql);
      ExecuteDone(session, sql);
    }
    for (const std::string_view sql : {
             "UPDATE u SET v=(SELECT missing_rhs FROM u),v=7",
             "UPDATE u SET v=(SELECT abs()),v=7",
             "UPDATE u SET v=(SELECT 1 UNION SELECT 1,2),v=7",
             "UPDATE u SET v=(SELECT 1,2) WHERE 0",
             "UPDATE u SET v=('a' COLLATE missing)='A' WHERE 0",
         }) {
      SCOPED_TRACE(sql);
      const auto rejected = session.Prepare(Utf8View{sql});
      ASSERT_FALSE(rejected.has_value());
      EXPECT_EQ(ErrorCode::kGeneric, rejected.error().code());
    }
    const auto missing =
        session.Prepare(Utf8View{"UPDATE u SET missing_target=(SELECT missing_rhs),v=7"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(std::string::npos, missing.error().ToString().find("missing_rhs"));
    const auto rows = QueryRows(session, "SELECT id,v FROM u ORDER BY id");
    ASSERT_EQ(2U, rows.size());
    EXPECT_EQ(1, rows[0][0].integer_value());
    EXPECT_EQ(2, rows[1][0].integer_value());
    EXPECT_EQ(7, rows[0][1].integer_value());
    EXPECT_EQ(7, rows[1][1].integer_value());
    EXPECT_GT(fixture.vfs->total_database_syncs(), 0U);
    EXPECT_GT(fixture.vfs->total_journal_syncs(), 0U);
  }
}

TEST(WriteSession, ChecksNewRowidsBeforeEvaluatingOrdinaryUpdateQueries) {
  SessionFixture fixture;
  auto& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE u(id INTEGER PRIMARY KEY,v INTEGER)");
  ExecuteDone(session, "INSERT INTO u VALUES(1,10)");
  struct Case {
    std::string_view sql;
    ErrorCode expected;
  };
  for (const Case test : {
           Case{"UPDATE u SET v=(SELECT 1 LIMIT 'x'),id=(SELECT abs(-9223372036854775808))",
                ErrorCode::kGeneric},
           Case{"UPDATE u SET v=(SELECT abs(-9223372036854775808)),id=(SELECT 'x')",
                ErrorCode::kTypeMismatch},
       }) {
    SCOPED_TRACE(test.sql);
    {
      auto statement = PrepareOne(session, test.sql);
      const auto rejected = statement.Step();
      ASSERT_FALSE(rejected.has_value());
      EXPECT_EQ(test.expected, rejected.error().code());
    }
    EXPECT_TRUE(session.autocommit());
  }
  const auto rows = QueryRows(session, "SELECT id,v FROM u");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ(1, rows[0][0].integer_value());
  EXPECT_EQ(10, rows[0][1].integer_value());
  EXPECT_GT(fixture.vfs->total_database_syncs(), 0U);
  EXPECT_GT(fixture.vfs->total_journal_syncs(), 0U);
}

TEST(WriteSession, ReportsAggregateMisuseTransitivelyDuringUpdateNameOnlyBinding) {
  SessionFixture fixture;
  auto& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE u(id INTEGER PRIMARY KEY,v INTEGER)");
  for (const std::string_view sql : {
           "UPDATE u SET missing_target=sum((SELECT 1,2))",
           "UPDATE u SET v=sum((SELECT 1,2)),v=7",
           "UPDATE u SET v=(SELECT 1,2)+sum((SELECT 3,4))",
       }) {
    SCOPED_TRACE(sql);
    const auto rejected = session.Prepare(Utf8View{sql});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, rejected.error().code());
    EXPECT_NE(std::string::npos,
              rejected.error().ToString().find("misuse of aggregate function sum()"));
    EXPECT_TRUE(session.autocommit());
  }
}

TEST(WriteSession, UsesConfiguredAutomaticLookupAndReleasesItAcrossResetAndFinalize) {
  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kFile, TemporaryStoreMode::kMemory}) {
    SCOPED_TRACE(static_cast<unsigned>(mode));
    SessionFixture fixture{
        WriteSessionOptions{
            .temporary_storage =
                TemporaryStorageOptions{
                    .mode = mode,
                    .sorter_memory_threshold =
                        mode == TemporaryStoreMode::kFile ? ByteCount{1024} : ByteCount{1},
                },
        },
    };
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE p(id INTEGER PRIMARY KEY,k TEXT)");
    ExecuteDone(session,
                "CREATE TABLE r(id INTEGER PRIMARY KEY,k TEXT,payload TEXT,v REAL DEFAULT 2)");
    ExecuteDone(session, "INSERT INTO p VALUES(1,'A')");
    ExecuteDone(session, "INSERT INTO p VALUES(2,'missing')");
    ExecuteDone(session, "INSERT INTO p VALUES(3,NULL)");
    ExecuteDone(session, "INSERT INTO r(id,k,payload) VALUES(10,'a','first')");
    ExecuteDone(session, "INSERT INTO r(id,k,payload) VALUES(11,'A','second')");
    ExecuteDone(session, "INSERT INTO r(id,k,payload) VALUES(12,NULL,'null-key')");
    ASSERT_FALSE(fixture.vfs->pathless_file_present());
    WriteStatement statement = PrepareOne(session,
                                          "SELECT p.id,r.id,r.payload,r.v FROM p LEFT JOIN r "
                                          "ON r.k=p.k COLLATE NOCASE WHERE p.id<=?1");
    RequireStatus(statement.Bind(1, SqlValue::Integer(3)));
    ASSERT_EQ(WriteStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(1, statement.row()[0].integer_value());
    EXPECT_EQ(10, statement.row()[1].integer_value());
    EXPECT_EQ("first", Text(statement.row()[2]));
    EXPECT_EQ(SqlValueType::kReal, statement.row()[3].type());
    EXPECT_EQ(2.0, statement.row()[3].real_value());
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, fixture.vfs->pathless_file_present());
    const SqlValue retained = statement.row()[2].Clone();
    ASSERT_EQ(WriteStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(11, statement.row()[1].integer_value());
    EXPECT_EQ("second", Text(statement.row()[2]));
    for (const std::int64_t id : {2, 3}) {
      ASSERT_EQ(WriteStep::kRow, TakeValue(statement.Step()));
      EXPECT_EQ(id, statement.row()[0].integer_value());
      for (std::size_t field = 1; field < statement.row().size(); ++field) {
        EXPECT_EQ(SqlValueType::kNull, statement.row()[field].type());
      }
    }
    EXPECT_EQ(WriteStep::kDone, TakeValue(statement.Step()));
    EXPECT_FALSE(fixture.vfs->pathless_file_present());
    EXPECT_EQ("first", Text(retained));
    RequireStatus(statement.Reset());
    RequireStatus(statement.Bind(1, SqlValue::Integer(1)));
    ASSERT_EQ(WriteStep::kRow, TakeValue(statement.Step()));
    EXPECT_EQ(10, statement.row()[1].integer_value());
    RequireStatus(statement.Reset());
    EXPECT_FALSE(fixture.vfs->pathless_file_present());
    RequireStatus(statement.Bind(1, SqlValue::Integer(0)));
    EXPECT_EQ(WriteStep::kDone, TakeValue(statement.Step()));
    EXPECT_FALSE(fixture.vfs->pathless_file_present());
    RequireStatus(statement.Finalize());

    const auto unmatched = QueryRows(session,
                                     "SELECT p.id FROM p LEFT JOIN r ON r.k=p.k COLLATE NOCASE "
                                     "WHERE r.id IS NULL OR r.id<0");
    ASSERT_EQ(2U, unmatched.size());
    EXPECT_EQ(2, unmatched[0][0].integer_value());
    EXPECT_EQ(3, unmatched[1][0].integer_value());
    const auto nulls = QueryRows(
        session, "SELECT count(r.id),sum(r.v) FROM p LEFT JOIN r ON r.k IS p.k COLLATE NOCASE");
    ASSERT_EQ(1U, nulls.size());
    EXPECT_EQ(3, nulls[0][0].integer_value());
    EXPECT_EQ(SqlValueType::kReal, nulls[0][1].type());
    EXPECT_EQ(6.0, nulls[0][1].real_value());
    EXPECT_FALSE(fixture.vfs->pathless_file_present());
    EXPECT_TRUE(session.autocommit());
  }
}

TEST(WriteSession, JoinsEmptyAndNewlyWrittenSourcesWithoutInventingRows) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE a(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "CREATE TABLE b(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "INSERT INTO a VALUES(1)");
  ExecuteDone(session, "INSERT INTO a VALUES(2)");
  EXPECT_TRUE(QueryRows(session, "SELECT a.id,b.id FROM a INNER JOIN b ON a.id=b.id").empty());
  EXPECT_TRUE(QueryRows(session, "SELECT b.id,a.id FROM b LEFT JOIN a ON a.id=b.id").empty());
  EXPECT_TRUE(QueryRows(session, "SELECT a.id,b.id FROM a RIGHT JOIN b ON a.id=b.id").empty());
  EXPECT_TRUE(QueryRows(session, "SELECT a.id,b.id FROM b a FULL JOIN b b ON a.id=b.id").empty());
  const auto full_empty_right =
      QueryRows(session, "SELECT a.id,b.id FROM a FULL JOIN b ON a.id=b.id ORDER BY a.id");
  ASSERT_EQ(2U, full_empty_right.size());
  for (std::size_t index = 0; index < full_empty_right.size(); ++index) {
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), full_empty_right[index][0].integer_value());
    EXPECT_EQ(SqlValueType::kNull, full_empty_right[index][1].type());
  }
  const auto full_empty_left =
      QueryRows(session, "SELECT b.id,a.id FROM b FULL JOIN a ON a.id=b.id ORDER BY a.id");
  ASSERT_EQ(2U, full_empty_left.size());
  for (std::size_t index = 0; index < full_empty_left.size(); ++index) {
    EXPECT_EQ(SqlValueType::kNull, full_empty_left[index][0].type());
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), full_empty_left[index][1].integer_value());
  }
  const auto right_empty_prefix =
      QueryRows(session, "SELECT b.id,a.id FROM b RIGHT JOIN a ON a.id=b.id ORDER BY a.id");
  ASSERT_EQ(2U, right_empty_prefix.size());
  for (std::size_t index = 0; index < right_empty_prefix.size(); ++index) {
    EXPECT_EQ(SqlValueType::kNull, right_empty_prefix[index][0].type());
    EXPECT_EQ(static_cast<std::int64_t>(index + 1U), right_empty_prefix[index][1].integer_value());
  }
  for (const std::string_view condition : {"", " ON a.id=b.id", " USING(id)"}) {
    const auto rows = QueryRows(session, "SELECT a.id,b.id,b._rowid_ FROM a LEFT JOIN b" +
                                             std::string{condition} + " ORDER BY a.id");
    ASSERT_EQ(2U, rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
      EXPECT_EQ(static_cast<std::int64_t>(index + 1U), rows[index][0].integer_value());
      EXPECT_EQ(SqlValueType::kNull, rows[index][1].type());
      EXPECT_EQ(SqlValueType::kNull, rows[index][2].type());
    }
  }
  ExecuteDone(session, "INSERT INTO b VALUES(2)");
  const auto matched =
      QueryRows(session, "SELECT a.id,b.id FROM a LEFT JOIN b ON a.id=b.id ORDER BY a.id");
  ASSERT_EQ(2U, matched.size());
  EXPECT_EQ(SqlValueType::kNull, matched[0][1].type());
  EXPECT_EQ(2, matched[1][1].integer_value());
  EXPECT_TRUE(
      QueryRows(session, "SELECT a.id FROM a LEFT JOIN b ON a.id=b.id WHERE b.id>99").empty());
  const auto empty = QueryRows(session, "SELECT count(),count(b.id) FROM a LEFT JOIN b ON 0");
  ASSERT_EQ(1U, empty.size());
  EXPECT_EQ(2, empty[0][0].integer_value());
  EXPECT_EQ(0, empty[0][1].integer_value());
  EXPECT_TRUE(session.autocommit());
}

TEST(WriteSession, ComparesUsingColumnsWithCanonicalAffinityAndNullSemantics) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE l(k TEXT,lvalue INTEGER)");
  ExecuteDone(session, "CREATE TABLE r(k TEXT,rvalue INTEGER)");
  ExecuteDone(session, "INSERT INTO l VALUES('A',1)");
  ExecuteDone(session, "INSERT INTO l VALUES('n',2)");
  ExecuteDone(session, "INSERT INTO l VALUES(NULL,3)");
  ExecuteDone(session, "INSERT INTO r VALUES('a',10)");
  ExecuteDone(session, "INSERT INTO r VALUES(NULL,30)");
  const auto forward =
      QueryRows(session, "SELECT k,r.k,lvalue,rvalue FROM l LEFT JOIN r USING(k) ORDER BY lvalue");
  ASSERT_EQ(3U, forward.size());
  EXPECT_EQ("A", Text(forward[0][0]));
  EXPECT_EQ("n", Text(forward[1][0]));
  for (std::size_t index = 0; index < forward.size(); ++index) {
    EXPECT_EQ(SqlValueType::kNull, forward[index][1].type());
    EXPECT_EQ(SqlValueType::kNull, forward[index][3].type());
  }
  EXPECT_EQ(SqlValueType::kNull, forward[2][0].type());
  const auto reversed =
      QueryRows(session, "SELECT k,l.k,rvalue,lvalue FROM r LEFT JOIN l USING(k) ORDER BY rvalue");
  ASSERT_EQ(2U, reversed.size());
  for (const auto& row : reversed) {
    EXPECT_EQ(SqlValueType::kNull, row[1].type());
    EXPECT_EQ(SqlValueType::kNull, row[3].type());
  }
  ExecuteDone(session, "CREATE TABLE numeric_key(k INTEGER,rvalue INTEGER)");
  ExecuteDone(session, "INSERT INTO numeric_key VALUES(1,40)");
  ExecuteDone(session, "INSERT INTO l VALUES('01',4)");
  const auto numeric = QueryRows(session, "SELECT k,n.k,lvalue FROM l JOIN numeric_key n USING(k)");
  ASSERT_EQ(1U, numeric.size());
  EXPECT_EQ("01", Text(numeric[0][0]));
  EXPECT_EQ(1, numeric[0][1].integer_value());
  EXPECT_EQ(4, numeric[0][2].integer_value());
}

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

TEST(WriteSession, JournalFreeFullErrorRollsBackTheExplicitTransaction) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY,Name TEXT)");
  ExecuteDone(session, "BEGIN");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'prior')");
  ExecuteDone(session, "INSERT INTO Items VALUES(9223372036854775807,'maximum')");
  fixture.vfs->SetRandomByte(std::byte{0});

  WriteStatement insert = PrepareOne(session, "INSERT INTO Items(Name) VALUES('full')");
  const auto failed = insert.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kFull, failed.error().code());
  EXPECT_TRUE(session.autocommit());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM Items").empty());
}

TEST(WriteSession, ConditionalJournalsPreserveOuterWorkAfterOrdinaryAbort) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY,n INTEGER)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,1)");
  ExecuteDone(session, "INSERT INTO Items VALUES(2,-9223372036854775808)");
  ExecuteDone(session, "BEGIN");
  ExecuteDone(session, "INSERT INTO Items VALUES(3,3)");

  for (const std::string_view sql :
       {"INSERT INTO Items VALUES(1,99)", "UPDATE Items SET n=abs(n)+10 WHERE id=2",
        "UPDATE Items SET n=abs(n)+10"}) {
    SCOPED_TRACE(sql);
    WriteStatement statement = PrepareOne(session, sql);
    const auto failed = statement.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_FALSE(session.autocommit());
    EXPECT_EQ(0U, session.changes());
    const auto reset = statement.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(failed.error().code(), reset.error().code());
    const auto rows = QueryRows(session, "SELECT id,n FROM Items ORDER BY id");
    ASSERT_EQ(3U, rows.size());
    EXPECT_EQ(1, rows[0][1].integer_value());
    EXPECT_EQ(3, rows[2][1].integer_value());
  }
  ExecuteDone(session, "COMMIT");
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

TEST(WriteSession, PropagatesTemporaryStoreModeToStatementJournalSpillPolicy) {
  constexpr std::size_t kRowCount = 300;
  constexpr std::size_t kPayloadBytes = 400;
  constexpr std::size_t kFileCapacity = 256U * 1024U;
  using LargeWriteVfs = test::WritePagerMemoryVfs<kFileCapacity>;

  for (const TemporaryStoreMode mode : {TemporaryStoreMode::kFile, TemporaryStoreMode::kMemory}) {
    SCOPED_TRACE(static_cast<unsigned>(mode));
    auto owned_vfs = std::make_unique<LargeWriteVfs>(false);
    LargeWriteVfs* const vfs = owned_vfs.get();
    WriteSession session =
        TakeValue(WriteSession::Open(std::move(owned_vfs), test::kWritePagerDatabasePath,
                                     WriteSessionOptions{
                                         .temporary_storage =
                                             TemporaryStorageOptions{
                                                 .mode = mode,
                                             },
                                     }));
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, payload TEXT)");
    ExecuteDone(session, "BEGIN");
    const std::string original(kPayloadBytes, 'x');
    for (std::size_t index = 0; index < kRowCount; ++index) {
      ExecuteDone(session, "INSERT INTO Items VALUES(" + std::to_string(index + 1U) + ",'" +
                               original + "')");
    }
    ExecuteDone(session, "COMMIT");

    ExecuteDone(session, "BEGIN");
    const std::string replacement(kPayloadBytes, 'y');
    ExecuteDone(session, "UPDATE Items SET payload='" + replacement + "'");
    ExecuteDone(session, "SAVEPOINT retained");
    const std::string savepoint_replacement(kPayloadBytes, 'z');
    ExecuteDone(session, "UPDATE Items SET payload='" + savepoint_replacement + "'");
    EXPECT_EQ(mode == TemporaryStoreMode::kFile, vfs->pathless_open_count() > 0U);
    ExecuteDone(session, "ROLLBACK TO retained");
    ExecuteDone(session, "RELEASE retained");
    ExecuteDone(session, "ROLLBACK");
  }
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

  std::unique_ptr<BtreeDatabase> pager_database =
      test::OpenWriteDatabase<BtreeDatabase>(*fixture.vfs, 64U);
  Pager* pager = pager_database == nullptr ? nullptr : &pager_database->pager();
  ASSERT_NE(nullptr, pager);
  RequireStatus(pager->BeginRead());
  const CatalogSnapshotPtr catalog =
      TakeValue(LoadCatalog(*pager_database, CatalogLoadOptions{.generation = 101}));
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

TEST(WriteSession, UsesNativeUniqueDescendingNullTailAnalyzeShortcut) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(Value INTEGER)");
  ExecuteDone(session, "INSERT INTO Items VALUES(NULL)");
  ExecuteDone(session, "INSERT INTO Items VALUES(NULL)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1)");
  ExecuteDone(session, "CREATE UNIQUE INDEX items_value ON Items(Value DESC)");
  ExecuteDone(session, "ANALYZE items_value");
  const auto rows = QueryRows(session, "SELECT stat FROM sqlite_stat1 WHERE idx='items_value'");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("3 1", Text(rows[0][0]));
  ExecuteDone(session, "CREATE UNIQUE INDEX items_value_asc ON Items(Value)");
  ExecuteDone(session, "ANALYZE items_value_asc");
  const auto ascending =
      QueryRows(session, "SELECT stat FROM sqlite_stat1 WHERE idx='items_value_asc'");
  ASSERT_EQ(1U, ascending.size());
  EXPECT_EQ("3 2", Text(ascending[0][0]));
}

TEST(WriteSession, AnalyzesSingleUniqueNotNullKeyWithoutPreviousFields) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(Value INTEGER NOT NULL)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1)");
  ExecuteDone(session, "INSERT INTO Items VALUES(2)");
  ExecuteDone(session, "INSERT INTO Items VALUES(3)");
  ExecuteDone(session, "CREATE UNIQUE INDEX items_value ON Items(Value)");
  ExecuteDone(session, "ANALYZE items_value");
  const auto rows = QueryRows(session, "SELECT stat FROM sqlite_stat1 WHERE idx='items_value'");
  ASSERT_EQ(1U, rows.size());
  EXPECT_EQ("3 1", Text(rows[0][0]));
}

TEST(WriteSession, DefersWarmPreparationReadFailuresToExecution) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1)");
  const std::size_t reads_before = fixture.vfs->total_database_reads();
  fixture.vfs->FailDatabaseReadAfter(0U, ErrorCode::kIo);

  auto prepared = session.Prepare(Utf8View{"SELECT id FROM Items WHERE id=?1"});
  ASSERT_TRUE(prepared.has_value());
  ASSERT_TRUE(prepared->statement.has_value());
  EXPECT_EQ(reads_before, fixture.vfs->total_database_reads());
  ASSERT_EQ(1U, prepared->statement->result_columns().size());
  ASSERT_EQ(1U, prepared->statement->parameter_count());
  RequireStatus(prepared->statement->Bind(1, SqlValue::Integer(1)));

  const auto failed = prepared->statement->Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_GT(fixture.vfs->total_database_reads(), reads_before);
  const auto finalized = prepared->statement->Finalize();
  ASSERT_FALSE(finalized.has_value());
  EXPECT_EQ(ErrorCode::kIo, finalized.error().code());

  WriteStatement retried = PrepareOne(session, "SELECT id FROM Items");
  ASSERT_EQ(WriteStep::kRow, TakeValue(retried.Step()));
  EXPECT_EQ(1, retried.row()[0].integer_value());
  RequireStatus(retried.Finalize());
}

TEST(WriteSession, KeepsInitializedSchemasAfterDmlRollback) {
  for (const bool named : {false, true}) {
    SCOPED_TRACE(named);
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "BEGIN");
    if (named) {
      ExecuteDone(session, "SAVEPOINT s");
    }
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    if (named) {
      ExecuteDone(session, "ROLLBACK TO s");
      ExecuteDone(session, "RELEASE s");
      ExecuteDone(session, "COMMIT");
    } else {
      ExecuteDone(session, "ROLLBACK");
    }
    const std::size_t reads_before = fixture.vfs->total_database_reads();
    fixture.vfs->FailDatabaseReadAfter(0U, ErrorCode::kIo);
    auto prepared = session.Prepare(Utf8View{"SELECT id FROM Items"});
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->statement.has_value());
    EXPECT_EQ(reads_before, fixture.vfs->total_database_reads());
    RequireStatus(prepared->statement->Finalize());
  }
}

TEST(WriteSession, InvalidatesInitializedSchemasAfterDdlRollback) {
  for (const bool named : {false, true}) {
    SCOPED_TRACE(named);
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    WriteStatement retained = PrepareOne(session, "SELECT id FROM Items");
    ExecuteDone(session, "BEGIN");
    if (named) {
      ExecuteDone(session, "SAVEPOINT s");
    }
    ExecuteDone(session, "CREATE TABLE Removed(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, named ? "ROLLBACK TO s" : "ROLLBACK");
    const auto missing = session.Prepare(Utf8View{"SELECT id FROM Removed"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, missing.error().code());
    EXPECT_EQ(WriteStep::kDone, TakeValue(retained.Step()));
    RequireStatus(retained.Finalize());
    if (named) {
      ExecuteDone(session, "ROLLBACK");
    }
  }
}

TEST(WriteSession, ReloadsAfterSchemaRollbackRatherThanUsingRetainedMetadata) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "BEGIN");
  ExecuteDone(session, "CREATE TABLE Removed(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "ROLLBACK");
  fixture.vfs->FailDatabaseReadAfter(0U, ErrorCode::kIo);
  const auto failed = session.Prepare(Utf8View{"SELECT id FROM Items"});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  auto recovered = session.Prepare(Utf8View{"SELECT id FROM Items"});
  ASSERT_TRUE(recovered.has_value());
  ASSERT_TRUE(recovered->statement.has_value());
  RequireStatus(recovered->statement->Finalize());
}

TEST(WriteSession, PreservesExplicitTransactionsAcrossPreparationProbeFailures) {
  for (const ErrorCode failure : {ErrorCode::kIo, ErrorCode::kOutOfMemory}) {
    SCOPED_TRACE(static_cast<unsigned>(failure));
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "BEGIN");
    fixture.vfs->FailDatabaseReadAfter(0U, failure);
    const auto failed = session.Prepare(Utf8View{"INSERT INTO absent_table VALUES(1)"});
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failure == ErrorCode::kOutOfMemory ? ErrorCode::kOutOfMemory : ErrorCode::kGeneric,
              failed.error().code());
    EXPECT_FALSE(session.autocommit());
    WriteStatement valid = PrepareOne(session, "SELECT id FROM Items");
    EXPECT_EQ(WriteStep::kDone, TakeValue(valid.Step()));
    RequireStatus(valid.Finalize());
    EXPECT_FALSE(session.autocommit());
    ExecuteDone(session, "ROLLBACK");
  }
}

TEST(WriteSession, RetriesNativeWritableTargetLookupsAfterSchemaChanges) {
  for (const std::string_view sql : {
           "INSERT INTO Added(id) VALUES(?1)",
           "UPDATE Added SET id=?1",
           "DELETE FROM Added WHERE id=?1",
           "CREATE INDEX candidate ON Added(id)",
           "ANALYZE Added",
       }) {
    SCOPED_TRACE(sql);
    SessionFixture fixture;
    ExecuteDone(fixture.Get(), "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    SessionFixture replacement;
    ExecuteDone(replacement.Get(), "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(replacement.Get(), "CREATE TABLE Added(id INTEGER PRIMARY KEY)");
    fixture.vfs->LoadDatabase(replacement.vfs->database_bytes());
    auto prepared = fixture.Get().Prepare(Utf8View{sql});
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->statement.has_value());
    RequireStatus(prepared->statement->Finalize());
  }
}

TEST(WriteSession, RetriesExplicitWritableColumnsAfterSchemaChanges) {
  for (const std::string_view sql : {
           "INSERT INTO Items(Added) VALUES(?1)",
           "UPDATE Items SET Added=?1",
           "CREATE INDEX candidate ON Items(Added)",
       }) {
    SCOPED_TRACE(sql);
    SessionFixture fixture;
    ExecuteDone(fixture.Get(), "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    SessionFixture replacement;
    ExecuteDone(replacement.Get(), "CREATE TABLE Items(id INTEGER PRIMARY KEY,Added INT)");
    ExecuteDone(replacement.Get(), "CREATE TABLE Marker(id INTEGER PRIMARY KEY)");
    fixture.vfs->LoadDatabase(replacement.vfs->database_bytes());
    auto prepared = fixture.Get().Prepare(Utf8View{sql});
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->statement.has_value());
    RequireStatus(prepared->statement->Finalize());
  }
}

TEST(WriteSession, DistinguishesEarlyTableErrorsFromIndexCollisionProvenance) {
  for (const bool table_error : {false, true}) {
    SCOPED_TRACE(table_error);
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    const std::size_t reads_before = fixture.vfs->total_database_reads();
    fixture.vfs->FailDatabaseReadAfter(0U, ErrorCode::kOutOfMemory);
    const auto failed = session.Prepare(
        Utf8View{table_error ? "CREATE TABLE Items(x)" : "CREATE INDEX Items ON Items(id)"});
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(table_error ? ErrorCode::kOutOfMemory : ErrorCode::kGeneric, failed.error().code());
    EXPECT_EQ(reads_before + (table_error ? 1U : 0U), fixture.vfs->total_database_reads());
  }
}

TEST(WriteSession, RetainsCleanupAndInvalidityAfterKnownSchemaProbeFailure) {
  SessionFixture fixture;
  ExecuteDone(fixture.Get(), "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
  SessionFixture replacement;
  ExecuteDone(replacement.Get(), "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
  ExecuteDone(replacement.Get(), "CREATE TABLE Added(id INTEGER PRIMARY KEY)");
  fixture.vfs->LoadDatabase(replacement.vfs->database_bytes());
  fixture.vfs->FailNextDatabaseUnlock(DatabaseLock::kNone, ErrorCode::kIo);
  const auto failed = fixture.Get().Prepare(Utf8View{"SELECT id FROM Added"});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kGeneric, failed.error().code());
  fixture.vfs->FailDatabaseReadAfter(0U, ErrorCode::kIo);
  const auto stale = fixture.Get().Prepare(Utf8View{"SELECT id FROM Items"});
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(ErrorCode::kIo, stale.error().code());
  auto recovered = fixture.Get().Prepare(Utf8View{"SELECT id FROM Added"});
  ASSERT_TRUE(recovered.has_value());
  ASSERT_TRUE(recovered->statement.has_value());
  RequireStatus(recovered->statement->Finalize());
}

TEST(WriteSession, PublishesSchemaBeforeTerminalCleanupCompletes) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
  WriteStatement create = PrepareOne(session, "CREATE TABLE Added(id INTEGER PRIMARY KEY)");
  fixture.vfs->FailDatabaseUnlockAfter(DatabaseLock::kNone, 1U, ErrorCode::kIo);
  const auto failed = create.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  auto visible = session.Prepare(Utf8View{"SELECT id FROM Added"});
  ASSERT_TRUE(visible.has_value());
  ASSERT_TRUE(visible->statement.has_value());
  RequireStatus(visible->statement->Finalize());
  const auto reset = create.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(ErrorCode::kIo, reset.error().code());
  RequireStatus(create.Finalize());
  EXPECT_TRUE(QueryRows(session, "SELECT id FROM Added").empty());
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

TEST(WriteSession, RetiresImplicitSuccessAfterAutomaticCommitRollback) {
  SessionFixture fixture;
  WriteSession& session = fixture.Get();
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1)");
  WriteStatement insert = PrepareOne(session, "INSERT INTO Items VALUES(?1)");
  RequireStatus(insert.Bind(1, SqlValue::Integer(2)));

  fixture.vfs->FailNextDatabaseLock(DatabaseLock::kExclusive, ErrorCode::kBusy);
  const auto failed = insert.Step();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kBusy, failed.error().code());
  EXPECT_TRUE(session.autocommit());
  EXPECT_EQ(0U, session.changes());
  EXPECT_EQ(2, session.last_insert_rowid());
  const auto old_rows = QueryRows(session, "SELECT id FROM Items ORDER BY id");
  ASSERT_EQ(1U, old_rows.size());
  EXPECT_EQ(1, old_rows[0][0].integer_value());

  const auto reset = insert.Reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(ErrorCode::kBusy, reset.error().code());
  EXPECT_EQ(failed.error().ToString(), reset.error().ToString());
  ASSERT_TRUE(insert.Bind(1, SqlValue::Integer(2)).has_value());
  EXPECT_EQ(WriteStep::kDone, TakeValue(insert.Step()));
  EXPECT_TRUE(session.autocommit());
  EXPECT_EQ(1U, session.changes());
  const auto retried_rows = QueryRows(session, "SELECT id FROM Items ORDER BY id");
  ASSERT_EQ(2U, retried_rows.size());
  EXPECT_EQ(2, retried_rows[1][0].integer_value());
}

TEST(WriteSession, RebindsAfterRetiredImplicitCompletion) {
  for (const ErrorCode failure : {ErrorCode::kBusy, ErrorCode::kIo}) {
    SCOPED_TRACE(static_cast<int>(failure));
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    WriteStatement insert = PrepareOne(session, "INSERT INTO Items VALUES(?1)");
    RequireStatus(insert.Bind(1, SqlValue::Integer(2)));
    fixture.vfs->FailNextDatabaseLock(DatabaseLock::kExclusive, failure);
    const auto failed = insert.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failure, failed.error().code());
    EXPECT_EQ(0U, session.changes());

    const auto reset = insert.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(failed.error().ToString(), reset.error().ToString());
    ASSERT_TRUE(insert.Bind(1, SqlValue::Integer(3)).has_value());
    EXPECT_EQ(WriteStep::kDone, TakeValue(insert.Step()));
    EXPECT_EQ(3, session.last_insert_rowid());
    EXPECT_EQ(1U, session.changes());
    const auto rows = QueryRows(session, "SELECT id FROM Items ORDER BY id");
    ASSERT_EQ(2U, rows.size());
    EXPECT_EQ(1, rows[0][0].integer_value());
    EXPECT_EQ(3, rows[1][0].integer_value());
  }
}

TEST(WriteSession, ReexecutesAfterRetiredImplicitCompletionWithoutReset) {
  for (const ErrorCode failure : {ErrorCode::kBusy, ErrorCode::kIo}) {
    SCOPED_TRACE(static_cast<int>(failure));
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    WriteStatement insert = PrepareOne(session, "INSERT INTO Items VALUES(?1)");
    RequireStatus(insert.Bind(1, SqlValue::Integer(2)));
    fixture.vfs->FailNextDatabaseLock(DatabaseLock::kExclusive, failure);
    const auto failed = insert.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failure, failed.error().code());
    EXPECT_EQ(0U, session.changes());
    EXPECT_TRUE(session.autocommit());

    EXPECT_EQ(WriteStep::kDone, TakeValue(insert.Step()));
    EXPECT_EQ(1U, session.changes());
    EXPECT_EQ(2, session.last_insert_rowid());
    const auto rows = QueryRows(session, "SELECT id FROM Items ORDER BY id");
    ASSERT_EQ(2U, rows.size());
    EXPECT_EQ(2, rows[1][0].integer_value());
  }
}

TEST(WriteSession, FinalizesRetiredImplicitCompletionWithOriginalError) {
  for (const ErrorCode failure : {ErrorCode::kBusy, ErrorCode::kIo}) {
    SCOPED_TRACE(static_cast<int>(failure));
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    WriteStatement insert = PrepareOne(session, "INSERT INTO Items VALUES(2)");
    fixture.vfs->FailNextDatabaseLock(DatabaseLock::kExclusive, failure);
    const auto failed = insert.Step();
    ASSERT_FALSE(failed.has_value());
    const auto finalized = insert.Finalize();
    ASSERT_FALSE(finalized.has_value());
    EXPECT_EQ(failed.error().ToString(), finalized.error().ToString());
    EXPECT_FALSE(insert.valid());
    EXPECT_EQ(0U, session.changes());
    EXPECT_EQ(2, session.last_insert_rowid());
    EXPECT_TRUE(session.autocommit());
    RequireStatus(insert.Finalize());
    const auto old_rows = QueryRows(session, "SELECT id FROM Items");
    ASSERT_EQ(1U, old_rows.size());
    EXPECT_EQ(1, old_rows[0][0].integer_value());
    ExecuteDone(session, "INSERT INTO Items VALUES(2)");
    EXPECT_EQ(1U, session.changes());
  }
}

TEST(WriteSession, DiscardsRolledBackCatalogCompletionBeforeReexecution) {
  for (const ErrorCode failure : {ErrorCode::kBusy, ErrorCode::kIo}) {
    SCOPED_TRACE(static_cast<int>(failure));
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    WriteStatement create = PrepareOne(session, "CREATE TABLE New(id INTEGER PRIMARY KEY)");
    fixture.vfs->FailNextDatabaseLock(DatabaseLock::kExclusive, failure);
    const auto failed = create.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failure, failed.error().code());
    EXPECT_TRUE(session.autocommit());
    EXPECT_EQ(1U, session.changes());
    EXPECT_EQ(1, session.last_insert_rowid());
    const auto missing = session.Prepare(Utf8View{"SELECT id FROM New"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, missing.error().code());

    const auto reset = create.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(failed.error().ToString(), reset.error().ToString());
    EXPECT_EQ(WriteStep::kDone, TakeValue(create.Step()));
    EXPECT_EQ(1U, session.changes());
    EXPECT_EQ(1, session.last_insert_rowid());
    EXPECT_TRUE(QueryRows(session, "SELECT id FROM New").empty());
  }
}

TEST(WriteSession, RetainsDurableDmlCompletionAcrossCleanupFailures) {
  for (const DatabaseLock lock : {DatabaseLock::kShared, DatabaseLock::kNone}) {
    SCOPED_TRACE(static_cast<int>(lock));
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Value INT)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1,10)");
    ExecuteDone(session, "INSERT INTO Items VALUES(2,20)");
    WriteStatement update = PrepareOne(session, "UPDATE Items SET Value=Value+1");
    fixture.vfs->FailDatabaseUnlockAfter(lock, lock == DatabaseLock::kNone ? 1U : 0U,
                                         ErrorCode::kIo);
    const auto failed = update.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_EQ(1U, session.changes());
    const std::size_t writes = fixture.vfs->total_database_writes();

    const auto reset = update.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(failed.error().ToString(), reset.error().ToString());
    EXPECT_EQ(writes, fixture.vfs->total_database_writes());
    EXPECT_TRUE(session.autocommit());
    EXPECT_EQ(2U, session.changes());
    EXPECT_EQ(2, session.last_insert_rowid());
    const auto rows = QueryRows(session, "SELECT Value FROM Items ORDER BY id");
    ASSERT_EQ(2U, rows.size());
    EXPECT_EQ(11, rows[0][0].integer_value());
    EXPECT_EQ(21, rows[1][0].integer_value());
    RequireStatus(update.Reset());
  }
}

TEST(WriteSession, RetainsDurableCatalogCompletionAcrossCleanupFailures) {
  for (const DatabaseLock lock : {DatabaseLock::kShared, DatabaseLock::kNone}) {
    SCOPED_TRACE(static_cast<int>(lock));
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    WriteStatement create = PrepareOne(session, "CREATE TABLE New(id INTEGER PRIMARY KEY)");
    fixture.vfs->FailDatabaseUnlockAfter(lock, lock == DatabaseLock::kNone ? 1U : 0U,
                                         ErrorCode::kIo);
    const auto failed = create.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    const std::size_t writes = fixture.vfs->total_database_writes();

    const auto reset = create.Reset();
    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(failed.error().ToString(), reset.error().ToString());
    EXPECT_EQ(writes, fixture.vfs->total_database_writes());
    EXPECT_TRUE(session.autocommit());
    EXPECT_EQ(1U, session.changes());
    EXPECT_EQ(1, session.last_insert_rowid());
    EXPECT_TRUE(QueryRows(session, "SELECT id FROM New").empty());
    const auto schema = QueryRows(session, "SELECT name FROM sqlite_schema WHERE name='New'");
    ASSERT_EQ(1U, schema.size());
    EXPECT_EQ("New", Text(schema[0][0]));
    RequireStatus(create.Reset());
  }
}

TEST(WriteSession, ClearsEveryRolledBackDmlChangeCount) {
  for (const std::string_view sql :
       {"INSERT INTO Items VALUES(3,30)", "UPDATE Items SET Value=Value+1", "DELETE FROM Items"}) {
    SCOPED_TRACE(sql);
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Value INT)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1,10)");
    ExecuteDone(session, "INSERT INTO Items VALUES(2,20)");
    WriteStatement mutation = PrepareOne(session, sql);
    fixture.vfs->FailNextDatabaseLock(DatabaseLock::kExclusive, ErrorCode::kIo);
    const auto failed = mutation.Step();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(ErrorCode::kIo, failed.error().code());
    EXPECT_TRUE(session.autocommit());
    EXPECT_EQ(0U, session.changes());
    EXPECT_EQ(sql.starts_with("INSERT") ? 3 : 2, session.last_insert_rowid());
    const auto rows = QueryRows(session, "SELECT id,Value FROM Items ORDER BY id");
    ASSERT_EQ(2U, rows.size());
    EXPECT_EQ(1, rows[0][0].integer_value());
    EXPECT_EQ(10, rows[0][1].integer_value());
    EXPECT_EQ(2, rows[1][0].integer_value());
    EXPECT_EQ(20, rows[1][1].integer_value());
  }
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

TEST(WriteSession, RetriesOnlyPendingFullRollbackCleanupBeforeFreshExecution) {
  for (const auto lock : {DatabaseLock::kShared, DatabaseLock::kNone}) {
    SCOPED_TRACE(static_cast<int>(lock));
    SessionFixture fixture;
    WriteSession& session = fixture.Get();
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "BEGIN");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    auto rollback = PrepareOne(session, "ROLLBACK");
    fixture.vfs->FailNextDatabaseUnlock(lock, ErrorCode::kIo);

    const auto first = rollback.Step();

    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(ErrorCode::kIo, first.error().code());
    EXPECT_FALSE(session.autocommit());
    const auto deletes = fixture.vfs->total_journal_deletes();
    EXPECT_EQ(WriteStep::kDone, TakeValue(rollback.Step()));
    EXPECT_TRUE(session.autocommit());
    EXPECT_EQ(deletes, fixture.vfs->total_journal_deletes());
    EXPECT_TRUE(QueryRows(session, "SELECT id FROM Items").empty());

    const auto fresh = rollback.Step();

    ASSERT_FALSE(fresh.has_value());
    EXPECT_EQ(ErrorCode::kGeneric, fresh.error().code());
    EXPECT_TRUE(session.autocommit());
  }
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
