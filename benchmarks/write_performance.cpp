#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
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
constexpr std::size_t kValueSize = 256;
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

struct WorkResult {
  std::uint64_t changed_rows = 0;
  std::int64_t last_insert_rowid = 0;
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

void ConfigureSqlite(sqlite3* database, ProfileKind profile) {
  VerifySqliteIdentity();
  if (profile == ProfileKind::kEngineDefault) {
    return;
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

[[nodiscard]] modern_sqlite::WriteStatement PrepareModern(modern_sqlite::WriteSession& session,
                                                          std::string_view sql) {
  modern_sqlite::WritePrepareOutput prepared =
      TakeValue(session.Prepare(modern_sqlite::Utf8View{sql}));
  if (!prepared.statement.has_value()) {
    throw HarnessFailure{"Modern preparation produced no statement"};
  }
  return std::move(*prepared.statement);
}

void ExecuteModern(modern_sqlite::WriteSession& session, std::string_view sql) {
  modern_sqlite::WriteStatement statement = PrepareModern(session, sql);
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone) {
    throw BenchmarkMismatch{"Modern transaction command produced a row"};
  }
  RequireStatus(statement.Finalize());
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

[[nodiscard]] std::uint64_t RunCreateModern(const std::filesystem::path& path,
                                            std::size_t operations) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  for (std::size_t index = 0; index < operations; ++index) {
    const std::string sql =
        "CREATE TABLE " + TableName(index) + "(id INTEGER PRIMARY KEY,v BLOB NOT NULL DEFAULT x'')";
    ExecuteModern(session, sql);
    if (session.changes() != 0U) {
      throw BenchmarkMismatch{"Modern CREATE change count differs"};
    }
    VerifyModernLastInsertRowid(session, 0);
  }
  return 0;
}

[[nodiscard]] std::uint64_t RunCreateSqlite(const std::filesystem::path& path, ProfileKind profile,
                                            std::size_t operations) {
  SqliteDatabase database{path};
  ConfigureSqlite(database.get(), profile);
  for (std::size_t index = 0; index < operations; ++index) {
    const std::string sql =
        "CREATE TABLE " + TableName(index) + "(id INTEGER PRIMARY KEY,v BLOB NOT NULL DEFAULT x'')";
    SqliteStatement statement{database.get(), sql};
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
      throw BenchmarkMismatch{"SQLite CREATE completion differs"};
    }
    if (sqlite3_changes64(database.get()) != 0) {
      throw BenchmarkMismatch{"SQLite CREATE change count differs"};
    }
    VerifySqliteLastInsertRowid(database.get(), 0);
    statement.Finalize();
  }
  database.Close();
  return 0;
}

[[nodiscard]] std::uint64_t RunInsertModern(const std::filesystem::path& path,
                                            std::size_t operations, bool explicit_transaction) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  if (explicit_transaction) {
    ExecuteModern(session, "BEGIN");
  }
  modern_sqlite::WriteStatement statement = PrepareModern(session, kInsertSql);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<std::int64_t>(index) + 1;
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    BindModernBlob(statement, {.index = 2, .seed = rowid});
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, rowid);
  }
  RequireStatus(statement.Finalize());
  if (explicit_transaction) {
    ExecuteModern(session, "COMMIT");
  }
  VerifyModernLastInsertRowid(session, static_cast<std::int64_t>(operations));
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunInsertSqlite(const std::filesystem::path& path, ProfileKind profile,
                                            std::size_t operations, bool explicit_transaction) {
  SqliteDatabase database{path};
  ConfigureSqlite(database.get(), profile);
  if (explicit_transaction) {
    ExecuteSqlite(database.get(), "BEGIN");
  }
  SqliteStatement statement{database.get(), kInsertSql};
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<sqlite3_int64>(index) + 1;
    if (sqlite3_bind_int64(statement.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    BindSqliteBlob(statement, {.index = 2, .seed = rowid});
    changed_rows += StepSqlite(statement, database.get(), 1);
    VerifySqliteLastInsertRowid(database.get(), rowid);
  }
  statement.Finalize();
  if (explicit_transaction) {
    ExecuteSqlite(database.get(), "COMMIT");
  }
  VerifySqliteLastInsertRowid(database.get(), static_cast<sqlite3_int64>(operations));
  database.Close();
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdatePointModern(const std::filesystem::path& path,
                                                 std::size_t operations) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement = PrepareModern(session, kUpdatePointSql);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<std::int64_t>(index) + 1;
    BindModernBlob(statement, {.index = 1, .seed = rowid + 1'000'000});
    RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  RequireStatus(statement.Finalize());
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdatePointSqlite(const std::filesystem::path& path,
                                                 ProfileKind profile, std::size_t operations) {
  SqliteDatabase database{path};
  ConfigureSqlite(database.get(), profile);
  SqliteStatement statement{database.get(), kUpdatePointSql};
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<sqlite3_int64>(index) + 1;
    BindSqliteBlob(statement, {.index = 1, .seed = rowid + 1'000'000});
    if (sqlite3_bind_int64(statement.get(), 2, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    changed_rows += StepSqlite(statement, database.get(), 1);
    VerifySqliteLastInsertRowid(database.get(), 0);
  }
  statement.Finalize();
  database.Close();
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdateScanModern(const std::filesystem::path& path,
                                                std::size_t operations) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement =
      PrepareModern(session, "UPDATE kv SET v=?1,version=version+1 WHERE k<=?2");
  BindModernBlob(statement, {.index = 1, .seed = 9'000'000});
  RequireStatus(
      statement.Bind(2, modern_sqlite::SqlValue::Integer(static_cast<std::int64_t>(operations))));
  const std::uint64_t changed_rows = StepModern(statement, session, operations);
  VerifyModernLastInsertRowid(session, 0);
  RequireStatus(statement.Finalize());
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdateScanSqlite(const std::filesystem::path& path,
                                                ProfileKind profile, std::size_t operations) {
  SqliteDatabase database{path};
  ConfigureSqlite(database.get(), profile);
  SqliteStatement statement{database.get(), "UPDATE kv SET v=?1,version=version+1 WHERE k<=?2"};
  BindSqliteBlob(statement, {.index = 1, .seed = 9'000'000});
  if (sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(operations)) != SQLITE_OK) {
    throw HarnessFailure{"SQLite scan bound failed"};
  }
  const std::uint64_t changed_rows = StepSqlite(statement, database.get(), operations);
  VerifySqliteLastInsertRowid(database.get(), 0);
  statement.Finalize();
  database.Close();
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeletePointModern(const std::filesystem::path& path,
                                                 std::size_t operations) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement = PrepareModern(session, kDeletePointSql);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<std::int64_t>(index) + 1;
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  RequireStatus(statement.Finalize());
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeletePointSqlite(const std::filesystem::path& path,
                                                 ProfileKind profile, std::size_t operations) {
  SqliteDatabase database{path};
  ConfigureSqlite(database.get(), profile);
  SqliteStatement statement{database.get(), kDeletePointSql};
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<sqlite3_int64>(index) + 1;
    if (sqlite3_bind_int64(statement.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    changed_rows += StepSqlite(statement, database.get(), 1);
    VerifySqliteLastInsertRowid(database.get(), 0);
  }
  statement.Finalize();
  database.Close();
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeleteScanModern(const std::filesystem::path& path,
                                                std::size_t operations) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement = PrepareModern(session, "DELETE FROM kv WHERE k<=?1");
  RequireStatus(
      statement.Bind(1, modern_sqlite::SqlValue::Integer(static_cast<std::int64_t>(operations))));
  const std::uint64_t changed_rows = StepModern(statement, session, operations);
  VerifyModernLastInsertRowid(session, 0);
  RequireStatus(statement.Finalize());
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeleteScanSqlite(const std::filesystem::path& path,
                                                ProfileKind profile, std::size_t operations) {
  SqliteDatabase database{path};
  ConfigureSqlite(database.get(), profile);
  SqliteStatement statement{database.get(), "DELETE FROM kv WHERE k<=?1"};
  if (sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(operations)) != SQLITE_OK) {
    throw HarnessFailure{"SQLite scan bound failed"};
  }
  const std::uint64_t changed_rows = StepSqlite(statement, database.get(), operations);
  VerifySqliteLastInsertRowid(database.get(), 0);
  statement.Finalize();
  database.Close();
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

[[nodiscard]] std::uint64_t RunMixedModern(const std::filesystem::path& path,
                                           std::size_t operations, bool rollback) {
  const MixedCounts counts = SplitMixed(operations);
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  ExecuteModern(session, "BEGIN");
  modern_sqlite::WriteStatement update = PrepareModern(session, kUpdatePointSql);
  modern_sqlite::WriteStatement remove = PrepareModern(session, kDeletePointSql);
  modern_sqlite::WriteStatement insert = PrepareModern(session, kInsertSql);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < counts.updates; ++index) {
    const auto rowid = static_cast<std::int64_t>(index) + 1;
    BindModernBlob(update, {.index = 1, .seed = rowid + 2'000'000});
    RequireStatus(update.Bind(2, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(update, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  for (std::size_t index = 0; index < counts.deletes; ++index) {
    const auto rowid = static_cast<std::int64_t>(counts.updates + index) + 1;
    RequireStatus(remove.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(remove, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  for (std::size_t index = 0; index < counts.inserts; ++index) {
    const auto rowid = kPopulatedRows + static_cast<std::int64_t>(index) + 1;
    RequireStatus(insert.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    BindModernBlob(insert, {.index = 2, .seed = rowid});
    changed_rows += StepModern(insert, session, 1);
    VerifyModernLastInsertRowid(session, rowid);
  }
  RequireStatus(update.Finalize());
  RequireStatus(remove.Finalize());
  RequireStatus(insert.Finalize());
  ExecuteModern(session, rollback ? "ROLLBACK" : "COMMIT");
  const std::int64_t expected_last_insert_rowid =
      kPopulatedRows + static_cast<std::int64_t>(counts.inserts);
  VerifyModernLastInsertRowid(session, expected_last_insert_rowid);
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunMixedSqlite(const std::filesystem::path& path, ProfileKind profile,
                                           std::size_t operations, bool rollback) {
  const MixedCounts counts = SplitMixed(operations);
  SqliteDatabase database{path};
  ConfigureSqlite(database.get(), profile);
  ExecuteSqlite(database.get(), "BEGIN");
  SqliteStatement update{database.get(), kUpdatePointSql};
  SqliteStatement remove{database.get(), kDeletePointSql};
  SqliteStatement insert{database.get(), kInsertSql};
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < counts.updates; ++index) {
    const auto rowid = static_cast<sqlite3_int64>(index) + 1;
    BindSqliteBlob(update, {.index = 1, .seed = rowid + 2'000'000});
    if (sqlite3_bind_int64(update.get(), 2, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed update binding failed"};
    }
    changed_rows += StepSqlite(update, database.get(), 1);
    VerifySqliteLastInsertRowid(database.get(), 0);
  }
  for (std::size_t index = 0; index < counts.deletes; ++index) {
    const auto rowid = static_cast<sqlite3_int64>(counts.updates + index) + 1;
    if (sqlite3_bind_int64(remove.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed delete binding failed"};
    }
    changed_rows += StepSqlite(remove, database.get(), 1);
    VerifySqliteLastInsertRowid(database.get(), 0);
  }
  for (std::size_t index = 0; index < counts.inserts; ++index) {
    const auto rowid = kPopulatedRows + static_cast<sqlite3_int64>(index) + 1;
    if (sqlite3_bind_int64(insert.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed insert binding failed"};
    }
    BindSqliteBlob(insert, {.index = 2, .seed = rowid});
    changed_rows += StepSqlite(insert, database.get(), 1);
    VerifySqliteLastInsertRowid(database.get(), rowid);
  }
  update.Finalize();
  remove.Finalize();
  insert.Finalize();
  ExecuteSqlite(database.get(), rollback ? "ROLLBACK" : "COMMIT");
  const sqlite3_int64 expected_last_insert_rowid =
      kPopulatedRows + static_cast<sqlite3_int64>(counts.inserts);
  VerifySqliteLastInsertRowid(database.get(), expected_last_insert_rowid);
  database.Close();
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

[[nodiscard]] WorkResult RunModern(CaseKind kind, const std::filesystem::path& path,
                                   std::size_t operations) {
  switch (kind) {
    case CaseKind::kCreate:
      return WorkResult{.changed_rows = RunCreateModern(path, operations), .last_insert_rowid = 0};
    case CaseKind::kInsertPoint:
      return WorkResult{.changed_rows = RunInsertModern(path, operations, false),
                        .last_insert_rowid = static_cast<std::int64_t>(operations)};
    case CaseKind::kInsertBatch:
      return WorkResult{.changed_rows = RunInsertModern(path, operations, true),
                        .last_insert_rowid = static_cast<std::int64_t>(operations)};
    case CaseKind::kUpdatePoint:
      return WorkResult{.changed_rows = RunUpdatePointModern(path, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kUpdateScan:
      return WorkResult{.changed_rows = RunUpdateScanModern(path, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kDeletePoint:
      return WorkResult{.changed_rows = RunDeletePointModern(path, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kDeleteScan:
      return WorkResult{.changed_rows = RunDeleteScanModern(path, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kMixedCommit:
    case CaseKind::kMixedRollback: {
      const MixedCounts counts = SplitMixed(operations);
      return WorkResult{
          .changed_rows = RunMixedModern(path, operations, kind == CaseKind::kMixedRollback),
          .last_insert_rowid = kPopulatedRows + static_cast<std::int64_t>(counts.inserts),
      };
    }
  }
  throw HarnessFailure{"invalid Modern benchmark case"};
}

[[nodiscard]] WorkResult RunSqlite(CaseKind kind, ProfileKind profile,
                                   const std::filesystem::path& path, std::size_t operations) {
  switch (kind) {
    case CaseKind::kCreate:
      return WorkResult{.changed_rows = RunCreateSqlite(path, profile, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kInsertPoint:
      return WorkResult{.changed_rows = RunInsertSqlite(path, profile, operations, false),
                        .last_insert_rowid = static_cast<std::int64_t>(operations)};
    case CaseKind::kInsertBatch:
      return WorkResult{.changed_rows = RunInsertSqlite(path, profile, operations, true),
                        .last_insert_rowid = static_cast<std::int64_t>(operations)};
    case CaseKind::kUpdatePoint:
      return WorkResult{.changed_rows = RunUpdatePointSqlite(path, profile, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kUpdateScan:
      return WorkResult{.changed_rows = RunUpdateScanSqlite(path, profile, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kDeletePoint:
      return WorkResult{.changed_rows = RunDeletePointSqlite(path, profile, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kDeleteScan:
      return WorkResult{.changed_rows = RunDeleteScanSqlite(path, profile, operations),
                        .last_insert_rowid = 0};
    case CaseKind::kMixedCommit:
    case CaseKind::kMixedRollback: {
      const MixedCounts counts = SplitMixed(operations);
      return WorkResult{
          .changed_rows =
              RunMixedSqlite(path, profile, operations, kind == CaseKind::kMixedRollback),
          .last_insert_rowid = kPopulatedRows + static_cast<std::int64_t>(counts.inserts),
      };
    }
  }
  throw HarnessFailure{"invalid SQLite benchmark case"};
}

[[nodiscard]] std::vector<ExpectedRecord> ExpectedRecords(CaseKind kind, std::size_t operations) {
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
  records.reserve(static_cast<std::size_t>(kPopulatedRows) + mixed.inserts);
  for (std::int64_t rowid = 1; rowid <= kPopulatedRows; ++rowid) {
    if (!rollback && (kind == CaseKind::kDeletePoint || kind == CaseKind::kDeleteScan) &&
        std::cmp_less_equal(rowid, operations)) {
      continue;
    }
    if (!rollback && kind == CaseKind::kMixedCommit && std::cmp_greater(rowid, mixed.updates) &&
        std::cmp_less_equal(rowid, mixed.updates + mixed.deletes)) {
      continue;
    }
    std::int64_t seed = rowid;
    std::int64_t version = 0;
    if (!rollback && kind == CaseKind::kUpdatePoint && std::cmp_less_equal(rowid, operations)) {
      seed = rowid + 1'000'000;
      version = 1;
    } else if (!rollback && kind == CaseKind::kUpdateScan &&
               std::cmp_less_equal(rowid, operations)) {
      seed = 9'000'000;
      version = 1;
    } else if (!rollback && kind == CaseKind::kMixedCommit &&
               std::cmp_less_equal(rowid, mixed.updates)) {
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

[[nodiscard]] std::size_t ParseOperations(std::string_view text) {
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0U || value > 65'536U) {
    throw HarnessFailure{"operation count must be between 1 and 65536"};
  }
  return value;
}

void PrintReport(std::string_view engine, std::string_view profile, std::string_view case_id,
                 std::size_t operations, const WorkResult& work, const Verification& verification) {
  std::cout << R"({"case":")" << case_id << R"(","changed_rows":)" << work.changed_rows
            << R"(,"digest":")" << verification.digest << R"(","engine":")" << engine
            << R"(","final_rows":)" << verification.rows << R"(,"last_insert_rowid":)"
            << work.last_insert_rowid << R"(,"mode":"smoke","operations":)" << operations
            << R"(,"profile":")" << profile << R"(","schema_objects":)"
            << verification.schema_objects << R"(,"schema_version":1,"status":"complete"})" << '\n';
}

int Run(int argument_count, char* const* arguments) {
  if (argument_count != 7) {
    throw HarnessFailure{"usage: write benchmark smoke ENGINE PROFILE CASE DATABASE OPERATIONS"};
  }
  if (std::string_view{arguments[1]} != "smoke") {
    throw HarnessFailure{"unsupported write benchmark mode"};
  }
  const EngineKind engine = ParseEngine(arguments[2]);
  const ProfileKind profile = ParseProfile(arguments[3]);
  const CaseKind benchmark_case = ParseCase(arguments[4]);
  const std::filesystem::path path = arguments[5];
  const std::size_t operations = ParseOperations(arguments[6]);
  if (!std::filesystem::is_regular_file(path)) {
    throw HarnessFailure{"write benchmark database is not a file"};
  }
  const std::vector<std::byte> initial_bytes =
      benchmark_case == CaseKind::kMixedRollback ? ReadFile(path) : std::vector<std::byte>{};

  const WorkResult work = engine == EngineKind::kModern
                              ? RunModern(benchmark_case, path, operations)
                              : RunSqlite(benchmark_case, profile, path, operations);
  const std::vector<ExpectedRecord> expected = ExpectedRecords(benchmark_case, operations);
  const Verification sqlite_verification = benchmark_case == CaseKind::kCreate
                                               ? VerifyCreateWithSqlite(path, operations)
                                               : VerifyKvWithSqlite(path, expected);
  const Verification modern_verification = benchmark_case == CaseKind::kCreate
                                               ? VerifyCreateWithModern(path, operations)
                                               : VerifyKvWithModern(path, expected);
  const std::uint64_t expected_changes = benchmark_case == CaseKind::kCreate ? 0U : operations;
  if (sqlite_verification != modern_verification || work.changed_rows != expected_changes) {
    throw BenchmarkMismatch{"write benchmark final verification differs"};
  }
  if (benchmark_case == CaseKind::kMixedRollback && initial_bytes != ReadFile(path)) {
    throw BenchmarkMismatch{"write benchmark rollback changed main database bytes"};
  }
  EnsureNoSidecars(path);
  PrintReport(arguments[2], ProfileName(profile), arguments[4], operations, work,
              sqlite_verification);
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
