#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "read_fuzz.hpp"

namespace modern_sqlite::fuzz {
namespace {

constexpr std::size_t kMaximumSqlBytes = std::size_t{64} * 1024U;
constexpr std::size_t kMaximumRows = 256;

class ByteReader final {
 public:
  explicit ByteReader(std::span<const std::uint8_t> input) noexcept : input_(input) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    if (input_.empty()) {
      return 0;
    }
    const std::uint8_t value = input_[offset_ % input_.size()];
    ++offset_;
    return value;
  }

 private:
  std::span<const std::uint8_t> input_;
  std::size_t offset_ = 0;
};

[[nodiscard]] std::string GeneratedLiteral(ByteReader& input) {
  const std::uint8_t selector = input.Next();
  switch (selector % 3U) {
    case 0:
      return "NULL";
    case 1:
      return std::to_string(static_cast<std::int16_t>(selector) - 128);
    default:
      return "'fuzz-" + std::to_string(input.Next()) + "'";
  }
}

[[nodiscard]] std::string GeneratedCore(ByteReader& input, bool allow_values) {
  const std::uint8_t selector = input.Next();
  if (selector % 3U == 0U) {
    return "SELECT " + std::string{selector % 2U == 0U ? "DISTINCT " : ""} +
           GeneratedLiteral(input);
  }
  if (selector % 3U == 1U && allow_values) {
    std::string sql = "VALUES";
    const std::size_t rows = 1U + input.Next() % 3U;
    for (std::size_t row = 0; row < rows; ++row) {
      sql += row == 0U ? "(" : ",(";
      sql += GeneratedLiteral(input);
      sql += ')';
    }
    return sql;
  }
  return "SELECT " + std::string{selector % 2U == 0U ? "DISTINCT " : ""} +
         "value FROM model_rows WHERE id<=" + std::to_string(1U + input.Next() % 32U);
}

[[nodiscard]] std::string_view GeneratedOperator(ByteReader& input) noexcept {
  switch (input.Next() % 4U) {
    case 0:
      return " UNION ";
    case 1:
      return " UNION ALL ";
    case 2:
      return " EXCEPT ";
    default:
      return " INTERSECT ";
  }
}

[[nodiscard]] std::string GenerateCompoundSql(std::span<const std::uint8_t> bytes) {
  ByteReader input{bytes};
  const std::size_t core_count = 2U + input.Next() % 4U;
  std::string sql = GeneratedCore(input, true);
  for (std::size_t core = 1; core < core_count; ++core) {
    sql += GeneratedOperator(input);
    sql += GeneratedCore(input, core + 1U < core_count);
  }
  if (input.Next() % 2U != 0U) {
    sql += " ORDER BY 1";
    if (input.Next() % 3U == 0U) {
      sql += " COLLATE BINARY";
    }
    if (input.Next() % 2U != 0U) {
      sql += " DESC";
    }
    if (input.Next() % 2U != 0U) {
      sql += input.Next() % 2U == 0U ? " NULLS FIRST" : " NULLS LAST";
    }
  }
  if (input.Next() % 2U != 0U) {
    const auto limit = static_cast<std::int64_t>(input.Next() % 9U) - 1;
    sql += " LIMIT " + std::to_string(limit);
    if (input.Next() % 2U != 0U) {
      sql += " OFFSET " + std::to_string(input.Next() % 6U);
    }
  }
  return sql;
}

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

std::string GenerateReadSqlInput(std::span<const std::uint8_t> input) {
  return GenerateCompoundSql(input);
}

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
  const std::string generated = GenerateReadSqlInput(input);
  ConsumePrepared(session.Prepare(Utf8View{generated}));
}

}  // namespace modern_sqlite::fuzz
