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

[[nodiscard]] std::string GenerateMembershipSql(std::span<const std::uint8_t> bytes) {
  ByteReader input{bytes};
  static_cast<void>(input.Next());
  constexpr std::uint8_t kMembershipShapes = 8;
  const auto shape = input.Next() % kMembershipShapes;
  const std::string bound = std::to_string(1U + input.Next() % 8U);
  const std::string first = std::to_string(input.Next() % 8U);
  const std::string second = std::to_string(input.Next() % 8U);
  const std::string third = std::to_string(input.Next() % 8U);
  const std::string values = "(" + first + "," + second + "," + third + ")";
  const std::string source = " FROM model_rows WHERE id<=" + bound;
  switch (shape) {
    case 0:
      return "SELECT id,id IN (" + first + "," + second + ",NULL)" + source + " ORDER BY id";
    case 1:
      return "SELECT id NOT IN " + values + source;
    case 2:
      return "SELECT nullable IN (),nullable NOT IN ()" + source;
    case 3:
      return "SELECT label IN ('A' COLLATE NOCASE,'b','c')" + source;
    case 4:
      return "SELECT (SELECT o.nullable IN " + values + ") FROM model_rows o WHERE o.id<=" + bound;
    case 5:
      return "SELECT value IN " + values + " AS matched" + source + " AND matched ORDER BY matched";
    case 6:
      return "SELECT value IN " + values + ",count()" + source + " GROUP BY 1 ORDER BY 1";
    default:
      return "SELECT id IN (" + first + "," + second + ",NULL)" + source + " UNION SELECT id IN " +
             values + source;
  }
}

[[nodiscard]] std::string GenerateSubquerySql(std::span<const std::uint8_t> bytes) {
  ByteReader input{bytes};
  static_cast<void>(input.Next());
  bool exists = input.Next() % 2U != 0U;
  std::string child;
  const auto kind = input.Next() % 14U;
  if (kind >= 10U) {
    const std::string bound = std::to_string(1U + input.Next() % 4U);
    switch (kind) {
      case 10:
        return "SELECT 1 AS outer_value FROM model_rows o WHERE o.id<=" + bound +
               " AND (SELECT (SELECT outer_value))=1 ORDER BY o.id";
      case 11:
        return "SELECT coalesce(o.nullable,o.value) AS outer_value FROM model_rows o "
               "WHERE o.id<=" +
               bound + " AND (SELECT outer_value) IS NOT NULL ORDER BY (SELECT outer_value),o.id";
      case 12:
        return "SELECT id AS outer_value FROM model_rows a FULL JOIN model_rows b USING(id) "
               "WHERE (SELECT outer_value)<=" +
               bound + " ORDER BY outer_value";
      default:
        return "SELECT o.nullable IS NULL AS outer_value,count(*) FROM model_rows o WHERE o.id<=" +
               bound +
               " GROUP BY o.nullable IS NULL HAVING (SELECT outer_value)>=0 "
               "ORDER BY outer_value";
    }
  }
  switch (kind) {
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
    case 3:
      child = GenerateJoinSql(bytes);
      exists = true;
      break;
    case 4:
      child = "SELECT i.label FROM model_rows i WHERE i.id>=o.id+" +
              std::to_string(input.Next() % 3U) + " ORDER BY i.id LIMIT " +
              std::to_string(input.Next() % 3U) + " OFFSET " + std::to_string(input.Next() % 3U);
      break;
    case 5:
      child = "VALUES(o.value),(o.nullable)";
      break;
    case 6:
      child =
          "SELECT count(*) FROM model_rows i WHERE i.nullable IS o.nullable "
          "GROUP BY i.id%3 ORDER BY 1";
      break;
    case 7:
      child = "SELECT (SELECT o.label) UNION ALL SELECT o.label ORDER BY 1";
      break;
    case 8:
      child =
          "SELECT b.label||o.label FROM model_rows a LEFT JOIN model_rows b "
          "ON b.id=a.id AND b.id<=o.id WHERE a.id<=3 ORDER BY a.id";
      break;
    default:
      child = "SELECT o.nullable FROM model_rows i WHERE i.id<=3 ORDER BY (SELECT o.value)";
      exists = true;
      break;
  }
  std::string sql = "SELECT " + std::string{exists ? "EXISTS(" : "("} + child + ")";
  const std::size_t nesting = input.Next() % 3U;
  for (std::size_t depth = 0; depth < nesting; ++depth) {
    sql = "SELECT (" + sql + ")";
  }
  if (kind >= 4U && input.Next() % 2U != 0U) {
    return sql + " AS captured_value FROM model_rows o WHERE o.id<=" +
           std::to_string(1U + input.Next() % 4U) +
           " AND captured_value IS NOT NULL ORDER BY captured_value,o.id";
  }
  return sql + (kind >= 4U ? " FROM model_rows o WHERE id<=" : " FROM model_rows WHERE id<=") +
         std::to_string(1U + input.Next() % 4U);
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
  constexpr std::array<std::string_view, 6> kOperations{
      "=", " IS ", ">", ">=", "<", "<=",
  };
  const std::string_view operation = kOperations[input.Next() % kOperations.size()];
  constexpr std::array<std::string_view, 5> kKeys{
      "b.text_key", "b.numeric_key", "b.text_key", "b.default_text", "b.numeric_key",
  };
  constexpr std::array<std::string_view, 5> kValues{
      "p.text_key", "p.numeric_key", "p.raw_key", "p.text_key", "p.numeric_key",
  };
  const std::size_t key = input.Next() % kKeys.size();
  std::string predicate = key == 4U ? "b.text_key IS p.text_key AND " : "";
  predicate += std::string{kKeys[key]} + std::string{operation} + std::string{kValues[key]};
  if (input.Next() % 2U != 0U) {
    predicate += " AND b.numeric_key IS p.numeric_key";
  }
  predicate += " AND b.id>" + std::to_string(input.Next() % 13U);
  const std::string source = " FROM probe_rows p " + std::string{join} + " key_rows b ON " +
                             predicate +
                             " WHERE p.id IS NULL OR p.id<=" + std::to_string(input.Next() % 7U);
  std::string sql;
  switch (input.Next() % 6U) {
    case 0:
      sql = "SELECT p.id,b.id,b.numeric_key" + source + " ORDER BY p.id,b.id";
      break;
    case 1:
      sql = "SELECT b.numeric_key,count(),count(b.rowid)" + source +
            " GROUP BY b.numeric_key ORDER BY b.numeric_key";
      break;
    case 2:
      sql = "SELECT DISTINCT b.id" + source + " ORDER BY b.id";
      break;
    case 3:
      sql = "SELECT p.id,b.id,b.payload,b.default_text,typeof(b.real_key),b.real_key" + source +
            " ORDER BY p.id,b.id";
      break;
    case 4:
      sql = "SELECT b.numeric_key,count(b.payload),group_concat(b.payload ORDER BY b.id DESC)" +
            source + " GROUP BY b.numeric_key ORDER BY b.numeric_key";
      break;
    default:
      sql = "SELECT DISTINCT b.payload COLLATE BINARY" + source + " ORDER BY b.payload";
      break;
  }
  sql += " LIMIT " + std::to_string(input.Next() % 13U) + " OFFSET " +
         std::to_string(input.Next() % 4U);
  return input.Next() % 4U == 0U ? "SELECT EXISTS(" + sql + ")" : sql;
}

[[nodiscard]] std::string GenerateAutomaticJoinSql(std::span<const std::uint8_t> bytes) {
  ByteReader input{bytes};
  static_cast<void>(input.Next());
  constexpr std::array<std::string_view, 4> kJoins{" JOIN ", " LEFT JOIN ", " CROSS JOIN ", ","};
  constexpr std::array<std::string_view, 6> kPredicates{
      "b.value=a.value",    "b.nullable IS a.nullable",      "b.label=a.label COLLATE NOCASE",
      "b.value=a.nullable", "b.label=a.label COLLATE RTRIM", "b.nullable IS NULL",
  };
  const std::size_t join = input.Next() % kJoins.size();
  const std::string_view predicate = kPredicates[input.Next() % kPredicates.size()];
  const std::string bound = "a.id<=" + std::to_string(1U + input.Next() % 8U);
  const std::size_t consumer = input.Next() % 6U;
  std::string source = " FROM model_rows a" + std::string{kJoins[join]} + "model_rows b";
  if (join < 2U) {
    source += " ON " + std::string{predicate};
  }
  if (consumer == 3U) {
    source += " LEFT JOIN model_rows c ON c.value=b.value";
  }
  source += " WHERE " + (join < 2U ? bound : std::string{predicate} + " AND " + bound);
  const std::string limit = " LIMIT " + std::to_string(input.Next() % 13U) + " OFFSET " +
                            std::to_string(input.Next() % 4U);
  switch (consumer) {
    case 0:
      return "SELECT a.id,b.id,b.label" + source + " ORDER BY a.id,b.id" + limit;
    case 1:
      return "SELECT a.id,count(b.id),sum(b.value),group_concat(b.label ORDER BY b.id DESC)" +
             source + " GROUP BY a.id ORDER BY a.id" + limit;
    case 2:
      return "SELECT DISTINCT b.nullable,b.label" + source + " ORDER BY 1,2" + limit;
    case 3:
      return "SELECT a.id,b.id,c.id,b.label,c.label" + source + " ORDER BY a.id,b.id,c.id" + limit;
    case 4:
      return "SELECT (SELECT b.label" + source + " ORDER BY a.id,b.id" + limit + ")";
    default:
      return "SELECT EXISTS(SELECT b.label" + source + limit + ")";
  }
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
  if (selected % 8U == 7U) {
    return GenerateAutomaticJoinSql(input);
  }
  if (selected % 8U == 3U) {
    return GenerateSubquerySql(input);
  }
  if (selected % 8U == 6U) {
    return GenerateMembershipSql(input);
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
