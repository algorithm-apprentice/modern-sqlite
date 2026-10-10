#if defined(MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS) && \
    MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <limits>

#include "modern_sqlite/instrumentation/counters.hpp"

namespace {

std::array<std::uint64_t, modern_sqlite::instrumentation::kProbeTagCount> sqlite_probe_counts{};

void SqliteProbe(sqlite3_context* context, int argument_count, sqlite3_value** arguments) {
  if (argument_count != 2 || arguments == nullptr ||
      sqlite3_value_type(arguments[0]) != SQLITE_INTEGER) {
    sqlite3_result_error(context, "modern_sqlite_probe requires an integer tag and one value", -1);
    return;
  }
  const sqlite3_int64 tag = sqlite3_value_int64(arguments[0]);
  if (tag <= 0 ||
      static_cast<std::uint64_t>(tag) >= modern_sqlite::instrumentation::kProbeTagCount) {
    sqlite3_result_error(context, "modern_sqlite_probe tag is invalid", -1);
    return;
  }
  std::uint64_t& count = sqlite_probe_counts[static_cast<std::size_t>(tag)];
  if (count == std::numeric_limits<std::uint64_t>::max()) {
    sqlite3_result_error(context, "modern_sqlite_probe call count is exhausted", -1);
    return;
  }
  ++count;
  sqlite3_result_value(context, arguments[1]);
}

int DistinctCompoundSqliteOpen(const char* filename, sqlite3** database, int flags,
                               const char* vfs_name) {
  const int opened = sqlite3_open_v2(filename, database, flags, vfs_name);
  if (opened != SQLITE_OK || database == nullptr || *database == nullptr) {
    return opened;
  }
  return sqlite3_create_function_v2(*database, "modern_sqlite_probe", 2, SQLITE_UTF8, nullptr,
                                    SqliteProbe, nullptr, nullptr, nullptr);
}

}  // namespace

#define sqlite3_open_v2 DistinctCompoundSqliteOpen
#endif
#define kWorkloads kOrderByWorkloads
#define FindWorkload FindOrderByWorkload
#define RunCommand RunOrderByCommand
#define main order_by_benchmark_embedded_main
#include "order_by_performance.cpp"
#undef main
#undef RunCommand
#undef FindWorkload
#undef kWorkloads
#if defined(MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS) && \
    MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
#undef sqlite3_open_v2
#endif

namespace {

[[nodiscard]] std::string BuildLargeValuesUnionSql() {
  std::string sql = "VALUES";
  for (std::size_t value = 1; value <= 256U; ++value) {
    sql += value == 1U ? "(" : ",(";
    sql += std::to_string(value);
    sql += ')';
  }
  sql += " UNION ALL SELECT id+0 FROM items ORDER BY 1 LIMIT 64";
  return sql;
}

const std::string kLargeValuesUnionSql = BuildLargeValuesUnionSql();

[[nodiscard]] WorkloadDefinition ReadWorkload(std::string_view id, std::string_view sql,
                                              std::uint64_t items, std::uint64_t rows,
                                              std::uint64_t bytes, std::uint64_t digest,
                                              bool memory, std::size_t threshold,
                                              std::uint64_t measured_iterations = 1,
                                              std::uint64_t measured_digest = 0) {
  const WorkResult work = Work(1, items, rows, bytes, 1, 0, digest);
  const WorkResult measured =
      measured_iterations == 1
          ? work
          : Work(measured_iterations, measured_iterations * items, measured_iterations * rows,
                 measured_iterations * bytes, measured_iterations, 0, measured_digest);
  return WorkloadDefinition{
      .id = id,
      .kind = WorkloadKind::kOrder,
      .sql = sql,
      .indexed = true,
      .seed = kFitSeed,
      .row_count = 65'536,
      .page_count = 4'866,
      .application_id = kOrderApplicationId,
      .minimum_wall_ns = kIndexMinimumWallNanoseconds,
      .warmup_iterations = 1,
      .measured_iterations = measured_iterations,
      .diagnostic_iterations = 1,
      .items_per_iteration = items,
      .result_rows_per_iteration = rows,
      .payload_size = kOrderValueSize,
      .temporary_store_memory = memory,
      .sorter_memory_threshold = threshold,
      .warmup = work,
      .measured = measured,
      .diagnostic = work,
      .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
  };
}

const std::array<WorkloadDefinition, 14> kWorkloads{{
    ReadWorkload("distinct-low-card-memory", "SELECT DISTINCT flag FROM items", 65'536, 2, 16,
                 0xE1E57E17779545B9ULL, true, 64U << 20U, 2, 0xA5D1922ACDC6225DULL),
    ReadWorkload("distinct-high-card-memory", "SELECT DISTINCT category FROM items", 65'536, 65'536,
                 1'114'112, 0x1115D744F5A9F725ULL, true, 64U << 20U),
    ReadWorkload("distinct-collated-memory",
                 "SELECT DISTINCT iif(flag=0,'A','a') COLLATE NOCASE FROM items", 65'536, 1, 1,
                 0x200B1B815C9EA247ULL, true, 64U << 20U, 2, 0x2596634236BD4EC5ULL),
    ReadWorkload("distinct-order-limit-memory",
                 "SELECT DISTINCT score+0 FROM items ORDER BY 1 DESC LIMIT 64", 65'536, 64, 512,
                 0xB23D465428610B6EULL, true, 64U << 20U, 2, 0xB9171F9835740255ULL),
    ReadWorkload("values-union-all-limit-memory", kLargeValuesUnionSql, 65'792, 64, 512,
                 0xEBFB42850728072EULL, true, 64U << 20U, 4, 0xE9F68D7C90B0B405ULL),
    ReadWorkload("union-replace-file",
                 "SELECT score FROM items WHERE id<=32768 "
                 "UNION SELECT score+0.0 FROM items WHERE id>32768",
                 65'536, 4'096, 32'768, 0xF2F777673354D459ULL, false, 2U << 20U),
    ReadWorkload("except-membership-memory",
                 "SELECT score FROM items WHERE flag=0 "
                 "EXCEPT SELECT score FROM items WHERE id%4=0",
                 49'152, 1'024, 8'192, 0x7B9F56FA8BDA78EEULL, true, 64U << 20U),
    ReadWorkload("intersect-membership-memory",
                 "SELECT score FROM items WHERE flag=0 "
                 "INTERSECT SELECT score FROM items WHERE id%4=0",
                 49'152, 1'024, 8'192, 0x83509A56B1476DEEULL, true, 64U << 20U),
    ReadWorkload("ordered-union-all-topn-memory",
                 "SELECT id,payload FROM items WHERE flag=0 "
                 "UNION ALL SELECT id,payload FROM items WHERE flag=1 "
                 "UNION ALL SELECT id,payload FROM items WHERE id%4=0 "
                 "ORDER BY payload,id LIMIT 64 OFFSET 64",
                 81'920, 64, 16'896, 0x9A886719EEA2BE2FULL, true, 64U << 20U),
    ReadWorkload("ordered-union-merge-memory",
                 "SELECT score FROM items WHERE id<=32768 "
                 "UNION SELECT score+0.0 FROM items WHERE id>32768 ORDER BY 1",
                 65'536, 4'096, 32'768, 0xF2F777673354D459ULL, true, 64U << 20U),
    ReadWorkload("ordered-except-merge-memory",
                 "SELECT score FROM items WHERE flag=0 "
                 "EXCEPT SELECT score FROM items WHERE id%4=0 ORDER BY 1",
                 49'152, 1'024, 8'192, 0x7B9F56FA8BDA78EEULL, true, 64U << 20U),
    ReadWorkload("ordered-intersect-merge-memory",
                 "SELECT score FROM items WHERE flag=0 "
                 "INTERSECT SELECT score FROM items WHERE id%4=0 ORDER BY 1",
                 49'152, 1'024, 8'192, 0x83509A56B1476DEEULL, true, 64U << 20U),
    ReadWorkload("long-mixed-compound-memory",
                 "SELECT score FROM items WHERE id<=16384 "
                 "UNION ALL SELECT score FROM items WHERE id>16384 AND id<=32768 "
                 "UNION SELECT score FROM items WHERE id>32768 AND id<=49152 "
                 "EXCEPT SELECT score FROM items WHERE id%8=0 "
                 "INTERSECT SELECT score FROM items WHERE flag=1 ORDER BY 1",
                 90'112, 2'048, 16'384, 0xAA64FC0C7C66E5EEULL, true, 64U << 20U),
    ReadWorkload("union-keyed-relation-before-file",
                 "SELECT category,payload FROM items WHERE id<=32768 "
                 "UNION SELECT category,payload FROM items WHERE id>32768",
                 65'536, 65'536, 17'891'328, 0xCE76FDEE5EB31AC8ULL, false, 2U << 20U),
}};

#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
[[nodiscard]] std::string BuildLargeValuesUnionDiagnosticSql() {
  std::string sql = "VALUES";
  for (std::size_t value = 1; value <= 256U; ++value) {
    sql += value == 1U ? "(" : ",(";
    sql += "modern_sqlite_probe(";
    sql += std::to_string(value);
    sql += ',';
    sql += std::to_string(value);
    sql += "))";
  }
  sql += " UNION ALL SELECT modern_sqlite_probe(257,id+0) FROM items ORDER BY 1 LIMIT 64";
  return sql;
}

const std::string kLargeValuesUnionDiagnosticSql = BuildLargeValuesUnionDiagnosticSql();

const std::array<std::pair<std::string_view, std::string_view>, 14> kDiagnosticSql{{
    {"distinct-low-card-memory", "SELECT DISTINCT modern_sqlite_probe(1,flag) FROM items"},
    {"distinct-high-card-memory", "SELECT DISTINCT modern_sqlite_probe(1,category) FROM items"},
    {"distinct-collated-memory",
     "SELECT DISTINCT modern_sqlite_probe(1,iif(flag=0,'A','a')) COLLATE NOCASE FROM items"},
    {"distinct-order-limit-memory",
     "SELECT DISTINCT modern_sqlite_probe(1,score+0) FROM items ORDER BY 1 DESC LIMIT 64"},
    {"values-union-all-limit-memory", kLargeValuesUnionDiagnosticSql},
    {"union-replace-file",
     "SELECT modern_sqlite_probe(1,score) FROM items WHERE id<=32768 "
     "UNION SELECT modern_sqlite_probe(2,score+0.0) FROM items WHERE id>32768"},
    {"except-membership-memory",
     "SELECT modern_sqlite_probe(1,score) FROM items WHERE flag=0 "
     "EXCEPT SELECT modern_sqlite_probe(2,score) FROM items WHERE id%4=0"},
    {"intersect-membership-memory",
     "SELECT modern_sqlite_probe(1,score) FROM items WHERE flag=0 "
     "INTERSECT SELECT modern_sqlite_probe(2,score) FROM items WHERE id%4=0"},
    {"ordered-union-all-topn-memory",
     "SELECT modern_sqlite_probe(1,id),payload FROM items WHERE flag=0 "
     "UNION ALL SELECT modern_sqlite_probe(2,id),payload FROM items WHERE flag=1 "
     "UNION ALL SELECT modern_sqlite_probe(3,id),payload FROM items WHERE id%4=0 "
     "ORDER BY 2,1 LIMIT 64 OFFSET 64"},
    {"ordered-union-merge-memory",
     "SELECT modern_sqlite_probe(1,score) FROM items WHERE id<=32768 "
     "UNION SELECT modern_sqlite_probe(2,score+0.0) FROM items WHERE id>32768 ORDER BY 1"},
    {"ordered-except-merge-memory",
     "SELECT modern_sqlite_probe(1,score) FROM items WHERE flag=0 "
     "EXCEPT SELECT modern_sqlite_probe(2,score) FROM items WHERE id%4=0 ORDER BY 1"},
    {"ordered-intersect-merge-memory",
     "SELECT modern_sqlite_probe(1,score) FROM items WHERE flag=0 "
     "INTERSECT SELECT modern_sqlite_probe(2,score) FROM items WHERE id%4=0 ORDER BY 1"},
    {"long-mixed-compound-memory",
     "SELECT modern_sqlite_probe(1,score) FROM items WHERE id<=16384 "
     "UNION ALL SELECT modern_sqlite_probe(2,score) FROM items "
     "WHERE id>16384 AND id<=32768 "
     "UNION SELECT modern_sqlite_probe(3,score) FROM items WHERE id>32768 AND id<=49152 "
     "EXCEPT SELECT modern_sqlite_probe(4,score) FROM items WHERE id%8=0 "
     "INTERSECT SELECT modern_sqlite_probe(5,score) FROM items WHERE flag=1 ORDER BY 1"},
    {"union-keyed-relation-before-file",
     "SELECT modern_sqlite_probe(1,category),payload FROM items WHERE id<=32768 "
     "UNION SELECT modern_sqlite_probe(2,category),payload FROM items WHERE id>32768"},
}};

[[maybe_unused, nodiscard]] std::string_view FindDiagnosticSql(std::string_view id) {
  const auto iterator =
      std::ranges::find_if(kDiagnosticSql, [id](const auto& entry) { return entry.first == id; });
  if (iterator == kDiagnosticSql.end()) {
    throw HarnessFailure{"unknown DISTINCT/compound diagnostic case"};
  }
  return iterator->second;
}

[[nodiscard]] std::vector<std::uint64_t> ExpectedProbeCounts(std::string_view id) {
  if (id == "values-union-all-limit-memory") {
    std::vector<std::uint64_t> counts(256, 1);
    counts.push_back(65'536);
    return counts;
  }
  if (id == "union-replace-file" || id == "ordered-union-merge-memory" ||
      id == "union-keyed-relation-before-file") {
    return {32'768, 32'768};
  }
  if (id == "except-membership-memory" || id == "intersect-membership-memory" ||
      id == "ordered-except-merge-memory" || id == "ordered-intersect-merge-memory") {
    return {32'768, 16'384};
  }
  if (id == "ordered-union-all-topn-memory") {
    return {32'768, 32'768, 16'384};
  }
  if (id == "long-mixed-compound-memory") {
    return {16'384, 16'384, 16'384, 8'192, 32'768};
  }
  if (id == "distinct-low-card-memory" || id == "distinct-high-card-memory" ||
      id == "distinct-collated-memory" || id == "distinct-order-limit-memory") {
    return {65'536};
  }
  throw HarnessFailure{"unknown DISTINCT/compound probe-count case"};
}
#endif

[[nodiscard]] const WorkloadDefinition& FindWorkload(std::string_view id) {
  const auto iterator = std::ranges::find(kWorkloads, id, &WorkloadDefinition::id);
  if (iterator == kWorkloads.end()) {
    throw HarnessFailure{"unknown DISTINCT/compound benchmark case"};
  }
  return *iterator;
}

#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
[[nodiscard]] DiagnosticRun RunDistinctModernDiagnostic(
    const std::filesystem::path& path, const WorkloadDefinition& definition,
    std::span<const std::uint64_t> permutation,
    modern_sqlite::instrumentation::ProbeCounterCollection& probes) {
  ModernEngine engine = ModernEngine::Open(path, definition);
  RequireWork("pre-verification", engine.Verify(definition), definition.verification);
  ModernEngine::Statement warmup_statement = engine.Prepare(definition);
  const WorkResult warmup =
      engine.Run(warmup_statement, definition, definition.warmup_iterations, permutation);
  RequireWork("warmup", warmup, definition.warmup);
  engine.Finalize(warmup_statement);

  modern_sqlite::instrumentation::CounterCollection counters;
  WorkResult diagnostic;
  probes.Reset();
  {
    const modern_sqlite::instrumentation::ScopedCounterCollection counter_scope{counters};
    const modern_sqlite::instrumentation::ScopedProbeCounterCollection probe_scope{probes};
    ModernEngine::Statement statement = engine.Prepare(definition);
    diagnostic = engine.Run(statement, definition, definition.diagnostic_iterations, permutation);
    engine.Finalize(statement);
  }
  RequireWork("diagnostic", diagnostic, definition.diagnostic);
  RequireWork("post-verification", engine.Verify(definition), definition.verification);
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
      .modern = ReadModernCounters(counters),
      .sqlite = std::nullopt,
  };
}

[[nodiscard]] DiagnosticRun RunDistinctSqliteDiagnostic(
    const std::filesystem::path& path, const WorkloadDefinition& definition,
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

  sqlite_probe_counts.fill(0);
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

template <typename Getter>
[[nodiscard]] std::vector<std::uint64_t> ValidateProbeCounts(
    std::span<const std::uint64_t> expected, Getter get_count) {
  if (expected.empty() || expected.size() >= modern_sqlite::instrumentation::kProbeTagCount ||
      get_count(0) != 0) {
    throw BenchmarkMismatch{"DISTINCT/compound probe-count shape is invalid"};
  }
  std::vector<std::uint64_t> observed;
  observed.reserve(expected.size());
  for (std::size_t tag = 1; tag < modern_sqlite::instrumentation::kProbeTagCount; ++tag) {
    const std::uint64_t count = get_count(tag);
    if (tag <= expected.size()) {
      if (count != expected[tag - 1U]) {
        throw BenchmarkMismatch{"DISTINCT/compound diagnostic probe count mismatch"};
      }
      observed.push_back(count);
    } else if (count != 0) {
      throw BenchmarkMismatch{"DISTINCT/compound diagnostic used an unexpected probe tag"};
    }
  }
  return observed;
}

[[nodiscard]] std::uint64_t ProbeRowCount(std::span<const std::uint64_t> counts) {
  std::uint64_t total = 0;
  for (const std::uint64_t count : counts) {
    if (count > std::numeric_limits<std::uint64_t>::max() - total) {
      throw BenchmarkMismatch{"DISTINCT/compound diagnostic probe count overflowed"};
    }
    total += count;
  }
  return total;
}

void PrintDistinctCompoundDiagnosticReport(EngineKind engine, const WorkloadDefinition& definition,
                                           const DiagnosticRun& run,
                                           std::span<const std::uint64_t> probe_counts) {
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
  std::cout << ",\"mode\":\"diagnostic\",\"probe_counts\":[";
  for (std::size_t index = 0; index < probe_counts.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    std::cout << probe_counts[index];
  }
  std::cout << "],\"schema_version\":1,\"source_rows\":" << ProbeRowCount(probe_counts)
            << ",\"sqlite\":";
  PrintSqliteIdentity(std::cout);
  std::cout << ",\"work\":";
  PrintWork(std::cout, run.work);
  std::cout << ",\"workload_semantics_version\":1}\n";
}
#endif

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
    WorkloadDefinition diagnostic_definition = definition;
    diagnostic_definition.sql = FindDiagnosticSql(definition.id);
    const std::vector<std::uint64_t> expected_probe_counts = ExpectedProbeCounts(definition.id);
    std::vector<std::uint64_t> observed_probe_counts;
    DiagnosticRun run;
    if (engine == EngineKind::kModern) {
      modern_sqlite::instrumentation::ProbeCounterCollection probes;
      run = RunDistinctModernDiagnostic(database_path, diagnostic_definition, permutation, probes);
      observed_probe_counts = ValidateProbeCounts(
          expected_probe_counts, [&probes](std::size_t tag) { return probes.Value(tag); });
    } else {
      run = RunDistinctSqliteDiagnostic(database_path, diagnostic_definition, permutation);
      observed_probe_counts = ValidateProbeCounts(
          expected_probe_counts, [](std::size_t tag) { return sqlite_probe_counts[tag]; });
    }
    if (ProbeRowCount(observed_probe_counts) != definition.diagnostic.items) {
      throw BenchmarkMismatch{"DISTINCT/compound diagnostic source-row count mismatch"};
    }
    ValidateNoSidecars(database_path);
    PrintDistinctCompoundDiagnosticReport(engine, definition, run, observed_probe_counts);
#else
    if (run_kind != "baseline" && run_kind != "smoke") {
      throw HarnessFailure{"timing binary accepts only baseline or smoke runs"};
    }
    const bool smoke = run_kind == "smoke";
    TimingRun run;
    if (engine == EngineKind::kModern) {
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

int main(int argc, char** argv) {
  try {
    return RunCommand(argc, argv);
  } catch (const BenchmarkMismatch& error) {
    std::cerr << "DISTINCT/compound performance mismatch: " << error.what() << '\n';
    return 2;
  } catch (const HarnessFailure& error) {
    std::cerr << "DISTINCT/compound performance harness error: " << error.what() << '\n';
    return 1;
  } catch (const std::bad_alloc&) {
    std::cerr << "DISTINCT/compound performance harness error: out of memory\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "DISTINCT/compound performance harness error: " << error.what() << '\n';
    return 1;
  }
}
