#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "write_fuzz.hpp"
#include "write_fuzz_support.hpp"

namespace modern_sqlite::fuzz {
namespace {

constexpr std::size_t kMaximumSqlBytes = std::size_t{64} * 1024U;
constexpr std::size_t kMaximumStatements = 32;
constexpr std::size_t kMaximumTotalSteps = 4096;
constexpr std::size_t kMaximumRowsPerStatement = 256;
[[nodiscard]] bool ExecuteDone(WriteSession& session, std::string_view sql) {
  auto prepared = session.Prepare(Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value() ||
      prepared->next_offset.value() != sql.size()) {
    return false;
  }
  WriteStatement statement = std::move(*prepared->statement);
  const Result<WriteStep> stepped = statement.Step();
  const Status finalized = statement.Finalize();
  return stepped.has_value() && *stepped == WriteStep::kDone && finalized.has_value();
}

[[nodiscard]] bool SeedDatabase(WriteSession& session) {
  return ExecuteDone(session,
                     "CREATE TABLE fuzz_target("
                     "id INTEGER PRIMARY KEY,"
                     "value TEXT NOT NULL DEFAULT 'seed',"
                     "score REAL,"
                     "payload BLOB"
                     ")") &&
         ExecuteDone(session, "INSERT INTO fuzz_target VALUES(1,'one',1,x'01')") &&
         ExecuteDone(session,
                     "CREATE INDEX fuzz_target_value_score "
                     "ON fuzz_target(value COLLATE NOCASE DESC,score)") &&
         ExecuteDone(session, "ANALYZE fuzz_target");
}

[[nodiscard]] bool ConsumeStatement(WriteStatement& statement, std::size_t& total_steps) {
  std::size_t rows = 0;
  while (total_steps < kMaximumTotalSteps) {
    const Result<WriteStep> stepped = statement.Step();
    ++total_steps;
    if (!stepped.has_value() || *stepped == WriteStep::kDone) {
      [[maybe_unused]] const Status finalized = statement.Finalize();
      return true;
    }
    ++rows;
    if (rows >= kMaximumRowsPerStatement) {
      [[maybe_unused]] const Status finalized = statement.Finalize();
      return false;
    }
  }
  [[maybe_unused]] const Status finalized = statement.Finalize();
  return false;
}

void BestEffortRollback(WriteSession& session) {
  if (!session.autocommit()) {
    static_cast<void>(ExecuteDone(session, "ROLLBACK"));
  }
}

}  // namespace

void RunWriteSqlInput(std::span<const std::uint8_t> input) {
  if (input.size() > kMaximumSqlBytes) {
    return;
  }

  detail::PrivateDatabase database;
  if (!database.Initialize({})) {
    return;
  }
  Result<WriteSession> opened = WriteSession::Open(database.path());
  if (!opened.has_value()) {
    return;
  }
  WriteSession session = std::move(*opened);
  if (!SeedDatabase(session)) {
    return;
  }

  const char* const bytes = input.empty() ? "" : reinterpret_cast<const char*>(input.data());
  std::string_view remaining{bytes, input.size()};
  std::size_t statements = 0;
  std::size_t total_steps = 0;
  while (!remaining.empty() && statements < kMaximumStatements &&
         total_steps < kMaximumTotalSteps) {
    Result<WritePrepareOutput> prepared = session.Prepare(Utf8View{remaining});
    if (!prepared.has_value()) {
      break;
    }
    const std::size_t consumed = prepared->next_offset.value();
    if (consumed == 0U || consumed > remaining.size()) {
      break;
    }
    remaining.remove_prefix(consumed);
    if (!prepared->statement.has_value()) {
      continue;
    }
    ++statements;
    WriteStatement statement = std::move(*prepared->statement);
    if (!ConsumeStatement(statement, total_steps)) {
      break;
    }
  }
  BestEffortRollback(session);
}

}  // namespace modern_sqlite::fuzz
