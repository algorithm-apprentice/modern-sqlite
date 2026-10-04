#ifndef MODERN_SQLITE_BINDER_BOUND_SELECT_HPP_
#define MODERN_SQLITE_BINDER_BOUND_SELECT_HPP_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/syntax/ast.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

class Collation;
class FunctionRegistry;
namespace binder_detail {
class SelectBinder;
}  // namespace binder_detail

template <typename Tag>
class BoundId final {
 public:
  constexpr explicit BoundId(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const BoundId&) const noexcept = default;

 private:
  std::uint32_t value_;
};

struct BoundExpressionIdTag;
struct BoundSourceColumnIdTag;
struct BoundParameterIdTag;
struct BoundCollationIdTag;
struct BoundFunctionIdTag;

using BoundExpressionId = BoundId<BoundExpressionIdTag>;
using BoundSourceColumnId = BoundId<BoundSourceColumnIdTag>;
using BoundParameterId = BoundId<BoundParameterIdTag>;
using BoundCollationId = BoundId<BoundCollationIdTag>;
using BoundFunctionId = BoundId<BoundFunctionIdTag>;

enum class BoundSourceKind : std::uint8_t {
  kCatalogTable,
  kSchemaTable,
};

enum class BoundUnaryOperation : std::uint8_t {
  kPositive,
  kNegative,
  kBitwiseNot,
  kLogicalNot,
};

enum class BoundBinaryOperation : std::uint8_t {
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kRemainder,
  kShiftLeft,
  kShiftRight,
  kBitwiseAnd,
  kBitwiseOr,
  kConcatenate,
  kLogicalAnd,
  kLogicalOr,
};

enum class BoundTruthValue : std::uint8_t {
  kFalse,
  kTrue,
};

enum class BoundTruthHint : std::uint8_t {
  kNone,
  kAlwaysFalse,
  kAlwaysTrue,
};

struct BoundTableSource {
  BoundSourceKind kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
  SourceSpan span{};
};

struct BoundSourceColumn {
  std::string_view name{};
  std::optional<std::string_view> declared_type{};
  TypeAffinity affinity = TypeAffinity::kNone;
  std::string_view collation_name{};
  std::optional<ColumnId> catalog_column{};
};

struct BoundCollation {
  std::string name{};
};

struct BoundScalarFunction {
  std::string name{};
  bool deterministic = false;
  bool uses_collation = false;
};

struct BoundParameter {
  std::optional<std::string> name{};
};

struct BoundExpressionProperties {
  TypeAffinity affinity = TypeAffinity::kNone;
  std::optional<BoundCollationId> collation{};
  bool has_explicit_collation = false;
  BoundTruthHint truth_hint = BoundTruthHint::kNone;
};

struct BoundLiteralExpression {
  SqlValue value{};
  bool boolean_keyword = false;
};

struct BoundColumnExpression {
  BoundSourceColumnId column;
};

struct BoundRowIdExpression {};

struct BoundParameterExpression {
  BoundParameterId parameter;
};

struct BoundUnaryExpression {
  BoundUnaryOperation operation = BoundUnaryOperation::kPositive;
  BoundExpressionId operand;
};

struct BoundBinaryExpression {
  BoundBinaryOperation operation = BoundBinaryOperation::kAdd;
  BoundExpressionId left;
  BoundExpressionId right;
};

struct BoundComparisonExpression {
  SqlComparison comparison = SqlComparison::kEqual;
  TypeAffinity affinity = TypeAffinity::kNone;
  BoundCollationId collation;
  BoundExpressionId left;
  BoundExpressionId right;
};

struct BoundTruthTestExpression {
  BoundExpressionId operand;
  BoundTruthValue expected = BoundTruthValue::kTrue;
  bool negated = false;
};

struct BoundAliasReferenceExpression {
  BoundExpressionId target;
};

struct BoundScalarCallExpression {
  BoundFunctionId function;
  BoundCollationId collation;
  std::vector<BoundExpressionId> arguments{};
};

struct BoundCoalesceExpression {
  std::vector<BoundExpressionId> arguments{};
};

struct BoundConditionalExpression {
  std::vector<BoundExpressionId> arguments{};
};

struct BoundLikelihoodExpression {
  BoundExpressionId operand;
  double probability = 0.0;
};

struct BoundCollateExpression {
  BoundExpressionId operand;
  BoundCollationId collation;
};

using BoundExpressionPayload =
    std::variant<BoundLiteralExpression, BoundColumnExpression, BoundRowIdExpression,
                 BoundParameterExpression, BoundUnaryExpression, BoundBinaryExpression,
                 BoundComparisonExpression, BoundTruthTestExpression, BoundAliasReferenceExpression,
                 BoundScalarCallExpression, BoundCoalesceExpression, BoundConditionalExpression,
                 BoundLikelihoodExpression, BoundCollateExpression>;

struct BoundExpression {
  SourceSpan span{};
  BoundExpressionProperties properties{};
  BoundExpressionPayload payload;
};

enum class BoundExpressionKind : std::uint8_t {
  kLiteral,
  kColumn,
  kRowId,
  kParameter,
  kUnary,
  kBinary,
  kComparison,
  kTruthTest,
  kAliasReference,
  kScalarCall,
  kCoalesce,
  kConditional,
  kLikelihood,
  kCollate,
};

[[nodiscard]] BoundExpressionKind BoundExpressionKindOf(const BoundExpression& expression) noexcept;
[[nodiscard]] std::string_view BoundExpressionKindName(BoundExpressionKind kind) noexcept;

struct BoundResultColumn {
  BoundExpressionId expression;
  std::string name{};
  std::optional<std::string> declared_type{};
  TypeAffinity affinity = TypeAffinity::kNone;
};

struct BoundLimit {
  BoundExpressionId limit;
  std::optional<BoundExpressionId> offset{};
};

enum class BindErrorCode : std::uint8_t {
  kInvalidInput,
  kUnsupportedFeature,
  kNoSuchTable,
  kNoSuchColumn,
  kAmbiguousColumn,
  kNoTablesSpecified,
  kNoSuchFunction,
  kWrongFunctionArity,
  kNoSuchCollation,
  kInvalidLiteral,
  kInvalidVariableNumber,
  kParameterLimitExceeded,
  kFunctionArgumentLimitExceeded,
  kResultColumnLimitExceeded,
  kInternalInvariant,
};

struct BindError {
  BindErrorCode code = BindErrorCode::kInternalInvariant;
  SourceSpan span{};
  std::string detail{};

  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view BindErrorCodeName(BindErrorCode code) noexcept;

struct BindOptions {
  std::size_t maximum_parameters = 32'766;
  std::size_t maximum_result_columns = 2'000;
  std::size_t maximum_function_arguments = 1'000;
  bool enable_double_quoted_strings = true;
};

class BindEnvironment final {
 public:
  BindEnvironment(const FunctionRegistry& functions, std::span<const Collation* const> collations,
                  std::uint64_t registration_generation) noexcept
      : functions_(&functions),
        collations_(collations),
        registration_generation_(registration_generation) {}

  [[nodiscard]] static BindEnvironment Core() noexcept;
  [[nodiscard]] const FunctionRegistry& functions() const noexcept { return *functions_; }
  [[nodiscard]] std::span<const Collation* const> collations() const noexcept {
    return collations_;
  }
  [[nodiscard]] std::uint64_t registration_generation() const noexcept {
    return registration_generation_;
  }

 private:
  const FunctionRegistry* functions_;
  std::span<const Collation* const> collations_;
  std::uint64_t registration_generation_;
};

class BoundSelect final {
 public:
  BoundSelect(const BoundSelect&) = delete;
  BoundSelect& operator=(const BoundSelect&) = delete;
  BoundSelect(BoundSelect&&) noexcept;
  BoundSelect& operator=(BoundSelect&&) noexcept;
  ~BoundSelect();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Utf8View source() const noexcept;
  [[nodiscard]] const CatalogSnapshot* catalog() const noexcept;
  [[nodiscard]] std::uint64_t registration_generation() const noexcept;
  [[nodiscard]] const BoundTableSource* table_source() const noexcept;
  [[nodiscard]] std::span<const BoundSourceColumn> source_columns() const noexcept;
  [[nodiscard]] std::span<const BoundCollation> collations() const noexcept;
  [[nodiscard]] std::span<const BoundScalarFunction> functions() const noexcept;
  [[nodiscard]] std::span<const BoundParameter> parameters() const noexcept;
  [[nodiscard]] std::span<const BoundExpression> expressions() const noexcept;
  [[nodiscard]] const BoundExpression& expression(BoundExpressionId id) const noexcept;
  [[nodiscard]] std::span<const BoundResultColumn> result_columns() const noexcept;
  [[nodiscard]] std::optional<BoundExpressionId> where_expression() const noexcept;
  [[nodiscard]] const BoundLimit* limit() const noexcept;

 private:
  friend class binder_detail::SelectBinder;

  struct Impl;

  explicit BoundSelect(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

using BindSelectResult = std::expected<BoundSelect, BindError>;

[[nodiscard]] BindSelectResult BindSelectStatement(
    SyntaxTree tree, CatalogSnapshotPtr catalog,
    BindEnvironment environment = BindEnvironment::Core(), BindOptions options = {});

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_BINDER_BOUND_SELECT_HPP_
