#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
constexpr std::uint64_t kSplitMixIncrement = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kKeyOrderSeed = 0xD1B54A32D192ED03ULL;
constexpr std::size_t kValueSize = 256;
constexpr std::size_t kTimingRepetitions = 3;
constexpr std::uint64_t kMinimumWallNanoseconds = 20'000'000ULL;
constexpr std::int64_t kPopulatedRows = 65'536;
constexpr std::string_view kInsertSql = "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)";
constexpr std::string_view kUpdatePointSql = "UPDATE kv SET v=?1,version=version+1 WHERE k=?2";
constexpr std::string_view kDeletePointSql = "DELETE FROM kv WHERE k=?1";
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

enum class EngineKind : std::uint8_t {
  kModern,
  kSqlite,
};

enum class ProfileKind : std::uint8_t {
  kEngineDefault,
  kMatchedDurable,
};

enum class CaseKind : std::uint8_t {
  kCreate,
  kInsertPoint,
  kInsertBatch,
  kUpdatePoint,
  kUpdateScan,
  kDeletePoint,
  kDeleteScan,
  kMixedCommit,
  kMixedRollback,
};

enum class RunKind : std::uint8_t {
  kSmoke,
  kBaseline,
};

struct WorkResult {
  std::uint64_t changed_rows = 0;
  std::int64_t last_insert_rowid = 0;
};

struct WorkloadScale {
  std::size_t operations = 0;
  std::uint64_t transactions = 0;
  std::uint64_t dml_operations = 0;
  std::uint64_t row_mutations = 0;
};

struct EffectiveConfiguration {
  std::int64_t page_size = 0;
  std::int64_t cache_size = 0;
  std::int64_t mmap_bytes = 0;
  std::string temp_store;
  std::string journal_mode;
  std::string synchronous;
  std::string locking_mode;
  std::string thread_mode;

  bool operator==(const EffectiveConfiguration&) const = default;
};

struct ExpectedRecord {
  std::int64_t rowid;
  std::int64_t value_seed;
  std::int64_t version;
};

struct Verification {
  std::size_t rows = 0;
  std::size_t schema_objects = 0;
  std::string digest;

  bool operator==(const Verification&) const = default;
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

  SqliteStatement(SqliteStatement&& other) noexcept
      : statement_(std::exchange(other.statement_, nullptr)) {}

  SqliteStatement& operator=(SqliteStatement&& other) noexcept {
    if (this != &other) {
      if (statement_ != nullptr) {
        static_cast<void>(sqlite3_finalize(statement_));
      }
      statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
  }

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

[[nodiscard]] modern_sqlite::ByteBuffer ValueFor(std::int64_t seed) {
  modern_sqlite::ByteBuffer value{modern_sqlite::ByteCount{kValueSize}};
  std::ranges::fill(value.mutable_view(), std::byte{'0'});
  std::array<char, 16> digits{};
  const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), seed, 16);
  if (error != std::errc{}) {
    throw HarnessFailure{"cannot render benchmark value"};
  }
  const auto count = static_cast<std::size_t>(end - digits.data());
  if (count > 8U) {
    throw HarnessFailure{"benchmark value seed exceeds its prefix"};
  }
  const std::size_t offset = 8U - count;
  for (std::size_t index = 0; index < count; ++index) {
    value.mutable_view()[offset + index] =
        static_cast<std::byte>(static_cast<std::uint8_t>(digits[index]));
  }
  return value;
}

[[nodiscard]] std::string TableName(std::size_t index) {
  std::array<char, 4> digits{};
  const auto [end, error] = std::to_chars(digits.data(), digits.data() + 3, index);
  if (error != std::errc{}) {
    throw HarnessFailure{"cannot render CREATE table index"};
  }
  const auto count = static_cast<std::size_t>(end - digits.data());
  std::string name{"t"};
  name.append(3U - count, '0');
  name.append(digits.data(), count);
  return name;
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

void VerifySqliteIdentity() {
  if (std::string_view{sqlite3_libversion()} != kSqliteVersion ||
      std::string_view{sqlite3_sourceid()} != kSqliteSourceId) {
    throw HarnessFailure{"write benchmark SQLite identity is not pinned"};
  }
}

[[nodiscard]] std::string_view SqliteSynchronousName(std::int64_t value) {
  switch (value) {
    case 0:
      return "off";
    case 1:
      return "normal";
    case 2:
      return "full";
    case 3:
      return "extra";
    default:
      throw HarnessFailure{"SQLite synchronous mode is invalid"};
  }
}

[[nodiscard]] std::string_view SqliteTempStoreName(std::int64_t value) {
  switch (value) {
    case 0:
      return "default";
    case 1:
      return "file";
    case 2:
      return "memory";
    default:
      throw HarnessFailure{"SQLite temp-store mode is invalid"};
  }
}

[[nodiscard]] EffectiveConfiguration ReadSqliteConfiguration(sqlite3* database) {
  return EffectiveConfiguration{
      .page_size = SqliteSingleInteger(database, "PRAGMA page_size"),
      .cache_size = SqliteSingleInteger(database, "PRAGMA cache_size"),
      .mmap_bytes = SqliteSingleInteger(database, "PRAGMA mmap_size"),
      .temp_store =
          std::string{SqliteTempStoreName(SqliteSingleInteger(database, "PRAGMA temp_store"))},
      .journal_mode = SqliteSingleText(database, "PRAGMA journal_mode"),
      .synchronous =
          std::string{SqliteSynchronousName(SqliteSingleInteger(database, "PRAGMA synchronous"))},
      .locking_mode = SqliteSingleText(database, "PRAGMA locking_mode"),
      .thread_mode = "single",
  };
}

[[nodiscard]] EffectiveConfiguration ConfigureSqlite(sqlite3* database, ProfileKind profile) {
  VerifySqliteIdentity();
  if (profile == ProfileKind::kMatchedDurable) {
    ExecuteSqlite(database, "PRAGMA cache_size=512");
    ExecuteSqlite(database, "PRAGMA mmap_size=0");
    ExecuteSqlite(database, "PRAGMA temp_store=MEMORY");
    ExecuteSqlite(database, "PRAGMA journal_mode=DELETE");
    ExecuteSqlite(database, "PRAGMA synchronous=FULL");
    ExecuteSqlite(database, "PRAGMA locking_mode=NORMAL");
  }
  EffectiveConfiguration configuration = ReadSqliteConfiguration(database);
  if (profile == ProfileKind::kMatchedDurable && configuration != EffectiveConfiguration{
                                                                      .page_size = 4096,
                                                                      .cache_size = 512,
                                                                      .mmap_bytes = 0,
                                                                      .temp_store = "memory",
                                                                      .journal_mode = "delete",
                                                                      .synchronous = "full",
                                                                      .locking_mode = "normal",
                                                                      .thread_mode = "single",
                                                                  }) {
    throw HarnessFailure{"write benchmark SQLite matched configuration differs"};
  }
  return configuration;
}

[[nodiscard]] EffectiveConfiguration ModernConfiguration() {
  return EffectiveConfiguration{
      .page_size = 4096,
      .cache_size = 512,
      .mmap_bytes = 0,
      .temp_store = "memory",
      .journal_mode = "delete",
      .synchronous = "full",
      .locking_mode = "normal",
      .thread_mode = "single",
  };
}

[[nodiscard]] modern_sqlite::WriteStatement PrepareModern(modern_sqlite::WriteSession& session,
                                                          std::string_view sql) {
  modern_sqlite::WritePrepareOutput prepared =
      TakeValue(session.Prepare(modern_sqlite::Utf8View{sql}));
  if (!prepared.statement.has_value()) {
    throw HarnessFailure{"Modern preparation produced no statement"};
  }
  return std::move(*prepared.statement);
}

void StepModernCommand(modern_sqlite::WriteStatement& statement) {
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone) {
    throw BenchmarkMismatch{"Modern transaction command produced a row"};
  }
}

void StepSqliteCommand(const SqliteStatement& statement) {
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw BenchmarkMismatch{"SQLite transaction command completion differs"};
  }
}

void VerifyModernLastInsertRowid(const modern_sqlite::WriteSession& session,
                                 std::int64_t expected) {
  if (session.last_insert_rowid() != expected) {
    throw BenchmarkMismatch{"Modern last-insert-rowid differs"};
  }
}

void VerifySqliteLastInsertRowid(sqlite3* database, sqlite3_int64 expected) {
  if (sqlite3_last_insert_rowid(database) != expected) {
    throw BenchmarkMismatch{"SQLite last-insert-rowid differs"};
  }
}

[[nodiscard]] std::uint64_t StepModern(modern_sqlite::WriteStatement& statement,
                                       const modern_sqlite::WriteSession& session,
                                       std::uint64_t expected_changes) {
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone ||
      session.changes() != expected_changes) {
    throw BenchmarkMismatch{"Modern DML completion differs"};
  }
  RequireStatus(statement.Reset());
  return expected_changes;
}

[[nodiscard]] std::uint64_t StepSqlite(const SqliteStatement& statement, sqlite3* database,
                                       std::uint64_t expected_changes) {
  if (sqlite3_step(statement.get()) != SQLITE_DONE ||
      sqlite3_changes64(database) != static_cast<sqlite3_int64>(expected_changes) ||
      sqlite3_reset(statement.get()) != SQLITE_OK ||
      sqlite3_clear_bindings(statement.get()) != SQLITE_OK) {
    throw BenchmarkMismatch{"SQLite DML completion differs"};
  }
  return expected_changes;
}

struct BlobBinding {
  std::size_t index;
  std::int64_t seed;
};

void BindModernBlob(modern_sqlite::WriteStatement& statement, BlobBinding binding) {
  const modern_sqlite::SqlValue value = modern_sqlite::SqlValue::Blob(ValueFor(binding.seed));
  RequireStatus(statement.Bind(binding.index, value));
}

void BindSqliteBlob(const SqliteStatement& statement, BlobBinding binding) {
  if (!std::in_range<int>(binding.index)) {
    throw HarnessFailure{"SQLite BLOB binding index is out of range"};
  }
  const modern_sqlite::ByteBuffer value = ValueFor(binding.seed);
  if (sqlite3_bind_blob(statement.get(), static_cast<int>(binding.index), value.view().data(),
                        static_cast<int>(value.view().size()), SqliteTransient()) != SQLITE_OK) {
    throw HarnessFailure{"SQLite BLOB binding failed"};
  }
}

[[nodiscard]] std::uint64_t RunCreateModern(modern_sqlite::WriteSession& session,
                                            std::size_t operations,
                                            std::vector<modern_sqlite::WriteStatement>& completed) {
  for (std::size_t index = 0; index < operations; ++index) {
    const std::string sql =
        "CREATE TABLE " + TableName(index) + "(id INTEGER PRIMARY KEY,v BLOB NOT NULL DEFAULT x'')";
    modern_sqlite::WriteStatement statement = PrepareModern(session, sql);
    StepModernCommand(statement);
    if (session.changes() != 0U) {
      throw BenchmarkMismatch{"Modern CREATE change count differs"};
    }
    VerifyModernLastInsertRowid(session, 0);
    completed.push_back(std::move(statement));
  }
  return 0;
}

[[nodiscard]] std::uint64_t RunCreateSqlite(sqlite3* database, std::size_t operations,
                                            std::vector<SqliteStatement>& completed) {
  for (std::size_t index = 0; index < operations; ++index) {
    const std::string sql =
        "CREATE TABLE " + TableName(index) + "(id INTEGER PRIMARY KEY,v BLOB NOT NULL DEFAULT x'')";
    SqliteStatement statement{database, sql};
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
      throw BenchmarkMismatch{"SQLite CREATE completion differs"};
    }
    if (sqlite3_changes64(database) != 0) {
      throw BenchmarkMismatch{"SQLite CREATE change count differs"};
    }
    VerifySqliteLastInsertRowid(database, 0);
    completed.push_back(std::move(statement));
  }
  return 0;
}

[[nodiscard]] std::uint64_t RunInsertModern(const modern_sqlite::WriteSession& session,
                                            modern_sqlite::WriteStatement& statement,
                                            modern_sqlite::WriteStatement* begin,
                                            modern_sqlite::WriteStatement* commit,
                                            std::size_t operations) {
  const bool explicit_transaction = begin != nullptr;
  if (explicit_transaction) {
    if (commit == nullptr) {
      throw HarnessFailure{"Modern explicit INSERT has no COMMIT statement"};
    }
    StepModernCommand(*begin);
  }
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<std::int64_t>(index) + 1;
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    BindModernBlob(statement, {.index = 2, .seed = rowid});
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, rowid);
  }
  if (explicit_transaction) {
    StepModernCommand(*commit);
  }
  VerifyModernLastInsertRowid(session, static_cast<std::int64_t>(operations));
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunInsertSqlite(sqlite3* database, const SqliteStatement& statement,
                                            const SqliteStatement* begin,
                                            const SqliteStatement* commit, std::size_t operations) {
  const bool explicit_transaction = begin != nullptr;
  if (explicit_transaction) {
    if (commit == nullptr) {
      throw HarnessFailure{"SQLite explicit INSERT has no COMMIT statement"};
    }
    StepSqliteCommand(*begin);
  }
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<sqlite3_int64>(index) + 1;
    if (sqlite3_bind_int64(statement.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    BindSqliteBlob(statement, {.index = 2, .seed = rowid});
    changed_rows += StepSqlite(statement, database, 1);
    VerifySqliteLastInsertRowid(database, rowid);
  }
  if (explicit_transaction) {
    StepSqliteCommand(*commit);
  }
  VerifySqliteLastInsertRowid(database, static_cast<sqlite3_int64>(operations));
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdatePointModern(const modern_sqlite::WriteSession& session,
                                                 modern_sqlite::WriteStatement& statement,
                                                 std::span<const std::int64_t> rowids) {
  std::uint64_t changed_rows = 0;
  for (const std::int64_t rowid : rowids) {
    BindModernBlob(statement, {.index = 1, .seed = rowid + 1'000'000});
    RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdatePointSqlite(sqlite3* database,
                                                 const SqliteStatement& statement,
                                                 std::span<const std::int64_t> rowids) {
  std::uint64_t changed_rows = 0;
  for (const std::int64_t rowid : rowids) {
    BindSqliteBlob(statement, {.index = 1, .seed = rowid + 1'000'000});
    if (sqlite3_bind_int64(statement.get(), 2, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    changed_rows += StepSqlite(statement, database, 1);
    VerifySqliteLastInsertRowid(database, 0);
  }
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdateScanModern(const modern_sqlite::WriteSession& session,
                                                modern_sqlite::WriteStatement& statement,
                                                std::optional<std::int64_t> maximum_rowid,
                                                std::uint64_t expected_changes) {
  BindModernBlob(statement, {.index = 1, .seed = 9'000'000});
  if (maximum_rowid.has_value()) {
    RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Integer(*maximum_rowid)));
  }
  const std::uint64_t changed_rows = StepModern(statement, session, expected_changes);
  VerifyModernLastInsertRowid(session, 0);
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdateScanSqlite(sqlite3* database, const SqliteStatement& statement,
                                                std::optional<std::int64_t> maximum_rowid,
                                                std::uint64_t expected_changes) {
  BindSqliteBlob(statement, {.index = 1, .seed = 9'000'000});
  if (maximum_rowid.has_value() &&
      sqlite3_bind_int64(statement.get(), 2, *maximum_rowid) != SQLITE_OK) {
    throw HarnessFailure{"SQLite scan bound failed"};
  }
  const std::uint64_t changed_rows = StepSqlite(statement, database, expected_changes);
  VerifySqliteLastInsertRowid(database, 0);
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeletePointModern(const modern_sqlite::WriteSession& session,
                                                 modern_sqlite::WriteStatement& statement,
                                                 std::span<const std::int64_t> rowids) {
  std::uint64_t changed_rows = 0;
  for (const std::int64_t rowid : rowids) {
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeletePointSqlite(sqlite3* database,
                                                 const SqliteStatement& statement,
                                                 std::span<const std::int64_t> rowids) {
  std::uint64_t changed_rows = 0;
  for (const std::int64_t rowid : rowids) {
    if (sqlite3_bind_int64(statement.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    changed_rows += StepSqlite(statement, database, 1);
    VerifySqliteLastInsertRowid(database, 0);
  }
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeleteScanModern(const modern_sqlite::WriteSession& session,
                                                modern_sqlite::WriteStatement& statement,
                                                std::optional<std::int64_t> maximum_rowid,
                                                std::uint64_t expected_changes) {
  if (maximum_rowid.has_value()) {
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(*maximum_rowid)));
  }
  const std::uint64_t changed_rows = StepModern(statement, session, expected_changes);
  VerifyModernLastInsertRowid(session, 0);
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeleteScanSqlite(sqlite3* database, const SqliteStatement& statement,
                                                std::optional<std::int64_t> maximum_rowid,
                                                std::uint64_t expected_changes) {
  if (maximum_rowid.has_value() &&
      sqlite3_bind_int64(statement.get(), 1, *maximum_rowid) != SQLITE_OK) {
    throw HarnessFailure{"SQLite scan bound failed"};
  }
  const std::uint64_t changed_rows = StepSqlite(statement, database, expected_changes);
  VerifySqliteLastInsertRowid(database, 0);
  return changed_rows;
}

struct MixedCounts {
  std::size_t updates;
  std::size_t deletes;
  std::size_t inserts;
};

[[nodiscard]] MixedCounts SplitMixed(std::size_t operations) {
  const std::size_t updates = operations / 3U;
  const std::size_t deletes = operations / 3U;
  return MixedCounts{
      .updates = updates,
      .deletes = deletes,
      .inserts = operations - updates - deletes,
  };
}

struct ModernMixedStatements {
  modern_sqlite::WriteStatement& begin;
  modern_sqlite::WriteStatement& update;
  modern_sqlite::WriteStatement& remove;
  modern_sqlite::WriteStatement& insert;
  modern_sqlite::WriteStatement& terminal;
};

[[nodiscard]] std::uint64_t RunMixedModern(const modern_sqlite::WriteSession& session,
                                           ModernMixedStatements statements,
                                           std::span<const std::int64_t> permutation,
                                           std::size_t operations) {
  const MixedCounts counts = SplitMixed(operations);
  StepModernCommand(statements.begin);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < counts.updates; ++index) {
    const std::int64_t rowid = permutation[index];
    BindModernBlob(statements.update, {.index = 1, .seed = rowid + 2'000'000});
    RequireStatus(statements.update.Bind(2, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statements.update, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  for (std::size_t index = 0; index < counts.deletes; ++index) {
    const std::int64_t rowid = permutation[counts.updates + index];
    RequireStatus(statements.remove.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statements.remove, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  for (std::size_t index = 0; index < counts.inserts; ++index) {
    const auto rowid = kPopulatedRows + static_cast<std::int64_t>(index) + 1;
    RequireStatus(statements.insert.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    BindModernBlob(statements.insert, {.index = 2, .seed = rowid});
    changed_rows += StepModern(statements.insert, session, 1);
    VerifyModernLastInsertRowid(session, rowid);
  }
  StepModernCommand(statements.terminal);
  const std::int64_t expected_last_insert_rowid =
      kPopulatedRows + static_cast<std::int64_t>(counts.inserts);
  VerifyModernLastInsertRowid(session, expected_last_insert_rowid);
  return changed_rows;
}

struct SqliteMixedStatements {
  const SqliteStatement& begin;
  const SqliteStatement& update;
  const SqliteStatement& remove;
  const SqliteStatement& insert;
  const SqliteStatement& terminal;
};

[[nodiscard]] std::uint64_t RunMixedSqlite(sqlite3* database, SqliteMixedStatements statements,
                                           std::span<const std::int64_t> permutation,
                                           std::size_t operations) {
  const MixedCounts counts = SplitMixed(operations);
  StepSqliteCommand(statements.begin);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < counts.updates; ++index) {
    const std::int64_t rowid = permutation[index];
    BindSqliteBlob(statements.update, {.index = 1, .seed = rowid + 2'000'000});
    if (sqlite3_bind_int64(statements.update.get(), 2, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed update binding failed"};
    }
    changed_rows += StepSqlite(statements.update, database, 1);
    VerifySqliteLastInsertRowid(database, 0);
  }
  for (std::size_t index = 0; index < counts.deletes; ++index) {
    const std::int64_t rowid = permutation[counts.updates + index];
    if (sqlite3_bind_int64(statements.remove.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed delete binding failed"};
    }
    changed_rows += StepSqlite(statements.remove, database, 1);
    VerifySqliteLastInsertRowid(database, 0);
  }
  for (std::size_t index = 0; index < counts.inserts; ++index) {
    const auto rowid = kPopulatedRows + static_cast<sqlite3_int64>(index) + 1;
    if (sqlite3_bind_int64(statements.insert.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed insert binding failed"};
    }
    BindSqliteBlob(statements.insert, {.index = 2, .seed = rowid});
    changed_rows += StepSqlite(statements.insert, database, 1);
    VerifySqliteLastInsertRowid(database, rowid);
  }
  StepSqliteCommand(statements.terminal);
  const sqlite3_int64 expected_last_insert_rowid =
      kPopulatedRows + static_cast<sqlite3_int64>(counts.inserts);
  VerifySqliteLastInsertRowid(database, expected_last_insert_rowid);
  return changed_rows;
}

[[nodiscard]] EngineKind ParseEngine(std::string_view value) {
  if (value == "modern") {
    return EngineKind::kModern;
  }
  if (value == "sqlite") {
    return EngineKind::kSqlite;
  }
  throw HarnessFailure{"unsupported write benchmark engine"};
}

[[nodiscard]] ProfileKind ParseProfile(std::string_view value) {
  if (value == "engine-default") {
    return ProfileKind::kEngineDefault;
  }
  if (value == "matched-durable") {
    return ProfileKind::kMatchedDurable;
  }
  throw HarnessFailure{"unsupported write benchmark profile"};
}

[[nodiscard]] CaseKind ParseCase(std::string_view value) {
  if (value == "create-table-implicit") {
    return CaseKind::kCreate;
  }
  if (value == "insert-point-implicit") {
    return CaseKind::kInsertPoint;
  }
  if (value == "insert-batch-explicit") {
    return CaseKind::kInsertBatch;
  }
  if (value == "update-point-implicit") {
    return CaseKind::kUpdatePoint;
  }
  if (value == "update-scan-implicit") {
    return CaseKind::kUpdateScan;
  }
  if (value == "delete-point-implicit") {
    return CaseKind::kDeletePoint;
  }
  if (value == "delete-scan-implicit") {
    return CaseKind::kDeleteScan;
  }
  if (value == "mixed-batch-commit") {
    return CaseKind::kMixedCommit;
  }
  if (value == "mixed-batch-rollback") {
    return CaseKind::kMixedRollback;
  }
  throw HarnessFailure{"unsupported write benchmark case"};
}

[[nodiscard]] std::string_view ProfileName(ProfileKind profile) noexcept {
  return profile == ProfileKind::kEngineDefault ? "engine-default" : "matched-durable";
}

[[nodiscard]] RunKind ParseRunKind(std::string_view value) {
  if (value == "smoke") {
    return RunKind::kSmoke;
  }
  if (value == "baseline") {
    return RunKind::kBaseline;
  }
  throw HarnessFailure{"run kind must be smoke or baseline"};
}

[[nodiscard]] std::string_view RunKindName(RunKind kind) noexcept {
  return kind == RunKind::kSmoke ? "smoke" : "baseline";
}

[[nodiscard]] WorkloadScale ScaleFor(CaseKind kind, RunKind run_kind) {
  const bool smoke = run_kind == RunKind::kSmoke;
  switch (kind) {
    case CaseKind::kCreate:
      return WorkloadScale{
          .operations = smoke ? 1U : 256U,
          .transactions = smoke ? 1U : 256U,
          .dml_operations = smoke ? 1U : 256U,
          .row_mutations = 0,
      };
    case CaseKind::kInsertPoint:
      return WorkloadScale{
          .operations = smoke ? 1U : 512U,
          .transactions = smoke ? 1U : 512U,
          .dml_operations = smoke ? 1U : 512U,
          .row_mutations = smoke ? 1U : 512U,
      };
    case CaseKind::kInsertBatch:
      return WorkloadScale{
          .operations = smoke ? 8U : 65'536U,
          .transactions = 1,
          .dml_operations = smoke ? 8U : 65'536U,
          .row_mutations = smoke ? 8U : 65'536U,
      };
    case CaseKind::kUpdatePoint:
      return WorkloadScale{
          .operations = smoke ? 1U : 512U,
          .transactions = smoke ? 1U : 512U,
          .dml_operations = smoke ? 1U : 512U,
          .row_mutations = smoke ? 1U : 512U,
      };
    case CaseKind::kUpdateScan:
      return WorkloadScale{
          .operations = smoke ? 8U : 65'536U,
          .transactions = 1,
          .dml_operations = 1,
          .row_mutations = smoke ? 8U : 65'536U,
      };
    case CaseKind::kDeletePoint:
      return WorkloadScale{
          .operations = smoke ? 1U : 512U,
          .transactions = smoke ? 1U : 512U,
          .dml_operations = smoke ? 1U : 512U,
          .row_mutations = smoke ? 1U : 512U,
      };
    case CaseKind::kDeleteScan:
      return WorkloadScale{
          .operations = smoke ? 8U : 65'536U,
          .transactions = 1,
          .dml_operations = 1,
          .row_mutations = smoke ? 8U : 65'536U,
      };
    case CaseKind::kMixedCommit:
    case CaseKind::kMixedRollback:
      return WorkloadScale{
          .operations = smoke ? 8U : 12'288U,
          .transactions = 1,
          .dml_operations = smoke ? 8U : 12'288U,
          .row_mutations = smoke ? 8U : 12'288U,
      };
  }
  throw HarnessFailure{"invalid write benchmark case"};
}

[[nodiscard]] std::vector<std::int64_t> GenerateKeyOrder() {
  std::vector<std::int64_t> result(static_cast<std::size_t>(kPopulatedRows));
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::int64_t>(index) + 1;
  }
  std::uint64_t state = kKeyOrderSeed;
  const auto next = [&state] {
    state += kSplitMixIncrement;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  };
  for (std::size_t index = result.size() - 1U; index > 0; --index) {
    const std::uint64_t bound = static_cast<std::uint64_t>(index) + 1U;
    const std::uint64_t threshold = (0U - bound) % bound;
    std::uint64_t random = 0;
    do {
      random = next();
    } while (random < threshold);
    const auto selected = static_cast<std::size_t>(random % bound);
    std::swap(result[index], result[selected]);
  }
  return result;
}

[[nodiscard]] std::uint64_t ProcessCpuNanoseconds() {
  timespec value{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0 || value.tv_sec < 0 ||
      value.tv_nsec < 0) {
    throw HarnessFailure{"cannot read CLOCK_PROCESS_CPUTIME_ID"};
  }
  const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
  const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
  if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) / 1'000'000'000ULL) {
    throw HarnessFailure{"process CPU clock overflow"};
  }
  return seconds * 1'000'000'000ULL + nanoseconds;
}

struct TimedWork {
  std::size_t index = 0;
  std::uint64_t wall_ns = 0;
  std::uint64_t cpu_ns = 0;
  WorkResult work;
};

template <typename Callable>
[[nodiscard]] TimedWork RunMeasured(std::size_t index, bool measured, Callable&& callable) {
  if (!measured) {
    return TimedWork{.index = index, .work = std::forward<Callable>(callable)()};
  }
  const std::uint64_t cpu_started = ProcessCpuNanoseconds();
  const auto wall_started = std::chrono::steady_clock::now();
  const WorkResult work = std::forward<Callable>(callable)();
  const auto wall_finished = std::chrono::steady_clock::now();
  const std::uint64_t cpu_finished = ProcessCpuNanoseconds();
  if (cpu_finished < cpu_started) {
    throw HarnessFailure{"process CPU clock moved backward"};
  }
  const auto wall_duration =
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall_finished - wall_started).count();
  if (wall_duration <= 0) {
    throw HarnessFailure{"steady clock produced a nonpositive duration"};
  }
  return TimedWork{
      .index = index,
      .wall_ns = static_cast<std::uint64_t>(wall_duration),
      .cpu_ns = cpu_finished - cpu_started,
      .work = work,
  };
}

struct Execution {
  TimedWork timed;
  EffectiveConfiguration configuration;
};

[[nodiscard]] std::optional<std::int64_t> ScanMaximum(const WorkloadScale& scale) {
  if (scale.operations == static_cast<std::size_t>(kPopulatedRows)) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(scale.operations);
}

[[nodiscard]] std::string_view UpdateScanSql(const WorkloadScale& scale) {
  return ScanMaximum(scale).has_value() ? "UPDATE kv SET v=?1,version=version+1 WHERE k<=?2"
                                        : "UPDATE kv SET v=?1,version=version+1 WHERE k>=1";
}

[[nodiscard]] std::string_view DeleteScanSql(const WorkloadScale& scale) {
  return ScanMaximum(scale).has_value() ? "DELETE FROM kv WHERE k<=?1"
                                        : "DELETE FROM kv WHERE k>=1";
}

void FinalizeModern(modern_sqlite::WriteStatement& statement) {
  RequireStatus(statement.Finalize());
}

[[nodiscard]] Execution ExecuteModernWork(CaseKind kind, const std::filesystem::path& path,
                                          const WorkloadScale& scale,
                                          std::span<const std::int64_t> key_order, bool measured,
                                          std::size_t index) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  const EffectiveConfiguration configuration = ModernConfiguration();
  TimedWork timed;
  switch (kind) {
    case CaseKind::kCreate: {
      std::vector<modern_sqlite::WriteStatement> completed;
      completed.reserve(scale.operations);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunCreateModern(session, scale.operations, completed),
            .last_insert_rowid = 0,
        };
      });
      for (modern_sqlite::WriteStatement& statement : completed) {
        FinalizeModern(statement);
      }
      break;
    }
    case CaseKind::kInsertPoint:
    case CaseKind::kInsertBatch: {
      const bool explicit_transaction = kind == CaseKind::kInsertBatch;
      std::optional<modern_sqlite::WriteStatement> begin;
      std::optional<modern_sqlite::WriteStatement> commit;
      if (explicit_transaction) {
        begin.emplace(PrepareModern(session, "BEGIN"));
        commit.emplace(PrepareModern(session, "COMMIT"));
      }
      modern_sqlite::WriteStatement statement = PrepareModern(session, kInsertSql);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunInsertModern(session, statement, begin.has_value() ? &*begin : nullptr,
                                commit.has_value() ? &*commit : nullptr, scale.operations),
            .last_insert_rowid = static_cast<std::int64_t>(scale.operations),
        };
      });
      FinalizeModern(statement);
      if (begin.has_value() && commit.has_value()) {
        FinalizeModern(*begin);
        FinalizeModern(*commit);
      }
      break;
    }
    case CaseKind::kUpdatePoint: {
      modern_sqlite::WriteStatement statement = PrepareModern(session, kUpdatePointSql);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunUpdatePointModern(session, statement, key_order.first(scale.operations)),
            .last_insert_rowid = 0,
        };
      });
      FinalizeModern(statement);
      break;
    }
    case CaseKind::kUpdateScan: {
      modern_sqlite::WriteStatement statement = PrepareModern(session, UpdateScanSql(scale));
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunUpdateScanModern(session, statement, ScanMaximum(scale), scale.row_mutations),
            .last_insert_rowid = 0,
        };
      });
      FinalizeModern(statement);
      break;
    }
    case CaseKind::kDeletePoint: {
      modern_sqlite::WriteStatement statement = PrepareModern(session, kDeletePointSql);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunDeletePointModern(session, statement, key_order.first(scale.operations)),
            .last_insert_rowid = 0,
        };
      });
      FinalizeModern(statement);
      break;
    }
    case CaseKind::kDeleteScan: {
      modern_sqlite::WriteStatement statement = PrepareModern(session, DeleteScanSql(scale));
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunDeleteScanModern(session, statement, ScanMaximum(scale), scale.row_mutations),
            .last_insert_rowid = 0,
        };
      });
      FinalizeModern(statement);
      break;
    }
    case CaseKind::kMixedCommit:
    case CaseKind::kMixedRollback: {
      const bool rollback = kind == CaseKind::kMixedRollback;
      modern_sqlite::WriteStatement begin = PrepareModern(session, "BEGIN");
      modern_sqlite::WriteStatement update = PrepareModern(session, kUpdatePointSql);
      modern_sqlite::WriteStatement remove = PrepareModern(session, kDeletePointSql);
      modern_sqlite::WriteStatement insert = PrepareModern(session, kInsertSql);
      modern_sqlite::WriteStatement terminal =
          PrepareModern(session, rollback ? "ROLLBACK" : "COMMIT");
      const MixedCounts counts = SplitMixed(scale.operations);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunMixedModern(session,
                                           ModernMixedStatements{
                                               .begin = begin,
                                               .update = update,
                                               .remove = remove,
                                               .insert = insert,
                                               .terminal = terminal,
                                           },
                                           key_order, scale.operations),
            .last_insert_rowid = kPopulatedRows + static_cast<std::int64_t>(counts.inserts),
        };
      });
      FinalizeModern(update);
      FinalizeModern(remove);
      FinalizeModern(insert);
      FinalizeModern(begin);
      FinalizeModern(terminal);
      break;
    }
  }
  return Execution{.timed = timed, .configuration = configuration};
}

[[nodiscard]] Execution ExecuteSqliteWork(CaseKind kind, ProfileKind profile,
                                          const std::filesystem::path& path,
                                          const WorkloadScale& scale,
                                          std::span<const std::int64_t> key_order, bool measured,
                                          std::size_t index) {
  SqliteDatabase database{path};
  const EffectiveConfiguration configuration = ConfigureSqlite(database.get(), profile);
  TimedWork timed;
  switch (kind) {
    case CaseKind::kCreate: {
      std::vector<SqliteStatement> completed;
      completed.reserve(scale.operations);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunCreateSqlite(database.get(), scale.operations, completed),
            .last_insert_rowid = 0,
        };
      });
      for (SqliteStatement& statement : completed) {
        statement.Finalize();
      }
      break;
    }
    case CaseKind::kInsertPoint:
    case CaseKind::kInsertBatch: {
      const bool explicit_transaction = kind == CaseKind::kInsertBatch;
      std::optional<SqliteStatement> begin;
      std::optional<SqliteStatement> commit;
      if (explicit_transaction) {
        begin.emplace(database.get(), "BEGIN");
        commit.emplace(database.get(), "COMMIT");
      }
      SqliteStatement statement{database.get(), kInsertSql};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunInsertSqlite(database.get(), statement, begin.has_value() ? &*begin : nullptr,
                                commit.has_value() ? &*commit : nullptr, scale.operations),
            .last_insert_rowid = static_cast<std::int64_t>(scale.operations),
        };
      });
      statement.Finalize();
      if (begin.has_value() && commit.has_value()) {
        begin->Finalize();
        commit->Finalize();
      }
      break;
    }
    case CaseKind::kUpdatePoint: {
      SqliteStatement statement{database.get(), kUpdatePointSql};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunUpdatePointSqlite(database.get(), statement, key_order.first(scale.operations)),
            .last_insert_rowid = 0,
        };
      });
      statement.Finalize();
      break;
    }
    case CaseKind::kUpdateScan: {
      SqliteStatement statement{database.get(), UpdateScanSql(scale)};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunUpdateScanSqlite(database.get(), statement, ScanMaximum(scale),
                                                scale.row_mutations),
            .last_insert_rowid = 0,
        };
      });
      statement.Finalize();
      break;
    }
    case CaseKind::kDeletePoint: {
      SqliteStatement statement{database.get(), kDeletePointSql};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunDeletePointSqlite(database.get(), statement, key_order.first(scale.operations)),
            .last_insert_rowid = 0,
        };
      });
      statement.Finalize();
      break;
    }
    case CaseKind::kDeleteScan: {
      SqliteStatement statement{database.get(), DeleteScanSql(scale)};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunDeleteScanSqlite(database.get(), statement, ScanMaximum(scale),
                                                scale.row_mutations),
            .last_insert_rowid = 0,
        };
      });
      statement.Finalize();
      break;
    }
    case CaseKind::kMixedCommit:
    case CaseKind::kMixedRollback: {
      const bool rollback = kind == CaseKind::kMixedRollback;
      SqliteStatement begin{database.get(), "BEGIN"};
      SqliteStatement update{database.get(), kUpdatePointSql};
      SqliteStatement remove{database.get(), kDeletePointSql};
      SqliteStatement insert{database.get(), kInsertSql};
      SqliteStatement terminal{database.get(), rollback ? "ROLLBACK" : "COMMIT"};
      const MixedCounts counts = SplitMixed(scale.operations);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunMixedSqlite(database.get(),
                                           SqliteMixedStatements{
                                               .begin = begin,
                                               .update = update,
                                               .remove = remove,
                                               .insert = insert,
                                               .terminal = terminal,
                                           },
                                           key_order, scale.operations),
            .last_insert_rowid = kPopulatedRows + static_cast<std::int64_t>(counts.inserts),
        };
      });
      update.Finalize();
      remove.Finalize();
      insert.Finalize();
      begin.Finalize();
      terminal.Finalize();
      break;
    }
  }
  database.Close();
  return Execution{.timed = timed, .configuration = configuration};
}

[[nodiscard]] std::vector<ExpectedRecord> ExpectedRecords(CaseKind kind, std::size_t operations,
                                                          std::span<const std::int64_t> key_order) {
  std::vector<ExpectedRecord> records;
  if (kind == CaseKind::kCreate) {
    return records;
  }
  if (kind == CaseKind::kInsertPoint || kind == CaseKind::kInsertBatch) {
    records.reserve(operations);
    for (std::size_t index = 0; index < operations; ++index) {
      const auto rowid = static_cast<std::int64_t>(index) + 1;
      records.push_back(ExpectedRecord{.rowid = rowid, .value_seed = rowid, .version = 0});
    }
    return records;
  }

  const bool rollback = kind == CaseKind::kMixedRollback;
  const MixedCounts mixed = SplitMixed(operations);
  std::vector<std::uint8_t> mutations(static_cast<std::size_t>(kPopulatedRows) + 1U, 0);
  if (!rollback && (kind == CaseKind::kUpdatePoint || kind == CaseKind::kDeletePoint)) {
    const std::uint8_t mutation = kind == CaseKind::kUpdatePoint ? 1U : 2U;
    for (const std::int64_t rowid : key_order.first(operations)) {
      mutations[static_cast<std::size_t>(rowid)] = mutation;
    }
  } else if (!rollback && kind == CaseKind::kMixedCommit) {
    for (std::size_t index = 0; index < mixed.updates; ++index) {
      mutations[static_cast<std::size_t>(key_order[index])] = 3U;
    }
    for (std::size_t index = 0; index < mixed.deletes; ++index) {
      mutations[static_cast<std::size_t>(key_order[mixed.updates + index])] = 2U;
    }
  }
  records.reserve(static_cast<std::size_t>(kPopulatedRows) + mixed.inserts);
  for (std::int64_t rowid = 1; rowid <= kPopulatedRows; ++rowid) {
    const std::uint8_t mutation = mutations[static_cast<std::size_t>(rowid)];
    if ((!rollback && kind == CaseKind::kDeleteScan && std::cmp_less_equal(rowid, operations)) ||
        mutation == 2U) {
      continue;
    }
    std::int64_t seed = rowid;
    std::int64_t version = 0;
    if (mutation == 1U) {
      seed = rowid + 1'000'000;
      version = 1;
    } else if (!rollback && kind == CaseKind::kUpdateScan &&
               std::cmp_less_equal(rowid, operations)) {
      seed = 9'000'000;
      version = 1;
    } else if (mutation == 3U) {
      seed = rowid + 2'000'000;
      version = 1;
    }
    records.push_back(ExpectedRecord{.rowid = rowid, .value_seed = seed, .version = version});
  }
  if (!rollback && kind == CaseKind::kMixedCommit) {
    for (std::size_t index = 0; index < mixed.inserts; ++index) {
      const auto rowid = kPopulatedRows + static_cast<std::int64_t>(index) + 1;
      records.push_back(ExpectedRecord{.rowid = rowid, .value_seed = rowid, .version = 0});
    }
  }
  return records;
}

[[nodiscard]] Verification VerifyCreateWithSqlite(const std::filesystem::path& path,
                                                  std::size_t operations) {
  SqliteDatabase database{path};
  if (SqliteSingleText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw BenchmarkMismatch{"SQLite CREATE integrity check failed"};
  }
  SqliteStatement statement{database.get(),
                            "SELECT name FROM sqlite_schema WHERE type='table' ORDER BY rowid"};
  Digest digest;
  std::size_t count = 0;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(statement.get(), 0);
    const int bytes = sqlite3_column_bytes(statement.get(), 0);
    const std::string expected = TableName(count);
    if (text == nullptr || std::cmp_not_equal(bytes, expected.size()) ||
        std::string_view{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)} !=
            expected) {
      throw BenchmarkMismatch{"SQLite CREATE schema differs"};
    }
    digest.AddBytes(modern_sqlite::AsBytes(expected));
    ++count;
  }
  statement.Finalize();
  database.Close();
  if (count != operations) {
    throw BenchmarkMismatch{"SQLite CREATE object count differs"};
  }
  return Verification{.rows = 0, .schema_objects = count, .digest = digest.Hex()};
}

[[nodiscard]] Verification VerifyCreateWithModern(const std::filesystem::path& path,
                                                  std::size_t operations) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement =
      PrepareModern(session, "SELECT name FROM sqlite_schema");
  Digest digest;
  std::size_t count = 0;
  while (true) {
    const modern_sqlite::WriteStep step = TakeValue(statement.Step());
    if (step == modern_sqlite::WriteStep::kDone) {
      break;
    }
    const std::optional<modern_sqlite::Utf8View> text = statement.row()[0].text_value();
    const std::string expected = TableName(count);
    if (!text.has_value() || text.value().bytes() != expected) {
      throw BenchmarkMismatch{"Modern CREATE schema differs"};
    }
    digest.AddBytes(modern_sqlite::AsBytes(expected));
    ++count;
  }
  RequireStatus(statement.Finalize());
  if (count != operations) {
    throw BenchmarkMismatch{"Modern CREATE object count differs"};
  }
  return Verification{.rows = 0, .schema_objects = count, .digest = digest.Hex()};
}

[[nodiscard]] Verification VerifyKvWithSqlite(const std::filesystem::path& path,
                                              const std::vector<ExpectedRecord>& expected) {
  SqliteDatabase database{path};
  if (SqliteSingleText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw BenchmarkMismatch{"SQLite integrity_check failed"};
  }
  SqliteStatement statement{database.get(), "SELECT k,v,version FROM kv"};
  Digest digest;
  std::size_t index = 0;
  while (true) {
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW || index >= expected.size()) {
      throw BenchmarkMismatch{"SQLite final scan differs"};
    }
    const ExpectedRecord& record = expected[index];
    const std::int64_t rowid = sqlite3_column_int64(statement.get(), 0);
    const void* pointer = sqlite3_column_blob(statement.get(), 1);
    const int bytes = sqlite3_column_bytes(statement.get(), 1);
    const std::int64_t version = sqlite3_column_int64(statement.get(), 2);
    if (pointer == nullptr || std::cmp_not_equal(bytes, kValueSize) || rowid != record.rowid ||
        version != record.version) {
      throw BenchmarkMismatch{"SQLite final row differs"};
    }
    const modern_sqlite::ByteBuffer expected_value = ValueFor(record.value_seed);
    const modern_sqlite::ByteView actual{static_cast<const std::byte*>(pointer), kValueSize};
    if (!std::ranges::equal(actual, expected_value.view())) {
      throw BenchmarkMismatch{"SQLite final BLOB differs"};
    }
    digest.AddInteger(rowid);
    digest.AddBytes(actual);
    digest.AddInteger(version);
    ++index;
  }
  statement.Finalize();
  database.Close();
  if (index != expected.size()) {
    throw BenchmarkMismatch{"SQLite final row count differs"};
  }
  return Verification{.rows = index, .schema_objects = 1, .digest = digest.Hex()};
}

[[nodiscard]] Verification VerifyKvWithModern(const std::filesystem::path& path,
                                              const std::vector<ExpectedRecord>& expected) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement = PrepareModern(session, "SELECT k,v,version FROM kv");
  Digest digest;
  std::size_t index = 0;
  while (true) {
    const modern_sqlite::WriteStep step = TakeValue(statement.Step());
    if (step == modern_sqlite::WriteStep::kDone) {
      break;
    }
    if (index >= expected.size() || statement.row().size() != 3U) {
      throw BenchmarkMismatch{"Modern final scan differs"};
    }
    const ExpectedRecord& record = expected[index];
    const std::int64_t rowid =
        statement.row()[0].integer_value().value_or(std::numeric_limits<std::int64_t>::min());
    const std::optional<modern_sqlite::ByteView> blob = statement.row()[1].blob_value();
    const std::int64_t version =
        statement.row()[2].integer_value().value_or(std::numeric_limits<std::int64_t>::min());
    if (!blob.has_value() || blob.value().size() != kValueSize || rowid != record.rowid ||
        version != record.version) {
      throw BenchmarkMismatch{"Modern final row differs"};
    }
    const modern_sqlite::ByteBuffer expected_value = ValueFor(record.value_seed);
    if (!std::ranges::equal(blob.value(), expected_value.view())) {
      throw BenchmarkMismatch{"Modern final BLOB differs"};
    }
    digest.AddInteger(rowid);
    digest.AddBytes(blob.value());
    digest.AddInteger(version);
    ++index;
  }
  RequireStatus(statement.Finalize());
  if (index != expected.size()) {
    throw BenchmarkMismatch{"Modern final row count differs"};
  }
  return Verification{.rows = index, .schema_objects = 1, .digest = digest.Hex()};
}

[[nodiscard]] std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    throw HarnessFailure{"cannot open benchmark database"};
  }
  const std::streamoff end = input.tellg();
  if (end < 0) {
    throw HarnessFailure{"cannot size benchmark database"};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  if (!input) {
    throw HarnessFailure{"cannot read benchmark database"};
  }
  return bytes;
}

void EnsureNoSidecars(const std::filesystem::path& path) {
  for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
    if (std::filesystem::exists(path.string() + std::string{suffix})) {
      throw BenchmarkMismatch{"write benchmark left a sidecar"};
    }
  }
}

[[nodiscard]] std::vector<ExpectedRecord> OriginalRecords() {
  std::vector<ExpectedRecord> records;
  records.reserve(static_cast<std::size_t>(kPopulatedRows));
  for (std::int64_t rowid = 1; rowid <= kPopulatedRows; ++rowid) {
    records.push_back(ExpectedRecord{.rowid = rowid, .value_seed = rowid, .version = 0});
  }
  return records;
}

void VerifyInitialInput(EngineKind engine, CaseKind kind, const std::filesystem::path& path) {
  EnsureNoSidecars(path);
  if (kind == CaseKind::kCreate) {
    if (!ReadFile(path).empty()) {
      throw BenchmarkMismatch{"CREATE input is not a zero-byte database"};
    }
    return;
  }
  const std::vector<ExpectedRecord> expected =
      kind == CaseKind::kInsertPoint || kind == CaseKind::kInsertBatch
          ? std::vector<ExpectedRecord>{}
          : OriginalRecords();
  const Verification verification = engine == EngineKind::kModern
                                        ? VerifyKvWithModern(path, expected)
                                        : VerifyKvWithSqlite(path, expected);
  if (verification.rows != expected.size() || verification.schema_objects != 1U) {
    throw BenchmarkMismatch{"write benchmark input verification differs"};
  }
}

struct FreshDatabaseRequest {
  const std::filesystem::path& input;
  const std::filesystem::path& scratch;
  std::string_view name;
};

[[nodiscard]] std::filesystem::path FreshDatabasePath(FreshDatabaseRequest request) {
  const std::filesystem::path output = request.scratch / std::string{request.name};
  if (std::filesystem::exists(output)) {
    throw HarnessFailure{"scratch database already exists"};
  }
  for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
    if (std::filesystem::exists(output.string() + std::string{suffix})) {
      throw HarnessFailure{"scratch database sidecar already exists"};
    }
  }
  std::error_code error;
  if (!std::filesystem::copy_file(request.input, output, std::filesystem::copy_options::none,
                                  error)) {
    throw HarnessFailure{"cannot copy write benchmark input: " + error.message()};
  }
  return output;
}

void RemoveFreshDatabase(const std::filesystem::path& path) {
  EnsureNoSidecars(path);
  std::error_code error;
  if (!std::filesystem::remove(path, error) || error) {
    throw HarnessFailure{"cannot remove write benchmark scratch database"};
  }
}

struct VerifiedWork {
  WorkResult work;
  Verification verification;
};

struct VerifiedRepetition {
  TimedWork timed;
  Verification verification;
};

struct TimingRun {
  WorkloadScale scale;
  EffectiveConfiguration configuration;
  VerifiedWork warmup;
  std::vector<VerifiedRepetition> repetitions;
};

[[nodiscard]] Execution ExecuteWork(EngineKind engine, ProfileKind profile, CaseKind kind,
                                    const std::filesystem::path& path, const WorkloadScale& scale,
                                    std::span<const std::int64_t> key_order, bool measured,
                                    std::size_t index) {
  return engine == EngineKind::kModern
             ? ExecuteModernWork(kind, path, scale, key_order, measured, index)
             : ExecuteSqliteWork(kind, profile, path, scale, key_order, measured, index);
}

[[nodiscard]] std::int64_t ExpectedLastInsertRowid(CaseKind kind, const WorkloadScale& scale) {
  if (kind == CaseKind::kInsertPoint || kind == CaseKind::kInsertBatch) {
    return static_cast<std::int64_t>(scale.operations);
  }
  if (kind == CaseKind::kMixedCommit || kind == CaseKind::kMixedRollback) {
    return kPopulatedRows + static_cast<std::int64_t>(SplitMixed(scale.operations).inserts);
  }
  return 0;
}

[[nodiscard]] Verification VerifyFinalOutput(CaseKind kind, const WorkloadScale& scale,
                                             const std::filesystem::path& path,
                                             std::span<const std::int64_t> key_order) {
  const std::vector<ExpectedRecord> expected = ExpectedRecords(kind, scale.operations, key_order);
  const Verification sqlite_verification = kind == CaseKind::kCreate
                                               ? VerifyCreateWithSqlite(path, scale.operations)
                                               : VerifyKvWithSqlite(path, expected);
  const Verification modern_verification = kind == CaseKind::kCreate
                                               ? VerifyCreateWithModern(path, scale.operations)
                                               : VerifyKvWithModern(path, expected);
  if (sqlite_verification != modern_verification) {
    throw BenchmarkMismatch{"write benchmark final verification differs"};
  }
  return sqlite_verification;
}

void ValidateExecutedWork(CaseKind kind, const WorkloadScale& scale, const WorkResult& work) {
  const std::uint64_t expected_changes = kind == CaseKind::kCreate ? 0U : scale.row_mutations;
  if (work.changed_rows != expected_changes ||
      work.last_insert_rowid != ExpectedLastInsertRowid(kind, scale)) {
    throw BenchmarkMismatch{"write benchmark completion counts differ"};
  }
}

[[nodiscard]] TimingRun RunTiming(EngineKind engine, ProfileKind profile, CaseKind kind,
                                  RunKind run_kind, const std::filesystem::path& input,
                                  const std::filesystem::path& scratch) {
  VerifyInitialInput(engine, kind, input);
  const WorkloadScale scale = ScaleFor(kind, run_kind);
  const std::vector<std::int64_t> key_order = GenerateKeyOrder();
  const std::vector<std::byte> rollback_input =
      kind == CaseKind::kMixedRollback ? ReadFile(input) : std::vector<std::byte>{};
  std::vector<std::filesystem::path> paths;
  paths.reserve(1U + kTimingRepetitions);

  const std::filesystem::path warmup_path = FreshDatabasePath(
      FreshDatabaseRequest{.input = input, .scratch = scratch, .name = "warmup.db"});
  paths.push_back(warmup_path);
  Execution warmup = ExecuteWork(engine, profile, kind, warmup_path, scale, key_order, false, 0);

  const std::size_t repetition_count = run_kind == RunKind::kSmoke ? 1U : kTimingRepetitions;
  std::vector<Execution> executions;
  executions.reserve(repetition_count);
  for (std::size_t index = 0; index < repetition_count; ++index) {
    const std::string name = "repetition-" + std::to_string(index) + ".db";
    const std::filesystem::path path =
        FreshDatabasePath(FreshDatabaseRequest{.input = input, .scratch = scratch, .name = name});
    paths.push_back(path);
    Execution execution = ExecuteWork(engine, profile, kind, path, scale, key_order, true, index);
    if (run_kind == RunKind::kBaseline && execution.timed.wall_ns < kMinimumWallNanoseconds) {
      throw HarnessFailure{"baseline repetition did not reach the minimum wall time: " +
                           std::to_string(execution.timed.wall_ns) + " ns"};
    }
    if (execution.configuration != warmup.configuration) {
      throw HarnessFailure{"effective write configuration changed between repetitions"};
    }
    executions.push_back(std::move(execution));
  }

  ValidateExecutedWork(kind, scale, warmup.timed.work);
  const Verification warmup_verification = VerifyFinalOutput(kind, scale, warmup_path, key_order);
  if (kind == CaseKind::kMixedRollback && rollback_input != ReadFile(warmup_path)) {
    throw BenchmarkMismatch{"write benchmark rollback changed warmup database bytes"};
  }

  std::vector<VerifiedRepetition> repetitions;
  repetitions.reserve(repetition_count);
  for (std::size_t index = 0; index < executions.size(); ++index) {
    const Execution& execution = executions[index];
    ValidateExecutedWork(kind, scale, execution.timed.work);
    const Verification verification = VerifyFinalOutput(kind, scale, paths[index + 1U], key_order);
    if (verification != warmup_verification) {
      throw BenchmarkMismatch{"write benchmark repetition result differs from warmup"};
    }
    if (kind == CaseKind::kMixedRollback && rollback_input != ReadFile(paths[index + 1U])) {
      throw BenchmarkMismatch{"write benchmark rollback changed repetition database bytes"};
    }
    repetitions.push_back(
        VerifiedRepetition{.timed = execution.timed, .verification = verification});
  }

  for (const std::filesystem::path& path : paths) {
    RemoveFreshDatabase(path);
  }
  return TimingRun{
      .scale = scale,
      .configuration = std::move(warmup.configuration),
      .warmup = VerifiedWork{.work = warmup.timed.work, .verification = warmup_verification},
      .repetitions = std::move(repetitions),
  };
}

void PrintJsonString(std::ostream& output, std::string_view value) {
  output << '"';
  for (const char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        output << character;
        break;
    }
  }
  output << '"';
}

void PrintConfiguration(std::ostream& output, const EffectiveConfiguration& configuration) {
  output << R"({"cache_size":)" << configuration.cache_size << R"(,"journal_mode":)";
  PrintJsonString(output, configuration.journal_mode);
  output << R"(,"locking_mode":)";
  PrintJsonString(output, configuration.locking_mode);
  output << R"(,"mmap_bytes":)" << configuration.mmap_bytes << R"(,"page_size":)"
         << configuration.page_size << R"(,"synchronous":)";
  PrintJsonString(output, configuration.synchronous);
  output << R"(,"temp_store":)";
  PrintJsonString(output, configuration.temp_store);
  output << R"(,"thread_mode":)";
  PrintJsonString(output, configuration.thread_mode);
  output << '}';
}

void PrintWork(std::ostream& output, const WorkloadScale& scale, const WorkResult& work,
               const Verification& verification) {
  output << R"("changed_rows":)" << work.changed_rows << R"(,"digest":)";
  PrintJsonString(output, verification.digest);
  output << R"(,"dml_operations":)" << scale.dml_operations << R"(,"final_rows":)"
         << verification.rows << R"(,"last_insert_rowid":)" << work.last_insert_rowid
         << R"(,"row_mutations":)" << scale.row_mutations << R"(,"schema_objects":)"
         << verification.schema_objects << R"(,"transactions":)" << scale.transactions;
}

void PrintReport(std::string_view engine, ProfileKind profile, std::string_view case_id,
                 RunKind run_kind, const TimingRun& run) {
  std::cout << R"({"case":)";
  PrintJsonString(std::cout, case_id);
  std::cout << R"(,"completion":{"fresh_databases":)" << run.repetitions.size() + 1U
            << R"(,"measured_repetitions":)" << run.repetitions.size()
            << R"(,"post_verifications":)" << run.repetitions.size() + 1U
            << R"(,"pre_verifications":1,"status":"complete","warmups":1},)"
               R"("effective_configuration":)";
  PrintConfiguration(std::cout, run.configuration);
  std::cout << R"(,"engine":)";
  PrintJsonString(std::cout, engine);
  std::cout << R"(,"mode":"timing","profile":)";
  PrintJsonString(std::cout, ProfileName(profile));
  std::cout << R"(,"repetitions":[)";
  for (std::size_t index = 0; index < run.repetitions.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    const VerifiedRepetition& repetition = run.repetitions[index];
    std::cout << '{';
    PrintWork(std::cout, run.scale, repetition.timed.work, repetition.verification);
    std::cout << R"(,"cpu_ns":)" << repetition.timed.cpu_ns << R"(,"index":)"
              << repetition.timed.index << R"(,"wall_ns":)" << repetition.timed.wall_ns << '}';
  }
  std::cout << R"(],"run_kind":)";
  PrintJsonString(std::cout, RunKindName(run_kind));
  std::cout << R"(,"schema_version":1,"timer":{"cpu":"CLOCK_PROCESS_CPUTIME_ID",)"
               R"("wall":"steady_clock"},"warmup":{)";
  PrintWork(std::cout, run.scale, run.warmup.work, run.warmup.verification);
  std::cout << R"(},"workload_semantics_version":1})" << '\n';
}

int Run(int argument_count, char* const* arguments) {
  if (argument_count != 8 || std::string_view{arguments[1]} != "run") {
    throw HarnessFailure{
        "usage: write benchmark run ENGINE PROFILE CASE INPUT SCRATCH <smoke|baseline>"};
  }
  const EngineKind engine = ParseEngine(arguments[2]);
  const ProfileKind profile = ParseProfile(arguments[3]);
  const CaseKind benchmark_case = ParseCase(arguments[4]);
  const std::filesystem::path input = arguments[5];
  const std::filesystem::path scratch = arguments[6];
  const RunKind run_kind = ParseRunKind(arguments[7]);
  if (!std::filesystem::is_regular_file(input)) {
    throw HarnessFailure{"write benchmark input is not a file"};
  }
  if (!std::filesystem::is_directory(scratch)) {
    throw HarnessFailure{"write benchmark scratch path is not a directory"};
  }
  const TimingRun run = RunTiming(engine, profile, benchmark_case, run_kind, input, scratch);
  PrintReport(arguments[2], profile, arguments[4], run_kind, run);
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
