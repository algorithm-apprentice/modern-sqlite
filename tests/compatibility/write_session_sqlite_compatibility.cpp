#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "tests/compatibility/write_session_crash_harness.hpp"

namespace {

constexpr int kReadOnlyFlags = SQLITE_OPEN_READONLY + SQLITE_OPEN_NOMUTEX;
constexpr int kCreateFlags = SQLITE_OPEN_READWRITE + SQLITE_OPEN_CREATE + SQLITE_OPEN_NOMUTEX;

template <typename T>
[[nodiscard]] T TakeValue(modern_sqlite::Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

void RequireStatus(modern_sqlite::Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

template <typename T>
[[nodiscard]] T TakeOptional(std::optional<T> value, std::string_view message) {
  if (!value.has_value()) {
    throw std::runtime_error{std::string{message}};
  }
  return std::move(value).value();
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-write-session-compatibility-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path DatabasePath(std::string_view name) const {
    return path_ / (std::string{name} + ".sqlite");
  }

 private:
  std::filesystem::path path_;
};

class Database final {
 public:
  Database(const std::filesystem::path& path, int flags) {
    const int result = sqlite3_open_v2(path.string().c_str(), &database_, flags, nullptr);
    if (result != SQLITE_OK) {
      const std::string message =
          database_ == nullptr ? "sqlite3_open_v2 failed" : sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        static_cast<void>(sqlite3_close(database_));
      }
      throw std::runtime_error(message);
    }
  }

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  ~Database() noexcept {
    if (database_ != nullptr) {
      static_cast<void>(sqlite3_close(database_));
    }
  }

  [[nodiscard]] sqlite3* get() const noexcept { return database_; }

 private:
  sqlite3* database_ = nullptr;
};

class Statement final {
 public:
  Statement(sqlite3* database, std::string_view sql) {
    const std::string owned{sql};
    if (sqlite3_prepare_v2(database, owned.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(database));
    }
  }

  explicit Statement(sqlite3_stmt* statement) noexcept : statement_(statement) {}

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  ~Statement() noexcept { static_cast<void>(Finalize()); }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

  [[nodiscard]] int Finalize() noexcept {
    if (statement_ == nullptr) {
      return SQLITE_OK;
    }
    return sqlite3_finalize(std::exchange(statement_, nullptr));
  }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

void ExecuteSqlite(sqlite3* database, std::string_view sql) {
  const std::string owned{sql};
  char* error = nullptr;
  if (sqlite3_exec(database, owned.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(database) : std::string{error};
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

[[nodiscard]] std::int64_t QueryInteger(sqlite3* database, std::string_view sql) {
  const Statement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(database));
  }
  return sqlite3_column_int64(statement.get(), 0);
}

[[nodiscard]] std::string QueryText(sqlite3* database, std::string_view sql) {
  const Statement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(database));
  }
  const unsigned char* text = sqlite3_column_text(statement.get(), 0);
  return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

[[nodiscard]] modern_sqlite::WriteStatement Prepare(modern_sqlite::WriteSession& session,
                                                    std::string_view sql) {
  auto prepared = session.Prepare(modern_sqlite::Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value()) {
    throw std::runtime_error{"Modern write-session prepare failed"};
  }
  return std::move(*prepared->statement);
}

void ExecuteModern(modern_sqlite::WriteSession& session, std::string_view sql) {
  modern_sqlite::WriteStatement statement = Prepare(session, sql);
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone) {
    throw std::runtime_error{"Modern write statement produced a row"};
  }
}

void VerifyIntegrity(sqlite3* database) {
  if (QueryText(database, "PRAGMA integrity_check") != "ok") {
    throw std::runtime_error{"SQLite integrity_check failed"};
  }
}

void WriteImage(const std::filesystem::path& path, modern_sqlite::ByteView image) {
  std::error_code error;
  static_cast<void>(std::filesystem::remove(path, error));
  static_cast<void>(std::filesystem::remove(path.string() + "-journal", error));
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(reinterpret_cast<const char*>(image.data()),
               static_cast<std::streamsize>(image.size()));
  if (!output) {
    throw std::runtime_error{"could not write recovered write-session image"};
  }
}

struct CrashExpectedState {
  std::string_view rows;
  bool temp_visible;
};

[[nodiscard]] CrashExpectedState ExpectedCrashState(std::string_view scenario, bool terminal) {
  constexpr CrashExpectedState kInitial{
      .rows = "1:one,2:two,3:three",
      .temp_visible = false,
  };
  if (!terminal || scenario == "create-full-rollback" || scenario == "create-rollback-to" ||
      scenario == "full-dml-rollback") {
    return kInitial;
  }
  if (scenario == "implicit-insert") {
    return CrashExpectedState{.rows = "1:one,2:two,3:three,4:four", .temp_visible = false};
  }
  if (scenario == "implicit-create") {
    return CrashExpectedState{.rows = kInitial.rows, .temp_visible = true};
  }
  if (scenario == "exact-rowid-move") {
    return CrashExpectedState{.rows = "2:two,3:three,10:moved", .temp_visible = false};
  }
  if (scenario == "scan-rowid-move") {
    return CrashExpectedState{.rows = "1:one,12:twox,13:threex", .temp_visible = false};
  }
  if (scenario == "scan-delete") {
    return CrashExpectedState{.rows = "1:one", .temp_visible = false};
  }
  if (scenario == "explicit-commit") {
    return CrashExpectedState{.rows = "1:updated,3:three,4:four", .temp_visible = false};
  }
  if (scenario == "constraint-then-commit") {
    return CrashExpectedState{
        .rows = "1:one,2:two,3:three,4:kept",
        .temp_visible = false,
    };
  }
  if (scenario == "named-rollback-then-commit") {
    return CrashExpectedState{.rows = "1:outer,2:two,3:three", .temp_visible = false};
  }
  if (scenario == "transaction-savepoint-release") {
    return CrashExpectedState{
        .rows = "1:one,2:two,3:three,4:savepoint",
        .temp_visible = false,
    };
  }
  throw std::runtime_error{"unknown write-session crash scenario"};
}

struct CrashVerifierContext {
  const TemporaryDirectory* directory;
  std::size_t sequence = 0;
};

void VerifyCrashImageWithSqlite(void* raw_context, std::string_view scenario, std::size_t cut,
                                bool writes_are_durable, bool terminal,
                                modern_sqlite::ByteView image) {
  if (raw_context == nullptr) {
    throw std::runtime_error{"write-session crash verifier context is missing"};
  }
  auto& context = *static_cast<CrashVerifierContext*>(raw_context);
  const std::filesystem::path path =
      context.directory->DatabasePath("crash-" + std::to_string(context.sequence++));
  WriteImage(path, image);
  const Database sqlite{path, kReadOnlyFlags};
  VerifyIntegrity(sqlite.get());
  const CrashExpectedState expected = ExpectedCrashState(scenario, terminal);
  const std::string rows = QueryText(
      sqlite.get(),
      "SELECT group_concat(id||':'||Name,',') FROM (SELECT id,Name FROM Items ORDER BY id)");
  const bool temp_visible =
      QueryInteger(sqlite.get(), "SELECT count(*) FROM sqlite_schema WHERE name='Temp'") == 1;
  if (rows != expected.rows || temp_visible != expected.temp_visible) {
    throw std::runtime_error{std::string{scenario} + " cut=" + std::to_string(cut) +
                             " durability=" + (writes_are_durable ? "durable" : "volatile") +
                             " disagrees in pinned SQLite"};
  }
}

using TraceStorage =
    std::variant<std::monostate, std::int64_t, double, std::string, std::vector<unsigned char>>;

struct TraceValue {
  TraceStorage storage;

  bool operator==(const TraceValue&) const = default;
};

using TraceRow = std::vector<TraceValue>;

struct TraceOutcome {
  int primary_code = SQLITE_OK;
  std::optional<int> finalize_primary_code{};
  std::size_t column_count = 0;
  std::vector<TraceRow> rows;
  std::uint64_t changes = 0;
  std::int64_t last_insert_rowid = 0;
  bool autocommit = true;

  bool operator==(const TraceOutcome&) const = default;
};

struct TraceCase {
  std::string_view sql;
  std::optional<std::uint32_t> schema_cookie{};
};

[[nodiscard]] constexpr int PrimaryCode(int sqlite_code) noexcept {
  return static_cast<int>(static_cast<unsigned int>(sqlite_code) & 0xffU);
}

[[nodiscard]] TraceValue CopyModernValue(const modern_sqlite::SqlValue& value) {
  switch (value.type()) {
    case modern_sqlite::SqlValueType::kNull:
      return TraceValue{};
    case modern_sqlite::SqlValueType::kInteger:
      return TraceValue{.storage =
                            TakeOptional(value.integer_value(), "Modern INTEGER has no payload")};
    case modern_sqlite::SqlValueType::kReal:
      return TraceValue{.storage = TakeOptional(value.real_value(), "Modern REAL has no payload")};
    case modern_sqlite::SqlValueType::kText:
      return TraceValue{
          .storage =
              std::string{TakeOptional(value.text_value(), "Modern TEXT has no payload").bytes()}};
    case modern_sqlite::SqlValueType::kBlob: {
      const modern_sqlite::ByteView bytes =
          TakeOptional(value.blob_value(), "Modern BLOB has no payload");
      std::vector<unsigned char> copy;
      copy.reserve(bytes.size());
      for (const std::byte byte : bytes) {
        copy.push_back(std::to_integer<unsigned char>(byte));
      }
      return TraceValue{.storage = std::move(copy)};
    }
  }
  throw std::runtime_error{"Modern returned an unknown SQL storage class"};
}

[[nodiscard]] TraceValue CopySqliteValue(sqlite3_stmt* statement, int column) {
  switch (sqlite3_column_type(statement, column)) {
    case SQLITE_NULL:
      return TraceValue{};
    case SQLITE_INTEGER:
      return TraceValue{.storage = sqlite3_column_int64(statement, column)};
    case SQLITE_FLOAT:
      return TraceValue{.storage = sqlite3_column_double(statement, column)};
    case SQLITE_TEXT: {
      const int size = sqlite3_column_bytes(statement, column);
      const unsigned char* data = sqlite3_column_text(statement, column);
      if (size == 0) {
        return TraceValue{.storage = std::string{}};
      }
      return TraceValue{
          .storage =
              std::string{reinterpret_cast<const char*>(data), static_cast<std::size_t>(size)},
      };
    }
    case SQLITE_BLOB: {
      const int size = sqlite3_column_bytes(statement, column);
      const auto* data = static_cast<const unsigned char*>(sqlite3_column_blob(statement, column));
      if (size == 0) {
        return TraceValue{.storage = std::vector<unsigned char>{}};
      }
      return TraceValue{
          .storage = std::vector<unsigned char>{data, data + static_cast<std::size_t>(size)},
      };
    }
    default:
      throw std::runtime_error{"SQLite returned an unknown SQL storage class"};
  }
}

void CaptureModernState(const modern_sqlite::WriteSession& session, TraceOutcome& outcome) {
  outcome.changes = session.changes();
  outcome.last_insert_rowid = session.last_insert_rowid();
  outcome.autocommit = session.autocommit();
}

void CaptureSqliteState(sqlite3* database, TraceOutcome& outcome) {
  const sqlite3_int64 changes = sqlite3_changes64(database);
  if (changes < 0) {
    throw std::runtime_error{"SQLite returned a negative change count"};
  }
  outcome.changes = static_cast<std::uint64_t>(changes);
  outcome.last_insert_rowid = sqlite3_last_insert_rowid(database);
  outcome.autocommit = sqlite3_get_autocommit(database) != 0;
}

[[nodiscard]] TraceOutcome RunModernStatement(modern_sqlite::WriteSession& session,
                                              std::string_view sql) {
  TraceOutcome outcome;
  auto prepared = session.Prepare(modern_sqlite::Utf8View{sql});
  if (!prepared.has_value()) {
    outcome.primary_code = prepared.error().primary_sqlite_code();
    CaptureModernState(session, outcome);
    return outcome;
  }
  if (!prepared->statement.has_value() || prepared->next_offset.value() != sql.size()) {
    throw std::runtime_error{"Modern did not prepare exactly one trace statement"};
  }

  modern_sqlite::WriteStatement statement = std::move(*prepared->statement);
  outcome.column_count = statement.result_columns().size();
  while (true) {
    const auto stepped = statement.Step();
    if (!stepped.has_value()) {
      outcome.primary_code = stepped.error().primary_sqlite_code();
      break;
    }
    if (*stepped == modern_sqlite::WriteStep::kDone) {
      break;
    }
    TraceRow row;
    row.reserve(statement.row().size());
    for (const modern_sqlite::SqlValue& value : statement.row()) {
      row.push_back(CopyModernValue(value));
    }
    outcome.rows.push_back(std::move(row));
  }

  const modern_sqlite::Status finalized = statement.Finalize();
  outcome.finalize_primary_code =
      finalized.has_value() ? SQLITE_OK : finalized.error().primary_sqlite_code();
  CaptureModernState(session, outcome);
  return outcome;
}

[[nodiscard]] TraceOutcome RunSqliteStatement(sqlite3* database, std::string_view sql) {
  TraceOutcome outcome;
  const std::string owned{sql};
  sqlite3_stmt* raw_statement = nullptr;
  const char* tail = nullptr;
  const int prepared = sqlite3_prepare_v2(database, owned.c_str(), static_cast<int>(owned.size()),
                                          &raw_statement, &tail);
  Statement statement{raw_statement};
  if (prepared != SQLITE_OK) {
    outcome.primary_code = PrimaryCode(prepared);
    if (raw_statement != nullptr) {
      outcome.finalize_primary_code = PrimaryCode(statement.Finalize());
    }
    CaptureSqliteState(database, outcome);
    return outcome;
  }
  if (tail != owned.c_str() + owned.size()) {
    throw std::runtime_error{"SQLite did not prepare exactly one trace statement"};
  }

  outcome.column_count = static_cast<std::size_t>(sqlite3_column_count(statement.get()));
  while (true) {
    const int stepped = sqlite3_step(statement.get());
    if (stepped == SQLITE_DONE) {
      break;
    }
    if (stepped != SQLITE_ROW) {
      outcome.primary_code = PrimaryCode(stepped);
      break;
    }
    TraceRow row;
    row.reserve(outcome.column_count);
    for (std::size_t column = 0; column < outcome.column_count; ++column) {
      row.push_back(CopySqliteValue(statement.get(), static_cast<int>(column)));
    }
    outcome.rows.push_back(std::move(row));
  }

  outcome.finalize_primary_code = PrimaryCode(statement.Finalize());
  CaptureSqliteState(database, outcome);
  return outcome;
}

[[nodiscard]] std::string DescribeValue(const TraceValue& value) {
  switch (value.storage.index()) {
    case 0:
      return "null";
    case 1:
      return "integer(" + std::to_string(std::get<std::int64_t>(value.storage)) + ")";
    case 2: {
      std::ostringstream output;
      output << "real(" << std::setprecision(17) << std::get<double>(value.storage) << ")";
      return output.str();
    }
    case 3:
      return "text(" + std::get<std::string>(value.storage) + ")";
    case 4: {
      std::ostringstream output;
      output << "blob(";
      for (const unsigned char byte : std::get<std::vector<unsigned char>>(value.storage)) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
      }
      output << ")";
      return output.str();
    }
    default:
      return "unknown";
  }
}

[[nodiscard]] std::string DescribeOutcome(const TraceOutcome& outcome) {
  std::ostringstream output;
  output << "primary=" << outcome.primary_code << ", finalize=";
  if (outcome.finalize_primary_code.has_value()) {
    output << *outcome.finalize_primary_code;
  } else {
    output << "none";
  }
  output << ", columns=" << outcome.column_count << ", changes=" << outcome.changes
         << ", last_insert_rowid=" << outcome.last_insert_rowid
         << ", autocommit=" << outcome.autocommit << ", rows=[";
  for (std::size_t row = 0; row < outcome.rows.size(); ++row) {
    if (row != 0) {
      output << ",";
    }
    output << "[";
    for (std::size_t column = 0; column < outcome.rows[row].size(); ++column) {
      if (column != 0) {
        output << ",";
      }
      output << DescribeValue(outcome.rows[row][column]);
    }
    output << "]";
  }
  output << "]";
  return output.str();
}

void CompareOutcomes(std::size_t index, std::string_view sql, const TraceOutcome& modern,
                     const TraceOutcome& sqlite) {
  if (modern == sqlite) {
    return;
  }
  throw std::runtime_error{"differential trace mismatch at statement " + std::to_string(index + 1) +
                           ": " + std::string{sql} + "\nModern: " + DescribeOutcome(modern) +
                           "\nSQLite: " + DescribeOutcome(sqlite)};
}

[[nodiscard]] std::uint32_t ReadSchemaCookie(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw std::runtime_error{"failed to open database header"};
  }
  input.seekg(40);
  std::array<unsigned char, 4> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error{"failed to read database schema cookie"};
  }
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

void VerifyDifferentialTrace(const std::filesystem::path& modern_path,
                             const std::filesystem::path& sqlite_path) {
  const std::vector<TraceCase> trace{
      {
          .sql = "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT NOT NULL DEFAULT 'seed', "
                 "Score REAL, Payload BLOB)",
          .schema_cookie = 1,
      },
      {
          .sql = "SELECT type,name,tbl_name,rootpage,sql FROM sqlite_schema WHERE name='Items'",
          .schema_cookie = 1,
      },
      {
          .sql = "CREATE TABLE IF NOT EXISTS Items(ignored TEXT)",
          .schema_cookie = 1,
      },
      {.sql = "INSERT INTO Items DEFAULT VALUES"},
      {.sql = "INSERT INTO Items VALUES(5,'five',5,x'00FF')"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "INSERT INTO Items VALUES(5,'duplicate',9,x'01')"},
      {.sql = "INSERT INTO Items(id,Name) VALUES(6,NULL)"},
      {.sql = "INSERT INTO Items(rowid,Name) VALUES('not-rowid','bad')"},
      {.sql = "INSERT INTO Items(id,Name) VALUES(7.5,'fractional')"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "UPDATE Items SET Name='exact',Score=2 WHERE id=1"},
      {.sql = "UPDATE Items SET id=id+10,Name=Name||'x' WHERE Score>=2"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "UPDATE Items SET id=12.5 WHERE id=11"},
      {.sql = "UPDATE Items SET id=15 WHERE id=11"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "DELETE FROM Items WHERE id=11"},
      {.sql = "DELETE FROM Items WHERE Score>=5"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "BEGIN"},
      {.sql = "INSERT INTO Items VALUES(20,'kept',20,x'20')"},
      {.sql = "INSERT INTO Items VALUES(20,'duplicate',21,x'21')"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "COMMIT"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "BEGIN IMMEDIATE"},
      {.sql = "UPDATE Items SET Name='rolled back' WHERE id=20"},
      {.sql = "INSERT INTO Items VALUES(21,'temporary',NULL,NULL)"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "ROLLBACK"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "SAVEPOINT outer"},
      {.sql = "INSERT INTO Items VALUES(30,'savepoint',30,NULL)"},
      {.sql = "SAVEPOINT inner"},
      {.sql = "UPDATE Items SET Name='inner' WHERE id=30"},
      {.sql = "SELECT id,Name FROM Items"},
      {.sql = "ROLLBACK TO inner"},
      {.sql = "SELECT id,Name FROM Items"},
      {.sql = "RELEASE inner"},
      {.sql = "ROLLBACK TO outer"},
      {.sql = "SELECT id,Name FROM Items"},
      {.sql = "RELEASE outer"},
      {.sql = "SELECT id,Name,Score,Payload FROM Items"},
      {.sql = "BEGIN"},
      {.sql = "CREATE TABLE Temp(id INTEGER PRIMARY KEY)"},
      {.sql = "SELECT type,name,tbl_name,rootpage,sql FROM sqlite_schema WHERE name='Temp'"},
      {.sql = "INSERT INTO Temp DEFAULT VALUES"},
      {.sql = "SELECT id FROM Temp"},
      {.sql = "ROLLBACK", .schema_cookie = 1},
      {
          .sql = "SELECT type,name,tbl_name,rootpage,sql FROM sqlite_schema WHERE name='Temp'",
          .schema_cookie = 1,
      },
      {.sql = "SELECT id,Name,Score,Payload FROM Items", .schema_cookie = 1},
      {
          .sql = "SELECT type,name,tbl_name,rootpage,sql FROM sqlite_schema WHERE name='Items'",
          .schema_cookie = 1,
      },
  };

  {
    modern_sqlite::WriteSession modern =
        TakeValue(modern_sqlite::WriteSession::Open(modern_path.string()));
    const Database sqlite{sqlite_path, kCreateFlags};
    for (std::size_t index = 0; index < trace.size(); ++index) {
      const TraceOutcome modern_outcome = RunModernStatement(modern, trace[index].sql);
      const TraceOutcome sqlite_outcome = RunSqliteStatement(sqlite.get(), trace[index].sql);
      CompareOutcomes(index, trace[index].sql, modern_outcome, sqlite_outcome);
      if (trace[index].schema_cookie.has_value()) {
        const std::uint32_t expected_cookie =
            TakeOptional(trace[index].schema_cookie, "trace schema cookie is missing");
        const std::uint32_t modern_cookie = ReadSchemaCookie(modern_path);
        const std::uint32_t sqlite_cookie = ReadSchemaCookie(sqlite_path);
        if (modern_cookie != expected_cookie || sqlite_cookie != expected_cookie) {
          throw std::runtime_error{"schema cookie mismatch after statement " +
                                   std::to_string(index + 1)};
        }
      }
    }
  }

  const Database modern_sqlite{modern_path, kReadOnlyFlags};
  const Database reference_sqlite{sqlite_path, kReadOnlyFlags};
  VerifyIntegrity(modern_sqlite.get());
  VerifyIntegrity(reference_sqlite.get());
}

void VerifyModernCreated(const std::filesystem::path& path) {
  {
    modern_sqlite::WriteSession session =
        TakeValue(modern_sqlite::WriteSession::Open(path.string()));
    ExecuteModern(session,
                  "CREATE TABLE Items("
                  "id INTEGER PRIMARY KEY, "
                  "Name TEXT NOT NULL DEFAULT 'seed', "
                  "Score REAL"
                  ")");
    ExecuteModern(session, "INSERT INTO Items DEFAULT VALUES");
    ExecuteModern(session, "INSERT INTO Items VALUES(5,'five',5)");
    ExecuteModern(session, "UPDATE Items SET id=id+10,Name=Name||'x' WHERE Score>=5");
    ExecuteModern(session, "DELETE FROM Items WHERE id=1");
    ExecuteModern(session, "BEGIN");
    ExecuteModern(session, "INSERT INTO Items VALUES(20,'rollback',20)");
    ExecuteModern(session, "ROLLBACK");
    if (session.changes() != 1U || session.last_insert_rowid() != 20 || !session.autocommit()) {
      throw std::runtime_error{"Modern connection state disagrees after rollback"};
    }
  }

  const Database sqlite{path, kReadOnlyFlags};
  VerifyIntegrity(sqlite.get());
  if (QueryInteger(sqlite.get(), "SELECT count(*) FROM Items") != 1 ||
      QueryInteger(sqlite.get(), "SELECT id FROM Items") != 15 ||
      QueryText(sqlite.get(), "SELECT Name FROM Items") != "fivex" ||
      QueryText(sqlite.get(), "SELECT sql FROM sqlite_schema WHERE name='Items'") !=
          "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT NOT NULL DEFAULT 'seed', Score "
          "REAL)") {
    throw std::runtime_error{"SQLite disagrees with Modern-created database"};
  }
}

void VerifySqliteCreated(const std::filesystem::path& path) {
  {
    const Database sqlite{path, kCreateFlags};
    ExecuteSqlite(sqlite.get(),
                  "CREATE TABLE Items("
                  "id INTEGER PRIMARY KEY, "
                  "Name TEXT NOT NULL DEFAULT 'seed', "
                  "Score REAL"
                  ");"
                  "INSERT INTO Items VALUES(1,'alpha',1);"
                  "INSERT INTO Items VALUES(2,'beta',2);"
                  "INSERT INTO Items VALUES(3,'gamma',3);");
  }

  {
    modern_sqlite::WriteSession session =
        TakeValue(modern_sqlite::WriteSession::Open(path.string()));
    ExecuteModern(session, "UPDATE Items SET id=id+10 WHERE Score>=2");
    ExecuteModern(session, "DELETE FROM Items WHERE id=1");
    ExecuteModern(session, "INSERT INTO Items DEFAULT VALUES");
    if (session.last_insert_rowid() != 14 || session.changes() != 1U) {
      throw std::runtime_error{"Modern connection state disagrees for SQLite-created database"};
    }
    ExecuteModern(session, "SAVEPOINT s");
    ExecuteModern(session, "INSERT INTO Items VALUES(20,'rollback',20)");
    ExecuteModern(session, "ROLLBACK TO s");
    ExecuteModern(session, "RELEASE s");
  }

  const Database sqlite{path, kReadOnlyFlags};
  VerifyIntegrity(sqlite.get());
  if (QueryInteger(sqlite.get(), "SELECT count(*) FROM Items") != 3 ||
      QueryText(sqlite.get(),
                "SELECT group_concat(id,',') FROM (SELECT id FROM Items ORDER BY id)") !=
          "12,13,14" ||
      QueryText(sqlite.get(), "SELECT Name FROM Items WHERE id=12") != "beta" ||
      QueryText(sqlite.get(), "SELECT Name FROM Items WHERE id=14") != "seed") {
    throw std::runtime_error{"SQLite disagrees after Modern mutated SQLite-created database"};
  }
}

void InsertBoundValues(modern_sqlite::WriteSession& session) {
  modern_sqlite::WriteStatement statement =
      Prepare(session, "INSERT INTO Items(id,Name,Score,Payload) VALUES(?1,?2,?3,?4)");
  const std::array<std::byte, 2> payload{std::byte{0x00}, std::byte{0xff}};
  RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(30)));
  RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Text(std::string{"nul\0text", 8})));
  RequireStatus(statement.Bind(3, modern_sqlite::SqlValue::Integer(7)));
  RequireStatus(
      statement.Bind(4, modern_sqlite::SqlValue::Blob(modern_sqlite::ByteBuffer::CopyOf(payload))));
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone) {
    throw std::runtime_error{"bound compatibility INSERT produced a row"};
  }
  RequireStatus(statement.Finalize());
}

void InsertNamedRow(modern_sqlite::WriteSession& session, std::int64_t rowid, std::string name) {
  modern_sqlite::WriteStatement statement =
      Prepare(session, "INSERT INTO Items(id,Name) VALUES(?1,?2)");
  RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
  RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Text(std::move(name))));
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone) {
    throw std::runtime_error{"boundary compatibility INSERT produced a row"};
  }
  RequireStatus(statement.Finalize());
}

void VerifyModernEmbeddedValue(const std::filesystem::path& path) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement = Prepare(session,
                                                    "SELECT Name,Score,Payload FROM Items "
                                                    "WHERE id=30");
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kRow ||
      statement.row().size() != 3U) {
    throw std::runtime_error{"Modern did not return the embedded compatibility row"};
  }
  const std::array<std::byte, 2> expected_payload{std::byte{0x00}, std::byte{0xff}};
  const modern_sqlite::ByteView actual_payload =
      TakeOptional(statement.row()[2].blob_value(), "embedded payload is missing");
  if (TakeOptional(statement.row()[0].text_value(), "embedded name is missing").bytes() !=
          std::string_view{"nul\0text", 8} ||
      TakeOptional(statement.row()[1].real_value(), "embedded score is missing") != 7.0 ||
      !std::ranges::equal(actual_payload, expected_payload) ||
      TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone) {
    throw std::runtime_error{"Modern disagrees after the page-size handoff"};
  }
  RequireStatus(statement.Finalize());
}

void VerifyOnePageSize(const TemporaryDirectory& directory, int page_size) {
  const std::filesystem::path path =
      directory.DatabasePath("page-size-" + std::to_string(page_size));
  {
    const Database sqlite{path, kCreateFlags};
    ExecuteSqlite(sqlite.get(), "PRAGMA page_size=" + std::to_string(page_size));
    ExecuteSqlite(sqlite.get(), "PRAGMA journal_mode=DELETE");
    ExecuteSqlite(sqlite.get(),
                  "CREATE TABLE Items("
                  "id INTEGER PRIMARY KEY,"
                  "Name TEXT NOT NULL DEFAULT 'seed',"
                  "Score REAL,"
                  "Payload BLOB"
                  ")");
    ExecuteSqlite(sqlite.get(), "INSERT INTO Items VALUES(-1,'negative',-1,x'00')");
    ExecuteSqlite(sqlite.get(), "INSERT INTO Items VALUES(0,'zero',0,NULL)");
    ExecuteSqlite(sqlite.get(), "INSERT INTO Items VALUES(2,'overflow',2,zeroblob(70000))");
    if (QueryInteger(sqlite.get(), "PRAGMA page_size") != page_size) {
      throw std::runtime_error{"SQLite did not create the requested page size"};
    }
  }

  {
    modern_sqlite::WriteSession session =
        TakeValue(modern_sqlite::WriteSession::Open(path.string()));
    ExecuteModern(session, "INSERT INTO Items DEFAULT VALUES");
    ExecuteModern(session, "UPDATE Items SET id=id+10,Name=Name||'x' WHERE id>=0");
    ExecuteModern(session, "DELETE FROM Items WHERE id=10");
    ExecuteModern(session, "BEGIN");
    ExecuteModern(session, "INSERT INTO Items VALUES(20,'rollback',20,NULL)");
    ExecuteModern(session, "ROLLBACK");
    ExecuteModern(session, "SAVEPOINT s");
    ExecuteModern(session, "INSERT INTO Items VALUES(21,'savepoint',21,NULL)");
    ExecuteModern(session, "ROLLBACK TO s");
    ExecuteModern(session, "RELEASE s");
    InsertNamedRow(session, std::numeric_limits<std::int64_t>::min(), "minimum");
    InsertNamedRow(session, std::numeric_limits<std::int64_t>::max(), "maximum");
    InsertBoundValues(session);
    if (session.changes() != 1U || session.last_insert_rowid() != 30 || !session.autocommit()) {
      throw std::runtime_error{"Modern connection state differs in the page-size matrix"};
    }
  }

  {
    const Database sqlite{path, kReadOnlyFlags};
    VerifyIntegrity(sqlite.get());
    if (QueryInteger(sqlite.get(), "PRAGMA page_size") != page_size ||
        QueryText(sqlite.get(),
                  "SELECT group_concat(id,',') FROM (SELECT id FROM Items ORDER BY id)") !=
            "-9223372036854775808,-1,12,13,30,9223372036854775807" ||
        QueryInteger(sqlite.get(), "SELECT length(Payload) FROM Items WHERE id=12") != 70000 ||
        QueryText(sqlite.get(), "SELECT hex(Name) FROM Items WHERE id=30") != "6E756C0074657874" ||
        QueryText(sqlite.get(), "SELECT typeof(Score) FROM Items WHERE id=30") != "real" ||
        QueryText(sqlite.get(), "SELECT hex(Payload) FROM Items WHERE id=30") != "00FF" ||
        QueryText(sqlite.get(), "SELECT typeof(Payload) FROM Items WHERE id=13") != "null" ||
        QueryText(sqlite.get(), "SELECT Name FROM Items WHERE id=-9223372036854775808") !=
            "minimum" ||
        QueryText(sqlite.get(), "SELECT Name FROM Items WHERE id=9223372036854775807") !=
            "maximum" ||
        QueryInteger(sqlite.get(), "SELECT count(*) FROM Items WHERE id IN(20,21)") != 0) {
      throw std::runtime_error{"SQLite disagrees in the public page-size matrix"};
    }
  }
  VerifyModernEmbeddedValue(path);
}

void VerifyAlternatingOwnership(const std::filesystem::path& path) {
  {
    modern_sqlite::WriteSession session =
        TakeValue(modern_sqlite::WriteSession::Open(path.string()));
    ExecuteModern(session,
                  "CREATE TABLE Items("
                  "id INTEGER PRIMARY KEY,"
                  "Name TEXT NOT NULL,"
                  "Score REAL,"
                  "Payload BLOB"
                  ")");
    ExecuteModern(session, "INSERT INTO Items VALUES(1,'one',1,x'01')");
    ExecuteModern(session, "INSERT INTO Items VALUES(2,'two',2,x'02')");
  }
  {
    const Database sqlite{path, kCreateFlags};
    ExecuteSqlite(sqlite.get(), "INSERT INTO Items VALUES(3,'three',3,x'03')");
    ExecuteSqlite(sqlite.get(), "UPDATE Items SET Name='sqlite' WHERE id=1");
    ExecuteSqlite(sqlite.get(), "DELETE FROM Items WHERE id=2");
    ExecuteSqlite(sqlite.get(), "BEGIN");
    ExecuteSqlite(sqlite.get(), "INSERT INTO Items VALUES(4,'rollback',4,x'04')");
    ExecuteSqlite(sqlite.get(), "ROLLBACK");
    VerifyIntegrity(sqlite.get());
  }
  {
    modern_sqlite::WriteSession session =
        TakeValue(modern_sqlite::WriteSession::Open(path.string()));
    ExecuteModern(session, "UPDATE Items SET id=13,Name=Name||'x' WHERE id=3");
    ExecuteModern(session, "SAVEPOINT s");
    ExecuteModern(session, "INSERT INTO Items VALUES(5,'rollback',5,x'05')");
    ExecuteModern(session, "ROLLBACK TO s");
    ExecuteModern(session, "RELEASE s");
    InsertBoundValues(session);
  }
  {
    const Database sqlite{path, kReadOnlyFlags};
    VerifyIntegrity(sqlite.get());
    if (QueryText(sqlite.get(),
                  "SELECT group_concat(id,',') FROM (SELECT id FROM Items ORDER BY id)") !=
            "1,13,30" ||
        QueryText(sqlite.get(), "SELECT Name FROM Items WHERE id=1") != "sqlite" ||
        QueryText(sqlite.get(), "SELECT Name FROM Items WHERE id=13") != "threex" ||
        QueryText(sqlite.get(), "SELECT hex(Payload) FROM Items WHERE id=13") != "03") {
      throw std::runtime_error{"alternating engine ownership produced the wrong final state"};
    }
  }
  VerifyModernEmbeddedValue(path);
}

void VerifyUnsupportedMutationBoundaries(const std::filesystem::path& path) {
  {
    const Database sqlite{path, kCreateFlags};
    ExecuteSqlite(sqlite.get(),
                  "CREATE TABLE Indexed(id INTEGER PRIMARY KEY,value TEXT UNIQUE);"
                  "INSERT INTO Indexed VALUES(1,'one');"
                  "CREATE TABLE Wr(key TEXT PRIMARY KEY,value TEXT) WITHOUT ROWID;"
                  "INSERT INTO Wr VALUES('key','value');");
    VerifyIntegrity(sqlite.get());
  }
  {
    modern_sqlite::WriteSession session =
        TakeValue(modern_sqlite::WriteSession::Open(path.string()));
    ExecuteModern(session, "INSERT INTO Indexed VALUES(2,'two')");
    for (const auto& [sql, expected_code] : std::array{
             std::pair{"UPDATE Indexed SET value='changed' WHERE id=1",
                       modern_sqlite::ErrorCode::kProtocol},
             std::pair{"DELETE FROM Wr WHERE key='key'", modern_sqlite::ErrorCode::kGeneric},
         }) {
      const auto prepared = session.Prepare(modern_sqlite::Utf8View{sql});
      if (prepared.has_value() || prepared.error().code() != expected_code) {
        throw std::runtime_error{"Modern accepted an unsupported mutation"};
      }
    }
  }
  const Database sqlite{path, kReadOnlyFlags};
  VerifyIntegrity(sqlite.get());
  if (QueryText(sqlite.get(),
                "SELECT group_concat(id||':'||value,',') "
                "FROM (SELECT id,value FROM Indexed INDEXED BY sqlite_autoindex_Indexed_1 "
                "ORDER BY value)") != "1:one,2:two" ||
      QueryText(sqlite.get(), "SELECT value FROM Wr WHERE key='key'") != "value") {
    throw std::runtime_error{"indexed INSERT or unsupported mutation produced the wrong contents"};
  }
}

constexpr std::array<std::string_view, 12> kCrashCaseIds{
    "implicit-insert",
    "implicit-create",
    "exact-rowid-move",
    "scan-rowid-move",
    "scan-delete",
    "explicit-commit",
    "constraint-then-commit",
    "named-rollback-then-commit",
    "transaction-savepoint-release",
    "create-full-rollback",
    "create-rollback-to",
    "full-dml-rollback",
};

constexpr std::array<int, 8> kPageSizes{
    512, 1024, 2048, 4096, 8192, 16384, 32768, 65536,
};

constexpr std::array<std::string_view, 5> kNonPageCaseIds{
    "differential-trace",    "modern-created",         "sqlite-created",
    "alternating-ownership", "unsupported-boundaries",
};

constexpr std::array<std::string_view, 25> kAllCaseIds{
    "alternating-ownership",  "constraint-then-commit", "create-full-rollback",
    "create-rollback-to",     "differential-trace",     "exact-rowid-move",
    "explicit-commit",        "full-dml-rollback",      "implicit-create",
    "implicit-insert",        "modern-created",         "named-rollback-then-commit",
    "page-size-1024",         "page-size-16384",        "page-size-2048",
    "page-size-32768",        "page-size-4096",         "page-size-512",
    "page-size-65536",        "page-size-8192",         "scan-delete",
    "scan-rowid-move",        "sqlite-created",         "transaction-savepoint-release",
    "unsupported-boundaries",
};

struct CommandLineOptions {
  std::optional<std::string_view> case_id{};
  std::optional<std::size_t> cut{};
  std::optional<bool> writes_are_durable{};
};

[[nodiscard]] std::string ShellQuote(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted.append("'\\''");
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

[[nodiscard]] bool IsCrashCase(std::string_view case_id) {
  return std::ranges::find(kCrashCaseIds, case_id) != kCrashCaseIds.end();
}

[[nodiscard]] std::optional<int> PageSizeCase(std::string_view case_id) {
  for (const int page_size : kPageSizes) {
    if (case_id == "page-size-" + std::to_string(page_size)) {
      return page_size;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool IsNonCrashCase(std::string_view case_id) {
  return std::ranges::find(kNonPageCaseIds, case_id) != kNonPageCaseIds.end() ||
         PageSizeCase(case_id).has_value();
}

[[nodiscard]] CommandLineOptions ParseCommandLine(int argument_count, char* const* arguments) {
  CommandLineOptions options;
  for (int index = 1; index < argument_count; ++index) {
    const std::string_view argument = arguments[index];
    const auto next_value = [&]() -> std::string_view {
      if (index + 1 >= argument_count) {
        throw std::runtime_error{std::string{argument} + " requires a value"};
      }
      return arguments[++index];
    };
    if (argument == "--case") {
      if (options.case_id.has_value()) {
        throw std::runtime_error{"--case may be specified only once"};
      }
      options.case_id = next_value();
    } else if (argument == "--cut") {
      if (options.cut.has_value()) {
        throw std::runtime_error{"--cut may be specified only once"};
      }
      const std::string_view value = next_value();
      std::size_t cut = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), cut);
      if (error != std::errc{} || end != value.data() + value.size() || cut == 0U) {
        throw std::runtime_error{"--cut requires a positive decimal integer"};
      }
      options.cut = cut;
    } else if (argument == "--durability") {
      if (options.writes_are_durable.has_value()) {
        throw std::runtime_error{"--durability may be specified only once"};
      }
      const std::string_view value = next_value();
      if (value == "volatile") {
        options.writes_are_durable = false;
      } else if (value == "durable") {
        options.writes_are_durable = true;
      } else {
        throw std::runtime_error{"--durability requires volatile or durable"};
      }
    } else {
      throw std::runtime_error{"unknown argument: " + std::string{argument}};
    }
  }

  if (!options.case_id.has_value()) {
    if (options.cut.has_value() || options.writes_are_durable.has_value()) {
      throw std::runtime_error{"--cut and --durability require --case"};
    }
    return options;
  }
  if (!IsCrashCase(options.case_id.value()) && !IsNonCrashCase(options.case_id.value())) {
    throw std::runtime_error{"unknown compatibility case: " + std::string{options.case_id.value()}};
  }
  if (!IsCrashCase(options.case_id.value()) &&
      (options.cut.has_value() || options.writes_are_durable.has_value())) {
    throw std::runtime_error{"--cut and --durability apply only to crash cases"};
  }
  return options;
}

void RunCrashCases(const TemporaryDirectory& directory, const CommandLineOptions& options,
                   std::string_view executable) {
  CrashVerifierContext crash_context{.directory = &directory};
  const modern_sqlite::test::WriteSessionCrashVerification crash_verification{
      .context = &crash_context,
      .verify = VerifyCrashImageWithSqlite,
      .scenario_filter = options.case_id.value_or(std::string_view{}),
      .cut_filter = options.cut,
      .durability_filter = options.writes_are_durable,
      .executable = executable,
  };
  modern_sqlite::test::RunWriteSessionCrashHarness(crash_verification);
  modern_sqlite::test::RunWriteSessionTransactionCrashHarness(crash_verification);
}

void RunNonCrashCase(std::string_view case_id, const TemporaryDirectory& directory) {
  if (case_id == "differential-trace") {
    VerifyDifferentialTrace(directory.DatabasePath("modern-trace"),
                            directory.DatabasePath("sqlite-trace"));
  } else if (case_id == "modern-created") {
    VerifyModernCreated(directory.DatabasePath("modern-created"));
  } else if (case_id == "sqlite-created") {
    VerifySqliteCreated(directory.DatabasePath("sqlite-created"));
  } else if (case_id == "alternating-ownership") {
    VerifyAlternatingOwnership(directory.DatabasePath("alternating-ownership"));
  } else if (case_id == "unsupported-boundaries") {
    VerifyUnsupportedMutationBoundaries(directory.DatabasePath("unsupported-boundaries"));
  } else {
    VerifyOnePageSize(
        directory, TakeOptional(PageSizeCase(case_id), "page-size compatibility case is invalid"));
  }
}

void RunNamedNonCrashCase(std::string_view case_id, const TemporaryDirectory& directory,
                          std::string_view executable) {
  try {
    RunNonCrashCase(case_id, directory);
  } catch (const std::exception& error) {
    throw std::runtime_error{"case " + std::string{case_id} + ": " + error.what() +
                             "\nreproduce: " + ShellQuote(executable) + " --case " +
                             std::string{case_id}};
  }
}

void RunAllCases(const TemporaryDirectory& directory, std::string_view executable) {
  if (!std::ranges::is_sorted(kAllCaseIds) ||
      std::ranges::adjacent_find(kAllCaseIds) != kAllCaseIds.end()) {
    throw std::runtime_error{"compatibility case registry is not unique lexical order"};
  }
  for (const std::string_view case_id : kAllCaseIds) {
    if (IsCrashCase(case_id)) {
      CommandLineOptions selected;
      selected.case_id = case_id;
      RunCrashCases(directory, selected, executable);
    } else {
      RunNamedNonCrashCase(case_id, directory, executable);
    }
  }
}

}  // namespace

int main(int argument_count, char* const* arguments) try {
  const CommandLineOptions options = ParseCommandLine(argument_count, arguments);
  const TemporaryDirectory directory;
  const std::string_view executable = arguments[0];
  if (!options.case_id.has_value()) {
    RunAllCases(directory, executable);
  } else if (IsCrashCase(options.case_id.value())) {
    RunCrashCases(directory, options, executable);
  } else {
    RunNamedNonCrashCase(options.case_id.value(), directory, executable);
  }
  return 0;
} catch (const std::exception& error) {
  static_cast<void>(std::fprintf(stderr, "%s\n", error.what()));
  return 1;
}
