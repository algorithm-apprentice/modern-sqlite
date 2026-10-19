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
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "order_by_performance_build_config.hpp"
#include "sqlite3.h"

#ifndef MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
#define MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS 0
#endif

#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS != 0 && \
    MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS != 1
#error "MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS must be 0 or 1"
#endif

#if !defined(NDEBUG)
#error "The order-by performance benchmark requires NDEBUG"
#endif

#if !defined(_MSC_VER) && !defined(__OPTIMIZE__)
#error "The order-by performance benchmark requires compiler optimization"
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    defined(__SANITIZE_UNDEFINED__) || defined(__COVERAGE__) || defined(__GCOV__)
#error "The order-by performance benchmark forbids sanitizers and coverage"
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#error "The order-by performance benchmark forbids sanitizers"
#endif
#endif

namespace {

constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ULL;
constexpr std::uint64_t kSplitMixIncrement = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kFitSeed = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kPressureSeed = 0xD1B54A32D192ED03ULL;
constexpr std::uint32_t kApplicationId = 1'297'305'936U;
constexpr std::uint32_t kIndexApplicationId = 1'297'305'937U;
constexpr std::uint32_t kOrderApplicationId = 1'297'305'426U;
constexpr std::uint32_t kUserVersion = 1U;
constexpr std::uint64_t kMinimumWallNanoseconds = 200'000'000ULL;
constexpr std::uint64_t kIndexMinimumWallNanoseconds = 5'000'000ULL;
constexpr std::size_t kValueSize = 256U;
constexpr std::size_t kIndexValueSize = 128U;
constexpr std::size_t kOrderValueSize = 256U;
constexpr std::size_t kIntegerResultBytes = 8U;
constexpr std::size_t kScanResultBytes = kIntegerResultBytes + kValueSize;
constexpr std::size_t kTimingRepetitions = 3U;
constexpr std::uint64_t kResultSinkInitial = 0x6A09E667F3BCC909ULL;
constexpr std::string_view kPointSql = "SELECT v FROM kv WHERE k=?1";
constexpr std::string_view kScanSql = "SELECT k,v FROM kv";
constexpr std::string_view kIndexVerificationSql = "SELECT id,payload FROM items";
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
  kIndexEqualityCoveringHit,
  kIndexEqualityCoveringMiss,
  kIndexEqualityNoncoveringHit,
  kIndexMultiEqualityCovering,
  kIndexRangeCovering,
  kIndexRangeNoncovering,
  kIndexUnselectiveNoncovering,
  kIndexUnselectiveCovering,
  kIndexRangeLowerOnlyCovering,
  kIndexRangeUpperOnlyCovering,
  kIndexInsert,
  kIndexUpdate,
  kIndexDelete,
  kIndexCreate,
  kIndexAnalyze,
  kOrder,
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
  bool indexed = false;
  bool query_only = true;
  std::uint64_t seed = 0;
  std::uint64_t row_count = 0;
  std::uint64_t page_count = 0;
  std::uint32_t application_id = kApplicationId;
  std::uint32_t user_version = kUserVersion;
  std::uint64_t minimum_wall_ns = kMinimumWallNanoseconds;
  std::uint64_t warmup_iterations = 0;
  std::uint64_t measured_iterations = 0;
  std::uint64_t diagnostic_iterations = 0;
  std::uint64_t items_per_iteration = 0;
  std::uint64_t result_rows_per_iteration = 0;
  std::size_t payload_size = kIndexValueSize;
  bool temporary_store_memory = true;
  std::size_t sorter_memory_threshold = 1U << 20U;
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

constexpr std::array<WorkloadDefinition, 31> kWorkloads{{
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
    {
        .id = "index-equality-covering-hit",
        .kind = WorkloadKind::kIndexEqualityCoveringHit,
        .sql = "SELECT id,score FROM items WHERE category=?1",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 128,
        .measured_iterations = 4'096,
        .diagnostic_iterations = 128,
        .items_per_iteration = 1,
        .warmup = Work(128, 128, 128, 2'048, 128, 0, 0xAB52CC9D2C671100ULL),
        .measured = Work(4'096, 4'096, 4'096, 65'536, 4'096, 0, 0x361A0560FAAE23FAULL),
        .diagnostic = Work(128, 128, 128, 2'048, 128, 0, 0xAB52CC9D2C671100ULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-equality-covering-miss",
        .kind = WorkloadKind::kIndexEqualityCoveringMiss,
        .sql = "SELECT id,score FROM items WHERE category=?1",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 128,
        .measured_iterations = 4'096,
        .diagnostic_iterations = 128,
        .items_per_iteration = 1,
        .warmup = Work(128, 128, 0, 0, 0, 128, 0x5133EC9CB399BAADULL),
        .measured = Work(4'096, 4'096, 0, 0, 0, 4'096, 0x80E144ABBBC5B0E0ULL),
        .diagnostic = Work(128, 128, 0, 0, 0, 128, 0x5133EC9CB399BAADULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-equality-noncovering-hit",
        .kind = WorkloadKind::kIndexEqualityNoncoveringHit,
        .sql = "SELECT payload FROM items WHERE category=?1",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 128,
        .measured_iterations = 4'096,
        .diagnostic_iterations = 128,
        .items_per_iteration = 1,
        .warmup = Work(128, 128, 128, 16'384, 128, 0, 0x7D2466F408E2807BULL),
        .measured = Work(4'096, 4'096, 4'096, 524'288, 4'096, 0, 0xFA336362F74EB93FULL),
        .diagnostic = Work(128, 128, 128, 16'384, 128, 0, 0x7D2466F408E2807BULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-multi-equality-covering",
        .kind = WorkloadKind::kIndexMultiEqualityCovering,
        .sql = "SELECT id FROM items WHERE category=?1 AND score=?2",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 128,
        .measured_iterations = 4'096,
        .diagnostic_iterations = 128,
        .items_per_iteration = 1,
        .warmup = Work(128, 128, 128, 1'024, 128, 0, 0x6BE770E13C4C5A3EULL),
        .measured = Work(4'096, 4'096, 4'096, 32'768, 4'096, 0, 0x1E9753DC95EE454AULL),
        .diagnostic = Work(128, 128, 128, 1'024, 128, 0, 0x6BE770E13C4C5A3EULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-range-covering",
        .kind = WorkloadKind::kIndexRangeCovering,
        .sql = "SELECT id,score FROM items WHERE category>=?1 AND category<?2",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 2,
        .measured_iterations = 2'048,
        .diagnostic_iterations = 2,
        .items_per_iteration = 64,
        .warmup = Work(2, 128, 128, 2'048, 2, 0, 0x7AFFFC22BF2789A8ULL),
        .measured = Work(2'048, 131'072, 131'072, 2'097'152, 2'048, 0, 0xED6CBC89154321A9ULL),
        .diagnostic = Work(2, 128, 128, 2'048, 2, 0, 0x7AFFFC22BF2789A8ULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-range-noncovering",
        .kind = WorkloadKind::kIndexRangeNoncovering,
        .sql = "SELECT payload FROM items WHERE category>=?1 AND category<?2",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 2,
        .measured_iterations = 2'048,
        .diagnostic_iterations = 2,
        .items_per_iteration = 64,
        .warmup = Work(2, 128, 128, 16'384, 2, 0, 0xE5C3214557991CC2ULL),
        .measured = Work(2'048, 131'072, 131'072, 16'777'216, 2'048, 0, 0x936FBE83F4C7C437ULL),
        .diagnostic = Work(2, 128, 128, 16'384, 2, 0, 0xE5C3214557991CC2ULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-range-lower-only-covering",
        .kind = WorkloadKind::kIndexRangeLowerOnlyCovering,
        .sql = "SELECT id,score FROM items WHERE category>=?1",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 2,
        .measured_iterations = 2'048,
        .diagnostic_iterations = 2,
        .items_per_iteration = 64,
        .warmup = Work(2, 128, 128, 2'048, 2, 0, 0x4C545FA7E9832419ULL),
        .measured = Work(2'048, 131'072, 131'072, 2'097'152, 2'048, 0, 0xCA040064E621F670ULL),
        .diagnostic = Work(2, 128, 128, 2'048, 2, 0, 0x4C545FA7E9832419ULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-range-upper-only-covering",
        .kind = WorkloadKind::kIndexRangeUpperOnlyCovering,
        .sql = "SELECT id,score FROM items WHERE category<?1",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 2,
        .measured_iterations = 2'048,
        .diagnostic_iterations = 2,
        .items_per_iteration = 64,
        .warmup = Work(2, 128, 128, 2'048, 2, 0, 0xDA147AE25E7E020DULL),
        .measured = Work(2'048, 131'072, 131'072, 2'097'152, 2'048, 0, 0x2DCC661ADBE23EC8ULL),
        .diagnostic = Work(2, 128, 128, 2'048, 2, 0, 0xDA147AE25E7E020DULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-unselective-noncovering",
        .kind = WorkloadKind::kIndexUnselectiveNoncovering,
        .sql = "SELECT payload FROM items WHERE flag=?1",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 32,
        .diagnostic_iterations = 1,
        .items_per_iteration = 2'048,
        .warmup = Work(1, 2'048, 2'048, 262'144, 1, 0, 0x9156E0A13A7F27DAULL),
        .measured = Work(32, 65'536, 65'536, 8'388'608, 32, 0, 0x4C5C08A7D63EA9B5ULL),
        .diagnostic = Work(1, 2'048, 2'048, 262'144, 1, 0, 0x9156E0A13A7F27DAULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-unselective-covering",
        .kind = WorkloadKind::kIndexUnselectiveCovering,
        .sql = "SELECT id FROM items WHERE flag=?1",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 256,
        .diagnostic_iterations = 1,
        .items_per_iteration = 2'048,
        .warmup = Work(1, 2'048, 2'048, 16'384, 1, 0, 0xA8CCC1A94D5C8252ULL),
        .measured = Work(256, 524'288, 524'288, 4'194'304, 256, 0, 0x76D9F609F97F9A14ULL),
        .diagnostic = Work(1, 2'048, 2'048, 16'384, 1, 0, 0xA8CCC1A94D5C8252ULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-insert",
        .kind = WorkloadKind::kIndexInsert,
        .sql = "INSERT INTO items(id,category,score,flag,payload) VALUES(?1,?2,?3,?4,?5)",
        .indexed = true,
        .query_only = false,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 4,
        .measured_iterations = 32,
        .diagnostic_iterations = 8,
        .items_per_iteration = 1,
        .warmup = Work(4, 4, 4, 652, 4, 0, 0x6B31DB7280E7B4E1ULL),
        .measured = Work(32, 32, 32, 5'216, 32, 0, 0xF6254A9B814C5F11ULL),
        .diagnostic = Work(8, 8, 8, 1'304, 8, 0, 0x83CBA44B78603DE5ULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-update",
        .kind = WorkloadKind::kIndexUpdate,
        .sql = "UPDATE items SET category=?1,score=?2 WHERE id=?3",
        .indexed = true,
        .query_only = false,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 4,
        .measured_iterations = 32,
        .diagnostic_iterations = 8,
        .items_per_iteration = 1,
        .warmup = Work(4, 4, 4, 108, 4, 0, 0x640FB2C804BA3C01ULL),
        .measured = Work(32, 32, 32, 864, 32, 0, 0xAC05E88DCE68318CULL),
        .diagnostic = Work(8, 8, 8, 216, 8, 0, 0x498C689FA9A9A4EBULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-delete",
        .kind = WorkloadKind::kIndexDelete,
        .sql = "DELETE FROM items WHERE id=?1",
        .indexed = true,
        .query_only = false,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 4,
        .measured_iterations = 32,
        .diagnostic_iterations = 8,
        .items_per_iteration = 1,
        .warmup = Work(4, 4, 4, 32, 4, 0, 0x4863B06C0E585966ULL),
        .measured = Work(32, 32, 32, 256, 32, 0, 0xB44CBDF711546E1FULL),
        .diagnostic = Work(8, 8, 8, 64, 8, 0, 0x25E32D4D5EB54409ULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-create",
        .kind = WorkloadKind::kIndexCreate,
        .sql = "CREATE INDEX benchmark_payload_{iteration} ON items(payload)",
        .indexed = true,
        .query_only = false,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 8,
        .measured_iterations = 8,
        .diagnostic_iterations = 8,
        .items_per_iteration = 4'096,
        .warmup = Work(8, 32'768, 0, 0, 8, 0, 0xE24D1064ED011D15ULL),
        .measured = Work(8, 32'768, 0, 0, 8, 0, 0xE24D1064ED011D15ULL),
        .diagnostic = Work(8, 32'768, 0, 0, 8, 0, 0xE24D1064ED011D15ULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "index-analyze",
        .kind = WorkloadKind::kIndexAnalyze,
        .sql = "ANALYZE",
        .indexed = true,
        .query_only = false,
        .seed = kFitSeed,
        .row_count = 4'096,
        .page_count = 206,
        .application_id = kIndexApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 16,
        .diagnostic_iterations = 1,
        .items_per_iteration = 12'288,
        .warmup = Work(1, 12'288, 0, 0, 1, 0, 0x3437F226ABBCF86DULL),
        .measured = Work(16, 196'608, 0, 0, 16, 0, 0x5C064F72F09A4215ULL),
        .diagnostic = Work(1, 12'288, 0, 0, 1, 0, 0x3437F226ABBCF86DULL),
        .verification = Work(1, 4'096, 4'096, 557'056, 1, 0, 0xD0470B9DD267E30CULL),
    },
    {
        .id = "order-integer-memory",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,score FROM items ORDER BY score+0,id",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 65'536,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = true,
        .sorter_memory_threshold = 64U << 20U,
        .warmup = Work(1, 65'536, 65'536, 1'048'576, 1, 0, 0x37C934632EB10CC7ULL),
        .measured = Work(1, 65'536, 65'536, 1'048'576, 1, 0, 0x37C934632EB10CC7ULL),
        .diagnostic = Work(1, 65'536, 65'536, 1'048'576, 1, 0, 0x37C934632EB10CC7ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-mixed-collated-memory",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,category,score FROM items "
               "ORDER BY iif(flag=0,category,score) COLLATE NOCASE,id",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 65'536,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = true,
        .sorter_memory_threshold = 64U << 20U,
        .warmup = Work(1, 65'536, 65'536, 2'162'688, 1, 0, 0x9C74BEA87A8AC44AULL),
        .measured = Work(1, 65'536, 65'536, 2'162'688, 1, 0, 0x9C74BEA87A8AC44AULL),
        .diagnostic = Work(1, 65'536, 65'536, 2'162'688, 1, 0, 0x9C74BEA87A8AC44AULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-desc-null-memory",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,score FROM items "
               "ORDER BY nullif(score,0) DESC NULLS FIRST,id DESC",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 65'536,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = true,
        .sorter_memory_threshold = 64U << 20U,
        .warmup = Work(1, 65'536, 65'536, 1'048'576, 1, 0, 0xBCD4AE4ED143B5B1ULL),
        .measured = Work(1, 65'536, 65'536, 1'048'576, 1, 0, 0xBCD4AE4ED143B5B1ULL),
        .diagnostic = Work(1, 65'536, 65'536, 1'048'576, 1, 0, 0xBCD4AE4ED143B5B1ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-noncovering-payload-memory",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,payload FROM items ORDER BY category DESC,id",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 65'536,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = true,
        .sorter_memory_threshold = 64U << 20U,
        .warmup = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0x7857D644387BD6F2ULL),
        .measured = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0x7857D644387BD6F2ULL),
        .diagnostic = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0x7857D644387BD6F2ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-minimal-spill-file",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,payload FROM items ORDER BY category,id",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 65'536,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = false,
        .sorter_memory_threshold = 10U << 20U,
        .warmup = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xF0C1F8EC883AC63AULL),
        .measured = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xF0C1F8EC883AC63AULL),
        .diagnostic = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xF0C1F8EC883AC63AULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-multi-spill-file",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,payload FROM items ORDER BY category,id",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 65'536,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = false,
        .sorter_memory_threshold = 2U << 20U,
        .warmup = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xF0C1F8EC883AC63AULL),
        .measured = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xF0C1F8EC883AC63AULL),
        .diagnostic = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xF0C1F8EC883AC63AULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-full-sort-limit-memory",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,payload FROM items ORDER BY category,id LIMIT -1 OFFSET 16384",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 49'152,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = true,
        .sorter_memory_threshold = 64U << 20U,
        .warmup = Work(1, 65'536, 49'152, 12'976'128, 1, 0, 0xFEBEC651A2ECC9D9ULL),
        .measured = Work(1, 65'536, 49'152, 12'976'128, 1, 0, 0xFEBEC651A2ECC9D9ULL),
        .diagnostic = Work(1, 65'536, 49'152, 12'976'128, 1, 0, 0xFEBEC651A2ECC9D9ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-topn-memory",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,score FROM items ORDER BY category,id LIMIT 64 OFFSET 64",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 2,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 64,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = true,
        .sorter_memory_threshold = 64U << 20U,
        .warmup = Work(1, 65'536, 64, 1'024, 1, 0, 0x841C68E96FE9DF19ULL),
        .measured = Work(2, 131'072, 128, 2'048, 2, 0, 0x7F67327863CA78BDULL),
        .diagnostic = Work(1, 65'536, 64, 1'024, 1, 0, 0x841C68E96FE9DF19ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-topn-file",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,payload FROM items ORDER BY payload,id LIMIT 64 OFFSET 60000",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 64,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = false,
        .sorter_memory_threshold = 2U << 20U,
        .warmup = Work(1, 65'536, 64, 16'896, 1, 0, 0xBB6EC424658D0EAFULL),
        .measured = Work(1, 65'536, 64, 16'896, 1, 0, 0xBB6EC424658D0EAFULL),
        .diagnostic = Work(1, 65'536, 64, 16'896, 1, 0, 0xBB6EC424658D0EAFULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
    },
    {
        .id = "order-index-compatible-before",
        .kind = WorkloadKind::kOrder,
        .sql = "SELECT id,payload FROM items ORDER BY score DESC,id",
        .indexed = true,
        .seed = kFitSeed,
        .row_count = 65'536,
        .page_count = 4'866,
        .application_id = kOrderApplicationId,
        .minimum_wall_ns = kIndexMinimumWallNanoseconds,
        .warmup_iterations = 1,
        .measured_iterations = 1,
        .diagnostic_iterations = 1,
        .items_per_iteration = 65'536,
        .result_rows_per_iteration = 65'536,
        .payload_size = kOrderValueSize,
        .temporary_store_memory = true,
        .sorter_memory_threshold = 64U << 20U,
        .warmup = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xC5059DC481CF39C6ULL),
        .measured = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xC5059DC481CF39C6ULL),
        .diagnostic = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xC5059DC481CF39C6ULL),
        .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
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

  void AddBytes(std::span<const std::byte> bytes) noexcept {
    AddKey(bytes.size());
    for (const std::byte value : bytes) {
      AddByte(std::to_integer<std::uint8_t>(value));
    }
  }

  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

 private:
  std::uint64_t value_ = kFnvOffset;
};

[[nodiscard]] const WorkloadDefinition& FindWorkload(std::string_view id) {
  const auto iterator = std::ranges::find(kWorkloads, id, &WorkloadDefinition::id);
  if (iterator == kWorkloads.end() || iterator->kind != WorkloadKind::kOrder) {
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

[[nodiscard]] std::string IndexedText(std::string_view prefix, std::uint64_t logical_row) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result{prefix};
  result.append(4U, '0');
  for (std::size_t index = 0; index < 4U; ++index) {
    const std::size_t shift = (3U - index) * 4U;
    result[prefix.size() + index] = digits[static_cast<std::size_t>((logical_row >> shift) & 0xFU)];
  }
  return result;
}

void AddText(Digest& digest, std::string_view text) noexcept {
  digest.AddKey(text.size());
  for (const char character : text) {
    digest.AddByte(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
  }
}

void VerifyIndexValue(modern_sqlite::ByteView value, std::uint64_t logical_row, Digest& digest,
                      std::size_t expected_size = kIndexValueSize) {
  if (value.size() != expected_size) {
    throw BenchmarkMismatch{"indexed result BLOB has the wrong length"};
  }
  const auto prefix = ExpectedPrefix(logical_row);
  for (std::size_t index = 0; index < value.size(); ++index) {
    const std::byte expected = index < prefix.size() ? prefix[index] : static_cast<std::byte>('0');
    if (value[index] != expected) {
      throw BenchmarkMismatch{index < prefix.size() ? "indexed result BLOB has the wrong key prefix"
                                                    : "indexed result BLOB has the wrong payload"};
    }
    digest.AddByte(std::to_integer<std::uint8_t>(value[index]));
  }
}

[[nodiscard]] bool IsIndexWorkload(WorkloadKind kind) noexcept {
  return kind != WorkloadKind::kPointPresent && kind != WorkloadKind::kPointMissing &&
         kind != WorkloadKind::kScan && kind != WorkloadKind::kOrder;
}

[[nodiscard]] bool IsOrderWorkload(WorkloadKind kind) noexcept {
  return kind == WorkloadKind::kOrder;
}

[[nodiscard]] bool IsStatefulIndexWorkload(WorkloadKind kind) noexcept {
  return kind == WorkloadKind::kIndexInsert || kind == WorkloadKind::kIndexUpdate ||
         kind == WorkloadKind::kIndexDelete || kind == WorkloadKind::kIndexCreate ||
         kind == WorkloadKind::kIndexAnalyze;
}

struct IndexInvocation {
  std::uint64_t logical_row = 0;
  std::uint64_t first_row = 0;
  std::uint64_t end_row = 0;
  std::uint64_t flag = 0;
  std::uint64_t expected_rows = 0;
  std::string first_text;
  std::string second_text;
};

[[nodiscard]] IndexInvocation MakeIndexInvocation(const WorkloadDefinition& definition,
                                                  std::uint64_t iteration,
                                                  std::span<const std::uint64_t> permutation) {
  const auto permutation_index = static_cast<std::size_t>(iteration % definition.row_count);
  const std::uint64_t logical_row = permutation[permutation_index];
  IndexInvocation invocation{
      .logical_row = logical_row,
      .first_row = logical_row,
      .end_row = logical_row + 1U,
      .flag = logical_row % 2U,
      .expected_rows = 1U,
      .first_text = {},
      .second_text = {},
  };
  switch (definition.kind) {
    case WorkloadKind::kIndexEqualityCoveringHit:
    case WorkloadKind::kIndexEqualityNoncoveringHit:
    case WorkloadKind::kIndexMultiEqualityCovering:
      invocation.first_text = IndexedText("category-", logical_row);
      return invocation;
    case WorkloadKind::kIndexEqualityCoveringMiss:
      invocation.first_text = IndexedText("missing-", logical_row);
      invocation.expected_rows = 0U;
      return invocation;
    case WorkloadKind::kIndexRangeCovering:
    case WorkloadKind::kIndexRangeNoncovering:
      invocation.first_row = ((logical_row - 1U) % (definition.row_count - 63U)) + 1U;
      invocation.end_row = invocation.first_row + 64U;
      invocation.expected_rows = 64U;
      invocation.first_text = IndexedText("category-", invocation.first_row);
      invocation.second_text = IndexedText("category-", invocation.end_row);
      return invocation;
    case WorkloadKind::kIndexRangeLowerOnlyCovering:
      invocation.first_row = definition.row_count - 63U;
      invocation.end_row = definition.row_count + 1U;
      invocation.expected_rows = 64U;
      invocation.first_text = IndexedText("category-", invocation.first_row);
      return invocation;
    case WorkloadKind::kIndexRangeUpperOnlyCovering:
      invocation.first_row = 1U;
      invocation.end_row = 65U;
      invocation.expected_rows = 64U;
      invocation.first_text = IndexedText("category-", invocation.end_row);
      return invocation;
    case WorkloadKind::kIndexUnselectiveNoncovering:
    case WorkloadKind::kIndexUnselectiveCovering:
      invocation.expected_rows = definition.row_count / 2U;
      return invocation;
    case WorkloadKind::kPointPresent:
    case WorkloadKind::kPointMissing:
    case WorkloadKind::kScan:
    case WorkloadKind::kOrder:
      throw BenchmarkMismatch{"non-index workload reached indexed invocation setup"};
    case WorkloadKind::kIndexInsert:
    case WorkloadKind::kIndexUpdate:
    case WorkloadKind::kIndexDelete:
    case WorkloadKind::kIndexCreate:
    case WorkloadKind::kIndexAnalyze:
      throw BenchmarkMismatch{"stateful workload reached indexed invocation setup"};
  }
  throw BenchmarkMismatch{"invalid indexed workload kind"};
}

void AddIndexInvocationDigest(Digest& digest, WorkloadKind kind,
                              const IndexInvocation& invocation) {
  digest.AddTag('I');
  digest.AddByte(static_cast<std::uint8_t>(kind));
  digest.AddKey(invocation.logical_row);
  digest.AddKey(invocation.first_row);
  digest.AddKey(invocation.end_row);
  digest.AddKey(invocation.flag);
  AddText(digest, invocation.first_text);
  AddText(digest, invocation.second_text);
}

[[maybe_unused]] void MixResult(WorkResult result) noexcept {
  result_sink.fetch_xor(result.digest ^ result.rows ^ result.bytes, std::memory_order_relaxed);
}

void RequireWork(std::string_view label, const WorkResult& actual, const WorkResult& expected) {
  if (actual != expected) {
    throw BenchmarkMismatch{
        std::string{label} + " work or digest mismatch: actual=" +
        std::to_string(actual.operations) + "/" + std::to_string(actual.items) + "/" +
        std::to_string(actual.rows) + "/" + std::to_string(actual.bytes) + "/" +
        std::to_string(actual.result_hits) + "/" + std::to_string(actual.result_misses) + "/" +
        std::to_string(actual.digest) + " expected=" + std::to_string(expected.operations) + "/" +
        std::to_string(expected.items) + "/" + std::to_string(expected.rows) + "/" +
        std::to_string(expected.bytes) + "/" + std::to_string(expected.result_hits) + "/" +
        std::to_string(expected.result_misses) + "/" + std::to_string(expected.digest)};
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

void DigestModernValue(const modern_sqlite::SqlValue& value, WorkResult& result, Digest& digest) {
  switch (value.type()) {
    case modern_sqlite::SqlValueType::kNull:
      digest.AddTag('N');
      return;
    case modern_sqlite::SqlValueType::kInteger: {
      const std::optional<std::int64_t> integer = value.integer_value();
      if (!integer.has_value()) {
        throw BenchmarkMismatch{"Modern SQLite integer result is malformed"};
      }
      digest.AddTag('I');
      digest.AddKey(std::bit_cast<std::uint64_t>(*integer));
      result.bytes += sizeof(std::int64_t);
      return;
    }
    case modern_sqlite::SqlValueType::kReal: {
      const std::optional<double> real = value.real_value();
      if (!real.has_value()) {
        throw BenchmarkMismatch{"Modern SQLite real result is malformed"};
      }
      digest.AddTag('F');
      digest.AddKey(std::bit_cast<std::uint64_t>(*real));
      result.bytes += sizeof(double);
      return;
    }
    case modern_sqlite::SqlValueType::kText: {
      const std::optional<modern_sqlite::Utf8View> text = value.text_value();
      if (!text.has_value()) {
        throw BenchmarkMismatch{"Modern SQLite text result is malformed"};
      }
      digest.AddTag('T');
      digest.AddBytes(
          std::as_bytes(std::span<const char>{text->bytes().data(), text->bytes().size()}));
      result.bytes += text->bytes().size();
      return;
    }
    case modern_sqlite::SqlValueType::kBlob: {
      const std::optional<modern_sqlite::ByteView> blob = value.blob_value();
      if (!blob.has_value()) {
        throw BenchmarkMismatch{"Modern SQLite blob result is malformed"};
      }
      digest.AddTag('B');
      digest.AddBytes(*blob);
      result.bytes += blob->size();
      return;
    }
  }
  throw BenchmarkMismatch{"Modern SQLite result has an invalid storage class"};
}

[[nodiscard]] WorkResult RunModernOrder(modern_sqlite::ReadStatement& statement,
                                        const WorkloadDefinition& definition,
                                        std::uint64_t iterations) {
  WorkResult result;
  Digest digest;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    digest.AddTag('O');
    std::uint64_t row_count = 0;
    while (true) {
      const modern_sqlite::ReadStep step =
          TakeResult(statement.Step(), "Modern SQLite ORDER BY step");
      if (step == modern_sqlite::ReadStep::kDone) {
        break;
      }
      digest.AddTag('R');
      digest.AddKey(statement.row().size());
      for (const modern_sqlite::SqlValue& value : statement.row()) {
        DigestModernValue(value, result, digest);
      }
      ++row_count;
    }
    if (row_count != definition.result_rows_per_iteration) {
      throw BenchmarkMismatch{"Modern SQLite ORDER BY returned the wrong row count"};
    }
    digest.AddTag('D');
    RequireStatus(statement.Reset(), "Modern SQLite ORDER BY reset");
    ++result.operations;
    result.items += definition.items_per_iteration;
    result.rows += row_count;
    ++result.result_hits;
  }
  result.digest = digest.value();
  return result;
}

void BindModernIndex(modern_sqlite::ReadStatement& statement, const WorkloadDefinition& definition,
                     const IndexInvocation& invocation) {
  switch (definition.kind) {
    case WorkloadKind::kIndexEqualityCoveringHit:
    case WorkloadKind::kIndexEqualityCoveringMiss:
    case WorkloadKind::kIndexEqualityNoncoveringHit:
      RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Text(invocation.first_text)),
                    "Modern SQLite indexed text bind");
      return;
    case WorkloadKind::kIndexMultiEqualityCovering:
      RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Text(invocation.first_text)),
                    "Modern SQLite indexed category bind");
      RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Integer(static_cast<std::int64_t>(
                                          invocation.logical_row % 256U))),
                    "Modern SQLite indexed score bind");
      return;
    case WorkloadKind::kIndexRangeCovering:
    case WorkloadKind::kIndexRangeNoncovering:
      RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Text(invocation.first_text)),
                    "Modern SQLite indexed lower bind");
      RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Text(invocation.second_text)),
                    "Modern SQLite indexed upper bind");
      return;
    case WorkloadKind::kIndexRangeLowerOnlyCovering:
    case WorkloadKind::kIndexRangeUpperOnlyCovering:
      RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Text(invocation.first_text)),
                    "Modern SQLite indexed one-sided bind");
      return;
    case WorkloadKind::kIndexUnselectiveNoncovering:
    case WorkloadKind::kIndexUnselectiveCovering:
      RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(
                                          static_cast<std::int64_t>(invocation.flag))),
                    "Modern SQLite indexed flag bind");
      return;
    case WorkloadKind::kPointPresent:
    case WorkloadKind::kPointMissing:
    case WorkloadKind::kScan:
    case WorkloadKind::kOrder:
      throw BenchmarkMismatch{"non-index workload reached indexed bind"};
    case WorkloadKind::kIndexInsert:
    case WorkloadKind::kIndexUpdate:
    case WorkloadKind::kIndexDelete:
    case WorkloadKind::kIndexCreate:
    case WorkloadKind::kIndexAnalyze:
      throw BenchmarkMismatch{"stateful workload reached indexed bind"};
  }
  throw BenchmarkMismatch{"invalid indexed workload bind"};
}

[[nodiscard]] std::uint64_t ExpectedIndexRow(const WorkloadDefinition& definition,
                                             const IndexInvocation& invocation,
                                             std::uint64_t row_index) {
  switch (definition.kind) {
    case WorkloadKind::kIndexRangeCovering:
    case WorkloadKind::kIndexRangeNoncovering:
    case WorkloadKind::kIndexRangeLowerOnlyCovering:
    case WorkloadKind::kIndexRangeUpperOnlyCovering:
      return invocation.first_row + row_index;
    case WorkloadKind::kIndexUnselectiveNoncovering:
    case WorkloadKind::kIndexUnselectiveCovering:
      return invocation.flag == 0U ? 2U + row_index * 2U : 1U + row_index * 2U;
    case WorkloadKind::kIndexEqualityCoveringHit:
    case WorkloadKind::kIndexEqualityCoveringMiss:
    case WorkloadKind::kIndexEqualityNoncoveringHit:
    case WorkloadKind::kIndexMultiEqualityCovering:
      return invocation.logical_row;
    case WorkloadKind::kPointPresent:
    case WorkloadKind::kPointMissing:
    case WorkloadKind::kScan:
    case WorkloadKind::kOrder:
      throw BenchmarkMismatch{"non-index workload reached indexed row validation"};
    case WorkloadKind::kIndexInsert:
    case WorkloadKind::kIndexUpdate:
    case WorkloadKind::kIndexDelete:
    case WorkloadKind::kIndexCreate:
    case WorkloadKind::kIndexAnalyze:
      throw BenchmarkMismatch{"stateful workload reached indexed row validation"};
  }
  throw BenchmarkMismatch{"invalid indexed workload row validation"};
}

void ValidateModernIndexRow(std::span<const modern_sqlite::SqlValue> row,
                            const WorkloadDefinition& definition, const IndexInvocation& invocation,
                            std::uint64_t row_index, WorkResult& result, Digest& digest) {
  const std::uint64_t expected_row = ExpectedIndexRow(definition, invocation, row_index);
  digest.AddTag('R');
  digest.AddKey(expected_row);
  switch (definition.kind) {
    case WorkloadKind::kIndexEqualityCoveringHit:
    case WorkloadKind::kIndexRangeCovering:
    case WorkloadKind::kIndexRangeLowerOnlyCovering:
    case WorkloadKind::kIndexRangeUpperOnlyCovering: {
      if (row.size() != 2U ||
          row[0].integer_value() !=
              std::optional<std::int64_t>{static_cast<std::int64_t>(expected_row)} ||
          row[1].integer_value() !=
              std::optional<std::int64_t>{static_cast<std::int64_t>(expected_row % 256U)}) {
        throw BenchmarkMismatch{"Modern SQLite indexed covering row is invalid"};
      }
      digest.AddKey(expected_row % 256U);
      result.bytes += 2U * kIntegerResultBytes;
      return;
    }
    case WorkloadKind::kIndexEqualityCoveringMiss:
      throw BenchmarkMismatch{"Modern SQLite missing indexed lookup returned a row"};
    case WorkloadKind::kIndexEqualityNoncoveringHit:
    case WorkloadKind::kIndexRangeNoncovering:
    case WorkloadKind::kIndexUnselectiveNoncovering: {
      const std::optional<modern_sqlite::ByteView> blob =
          row.size() == 1U ? row[0].blob_value() : std::nullopt;
      if (!blob.has_value()) {
        throw BenchmarkMismatch{"Modern SQLite indexed payload row is invalid"};
      }
      VerifyIndexValue(*blob, expected_row, digest);
      result.bytes += kIndexValueSize;
      return;
    }
    case WorkloadKind::kIndexMultiEqualityCovering:
    case WorkloadKind::kIndexUnselectiveCovering:
      if (row.size() != 1U ||
          row[0].integer_value() !=
              std::optional<std::int64_t>{static_cast<std::int64_t>(expected_row)}) {
        throw BenchmarkMismatch{"Modern SQLite indexed rowid result is invalid"};
      }
      result.bytes += kIntegerResultBytes;
      return;
    case WorkloadKind::kPointPresent:
    case WorkloadKind::kPointMissing:
    case WorkloadKind::kScan:
    case WorkloadKind::kOrder:
      throw BenchmarkMismatch{"non-index workload reached indexed result validation"};
    case WorkloadKind::kIndexInsert:
    case WorkloadKind::kIndexUpdate:
    case WorkloadKind::kIndexDelete:
    case WorkloadKind::kIndexCreate:
    case WorkloadKind::kIndexAnalyze:
      throw BenchmarkMismatch{"stateful workload reached indexed result validation"};
  }
  throw BenchmarkMismatch{"invalid indexed result shape"};
}

[[nodiscard]] WorkResult RunModernIndex(modern_sqlite::ReadStatement& statement,
                                        const WorkloadDefinition& definition,
                                        std::uint64_t iterations,
                                        std::span<const std::uint64_t> permutation) {
  WorkResult result;
  Digest digest;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const IndexInvocation invocation = MakeIndexInvocation(definition, iteration, permutation);
    BindModernIndex(statement, definition, invocation);
    AddIndexInvocationDigest(digest, definition.kind, invocation);
    std::uint64_t row_index = 0;
    while (true) {
      const modern_sqlite::ReadStep step =
          TakeResult(statement.Step(), "Modern SQLite indexed step");
      if (step == modern_sqlite::ReadStep::kDone) {
        break;
      }
      ValidateModernIndexRow(statement.row(), definition, invocation, row_index, result, digest);
      ++row_index;
    }
    if (row_index != invocation.expected_rows) {
      throw BenchmarkMismatch{"Modern SQLite indexed query returned the wrong row count"};
    }
    digest.AddTag('D');
    RequireStatus(statement.Reset(), "Modern SQLite indexed reset");
    ++result.operations;
    result.items += definition.items_per_iteration;
    result.rows += row_index;
    if (row_index == 0U) {
      ++result.result_misses;
    } else {
      ++result.result_hits;
    }
  }
  result.digest = digest.value();
  return result;
}

[[nodiscard]] WorkResult VerifyModernIndex(modern_sqlite::ReadSession& session,
                                           const WorkloadDefinition& definition) {
  modern_sqlite::ReadStatement statement = PrepareModern(session, kIndexVerificationSql);
  WorkResult result;
  Digest digest;
  digest.AddTag('V');
  std::uint64_t expected_row = 1U;
  while (true) {
    const modern_sqlite::ReadStep step =
        TakeResult(statement.Step(), "Modern SQLite indexed verification step");
    if (step == modern_sqlite::ReadStep::kDone) {
      break;
    }
    const std::span<const modern_sqlite::SqlValue> row = statement.row();
    const std::optional<modern_sqlite::ByteView> blob =
        row.size() == 2U ? row[1].blob_value() : std::nullopt;
    if (row.size() != 2U ||
        row[0].integer_value() !=
            std::optional<std::int64_t>{static_cast<std::int64_t>(expected_row)} ||
        !blob.has_value()) {
      throw BenchmarkMismatch{"Modern SQLite indexed verification row is invalid"};
    }
    digest.AddTag('R');
    digest.AddKey(expected_row);
    VerifyIndexValue(*blob, expected_row, digest, definition.payload_size);
    result.bytes += kIntegerResultBytes + definition.payload_size;
    ++result.items;
    ++result.rows;
    ++expected_row;
  }
  if (expected_row - 1U != definition.row_count) {
    throw BenchmarkMismatch{"Modern SQLite indexed verification row count is invalid"};
  }
  digest.AddTag('D');
  RequireStatus(statement.Reset(), "Modern SQLite indexed verification reset");
  RequireStatus(statement.Finalize(), "Modern SQLite indexed verification finalize");
  result.operations = 1U;
  result.result_hits = 1U;
  result.digest = digest.value();
  return result;
}

class ModernEngine final {
 public:
  explicit ModernEngine(modern_sqlite::ReadSession session) : session_(std::move(session)) {}

  [[nodiscard]] static ModernEngine Open(const std::filesystem::path& path,
                                         const WorkloadDefinition& definition) {
    if (IsOrderWorkload(definition.kind)) {
      return ModernEngine{TakeResult(
          modern_sqlite::ReadSession::Open(
              path.string(),
              modern_sqlite::ReadSessionOptions{
                  .temporary_storage =
                      modern_sqlite::TemporaryStorageOptions{
                          .mode = definition.temporary_store_memory
                                      ? modern_sqlite::TemporaryStoreMode::kMemory
                                      : modern_sqlite::TemporaryStoreMode::kFile,
                          .sorter_memory_threshold =
                              modern_sqlite::ByteCount{definition.sorter_memory_threshold},
                      },
              }),
          "Modern SQLite ORDER BY open")};
    }
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
    if (IsOrderWorkload(definition.kind)) {
      return RunModernOrder(statement, definition, iterations);
    }
    if (IsIndexWorkload(definition.kind)) {
      return RunModernIndex(statement, definition, iterations, permutation);
    }
    if (definition.kind == WorkloadKind::kScan) {
      return RunModernScan(statement, definition, iterations);
    }
    return RunModernPoint(statement, definition, iterations, permutation);
  }

  void Finalize(Statement& statement) {
    RequireStatus(statement.Finalize(), "Modern SQLite finalize");
  }

  [[nodiscard]] WorkResult Verify(const WorkloadDefinition& definition) {
    if (definition.indexed) {
      return VerifyModernIndex(session_, definition);
    }
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
  const SqliteStatement statement{raw_statement};
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

void ConfigureSqlite(sqlite3* database, const WorkloadDefinition& definition) {
  const bool query_only = definition.query_only;
  CheckSqlite(sqlite3_extended_result_codes(database, 0), database, "SQLite extended result codes");
  constexpr std::array<std::pair<int, int>, 5> configurations{{
      {SQLITE_DBCONFIG_DQS_DML, 1},
      {SQLITE_DBCONFIG_DQS_DDL, 1},
      {SQLITE_DBCONFIG_REVERSE_SCANORDER, 0},
      {SQLITE_DBCONFIG_ENABLE_COMMENTS, 1},
      {1023, 17},
  }};
  for (const auto& [configuration, expected] : configurations) {
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
  for (const auto& [limit, expected] : limits) {
    static_cast<void>(sqlite3_limit(database, limit, expected));
    if (sqlite3_limit(database, limit, -1) != expected) {
      throw BenchmarkMismatch{"SQLite limit configuration failed"};
    }
  }
  CheckSqlite(sqlite3_exec(database, "PRAGMA cache_size=512", nullptr, nullptr, nullptr), database,
              "SQLite cache_size");
  CheckSqlite(sqlite3_exec(database, "PRAGMA mmap_size=0", nullptr, nullptr, nullptr), database,
              "SQLite mmap_size");
  const bool order_workload = IsOrderWorkload(definition.kind);
  const char* temp_store = !order_workload || definition.temporary_store_memory ? "MEMORY" : "FILE";
  const std::string temp_store_pragma = "PRAGMA temp_store=" + std::string{temp_store};
  CheckSqlite(sqlite3_exec(database, temp_store_pragma.c_str(), nullptr, nullptr, nullptr),
              database, "SQLite temp_store");
  if (order_workload) {
    const std::size_t threshold_pages = 1U + (definition.sorter_memory_threshold - 1U) / 4096U;
    const std::string temp_cache_pragma =
        "PRAGMA temp.cache_size=" + std::to_string(std::max<std::size_t>(1U, threshold_pages));
    CheckSqlite(sqlite3_exec(database, temp_cache_pragma.c_str(), nullptr, nullptr, nullptr),
                database, "SQLite temp cache_size");
    if (SqliteSingleInteger(database, "PRAGMA temp.cache_size") !=
        static_cast<std::int64_t>(threshold_pages)) {
      throw BenchmarkMismatch{"SQLite temp cache_size does not match the ORDER BY contract"};
    }
  }
  CheckSqlite(sqlite3_exec(database, "PRAGMA synchronous=FULL", nullptr, nullptr, nullptr),
              database, "SQLite synchronous");
  CheckSqlite(sqlite3_exec(database, query_only ? "PRAGMA query_only=ON" : "PRAGMA query_only=OFF",
                           nullptr, nullptr, nullptr),
              database, "SQLite query_only");
  if (SqliteSingleInteger(database, "PRAGMA cache_size") != 512 ||
      SqliteSingleInteger(database, "PRAGMA mmap_size") != 0 ||
      SqliteSingleInteger(database, "PRAGMA temp_store") !=
          ((!order_workload || definition.temporary_store_memory) ? 2 : 1) ||
      SqliteSingleInteger(database, "PRAGMA synchronous") != 2 ||
      SqliteSingleInteger(database, "PRAGMA query_only") != (query_only ? 1 : 0)) {
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

void DigestSqliteValue(sqlite3_stmt* statement, int column, WorkResult& result, Digest& digest) {
  switch (sqlite3_column_type(statement, column)) {
    case SQLITE_NULL:
      digest.AddTag('N');
      return;
    case SQLITE_INTEGER:
      digest.AddTag('I');
      digest.AddKey(std::bit_cast<std::uint64_t>(
          static_cast<std::int64_t>(sqlite3_column_int64(statement, column))));
      result.bytes += sizeof(std::int64_t);
      return;
    case SQLITE_FLOAT:
      digest.AddTag('F');
      digest.AddKey(std::bit_cast<std::uint64_t>(sqlite3_column_double(statement, column)));
      result.bytes += sizeof(double);
      return;
    case SQLITE_TEXT: {
      const int length = sqlite3_column_bytes(statement, column);
      const auto* pointer = sqlite3_column_text(statement, column);
      if (length < 0 || (length != 0 && pointer == nullptr)) {
        throw BenchmarkMismatch{"SQLite ORDER BY text result is malformed"};
      }
      digest.AddTag('T');
      digest.AddBytes(std::span<const std::byte>{reinterpret_cast<const std::byte*>(pointer),
                                                 static_cast<std::size_t>(length)});
      result.bytes += static_cast<std::size_t>(length);
      return;
    }
    case SQLITE_BLOB: {
      const int length = sqlite3_column_bytes(statement, column);
      const void* pointer = sqlite3_column_blob(statement, column);
      if (length < 0 || (length != 0 && pointer == nullptr)) {
        throw BenchmarkMismatch{"SQLite ORDER BY blob result is malformed"};
      }
      digest.AddTag('B');
      digest.AddBytes(std::span<const std::byte>{static_cast<const std::byte*>(pointer),
                                                 static_cast<std::size_t>(length)});
      result.bytes += static_cast<std::size_t>(length);
      return;
    }
    default:
      throw BenchmarkMismatch{"SQLite ORDER BY result has an invalid storage class"};
  }
}

[[nodiscard]] WorkResult RunSqliteOrder(sqlite3* database, sqlite3_stmt* statement,
                                        const WorkloadDefinition& definition,
                                        std::uint64_t iterations) {
  WorkResult result;
  Digest digest;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    digest.AddTag('O');
    std::uint64_t row_count = 0;
    while (true) {
      const int step = sqlite3_step(statement);
      if (step == SQLITE_DONE) {
        break;
      }
      if (step != SQLITE_ROW) {
        CheckSqlite(step, database, "SQLite ORDER BY step");
      }
      const int column_count = sqlite3_column_count(statement);
      if (column_count <= 0) {
        throw BenchmarkMismatch{"SQLite ORDER BY returned no result columns"};
      }
      digest.AddTag('R');
      digest.AddKey(static_cast<std::uint64_t>(column_count));
      for (int column = 0; column < column_count; ++column) {
        DigestSqliteValue(statement, column, result, digest);
      }
      ++row_count;
    }
    if (row_count != definition.result_rows_per_iteration) {
      throw BenchmarkMismatch{"SQLite ORDER BY returned the wrong row count"};
    }
    digest.AddTag('D');
    CheckSqlite(sqlite3_reset(statement), database, "SQLite ORDER BY reset");
    ++result.operations;
    result.items += definition.items_per_iteration;
    result.rows += row_count;
    ++result.result_hits;
  }
  result.digest = digest.value();
  return result;
}

void BindSqliteIndex(sqlite3* database, sqlite3_stmt* statement,
                     const WorkloadDefinition& definition, const IndexInvocation& invocation) {
  const auto bind_text = [&](int parameter, const std::string& value, std::string_view operation) {
    CheckSqlite(sqlite3_bind_text(statement, parameter, value.data(),
                                  static_cast<int>(value.size()), nullptr),
                database, operation);
  };
  switch (definition.kind) {
    case WorkloadKind::kIndexEqualityCoveringHit:
    case WorkloadKind::kIndexEqualityCoveringMiss:
    case WorkloadKind::kIndexEqualityNoncoveringHit:
      bind_text(1, invocation.first_text, "SQLite indexed text bind");
      return;
    case WorkloadKind::kIndexMultiEqualityCovering:
      bind_text(1, invocation.first_text, "SQLite indexed category bind");
      CheckSqlite(sqlite3_bind_int64(statement, 2,
                                     static_cast<sqlite3_int64>(invocation.logical_row % 256U)),
                  database, "SQLite indexed score bind");
      return;
    case WorkloadKind::kIndexRangeCovering:
    case WorkloadKind::kIndexRangeNoncovering:
      bind_text(1, invocation.first_text, "SQLite indexed lower bind");
      bind_text(2, invocation.second_text, "SQLite indexed upper bind");
      return;
    case WorkloadKind::kIndexRangeLowerOnlyCovering:
    case WorkloadKind::kIndexRangeUpperOnlyCovering:
      bind_text(1, invocation.first_text, "SQLite indexed one-sided bind");
      return;
    case WorkloadKind::kIndexUnselectiveNoncovering:
    case WorkloadKind::kIndexUnselectiveCovering:
      CheckSqlite(sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(invocation.flag)),
                  database, "SQLite indexed flag bind");
      return;
    case WorkloadKind::kPointPresent:
    case WorkloadKind::kPointMissing:
    case WorkloadKind::kScan:
    case WorkloadKind::kOrder:
      throw BenchmarkMismatch{"non-index workload reached SQLite indexed bind"};
    case WorkloadKind::kIndexInsert:
    case WorkloadKind::kIndexUpdate:
    case WorkloadKind::kIndexDelete:
    case WorkloadKind::kIndexCreate:
    case WorkloadKind::kIndexAnalyze:
      throw BenchmarkMismatch{"stateful workload reached SQLite indexed bind"};
  }
  throw BenchmarkMismatch{"invalid SQLite indexed workload bind"};
}

void ValidateSqliteIndexRow(sqlite3_stmt* statement, const WorkloadDefinition& definition,
                            const IndexInvocation& invocation, std::uint64_t row_index,
                            WorkResult& result, Digest& digest) {
  const std::uint64_t expected_row = ExpectedIndexRow(definition, invocation, row_index);
  digest.AddTag('R');
  digest.AddKey(expected_row);
  switch (definition.kind) {
    case WorkloadKind::kIndexEqualityCoveringHit:
    case WorkloadKind::kIndexRangeCovering:
    case WorkloadKind::kIndexRangeLowerOnlyCovering:
    case WorkloadKind::kIndexRangeUpperOnlyCovering:
      if (sqlite3_column_count(statement) != 2 ||
          sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
          sqlite3_column_int64(statement, 0) != static_cast<sqlite3_int64>(expected_row) ||
          sqlite3_column_int64(statement, 1) != static_cast<sqlite3_int64>(expected_row % 256U)) {
        throw BenchmarkMismatch{"SQLite indexed covering row is invalid"};
      }
      digest.AddKey(expected_row % 256U);
      result.bytes += 2U * kIntegerResultBytes;
      return;
    case WorkloadKind::kIndexEqualityCoveringMiss:
      throw BenchmarkMismatch{"SQLite missing indexed lookup returned a row"};
    case WorkloadKind::kIndexEqualityNoncoveringHit:
    case WorkloadKind::kIndexRangeNoncovering:
    case WorkloadKind::kIndexUnselectiveNoncovering: {
      if (sqlite3_column_count(statement) != 1 ||
          sqlite3_column_type(statement, 0) != SQLITE_BLOB) {
        throw BenchmarkMismatch{"SQLite indexed payload row is invalid"};
      }
      const void* pointer = sqlite3_column_blob(statement, 0);
      const int length = sqlite3_column_bytes(statement, 0);
      if (pointer == nullptr || length != static_cast<int>(definition.payload_size)) {
        throw BenchmarkMismatch{"SQLite indexed payload bytes are invalid"};
      }
      VerifyIndexValue(modern_sqlite::ByteView{static_cast<const std::byte*>(pointer),
                                               static_cast<std::size_t>(length)},
                       expected_row, digest);
      result.bytes += kIndexValueSize;
      return;
    }
    case WorkloadKind::kIndexMultiEqualityCovering:
    case WorkloadKind::kIndexUnselectiveCovering:
      if (sqlite3_column_count(statement) != 1 ||
          sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
          sqlite3_column_int64(statement, 0) != static_cast<sqlite3_int64>(expected_row)) {
        throw BenchmarkMismatch{"SQLite indexed rowid result is invalid"};
      }
      result.bytes += kIntegerResultBytes;
      return;
    case WorkloadKind::kPointPresent:
    case WorkloadKind::kPointMissing:
    case WorkloadKind::kScan:
    case WorkloadKind::kOrder:
      throw BenchmarkMismatch{"non-index workload reached SQLite indexed result validation"};
    case WorkloadKind::kIndexInsert:
    case WorkloadKind::kIndexUpdate:
    case WorkloadKind::kIndexDelete:
    case WorkloadKind::kIndexCreate:
    case WorkloadKind::kIndexAnalyze:
      throw BenchmarkMismatch{"stateful workload reached SQLite indexed result validation"};
  }
  throw BenchmarkMismatch{"invalid SQLite indexed result shape"};
}

[[nodiscard]] WorkResult RunSqliteIndex(sqlite3* database, sqlite3_stmt* statement,
                                        const WorkloadDefinition& definition,
                                        std::uint64_t iterations,
                                        std::span<const std::uint64_t> permutation) {
  WorkResult result;
  Digest digest;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const IndexInvocation invocation = MakeIndexInvocation(definition, iteration, permutation);
    BindSqliteIndex(database, statement, definition, invocation);
    AddIndexInvocationDigest(digest, definition.kind, invocation);
    std::uint64_t row_index = 0;
    while (true) {
      const int step = sqlite3_step(statement);
      if (step == SQLITE_DONE) {
        break;
      }
      if (step != SQLITE_ROW) {
        CheckSqlite(step, database, "SQLite indexed step");
      }
      ValidateSqliteIndexRow(statement, definition, invocation, row_index, result, digest);
      ++row_index;
    }
    if (row_index != invocation.expected_rows) {
      throw BenchmarkMismatch{"SQLite indexed query returned the wrong row count"};
    }
    digest.AddTag('D');
    CheckSqlite(sqlite3_reset(statement), database, "SQLite indexed reset");
    ++result.operations;
    result.items += definition.items_per_iteration;
    result.rows += row_index;
    if (row_index == 0U) {
      ++result.result_misses;
    } else {
      ++result.result_hits;
    }
  }
  result.digest = digest.value();
  return result;
}

[[nodiscard]] WorkResult VerifySqliteIndex(sqlite3* database,
                                           const WorkloadDefinition& definition) {
  sqlite3_stmt* raw_statement = nullptr;
  const char* tail = nullptr;
  CheckSqlite(sqlite3_prepare_v3(database, kIndexVerificationSql.data(),
                                 static_cast<int>(kIndexVerificationSql.size()),
                                 SQLITE_PREPARE_PERSISTENT, &raw_statement, &tail),
              database, "SQLite indexed verification prepare");
  SqliteStatement statement{raw_statement};
  if (raw_statement == nullptr ||
      tail != kIndexVerificationSql.data() + kIndexVerificationSql.size()) {
    throw BenchmarkMismatch{"SQLite indexed verification prepare was incomplete"};
  }
  WorkResult result;
  Digest digest;
  digest.AddTag('V');
  std::uint64_t expected_row = 1U;
  while (true) {
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW || sqlite3_column_count(statement.get()) != 2 ||
        sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(statement.get(), 0) != static_cast<sqlite3_int64>(expected_row) ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_BLOB) {
      throw BenchmarkMismatch{"SQLite indexed verification row is invalid"};
    }
    const void* pointer = sqlite3_column_blob(statement.get(), 1);
    const int length = sqlite3_column_bytes(statement.get(), 1);
    if (pointer == nullptr || length != static_cast<int>(definition.payload_size)) {
      throw BenchmarkMismatch{"SQLite indexed verification payload is invalid"};
    }
    digest.AddTag('R');
    digest.AddKey(expected_row);
    VerifyIndexValue(modern_sqlite::ByteView{static_cast<const std::byte*>(pointer),
                                             static_cast<std::size_t>(length)},
                     expected_row, digest, definition.payload_size);
    result.bytes += kIntegerResultBytes + definition.payload_size;
    ++result.items;
    ++result.rows;
    ++expected_row;
  }
  if (expected_row - 1U != definition.row_count) {
    throw BenchmarkMismatch{"SQLite indexed verification row count is invalid"};
  }
  digest.AddTag('D');
  CheckSqlite(sqlite3_reset(statement.get()), database, "SQLite indexed verification reset");
  result.operations = 1U;
  result.result_hits = 1U;
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
    if (IsOrderWorkload(definition.kind)) {
      if (definition.sorter_memory_threshold % 4096U != 0U) {
        throw BenchmarkMismatch{"SQLite PMA threshold is not page aligned"};
      }
      const std::size_t threshold_pages = definition.sorter_memory_threshold / 4096U;
      if (threshold_pages == 0U || threshold_pages > std::numeric_limits<unsigned int>::max()) {
        throw BenchmarkMismatch{"SQLite PMA threshold page count is invalid"};
      }
      static_cast<void>(sqlite3_shutdown());
      const int configured =
          sqlite3_config(SQLITE_CONFIG_PMASZ, static_cast<unsigned int>(threshold_pages));
      if (configured != SQLITE_OK) {
        throw BenchmarkMismatch{"SQLite PMA size configuration failed"};
      }
    }
    sqlite3* database = nullptr;
    const int open_mode = definition.query_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    const int result =
        sqlite3_open_v2(path.string().c_str(), &database, open_mode | SQLITE_OPEN_NOMUTEX, nullptr);
    if (result != SQLITE_OK) {
      const char* message = database == nullptr ? nullptr : sqlite3_errmsg(database);
      if (database != nullptr) {
        static_cast<void>(sqlite3_close_v2(database));
      }
      throw BenchmarkMismatch{message == nullptr ? "SQLite open failed" : message};
    }
    SqliteEngine engine{database};
    ConfigureSqlite(database, definition);
    if (sqlite3_db_readonly(database, "main") != (definition.query_only ? 1 : 0) ||
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
    if (IsOrderWorkload(definition.kind)) {
      return RunSqliteOrder(database_, statement.get(), definition, iterations);
    }
    if (IsIndexWorkload(definition.kind)) {
      return RunSqliteIndex(database_, statement.get(), definition, iterations, permutation);
    }
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
    if (definition.indexed) {
      return VerifySqliteIndex(database_, definition);
    }
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

[[nodiscard]] sqlite3_destructor_type SqliteTransient() noexcept {
  // SQLite defines SQLITE_TRANSIENT as the function-pointer sentinel -1.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));
}

[[nodiscard]] modern_sqlite::ByteBuffer IndexPayload(std::uint64_t logical_row) {
  modern_sqlite::ByteBuffer value{modern_sqlite::ByteCount{kIndexValueSize}};
  std::ranges::fill(value.mutable_view(), std::byte{'0'});
  const auto prefix = ExpectedPrefix(logical_row);
  std::ranges::copy(prefix, value.mutable_view().begin());
  return value;
}

[[nodiscard]] modern_sqlite::WriteStatement PrepareModernWrite(modern_sqlite::WriteSession& session,
                                                               std::string_view sql) {
  modern_sqlite::WritePrepareOutput prepared =
      TakeResult(session.Prepare(modern_sqlite::Utf8View{sql}), "Modern SQLite write prepare");
  if (!prepared.statement.has_value() || prepared.next_offset.value() != sql.size()) {
    throw BenchmarkMismatch{"Modern SQLite write prepare returned an incomplete statement"};
  }
  return std::move(*prepared.statement);
}

void StepModernCommand(modern_sqlite::WriteStatement& statement, std::string_view operation) {
  if (TakeResult(statement.Step(), operation) != modern_sqlite::WriteStep::kDone) {
    throw BenchmarkMismatch{std::string{operation} + " returned a row"};
  }
}

void FinalizeModernWrite(modern_sqlite::WriteStatement& statement) {
  RequireStatus(statement.Finalize(), "Modern SQLite write finalize");
}

[[nodiscard]] std::uint64_t StatefulRowId(const WorkloadDefinition& definition,
                                          std::uint64_t iteration,
                                          std::span<const std::uint64_t> permutation) {
  if (definition.kind == WorkloadKind::kIndexInsert) {
    return definition.row_count + iteration + 1U;
  }
  if (definition.kind == WorkloadKind::kIndexUpdate ||
      definition.kind == WorkloadKind::kIndexDelete) {
    return permutation[static_cast<std::size_t>(iteration % definition.row_count)];
  }
  return iteration + 1U;
}

[[nodiscard]] std::string StatefulCategory(WorkloadKind kind, std::uint64_t rowid) {
  return IndexedText(kind == WorkloadKind::kIndexInsert ? "insert-" : "update-", rowid);
}

[[nodiscard]] std::string StatefulSql(const WorkloadDefinition& definition,
                                      std::uint64_t iteration) {
  if (definition.kind == WorkloadKind::kIndexCreate) {
    return "CREATE INDEX benchmark_payload_" + std::to_string(iteration) + " ON items(payload)";
  }
  return std::string{definition.sql};
}

void AccumulateStatefulWork(const WorkloadDefinition& definition, std::uint64_t iteration,
                            std::span<const std::uint64_t> permutation, WorkResult& result,
                            Digest& digest) {
  const std::uint64_t rowid = StatefulRowId(definition, iteration, permutation);
  digest.AddTag('W');
  digest.AddByte(static_cast<std::uint8_t>(definition.kind));
  digest.AddKey(iteration);
  digest.AddKey(rowid);
  switch (definition.kind) {
    case WorkloadKind::kIndexInsert: {
      const std::string category = StatefulCategory(definition.kind, rowid);
      const std::uint64_t score = rowid % 256U;
      const std::uint64_t flag = rowid % 2U;
      AddText(digest, category);
      digest.AddKey(score);
      digest.AddKey(flag);
      const modern_sqlite::ByteBuffer payload = IndexPayload(rowid);
      for (const std::byte byte : payload.view()) {
        digest.AddByte(std::to_integer<std::uint8_t>(byte));
      }
      result.bytes += 3U * kIntegerResultBytes + category.size() + payload.view().size();
      ++result.rows;
      break;
    }
    case WorkloadKind::kIndexUpdate: {
      const std::string category = StatefulCategory(definition.kind, rowid);
      const std::uint64_t score = (rowid + 1U) % 256U;
      AddText(digest, category);
      digest.AddKey(score);
      result.bytes += 2U * kIntegerResultBytes + category.size();
      ++result.rows;
      break;
    }
    case WorkloadKind::kIndexDelete:
      result.bytes += kIntegerResultBytes;
      ++result.rows;
      break;
    case WorkloadKind::kIndexCreate:
    case WorkloadKind::kIndexAnalyze:
      AddText(digest, StatefulSql(definition, iteration));
      break;
    case WorkloadKind::kPointPresent:
    case WorkloadKind::kPointMissing:
    case WorkloadKind::kScan:
    case WorkloadKind::kOrder:
    case WorkloadKind::kIndexEqualityCoveringHit:
    case WorkloadKind::kIndexEqualityCoveringMiss:
    case WorkloadKind::kIndexEqualityNoncoveringHit:
    case WorkloadKind::kIndexMultiEqualityCovering:
    case WorkloadKind::kIndexRangeCovering:
    case WorkloadKind::kIndexRangeNoncovering:
    case WorkloadKind::kIndexUnselectiveNoncovering:
    case WorkloadKind::kIndexUnselectiveCovering:
    case WorkloadKind::kIndexRangeLowerOnlyCovering:
    case WorkloadKind::kIndexRangeUpperOnlyCovering:
      throw BenchmarkMismatch{"read workload reached stateful work accounting"};
  }
  ++result.operations;
  result.items += definition.items_per_iteration;
  ++result.result_hits;
}

[[nodiscard]] WorkResult RunModernStateful(modern_sqlite::WriteSession& session,
                                           const WorkloadDefinition& definition,
                                           std::uint64_t iterations,
                                           std::span<const std::uint64_t> permutation) {
  WorkResult result;
  Digest digest;
  if (definition.kind == WorkloadKind::kIndexCreate) {
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
      modern_sqlite::WriteStatement statement =
          PrepareModernWrite(session, StatefulSql(definition, iteration));
      StepModernCommand(statement, "Modern SQLite CREATE INDEX step");
      if (session.changes() != 0U) {
        throw BenchmarkMismatch{"Modern SQLite CREATE INDEX change count differs"};
      }
      RequireStatus(statement.Reset(), "Modern SQLite CREATE INDEX reset");
      FinalizeModernWrite(statement);
      AccumulateStatefulWork(definition, iteration, permutation, result, digest);
    }
    result.digest = digest.value();
    return result;
  }

  modern_sqlite::WriteStatement statement = PrepareModernWrite(session, definition.sql);
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const std::uint64_t rowid = StatefulRowId(definition, iteration, permutation);
    if (definition.kind == WorkloadKind::kIndexInsert) {
      const std::string category = StatefulCategory(definition.kind, rowid);
      const modern_sqlite::ByteBuffer payload = IndexPayload(rowid);
      RequireStatus(
          statement.Bind(1, modern_sqlite::SqlValue::Integer(static_cast<std::int64_t>(rowid))),
          "Modern SQLite insert rowid bind");
      RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Text(category)),
                    "Modern SQLite insert category bind");
      RequireStatus(statement.Bind(3, modern_sqlite::SqlValue::Integer(
                                          static_cast<std::int64_t>(rowid % 256U))),
                    "Modern SQLite insert score bind");
      RequireStatus(statement.Bind(
                        4, modern_sqlite::SqlValue::Integer(static_cast<std::int64_t>(rowid % 2U))),
                    "Modern SQLite insert flag bind");
      RequireStatus(statement.Bind(5, modern_sqlite::SqlValue::Blob(payload.Clone())),
                    "Modern SQLite insert payload bind");
    } else if (definition.kind == WorkloadKind::kIndexUpdate) {
      const std::string category = StatefulCategory(definition.kind, rowid);
      RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Text(category)),
                    "Modern SQLite update category bind");
      RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Integer(
                                          static_cast<std::int64_t>((rowid + 1U) % 256U))),
                    "Modern SQLite update score bind");
      RequireStatus(
          statement.Bind(3, modern_sqlite::SqlValue::Integer(static_cast<std::int64_t>(rowid))),
          "Modern SQLite update rowid bind");
    } else if (definition.kind == WorkloadKind::kIndexDelete) {
      RequireStatus(
          statement.Bind(1, modern_sqlite::SqlValue::Integer(static_cast<std::int64_t>(rowid))),
          "Modern SQLite delete rowid bind");
    }
    StepModernCommand(statement, "Modern SQLite stateful step");
    const std::uint64_t expected_changes = definition.kind == WorkloadKind::kIndexInsert ||
                                                   definition.kind == WorkloadKind::kIndexUpdate ||
                                                   definition.kind == WorkloadKind::kIndexDelete
                                               ? 1U
                                               : 0U;
    if (session.changes() != expected_changes) {
      throw BenchmarkMismatch{"Modern SQLite stateful change count differs"};
    }
    RequireStatus(statement.Reset(), "Modern SQLite stateful reset");
    AccumulateStatefulWork(definition, iteration, permutation, result, digest);
  }
  FinalizeModernWrite(statement);
  result.digest = digest.value();
  return result;
}

void BindSqliteInteger(sqlite3* database, sqlite3_stmt* statement, int parameter,
                       std::uint64_t value, std::string_view operation) {
  CheckSqlite(sqlite3_bind_int64(statement, parameter, static_cast<sqlite3_int64>(value)), database,
              operation);
}

void BindSqliteText(sqlite3* database, sqlite3_stmt* statement, int parameter,
                    const std::string& value, std::string_view operation) {
  CheckSqlite(sqlite3_bind_text(statement, parameter, value.data(), static_cast<int>(value.size()),
                                SqliteTransient()),
              database, operation);
}

struct StatefulSqliteStatementCounters {
  std::uint64_t vm_steps = 0;
  std::uint64_t fullscan_steps = 0;
  std::uint64_t statement_runs = 0;
  std::uint64_t reprepares = 0;
};

void AccumulateStatefulSqliteStatement(sqlite3_stmt* statement,
                                       StatefulSqliteStatementCounters& counters) {
  const auto read = [statement](int operation) {
    const int value = sqlite3_stmt_status(statement, operation, 0);
    if (value < 0) {
      throw BenchmarkMismatch{"SQLite stateful statement status failed"};
    }
    return static_cast<std::uint64_t>(value);
  };
  counters.vm_steps += read(SQLITE_STMTSTATUS_VM_STEP);
  counters.fullscan_steps += read(SQLITE_STMTSTATUS_FULLSCAN_STEP);
  counters.statement_runs += read(SQLITE_STMTSTATUS_RUN);
  counters.reprepares += read(SQLITE_STMTSTATUS_REPREPARE);
}

[[nodiscard]] WorkResult RunSqliteStateful(sqlite3* database, const WorkloadDefinition& definition,
                                           std::uint64_t iterations,
                                           std::span<const std::uint64_t> permutation,
                                           StatefulSqliteStatementCounters* measured_counters) {
  WorkResult result;
  Digest digest;
  if (definition.kind == WorkloadKind::kIndexCreate) {
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
      const std::string sql = StatefulSql(definition, iteration);
      sqlite3_stmt* raw_statement = nullptr;
      CheckSqlite(sqlite3_prepare_v3(database, sql.data(), static_cast<int>(sql.size()),
                                     SQLITE_PREPARE_PERSISTENT, &raw_statement, nullptr),
                  database, "SQLite CREATE INDEX prepare");
      SqliteStatement statement{raw_statement};
      if (sqlite3_step(statement.get()) != SQLITE_DONE || sqlite3_changes64(database) != 0 ||
          sqlite3_reset(statement.get()) != SQLITE_OK) {
        throw BenchmarkMismatch{"SQLite CREATE INDEX completion differs"};
      }
      if (measured_counters != nullptr) {
        AccumulateStatefulSqliteStatement(statement.get(), *measured_counters);
      }
      AccumulateStatefulWork(definition, iteration, permutation, result, digest);
    }
    result.digest = digest.value();
    return result;
  }

  sqlite3_stmt* raw_statement = nullptr;
  CheckSqlite(
      sqlite3_prepare_v3(database, definition.sql.data(), static_cast<int>(definition.sql.size()),
                         SQLITE_PREPARE_PERSISTENT, &raw_statement, nullptr),
      database, "SQLite stateful prepare");
  SqliteStatement statement{raw_statement};
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const std::uint64_t rowid = StatefulRowId(definition, iteration, permutation);
    if (definition.kind == WorkloadKind::kIndexInsert) {
      const std::string category = StatefulCategory(definition.kind, rowid);
      const modern_sqlite::ByteBuffer payload = IndexPayload(rowid);
      BindSqliteInteger(database, statement.get(), 1, rowid, "SQLite insert rowid bind");
      BindSqliteText(database, statement.get(), 2, category, "SQLite insert category bind");
      BindSqliteInteger(database, statement.get(), 3, rowid % 256U, "SQLite insert score bind");
      BindSqliteInteger(database, statement.get(), 4, rowid % 2U, "SQLite insert flag bind");
      CheckSqlite(sqlite3_bind_blob(statement.get(), 5, payload.view().data(),
                                    static_cast<int>(payload.view().size()), SqliteTransient()),
                  database, "SQLite insert payload bind");
    } else if (definition.kind == WorkloadKind::kIndexUpdate) {
      const std::string category = StatefulCategory(definition.kind, rowid);
      BindSqliteText(database, statement.get(), 1, category, "SQLite update category bind");
      BindSqliteInteger(database, statement.get(), 2, (rowid + 1U) % 256U,
                        "SQLite update score bind");
      BindSqliteInteger(database, statement.get(), 3, rowid, "SQLite update rowid bind");
    } else if (definition.kind == WorkloadKind::kIndexDelete) {
      BindSqliteInteger(database, statement.get(), 1, rowid, "SQLite delete rowid bind");
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
      CheckSqlite(sqlite3_errcode(database), database, "SQLite stateful step");
      throw BenchmarkMismatch{"SQLite stateful statement did not finish"};
    }
    const sqlite3_int64 expected_changes = definition.kind == WorkloadKind::kIndexInsert ||
                                                   definition.kind == WorkloadKind::kIndexUpdate ||
                                                   definition.kind == WorkloadKind::kIndexDelete
                                               ? 1
                                               : 0;
    if (sqlite3_changes64(database) != expected_changes ||
        sqlite3_reset(statement.get()) != SQLITE_OK ||
        sqlite3_clear_bindings(statement.get()) != SQLITE_OK) {
      throw BenchmarkMismatch{"SQLite stateful completion differs"};
    }
    AccumulateStatefulWork(definition, iteration, permutation, result, digest);
  }
  if (measured_counters != nullptr) {
    AccumulateStatefulSqliteStatement(statement.get(), *measured_counters);
  }
  result.digest = digest.value();
  return result;
}

class StatefulDatabaseCopy final {
 public:
  StatefulDatabaseCopy(const std::filesystem::path& source, std::size_t sequence)
      : path_{source.string() + ".stateful-" + std::to_string(sequence)} {
    if (std::filesystem::exists(path_)) {
      throw HarnessFailure{"stateful benchmark work database already exists"};
    }
    std::error_code error;
    if (!std::filesystem::copy_file(source, path_, std::filesystem::copy_options::none, error) ||
        error) {
      throw HarnessFailure{"cannot copy stateful benchmark database"};
    }
  }

  StatefulDatabaseCopy(const StatefulDatabaseCopy&) = delete;
  StatefulDatabaseCopy& operator=(const StatefulDatabaseCopy&) = delete;

  ~StatefulDatabaseCopy() noexcept { RemoveOwnedFiles(); }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  void Cleanup() {
    for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
      if (std::filesystem::exists(path_.string() + std::string{suffix})) {
        throw BenchmarkMismatch{"stateful benchmark left a sidecar file"};
      }
    }
    std::error_code error;
    if (!std::filesystem::remove(path_, error) || error) {
      throw HarnessFailure{"cannot remove stateful benchmark work database"};
    }
    cleaned_ = true;
  }

 private:
  void RemoveOwnedFiles() noexcept {
    if (cleaned_) {
      return;
    }
    std::error_code error;
    static_cast<void>(std::filesystem::remove(path_, error));
    for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
      error.clear();
      static_cast<void>(std::filesystem::remove(path_.string() + std::string{suffix}, error));
    }
  }

  std::filesystem::path path_;
  bool cleaned_ = false;
};

void VerifyStatefulOutput(const std::filesystem::path& path, const WorkloadDefinition& definition,
                          std::uint64_t iterations, std::span<const std::uint64_t> permutation) {
  sqlite3* raw_database = nullptr;
  const int opened = sqlite3_open_v2(path.string().c_str(), &raw_database,
                                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
  if (opened != SQLITE_OK) {
    if (raw_database != nullptr) {
      static_cast<void>(sqlite3_close_v2(raw_database));
    }
    throw BenchmarkMismatch{"SQLite could not verify stateful output"};
  }
  struct DatabaseCloser {
    void operator()(sqlite3* database) const noexcept {
      static_cast<void>(sqlite3_close_v2(database));
    }
  };
  const std::unique_ptr<sqlite3, DatabaseCloser> database{raw_database};
  if (SqliteSingleText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw BenchmarkMismatch{"stateful output failed SQLite integrity_check"};
  }
  const std::int64_t expected_rows =
      static_cast<std::int64_t>(definition.row_count) +
      (definition.kind == WorkloadKind::kIndexInsert   ? static_cast<std::int64_t>(iterations)
       : definition.kind == WorkloadKind::kIndexDelete ? -static_cast<std::int64_t>(iterations)
                                                       : 0);
  if (SqliteSingleInteger(database.get(), "SELECT count(*) FROM items") != expected_rows) {
    throw BenchmarkMismatch{"stateful output row count differs"};
  }

  if (definition.kind == WorkloadKind::kIndexInsert ||
      definition.kind == WorkloadKind::kIndexUpdate ||
      definition.kind == WorkloadKind::kIndexDelete) {
    const std::string_view sql = definition.kind == WorkloadKind::kIndexDelete
                                     ? "SELECT id FROM items INDEXED BY items_category_score "
                                       "WHERE id=?1"
                                 : definition.kind == WorkloadKind::kIndexInsert
                                     ? "SELECT category,score,flag,payload FROM items "
                                       "INDEXED BY items_category_score WHERE id=?1"
                                     : "SELECT category,score FROM items "
                                       "INDEXED BY items_category_score WHERE id=?1";
    sqlite3_stmt* raw_statement = nullptr;
    CheckSqlite(sqlite3_prepare_v3(database.get(), sql.data(), static_cast<int>(sql.size()), 0,
                                   &raw_statement, nullptr),
                database.get(), "SQLite stateful verification prepare");
    SqliteStatement statement{raw_statement};
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
      const std::uint64_t rowid = StatefulRowId(definition, iteration, permutation);
      BindSqliteInteger(database.get(), statement.get(), 1, rowid,
                        "SQLite stateful verification bind");
      const int step = sqlite3_step(statement.get());
      if (definition.kind == WorkloadKind::kIndexDelete) {
        if (step != SQLITE_DONE) {
          throw BenchmarkMismatch{"deleted indexed row remains visible"};
        }
      } else {
        const std::string expected_category = StatefulCategory(definition.kind, rowid);
        const unsigned char* category = sqlite3_column_text(statement.get(), 0);
        const int category_size = sqlite3_column_bytes(statement.get(), 0);
        const std::uint64_t expected_score =
            definition.kind == WorkloadKind::kIndexInsert ? rowid % 256U : (rowid + 1U) % 256U;
        if (step != SQLITE_ROW || category == nullptr ||
            std::string_view{reinterpret_cast<const char*>(category),
                             static_cast<std::size_t>(category_size)} != expected_category ||
            sqlite3_column_int64(statement.get(), 1) !=
                static_cast<sqlite3_int64>(expected_score)) {
          throw BenchmarkMismatch{"indexed mutation output differs"};
        }
        if (definition.kind == WorkloadKind::kIndexInsert) {
          const void* payload = sqlite3_column_blob(statement.get(), 3);
          const int payload_size = sqlite3_column_bytes(statement.get(), 3);
          const modern_sqlite::ByteBuffer expected_payload = IndexPayload(rowid);
          if (sqlite3_column_int64(statement.get(), 2) != static_cast<sqlite3_int64>(rowid % 2U) ||
              payload == nullptr ||
              payload_size != static_cast<int>(expected_payload.view().size()) ||
              !std::ranges::equal(modern_sqlite::ByteView{static_cast<const std::byte*>(payload),
                                                          static_cast<std::size_t>(payload_size)},
                                  expected_payload.view())) {
            throw BenchmarkMismatch{"indexed insert payload differs"};
          }
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
          throw BenchmarkMismatch{"indexed mutation output returned duplicate rows"};
        }
      }
      CheckSqlite(sqlite3_reset(statement.get()), database.get(),
                  "SQLite stateful verification reset");
      CheckSqlite(sqlite3_clear_bindings(statement.get()), database.get(),
                  "SQLite stateful verification clear");
    }
  } else if (definition.kind == WorkloadKind::kIndexCreate) {
    if (SqliteSingleInteger(database.get(),
                            "SELECT count(*) FROM sqlite_schema WHERE type='index' "
                            "AND name LIKE 'benchmark_payload_%'") !=
            static_cast<std::int64_t>(iterations) ||
        SqliteSingleInteger(
            database.get(),
            "SELECT count(*) FROM items INDEXED BY benchmark_payload_0 WHERE payload>=x''") !=
            static_cast<std::int64_t>(definition.row_count)) {
      throw BenchmarkMismatch{"CREATE INDEX output differs"};
    }
  } else if (definition.kind == WorkloadKind::kIndexAnalyze) {
    const std::string statistics =
        SqliteSingleText(database.get(),
                         "SELECT group_concat(idx||':'||stat,'|') FROM "
                         "(SELECT idx,stat FROM sqlite_stat1 WHERE tbl='items' ORDER BY idx)");
    if (statistics !=
        "items_category_score:4096 1 1|items_flag:4096 2048|"
        "items_score_desc:4096 16") {
      throw BenchmarkMismatch{"ANALYZE output differs"};
    }
  }
}

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

void VerifySourceWithEngine(EngineKind engine, const std::filesystem::path& path,
                            const WorkloadDefinition& definition) {
  WorkResult actual;
  if (engine == EngineKind::kModern) {
    ModernEngine modern = ModernEngine::Open(path, definition);
    actual = modern.Verify(definition);
  } else {
    SqliteEngine sqlite = SqliteEngine::Open(path, definition);
    actual = sqlite.Verify(definition);
  }
  RequireWork("stateful source verification", actual, definition.verification);
}

[[nodiscard]] WorkResult RunStatefulCopy(
    EngineKind engine, const std::filesystem::path& source, const WorkloadDefinition& definition,
    std::uint64_t iterations, std::span<const std::uint64_t> permutation, std::size_t sequence,
    std::optional<std::size_t> measured_index, std::optional<Repetition>& measured) {
  StatefulDatabaseCopy copy{source, sequence};
  WorkResult work;
  if (engine == EngineKind::kModern) {
    {
      modern_sqlite::WriteSession session = TakeResult(
          modern_sqlite::WriteSession::Open(copy.path().string()), "Modern SQLite write open");
      if (measured_index.has_value()) {
        measured = Measure(*measured_index, [&] {
          return RunModernStateful(session, definition, iterations, permutation);
        });
        work = measured->work;
      } else {
        work = RunModernStateful(session, definition, iterations, permutation);
      }
    }
  } else {
    {
      sqlite3* raw_database = nullptr;
      const int opened = sqlite3_open_v2(copy.path().string().c_str(), &raw_database,
                                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
      if (opened != SQLITE_OK) {
        if (raw_database != nullptr) {
          static_cast<void>(sqlite3_close_v2(raw_database));
        }
        throw BenchmarkMismatch{"SQLite stateful open failed"};
      }
      struct DatabaseCloser {
        void operator()(sqlite3* database) const noexcept {
          static_cast<void>(sqlite3_close_v2(database));
        }
      };
      const std::unique_ptr<sqlite3, DatabaseCloser> database{raw_database};
      ConfigureSqlite(database.get(), definition);
      if (measured_index.has_value()) {
        measured = Measure(*measured_index, [&] {
          return RunSqliteStateful(database.get(), definition, iterations, permutation, nullptr);
        });
        work = measured->work;
      } else {
        work = RunSqliteStateful(database.get(), definition, iterations, permutation, nullptr);
      }
    }
  }
  VerifyStatefulOutput(copy.path(), definition, iterations, permutation);
  copy.Cleanup();
  return work;
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

[[nodiscard]] std::uint64_t StatefulPreparedStatements(const WorkloadDefinition& definition,
                                                       std::uint64_t iterations) {
  return definition.kind == WorkloadKind::kIndexCreate ? iterations : 1U;
}

[[maybe_unused, nodiscard]] TimingRun RunStatefulTiming(
    EngineKind engine, const std::filesystem::path& path, const WorkloadDefinition& definition,
    bool smoke, std::span<const std::uint64_t> permutation) {
  VerifySourceWithEngine(engine, path, definition);
  std::optional<Repetition> unused;
  const WorkResult warmup = RunStatefulCopy(engine, path, definition, definition.warmup_iterations,
                                            permutation, 0U, std::nullopt, unused);
  RequireWork("stateful warmup", warmup, definition.warmup);

  const std::size_t repetition_count = smoke ? 1U : kTimingRepetitions;
  const std::uint64_t iterations = smoke ? 1U : definition.measured_iterations;
  std::vector<Repetition> repetitions;
  repetitions.reserve(repetition_count);
  for (std::size_t index = 0; index < repetition_count; ++index) {
    std::optional<Repetition> measured;
    const WorkResult work = RunStatefulCopy(engine, path, definition, iterations, permutation,
                                            index + 1U, index, measured);
    if (!measured.has_value() || measured->work != work) {
      throw BenchmarkMismatch{"stateful timing did not produce one measured repetition"};
    }
    if (!smoke) {
      RequireWork("stateful measured repetition", measured->work, definition.measured);
      if (measured->wall_ns < definition.minimum_wall_ns) {
        throw HarnessFailure{"stateful baseline repetition did not reach the minimum wall time"};
      }
    }
    repetitions.push_back(*measured);
  }
  VerifySourceWithEngine(engine, path, definition);

  const std::uint64_t execution_count = 1U + repetition_count;
  const std::uint64_t prepared_statements =
      StatefulPreparedStatements(definition, definition.warmup_iterations) +
      StatefulPreparedStatements(definition, iterations) * repetition_count + 2U;
  return TimingRun{
      .warmup = warmup,
      .repetitions = std::move(repetitions),
      .completion =
          Completion{
              .session_opens = execution_count + 2U,
              .statement_prepares = prepared_statements,
              .statement_finalizes = prepared_statements,
              .statement_resets = definition.warmup_iterations + iterations * repetition_count + 2U,
              .pre_verifications = 1,
              .post_verifications = execution_count + 1U,
          },
  };
}

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
      if (repetition.wall_ns < definition.minimum_wall_ns) {
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
#error "The order-by performance benchmark requires a recognized compiler"
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
#error "The order-by performance benchmark requires a recognized standard library"
#endif
  output << '}';
}

[[nodiscard]] constexpr std::string_view TargetArchitecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
#error "The order-by performance benchmark requires a recognized target architecture"
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
            << (MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS ? "true" : "false")
            << ",\"sanitizers\":false,\"standard_library\":";
  PrintStandardLibraryIdentity(std::cout);
  std::cout << "},\"mode\":\"identity\",\"schema_version\":1,\"source\":{\"revision\":";
  PrintJsonString(std::cout, modern_sqlite::order_by_performance_build_config::kGitRevision);
  std::cout << ",\"tree\":";
  PrintJsonString(std::cout, modern_sqlite::order_by_performance_build_config::kGitTree);
  std::cout << "},\"sqlite\":";
  PrintSqliteIdentity(std::cout);
  std::cout << "}\n";
}

void PrintConfiguration(std::ostream& output, const WorkloadDefinition& definition) {
  if (IsOrderWorkload(definition.kind)) {
    output << "{\"cache_pages\":512,\"journal_mode\":\"delete\",\"merge_fan_in\":16,"
              "\"mmap_bytes\":0,\"page_size\":4096,\"query_only\":true,"
              "\"sorter_memory_threshold\":"
           << definition.sorter_memory_threshold << ",\"synchronous\":\"full\",\"temp_store\":\""
           << (definition.temporary_store_memory ? "memory" : "file")
           << "\",\"thread_mode\":\"single\"}";
    return;
  }
  output << "{\"cache_pages\":512,\"journal_mode\":\"delete\",\"mmap_bytes\":0,"
            "\"page_size\":4096,\"query_only\":"
         << (definition.query_only ? "true" : "false")
         << ",\"synchronous\":\"full\","
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
  PrintConfiguration(std::cout, definition);
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
  std::uint64_t sort_operations = 0;
  std::uint64_t temp_bytes_spilled = 0;
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

#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
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
  ModernEngine engine = ModernEngine::Open(path, definition);
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
  if (!IsOrderWorkload(definition.kind) &&
      (pages_written != 0 || (!definition.pressure && cache_misses != 0) ||
       (definition.pressure && cache_misses == 0))) {
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

[[nodiscard]] Completion StatefulDiagnosticCompletion(const WorkloadDefinition& definition) {
  constexpr std::uint64_t kExecutionCount = 2U;
  const std::uint64_t prepared_statements =
      StatefulPreparedStatements(definition, definition.warmup_iterations) +
      StatefulPreparedStatements(definition, definition.diagnostic_iterations) + 2U;
  return Completion{
      .session_opens = kExecutionCount + 2U,
      .statement_prepares = prepared_statements,
      .statement_finalizes = prepared_statements,
      .statement_resets = definition.warmup_iterations + definition.diagnostic_iterations + 2U,
      .pre_verifications = 1,
      .post_verifications = kExecutionCount + 1U,
  };
}

[[nodiscard]] DiagnosticRun RunModernStatefulDiagnostic(
    const std::filesystem::path& path, const WorkloadDefinition& definition,
    std::span<const std::uint64_t> permutation) {
  VerifySourceWithEngine(EngineKind::kModern, path, definition);
  std::optional<Repetition> unused;
  const WorkResult warmup =
      RunStatefulCopy(EngineKind::kModern, path, definition, definition.warmup_iterations,
                      permutation, 0U, std::nullopt, unused);
  RequireWork("stateful diagnostic warmup", warmup, definition.warmup);

  StatefulDatabaseCopy copy{path, 1U};
  modern_sqlite::instrumentation::CounterCollection counters;
  WorkResult diagnostic;
  {
    modern_sqlite::WriteSession session = TakeResult(
        modern_sqlite::WriteSession::Open(copy.path().string()), "Modern SQLite write open");
    const modern_sqlite::instrumentation::ScopedCounterCollection scope{counters};
    diagnostic =
        RunModernStateful(session, definition, definition.diagnostic_iterations, permutation);
  }
  RequireWork("stateful diagnostic", diagnostic, definition.diagnostic);
  VerifyStatefulOutput(copy.path(), definition, definition.diagnostic_iterations, permutation);
  copy.Cleanup();
  VerifySourceWithEngine(EngineKind::kModern, path, definition);

  ModernCounterValues values = ReadModernCounters(counters);
  const auto pages_written =
      values
          .values[static_cast<std::size_t>(modern_sqlite::instrumentation::Counter::kPagesWritten)];
  if (pages_written == 0U) {
    throw BenchmarkMismatch{"Modern stateful diagnostic recorded no page writes"};
  }
  return DiagnosticRun{
      .work = diagnostic,
      .completion = StatefulDiagnosticCompletion(definition),
      .modern = values,
      .sqlite = std::nullopt,
  };
}

[[nodiscard]] DiagnosticRun RunSqliteStatefulDiagnostic(
    const std::filesystem::path& path, const WorkloadDefinition& definition,
    std::span<const std::uint64_t> permutation) {
  VerifySourceWithEngine(EngineKind::kSqlite, path, definition);
  std::optional<Repetition> unused;
  const WorkResult warmup =
      RunStatefulCopy(EngineKind::kSqlite, path, definition, definition.warmup_iterations,
                      permutation, 0U, std::nullopt, unused);
  RequireWork("stateful diagnostic warmup", warmup, definition.warmup);

  StatefulDatabaseCopy copy{path, 1U};
  sqlite3* raw_database = nullptr;
  const int opened = sqlite3_open_v2(copy.path().string().c_str(), &raw_database,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
  if (opened != SQLITE_OK) {
    if (raw_database != nullptr) {
      static_cast<void>(sqlite3_close_v2(raw_database));
    }
    throw BenchmarkMismatch{"SQLite stateful diagnostic open failed"};
  }
  struct DatabaseCloser {
    void operator()(sqlite3* database) const noexcept {
      static_cast<void>(sqlite3_close_v2(database));
    }
  };
  WorkResult diagnostic;
  SqliteCounterValues counters;
  {
    const std::unique_ptr<sqlite3, DatabaseCloser> database{raw_database};
    ConfigureSqlite(database.get(), definition);
    static_cast<void>(SqliteDbStatus(database.get(), SQLITE_DBSTATUS_CACHE_HIT, true));
    static_cast<void>(SqliteDbStatus(database.get(), SQLITE_DBSTATUS_CACHE_MISS, true));
    static_cast<void>(SqliteDbStatus(database.get(), SQLITE_DBSTATUS_CACHE_WRITE, true));
    static_cast<void>(SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, true));
    static_cast<void>(SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, true));

    StatefulSqliteStatementCounters statement_counters;
    diagnostic = RunSqliteStateful(database.get(), definition, definition.diagnostic_iterations,
                                   permutation, &statement_counters);
    counters.vm_steps = statement_counters.vm_steps;
    counters.fullscan_steps = statement_counters.fullscan_steps;
    counters.statement_runs = statement_counters.statement_runs;
    counters.reprepares = statement_counters.reprepares;
    counters.cache_hits = SqliteDbStatus(database.get(), SQLITE_DBSTATUS_CACHE_HIT, false).first;
    counters.cache_misses = SqliteDbStatus(database.get(), SQLITE_DBSTATUS_CACHE_MISS, false).first;
    counters.cache_writes =
        SqliteDbStatus(database.get(), SQLITE_DBSTATUS_CACHE_WRITE, false).first;
    counters.cache_bytes_current =
        SqliteDbStatus(database.get(), SQLITE_DBSTATUS_CACHE_USED, false).first;
    const auto malloc_count = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, false);
    counters.malloc_count_current = malloc_count.first;
    counters.malloc_count_highwater = malloc_count.second;
    counters.malloc_size_highwater = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, false).second;
  }
  RequireWork("stateful diagnostic", diagnostic, definition.diagnostic);
  VerifyStatefulOutput(copy.path(), definition, definition.diagnostic_iterations, permutation);
  copy.Cleanup();
  VerifySourceWithEngine(EngineKind::kSqlite, path, definition);
  if (counters.cache_writes == 0U) {
    throw BenchmarkMismatch{"SQLite stateful diagnostic recorded no cache writes"};
  }
  return DiagnosticRun{
      .work = diagnostic,
      .completion = StatefulDiagnosticCompletion(definition),
      .modern = std::nullopt,
      .sqlite = counters,
  };
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
  static_cast<void>(SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_TEMPBUF_SPILL, true));
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
  counters.sort_operations =
      static_cast<std::uint64_t>(sqlite3_stmt_status(statement.get(), SQLITE_STMTSTATUS_SORT, 0));
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
  counters.temp_bytes_spilled =
      SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_TEMPBUF_SPILL, false).first;
  counters.cache_bytes_current =
      SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_USED, false).first;
  const auto malloc_count = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, false);
  counters.malloc_count_current = malloc_count.first;
  counters.malloc_count_highwater = malloc_count.second;
  counters.malloc_size_highwater = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, false).second;

  RequireWork("post-verification", engine.Verify(definition), definition.verification);
  if (!IsOrderWorkload(definition.kind) &&
      (counters.cache_writes != 0 || (!definition.pressure && counters.cache_misses != 0) ||
       (definition.pressure && counters.cache_misses == 0))) {
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
         << ",\"sort_operations\":" << counters.sort_operations
         << ",\"malloc_count_current\":" << counters.malloc_count_current
         << ",\"malloc_count_highwater\":" << counters.malloc_count_highwater
         << ",\"malloc_size_highwater\":" << counters.malloc_size_highwater
         << ",\"reprepares\":" << counters.reprepares
         << ",\"statement_runs\":" << counters.statement_runs
         << ",\"temp_bytes_spilled\":" << counters.temp_bytes_spilled
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
  PrintConfiguration(std::cout, definition);
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
      load32(28) != definition.page_count || load32(60) != definition.user_version ||
      load32(68) != definition.application_id) {
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
#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
    if (run_kind != "diagnostic") {
      throw HarnessFailure{"diagnostic binary accepts only diagnostic runs"};
    }
    DiagnosticRun run;
    if (IsStatefulIndexWorkload(definition.kind) && engine == EngineKind::kModern) {
      run = RunModernStatefulDiagnostic(database_path, definition, permutation);
    } else if (IsStatefulIndexWorkload(definition.kind)) {
      run = RunSqliteStatefulDiagnostic(database_path, definition, permutation);
    } else if (engine == EngineKind::kModern) {
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
    if (IsStatefulIndexWorkload(definition.kind)) {
      run = RunStatefulTiming(engine, database_path, definition, smoke, permutation);
    } else if (engine == EngineKind::kModern) {
      ModernEngine modern = ModernEngine::Open(database_path, definition);
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
#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
    throw HarnessFailure{"diagnostic binary does not provide profile replay"};
#else
    if (IsStatefulIndexWorkload(definition.kind)) {
      throw HarnessFailure{"stateful index workloads do not provide profile replay"};
    }
    const std::uint64_t iterations = ParseIterations(argv[5]);
    WorkResult work;
    Completion completion;
    if (engine == EngineKind::kModern) {
      ModernEngine modern = ModernEngine::Open(database_path, definition);
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

#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
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
    std::cerr << "order-by performance mismatch: " << error.what() << '\n';
    return 2;
  } catch (const HarnessFailure& error) {
    std::cerr << "order-by performance harness error: " << error.what() << '\n';
    return 1;
  } catch (const std::bad_alloc&) {
    std::cerr << "order-by performance harness error: out of memory\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "order-by performance harness error: " << error.what() << '\n';
    return 1;
  }
}
