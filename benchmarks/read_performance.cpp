#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "read_performance_build_config.hpp"
#include "sqlite3.h"

#ifndef MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
#define MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS 0
#endif

#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS != 0 && \
    MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS != 1
#error "MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS must be 0 or 1"
#endif

#if !defined(NDEBUG)
#error "The read performance benchmark requires NDEBUG"
#endif

#if !defined(_MSC_VER) && !defined(__OPTIMIZE__)
#error "The read performance benchmark requires compiler optimization"
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    defined(__SANITIZE_UNDEFINED__) || defined(__COVERAGE__) || defined(__GCOV__)
#error "The read performance benchmark forbids sanitizers and coverage"
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#error "The read performance benchmark forbids sanitizers"
#endif
#endif

namespace {

constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ULL;
constexpr std::uint64_t kSplitMixIncrement = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kFitSeed = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kPressureSeed = 0xD1B54A32D192ED03ULL;
constexpr std::uint32_t kApplicationId = 1'297'305'936U;
constexpr std::uint32_t kUserVersion = 1U;
constexpr std::uint64_t kMinimumWallNanoseconds = 200'000'000ULL;
constexpr std::size_t kValueSize = 256U;
constexpr std::size_t kIntegerResultBytes = 8U;
constexpr std::size_t kScanResultBytes = kIntegerResultBytes + kValueSize;
constexpr std::size_t kTimingRepetitions = 3U;
constexpr std::uint64_t kResultSinkInitial = 0x6A09E667F3BCC909ULL;
constexpr std::string_view kPointSql = "SELECT v FROM kv WHERE k=?1";
constexpr std::string_view kScanSql = "SELECT k,v FROM kv";
constexpr std::string_view kSqliteVersion = "3.54.0";
constexpr std::string_view kSqliteSourceId =
    "2026-10-02 20:18:07 "
    "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2";

std::atomic<std::uint64_t> result_sink{kResultSinkInitial};

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

enum class WorkloadKind : std::uint8_t {
  kPointPresent,
  kPointMissing,
  kScan,
};

struct WorkResult {
  std::uint64_t operations = 0;
  std::uint64_t items = 0;
  std::uint64_t rows = 0;
  std::uint64_t bytes = 0;
  std::uint64_t result_hits = 0;
  std::uint64_t result_misses = 0;
  std::uint64_t digest = kFnvOffset;

  constexpr bool operator==(const WorkResult&) const noexcept = default;
};

struct WorkloadDefinition {
  std::string_view id;
  WorkloadKind kind;
  std::string_view sql;
  bool pressure = false;
  std::uint64_t seed = 0;
  std::uint64_t row_count = 0;
  std::uint64_t page_count = 0;
  std::uint64_t warmup_iterations = 0;
  std::uint64_t measured_iterations = 0;
  std::uint64_t diagnostic_iterations = 0;
  std::uint64_t items_per_iteration = 0;
  WorkResult warmup;
  WorkResult measured;
  WorkResult diagnostic;
  WorkResult verification;
};

constexpr WorkResult Work(std::uint64_t operations, std::uint64_t items, std::uint64_t rows,
                          std::uint64_t bytes, std::uint64_t result_hits,
                          std::uint64_t result_misses, std::uint64_t digest) noexcept {
  return WorkResult{
      .operations = operations,
      .items = items,
      .rows = rows,
      .bytes = bytes,
      .result_hits = result_hits,
      .result_misses = result_misses,
      .digest = digest,
  };
}

constexpr std::array<WorkloadDefinition, 6> kWorkloads{{
    {
        .id = "point-present-ipk-fit",
        .kind = WorkloadKind::kPointPresent,
        .sql = kPointSql,
        .pressure = false,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 276,
        .warmup_iterations = 4'096,
        .measured_iterations = 1'048'576,
        .diagnostic_iterations = 4'096,
        .items_per_iteration = 1,
        .warmup = Work(4'096, 4'096, 4'096, 1'048'576, 4'096, 0, 0xA7F2817A1D534E06ULL),
        .measured =
            Work(1'048'576, 1'048'576, 1'048'576, 268'435'456, 1'048'576, 0, 0x1FDD4B5874C27725ULL),
        .diagnostic = Work(4'096, 4'096, 4'096, 1'048'576, 4'096, 0, 0xA7F2817A1D534E06ULL),
        .verification = Work(1, 4'096, 4'096, 1'081'344, 1, 0, 0x81D3FC6A19796FADULL),
    },
    {
        .id = "point-missing-ipk-fit",
        .kind = WorkloadKind::kPointMissing,
        .sql = kPointSql,
        .pressure = false,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 276,
        .warmup_iterations = 4'096,
        .measured_iterations = 1'048'576,
        .diagnostic_iterations = 4'096,
        .items_per_iteration = 1,
        .warmup = Work(4'096, 4'096, 0, 0, 0, 4'096, 0x12E4577493E208D1ULL),
        .measured = Work(1'048'576, 1'048'576, 0, 0, 0, 1'048'576, 0x782F767E81E77F25ULL),
        .diagnostic = Work(4'096, 4'096, 0, 0, 0, 4'096, 0x12E4577493E208D1ULL),
        .verification = Work(1, 4'096, 4'096, 1'081'344, 1, 0, 0x81D3FC6A19796FADULL),
    },
    {
        .id = "scan-ipk-fit",
        .kind = WorkloadKind::kScan,
        .sql = kScanSql,
        .pressure = false,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 276,
        .warmup_iterations = 1,
        .measured_iterations = 1'024,
        .diagnostic_iterations = 1,
        .items_per_iteration = 4'096,
        .warmup = Work(1, 4'096, 4'096, 1'081'344, 1, 0, 0x81D3FC6A19796FADULL),
        .measured =
            Work(1'024, 4'194'304, 4'194'304, 1'107'296'256, 1'024, 0, 0xC9B5B3073CA44325ULL),
        .diagnostic = Work(1, 4'096, 4'096, 1'081'344, 1, 0, 0x81D3FC6A19796FADULL),
        .verification = Work(1, 4'096, 4'096, 1'081'344, 1, 0, 0x81D3FC6A19796FADULL),
    },
    {
        .id = "point-present-ipk-pressure",
        .kind = WorkloadKind::kPointPresent,
        .sql = kPointSql,
        .pressure = true,
        .seed = kPressureSeed,
        .row_count = 65'536,
        .page_count = 4'383,
        .warmup_iterations = 65'536,
        .measured_iterations = 1'048'576,
        .diagnostic_iterations = 65'536,
        .items_per_iteration = 1,
        .warmup = Work(65'536, 65'536, 65'536, 16'777'216, 65'536, 0, 0xB1FD505D6186BC52ULL),
        .measured =
            Work(1'048'576, 1'048'576, 1'048'576, 268'435'456, 1'048'576, 0, 0xA74F0B8981386185ULL),
        .diagnostic = Work(65'536, 65'536, 65'536, 16'777'216, 65'536, 0, 0xB1FD505D6186BC52ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0x6D8C1155F69EB405ULL),
    },
    {
        .id = "point-missing-ipk-pressure",
        .kind = WorkloadKind::kPointMissing,
        .sql = kPointSql,
        .pressure = true,
        .seed = kPressureSeed,
        .row_count = 65'536,
        .page_count = 4'383,
        .warmup_iterations = 65'536,
        .measured_iterations = 1'048'576,
        .diagnostic_iterations = 65'536,
        .items_per_iteration = 1,
        .warmup = Work(65'536, 65'536, 0, 0, 0, 65'536, 0x5849C891510BD9B5ULL),
        .measured = Work(1'048'576, 1'048'576, 0, 0, 0, 1'048'576, 0x3A86C57174254025ULL),
        .diagnostic = Work(65'536, 65'536, 0, 0, 0, 65'536, 0x5849C891510BD9B5ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0x6D8C1155F69EB405ULL),
    },
    {
        .id = "scan-ipk-pressure",
        .kind = WorkloadKind::kScan,
        .sql = kScanSql,
        .pressure = true,
        .seed = kPressureSeed,
        .row_count = 65'536,
        .page_count = 4'383,
        .warmup_iterations = 1,
        .measured_iterations = 64,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .warmup = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0x6D8C1155F69EB405ULL),
        .measured = Work(64, 4'194'304, 4'194'304, 1'107'296'256, 64, 0, 0xA5CAACC5A1828B25ULL),
        .diagnostic = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0x6D8C1155F69EB405ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0x6D8C1155F69EB405ULL),
    },
}};

class Digest final {
 public:
  void AddByte(std::uint8_t value) noexcept {
    value_ ^= value;
    value_ *= kFnvPrime;
  }

  void AddTag(char value) noexcept { AddByte(static_cast<std::uint8_t>(value)); }

  void AddKey(std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      AddByte(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
    }
  }

  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

 private:
  std::uint64_t value_ = kFnvOffset;
};

[[nodiscard]] const WorkloadDefinition& FindWorkload(std::string_view id) {
  const auto iterator = std::ranges::find(kWorkloads, id, &WorkloadDefinition::id);
  if (iterator == kWorkloads.end()) {
    throw HarnessFailure{"unknown workload case"};
  }
  return *iterator;
}

[[nodiscard]] EngineKind ParseEngine(std::string_view value) {
  if (value == "modern") {
    return EngineKind::kModern;
  }
  if (value == "sqlite") {
    return EngineKind::kSqlite;
  }
  throw HarnessFailure{"engine must be modern or sqlite"};
}

[[nodiscard]] std::string_view EngineName(EngineKind engine) noexcept {
  return engine == EngineKind::kModern ? "modern" : "sqlite";
}

[[nodiscard]] std::vector<std::uint64_t> GeneratePermutation(std::uint64_t count,
                                                             std::uint64_t seed) {
  if (count == 0 || count > std::numeric_limits<std::size_t>::max()) {
    throw HarnessFailure{"workload row count is not representable"};
  }
  std::vector<std::uint64_t> result(static_cast<std::size_t>(count));
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::uint64_t>(index) + 1U;
  }
  std::uint64_t state = seed;
  auto next = [&state] {
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

[[nodiscard]] std::array<std::byte, 8> ExpectedPrefix(std::uint64_t logical_row) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::array<std::byte, 8> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    const std::size_t shift = (result.size() - index - 1U) * 4U;
    const auto nibble = static_cast<std::size_t>((logical_row >> shift) & 0xFU);
    result[index] = static_cast<std::byte>(digits[nibble]);
  }
  return result;
}

void VerifyValue(modern_sqlite::ByteView value, std::uint64_t logical_row, Digest& digest) {
  if (value.size() != kValueSize) {
    throw BenchmarkMismatch{"result BLOB has the wrong length"};
  }
  const auto prefix = ExpectedPrefix(logical_row);
  for (std::size_t index = 0; index < value.size(); ++index) {
    const std::byte expected = index < prefix.size() ? prefix[index] : static_cast<std::byte>('0');
    if (value[index] != expected) {
      throw BenchmarkMismatch{index < prefix.size() ? "result BLOB has the wrong key prefix"
                                                    : "result BLOB has the wrong payload"};
    }
    digest.AddByte(std::to_integer<std::uint8_t>(value[index]));
  }
}

[[maybe_unused]] void MixResult(WorkResult result) noexcept {
  result_sink.fetch_xor(result.digest ^ result.rows ^ result.bytes, std::memory_order_relaxed);
}

void RequireWork(std::string_view label, const WorkResult& actual, const WorkResult& expected) {
  if (actual != expected) {
    throw BenchmarkMismatch{std::string{label} + " work or digest mismatch"};
  }
}

template <typename T>
[[nodiscard]] T TakeResult(modern_sqlite::Result<T> result, std::string_view operation) {
  if (!result.has_value()) {
    throw BenchmarkMismatch{std::string{operation} + ": " + result.error().ToString()};
  }
  return std::move(*result);
}

void RequireStatus(modern_sqlite::Status status, std::string_view operation) {
  if (!status.has_value()) {
    throw BenchmarkMismatch{std::string{operation} + ": " + status.error().ToString()};
  }
}

[[nodiscard]] modern_sqlite::ReadStatement PrepareModern(modern_sqlite::ReadSession& session,
                                                         std::string_view sql) {
  modern_sqlite::ReadPrepareOutput prepared =
      TakeResult(session.Prepare(modern_sqlite::Utf8View{sql}), "Modern SQLite prepare");
  if (!prepared.statement.has_value() || prepared.next_offset.value() != sql.size()) {
    throw BenchmarkMismatch{"Modern SQLite prepare returned an incomplete statement"};
  }
  return std::move(*prepared.statement);
}

[[nodiscard]] WorkResult RunModernPoint(modern_sqlite::ReadStatement& statement,
                                        const WorkloadDefinition& definition,
                                        std::uint64_t iterations,
                                        std::span<const std::uint64_t> permutation) {
  WorkResult result;
  Digest digest;
  const bool present = definition.kind == WorkloadKind::kPointPresent;
  const char tag = present ? 'P' : 'M';
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const std::size_t permutation_index =
        static_cast<std::size_t>(iteration % definition.row_count);
    const std::uint64_t logical_row = permutation[permutation_index];
    const std::uint64_t key = present ? logical_row * 2U : (logical_row * 2U) - 1U;
    RequireStatus(
        statement.Bind(1, modern_sqlite::SqlValue::Integer(static_cast<std::int64_t>(key))),
        "Modern SQLite bind");
    digest.AddTag(tag);
    digest.AddKey(key);
    const modern_sqlite::ReadStep first = TakeResult(statement.Step(), "Modern SQLite point step");
    if (present) {
      if (first != modern_sqlite::ReadStep::kRow || statement.row().size() != 1U) {
        throw BenchmarkMismatch{"Modern SQLite present lookup returned the wrong row shape"};
      }
      const auto blob = statement.row().front().blob_value();
      if (!blob.has_value()) {
        throw BenchmarkMismatch{"Modern SQLite present lookup did not return a BLOB"};
      }
      VerifyValue(*blob, logical_row, digest);
      if (TakeResult(statement.Step(), "Modern SQLite point terminal step") !=
          modern_sqlite::ReadStep::kDone) {
        throw BenchmarkMismatch{"Modern SQLite present lookup returned more than one row"};
      }
      ++result.rows;
      result.bytes += kValueSize;
      ++result.result_hits;
    } else {
      if (first != modern_sqlite::ReadStep::kDone) {
        throw BenchmarkMismatch{"Modern SQLite missing lookup returned a row"};
      }
      ++result.result_misses;
    }
    digest.AddTag('D');
    RequireStatus(statement.Reset(), "Modern SQLite point reset");
    ++result.operations;
    ++result.items;
  }
  result.digest = digest.value();
  return result;
}

[[nodiscard]] WorkResult RunModernScan(modern_sqlite::ReadStatement& statement,
                                       const WorkloadDefinition& definition,
                                       std::uint64_t iterations) {
  WorkResult result;
  Digest digest;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    digest.AddTag('S');
    std::uint64_t logical_row = 1;
    while (true) {
      const modern_sqlite::ReadStep step = TakeResult(statement.Step(), "Modern SQLite scan step");
      if (step == modern_sqlite::ReadStep::kDone) {
        break;
      }
      if (statement.row().size() != 2U) {
        throw BenchmarkMismatch{"Modern SQLite scan returned the wrong row shape"};
      }
      const auto key_value = statement.row()[0].integer_value();
      const auto blob = statement.row()[1].blob_value();
      const std::uint64_t expected_key = logical_row * 2U;
      if (!key_value.has_value() || *key_value != static_cast<std::int64_t>(expected_key) ||
          !blob.has_value()) {
        throw BenchmarkMismatch{"Modern SQLite scan returned the wrong key or type"};
      }
      digest.AddTag('R');
      digest.AddKey(expected_key);
      VerifyValue(*blob, logical_row, digest);
      ++result.rows;
      result.bytes += kScanResultBytes;
      ++result.items;
      ++logical_row;
    }
    if (logical_row - 1U != definition.row_count) {
      throw BenchmarkMismatch{"Modern SQLite scan returned the wrong row count"};
    }
    digest.AddTag('D');
    RequireStatus(statement.Reset(), "Modern SQLite scan reset");
    ++result.operations;
    ++result.result_hits;
  }
  result.digest = digest.value();
  return result;
}

class ModernEngine final {
 public:
  explicit ModernEngine(modern_sqlite::ReadSession session) : session_(std::move(session)) {}

  [[nodiscard]] static ModernEngine Open(const std::filesystem::path& path) {
    return ModernEngine{
        TakeResult(modern_sqlite::ReadSession::Open(path.string()), "Modern SQLite open")};
  }

  using Statement = modern_sqlite::ReadStatement;

  [[nodiscard]] Statement Prepare(const WorkloadDefinition& definition) {
    return PrepareModern(session_, definition.sql);
  }

  [[nodiscard]] WorkResult Run(Statement& statement, const WorkloadDefinition& definition,
                               std::uint64_t iterations,
                               std::span<const std::uint64_t> permutation) {
    if (definition.kind == WorkloadKind::kScan) {
      return RunModernScan(statement, definition, iterations);
    }
    return RunModernPoint(statement, definition, iterations, permutation);
  }

  void Finalize(Statement& statement) {
    RequireStatus(statement.Finalize(), "Modern SQLite finalize");
  }

  [[nodiscard]] WorkResult Verify(const WorkloadDefinition& definition) {
    WorkloadDefinition scan_definition = definition;
    scan_definition.kind = WorkloadKind::kScan;
    scan_definition.sql = kScanSql;
    Statement statement = Prepare(scan_definition);
    WorkResult result = RunModernScan(statement, scan_definition, 1);
    Finalize(statement);
    return result;
  }

 private:
  modern_sqlite::ReadSession session_;
};

void CheckSqlite(int result, sqlite3* database, std::string_view operation) {
  if (result == SQLITE_OK) {
    return;
  }
  const char* message = database == nullptr ? nullptr : sqlite3_errmsg(database);
  throw BenchmarkMismatch{std::string{operation} + ": " +
                          (message == nullptr ? "SQLite error" : message)};
}

class SqliteStatement final {
 public:
  SqliteStatement() = default;
  explicit SqliteStatement(sqlite3_stmt* statement) noexcept : statement_(statement) {}
  SqliteStatement(const SqliteStatement&) = delete;
  SqliteStatement& operator=(const SqliteStatement&) = delete;
  SqliteStatement(SqliteStatement&& other) noexcept
      : statement_(std::exchange(other.statement_, nullptr)) {}
  SqliteStatement& operator=(SqliteStatement&& other) noexcept {
    if (this != &other) {
      Reset();
      statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
  }
  ~SqliteStatement() { Reset(); }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

  [[nodiscard]] sqlite3_stmt* Release() noexcept { return std::exchange(statement_, nullptr); }

 private:
  void Reset() noexcept {
    if (statement_ != nullptr) {
      static_cast<void>(sqlite3_finalize(statement_));
      statement_ = nullptr;
    }
  }

  sqlite3_stmt* statement_ = nullptr;
};

[[nodiscard]] std::string SqliteSingleText(sqlite3* database, std::string_view sql) {
  sqlite3_stmt* raw_statement = nullptr;
  const char* tail = nullptr;
  CheckSqlite(sqlite3_prepare_v3(database, sql.data(), static_cast<int>(sql.size()), 0,
                                 &raw_statement, &tail),
              database, "SQLite pragma prepare");
  SqliteStatement statement{raw_statement};
  if (tail != sql.data() + sql.size() || sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw BenchmarkMismatch{"SQLite pragma did not return one row"};
  }
  const unsigned char* text = sqlite3_column_text(statement.get(), 0);
  const int length = sqlite3_column_bytes(statement.get(), 0);
  if (text == nullptr || length < 0) {
    throw BenchmarkMismatch{"SQLite pragma returned invalid text"};
  }
  std::string value{reinterpret_cast<const char*>(text), static_cast<std::size_t>(length)};
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw BenchmarkMismatch{"SQLite pragma returned extra rows"};
  }
  return value;
}

[[nodiscard]] std::int64_t SqliteSingleInteger(sqlite3* database, std::string_view sql) {
  sqlite3_stmt* raw_statement = nullptr;
  const char* tail = nullptr;
  CheckSqlite(sqlite3_prepare_v3(database, sql.data(), static_cast<int>(sql.size()), 0,
                                 &raw_statement, &tail),
              database, "SQLite pragma prepare");
  SqliteStatement statement{raw_statement};
  if (tail != sql.data() + sql.size() || sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw BenchmarkMismatch{"SQLite pragma did not return one row"};
  }
  const std::int64_t value = sqlite3_column_int64(statement.get(), 0);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw BenchmarkMismatch{"SQLite pragma returned extra rows"};
  }
  return value;
}

void ConfigureSqlite(sqlite3* database) {
  CheckSqlite(sqlite3_extended_result_codes(database, 0), database, "SQLite extended result codes");
  constexpr std::array<std::pair<int, int>, 5> configurations{{
      {SQLITE_DBCONFIG_DQS_DML, 1},
      {SQLITE_DBCONFIG_DQS_DDL, 1},
      {SQLITE_DBCONFIG_REVERSE_SCANORDER, 0},
      {SQLITE_DBCONFIG_ENABLE_COMMENTS, 1},
      {1023, 17},
  }};
  for (const auto [configuration, expected] : configurations) {
    int effective = 0;
    CheckSqlite(sqlite3_db_config(database, configuration, expected, &effective), database,
                "SQLite db_config");
    if (effective != expected) {
      throw BenchmarkMismatch{"SQLite db_config returned an unexpected value"};
    }
  }
  constexpr std::array<std::pair<int, int>, 7> limits{{
      {SQLITE_LIMIT_LENGTH, 1'048'576},
      {SQLITE_LIMIT_SQL_LENGTH, 65'536},
      {SQLITE_LIMIT_COLUMN, 256},
      {SQLITE_LIMIT_EXPR_DEPTH, 1'000},
      {SQLITE_LIMIT_VDBE_OP, 25'000},
      {SQLITE_LIMIT_FUNCTION_ARG, 1'000},
      {SQLITE_LIMIT_VARIABLE_NUMBER, 256},
  }};
  for (const auto [limit, expected] : limits) {
    static_cast<void>(sqlite3_limit(database, limit, expected));
    if (sqlite3_limit(database, limit, -1) != expected) {
      throw BenchmarkMismatch{"SQLite limit configuration failed"};
    }
  }
  CheckSqlite(sqlite3_exec(database, "PRAGMA cache_size=512", nullptr, nullptr, nullptr), database,
              "SQLite cache_size");
  CheckSqlite(sqlite3_exec(database, "PRAGMA mmap_size=0", nullptr, nullptr, nullptr), database,
              "SQLite mmap_size");
  CheckSqlite(sqlite3_exec(database, "PRAGMA temp_store=MEMORY", nullptr, nullptr, nullptr),
              database, "SQLite temp_store");
  CheckSqlite(sqlite3_exec(database, "PRAGMA synchronous=FULL", nullptr, nullptr, nullptr),
              database, "SQLite synchronous");
  CheckSqlite(sqlite3_exec(database, "PRAGMA query_only=ON", nullptr, nullptr, nullptr), database,
              "SQLite query_only");
  if (SqliteSingleInteger(database, "PRAGMA cache_size") != 512 ||
      SqliteSingleInteger(database, "PRAGMA mmap_size") != 0 ||
      SqliteSingleInteger(database, "PRAGMA temp_store") != 2 ||
      SqliteSingleInteger(database, "PRAGMA synchronous") != 2 ||
      SqliteSingleInteger(database, "PRAGMA query_only") != 1) {
    throw BenchmarkMismatch{"SQLite effective connection settings do not match"};
  }
}

[[nodiscard]] WorkResult RunSqlitePoint(sqlite3* database, sqlite3_stmt* statement,
                                        const WorkloadDefinition& definition,
                                        std::uint64_t iterations,
                                        std::span<const std::uint64_t> permutation) {
  WorkResult result;
  Digest digest;
  const bool present = definition.kind == WorkloadKind::kPointPresent;
  const char tag = present ? 'P' : 'M';
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const std::size_t permutation_index =
        static_cast<std::size_t>(iteration % definition.row_count);
    const std::uint64_t logical_row = permutation[permutation_index];
    const std::uint64_t key = present ? logical_row * 2U : (logical_row * 2U) - 1U;
    CheckSqlite(sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(key)), database,
                "SQLite bind");
    digest.AddTag(tag);
    digest.AddKey(key);
    const int first = sqlite3_step(statement);
    if (present) {
      if (first != SQLITE_ROW || sqlite3_column_count(statement) != 1 ||
          sqlite3_column_type(statement, 0) != SQLITE_BLOB) {
        throw BenchmarkMismatch{"SQLite present lookup returned the wrong row shape"};
      }
      const void* pointer = sqlite3_column_blob(statement, 0);
      const int length = sqlite3_column_bytes(statement, 0);
      if (pointer == nullptr || length != static_cast<int>(kValueSize)) {
        throw BenchmarkMismatch{"SQLite present lookup returned an invalid BLOB"};
      }
      const modern_sqlite::ByteView blob{
          static_cast<const std::byte*>(pointer),
          static_cast<std::size_t>(length),
      };
      VerifyValue(blob, logical_row, digest);
      if (sqlite3_step(statement) != SQLITE_DONE) {
        throw BenchmarkMismatch{"SQLite present lookup returned more than one row"};
      }
      ++result.rows;
      result.bytes += kValueSize;
      ++result.result_hits;
    } else {
      if (first != SQLITE_DONE) {
        throw BenchmarkMismatch{"SQLite missing lookup returned a row"};
      }
      ++result.result_misses;
    }
    digest.AddTag('D');
    CheckSqlite(sqlite3_reset(statement), database, "SQLite point reset");
    ++result.operations;
    ++result.items;
  }
  result.digest = digest.value();
  return result;
}

[[nodiscard]] WorkResult RunSqliteScan(sqlite3* database, sqlite3_stmt* statement,
                                       const WorkloadDefinition& definition,
                                       std::uint64_t iterations) {
  WorkResult result;
  Digest digest;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    digest.AddTag('S');
    std::uint64_t logical_row = 1;
    while (true) {
      const int step = sqlite3_step(statement);
      if (step == SQLITE_DONE) {
        break;
      }
      if (step != SQLITE_ROW || sqlite3_column_count(statement) != 2 ||
          sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 1) != SQLITE_BLOB) {
        throw BenchmarkMismatch{"SQLite scan returned the wrong row shape"};
      }
      const std::uint64_t expected_key = logical_row * 2U;
      if (sqlite3_column_int64(statement, 0) != static_cast<sqlite3_int64>(expected_key)) {
        throw BenchmarkMismatch{"SQLite scan returned the wrong key"};
      }
      const void* pointer = sqlite3_column_blob(statement, 1);
      const int length = sqlite3_column_bytes(statement, 1);
      if (pointer == nullptr || length != static_cast<int>(kValueSize)) {
        throw BenchmarkMismatch{"SQLite scan returned an invalid BLOB"};
      }
      const modern_sqlite::ByteView blob{
          static_cast<const std::byte*>(pointer),
          static_cast<std::size_t>(length),
      };
      digest.AddTag('R');
      digest.AddKey(expected_key);
      VerifyValue(blob, logical_row, digest);
      ++result.rows;
      result.bytes += kScanResultBytes;
      ++result.items;
      ++logical_row;
    }
    if (logical_row - 1U != definition.row_count) {
      throw BenchmarkMismatch{"SQLite scan returned the wrong row count"};
    }
    digest.AddTag('D');
    CheckSqlite(sqlite3_reset(statement), database, "SQLite scan reset");
    ++result.operations;
    ++result.result_hits;
  }
  result.digest = digest.value();
  return result;
}

class SqliteEngine final {
 public:
  SqliteEngine(const SqliteEngine&) = delete;
  SqliteEngine& operator=(const SqliteEngine&) = delete;
  SqliteEngine(SqliteEngine&& other) noexcept
      : database_(std::exchange(other.database_, nullptr)) {}
  SqliteEngine& operator=(SqliteEngine&& other) noexcept {
    if (this != &other) {
      Close();
      database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
  }
  ~SqliteEngine() { Close(); }

  [[nodiscard]] static SqliteEngine Open(const std::filesystem::path& path,
                                         const WorkloadDefinition& definition) {
    sqlite3* database = nullptr;
    const int result = sqlite3_open_v2(path.string().c_str(), &database,
                                       SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    if (result != SQLITE_OK) {
      const char* message = database == nullptr ? nullptr : sqlite3_errmsg(database);
      if (database != nullptr) {
        static_cast<void>(sqlite3_close_v2(database));
      }
      throw BenchmarkMismatch{message == nullptr ? "SQLite open failed" : message};
    }
    SqliteEngine engine{database};
    ConfigureSqlite(database);
    if (sqlite3_db_readonly(database, "main") != 1 ||
        SqliteSingleInteger(database, "PRAGMA page_size") != 4096 ||
        SqliteSingleInteger(database, "PRAGMA page_count") !=
            static_cast<std::int64_t>(definition.page_count) ||
        SqliteSingleText(database, "PRAGMA journal_mode") != "delete" ||
        SqliteSingleText(database, "PRAGMA integrity_check") != "ok") {
      throw BenchmarkMismatch{"SQLite fixture metadata or integrity is invalid"};
    }
    return engine;
  }

  using Statement = SqliteStatement;

  [[nodiscard]] Statement Prepare(const WorkloadDefinition& definition) {
    sqlite3_stmt* statement = nullptr;
    const char* tail = nullptr;
    CheckSqlite(sqlite3_prepare_v3(database_, definition.sql.data(),
                                   static_cast<int>(definition.sql.size()),
                                   SQLITE_PREPARE_PERSISTENT, &statement, &tail),
                database_, "SQLite prepare");
    if (statement == nullptr || tail != definition.sql.data() + definition.sql.size()) {
      if (statement != nullptr) {
        static_cast<void>(sqlite3_finalize(statement));
      }
      throw BenchmarkMismatch{"SQLite prepare returned an incomplete statement"};
    }
    return Statement{statement};
  }

  [[nodiscard]] WorkResult Run(Statement& statement, const WorkloadDefinition& definition,
                               std::uint64_t iterations,
                               std::span<const std::uint64_t> permutation) {
    if (definition.kind == WorkloadKind::kScan) {
      return RunSqliteScan(database_, statement.get(), definition, iterations);
    }
    return RunSqlitePoint(database_, statement.get(), definition, iterations, permutation);
  }

  void Finalize(Statement& statement) {
    sqlite3_stmt* raw = statement.Release();
    if (raw != nullptr) {
      CheckSqlite(sqlite3_finalize(raw), database_, "SQLite finalize");
    }
  }

  [[nodiscard]] WorkResult Verify(const WorkloadDefinition& definition) {
    WorkloadDefinition scan_definition = definition;
    scan_definition.kind = WorkloadKind::kScan;
    scan_definition.sql = kScanSql;
    Statement statement = Prepare(scan_definition);
    WorkResult result = RunSqliteScan(database_, statement.get(), scan_definition, 1);
    Finalize(statement);
    return result;
  }

  [[nodiscard]] sqlite3* database() const noexcept { return database_; }

 private:
  explicit SqliteEngine(sqlite3* database) noexcept : database_(database) {}

  void Close() noexcept {
    if (database_ != nullptr) {
      static_cast<void>(sqlite3_close_v2(database_));
      database_ = nullptr;
    }
  }

  sqlite3* database_ = nullptr;
};

[[maybe_unused, nodiscard]] std::uint64_t ProcessCpuNanoseconds() {
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

struct Repetition {
  std::size_t index = 0;
  std::uint64_t wall_ns = 0;
  std::uint64_t cpu_ns = 0;
  WorkResult work;
};

template <typename Callable>
[[nodiscard]] Repetition Measure(std::size_t index, Callable&& callable) {
  const std::uint64_t cpu_started = ProcessCpuNanoseconds();
  const auto wall_started = std::chrono::steady_clock::now();
  WorkResult work = std::forward<Callable>(callable)();
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
  MixResult(work);
  return Repetition{
      .index = index,
      .wall_ns = static_cast<std::uint64_t>(wall_duration),
      .cpu_ns = cpu_finished - cpu_started,
      .work = work,
  };
}

struct Completion {
  std::uint64_t session_opens = 1;
  std::uint64_t statement_prepares = 0;
  std::uint64_t statement_finalizes = 0;
  std::uint64_t statement_resets = 0;
  std::uint64_t pre_verifications = 1;
  std::uint64_t post_verifications = 1;
};

struct TimingRun {
  WorkResult warmup;
  std::vector<Repetition> repetitions;
  Completion completion;
};

template <typename Engine>
[[nodiscard]] TimingRun RunTiming(Engine& engine, const WorkloadDefinition& definition, bool smoke,
                                  std::span<const std::uint64_t> permutation) {
  RequireWork("pre-verification", engine.Verify(definition), definition.verification);
  typename Engine::Statement statement = engine.Prepare(definition);
  WorkResult warmup = engine.Run(statement, definition, definition.warmup_iterations, permutation);
  RequireWork("warmup", warmup, definition.warmup);

  const std::size_t repetition_count = smoke ? 1U : kTimingRepetitions;
  const std::uint64_t iterations = smoke ? 1U : definition.measured_iterations;
  std::vector<Repetition> repetitions;
  repetitions.reserve(repetition_count);
  for (std::size_t index = 0; index < repetition_count; ++index) {
    Repetition repetition =
        Measure(index, [&engine, &statement, &definition, iterations, permutation] {
          return engine.Run(statement, definition, iterations, permutation);
        });
    if (!smoke) {
      RequireWork("measured repetition", repetition.work, definition.measured);
      if (repetition.wall_ns < kMinimumWallNanoseconds) {
        throw HarnessFailure{"baseline repetition did not reach the minimum wall time"};
      }
    }
    repetitions.push_back(repetition);
  }
  engine.Finalize(statement);
  RequireWork("post-verification", engine.Verify(definition), definition.verification);

  const std::uint64_t measured_resets = iterations * static_cast<std::uint64_t>(repetition_count);
  return TimingRun{
      .warmup = warmup,
      .repetitions = std::move(repetitions),
      .completion =
          Completion{
              .session_opens = 1,
              .statement_prepares = 3,
              .statement_finalizes = 3,
              .statement_resets = definition.warmup_iterations + measured_resets + 2U,
              .pre_verifications = 1,
              .post_verifications = 1,
          },
  };
}

void PrintJsonString(std::ostream& output, std::string_view value) {
  constexpr std::string_view hex = "0123456789abcdef";
  output << '"';
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (byte) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
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
        if (byte < 0x20U) {
          output << "\\u00" << hex[byte >> 4U] << hex[byte & 0x0FU];
        } else {
          output << static_cast<char>(byte);
        }
        break;
    }
  }
  output << '"';
}

void PrintDigest(std::ostream& output, std::uint64_t digest) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::array<char, 16> rendered{};
  for (std::size_t index = 0; index < rendered.size(); ++index) {
    const std::size_t shift = (rendered.size() - index - 1U) * 4U;
    rendered[index] = hex[static_cast<std::size_t>((digest >> shift) & 0xFU)];
  }
  PrintJsonString(output, std::string_view{rendered.data(), rendered.size()});
}

void PrintWork(std::ostream& output, const WorkResult& work) {
  output << "{\"bytes\":" << work.bytes << ",\"digest\":";
  PrintDigest(output, work.digest);
  output << ",\"items\":" << work.items << ",\"operations\":" << work.operations
         << ",\"result_hits\":" << work.result_hits << ",\"result_misses\":" << work.result_misses
         << ",\"rows\":" << work.rows << '}';
}

[[nodiscard]] std::vector<std::string> SqliteCompileOptions() {
  std::vector<std::string> options;
  for (int index = 0;; ++index) {
    const char* option = sqlite3_compileoption_get(index);
    if (option == nullptr) {
      break;
    }
    const std::string_view value{option};
    if (!value.starts_with("COMPILER=")) {
      options.emplace_back(value);
    }
  }
  std::ranges::sort(options);
  return options;
}

void ValidateSqliteIdentity() {
  if (std::string_view{sqlite3_libversion()} != kSqliteVersion ||
      std::string_view{sqlite3_sourceid()} != kSqliteSourceId) {
    throw HarnessFailure{"linked SQLite identity does not match the pinned profile"};
  }
}

void PrintBuild(std::ostream& output, bool instrumentation) {
  output << "{\"build_type\":\"Release\",\"coverage\":false,\"instrumentation\":"
         << (instrumentation ? "true" : "false") << ",\"sanitizers\":false}";
}

void PrintCompilerIdentity(std::ostream& output) {
#if defined(__clang__)
  output << "{\"id\":\"clang\",\"version\":";
  PrintJsonString(output, __clang_version__);
#elif defined(__GNUC__)
  output << "{\"id\":\"gcc\",\"version\":";
  PrintJsonString(output, __VERSION__);
#elif defined(_MSC_VER)
  output << "{\"id\":\"msvc\",\"version\":";
  PrintJsonString(output, std::to_string(_MSC_FULL_VER));
#else
#error "The read performance benchmark requires a recognized compiler"
#endif
  output << '}';
}

void PrintStandardLibraryIdentity(std::ostream& output) {
#if defined(_LIBCPP_VERSION)
  output << "{\"id\":\"libc++\",\"version\":";
  PrintJsonString(output, std::to_string(_LIBCPP_VERSION));
#elif defined(__GLIBCXX__)
  output << "{\"id\":\"libstdc++\",\"version\":";
  PrintJsonString(output, std::to_string(__GLIBCXX__));
#elif defined(_MSVC_STL_VERSION)
  output << "{\"id\":\"msvc-stl\",\"version\":";
  PrintJsonString(output, std::to_string(_MSVC_STL_VERSION));
#else
#error "The read performance benchmark requires a recognized standard library"
#endif
  output << '}';
}

[[nodiscard]] constexpr std::string_view TargetArchitecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
#error "The read performance benchmark requires a recognized target architecture"
#endif
}

void PrintSqliteIdentity(std::ostream& output) {
  const std::vector<std::string> options = SqliteCompileOptions();
  output << "{\"compile_options\":[";
  for (std::size_t index = 0; index < options.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    PrintJsonString(output, options[index]);
  }
  output << "],\"source_id\":";
  PrintJsonString(output, sqlite3_sourceid());
  output << ",\"version\":";
  PrintJsonString(output, sqlite3_libversion());
  output << '}';
}

void PrintIdentityReport() {
  std::cout << "{\"build\":{\"architecture\":";
  PrintJsonString(std::cout, TargetArchitecture());
  std::cout << ",\"build_type\":\"Release\",\"compiler\":";
  PrintCompilerIdentity(std::cout);
  std::cout << ",\"cplusplus\":" << __cplusplus << ",\"coverage\":false,\"instrumentation\":"
            << (MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS ? "true" : "false")
            << ",\"sanitizers\":false,\"standard_library\":";
  PrintStandardLibraryIdentity(std::cout);
  std::cout << "},\"mode\":\"identity\",\"schema_version\":1,\"source\":{\"revision\":";
  PrintJsonString(std::cout, modern_sqlite::read_performance_build_config::kGitRevision);
  std::cout << ",\"tree\":";
  PrintJsonString(std::cout, modern_sqlite::read_performance_build_config::kGitTree);
  std::cout << "},\"sqlite\":";
  PrintSqliteIdentity(std::cout);
  std::cout << "}\n";
}

void PrintConfiguration(std::ostream& output) {
  output << "{\"cache_pages\":512,\"journal_mode\":\"delete\",\"mmap_bytes\":0,"
            "\"page_size\":4096,\"query_only\":true,\"synchronous\":\"full\","
            "\"temp_store\":\"memory\",\"thread_mode\":\"single\"}";
}

void PrintCompletion(std::ostream& output, const Completion& completion) {
  output << "{\"post_verifications\":" << completion.post_verifications
         << ",\"pre_verifications\":" << completion.pre_verifications
         << ",\"session_opens\":" << completion.session_opens
         << ",\"statement_finalizes\":" << completion.statement_finalizes
         << ",\"statement_prepares\":" << completion.statement_prepares
         << ",\"statement_resets\":" << completion.statement_resets << ",\"status\":\"complete\"}";
}

[[maybe_unused]] void PrintTimingReport(EngineKind engine, const WorkloadDefinition& definition,
                                        bool smoke, const TimingRun& run) {
  std::cout << "{\"build\":";
  PrintBuild(std::cout, false);
  std::cout << ",\"case\":";
  PrintJsonString(std::cout, definition.id);
  std::cout << ",\"completion\":";
  PrintCompletion(std::cout, run.completion);
  std::cout << ",\"completion_schema_version\":1,\"effective_configuration\":";
  PrintConfiguration(std::cout);
  std::cout << ",\"engine\":";
  PrintJsonString(std::cout, EngineName(engine));
  std::cout << ",\"mode\":\"timing\",\"repetitions\":[";
  for (std::size_t index = 0; index < run.repetitions.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    const Repetition& repetition = run.repetitions[index];
    std::cout << "{\"bytes\":" << repetition.work.bytes << ",\"cpu_ns\":" << repetition.cpu_ns
              << ",\"digest\":";
    PrintDigest(std::cout, repetition.work.digest);
    std::cout << ",\"index\":" << repetition.index << ",\"items\":" << repetition.work.items
              << ",\"operations\":" << repetition.work.operations
              << ",\"result_hits\":" << repetition.work.result_hits
              << ",\"result_misses\":" << repetition.work.result_misses
              << ",\"rows\":" << repetition.work.rows << ",\"wall_ns\":" << repetition.wall_ns
              << '}';
  }
  std::cout << "],\"run_kind\":";
  PrintJsonString(std::cout, smoke ? "smoke" : "baseline");
  std::cout << ",\"schema_version\":1,\"sqlite\":";
  PrintSqliteIdentity(std::cout);
  std::cout << ",\"timer\":{\"cpu\":\"CLOCK_PROCESS_CPUTIME_ID\","
               "\"wall\":\"steady_clock\"},\"warmup\":";
  PrintWork(std::cout, run.warmup);
  std::cout << ",\"workload_semantics_version\":1}\n";
}

struct ModernCounterValues {
  std::array<std::uint64_t, modern_sqlite::instrumentation::kCounterCount> values{};
};

struct SqliteCounterValues {
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  std::uint64_t cache_writes = 0;
  std::uint64_t cache_bytes_current = 0;
  std::uint64_t vm_steps = 0;
  std::uint64_t fullscan_steps = 0;
  std::uint64_t statement_runs = 0;
  std::uint64_t reprepares = 0;
  std::uint64_t malloc_count_current = 0;
  std::uint64_t malloc_count_highwater = 0;
  std::uint64_t malloc_size_highwater = 0;
};

struct DiagnosticRun {
  WorkResult work;
  Completion completion;
  std::optional<ModernCounterValues> modern;
  std::optional<SqliteCounterValues> sqlite;
};

#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
[[nodiscard]] ModernCounterValues ReadModernCounters(
    const modern_sqlite::instrumentation::CounterCollection& counters) {
  ModernCounterValues result;
  for (std::size_t index = 0; index < result.values.size(); ++index) {
    result.values[index] =
        counters.Value(static_cast<modern_sqlite::instrumentation::Counter>(index));
  }
  return result;
}

[[nodiscard]] DiagnosticRun RunModernDiagnostic(const std::filesystem::path& path,
                                                const WorkloadDefinition& definition,
                                                std::span<const std::uint64_t> permutation) {
  ModernEngine engine = ModernEngine::Open(path);
  RequireWork("pre-verification", engine.Verify(definition), definition.verification);
  ModernEngine::Statement warmup_statement = engine.Prepare(definition);
  const WorkResult warmup =
      engine.Run(warmup_statement, definition, definition.warmup_iterations, permutation);
  RequireWork("warmup", warmup, definition.warmup);
  engine.Finalize(warmup_statement);

  modern_sqlite::instrumentation::CounterCollection counters;
  WorkResult diagnostic;
  {
    const modern_sqlite::instrumentation::ScopedCounterCollection scope{counters};
    ModernEngine::Statement statement = engine.Prepare(definition);
    diagnostic = engine.Run(statement, definition, definition.diagnostic_iterations, permutation);
    engine.Finalize(statement);
  }
  RequireWork("diagnostic", diagnostic, definition.diagnostic);
  RequireWork("post-verification", engine.Verify(definition), definition.verification);
  ModernCounterValues values = ReadModernCounters(counters);
  const auto pages_written =
      values
          .values[static_cast<std::size_t>(modern_sqlite::instrumentation::Counter::kPagesWritten)];
  const auto cache_misses =
      values
          .values[static_cast<std::size_t>(modern_sqlite::instrumentation::Counter::kCacheMisses)];
  if (pages_written != 0 || (!definition.pressure && cache_misses != 0) ||
      (definition.pressure && cache_misses == 0)) {
    throw BenchmarkMismatch{"Modern SQLite diagnostic cache classification failed"};
  }
  return DiagnosticRun{
      .work = diagnostic,
      .completion =
          Completion{
              .session_opens = 1,
              .statement_prepares = 4,
              .statement_finalizes = 4,
              .statement_resets =
                  definition.warmup_iterations + definition.diagnostic_iterations + 2U,
              .pre_verifications = 1,
              .post_verifications = 1,
          },
      .modern = values,
      .sqlite = std::nullopt,
  };
}

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> SqliteDbStatus(sqlite3* database,
                                                                     int operation, bool reset) {
  int current = 0;
  int highwater = 0;
  CheckSqlite(sqlite3_db_status(database, operation, &current, &highwater, reset ? 1 : 0), database,
              "SQLite db status");
  if (current < 0 || highwater < 0) {
    throw BenchmarkMismatch{"SQLite db status returned a negative value"};
  }
  return {static_cast<std::uint64_t>(current), static_cast<std::uint64_t>(highwater)};
}

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> SqliteGlobalStatus(int operation,
                                                                         bool reset) {
  sqlite3_int64 current = 0;
  sqlite3_int64 highwater = 0;
  const int result = sqlite3_status64(operation, &current, &highwater, reset ? 1 : 0);
  if (result != SQLITE_OK || current < 0 || highwater < 0) {
    throw BenchmarkMismatch{"SQLite global status failed"};
  }
  return {static_cast<std::uint64_t>(current), static_cast<std::uint64_t>(highwater)};
}

[[nodiscard]] DiagnosticRun RunSqliteDiagnostic(const std::filesystem::path& path,
                                                const WorkloadDefinition& definition,
                                                std::span<const std::uint64_t> permutation) {
  SqliteEngine engine = SqliteEngine::Open(path, definition);
  RequireWork("pre-verification", engine.Verify(definition), definition.verification);
  SqliteEngine::Statement warmup_statement = engine.Prepare(definition);
  const WorkResult warmup =
      engine.Run(warmup_statement, definition, definition.warmup_iterations, permutation);
  RequireWork("warmup", warmup, definition.warmup);
  engine.Finalize(warmup_statement);

  static_cast<void>(SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_HIT, true));
  static_cast<void>(SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_MISS, true));
  static_cast<void>(SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_WRITE, true));
  static_cast<void>(SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, true));
  static_cast<void>(SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, true));

  SqliteEngine::Statement statement = engine.Prepare(definition);
  const WorkResult diagnostic =
      engine.Run(statement, definition, definition.diagnostic_iterations, permutation);
  RequireWork("diagnostic", diagnostic, definition.diagnostic);
  SqliteCounterValues counters;
  counters.vm_steps = static_cast<std::uint64_t>(
      sqlite3_stmt_status(statement.get(), SQLITE_STMTSTATUS_VM_STEP, 0));
  counters.fullscan_steps = static_cast<std::uint64_t>(
      sqlite3_stmt_status(statement.get(), SQLITE_STMTSTATUS_FULLSCAN_STEP, 0));
  counters.statement_runs =
      static_cast<std::uint64_t>(sqlite3_stmt_status(statement.get(), SQLITE_STMTSTATUS_RUN, 0));
  counters.reprepares = static_cast<std::uint64_t>(
      sqlite3_stmt_status(statement.get(), SQLITE_STMTSTATUS_REPREPARE, 0));
  engine.Finalize(statement);

  counters.cache_hits = SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_HIT, false).first;
  counters.cache_misses =
      SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_MISS, false).first;
  counters.cache_writes =
      SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_WRITE, false).first;
  counters.cache_bytes_current =
      SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_USED, false).first;
  const auto malloc_count = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, false);
  counters.malloc_count_current = malloc_count.first;
  counters.malloc_count_highwater = malloc_count.second;
  counters.malloc_size_highwater = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, false).second;

  RequireWork("post-verification", engine.Verify(definition), definition.verification);
  if (counters.cache_writes != 0 || (!definition.pressure && counters.cache_misses != 0) ||
      (definition.pressure && counters.cache_misses == 0)) {
    throw BenchmarkMismatch{"SQLite diagnostic cache classification failed"};
  }
  return DiagnosticRun{
      .work = diagnostic,
      .completion =
          Completion{
              .session_opens = 1,
              .statement_prepares = 4,
              .statement_finalizes = 4,
              .statement_resets =
                  definition.warmup_iterations + definition.diagnostic_iterations + 2U,
              .pre_verifications = 1,
              .post_verifications = 1,
          },
      .modern = std::nullopt,
      .sqlite = counters,
  };
}
#endif

void PrintModernCounters(std::ostream& output, const ModernCounterValues& counters) {
  output << '{';
  for (std::size_t index = 0; index < counters.values.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    const auto counter = static_cast<modern_sqlite::instrumentation::Counter>(index);
    PrintJsonString(output, modern_sqlite::instrumentation::CounterName(counter));
    output << ':' << counters.values[index];
  }
  output << '}';
}

void PrintSqliteCounters(std::ostream& output, const SqliteCounterValues& counters) {
  output << "{\"cache_bytes_current\":" << counters.cache_bytes_current
         << ",\"cache_hits\":" << counters.cache_hits
         << ",\"cache_misses\":" << counters.cache_misses
         << ",\"cache_writes\":" << counters.cache_writes
         << ",\"fullscan_steps\":" << counters.fullscan_steps
         << ",\"malloc_count_current\":" << counters.malloc_count_current
         << ",\"malloc_count_highwater\":" << counters.malloc_count_highwater
         << ",\"malloc_size_highwater\":" << counters.malloc_size_highwater
         << ",\"reprepares\":" << counters.reprepares
         << ",\"statement_runs\":" << counters.statement_runs
         << ",\"vm_steps\":" << counters.vm_steps << '}';
}

[[maybe_unused]] void PrintDiagnosticReport(EngineKind engine, const WorkloadDefinition& definition,
                                            const DiagnosticRun& run) {
  std::cout << "{\"build\":";
  PrintBuild(std::cout, true);
  std::cout << ",\"case\":";
  PrintJsonString(std::cout, definition.id);
  std::cout << ",\"completion\":";
  PrintCompletion(std::cout, run.completion);
  std::cout << ",\"completion_schema_version\":1,\"counters\":{\"modern\":";
  if (run.modern.has_value()) {
    PrintModernCounters(std::cout, *run.modern);
  } else {
    std::cout << "{}";
  }
  std::cout << ",\"sqlite\":";
  if (run.sqlite.has_value()) {
    PrintSqliteCounters(std::cout, *run.sqlite);
  } else {
    std::cout << "{}";
  }
  std::cout << "},\"diagnostic_schema_version\":1,\"effective_configuration\":";
  PrintConfiguration(std::cout);
  std::cout << ",\"engine\":";
  PrintJsonString(std::cout, EngineName(engine));
  std::cout << ",\"mode\":\"diagnostic\",\"schema_version\":1,\"sqlite\":";
  PrintSqliteIdentity(std::cout);
  std::cout << ",\"work\":";
  PrintWork(std::cout, run.work);
  std::cout << ",\"workload_semantics_version\":1}\n";
}

template <typename Engine>
[[nodiscard]] std::pair<WorkResult, Completion> RunProfile(
    Engine& engine, const WorkloadDefinition& definition, std::uint64_t iterations,
    std::span<const std::uint64_t> permutation) {
  RequireWork("pre-verification", engine.Verify(definition), definition.verification);
  typename Engine::Statement statement = engine.Prepare(definition);
  const WorkResult warmup =
      engine.Run(statement, definition, definition.warmup_iterations, permutation);
  RequireWork("warmup", warmup, definition.warmup);
  WorkResult work = engine.Run(statement, definition, iterations, permutation);
  MixResult(work);
  engine.Finalize(statement);
  RequireWork("post-verification", engine.Verify(definition), definition.verification);
  return {
      work,
      Completion{
          .session_opens = 1,
          .statement_prepares = 3,
          .statement_finalizes = 3,
          .statement_resets = definition.warmup_iterations + iterations + 2U,
          .pre_verifications = 1,
          .post_verifications = 1,
      },
  };
}

[[maybe_unused]] void PrintProfileReport(EngineKind engine, const WorkloadDefinition& definition,
                                         std::uint64_t iterations, const WorkResult& work,
                                         const Completion& completion) {
  std::cout << "{\"case\":";
  PrintJsonString(std::cout, definition.id);
  std::cout << ",\"completion\":";
  PrintCompletion(std::cout, completion);
  std::cout << ",\"completion_schema_version\":1,\"engine\":";
  PrintJsonString(std::cout, EngineName(engine));
  std::cout << ",\"iterations\":" << iterations
            << ",\"mode\":\"profile\","
               "\"schema_version\":1,\"work\":";
  PrintWork(std::cout, work);
  std::cout << "}\n";
}

void ValidateDatabaseFile(const std::filesystem::path& path, const WorkloadDefinition& definition) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw HarnessFailure{"cannot open benchmark database"};
  }
  std::array<unsigned char, 100> header{};
  input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
  if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
      !std::equal(header.begin(), header.begin() + 16,
                  reinterpret_cast<const unsigned char*>("SQLite format 3\0"))) {
    throw HarnessFailure{"benchmark database has an invalid SQLite header"};
  }
  const std::uint32_t page_size =
      (static_cast<std::uint32_t>(header[16]) << 8U) | static_cast<std::uint32_t>(header[17]);
  const auto load32 = [&header](std::size_t offset) {
    return (static_cast<std::uint32_t>(header[offset]) << 24U) |
           (static_cast<std::uint32_t>(header[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(header[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(header[offset + 3U]);
  };
  std::error_code error;
  const std::uintmax_t file_size = std::filesystem::file_size(path, error);
  if (error || page_size != 4096U ||
      file_size != definition.page_count * static_cast<std::uint64_t>(page_size) ||
      load32(28) != definition.page_count || load32(60) != kUserVersion ||
      load32(68) != kApplicationId) {
    throw HarnessFailure{"benchmark database metadata does not match the selected case"};
  }
  for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
    if (std::filesystem::exists(path.string() + std::string{suffix})) {
      throw HarnessFailure{"benchmark database has a sidecar file"};
    }
  }
}

void ValidateNoSidecars(const std::filesystem::path& path) {
  for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
    if (std::filesystem::exists(path.string() + std::string{suffix})) {
      throw BenchmarkMismatch{"read-only benchmark left a sidecar file"};
    }
  }
}

[[maybe_unused, nodiscard]] std::uint64_t ParseIterations(std::string_view value) {
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0 ||
      result > 100'000'000ULL) {
    throw HarnessFailure{"profile iterations must be from 1 through 100000000"};
  }
  return result;
}

void PrintUsage(std::ostream& output, std::string_view program) {
  output << "usage:\n"
         << "  " << program << " identity\n"
         << "  " << program << " run <modern|sqlite> <case> <database> "
         << "<baseline|smoke|diagnostic>\n"
         << "  " << program << " profile <modern|sqlite> <case> <database> <iterations>\n";
}

int RunCommand(int argc, char** argv) {
  if (argc == 2 && std::string_view{argv[1]} == "identity") {
    ValidateSqliteIdentity();
    PrintIdentityReport();
    return 0;
  }
  if (argc != 6) {
    PrintUsage(std::cerr, argv[0]);
    return 1;
  }
  ValidateSqliteIdentity();
  const std::string_view command{argv[1]};
  const EngineKind engine = ParseEngine(argv[2]);
  const WorkloadDefinition& definition = FindWorkload(argv[3]);
  const std::filesystem::path database_path{argv[4]};
  ValidateDatabaseFile(database_path, definition);
  const std::vector<std::uint64_t> permutation =
      GeneratePermutation(definition.row_count, definition.seed);

  if (command == "run") {
    const std::string_view run_kind{argv[5]};
#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
    if (run_kind != "diagnostic") {
      throw HarnessFailure{"diagnostic binary accepts only diagnostic runs"};
    }
    DiagnosticRun run;
    if (engine == EngineKind::kModern) {
      run = RunModernDiagnostic(database_path, definition, permutation);
    } else {
      run = RunSqliteDiagnostic(database_path, definition, permutation);
    }
    ValidateNoSidecars(database_path);
    PrintDiagnosticReport(engine, definition, run);
#else
    if (run_kind != "baseline" && run_kind != "smoke") {
      throw HarnessFailure{"timing binary accepts only baseline or smoke runs"};
    }
    const bool smoke = run_kind == "smoke";
    TimingRun run;
    if (engine == EngineKind::kModern) {
      ModernEngine modern = ModernEngine::Open(database_path);
      run = RunTiming(modern, definition, smoke, permutation);
    } else {
      SqliteEngine sqlite = SqliteEngine::Open(database_path, definition);
      run = RunTiming(sqlite, definition, smoke, permutation);
    }
    ValidateNoSidecars(database_path);
    PrintTimingReport(engine, definition, smoke, run);
#endif
    return 0;
  }

  if (command == "profile") {
#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
    throw HarnessFailure{"diagnostic binary does not provide profile replay"};
#else
    const std::uint64_t iterations = ParseIterations(argv[5]);
    WorkResult work;
    Completion completion;
    if (engine == EngineKind::kModern) {
      ModernEngine modern = ModernEngine::Open(database_path);
      std::tie(work, completion) = RunProfile(modern, definition, iterations, permutation);
    } else {
      SqliteEngine sqlite = SqliteEngine::Open(database_path, definition);
      std::tie(work, completion) = RunProfile(sqlite, definition, iterations, permutation);
    }
    ValidateNoSidecars(database_path);
    PrintProfileReport(engine, definition, iterations, work, completion);
    return 0;
#endif
  }

  PrintUsage(std::cerr, argv[0]);
  return 1;
}

}  // namespace

#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
void* operator new(std::size_t size) {
  MODERN_SQLITE_RECORD_COUNTER(modern_sqlite::instrumentation::Counter::kAllocations, 1U);
  if (void* allocation = std::malloc(size == 0 ? 1U : size); allocation != nullptr) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  MODERN_SQLITE_RECORD_COUNTER(modern_sqlite::instrumentation::Counter::kAllocations, 1U);
  void* allocation = nullptr;
  const std::size_t aligned_size = size == 0 ? static_cast<std::size_t>(alignment) : size;
  if (posix_memalign(&allocation, static_cast<std::size_t>(alignment), aligned_size) == 0) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::align_val_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::align_val_t) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept {
  std::free(allocation);
}
void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept {
  std::free(allocation);
}
#endif

int main(int argc, char** argv) {
  try {
    return RunCommand(argc, argv);
  } catch (const BenchmarkMismatch& error) {
    std::cerr << "read performance mismatch: " << error.what() << '\n';
    return 2;
  } catch (const HarnessFailure& error) {
    std::cerr << "read performance harness error: " << error.what() << '\n';
    return 1;
  } catch (const std::bad_alloc&) {
    std::cerr << "read performance harness error: out of memory\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "read performance harness error: " << error.what() << '\n';
    return 1;
  }
}
