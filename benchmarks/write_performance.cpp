#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"

#if !defined(NDEBUG)
#error "The write performance benchmark requires NDEBUG"
#endif

#if !defined(_MSC_VER) && !defined(__OPTIMIZE__)
#error "The write performance benchmark requires compiler optimization"
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    defined(__SANITIZE_UNDEFINED__) || defined(__COVERAGE__) || defined(__GCOV__)
#error "The write performance benchmark forbids sanitizers and coverage"
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#error "The write performance benchmark forbids sanitizers"
#endif
#endif

namespace {

constexpr int kSqliteOpenFlags = static_cast<int>(static_cast<unsigned int>(SQLITE_OPEN_READWRITE) |
                                                  static_cast<unsigned int>(SQLITE_OPEN_CREATE) |
                                                  static_cast<unsigned int>(SQLITE_OPEN_NOMUTEX));
constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ULL;
constexpr std::size_t kValueSize = 256;
constexpr std::string_view kInsertSql = "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)";
constexpr std::string_view kSqliteVersion = "3.54.0";
constexpr std::string_view kSqliteSourceId =
    "2026-10-02 20:18:07 "
    "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2";

[[nodiscard]] sqlite3_destructor_type SqliteTransient() noexcept {
  // SQLite defines SQLITE_TRANSIENT as the function-pointer sentinel -1.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));
}

class HarnessFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class BenchmarkMismatch final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

template <typename T>
[[nodiscard]] T TakeValue(modern_sqlite::Result<T> result) {
  if (!result.has_value()) {
    throw HarnessFailure{result.error().ToString()};
  }
  return std::move(*result);
}

void RequireStatus(modern_sqlite::Status status) {
  if (!status.has_value()) {
    throw HarnessFailure{status.error().ToString()};
  }
}

class SqliteDatabase final {
 public:
  explicit SqliteDatabase(const std::filesystem::path& path) {
    const int result =
        sqlite3_open_v2(path.string().c_str(), &database_, kSqliteOpenFlags, nullptr);
    if (result != SQLITE_OK) {
      const std::string message =
          database_ == nullptr ? "sqlite3_open_v2 failed" : sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        static_cast<void>(sqlite3_close(database_));
        database_ = nullptr;
      }
      throw HarnessFailure{message};
    }
  }

  SqliteDatabase(const SqliteDatabase&) = delete;
  SqliteDatabase& operator=(const SqliteDatabase&) = delete;

  ~SqliteDatabase() noexcept {
    if (database_ != nullptr) {
      static_cast<void>(sqlite3_close(database_));
    }
  }

  [[nodiscard]] sqlite3* get() const noexcept { return database_; }

  void Close() {
    if (database_ != nullptr && sqlite3_close(database_) != SQLITE_OK) {
      throw HarnessFailure{"sqlite3_close failed"};
    }
    database_ = nullptr;
  }

 private:
  sqlite3* database_ = nullptr;
};

class SqliteStatement final {
 public:
  SqliteStatement(sqlite3* database, std::string_view sql) {
    const std::string owned{sql};
    const int result = sqlite3_prepare_v3(database, owned.c_str(), -1, SQLITE_PREPARE_PERSISTENT,
                                          &statement_, nullptr);
    if (result != SQLITE_OK) {
      throw HarnessFailure{sqlite3_errmsg(database)};
    }
  }

  SqliteStatement(const SqliteStatement&) = delete;
  SqliteStatement& operator=(const SqliteStatement&) = delete;

  ~SqliteStatement() noexcept {
    if (statement_ != nullptr) {
      static_cast<void>(sqlite3_finalize(statement_));
    }
  }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

  void Finalize() {
    if (statement_ != nullptr && sqlite3_finalize(statement_) != SQLITE_OK) {
      throw HarnessFailure{"sqlite3_finalize failed"};
    }
    statement_ = nullptr;
  }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

class Digest final {
 public:
  void AddByte(std::uint8_t value) noexcept {
    value_ ^= value;
    value_ *= kFnvPrime;
  }

  void AddInteger(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < sizeof(bits); ++index) {
      AddByte(static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xffU));
    }
  }

  void AddBytes(modern_sqlite::ByteView bytes) noexcept {
    for (const std::byte byte : bytes) {
      AddByte(std::to_integer<std::uint8_t>(byte));
    }
  }

  [[nodiscard]] std::string Hex() const {
    std::array<char, 17> output{};
    const auto [end, error] = std::to_chars(output.data(), output.data() + 16, value_, 16);
    if (error != std::errc{}) {
      throw HarnessFailure{"cannot render result digest"};
    }
    const auto digits = static_cast<std::size_t>(end - output.data());
    std::string result(16U - digits, '0');
    result.append(output.data(), digits);
    return result;
  }

 private:
  std::uint64_t value_ = kFnvOffset;
};

[[nodiscard]] modern_sqlite::ByteBuffer ValueFor(std::int64_t rowid) {
  modern_sqlite::ByteBuffer value{modern_sqlite::ByteCount{kValueSize}};
  std::ranges::fill(value.mutable_view(), std::byte{'0'});
  std::array<char, 16> digits{};
  const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), rowid, 16);
  if (error != std::errc{}) {
    throw HarnessFailure{"cannot render benchmark value"};
  }
  const auto count = static_cast<std::size_t>(end - digits.data());
  if (count > 8U) {
    throw HarnessFailure{"benchmark rowid exceeds the value prefix"};
  }
  const std::size_t offset = 8U - count;
  for (std::size_t index = 0; index < count; ++index) {
    value.mutable_view()[offset + index] =
        static_cast<std::byte>(static_cast<std::uint8_t>(digits[index]));
  }
  return value;
}

void ExecuteSqlite(sqlite3* database, std::string_view sql) {
  const std::string owned{sql};
  char* error = nullptr;
  if (sqlite3_exec(database, owned.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(database) : std::string{error};
    sqlite3_free(error);
    throw HarnessFailure{message};
  }
}

[[nodiscard]] std::int64_t SqliteSingleInteger(sqlite3* database, std::string_view sql) {
  SqliteStatement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
    throw HarnessFailure{"SQLite configuration query returned no integer"};
  }
  const std::int64_t value = sqlite3_column_int64(statement.get(), 0);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw HarnessFailure{"SQLite configuration query returned extra rows"};
  }
  statement.Finalize();
  return value;
}

[[nodiscard]] std::string SqliteSingleText(sqlite3* database, std::string_view sql) {
  SqliteStatement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
    throw HarnessFailure{"SQLite configuration query returned no text"};
  }
  const unsigned char* text = sqlite3_column_text(statement.get(), 0);
  const int bytes = sqlite3_column_bytes(statement.get(), 0);
  if (text == nullptr || bytes < 0) {
    throw HarnessFailure{"SQLite configuration query returned invalid text"};
  }
  const std::string value{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw HarnessFailure{"SQLite configuration query returned extra rows"};
  }
  statement.Finalize();
  return value;
}

void ConfigureMatchedSqlite(sqlite3* database) {
  if (std::string_view{sqlite3_libversion()} != kSqliteVersion ||
      std::string_view{sqlite3_sourceid()} != kSqliteSourceId) {
    throw HarnessFailure{"write benchmark SQLite identity is not pinned"};
  }
  ExecuteSqlite(database, "PRAGMA cache_size=512");
  ExecuteSqlite(database, "PRAGMA mmap_size=0");
  ExecuteSqlite(database, "PRAGMA temp_store=MEMORY");
  ExecuteSqlite(database, "PRAGMA journal_mode=DELETE");
  ExecuteSqlite(database, "PRAGMA synchronous=FULL");
  ExecuteSqlite(database, "PRAGMA locking_mode=NORMAL");
  if (SqliteSingleInteger(database, "PRAGMA page_size") != 4096 ||
      SqliteSingleInteger(database, "PRAGMA cache_size") != 512 ||
      SqliteSingleInteger(database, "PRAGMA mmap_size") != 0 ||
      SqliteSingleInteger(database, "PRAGMA temp_store") != 2 ||
      SqliteSingleText(database, "PRAGMA journal_mode") != "delete" ||
      SqliteSingleInteger(database, "PRAGMA synchronous") != 2 ||
      SqliteSingleText(database, "PRAGMA locking_mode") != "normal") {
    throw HarnessFailure{"write benchmark SQLite matched configuration differs"};
  }
}

[[nodiscard]] std::uint64_t InsertModern(const std::filesystem::path& path,
                                         std::size_t operations) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WritePrepareOutput prepared =
      TakeValue(session.Prepare(modern_sqlite::Utf8View{kInsertSql}));
  if (!prepared.statement.has_value()) {
    throw HarnessFailure{"Modern INSERT preparation produced no statement"};
  }
  modern_sqlite::WriteStatement statement = std::move(*prepared.statement);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<std::int64_t>(index) + 1;
    const modern_sqlite::SqlValue value = modern_sqlite::SqlValue::Blob(ValueFor(rowid));
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    RequireStatus(statement.Bind(2, value));
    if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone || session.changes() != 1U ||
        session.last_insert_rowid() != rowid) {
      throw BenchmarkMismatch{"Modern INSERT completion differs"};
    }
    ++changed_rows;
    RequireStatus(statement.Reset());
  }
  RequireStatus(statement.Finalize());
  return changed_rows;
}

[[nodiscard]] std::uint64_t InsertSqlite(const std::filesystem::path& path,
                                         std::size_t operations) {
  SqliteDatabase database{path};
  ConfigureMatchedSqlite(database.get());
  SqliteStatement statement{database.get(), kInsertSql};
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<sqlite3_int64>(index) + 1;
    const modern_sqlite::ByteBuffer value = ValueFor(rowid);
    if (sqlite3_bind_int64(statement.get(), 1, rowid) != SQLITE_OK ||
        sqlite3_bind_blob(statement.get(), 2, value.view().data(),
                          static_cast<int>(value.view().size()), SqliteTransient()) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_DONE || sqlite3_changes64(database.get()) != 1 ||
        sqlite3_last_insert_rowid(database.get()) != rowid ||
        sqlite3_reset(statement.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(statement.get()) != SQLITE_OK) {
      throw BenchmarkMismatch{"SQLite INSERT completion differs"};
    }
    ++changed_rows;
  }
  statement.Finalize();
  database.Close();
  return changed_rows;
}

struct Verification {
  std::size_t rows = 0;
  std::string digest;
};

[[nodiscard]] Verification VerifyWithSqlite(const std::filesystem::path& path,
                                            std::size_t expected_rows) {
  SqliteDatabase database{path};
  SqliteStatement integrity{database.get(), "PRAGMA integrity_check"};
  if (sqlite3_step(integrity.get()) != SQLITE_ROW) {
    throw BenchmarkMismatch{"SQLite integrity_check returned no row"};
  }
  const unsigned char* integrity_text = sqlite3_column_text(integrity.get(), 0);
  if (integrity_text == nullptr ||
      std::string_view{reinterpret_cast<const char*>(integrity_text)} != "ok") {
    throw BenchmarkMismatch{"SQLite integrity_check failed"};
  }
  integrity.Finalize();

  SqliteStatement statement{database.get(), "SELECT k,v,version FROM kv"};
  Digest digest;
  std::size_t rows = 0;
  while (true) {
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_BLOB ||
        sqlite3_column_type(statement.get(), 2) != SQLITE_INTEGER) {
      throw BenchmarkMismatch{"SQLite final row has the wrong shape"};
    }
    const std::int64_t rowid = sqlite3_column_int64(statement.get(), 0);
    const void* pointer = sqlite3_column_blob(statement.get(), 1);
    const int bytes = sqlite3_column_bytes(statement.get(), 1);
    const std::int64_t version = sqlite3_column_int64(statement.get(), 2);
    if (pointer == nullptr || std::cmp_not_equal(bytes, kValueSize) || version != 0 ||
        std::cmp_not_equal(rowid, rows + 1U)) {
      throw BenchmarkMismatch{"SQLite final row differs"};
    }
    const modern_sqlite::ByteBuffer expected_value = ValueFor(rowid);
    const modern_sqlite::ByteView actual_value{static_cast<const std::byte*>(pointer), kValueSize};
    if (!std::ranges::equal(actual_value, expected_value.view())) {
      throw BenchmarkMismatch{"SQLite final BLOB differs"};
    }
    digest.AddInteger(rowid);
    digest.AddBytes(actual_value);
    digest.AddInteger(version);
    ++rows;
  }
  statement.Finalize();
  database.Close();
  if (rows != expected_rows) {
    throw BenchmarkMismatch{"SQLite final row count differs"};
  }
  return Verification{.rows = rows, .digest = digest.Hex()};
}

[[nodiscard]] Verification VerifyWithModern(const std::filesystem::path& path,
                                            std::size_t expected_rows) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WritePrepareOutput prepared =
      TakeValue(session.Prepare(modern_sqlite::Utf8View{"SELECT k,v,version FROM kv"}));
  if (!prepared.statement.has_value()) {
    throw HarnessFailure{"Modern final verification produced no statement"};
  }
  modern_sqlite::WriteStatement statement = std::move(*prepared.statement);
  Digest digest;
  std::size_t rows = 0;
  while (true) {
    const modern_sqlite::WriteStep step = TakeValue(statement.Step());
    if (step == modern_sqlite::WriteStep::kDone) {
      break;
    }
    if (statement.row().size() != 3U) {
      throw BenchmarkMismatch{"Modern final row has the wrong shape"};
    }
    const std::int64_t rowid =
        statement.row()[0].integer_value().value_or(std::numeric_limits<std::int64_t>::min());
    const std::optional<modern_sqlite::ByteView> blob = statement.row()[1].blob_value();
    const std::int64_t version =
        statement.row()[2].integer_value().value_or(std::numeric_limits<std::int64_t>::min());
    const modern_sqlite::ByteBuffer expected_value = ValueFor(rowid);
    if (!blob.has_value() || blob.value().size() != kValueSize || version != 0 ||
        std::cmp_not_equal(rowid, rows + 1U) ||
        !std::ranges::equal(blob.value(), expected_value.view())) {
      throw BenchmarkMismatch{"Modern final row differs"};
    }
    digest.AddInteger(rowid);
    digest.AddBytes(blob.value());
    digest.AddInteger(version);
    ++rows;
  }
  RequireStatus(statement.Finalize());
  if (rows != expected_rows) {
    throw BenchmarkMismatch{"Modern final row count differs"};
  }
  return Verification{.rows = rows, .digest = digest.Hex()};
}

[[nodiscard]] std::size_t ParseOperations(std::string_view text) {
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0U || value > 65'536U) {
    throw HarnessFailure{"operation count must be between 1 and 65536"};
  }
  return value;
}

void EnsureNoSidecars(const std::filesystem::path& path) {
  for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
    if (std::filesystem::exists(path.string() + std::string{suffix})) {
      throw BenchmarkMismatch{"write benchmark left a sidecar"};
    }
  }
}

void PrintReport(std::string_view engine, std::size_t operations, std::uint64_t changed_rows,
                 const Verification& verification) {
  std::cout << R"({"case":"insert-point-implicit","changed_rows":)" << changed_rows
            << R"(,"digest":")" << verification.digest << R"(","engine":")" << engine
            << R"(","final_rows":)" << verification.rows << R"(,"last_insert_rowid":)" << operations
            << R"(,"mode":"smoke","operations":)" << operations
            << R"(,"profile":"matched-durable","schema_version":1,"status":"complete"})" << '\n';
}

int Run(int argument_count, char* const* arguments) {
  if (argument_count != 7) {
    throw HarnessFailure{"usage: write benchmark smoke ENGINE PROFILE CASE DATABASE OPERATIONS"};
  }
  const std::string_view mode = arguments[1];
  const std::string_view engine = arguments[2];
  const std::string_view profile = arguments[3];
  const std::string_view benchmark_case = arguments[4];
  const std::filesystem::path path = arguments[5];
  const std::size_t operations = ParseOperations(arguments[6]);
  if (mode != "smoke") {
    throw HarnessFailure{"unsupported write benchmark mode"};
  }
  if (profile != "matched-durable") {
    throw HarnessFailure{"unsupported write benchmark profile"};
  }
  if (benchmark_case != "insert-point-implicit") {
    throw HarnessFailure{"unsupported write benchmark case"};
  }
  if (!std::filesystem::is_regular_file(path)) {
    throw HarnessFailure{"write benchmark database is not a file"};
  }

  std::uint64_t changed_rows = 0;
  if (engine == "modern") {
    changed_rows = InsertModern(path, operations);
  } else if (engine == "sqlite") {
    changed_rows = InsertSqlite(path, operations);
  } else {
    throw HarnessFailure{"unsupported write benchmark engine"};
  }

  const Verification sqlite_verification = VerifyWithSqlite(path, operations);
  const Verification modern_verification = VerifyWithModern(path, operations);
  if (sqlite_verification.rows != modern_verification.rows ||
      sqlite_verification.digest != modern_verification.digest || changed_rows != operations) {
    throw BenchmarkMismatch{"write benchmark final verification differs"};
  }
  EnsureNoSidecars(path);
  PrintReport(engine, operations, changed_rows, sqlite_verification);
  return 0;
}

}  // namespace

int main(int argument_count, char* const* arguments) {
  try {
    return Run(argument_count, arguments);
  } catch (const BenchmarkMismatch& error) {
    std::cerr << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
