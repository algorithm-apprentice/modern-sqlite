#if defined(MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS) && \
    MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <limits>

#include "modern_sqlite/instrumentation/counters.hpp"

namespace {

std::array<std::uint64_t, modern_sqlite::instrumentation::kAggregateWorkFirstTag>
    aggregate_sqlite_probes{};

void AggregateSqliteProbe(sqlite3_context* context, int count, sqlite3_value** arguments) {
  if (count != 2 || arguments == nullptr || sqlite3_value_type(arguments[0]) != SQLITE_INTEGER) {
    sqlite3_result_error(context, "aggregate probe requires an integer tag and one value", -1);
    return;
  }
  const sqlite3_int64 tag = sqlite3_value_int64(arguments[0]);
  if (tag <= 0 || static_cast<std::uint64_t>(tag) >= aggregate_sqlite_probes.size()) {
    sqlite3_result_error(context, "aggregate probe tag is invalid", -1);
    return;
  }
  std::uint64_t& value = aggregate_sqlite_probes[static_cast<std::size_t>(tag)];
  if (value == std::numeric_limits<std::uint64_t>::max()) {
    sqlite3_result_error(context, "aggregate probe count overflowed", -1);
    return;
  }
  ++value;
  sqlite3_result_value(context, arguments[1]);
}

int AggregateSqliteOpen(const char* path, sqlite3** database, int flags, const char* vfs) {
  const int opened = sqlite3_open_v2(path, database, flags, vfs);
  if (opened != SQLITE_OK || database == nullptr || *database == nullptr) {
    return opened;
  }
  return sqlite3_create_function_v2(*database, "modern_sqlite_probe", 2, SQLITE_UTF8, nullptr,
                                    AggregateSqliteProbe, nullptr, nullptr, nullptr);
}

}  // namespace

#define sqlite3_open_v2 AggregateSqliteOpen
#endif
#define kWorkloads kEmbeddedOrderWorkloads
#define FindWorkload FindEmbeddedOrderWorkload
#define RunCommand RunEmbeddedOrderCommand
#define main aggregate_embedded_order_main
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

constexpr std::size_t kAggregateMemoryBudget = 64U << 20U;
constexpr std::size_t kAggregateFileBudget = 2U << 20U;
constexpr std::size_t kAggregateOperationCount = 5;

struct AggregateCase {
  WorkloadDefinition workload;
  std::string_view diagnostic_sql;
  std::vector<std::uint64_t> probes;
  std::array<std::uint64_t, kAggregateOperationCount> operations;
};

[[nodiscard]] WorkloadDefinition AggregateWorkload(std::string_view id, std::string_view sql,
                                                   std::uint64_t items, std::uint64_t iterations,
                                                   std::uint64_t rows, std::uint64_t bytes,
                                                   std::uint64_t single_digest,
                                                   std::uint64_t measured_digest,
                                                   bool file = false) {
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
      .measured_iterations = iterations,
      .diagnostic_iterations = 1,
      .items_per_iteration = items,
      .result_rows_per_iteration = rows,
      .payload_size = kOrderValueSize,
      .temporary_store_memory = !file,
      .sorter_memory_threshold = file ? kAggregateFileBudget : kAggregateMemoryBudget,
      .warmup = Work(1, items, rows, bytes, 1, 0, single_digest),
      .measured = Work(iterations, iterations * items, iterations * rows, iterations * bytes,
                       iterations, 0, measured_digest),
      .diagnostic = Work(1, items, rows, bytes, 1, 0, single_digest),
      .verification = Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
  };
}

const std::array<AggregateCase, 15> kAggregateCases{{
    {
        AggregateWorkload(
            "global-functions-memory",
            "SELECT count(id+0),sum(score),total(score),avg(score),min(score+0),max(score+0) FROM "
            "items",
            65'536, 8, 1, 48, 0x4FAAA8E066A13C6EULL, 0x92DDAE023FD3037DULL),
        "SELECT modern_sqlite_probe(2,count(modern_sqlite_probe(1,id+0))),"
        "sum(score),total(score),avg(score),min(score+0),max(score+0) FROM items",
        {65'536, 1},
        {6, 393'216, 6, 0, 6},
    },
    {
        AggregateWorkload(
            "global-empty-memory",
            "SELECT count(id+0),sum(score),total(score),avg(score),min(score+0),max(score+0) FROM "
            "items WHERE 0",
            0, 65'536, 1, 16, 0x7D7EEAB76B309583ULL, 0xE2F49F8851122325ULL),
        "SELECT modern_sqlite_probe(2,count(modern_sqlite_probe(1,id+0))),"
        "sum(score),total(score),avg(score),min(score+0),max(score+0) FROM items WHERE 0",
        {0, 1},
        {6, 0, 6, 0, 6},
    },
    {
        AggregateWorkload("indexed-min-memory", "SELECT min(score) FROM items", 1, 65'536, 1, 8,
                          0x156194DB51EEEA60ULL, 0x946BFF8B56EC2325ULL),
        "SELECT modern_sqlite_probe(1,min(score)) FROM items",
        {1},
        {1, 1, 1, 0, 1},
    },
    {
        AggregateWorkload("indexed-max-memory", "SELECT max(score) FROM items", 1, 65'536, 1, 8,
                          0xE9A131D7326B603AULL, 0xE9268781575A2325ULL),
        "SELECT modern_sqlite_probe(1,max(score)) FROM items",
        {1},
        {1, 1, 1, 0, 1},
    },
    {
        AggregateWorkload("ordered-group-memory",
                          "SELECT score,count(id) FROM items GROUP BY score ORDER BY score", 65'536,
                          16, 4'096, 65'536, 0xC8343924DDEBD3EEULL, 0x135EB317488E5CA5ULL),
        "SELECT modern_sqlite_probe(2,score),count(modern_sqlite_probe(1,id)) "
        "FROM items GROUP BY score ORDER BY score",
        {65'536, 4'096},
        {1, 65'536, 4'096, 4'095, 1},
    },
    {
        AggregateWorkload("sorted-group-memory",
                          "SELECT score+0,count(id) FROM items GROUP BY score+0 ORDER BY 1", 65'536,
                          4, 4'096, 65'536, 0xC8343924DDEBD3EEULL, 0xDDB07CAC1B8F3485ULL),
        "SELECT modern_sqlite_probe(2,score+0),count(modern_sqlite_probe(1,id)) "
        "FROM items GROUP BY score+0 ORDER BY score+0",
        {65'536, 4'096},
        {1, 65'536, 4'096, 4'095, 1},
    },
    {
        AggregateWorkload("sorted-group-file",
                          "SELECT score+0,count(id) FROM items GROUP BY score+0 ORDER BY 1", 65'536,
                          8, 4'096, 65'536, 0xC8343924DDEBD3EEULL, 0x7B09094F028C0CE5ULL, true),
        "SELECT modern_sqlite_probe(2,score+0),count(modern_sqlite_probe(1,id)) "
        "FROM items GROUP BY score+0 ORDER BY score+0",
        {65'536, 4'096},
        {1, 65'536, 4'096, 4'095, 1},
    },
    {
        AggregateWorkload(
            "high-card-group-memory",
            "SELECT category,count(id) FROM items GROUP BY category ORDER BY category", 65'536, 2,
            65'536, 1'638'400, 0x4BCFDB1CB4DE1E15ULL, 0x957DA2D4A8DB7F45ULL),
        "SELECT modern_sqlite_probe(2,category),count(modern_sqlite_probe(1,id)) "
        "FROM items GROUP BY category ORDER BY category",
        {65'536, 65'536},
        {1, 65'536, 65'536, 65'535, 1},
    },
    {
        AggregateWorkload(
            "grouped-having-topn-memory",
            "SELECT score+0 AS g,count(id) AS n,sum(id) AS s FROM items GROUP BY score+0 "
            "HAVING count(id)>8 ORDER BY s DESC,g LIMIT 64 OFFSET 64",
            65'536, 4, 64, 1'536, 0x0E03C4D112C9B3DCULL, 0x5A77B6FB3107EE95ULL),
        "SELECT score+0 AS g,count(modern_sqlite_probe(1,id)) AS n,"
        "sum(modern_sqlite_probe(2,id)) AS s FROM items GROUP BY score+0 "
        "HAVING n>8 ORDER BY s DESC,g LIMIT 64 OFFSET 64",
        {65'536, 65'536},
        {2, 131'072, 8'192, 8'190, 2},
    },
    {
        AggregateWorkload("distinct-state-memory",
                          "SELECT count(DISTINCT score),sum(DISTINCT score) FROM items", 65'536, 4,
                          1, 16, 0x9831A884D4B5C935ULL, 0xE9671C75B1ED1C25ULL),
        "SELECT count(DISTINCT modern_sqlite_probe(1,score)),"
        "sum(DISTINCT modern_sqlite_probe(2,score)) FROM items",
        {65'536, 65'536},
        {2, 8'192, 2, 0, 2},
    },
    {
        AggregateWorkload(
            "filtered-state-memory",
            "SELECT count(id) FILTER (WHERE flag=1),sum(score) FILTER (WHERE flag=0) FROM items",
            65'536, 8, 1, 16, 0x69B488F754F8F77EULL, 0xD781388B7C8CF59DULL),
        "SELECT count(modern_sqlite_probe(1,id)) FILTER (WHERE flag=1),"
        "sum(modern_sqlite_probe(2,score)) FILTER (WHERE flag=0) FROM items",
        {32'768, 32'768},
        {2, 65'536, 2, 0, 2},
    },
    {
        AggregateWorkload("local-order-memory",
                          "SELECT group_concat(score ORDER BY score DESC,id) FROM items", 65'536, 4,
                          1, 309'919, 0x5D8975A11DEDB5B6ULL, 0x3D2DF2ADD6D5CA81ULL),
        "SELECT group_concat(modern_sqlite_probe(1,score) ORDER BY score DESC,id) FROM items",
        {65'536},
        {1, 65'536, 1, 0, 1},
    },
    {
        AggregateWorkload("unique-local-order-memory",
                          "SELECT group_concat(DISTINCT score ORDER BY score) FROM items", 65'536,
                          8, 1, 19'369, 0x09579402DA892A6BULL, 0x00CD5E2543AD11D5ULL),
        "SELECT group_concat(DISTINCT modern_sqlite_probe(1,score) "
        "ORDER BY modern_sqlite_probe(1,score)) FROM items",
        {65'536},
        {1, 4'096, 1, 0, 1},
    },
    {
        AggregateWorkload(
            "wide-modifier-file",
            "SELECT group_concat(score ORDER BY id),group_concat(score ORDER BY id DESC),"
            "count(DISTINCT category),group_concat(DISTINCT score ORDER BY score) FROM items",
            65'536, 1, 1, 639'215, 0xC9ABC9B4135A16A2ULL, 0xC9ABC9B4135A16A2ULL, true),
        "SELECT group_concat(modern_sqlite_probe(1,score) ORDER BY id),"
        "group_concat(modern_sqlite_probe(2,score) ORDER BY id DESC),"
        "count(DISTINCT modern_sqlite_probe(3,category)),"
        "group_concat(DISTINCT modern_sqlite_probe(4,score) "
        "ORDER BY modern_sqlite_probe(4,score)) FROM items",
        {65'536, 65'536, 65'536, 65'536},
        {4, 200'704, 4, 0, 4},
    },
    {
        AggregateWorkload("aggregate-compound-memory",
                          "SELECT score+0 AS g,count(id) FROM items GROUP BY score+0 "
                          "UNION ALL SELECT -1,count(id) FROM items ORDER BY 1 LIMIT 64 OFFSET 64",
                          131'072, 4, 64, 1'024, 0xDDF54A75AAF0AF2EULL, 0x0431523D9BF32905ULL),
        "SELECT score+0 AS g,count(modern_sqlite_probe(1,id)) FROM items GROUP BY score+0 "
        "UNION ALL SELECT -1,count(modern_sqlite_probe(2,id)) FROM items "
        "ORDER BY 1 LIMIT 64 OFFSET 64",
        {65'536, 65'536},
        {2, 131'072, 4'097, 4'095, 2},
    },
}};

[[nodiscard]] const AggregateCase& FindAggregateCase(std::string_view id) {
  const auto found = std::ranges::find_if(
      kAggregateCases, [id](const AggregateCase& value) { return value.workload.id == id; });
  if (found == kAggregateCases.end()) {
    throw HarnessFailure{"unknown aggregate benchmark case"};
  }
  return *found;
}

#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
[[nodiscard]] DiagnosticRun RunAggregateModernDiagnostic(
    const std::filesystem::path& path, const WorkloadDefinition& definition,
    std::span<const std::uint64_t> permutation,
    modern_sqlite::instrumentation::ProbeCounterCollection& probes) {
  ModernEngine engine = ModernEngine::Open(path, definition);
  RequireWork("pre-verification", engine.Verify(definition), definition.verification);
  ModernEngine::Statement warmup = engine.Prepare(definition);
  RequireWork("warmup", engine.Run(warmup, definition, definition.warmup_iterations, permutation),
              definition.warmup);
  engine.Finalize(warmup);
  modern_sqlite::instrumentation::CounterCollection counters;
  WorkResult work;
  probes.Reset();
  {
    const modern_sqlite::instrumentation::ScopedCounterCollection counter_scope{counters};
    const modern_sqlite::instrumentation::ScopedProbeCounterCollection probe_scope{probes};
    ModernEngine::Statement statement = engine.Prepare(definition);
    work = engine.Run(statement, definition, definition.diagnostic_iterations, permutation);
    engine.Finalize(statement);
  }
  RequireWork("diagnostic", work, definition.diagnostic);
  RequireWork("post-verification", engine.Verify(definition), definition.verification);
  return DiagnosticRun{
      .work = work,
      .completion =
          Completion{
              .session_opens = 1,
              .statement_prepares = 4,
              .statement_finalizes = 4,
              .statement_resets =
                  definition.warmup_iterations + definition.diagnostic_iterations + 2,
              .pre_verifications = 1,
              .post_verifications = 1,
          },
      .modern = ReadModernCounters(counters),
      .sqlite = std::nullopt,
  };
}

[[nodiscard]] DiagnosticRun RunAggregateSqliteDiagnostic(
    const std::filesystem::path& path, const WorkloadDefinition& definition,
    std::span<const std::uint64_t> permutation) {
  SqliteEngine engine = SqliteEngine::Open(path, definition);
  RequireWork("pre-verification", engine.Verify(definition), definition.verification);
  SqliteEngine::Statement warmup = engine.Prepare(definition);
  RequireWork("warmup", engine.Run(warmup, definition, definition.warmup_iterations, permutation),
              definition.warmup);
  engine.Finalize(warmup);
  static_cast<void>(SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_HIT, true));
  static_cast<void>(SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_MISS, true));
  static_cast<void>(SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_CACHE_WRITE, true));
  static_cast<void>(SqliteDbStatus(engine.database(), SQLITE_DBSTATUS_TEMPBUF_SPILL, true));
  static_cast<void>(SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, true));
  static_cast<void>(SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, true));
  aggregate_sqlite_probes.fill(0);
  SqliteEngine::Statement statement = engine.Prepare(definition);
  const WorkResult work =
      engine.Run(statement, definition, definition.diagnostic_iterations, permutation);
  RequireWork("diagnostic", work, definition.diagnostic);
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
  const auto allocations = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, false);
  counters.malloc_count_current = allocations.first;
  counters.malloc_count_highwater = allocations.second;
  counters.malloc_size_highwater = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, false).second;
  RequireWork("post-verification", engine.Verify(definition), definition.verification);
  return DiagnosticRun{
      .work = work,
      .completion =
          Completion{
              .session_opens = 1,
              .statement_prepares = 4,
              .statement_finalizes = 4,
              .statement_resets =
                  definition.warmup_iterations + definition.diagnostic_iterations + 2,
              .pre_verifications = 1,
              .post_verifications = 1,
          },
      .modern = std::nullopt,
      .sqlite = counters,
  };
}

template <typename Getter>
[[nodiscard]] std::vector<std::uint64_t> ValidateAggregateProbes(const AggregateCase& selected,
                                                                 Getter value) {
  if (value(0) != 0) {
    throw BenchmarkMismatch{"aggregate diagnostic used the zero probe tag"};
  }
  std::vector<std::uint64_t> observed;
  for (std::size_t tag = 1; tag < modern_sqlite::instrumentation::kAggregateWorkFirstTag; ++tag) {
    const std::uint64_t count = value(tag);
    if (tag <= selected.probes.size()) {
      if (count != selected.probes[tag - 1U]) {
        throw BenchmarkMismatch{"aggregate expression probe count mismatch"};
      }
      observed.push_back(count);
    } else if (count != 0) {
      throw BenchmarkMismatch{"aggregate diagnostic used an unassigned probe tag"};
    }
  }
  return observed;
}

void PrintAggregateDiagnostic(
    EngineKind engine, const AggregateCase& selected, const DiagnosticRun& run,
    std::span<const std::uint64_t> probes,
    const modern_sqlite::instrumentation::ProbeCounterCollection* aggregate) {
  std::cout << "{\"build\":";
  PrintBuild(std::cout, true);
  std::cout << ",\"case\":";
  PrintJsonString(std::cout, selected.workload.id);
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
  PrintConfiguration(std::cout, selected.workload);
  std::cout << ",\"engine\":";
  PrintJsonString(std::cout, EngineName(engine));
  std::cout << ",\"mode\":\"diagnostic\",\"probe_counts\":[";
  for (std::size_t index = 0; index < probes.size(); ++index) {
    std::cout << (index == 0 ? "" : ",") << probes[index];
  }
  std::cout << "],\"aggregate_work\":{";
  if (aggregate != nullptr) {
    for (std::size_t index = 0; index < modern_sqlite::instrumentation::kAggregateWorkMetricCount;
         ++index) {
      const auto metric = static_cast<modern_sqlite::instrumentation::AggregateWorkMetric>(index);
      const std::uint64_t value = aggregate->Value(metric);
      if (index < selected.operations.size() && value != selected.operations[index]) {
        throw BenchmarkMismatch{"aggregate lifecycle operation count mismatch"};
      }
      std::cout << (index == 0 ? "" : ",");
      PrintJsonString(std::cout, modern_sqlite::instrumentation::AggregateWorkMetricName(metric));
      std::cout << ':' << value;
    }
  }
  std::cout << "},\"schema_version\":1,\"sqlite\":";
  PrintSqliteIdentity(std::cout);
  std::cout << ",\"work\":";
  PrintWork(std::cout, run.work);
  std::cout << ",\"workload_semantics_version\":1}\n";
}
#endif

int RunAggregateCommand(int argc, char** argv) {
  ValidateSqliteIdentity();
  if (argc == 2 && std::string_view{argv[1]} == "identity") {
    PrintIdentityReport();
    return 0;
  }
  if (argc != 6) {
    PrintUsage(std::cerr, argv[0]);
    return 1;
  }
  const std::string_view command{argv[1]};
  const EngineKind engine = ParseEngine(argv[2]);
  const AggregateCase& selected = FindAggregateCase(argv[3]);
  const WorkloadDefinition& definition = selected.workload;
  const std::filesystem::path path{argv[4]};
  ValidateDatabaseFile(path, definition);
  const auto permutation = GeneratePermutation(definition.row_count, definition.seed);
  if (command == "run") {
#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
    if (std::string_view{argv[5]} != "diagnostic") {
      throw HarnessFailure{"aggregate diagnostic binary requires diagnostic mode"};
    }
    WorkloadDefinition diagnostic = definition;
    diagnostic.sql = selected.diagnostic_sql;
    modern_sqlite::instrumentation::ProbeCounterCollection aggregate{
        modern_sqlite::instrumentation::ProbeCollectionKind::AggregateWork};
    DiagnosticRun run;
    std::vector<std::uint64_t> observed;
    if (engine == EngineKind::kModern) {
      run = RunAggregateModernDiagnostic(path, diagnostic, permutation, aggregate);
      observed = ValidateAggregateProbes(
          selected, [&aggregate](std::size_t tag) { return aggregate.Value(tag); });
    } else {
      run = RunAggregateSqliteDiagnostic(path, diagnostic, permutation);
      observed = ValidateAggregateProbes(
          selected, [](std::size_t tag) { return aggregate_sqlite_probes[tag]; });
    }
    ValidateNoSidecars(path);
    PrintAggregateDiagnostic(engine, selected, run, observed,
                             engine == EngineKind::kModern ? &aggregate : nullptr);
#else
    const std::string_view mode{argv[5]};
    if (mode != "baseline" && mode != "smoke") {
      throw HarnessFailure{"aggregate timing binary requires baseline or smoke mode"};
    }
    TimingRun run;
    if (engine == EngineKind::kModern) {
      ModernEngine instance = ModernEngine::Open(path, definition);
      run = RunTiming(instance, definition, mode == "smoke", permutation);
    } else {
      SqliteEngine instance = SqliteEngine::Open(path, definition);
      run = RunTiming(instance, definition, mode == "smoke", permutation);
    }
    ValidateNoSidecars(path);
    PrintTimingReport(engine, definition, mode == "smoke", run);
#endif
    return 0;
  }
  if (command == "profile") {
#if MODERN_SQLITE_ORDER_BY_PERFORMANCE_DIAGNOSTICS
    throw HarnessFailure{"aggregate diagnostic binary does not provide profile mode"};
#else
    const std::uint64_t iterations = ParseIterations(argv[5]);
    WorkResult work;
    Completion completion;
    if (engine == EngineKind::kModern) {
      ModernEngine instance = ModernEngine::Open(path, definition);
      std::tie(work, completion) = RunProfile(instance, definition, iterations, permutation);
    } else {
      SqliteEngine instance = SqliteEngine::Open(path, definition);
      std::tie(work, completion) = RunProfile(instance, definition, iterations, permutation);
    }
    ValidateNoSidecars(path);
    PrintProfileReport(engine, definition, iterations, work, completion);
    return 0;
#endif
  }
  throw HarnessFailure{"unknown aggregate benchmark command"};
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return RunAggregateCommand(argc, argv);
  } catch (const BenchmarkMismatch& error) {
    std::cerr << "aggregate performance mismatch: " << error.what() << '\n';
    return 2;
  } catch (const HarnessFailure& error) {
    std::cerr << "aggregate performance harness error: " << error.what() << '\n';
    return 1;
  } catch (const std::bad_alloc&) {
    std::cerr << "aggregate performance harness error: out of memory\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "aggregate performance harness error: " << error.what() << '\n';
    return 1;
  }
}
