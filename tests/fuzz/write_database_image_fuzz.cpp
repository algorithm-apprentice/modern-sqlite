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

constexpr std::size_t kMaximumDatabaseBytes = std::size_t{1024} * 1024U;
constexpr std::size_t kMaximumRows = 256;

void ExecuteBounded(WriteSession& session, std::string_view sql) {
  Result<WritePrepareOutput> prepared = session.Prepare(Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value()) {
    return;
  }
  WriteStatement statement = std::move(*prepared->statement);
  for (std::size_t row = 0; row < kMaximumRows; ++row) {
    const Result<WriteStep> stepped = statement.Step();
    if (!stepped.has_value() || *stepped == WriteStep::kDone) {
      break;
    }
  }
  [[maybe_unused]] const Status finalized = statement.Finalize();
}

}  // namespace

void RunWriteDatabaseImageInput(std::span<const std::uint8_t> input) {
  if (input.size() > kMaximumDatabaseBytes) {
    return;
  }

  detail::PrivateDatabase database;
  if (!database.Initialize(input)) {
    return;
  }
  Result<WriteSession> opened = WriteSession::Open(database.path());
  if (!opened.has_value()) {
    return;
  }
  WriteSession session = std::move(*opened);

  ExecuteBounded(session, "SELECT name FROM sqlite_schema");
  ExecuteBounded(session,
                 "CREATE TABLE IF NOT EXISTS fuzz_target("
                 "id INTEGER PRIMARY KEY,"
                 "value TEXT NOT NULL DEFAULT 'seed',"
                 "score REAL,"
                 "payload BLOB"
                 ")");
  ExecuteBounded(session, "INSERT INTO fuzz_target VALUES(1,'one',1,x'01')");
  ExecuteBounded(session,
                 "CREATE INDEX IF NOT EXISTS fuzz_target_value_score "
                 "ON fuzz_target(value COLLATE NOCASE DESC,score)");
  ExecuteBounded(session, "UPDATE fuzz_target SET value=value||'x' WHERE id=1");
  ExecuteBounded(session, "DELETE FROM fuzz_target WHERE id=2");
  ExecuteBounded(session, "SAVEPOINT fuzz_scope");
  ExecuteBounded(session, "INSERT INTO fuzz_target VALUES(2,'rollback',2,x'02')");
  ExecuteBounded(session, "ROLLBACK TO fuzz_scope");
  ExecuteBounded(session, "RELEASE fuzz_scope");
  ExecuteBounded(session, "ANALYZE fuzz_target");
  ExecuteBounded(session, "SELECT id,value,score,payload FROM fuzz_target");
  if (!session.autocommit()) {
    ExecuteBounded(session, "ROLLBACK");
  }
}

}  // namespace modern_sqlite::fuzz
