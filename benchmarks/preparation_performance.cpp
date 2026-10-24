#define RunCommand RunEmbeddedReadCommand
#define main preparation_embedded_read_main
// NOLINTNEXTLINE(bugprone-suspicious-include): share the canonical timers and counters.
#include "read_performance.cpp"
#undef main
#undef RunCommand

#include <cstdio>

#include "modern_sqlite/syntax/parser.hpp"
#include "native_parser_adapter.h"
#include "preparation_performance_build_config.hpp"

namespace {

constexpr std::uint64_t kPreparationWarmup = 8;
constexpr std::uint64_t kPreparationSmokeIterations = 64;
constexpr std::uint64_t kPreparationDiagnosticIterations = 8;
constexpr std::size_t kPreparationWideColumns = 512;
constexpr std::size_t kPreparationCleanupIterations = 1'000;
constexpr std::uint64_t kPreparationFixturePages = 4'866;
constexpr modern_sqlite::ReadSessionOptions kPreparationSessionOptions{
    .temporary_storage = {.mode = modern_sqlite::TemporaryStoreMode::kMemory},
};

enum class PreparationPhase : std::uint8_t { Parse, Prepare };

struct PreparationCase {
  std::string_view id;
  std::string sql;
  bool accepted = true;
  std::size_t columns = 1;
  std::size_t parameters = 0;
  std::size_t error_tail = 0;
  std::uint64_t iterations = 0;
};

[[nodiscard]] std::string WidePreparationSql(std::size_t columns, bool malformed = false) {
  std::string sql{"SELECT id"};
  for (std::size_t column = 1; column < columns; ++column) {
    sql.append(",id");
  }
  sql.append(malformed ? ", FROM items" : " FROM items");
  return sql;
}

[[nodiscard]] std::string DeepPreparationSql(std::size_t depth, bool malformed = false) {
  std::string sql{"SELECT "};
  sql.append(depth, '(');
  sql.append("id");
  if (!malformed) {
    sql.append(depth, ')');
    sql.append(" FROM items WHERE id=?1");
  }
  return sql;
}

[[nodiscard]] const std::array<PreparationCase, 8>& PreparationCases() {
  static const std::array<PreparationCase, 8> cases = [] {
    std::array<PreparationCase, 8> definitions{{
        {.id = "short",
         .sql = "SELECT id FROM items WHERE id=?1",
         .parameters = 1,
         .iterations = 65'536},
        {.id = "wide-128", .sql = WidePreparationSql(128), .columns = 128, .iterations = 2'048},
        {.id = "wide-512",
         .sql = WidePreparationSql(kPreparationWideColumns),
         .columns = kPreparationWideColumns,
         .iterations = 512},
        {.id = "deep-64", .sql = DeepPreparationSql(64), .parameters = 1, .iterations = 16'384},
        {.id = "deep-256", .sql = DeepPreparationSql(256), .parameters = 1, .iterations = 4'096},
        {.id = "malformed-short",
         .sql = "SELECT (id+ FROM items",
         .accepted = false,
         .columns = 0,
         .iterations = 131'072},
        {.id = "malformed-wide",
         .sql = WidePreparationSql(128, true),
         .accepted = false,
         .columns = 0,
         .iterations = 2'048},
        {.id = "malformed-deep",
         .sql = DeepPreparationSql(256, true),
         .accepted = false,
         .columns = 0,
         .iterations = 16'384},
    }};
    for (PreparationCase& definition : definitions) {
      if (!definition.accepted) {
        const std::size_t from = definition.sql.find("FROM");
        definition.error_tail = from == std::string::npos ? definition.sql.size() : from + 4U;
      }
    }
    return definitions;
  }();
  return cases;
}

[[nodiscard]] PreparationPhase ParsePreparationPhase(std::string_view value) {
  if (value == "parse") {
    return PreparationPhase::Parse;
  }
  if (value == "prepare") {
    return PreparationPhase::Prepare;
  }
  throw HarnessFailure{"phase must be parse or prepare"};
}

[[nodiscard]] const PreparationCase& FindPreparationCase(std::string_view id) {
  const auto* const found = std::ranges::find(PreparationCases(), id, &PreparationCase::id);
  if (found == PreparationCases().end()) {
    throw HarnessFailure{"unknown preparation case"};
  }
  return *found;
}

struct NativeParserDeleter {
  void operator()(ModernSqliteNativeParser* parser) const noexcept {
    if (ModernSqliteNativeParserDestroy(parser) != SQLITE_OK) {
      std::fputs("native parser context did not close\n", stderr);
      std::abort();
    }
  }
};

using NativeParserOwner = std::unique_ptr<ModernSqliteNativeParser, NativeParserDeleter>;

[[nodiscard]] NativeParserOwner OpenNativeParser() {
  ModernSqliteNativeParser* parser = nullptr;
  if (ModernSqliteNativeParserCreate(&parser) != SQLITE_OK || parser == nullptr) {
    throw HarnessFailure{"native parser context creation failed"};
  }
  return NativeParserOwner{parser};
}

[[nodiscard]] ModernSqliteNativeParseResult NativeParse(ModernSqliteNativeParser* parser,
                                                        const std::string& sql) {
  ModernSqliteNativeParseResult result{};
  if (ModernSqliteNativeParserRun(parser, sql.c_str(), sql.size(), &result) != SQLITE_OK ||
      result.has_vdbe != 0 || result.consumed > sql.size()) {
    throw BenchmarkMismatch{"native syntax isolation or cleanup invariant failed"};
  }
  return result;
}

struct PreparationOutcome {
  bool accepted = false;
  std::optional<std::size_t> consumed;
  std::size_t columns = 0;
  std::size_t parameters = 0;

  constexpr bool operator==(const PreparationOutcome&) const noexcept = default;
};

struct PreparationCompletion {
  std::uint64_t parse_calls = 0;
  std::uint64_t prepare_calls = 0;
  std::uint64_t statement_finalizes = 0;
};

[[nodiscard]] PreparationOutcome ExpectedPreparationOutcome(const PreparationCase& definition,
                                                            PreparationPhase phase) {
  return PreparationOutcome{
      .accepted = definition.accepted,
      .consumed = definition.accepted
                      ? std::optional{definition.sql.size()}
                      : (phase == PreparationPhase::Parse ? std::optional{definition.error_tail}
                                                          : std::nullopt),
      .columns = phase == PreparationPhase::Prepare ? definition.columns : 0,
      .parameters = phase == PreparationPhase::Prepare ? definition.parameters : 0,
  };
}

template <typename Callable>
[[nodiscard]] WorkResult RunPreparationWork(Callable& operation, const PreparationCase& definition,
                                            PreparationPhase phase, std::uint64_t iterations,
                                            PreparationCompletion& completion) {
  const PreparationOutcome expected = ExpectedPreparationOutcome(definition, phase);
  WorkResult work;
  Digest digest;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    if (phase == PreparationPhase::Parse) {
      ++completion.parse_calls;
    } else {
      ++completion.prepare_calls;
    }
    const PreparationOutcome actual = operation();
    if (actual != expected) {
      throw BenchmarkMismatch{"preparation acceptance, tail or metadata differs from its control"};
    }
    digest.AddTag(actual.accepted ? 'A' : 'E');
    digest.AddKey(actual.consumed.value_or(0));
    digest.AddKey(actual.columns);
    digest.AddKey(actual.parameters);
    ++work.operations;
    ++work.items;
    work.bytes += definition.sql.size();
    work.result_hits += actual.accepted ? 1U : 0U;
    work.result_misses += actual.accepted ? 0U : 1U;
  }
  work.digest = digest.value();
  return work;
}

struct PreparationRun {
  WorkResult warmup;
  WorkResult diagnostic;
  std::vector<Repetition> repetitions;
  PreparationCompletion completion;
  std::optional<ModernCounterValues> modern;
  std::optional<ModernSqliteNativeParserStatus> sqlite;
};

#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
[[nodiscard]] ModernSqliteNativeParserStatus PreparationNativeStatus(
    ModernSqliteNativeParser* parser, sqlite3* database, bool reset) {
  ModernSqliteNativeParserStatus status{};
  if (parser != nullptr) {
    if (ModernSqliteNativeParserReadStatus(parser, reset ? 1 : 0, &status) != SQLITE_OK) {
      throw HarnessFailure{"native syntax allocation status failed"};
    }
    return status;
  }
  const auto allocations = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, reset);
  const auto memory = SqliteGlobalStatus(SQLITE_STATUS_MEMORY_USED, reset);
  const auto lookaside = SqliteDbStatus(database, SQLITE_DBSTATUS_LOOKASIDE_USED, reset);
  status.malloc_count_current = allocations.first;
  status.malloc_count_highwater = allocations.second;
  status.memory_bytes_current = memory.first;
  status.memory_bytes_highwater = memory.second;
  status.lookaside_used = lookaside.first;
  status.lookaside_highwater = lookaside.second;
  status.lookaside_hits = SqliteDbStatus(database, SQLITE_DBSTATUS_LOOKASIDE_HIT, reset).second;
  return status;
}
#endif

template <typename Callable>
[[nodiscard]] PreparationRun MeasurePreparation(
    Callable operation, const PreparationCase& definition, PreparationPhase phase,
    std::string_view mode, std::uint64_t profile_iterations, PreparationCompletion& completion,
    ModernSqliteNativeParser* parser = nullptr, sqlite3* database = nullptr) {
  PreparationRun run;
  static_cast<void>(RunPreparationWork(operation, definition, phase, 1, completion));
  run.warmup = RunPreparationWork(operation, definition, phase, kPreparationWarmup, completion);
#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
  static_cast<void>(mode);
  static_cast<void>(profile_iterations);
  if (parser != nullptr || database != nullptr) {
    static_cast<void>(PreparationNativeStatus(parser, database, true));
  }
  modern_sqlite::instrumentation::CounterCollection counters;
  {
    const modern_sqlite::instrumentation::ScopedCounterCollection scope{counters};
    run.diagnostic = RunPreparationWork(operation, definition, phase,
                                        kPreparationDiagnosticIterations, completion);
  }
  if (parser != nullptr || database != nullptr) {
    run.sqlite = PreparationNativeStatus(parser, database, false);
  } else {
    run.modern = ReadModernCounters(counters);
  }
#else
  static_cast<void>(parser);
  static_cast<void>(database);
  const bool smoke = mode == "smoke";
  const bool profile = mode == "profile";
  const std::size_t repetitions = smoke || profile ? 1U : kTimingRepetitions;
  const std::uint64_t iterations =
      profile ? profile_iterations : (smoke ? kPreparationSmokeIterations : definition.iterations);
  run.repetitions.reserve(repetitions);
  for (std::size_t index = 0; index < repetitions; ++index) {
    const Repetition repetition = Measure(index, [&] {
      return RunPreparationWork(operation, definition, phase, iterations, completion);
    });
    if (!smoke && !profile && repetition.wall_ns < kIndexMinimumWallNanoseconds) {
      throw HarnessFailure{"preparation repetition did not reach the minimum wall time"};
    }
    run.repetitions.push_back(repetition);
  }
#endif
  static_cast<void>(RunPreparationWork(operation, definition, phase, 1, completion));
  run.completion = completion;
  return run;
}

[[nodiscard]] PreparationRun RunPreparation(EngineKind engine, PreparationPhase phase,
                                            const PreparationCase& definition,
                                            const std::filesystem::path& database_path,
                                            std::string_view mode,
                                            std::uint64_t profile_iterations) {
  PreparationCompletion completion;
  if (phase == PreparationPhase::Parse) {
    if (engine == EngineKind::kModern) {
      return MeasurePreparation(
          [&] {
            auto parsed = modern_sqlite::ParseOne(modern_sqlite::Utf8View{definition.sql});
            if (!parsed.has_value()) {
              if (parsed.error().code != modern_sqlite::ParseErrorCode::kUnexpectedToken) {
                throw BenchmarkMismatch{"Modern parse returned a non-syntax failure"};
              }
              return PreparationOutcome{.consumed = parsed.error().next_offset.value()};
            }
            return PreparationOutcome{.accepted = parsed->tree.has_value(),
                                      .consumed = parsed->next_offset.value()};
          },
          definition, phase, mode, profile_iterations, completion);
    }
    const NativeParserOwner parser = OpenNativeParser();
    return MeasurePreparation(
        [&] {
          const auto parsed = NativeParse(parser.get(), definition.sql);
          if (parsed.code != SQLITE_DONE && parsed.code != SQLITE_ERROR) {
            throw HarnessFailure{"native parse returned a non-syntax failure"};
          }
          return PreparationOutcome{.accepted = parsed.code == SQLITE_DONE && parsed.errors == 0,
                                    .consumed = parsed.consumed};
        },
        definition, phase, mode, profile_iterations, completion, parser.get());
  }
  if (engine == EngineKind::kModern) {
    auto session = TakeResult(
        modern_sqlite::ReadSession::Open(database_path.string(), kPreparationSessionOptions),
        "Modern preparation session");
    return MeasurePreparation(
        [&] {
          auto prepared = session.Prepare(modern_sqlite::Utf8View{definition.sql});
          if (!prepared.has_value()) {
            if (prepared.error().code() != modern_sqlite::ErrorCode::kGeneric ||
                !prepared.error().message().starts_with("parse failed with unexpected_token")) {
              throw BenchmarkMismatch{"Modern prepare returned a non-syntax failure"};
            }
            return PreparationOutcome{};
          }
          if (!prepared->statement.has_value()) {
            throw BenchmarkMismatch{"Modern prepare returned no statement"};
          }
          const PreparationOutcome outcome{
              .accepted = true,
              .consumed = prepared->next_offset.value(),
              .columns = prepared->statement->result_columns().size(),
              .parameters = prepared->statement->parameter_count(),
          };
          RequireStatus(prepared->statement->Finalize(), "Modern preparation finalize");
          ++completion.statement_finalizes;
          return outcome;
        },
        definition, phase, mode, profile_iterations, completion);
  }
  WorkloadDefinition fixture{};
  fixture.page_count = kPreparationFixturePages;
  SqliteEngine sqlite = SqliteEngine::Open(database_path, fixture);
  static_cast<void>(sqlite3_limit(sqlite.database(), SQLITE_LIMIT_COLUMN, 2'000));
  static_cast<void>(sqlite3_limit(sqlite.database(), SQLITE_LIMIT_SQL_LENGTH, 1'000'000'000));
  return MeasurePreparation(
      [&] {
        sqlite3_stmt* raw = nullptr;
        const char* tail = nullptr;
        const int code =
            sqlite3_prepare_v3(sqlite.database(), definition.sql.c_str(),
                               static_cast<int>(definition.sql.size()), 0, &raw, &tail);
        SqliteStatement statement{raw};
        if (code != SQLITE_OK) {
          if (code != SQLITE_ERROR || raw != nullptr) {
            throw HarnessFailure{"native prepare returned a non-syntax failure"};
          }
          return PreparationOutcome{};
        }
        if (raw == nullptr || tail == nullptr) {
          throw BenchmarkMismatch{"native prepare returned no statement or tail"};
        }
        const PreparationOutcome outcome{
            .accepted = true,
            .consumed = static_cast<std::size_t>(tail - definition.sql.c_str()),
            .columns = static_cast<std::size_t>(sqlite3_column_count(raw)),
            .parameters = static_cast<std::size_t>(sqlite3_bind_parameter_count(raw)),
        };
        sqlite.Finalize(statement);
        ++completion.statement_finalizes;
        return outcome;
      },
      definition, phase, mode, profile_iterations, completion, nullptr, sqlite.database());
}

void PrintNativeParserIdentity() {
  namespace config = modern_sqlite::preparation_performance_build_config;
  std::cout << "{\"role\":\"derived-select-syntax\",\"semantic_callback_guards\":2,"
               "\"symbol_linkage\":\"translation-unit-local\",\"source_sha256\":";
  PrintJsonString(std::cout, config::kNativeSourceSha256);
  std::cout << ",\"generated_sha256\":";
  PrintJsonString(std::cout, config::kNativeGeneratedSha256);
  std::cout << ",\"source_id\":";
  PrintJsonString(std::cout, ModernSqliteNativeParserSourceId());
  std::cout << ",\"diagnostic_allocator_wrappers\":"
            << (MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS ? "true" : "false") << '}';
}

void PrintPreparationCases() {
  std::cout << R"({"schema_version":1,"minimum_wall_ns":)" << kIndexMinimumWallNanoseconds
            << ",\"warmup_iterations\":" << kPreparationWarmup
            << ",\"smoke_iterations\":" << kPreparationSmokeIterations
            << ",\"diagnostic_iterations\":" << kPreparationDiagnosticIterations << ",\"cases\":[";
  for (std::size_t index = 0; index < PreparationCases().size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    const auto& definition = PreparationCases()[index];
    std::cout << "{\"id\":";
    PrintJsonString(std::cout, definition.id);
    std::cout << ",\"sql\":";
    PrintJsonString(std::cout, definition.sql);
    std::cout << ",\"accepted\":" << (definition.accepted ? "true" : "false")
              << ",\"columns\":" << definition.columns
              << ",\"parameters\":" << definition.parameters
              << ",\"error_tail\":" << definition.error_tail
              << ",\"measured_iterations\":" << definition.iterations << '}';
  }
  std::cout << "]}\n";
}

void PrintPreparationReport(EngineKind engine, PreparationPhase phase,
                            const PreparationCase& definition, std::string_view mode,
                            const PreparationRun& run) {
  std::cout << R"({"schema_version":1,"mode":)";
  PrintJsonString(std::cout, mode == "diagnostic" ? "diagnostic" : "timing");
  std::cout << ",\"run_kind\":";
  PrintJsonString(std::cout, mode);
  std::cout << ",\"engine\":";
  PrintJsonString(std::cout, EngineName(engine));
  std::cout << ",\"phase\":";
  PrintJsonString(std::cout, phase == PreparationPhase::Parse ? "parse" : "prepare");
  std::cout << ",\"case\":";
  PrintJsonString(std::cout, definition.id);
  std::cout << ",\"reference_role\":";
  PrintJsonString(std::cout, phase == PreparationPhase::Parse ? "derived-select-syntax"
                                                              : "unmodified-public-prepare");
  std::cout << ",\"build\":";
  PrintBuild(std::cout, MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS != 0);
  std::cout << R"(,"completion":{"status":"complete","parse_calls":)" << run.completion.parse_calls
            << ",\"prepare_calls\":" << run.completion.prepare_calls
            << ",\"statement_finalizes\":" << run.completion.statement_finalizes
            << R"(,"statement_steps":0,"statement_resets":0},"effective_configuration":)";
  if (phase == PreparationPhase::Prepare) {
    const bool memory =
        engine == EngineKind::kSqlite || kPreparationSessionOptions.temporary_storage.mode ==
                                             modern_sqlite::TemporaryStoreMode::kMemory;
    PrintConfiguration(std::cout, memory ? "memory" : "file");
  } else {
    std::cout << "{\"catalog\":false,\"statement_scope\":\"SELECT\","
                 "\"modern_recursion_limit\":512,\"native_grammar\":\"Lemon\","
                 "\"maximum_columns\":2000,\"native_lookaside\":\"default\"}";
  }
  std::cout << ",\"warmup\":";
  PrintWork(std::cout, run.warmup);
  std::cout << ",\"repetitions\":[";
  for (std::size_t index = 0; index < run.repetitions.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    const auto& repetition = run.repetitions[index];
    std::cout << "{\"index\":" << repetition.index << ",\"wall_ns\":" << repetition.wall_ns
              << ",\"cpu_ns\":" << repetition.cpu_ns << ",\"work\":";
    PrintWork(std::cout, repetition.work);
    std::cout << '}';
  }
  std::cout << ']';
#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
  std::cout << ",\"work\":";
  PrintWork(std::cout, run.diagnostic);
  std::cout << R"(,"counters":{"modern":)";
  if (run.modern.has_value()) {
    PrintModernCounters(std::cout, *run.modern);
  } else {
    std::cout << "{}";
  }
  std::cout << ",\"sqlite\":";
  if (run.sqlite.has_value()) {
    const auto& status = *run.sqlite;
    std::cout << "{\"malloc_count_current\":" << status.malloc_count_current
              << ",\"malloc_count_highwater\":" << status.malloc_count_highwater
              << ",\"memory_bytes_current\":" << status.memory_bytes_current
              << ",\"memory_bytes_highwater\":" << status.memory_bytes_highwater
              << ",\"lookaside_used\":" << status.lookaside_used
              << ",\"lookaside_highwater\":" << status.lookaside_highwater
              << ",\"lookaside_hits\":" << status.lookaside_hits << '}';
  } else {
    std::cout << "{}";
  }
  std::cout << '}';
#endif
  std::cout << "}\n";
}

void RunNativeParserControls() {
  const NativeParserOwner parser = OpenNativeParser();
  const std::string unknown{"SELECT missing_column FROM missing_table"};
  const std::string malformed{"SELECT (1+"};
  const std::string tail{"SELECT 1; CREATE TABLE must_not_exist(x)"};
  const std::string trivia{"; /* prefix */ -- comment\n SELECT 1;"};
  if (NativeParse(parser.get(), unknown).code != SQLITE_DONE ||
      NativeParse(parser.get(), malformed).code != SQLITE_ERROR ||
      NativeParse(parser.get(), trivia).code != SQLITE_DONE ||
      NativeParse(parser.get(), tail).consumed != std::string_view{"SELECT 1;"}.size()) {
    throw BenchmarkMismatch{"native syntax/semantic/tail controls failed"};
  }
  constexpr std::array<std::string_view, 5> unsupported{{
      "CREATE TABLE forbidden(x)",
      "DROP TABLE forbidden",
      "PRAGMA user_version=9",
      "INSERT INTO forbidden VALUES(1)",
      "EXPLAIN SELECT 1",
  }};
  for (const auto sql : unsupported) {
    ModernSqliteNativeParseResult result{};
    if (ModernSqliteNativeParserRun(parser.get(), sql.data(), sql.size(), &result) !=
        SQLITE_MISUSE) {
      throw BenchmarkMismatch{"native syntax adapter admitted a foreign semantic action"};
    }
  }
  const std::string wide = WidePreparationSql(kPreparationWideColumns);
  static_cast<void>(NativeParse(parser.get(), wide));
  ModernSqliteNativeParserStatus before{};
  ModernSqliteNativeParserStatus after{};
  if (ModernSqliteNativeParserReadStatus(parser.get(), 1, &before) != SQLITE_OK) {
    throw HarnessFailure{"native cleanup control status failed"};
  }
  for (std::size_t iteration = 0; iteration < kPreparationCleanupIterations; ++iteration) {
    const bool accepted = iteration % 2U == 0;
    const auto result = NativeParse(parser.get(), accepted ? wide : malformed);
    if (result.code != (accepted ? SQLITE_DONE : SQLITE_ERROR)) {
      throw BenchmarkMismatch{"native same-context retry control failed"};
    }
  }
#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
  std::size_t oom_failures = 0;
  constexpr int kPreparationFailurePoints = 2'048;
  bool success = false;
  for (int failure = 0; failure < kPreparationFailurePoints; ++failure) {
    if (ModernSqliteNativeParserFailAfter(failure) != SQLITE_OK) {
      throw HarnessFailure{"native parser allocation fault was not armed"};
    }
    const auto result = NativeParse(parser.get(), wide);
    if (ModernSqliteNativeParserFailAfter(-1) != SQLITE_OK) {
      throw HarnessFailure{"native parser allocation fault was not cleared"};
    }
    if (result.code == SQLITE_DONE) {
      success = true;
      break;
    }
    if (result.code != SQLITE_NOMEM || NativeParse(parser.get(), unknown).code != SQLITE_DONE) {
      throw BenchmarkMismatch{"native parser OOM cleanup or same-context retry failed"};
    }
    ++oom_failures;
  }
  if (!success) {
    throw BenchmarkMismatch{"native parser allocation failure window exceeded its bound"};
  }
  if (oom_failures == 0) {
    throw BenchmarkMismatch{"native parser fault control did not reach the heap allocator"};
  }
  success = false;
  for (int failure = 0; failure < kPreparationFailurePoints; ++failure) {
    if (ModernSqliteNativeParserFailAfter(failure) != SQLITE_OK) {
      throw HarnessFailure{"native context allocation fault was not armed"};
    }
    ModernSqliteNativeParser* raw = nullptr;
    const int code = ModernSqliteNativeParserCreate(&raw);
    const NativeParserOwner candidate{raw};
    if (ModernSqliteNativeParserFailAfter(-1) != SQLITE_OK) {
      throw HarnessFailure{"native context allocation fault was not cleared"};
    }
    if (code == SQLITE_OK && candidate != nullptr) {
      success = true;
      break;
    }
    if (code != SQLITE_NOMEM || candidate != nullptr) {
      throw BenchmarkMismatch{"native context OOM did not clean its partial owner"};
    }
    ++oom_failures;
  }
  if (!success) {
    throw BenchmarkMismatch{"native context allocation failure window exceeded its bound"};
  }
#else
  constexpr std::size_t oom_failures = 0;
#endif
  if (ModernSqliteNativeParserReadStatus(parser.get(), 0, &after) != SQLITE_OK ||
      after.malloc_count_current != before.malloc_count_current ||
      after.memory_bytes_current != before.memory_bytes_current || after.lookaside_used != 0) {
    throw BenchmarkMismatch{"native parser retained an allocation or lookaside slot"};
  }
  sqlite3* raw_database = nullptr;
  const int opened = sqlite3_open(":memory:", &raw_database);
  const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database{raw_database,
                                                                       sqlite3_close_v2};
  CheckSqlite(opened, database.get(), "native control open");
  sqlite3_stmt* raw_statement = nullptr;
  const int code =
      sqlite3_prepare_v3(database.get(), unknown.c_str(), -1, 0, &raw_statement, nullptr);
  const SqliteStatement statement{raw_statement};
  if (code != SQLITE_ERROR || raw_statement != nullptr) {
    throw BenchmarkMismatch{"ordinary native prepare no longer reports the semantic error"};
  }
  std::cout << "{\"status\":\"complete\",\"no_vdbe_or_schema_mutation\":true,"
               "\"same_context_retry\":true,"
               "\"ordinary_prepare_still_rejects_unknown_table\":true,"
               "\"unsupported_statement_controls\":"
            << unsupported.size()
            << ",\"balanced_cleanup_iterations\":" << kPreparationCleanupIterations
            << ",\"oom_failures\":" << oom_failures << "}\n";
}

int RunPreparationCommand(int argc, const char* const* argv) {
  ValidateSqliteIdentity();
  if (std::string_view{ModernSqliteNativeParserSourceId()} != kSqliteSourceId) {
    throw HarnessFailure{"private parser reference identity is not pinned"};
  }
  if (argc == 2) {
    const std::string_view command{argv[1]};
    if (command == "identity") {
      std::cout << "{\"native_syntax_adapter\":";
      PrintNativeParserIdentity();
      std::cout << ",\"measurement\":";
      PrintIdentityReport();
      std::cout << "}\n";
      return 0;
    }
    if (command == "cases") {
      PrintPreparationCases();
      return 0;
    }
    if (command == "self-test") {
      RunNativeParserControls();
      return 0;
    }
  }
  if (argc != 7) {
    throw HarnessFailure{
        "usage: preparation <identity|cases|self-test> or "
        "<run|profile> <modern|sqlite> <parse|prepare> <case> <database> "
        "<baseline|smoke|diagnostic|profile-iterations>"};
  }
  const std::string_view command{argv[1]};
  const EngineKind engine = ParseEngine(argv[2]);
  const PreparationPhase phase = ParsePreparationPhase(argv[3]);
  const PreparationCase& definition = FindPreparationCase(argv[4]);
  const std::filesystem::path database{argv[5]};
  const std::string_view requested_mode{argv[6]};
#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
  if (command != "run" || requested_mode != "diagnostic") {
    throw HarnessFailure{"diagnostic preparation binary accepts only diagnostic runs"};
  }
  const std::string_view mode = requested_mode;
  constexpr std::uint64_t profile_iterations = 0;
#else
  const bool profile = command == "profile";
  const std::string_view mode = profile ? "profile" : requested_mode;
  const std::uint64_t profile_iterations = profile ? ParseIterations(requested_mode) : 0;
  if (!profile && (command != "run" || (mode != "baseline" && mode != "smoke"))) {
    throw HarnessFailure{"timing preparation binary accepts baseline, smoke or profile"};
  }
#endif
  if (!std::filesystem::is_regular_file(database)) {
    throw HarnessFailure{"preparation fixture does not exist"};
  }
  ValidateNoSidecars(database);
  const PreparationRun run =
      RunPreparation(engine, phase, definition, database, mode, profile_iterations);
  ValidateNoSidecars(database);
  PrintPreparationReport(engine, phase, definition, mode, run);
  return 0;
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): standard main requires char**.
int main(int argc, char** argv) {
  try {
    return RunPreparationCommand(argc, argv);
  } catch (const BenchmarkMismatch& error) {
    std::cerr << "preparation performance mismatch: " << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "preparation performance harness error: " << error.what() << '\n';
    return 1;
  }
}
