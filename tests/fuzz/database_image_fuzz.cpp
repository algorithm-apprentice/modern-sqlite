#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "../support/private_database.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "read_fuzz.hpp"

namespace modern_sqlite::fuzz {
namespace {

constexpr std::size_t kMaximumDatabaseBytes = std::size_t{1024} * 1024U;
constexpr std::size_t kMaximumRows = 256;

void ConsumePrepared(Result<ReadPrepareOutput> prepared) {
  if (!prepared.has_value() || !prepared->statement.has_value()) {
    return;
  }
  ReadStatement statement = std::move(*prepared->statement);
  for (std::size_t row = 0; row < kMaximumRows; ++row) {
    const Result<ReadStep> step = statement.Step();
    if (!step.has_value() || *step == ReadStep::kDone) {
      break;
    }
  }
  [[maybe_unused]] const Status finalized = statement.Finalize();
}

}  // namespace

void RunDatabaseImageInput(std::span<const std::uint8_t> input) {
  if (input.size() > kMaximumDatabaseBytes) {
    return;
  }
  test_support::PrivateDatabase database;
  if (!database.Initialize(input)) {
    return;
  }

  Result<ReadSession> opened = ReadSession::Open(database.path());
  if (!opened.has_value()) {
    return;
  }
  ReadSession session = std::move(*opened);
  Result<ReadPrepareOutput> schema = session.Prepare(Utf8View{"SELECT name FROM sqlite_schema"});
  database.Unlink();
  ConsumePrepared(std::move(schema));
  ConsumePrepared(session.Prepare(Utf8View{"SELECT 1"}));
}

}  // namespace modern_sqlite::fuzz
