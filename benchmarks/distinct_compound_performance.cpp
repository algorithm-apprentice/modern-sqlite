#define kWorkloads kOrderByWorkloads
#define FindWorkload FindOrderByWorkload
#define RunCommand RunOrderByCommand
#define main order_by_benchmark_embedded_main
#include "order_by_performance.cpp"
#undef main
#undef RunCommand
#undef FindWorkload
#undef kWorkloads

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

[[nodiscard]] WorkloadDefinition ReadWorkload(
    std::string_view id, std::string_view sql, std::uint64_t items,
    std::uint64_t rows, std::uint64_t bytes, std::uint64_t digest,
    bool memory, std::size_t threshold, std::uint64_t measured_iterations = 1,
    std::uint64_t measured_digest = 0) {
  const WorkResult work = Work(1, items, rows, bytes, 1, 0, digest);
  const WorkResult measured =
      measured_iterations == 1
          ? work
          : Work(measured_iterations, measured_iterations * items,
                 measured_iterations * rows, measured_iterations * bytes,
                 measured_iterations, 0, measured_digest);
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
      .verification =
          Work(1, 65'536, 65'536, 17'301'504, 1, 0, 0xB760B119722FEDF5ULL),
  };
}

const std::array<WorkloadDefinition, 14> kWorkloads{{
    ReadWorkload("distinct-low-card-memory", "SELECT DISTINCT flag FROM items",
                 65'536, 2, 16, 0xE1E57E17779545B9ULL, true, 64U << 20U, 2,
                 0xA5D1922ACDC6225DULL),
    ReadWorkload("distinct-high-card-memory", "SELECT DISTINCT category FROM items",
                 65'536, 65'536, 1'114'112, 0x1115D744F5A9F725ULL, true,
                 64U << 20U),
    ReadWorkload("distinct-collated-memory",
                 "SELECT DISTINCT iif(flag=0,'A','a') COLLATE NOCASE FROM items",
                 65'536, 1, 1, 0x200B1B815C9EA247ULL, true, 64U << 20U, 2,
                 0x2596634236BD4EC5ULL),
    ReadWorkload("distinct-order-limit-memory",
                 "SELECT DISTINCT score+0 FROM items ORDER BY 1 DESC LIMIT 64",
                 65'536, 64, 512, 0xB23D465428610B6EULL, true, 64U << 20U, 2,
                 0xB9171F9835740255ULL),
    ReadWorkload("values-union-all-limit-memory", kLargeValuesUnionSql, 65'792,
                 64, 512, 0xEBFB42850728072EULL, true, 64U << 20U, 4,
                 0xE9F68D7C90B0B405ULL),
    ReadWorkload("union-replace-file",
                 "SELECT score FROM items WHERE id<=32768 "
                 "UNION SELECT score+0.0 FROM items WHERE id>32768",
                 131'072, 4'096, 32'768, 0xF2F777673354D459ULL, false,
                 2U << 20U),
    ReadWorkload("except-membership-memory",
                 "SELECT score FROM items WHERE flag=0 "
                 "EXCEPT SELECT score FROM items WHERE id%4=0",
                 131'072, 1'024, 8'192, 0x7B9F56FA8BDA78EEULL, true,
                 64U << 20U),
    ReadWorkload("intersect-membership-memory",
                 "SELECT score FROM items WHERE flag=0 "
                 "INTERSECT SELECT score FROM items WHERE id%4=0",
                 131'072, 1'024, 8'192, 0x83509A56B1476DEEULL, true,
                 64U << 20U),
    ReadWorkload("ordered-union-all-topn-memory",
                 "SELECT id,payload FROM items WHERE flag=0 "
                 "UNION ALL SELECT id,payload FROM items WHERE flag=1 "
                 "UNION ALL SELECT id,payload FROM items WHERE id%4=0 "
                 "ORDER BY payload,id LIMIT 64 OFFSET 64",
                 196'608, 64, 16'896, 0x9A886719EEA2BE2FULL, true,
                 64U << 20U),
    ReadWorkload("ordered-union-merge-memory",
                 "SELECT score FROM items WHERE id<=32768 "
                 "UNION SELECT score+0.0 FROM items WHERE id>32768 ORDER BY 1",
                 131'072, 4'096, 32'768, 0xF2F777673354D459ULL, true,
                 64U << 20U),
    ReadWorkload("ordered-except-merge-memory",
                 "SELECT score FROM items WHERE flag=0 "
                 "EXCEPT SELECT score FROM items WHERE id%4=0 ORDER BY 1",
                 131'072, 1'024, 8'192, 0x7B9F56FA8BDA78EEULL, true,
                 64U << 20U),
    ReadWorkload("ordered-intersect-merge-memory",
                 "SELECT score FROM items WHERE flag=0 "
                 "INTERSECT SELECT score FROM items WHERE id%4=0 ORDER BY 1",
                 131'072, 1'024, 8'192, 0x83509A56B1476DEEULL, true,
                 64U << 20U),
    ReadWorkload("long-mixed-compound-memory",
                 "SELECT score FROM items WHERE id<=16384 "
                 "UNION ALL SELECT score FROM items WHERE id>16384 AND id<=32768 "
                 "UNION SELECT score FROM items WHERE id>32768 AND id<=49152 "
                 "EXCEPT SELECT score FROM items WHERE id%8=0 "
                 "INTERSECT SELECT score FROM items WHERE flag=1 ORDER BY 1",
                 327'680, 2'048, 16'384, 0xAA64FC0C7C66E5EEULL, true,
                 64U << 20U),
    ReadWorkload("union-keyed-relation-before-file",
                 "SELECT category,payload FROM items WHERE id<=32768 "
                 "UNION SELECT category,payload FROM items WHERE id>32768",
                 131'072, 65'536, 17'891'328, 0xCE76FDEE5EB31AC8ULL, false,
                 2U << 20U),
}};

[[nodiscard]] const WorkloadDefinition& FindWorkload(std::string_view id) {
  const auto iterator = std::ranges::find(kWorkloads, id, &WorkloadDefinition::id);
  if (iterator == kWorkloads.end()) {
    throw HarnessFailure{"unknown DISTINCT/compound benchmark case"};
  }
  return *iterator;
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
    DiagnosticRun run =
        engine == EngineKind::kModern
            ? RunModernDiagnostic(database_path, definition, permutation)
            : RunSqliteDiagnostic(database_path, definition, permutation);
    ValidateNoSidecars(database_path);
    PrintDiagnosticReport(engine, definition, run);
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
      std::tie(work, completion) =
          RunProfile(modern, definition, iterations, permutation);
    } else {
      SqliteEngine sqlite = SqliteEngine::Open(database_path, definition);
      std::tie(work, completion) =
          RunProfile(sqlite, definition, iterations, permutation);
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
