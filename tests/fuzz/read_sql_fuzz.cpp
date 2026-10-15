#include <array>
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
constexpr std::size_t kModelRowCount = 32;
constexpr std::uint8_t kAggregateSelectionDivisor = 4;

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

[[nodiscard]] std::string_view GeneratedDirection(ByteReader& input) noexcept {
  return input.Next() % 2U == 0U ? "ASC" : "DESC";
}

[[nodiscard]] std::string GenerateAggregateSql(std::span<const std::uint8_t> bytes) {
  ByteReader input{bytes};
  static_cast<void>(input.Next());
  const bool grouped = input.Next() % 2U != 0U;
  const std::size_t source_limit = 1U + input.Next() % kModelRowCount;
  const auto filter_floor = static_cast<std::int16_t>(input.Next()) - 128;
  const std::string direction{GeneratedDirection(input)};

  if (!grouped) {
    return "SELECT count(),sum(value),total(value),avg(value),min(label),max(label),"
           "group_concat(label ORDER BY id " +
           direction + ") FILTER (WHERE value>=" + std::to_string(filter_floor) +
           "),group_concat(DISTINCT nullable ORDER BY nullable " + direction +
           ") FROM model_rows WHERE id<=" + std::to_string(source_limit);
  }

  const std::size_t having_limit = input.Next() % 8U;
  std::string sql =
      "SELECT nullable IS NULL,count(),min(value),max(value),"
      "group_concat(label ORDER BY id " +
      direction + ") FILTER (WHERE value>=" + std::to_string(filter_floor) +
      "),group_concat(DISTINCT value<0 ORDER BY id " + direction +
      ") FROM model_rows WHERE id<=" + std::to_string(source_limit) +
      " GROUP BY nullable IS NULL HAVING count()>" + std::to_string(having_limit) + " ORDER BY 1";
  if (input.Next() % 2U != 0U) {
    sql += " LIMIT " + std::to_string(1U + input.Next() % 3U);
    if (input.Next() % 2U != 0U) {
      sql += " OFFSET " + std::to_string(input.Next() % 3U);
    }
  }
  return sql;
}

[[nodiscard]] std::string GenerateJoinSql(std::span<const std::uint8_t> bytes) {
  ByteReader input{bytes};
  static_cast<void>(input.Next());
  const bool cross = input.Next() % 2U == 0U;
  const bool distinct = input.Next() % 2U != 0U;
  const std::size_t left_count = 1U + input.Next() % 8U;
  const std::size_t right_count = 1U + input.Next() % 8U;
  if (input.Next() % 4U == 0U) {
    return "SELECT id,a.id,b.id FROM model_rows a NATURAL FULL JOIN model_rows b "
           "WHERE id<=" +
           std::to_string(left_count) + " ORDER BY id,a.id,b.id LIMIT " +
           std::to_string(input.Next() % 9U);
  }
  if (input.Next() % 4U == 0U) {
    return "SELECT a.id,b.id,a.nullable FROM model_rows a RIGHT JOIN model_rows b "
           "ON a.id=b.id AND a.id<=" +
           std::to_string(left_count) + " WHERE b.id<=" + std::to_string(right_count) +
           " ORDER BY b.id,a.id LIMIT " + std::to_string(input.Next() % 9U);
  }
  if (input.Next() % 4U == 0U) {
    const bool natural = input.Next() % 2U != 0U;
    return "SELECT id,a.id,b.id FROM model_rows a" +
           std::string{natural ? " NATURAL LEFT JOIN " : " LEFT JOIN "} + "model_rows b" +
           (natural ? "" : " USING(id)") + " WHERE id<=" + std::to_string(left_count) +
           " ORDER BY id LIMIT " + std::to_string(input.Next() % 9U);
  }
  if (input.Next() % 4U == 0U) {
    return "SELECT a.id,b.id,b.nullable FROM model_rows a LEFT JOIN model_rows b "
           "ON b.id=a.id AND b.id<=" +
           std::to_string(right_count) + " WHERE a.id<=" + std::to_string(left_count) +
           " ORDER BY a.id,b.id LIMIT " + std::to_string(input.Next() % 9U);
  }
  if (input.Next() % 4U == 0U) {
    return "SELECT b.id,count(DISTINCT a.value),max(a.oid),"
           "group_concat(a.label ORDER BY a.id DESC) FILTER (WHERE a.nullable IS NOT NULL) "
           "FROM model_rows a" +
           std::string{cross ? " CROSS JOIN " : ","} +
           "model_rows b WHERE a.id<=" + std::to_string(left_count) +
           " AND b.id<=" + std::to_string(right_count) +
           " GROUP BY b.id ORDER BY b.id DESC LIMIT " + std::to_string(input.Next() % 9U);
  }
  std::string sql =
      "SELECT " + std::string{distinct ? "DISTINCT " : ""} +
      "a.id,b.id,a.nullable+b.nullable FROM model_rows a" + (cross ? " CROSS JOIN " : ",") +
      "model_rows b WHERE a.id<=" + std::to_string(left_count) +
      " AND b.id<=" + std::to_string(right_count) + " AND a.value<=b.value ORDER BY 1 DESC,2";
  if (input.Next() % 2U != 0U) {
    sql += " LIMIT " + std::to_string(input.Next() % 9U) + " OFFSET " +
           std::to_string(input.Next() % 5U);
  }
  return sql;
}

[[nodiscard]] std::string GenerateSubquerySql(std::span<const std::uint8_t> bytes) {
  ByteReader input{bytes};
  static_cast<void>(input.Next());
  bool exists = input.Next() % 2U != 0U;
  std::string child;
  switch (input.Next() % 4U) {
    case 0:
      child = GeneratedCore(input, true);
      break;
    case 1:
      child = GenerateCompoundSql(bytes);
      break;
    case 2:
      child = GenerateAggregateSql(bytes);
      exists = true;
      break;
    default:
      child = GenerateJoinSql(bytes);
      exists = true;
      break;
  }
  std::string sql = "SELECT " + std::string{exists ? "EXISTS(" : "("} + child + ")";
  const std::size_t nesting = input.Next() % 3U;
  for (std::size_t depth = 0; depth < nesting; ++depth) {
    sql = "SELECT (" + sql + ")";
  }
  return sql + " FROM model_rows WHERE id<=" + std::to_string(1U + input.Next() % 4U);
}

[[nodiscard]] bool IsJoinedIndexInput(std::span<const std::uint8_t> input) {
  return !input.empty() && input.front() % 8U == 5U;
}

[[nodiscard]] std::string GenerateJoinedIndexSql(std::span<const std::uint8_t> bytes) {
  ByteReader input{bytes};
  static_cast<void>(input.Next());
  constexpr std::array<std::string_view, 4> kJoins{
      "JOIN",
      "LEFT JOIN",
      "RIGHT JOIN",
      "FULL JOIN",
  };
  const std::string_view join = kJoins[input.Next() % kJoins.size()];
  const std::string_view operation = input.Next() % 2U == 0U ? "=" : " IS ";
  constexpr std::array<std::string_view, 4> kKeys{
      "b.text_key",
      "b.numeric_key",
      "b.text_key",
      "b.default_text",
  };
  constexpr std::array<std::string_view, 4> kValues{
      "p.text_key",
      "p.numeric_key",
      "p.raw_key",
      "p.text_key",
  };
  const std::size_t key = input.Next() % kKeys.size();
  std::string predicate =
      std::string{kKeys[key]} + std::string{operation} + std::string{kValues[key]};
  if (input.Next() % 2U != 0U) {
    predicate += " AND b.numeric_key IS p.numeric_key";
  }
  predicate += " AND b.id>" + std::to_string(input.Next() % 13U);
  const std::string source = " FROM probe_rows p " + std::string{join} + " key_rows b ON " +
                             predicate +
                             " WHERE p.id IS NULL OR p.id<=" + std::to_string(input.Next() % 7U);
  std::string sql;
  switch (input.Next() % 3U) {
    case 0:
      sql = "SELECT p.id,b.id,b.numeric_key" + source + " ORDER BY p.id,b.id";
      break;
    case 1:
      sql = "SELECT b.numeric_key,count(),count(b.rowid)" + source +
            " GROUP BY b.numeric_key ORDER BY b.numeric_key";
      break;
    default:
      sql = "SELECT DISTINCT b.id" + source + " ORDER BY b.id";
      break;
  }
  sql += " LIMIT " + std::to_string(input.Next() % 13U) + " OFFSET " +
         std::to_string(input.Next() % 4U);
  return input.Next() % 4U == 0U ? "SELECT EXISTS(" + sql + ")" : sql;
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
  if (IsJoinedIndexInput(input)) {
    return GenerateJoinedIndexSql(input);
  }
  ByteReader selector{input};
  const std::uint8_t selected = selector.Next();
  if (selected % 8U == 3U) {
    return GenerateSubquerySql(input);
  }
  if (selected % kAggregateSelectionDivisor == 0U) {
    return GenerateAggregateSql(input);
  }
  if (selected % kAggregateSelectionDivisor == 1U) {
    return GenerateJoinSql(input);
  }
  return GenerateCompoundSql(input);
}

void RunReadSqlInput(std::span<const std::uint8_t> input, std::string_view database_path,
                     std::string_view joined_index_path) {
  if (input.size() > kMaximumSqlBytes) {
    return;
  }
  Result<ReadSession> opened =
      ReadSession::Open(IsJoinedIndexInput(input) ? joined_index_path : database_path);
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
