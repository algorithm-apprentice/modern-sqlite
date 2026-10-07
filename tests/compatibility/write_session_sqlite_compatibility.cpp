#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"

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

}  // namespace

int main() try {
  const TemporaryDirectory directory;
  VerifyDifferentialTrace(directory.DatabasePath("modern-trace"),
                          directory.DatabasePath("sqlite-trace"));
  VerifyModernCreated(directory.DatabasePath("modern-created"));
  VerifySqliteCreated(directory.DatabasePath("sqlite-created"));
  return 0;
} catch (const std::exception& error) {
  static_cast<void>(std::fprintf(stderr, "%s\n", error.what()));
  return 1;
}
