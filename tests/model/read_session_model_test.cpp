#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {
namespace {

constexpr std::uint64_t kModelSeed = UINT64_C(0x4d53514c52454144);
constexpr std::size_t kModelIterations = 64;
constexpr std::string_view kGeneratorVersion = "splitmix64-v1";

struct ModelRow {
  std::int64_t id = 0;
  std::int64_t value = 0;
  std::string_view label;
  std::optional<std::int64_t> nullable = std::nullopt;
};

constexpr std::array<std::int64_t, 32> kValues{
    -31, -29, -23, -19, -17, -13, -11, -7, -5, -3, -2, -1, 0,  1,  2,  3,
    5,   7,   11,  13,  17,  19,  23,  29, 31, 37, 41, 43, 47, 53, 59, 61,
};

constexpr std::array<std::string_view, 32> kLabels{
    "row-01", "row-02", "row-03", "row-04", "row-05", "row-06", "row-07", "row-08",
    "row-09", "row-10", "row-11", "row-12", "row-13", "row-14", "row-15", "row-16",
    "row-17", "row-18", "row-19", "row-20", "row-21", "row-22", "row-23", "row-24",
    "row-25", "row-26", "row-27", "row-28", "row-29", "row-30", "row-31", "row-32",
};

[[nodiscard]] constexpr std::array<ModelRow, 32> MakeRows() {
  std::array<ModelRow, 32> rows{};
  std::int64_t id = 1;
  for (std::size_t index = 0; index < rows.size(); ++index, ++id) {
    rows[index] = ModelRow{
        .id = id,
        .value = kValues[index],
        .label = kLabels[index],
        .nullable = id % 3 == 1 ? std::nullopt : std::optional<std::int64_t>{id * 10},
    };
  }
  return rows;
}

constexpr std::array<ModelRow, 32> kRows = MakeRows();

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t Next() noexcept {
    state_ += UINT64_C(0x9e3779b97f4a7c15);
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

void RequireStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path() / "fixtures" /
         "read_compatibility" / "sqlite-3.54.0-read-compatibility.db";
}

[[nodiscard]] ReadStatement Prepare(ReadSession& session, std::string_view sql) {
  ReadPrepareOutput prepared = TakeValue(session.Prepare(Utf8View{sql}));
  if (!prepared.statement.has_value()) {
    throw std::runtime_error{"model SQL produced no statement"};
  }
  return std::move(*prepared.statement);
}

void ExpectInteger(const SqlValue& value, std::int64_t expected) {
  ASSERT_EQ(SqlValueType::kInteger, value.type());
  ASSERT_TRUE(value.integer_value().has_value());
  EXPECT_EQ(expected, *value.integer_value());
}

void ExpectText(const SqlValue& value, std::string_view expected) {
  ASSERT_EQ(SqlValueType::kText, value.type());
  ASSERT_TRUE(value.text_value().has_value());
  EXPECT_EQ(expected, value.text_value()->bytes());
}

void ExpectNullable(const SqlValue& value, std::optional<std::int64_t> expected) {
  if (!expected.has_value()) {
    EXPECT_EQ(SqlValueType::kNull, value.type());
    return;
  }
  ExpectInteger(value, *expected);
}

void ExpectRowStep(ReadStatement& statement) {
  const Result<ReadStep> step = statement.Step();
  if (!step.has_value()) {
    throw std::runtime_error(step.error().ToString());
  }
  if (*step != ReadStep::kRow) {
    throw std::runtime_error{"model expected a row"};
  }
}

void ExpectDoneStep(ReadStatement& statement) {
  const Result<ReadStep> step = statement.Step();
  if (!step.has_value()) {
    throw std::runtime_error(step.error().ToString());
  }
  if (*step != ReadStep::kDone) {
    throw std::runtime_error{"model expected statement completion"};
  }
}

[[nodiscard]] std::string Trace(std::size_t case_index, std::string_view model_template,
                                const std::array<std::uint64_t, 4>& words,
                                std::string_view bindings) {
  std::ostringstream output;
  output << "generator=" << kGeneratorVersion << " seed=0x" << std::hex << kModelSeed << std::dec
         << " case=" << case_index << " template=" << model_template << " words=[";
  for (std::size_t index = 0; index < words.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << "0x" << std::hex << words[index] << std::dec;
  }
  output << "] bindings=" << bindings;
  return output.str();
}

void RunPredicateScan(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const auto threshold = static_cast<std::int64_t>(words[1] % 73U) - 36;
  const auto limit = static_cast<std::size_t>(words[2] % 8U);
  const auto offset = static_cast<std::size_t>(words[3] % 5U);
  ReadStatement statement = Prepare(session,
                                    "SELECT id, value, label, nullable FROM model_rows "
                                    "WHERE value>=?1 LIMIT ?2 OFFSET ?3");
  RequireStatus(statement.Bind(1, SqlValue::Integer(threshold)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(static_cast<std::int64_t>(limit))));
  RequireStatus(statement.Bind(3, SqlValue::Integer(static_cast<std::int64_t>(offset))));

  std::vector<const ModelRow*> matches;
  for (const ModelRow& row : kRows) {
    if (row.value >= threshold) {
      matches.push_back(&row);
    }
  }
  const std::size_t begin = std::min(offset, matches.size());
  const std::size_t end = std::min(matches.size(), begin + limit);
  for (std::size_t index = begin; index < end; ++index) {
    ExpectRowStep(statement);
    ASSERT_EQ(4U, statement.row().size());
    ExpectInteger(statement.row()[0], matches[index]->id);
    ExpectInteger(statement.row()[1], matches[index]->value);
    ExpectText(statement.row()[2], matches[index]->label);
    ExpectNullable(statement.row()[3], matches[index]->nullable);
  }
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

void RunRowIdLookup(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const auto rowid = static_cast<std::int64_t>(words[1] % 40U);
  ReadStatement statement =
      Prepare(session, "SELECT value, label, nullable FROM model_rows WHERE rowid=?1");
  RequireStatus(statement.Bind(1, SqlValue::Integer(rowid)));
  if (rowid >= 1 && std::cmp_less_equal(rowid, kRows.size())) {
    const ModelRow& expected = kRows[static_cast<std::size_t>(rowid - 1)];
    ExpectRowStep(statement);
    ASSERT_EQ(3U, statement.row().size());
    ExpectInteger(statement.row()[0], expected.value);
    ExpectText(statement.row()[1], expected.label);
    ExpectNullable(statement.row()[2], expected.nullable);
  }
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

using ModelValue = std::variant<std::monostate, std::int64_t, std::string>;

struct GeneratedValueInput {
  std::uint64_t selector;
  std::uint64_t payload;
};

[[nodiscard]] ModelValue GeneratedValue(GeneratedValueInput input) {
  switch (input.selector % 3U) {
    case 0:
      return std::monostate{};
    case 1:
      return static_cast<std::int64_t>(input.payload % 2001U) - 1000;
    default:
      return "model-" + std::to_string(input.payload % 1000U);
  }
}

[[nodiscard]] std::string DescribeValue(const ModelValue& value) {
  if (std::holds_alternative<std::monostate>(value)) {
    return "null";
  }
  if (const auto* integer = std::get_if<std::int64_t>(&value); integer != nullptr) {
    return "integer:" + std::to_string(*integer);
  }
  return "text:" + std::get<std::string>(value);
}

[[nodiscard]] std::string DescribeBindings(std::size_t model_template,
                                           const std::array<std::uint64_t, 4>& words) {
  std::ostringstream output;
  switch (model_template) {
    case 0:
      output << "threshold=" << static_cast<std::int64_t>(words[1] % 73U) - 36
             << ",limit=" << words[2] % 8U << ",offset=" << words[3] % 5U;
      break;
    case 1:
      output << "rowid=" << words[1] % 40U;
      break;
    case 2:
      output << "first="
             << DescribeValue(GeneratedValue({.selector = words[1], .payload = words[3]}))
             << ",second="
             << DescribeValue(GeneratedValue({.selector = words[2], .payload = words[0]}));
      break;
    case 3:
      output << "limit=" << (words[1] % 5U == 0U ? -1 : static_cast<std::int64_t>(words[1] % 9U))
             << ",offset=" << (words[2] % 4U == 0U ? -2 : static_cast<std::int64_t>(words[2] % 8U));
      break;
    case 4:
      output << "first_rowid=" << 1U + words[1] % kRows.size()
             << ",second_rowid=" << 1U + words[2] % kRows.size();
      break;
    case 5:
      output << "threshold=" << static_cast<std::int64_t>(words[1] % 73U) - 36
             << ",direction=" << (words[2] % 2U == 0U ? "ascending" : "descending")
             << ",nulls=" << (words[3] % 2U == 0U ? "first" : "last");
      break;
    case 6:
      output << "limit=" << (words[1] % 5U == 0U ? -1 : static_cast<std::int64_t>(words[1] % 9U))
             << ",offset=" << words[2] % 6U
             << ",direction=" << (words[2] % 2U == 0U ? "ascending" : "descending")
             << ",nulls=" << (words[3] % 2U == 0U ? "first" : "last");
      break;
    case 7:
      output << "values=[" << static_cast<std::int64_t>(words[0] % 11U) - 5 << ','
             << static_cast<std::int64_t>(words[1] % 11U) - 5 << ','
             << static_cast<std::int64_t>(words[2] % 11U) - 5 << "],operator=" << words[3] % 4U
             << ",cutoff=" << 1U + words[1] % kRows.size()
             << ",limit=" << (words[2] % 5U == 0U ? -1 : static_cast<std::int64_t>(words[2] % 9U))
             << ",offset=" << words[3] % 5U;
      break;
    case 8:
      output << "left_cutoff=" << 1U + words[1] % kRows.size()
             << ",right_begin=" << 1U + words[2] % kRows.size() << ",operators=[" << words[1] % 4U
             << ',' << words[3] % 4U << ']';
      break;
    case 9:
      output << "limit=" << (words[1] % 5U == 0U ? -1 : static_cast<std::int64_t>(words[1] % 9U))
             << ",offset=" << words[2] % 6U;
      break;
    default:
      output << "unreachable";
      break;
  }
  return output.str();
}

void BindValue(ReadStatement& statement, const ModelValue& value) {
  if (std::holds_alternative<std::monostate>(value)) {
    RequireStatus(statement.Bind(1, SqlValue{}));
  } else if (const auto* integer = std::get_if<std::int64_t>(&value); integer != nullptr) {
    RequireStatus(statement.Bind(1, SqlValue::Integer(*integer)));
  } else {
    RequireStatus(statement.Bind(1, SqlValue::Text(std::get<std::string>(value))));
  }
}

void ExpectValue(const SqlValue& actual, const ModelValue& expected) {
  if (std::holds_alternative<std::monostate>(expected)) {
    EXPECT_EQ(SqlValueType::kNull, actual.type());
  } else if (const auto* integer = std::get_if<std::int64_t>(&expected); integer != nullptr) {
    ExpectInteger(actual, *integer);
  } else {
    ExpectText(actual, std::get<std::string>(expected));
  }
}

void RunTypedRebinding(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const ModelValue first = GeneratedValue({.selector = words[1], .payload = words[3]});
  const ModelValue second = GeneratedValue({.selector = words[2], .payload = words[0]});
  ReadStatement statement = Prepare(session, "SELECT ?1 AS value");

  BindValue(statement, first);
  ExpectRowStep(statement);
  ASSERT_EQ(1U, statement.row().size());
  ExpectValue(statement.row()[0], first);
  RequireStatus(statement.Reset());

  BindValue(statement, second);
  ExpectRowStep(statement);
  ASSERT_EQ(1U, statement.row().size());
  ExpectValue(statement.row()[0], second);
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

void RunLimitOffset(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const std::int64_t limit = words[1] % 5U == 0U ? -1 : static_cast<std::int64_t>(words[1] % 9U);
  const std::int64_t offset = words[2] % 4U == 0U ? -2 : static_cast<std::int64_t>(words[2] % 8U);
  ReadStatement statement = Prepare(session, "SELECT id, value FROM model_rows LIMIT ?1 OFFSET ?2");
  RequireStatus(statement.Bind(1, SqlValue::Integer(limit)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(offset)));

  const std::size_t begin = static_cast<std::size_t>(std::max<std::int64_t>(0, offset));
  const std::size_t available = begin < kRows.size() ? kRows.size() - begin : 0U;
  const std::size_t count =
      limit < 0 ? available : std::min(available, static_cast<std::size_t>(limit));
  for (std::size_t index = begin; index < begin + count; ++index) {
    ExpectRowStep(statement);
    ASSERT_EQ(2U, statement.row().size());
    ExpectInteger(statement.row()[0], kRows[index].id);
    ExpectInteger(statement.row()[1], kRows[index].value);
  }
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

void ExpectModelRow(ReadStatement& statement, const ModelRow& expected) {
  ExpectRowStep(statement);
  ASSERT_EQ(3U, statement.row().size());
  ExpectInteger(statement.row()[0], expected.id);
  ExpectText(statement.row()[1], expected.label);
  ExpectNullable(statement.row()[2], expected.nullable);
}

void RunRepeatedExecution(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const auto first_index = static_cast<std::size_t>(words[1] % kRows.size());
  const auto second_index = static_cast<std::size_t>(words[2] % kRows.size());
  ReadStatement statement =
      Prepare(session, "SELECT id, label, nullable FROM model_rows WHERE rowid=?1");
  RequireStatus(statement.Bind(1, SqlValue::Integer(kRows[first_index].id)));
  ExpectModelRow(statement, kRows[first_index]);
  ExpectDoneStep(statement);
  ExpectModelRow(statement, kRows[first_index]);

  RequireStatus(statement.Reset());
  RequireStatus(statement.Bind(1, SqlValue::Integer(kRows[second_index].id)));
  ExpectModelRow(statement, kRows[second_index]);
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

void RunOrderedScan(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const auto threshold = static_cast<std::int64_t>(words[1] % 73U) - 36;
  const bool descending = words[2] % 2U != 0U;
  const bool nulls_first = words[3] % 2U == 0U;
  const std::string sql =
      "SELECT id, value, label, nullable FROM model_rows WHERE value>=?1 ORDER BY nullable " +
      std::string{descending ? "DESC" : "ASC"} + (nulls_first ? " NULLS FIRST" : " NULLS LAST") +
      ", value DESC, id";
  ReadStatement statement = Prepare(session, sql);
  RequireStatus(statement.Bind(1, SqlValue::Integer(threshold)));

  std::vector<const ModelRow*> matches;
  for (const ModelRow& row : kRows) {
    if (row.value >= threshold) {
      matches.push_back(&row);
    }
  }
  std::ranges::sort(matches, [=](const ModelRow* left, const ModelRow* right) {
    if (left->nullable.has_value() != right->nullable.has_value()) {
      return left->nullable.has_value() != nulls_first;
    }
    if (left->nullable != right->nullable) {
      return descending ? left->nullable > right->nullable : left->nullable < right->nullable;
    }
    if (left->value != right->value) {
      return left->value > right->value;
    }
    return left->id < right->id;
  });

  for (const ModelRow* expected : matches) {
    ExpectRowStep(statement);
    ASSERT_EQ(4U, statement.row().size());
    ExpectInteger(statement.row()[0], expected->id);
    ExpectInteger(statement.row()[1], expected->value);
    ExpectText(statement.row()[2], expected->label);
    ExpectNullable(statement.row()[3], expected->nullable);
  }
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

void RunOrderedLimit(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const std::int64_t limit = words[1] % 5U == 0U ? -1 : static_cast<std::int64_t>(words[1] % 9U);
  const auto offset = static_cast<std::size_t>(words[2] % 6U);
  const bool descending = words[2] % 2U != 0U;
  const bool nulls_first = words[3] % 2U == 0U;
  const std::string sql = "SELECT id, value, label, nullable FROM model_rows ORDER BY nullable " +
                          std::string{descending ? "DESC" : "ASC"} +
                          (nulls_first ? " NULLS FIRST" : " NULLS LAST") +
                          ", value DESC, id LIMIT ?1 OFFSET ?2";
  ReadStatement statement = Prepare(session, sql);
  RequireStatus(statement.Bind(1, SqlValue::Integer(limit)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(static_cast<std::int64_t>(offset))));

  std::vector<const ModelRow*> ordered;
  ordered.reserve(kRows.size());
  for (const ModelRow& row : kRows) {
    ordered.push_back(&row);
  }
  std::ranges::sort(ordered, [=](const ModelRow* left, const ModelRow* right) {
    if (left->nullable.has_value() != right->nullable.has_value()) {
      return left->nullable.has_value() != nulls_first;
    }
    if (left->nullable != right->nullable) {
      return descending ? left->nullable > right->nullable : left->nullable < right->nullable;
    }
    if (left->value != right->value) {
      return left->value > right->value;
    }
    return left->id < right->id;
  });

  const std::size_t begin = std::min(offset, ordered.size());
  const std::size_t available = ordered.size() - begin;
  const std::size_t count =
      limit < 0 ? available : std::min(available, static_cast<std::size_t>(limit));
  for (std::size_t index = begin; index < begin + count; ++index) {
    const ModelRow& expected = *ordered[index];
    ExpectRowStep(statement);
    ASSERT_EQ(4U, statement.row().size());
    ExpectInteger(statement.row()[0], expected.id);
    ExpectInteger(statement.row()[1], expected.value);
    ExpectText(statement.row()[2], expected.label);
    ExpectNullable(statement.row()[3], expected.nullable);
  }
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

enum class ModelCompoundOperator : std::uint8_t {
  kUnion,
  kUnionAll,
  kExcept,
  kIntersect,
};

[[nodiscard]] ModelCompoundOperator GeneratedCompoundOperator(std::uint64_t word) noexcept {
  return static_cast<ModelCompoundOperator>(word % 4U);
}

[[nodiscard]] std::string_view CompoundOperatorSql(ModelCompoundOperator operation) noexcept {
  switch (operation) {
    case ModelCompoundOperator::kUnion:
      return "UNION";
    case ModelCompoundOperator::kUnionAll:
      return "UNION ALL";
    case ModelCompoundOperator::kExcept:
      return "EXCEPT";
    case ModelCompoundOperator::kIntersect:
      return "INTERSECT";
  }
  return "UNION";
}

using NullableInteger = std::optional<std::int64_t>;

[[nodiscard]] bool ContainsValue(std::span<const NullableInteger> values,
                                 const NullableInteger& value) {
  return std::ranges::find(values, value) != values.end();
}

[[nodiscard]] std::vector<NullableInteger> UniqueValues(std::span<const NullableInteger> values) {
  std::vector<NullableInteger> unique;
  unique.reserve(values.size());
  for (const NullableInteger& value : values) {
    if (!ContainsValue(unique, value)) {
      unique.push_back(value);
    }
  }
  return unique;
}

[[nodiscard]] std::vector<NullableInteger> ApplyCompound(std::span<const NullableInteger> left,
                                                         std::span<const NullableInteger> right,
                                                         ModelCompoundOperator operation) {
  if (operation == ModelCompoundOperator::kUnionAll) {
    std::vector<NullableInteger> output{left.begin(), left.end()};
    output.insert(output.end(), right.begin(), right.end());
    return output;
  }
  const std::vector<NullableInteger> unique_left = UniqueValues(left);
  const std::vector<NullableInteger> unique_right = UniqueValues(right);
  std::vector<NullableInteger> output;
  switch (operation) {
    case ModelCompoundOperator::kUnion:
      output = unique_left;
      for (const NullableInteger& value : unique_right) {
        if (!ContainsValue(output, value)) {
          output.push_back(value);
        }
      }
      break;
    case ModelCompoundOperator::kExcept:
      for (const NullableInteger& value : unique_left) {
        if (!ContainsValue(unique_right, value)) {
          output.push_back(value);
        }
      }
      break;
    case ModelCompoundOperator::kIntersect:
      for (const NullableInteger& value : unique_left) {
        if (ContainsValue(unique_right, value)) {
          output.push_back(value);
        }
      }
      break;
    case ModelCompoundOperator::kUnionAll:
      break;
  }
  return output;
}

void SortModelValues(std::vector<NullableInteger>& values) {
  std::ranges::stable_sort(values, [](const NullableInteger& left, const NullableInteger& right) {
    if (left.has_value() != right.has_value()) {
      return !left.has_value();
    }
    return left.has_value() && right.has_value() && *left < *right;
  });
}

void ExpectNullableValue(const SqlValue& actual, const NullableInteger& expected) {
  if (!expected.has_value()) {
    EXPECT_EQ(SqlValueType::kNull, actual.type());
    return;
  }
  ExpectInteger(actual, *expected);
}

void RunValuesCompound(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const ModelCompoundOperator operation = GeneratedCompoundOperator(words[3]);
  const std::array values{
      static_cast<std::int64_t>(words[0] % 11U) - 5,
      static_cast<std::int64_t>(words[1] % 11U) - 5,
      static_cast<std::int64_t>(words[2] % 11U) - 5,
  };
  const auto cutoff = static_cast<std::int64_t>(1U + words[1] % kRows.size());
  const std::int64_t limit = words[2] % 5U == 0U ? -1 : static_cast<std::int64_t>(words[2] % 9U);
  const auto offset = static_cast<std::size_t>(words[3] % 5U);
  const std::string sql = "VALUES(?1),(?2),(?3) " + std::string{CompoundOperatorSql(operation)} +
                          " SELECT value FROM model_rows WHERE id<=?4 "
                          "ORDER BY 1 LIMIT ?5 OFFSET ?6";
  ReadStatement statement = Prepare(session, sql);
  for (std::size_t index = 0; index < values.size(); ++index) {
    RequireStatus(statement.Bind(index + 1U, SqlValue::Integer(values[index])));
  }
  RequireStatus(statement.Bind(4, SqlValue::Integer(cutoff)));
  RequireStatus(statement.Bind(5, SqlValue::Integer(limit)));
  RequireStatus(statement.Bind(6, SqlValue::Integer(static_cast<std::int64_t>(offset))));

  std::vector<NullableInteger> left;
  left.reserve(values.size());
  for (const std::int64_t value : values) {
    left.emplace_back(value);
  }
  std::vector<NullableInteger> right;
  for (const ModelRow& row : kRows) {
    if (row.id <= cutoff) {
      right.emplace_back(row.value);
    }
  }
  std::vector<NullableInteger> expected = ApplyCompound(left, right, operation);
  SortModelValues(expected);
  const std::size_t begin = std::min(offset, expected.size());
  const std::size_t available = expected.size() - begin;
  const std::size_t count =
      limit < 0 ? available : std::min(available, static_cast<std::size_t>(limit));
  for (std::size_t index = begin; index < begin + count; ++index) {
    ExpectRowStep(statement);
    ASSERT_EQ(1U, statement.row().size());
    ExpectNullableValue(statement.row()[0], expected[index]);
  }
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

void RunMixedTableCompoundCase(ReadSession& session, const std::array<std::uint64_t, 4>& words,
                               ModelCompoundOperator first_operation,
                               ModelCompoundOperator second_operation) {
  const auto left_cutoff = static_cast<std::int64_t>(1U + words[1] % kRows.size());
  const auto right_begin = static_cast<std::int64_t>(1U + words[2] % kRows.size());
  const std::string sql = "SELECT value FROM model_rows WHERE id<=?1 " +
                          std::string{CompoundOperatorSql(first_operation)} +
                          " SELECT value FROM model_rows WHERE id>=?2 " +
                          std::string{CompoundOperatorSql(second_operation)} +
                          " SELECT value FROM model_rows WHERE nullable IS NULL ORDER BY 1";
  ReadStatement statement = Prepare(session, sql);
  RequireStatus(statement.Bind(1, SqlValue::Integer(left_cutoff)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(right_begin)));

  std::vector<NullableInteger> left;
  std::vector<NullableInteger> right;
  std::vector<NullableInteger> third;
  for (const ModelRow& row : kRows) {
    if (row.id <= left_cutoff) {
      left.emplace_back(row.value);
    }
    if (row.id >= right_begin) {
      right.emplace_back(row.value);
    }
    if (!row.nullable.has_value()) {
      third.emplace_back(row.value);
    }
  }
  std::vector<NullableInteger> expected = ApplyCompound(left, right, first_operation);
  expected = ApplyCompound(expected, third, second_operation);
  SortModelValues(expected);
  for (const NullableInteger& value : expected) {
    ExpectRowStep(statement);
    ASSERT_EQ(1U, statement.row().size());
    ExpectNullableValue(statement.row()[0], value);
  }
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

void RunMixedTableCompound(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  RunMixedTableCompoundCase(session, words, GeneratedCompoundOperator(words[1]),
                            GeneratedCompoundOperator(words[3]));
}

void RunDistinctNullable(ReadSession& session, const std::array<std::uint64_t, 4>& words) {
  const std::int64_t limit = words[1] % 5U == 0U ? -1 : static_cast<std::int64_t>(words[1] % 9U);
  const auto offset = static_cast<std::size_t>(words[2] % 6U);
  ReadStatement statement =
      Prepare(session, "SELECT DISTINCT nullable FROM model_rows ORDER BY 1 LIMIT ?1 OFFSET ?2");
  RequireStatus(statement.Bind(1, SqlValue::Integer(limit)));
  RequireStatus(statement.Bind(2, SqlValue::Integer(static_cast<std::int64_t>(offset))));

  std::vector<NullableInteger> values;
  values.reserve(kRows.size());
  for (const ModelRow& row : kRows) {
    values.push_back(row.nullable);
  }
  std::vector<NullableInteger> expected = UniqueValues(values);
  SortModelValues(expected);
  const std::size_t begin = std::min(offset, expected.size());
  const std::size_t available = expected.size() - begin;
  const std::size_t count =
      limit < 0 ? available : std::min(available, static_cast<std::size_t>(limit));
  for (std::size_t index = begin; index < begin + count; ++index) {
    ExpectRowStep(statement);
    ASSERT_EQ(1U, statement.row().size());
    ExpectNullableValue(statement.row()[0], expected[index]);
  }
  ExpectDoneStep(statement);
  RequireStatus(statement.Finalize());
}

TEST(ReadSessionModel, MatchesDeterministicVectorModel) {
  ReadSession session = TakeValue(ReadSession::Open(FixturePath().string()));
  SplitMix64 random{kModelSeed};
  constexpr std::array<std::string_view, 10> templates{
      "predicate-scan", "rowid-lookup",  "typed-rebinding", "limit-offset",   "repeated-execution",
      "ordered-scan",   "ordered-limit", "values-compound", "mixed-compound", "distinct-nullable",
  };

  for (std::size_t case_index = 0; case_index < kModelIterations + 32U; ++case_index) {
    const std::array words{random.Next(), random.Next(), random.Next(), random.Next()};
    const auto selected = static_cast<std::size_t>(words[0] % templates.size());
    SCOPED_TRACE(Trace(case_index, templates[selected], words, DescribeBindings(selected, words)));
    switch (selected) {
      case 0:
        RunPredicateScan(session, words);
        break;
      case 1:
        RunRowIdLookup(session, words);
        break;
      case 2:
        RunTypedRebinding(session, words);
        break;
      case 3:
        RunLimitOffset(session, words);
        break;
      case 4:
        RunRepeatedExecution(session, words);
        break;
      case 5:
        RunOrderedScan(session, words);
        break;
      case 6:
        RunOrderedLimit(session, words);
        break;
      case 7:
        RunValuesCompound(session, words);
        break;
      case 8:
        RunMixedTableCompound(session, words);
        break;
      case 9:
        RunDistinctNullable(session, words);
        break;
      default:
        FAIL() << "unreachable model template";
    }
  }

  constexpr std::array<ModelCompoundOperator, 4> operations{
      ModelCompoundOperator::kUnion,
      ModelCompoundOperator::kUnionAll,
      ModelCompoundOperator::kExcept,
      ModelCompoundOperator::kIntersect,
  };
  const std::array<std::uint64_t, 4> matrix_words{
      UINT64_C(0x19),
      UINT64_C(0x0d),
      UINT64_C(0x17),
      UINT64_C(0x23),
  };
  for (const ModelCompoundOperator first : operations) {
    for (const ModelCompoundOperator second : operations) {
      SCOPED_TRACE("operator-matrix first=" + std::string{CompoundOperatorSql(first)} +
                   " second=" + std::string{CompoundOperatorSql(second)});
      RunMixedTableCompoundCase(session, matrix_words, first, second);
    }
  }
}

}  // namespace
}  // namespace modern_sqlite
