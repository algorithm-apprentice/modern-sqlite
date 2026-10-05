#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "read_fuzz.hpp"

namespace modern_sqlite::fuzz {
namespace {

constexpr std::size_t kMaximumSqlBytes = std::size_t{64} * 1024U;
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

void RunReadSqlInput(std::span<const std::uint8_t> input, std::string_view database_path) {
  if (input.size() > kMaximumSqlBytes) {
    return;
  }
  Result<ReadSession> opened = ReadSession::Open(database_path);
  if (!opened.has_value()) {
    return;
  }
  ReadSession session = std::move(*opened);
  const char* const bytes = input.empty() ? "" : reinterpret_cast<const char*>(input.data());
  ConsumePrepared(session.Prepare(Utf8View{std::string_view{bytes, input.size()}}));
}

}  // namespace modern_sqlite::fuzz
