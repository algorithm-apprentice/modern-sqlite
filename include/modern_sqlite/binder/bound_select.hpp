#ifndef MODERN_SQLITE_BINDER_BOUND_SELECT_HPP_
#define MODERN_SQLITE_BINDER_BOUND_SELECT_HPP_

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
enum class ColumnArgumentUse : std::uint8_t;
namespace binder_detail {
class StatementBinder;
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
struct BoundSourceIdTag;
struct BoundSourceColumnIdTag;
struct BoundParameterIdTag;
struct BoundCollationIdTag;
struct BoundFunctionIdTag;
struct BoundAggregateFunctionIdTag;
struct BoundAggregateInvocationIdTag;
struct BoundQueryIdTag;

using BoundExpressionId = BoundId<BoundExpressionIdTag>;
using BoundSourceId = BoundId<BoundSourceIdTag>;
using BoundSourceColumnId = BoundId<BoundSourceColumnIdTag>;
using BoundParameterId = BoundId<BoundParameterIdTag>;
using BoundCollationId = BoundId<BoundCollationIdTag>;
using BoundFunctionId = BoundId<BoundFunctionIdTag>;
using BoundAggregateFunctionId = BoundId<BoundAggregateFunctionIdTag>;
using BoundAggregateInvocationId = BoundId<BoundAggregateInvocationIdTag>;
using BoundQueryId = BoundId<BoundQueryIdTag>;

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
  BoundSourceId id{0};
  BoundSourceKind kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
  SourceSpan span{};
  SourceJoinKind join = SourceJoinKind::kFirst;
  std::size_t source_column_begin = 0;
  std::size_t source_column_count = 0;
  std::optional<BoundExpressionId> on{};
};

struct BoundSourceColumn {
  BoundSourceId source{0};
  bool rowid_alias = false;
  std::string_view name{};
  std::optional<std::string_view> declared_type{};
  TypeAffinity affinity = TypeAffinity::kNone;
  std::string_view collation_name{};
  std::optional<ColumnId> catalog_column{};
  // A direct link to the visible USING/NATURAL representative in this core.
  std::optional<BoundSourceColumnId> merged_into{};
  std::optional<BoundExpressionId> joined_value{};
};

struct BoundCollation {
  std::string name{};
};

struct BoundScalarFunction {
  std::string name{};
  bool deterministic = false;
  bool uses_collation = false;
  ColumnArgumentUse column_argument_use{};
};

enum class BoundAggregateExtremumKind : std::uint8_t {
  kNone,
  kMinimum,
  kMaximum,
};

struct BoundAggregateFunction {
  std::string name{};
  bool deterministic = false;
  bool uses_collation = false;
  BoundAggregateExtremumKind extremum = BoundAggregateExtremumKind::kNone;
};

struct BoundParameter {
  std::optional<std::string> name{};
};

struct BoundExpressionProperties {
  TypeAffinity affinity = TypeAffinity::kNone;
  std::optional<BoundCollationId> collation{};
  bool has_explicit_collation = false;
  BoundTruthHint truth_hint = BoundTruthHint::kNone;
  bool contains_aggregate = false;
  bool references_aggregate = false;
};

struct BoundLiteralExpression {
  SqlValue value{};
  bool boolean_keyword = false;
};

struct BoundColumnExpression {
  BoundSourceColumnId column;
};

struct BoundRowIdExpression {
  BoundSourceId source{0};
};

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
  std::optional<BoundQueryId> context{};
};

struct BoundScalarCallExpression {
  BoundFunctionId function;
  BoundCollationId collation;
  std::vector<BoundExpressionId> arguments{};
};

struct BoundAggregateCallExpression {
  BoundAggregateInvocationId invocation;
  BoundQueryId owner{0};
};

struct BoundSubqueryExpression {
  BoundQueryId query;
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

struct BoundInExpression {
  BoundExpressionId operand;
  std::vector<BoundExpressionId> values{};
  TypeAffinity affinity = TypeAffinity::kNone;
  BoundCollationId collation;
  bool rhs_is_constant = false;
  bool negated = false;
};

using BoundExpressionPayload =
    std::variant<BoundLiteralExpression, BoundColumnExpression, BoundRowIdExpression,
                 BoundParameterExpression, BoundUnaryExpression, BoundBinaryExpression,
                 BoundComparisonExpression, BoundTruthTestExpression, BoundAliasReferenceExpression,
                 BoundScalarCallExpression, BoundCoalesceExpression, BoundConditionalExpression,
                 BoundLikelihoodExpression, BoundCollateExpression, BoundAggregateCallExpression,
                 BoundSubqueryExpression, BoundInExpression>;

struct BoundExpression {
  SourceSpan span{};
  BoundExpressionProperties properties{};
  BoundExpressionPayload payload;
};

struct BoundSourceLeafIdentity {
  BoundSourceId source{0};
  // An omitted column identifies this source's rowid.
  std::optional<BoundSourceColumnId> column{};

  [[nodiscard]] constexpr bool is_rowid() const noexcept { return !column.has_value(); }
  constexpr auto operator<=>(const BoundSourceLeafIdentity&) const noexcept = default;
};

struct BoundSourceCapture {
  BoundSourceLeafIdentity identity{};
  std::optional<BoundExpressionId> value{};
};

struct BoundQueryContextCapture {
  BoundQueryId query{0};
};

struct BoundAggregateContextCapture {
  BoundQueryId query{0};
  BoundExpressionId expression;
};

using BoundQueryCapture =
    std::variant<BoundSourceCapture, BoundQueryContextCapture, BoundAggregateContextCapture>;

enum class BoundSourceDependency : std::uint8_t {
  kBinding,
  kValue,
};

[[nodiscard]] std::optional<BoundSourceLeafIdentity> BoundSourceLeafIdentityOf(
    const BoundExpression& expression, std::span<const BoundSourceColumn> source_columns) noexcept;

[[nodiscard]] std::optional<std::int32_t> BoundExpressionIntegerLiteralValue(
    std::span<const BoundExpression> expressions, BoundExpressionId id) noexcept;

[[nodiscard]] bool BoundExpressionIsIntegerOne(std::span<const BoundExpression> expressions,
                                               BoundExpressionId id) noexcept;

namespace binder_detail {
template <typename Capture>
  requires(std::is_same_v<std::remove_const_t<Capture>, BoundQueryCapture>)
[[nodiscard]] auto FindSourceCapture(std::span<Capture> captures,
                                     const BoundSourceLeafIdentity& identity) noexcept
    -> std::conditional_t<std::is_const_v<Capture>, const BoundSourceCapture*,
                          BoundSourceCapture*> {
  for (Capture& capture : captures) {
    if (auto* source = std::get_if<BoundSourceCapture>(&capture);
        source != nullptr && source->identity == identity) {
      return source;
    }
  }
  return nullptr;
}

template <typename CaptureLookup, typename Visitor>
[[nodiscard]] bool VisitQueryDependencies(std::span<const BoundExpression> expressions,
                                          std::span<const BoundSourceColumn> columns,
                                          BoundExpressionId id, const CaptureLookup& captures,
                                          BoundSourceDependency dependency, Visitor& visitor) {
  if (id.value() >= expressions.size()) {
    return false;
  }
  const BoundExpression& expression = expressions[id.value()];
  if (const auto identity = BoundSourceLeafIdentityOf(expression, columns); identity.has_value()) {
    return visitor(BoundQueryCapture{BoundSourceCapture{.identity = *identity, .value = id}});
  }
  return std::visit(
      [&](const auto& payload) -> bool {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BoundLiteralExpression> ||
                      std::is_same_v<Payload, BoundParameterExpression>) {
          return true;
        } else if constexpr (std::is_same_v<Payload, BoundAggregateCallExpression>) {
          return visitor(BoundQueryCapture{BoundAggregateContextCapture{
              .query = payload.owner,
              .expression = id,
          }});
        } else if constexpr (std::is_same_v<Payload, BoundSubqueryExpression>) {
          const auto inputs = captures(payload.query);
          if (!inputs.has_value()) {
            return false;
          }
          for (const BoundQueryCapture& capture : *inputs) {
            const auto* source = std::get_if<BoundSourceCapture>(&capture);
            if ((dependency == BoundSourceDependency::kBinding || source == nullptr ||
                 source->value.has_value()) &&
                !visitor(capture)) {
              return false;
            }
          }
          return true;
        } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                             std::is_same_v<Payload, BoundTruthTestExpression> ||
                             std::is_same_v<Payload, BoundLikelihoodExpression> ||
                             std::is_same_v<Payload, BoundCollateExpression>) {
          return VisitQueryDependencies(expressions, columns, payload.operand, captures, dependency,
                                        visitor);
        } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression> ||
                             std::is_same_v<Payload, BoundComparisonExpression>) {
          return VisitQueryDependencies(expressions, columns, payload.left, captures, dependency,
                                        visitor) &&
                 VisitQueryDependencies(expressions, columns, payload.right, captures, dependency,
                                        visitor);
        } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
          if (payload.context.has_value() &&
              !visitor(BoundQueryCapture{BoundQueryContextCapture{.query = *payload.context}})) {
            return false;
          }
          return VisitQueryDependencies(expressions, columns, payload.target, captures, dependency,
                                        visitor);
        } else if constexpr (std::is_same_v<Payload, BoundInExpression>) {
          return VisitQueryDependencies(expressions, columns, payload.operand, captures, dependency,
                                        visitor) &&
                 std::ranges::all_of(payload.values, [&](BoundExpressionId value) {
                   return VisitQueryDependencies(expressions, columns, value, captures, dependency,
                                                 visitor);
                 });
        } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression> ||
                             std::is_same_v<Payload, BoundCoalesceExpression> ||
                             std::is_same_v<Payload, BoundConditionalExpression>) {
          return std::ranges::all_of(payload.arguments, [&](BoundExpressionId argument) {
            return VisitQueryDependencies(expressions, columns, argument, captures, dependency,
                                          visitor);
          });
        } else {
          return false;
        }
      },
      expression.payload);
}

template <typename CaptureLookup, typename Visitor>
[[nodiscard]] bool VisitSourceLeaves(std::span<const BoundExpression> expressions,
                                     std::span<const BoundSourceColumn> columns,
                                     BoundExpressionId id, const CaptureLookup& captures,
                                     BoundSourceDependency dependency, Visitor& visitor) {
  auto source = [&](const BoundQueryCapture& capture) {
    const auto* leaf = std::get_if<BoundSourceCapture>(&capture);
    return leaf == nullptr || visitor(leaf->identity, leaf->value);
  };
  return VisitQueryDependencies(expressions, columns, id, captures, dependency, source);
}

template <typename CaptureLookup, typename Visitor>
[[nodiscard]] bool VisitSourceOwners(std::span<const BoundExpression> expressions,
                                     std::span<const BoundSourceColumn> columns,
                                     BoundExpressionId id, const CaptureLookup& captures,
                                     Visitor& visitor) {
  auto owner = [&](const BoundSourceLeafIdentity& identity, std::optional<BoundExpressionId>) {
    visitor(identity.source);
    return true;
  };
  return VisitSourceLeaves(expressions, columns, id, captures, BoundSourceDependency::kBinding,
                           owner);
}
}  // namespace binder_detail

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
  kAggregateCall,
  kSubquery,
  In,
};

[[nodiscard]] BoundExpressionKind BoundExpressionKindOf(const BoundExpression& expression) noexcept;
[[nodiscard]] std::string_view BoundExpressionKindName(BoundExpressionKind kind) noexcept;

struct BoundResultColumn {
  BoundExpressionId expression;
  std::string name{};
  std::optional<std::string> declared_type{};
  TypeAffinity affinity = TypeAffinity::kNone;
};

enum class BoundValuesEvaluationSchedule : std::uint8_t {
  kProducerEager,
  kConsumerFiltered,
};

enum class BoundNullPlacement : std::uint8_t {
  kFirst,
  kLast,
};

struct BoundOrderingTerm {
  BoundExpressionId expression;
  BoundCollationId collation;
  SortOrder order = SortOrder::kAscending;
  BoundNullPlacement null_placement = BoundNullPlacement::kFirst;
  std::optional<std::size_t> result_column{};
  bool explicit_collation = false;

  constexpr auto operator<=>(const BoundOrderingTerm&) const noexcept = default;
};

struct BoundAggregateInvocation {
  SourceSpan span{};
  BoundAggregateFunctionId function;
  BoundCollationId collation;
  BoundQueryId owner{0};
  std::vector<BoundExpressionId> arguments{};
  bool distinct = false;
  std::vector<BoundOrderingTerm> order_by{};
  std::optional<BoundExpressionId> filter{};
  bool argument_reuses_single_order_key = false;
  bool source_anchored = false;
};

struct BoundGroupingTerm {
  BoundExpressionId expression;
  BoundCollationId collation;
  std::optional<std::size_t> result_column{};
  bool ordinal_reference = false;
};

struct BoundAggregateProgram {
  std::vector<BoundAggregateInvocationId> invocations{};
  std::size_t having_invocation_begin = 0;
  std::vector<BoundGroupingTerm> group_by{};
  std::optional<BoundExpressionId> having{};
  std::vector<BoundExpressionId> source_leaves{};
  std::vector<BoundExpressionId> representative_leaves{};
};

struct BoundSelectCore {
  SelectQuantifier quantifier = SelectQuantifier::kDefault;
  std::vector<BoundTableSource> table_sources{};
  std::size_t source_column_begin = 0;
  std::size_t source_column_count = 0;
  std::vector<BoundResultColumn> result_columns{};
  std::optional<BoundExpressionId> where{};
  std::optional<BoundAggregateProgram> aggregate{};
};

struct BoundValuesCore {
  std::vector<std::vector<BoundExpressionId>> rows{};
  std::vector<BoundResultColumn> result_columns{};
  BoundValuesEvaluationSchedule schedule = BoundValuesEvaluationSchedule::kProducerEager;
};

using BoundQueryCore = std::variant<BoundSelectCore, BoundValuesCore>;

struct BoundLimit {
  BoundExpressionId limit;
  std::optional<BoundExpressionId> offset{};

  constexpr auto operator<=>(const BoundLimit&) const noexcept = default;
};

enum class BoundQueryDestination : std::uint8_t {
  kClient,
  kScalar,
  kExists,
};

struct BoundQuery {
  BoundQueryId id{0};
  std::optional<BoundQueryId> parent{};
  BoundQueryDestination destination = BoundQueryDestination::kClient;
  std::vector<BoundQueryCapture> captures{};
  std::size_t core_begin = 0;
  std::size_t core_count = 0;
  std::vector<CompoundOperator> compound_operators{};
  std::vector<BoundCollationId> compound_collations{};
  std::vector<BoundResultColumn> output_columns{};
  std::vector<BoundOrderingTerm> order_by{};
  std::optional<BoundLimit> limit{};
};

[[nodiscard]] bool BoundQueryHasSingleRowDemand(
    const BoundQuery& query, std::span<const BoundExpression> expressions) noexcept;

[[nodiscard]] bool BoundQueryCanOmitSetComparisons(
    const BoundQuery& query, std::span<const BoundExpression> expressions) noexcept;

[[nodiscard]] bool BoundQueryRequiresResultValues(
    const BoundQuery& query, std::span<const BoundExpression> expressions) noexcept;

[[nodiscard]] std::span<const BoundResultColumn> BoundCoreResultColumns(
    const BoundQueryCore& core) noexcept;
[[nodiscard]] std::optional<BoundCollationId> BoundCompoundResultCollation(
    std::span<const BoundQueryCore> cores, std::span<const BoundExpression> expressions,
    std::size_t column) noexcept;
[[nodiscard]] bool BoundQueryHasCompoundOrderWrapper(const BoundQuery& query) noexcept;
[[nodiscard]] bool BoundCompoundRequiresMerge(CompoundOperator operation, bool ordered,
                                              bool limit_one) noexcept;
[[nodiscard]] std::optional<std::size_t> BoundCompoundSplit(
    std::span<const CompoundOperator> operations, bool merge) noexcept;

enum class BindErrorCode : std::uint8_t {
  kInvalidInput,
  kUnsupportedFeature,
  kNoSuchTable,
  kNoSuchColumn,
  kAmbiguousColumn,
  kNoTablesSpecified,
  kNoSuchFunction,
  kWrongFunctionArity,
  kAggregateMisuse,
  kHavingWithoutAggregate,
  kNoSuchCollation,
  kOrderByTermOutOfRange,
  kGroupByTermOutOfRange,
  kCompoundOrderByTerm,
  kCompoundColumnCount,
  kValuesColumnCount,
  kInvalidLiteral,
  kInvalidVariableNumber,
  kParameterLimitExceeded,
  kFunctionArgumentLimitExceeded,
  kAggregateTermLimitExceeded,
  kResultColumnLimitExceeded,
  kColumnCountMismatch,
  kDuplicateColumn,
  kTableAlreadyExists,
  kIndexAlreadyExists,
  kObjectNameReserved,
  kIndexedTableUnsupported,
  kJoinConditionScope,
  kExpressionDepthExceeded,
  kInternalInvariant,
};

struct BindError {
  BindErrorCode code = BindErrorCode::kInternalInvariant;
  SourceSpan span{};
  std::string detail{};
  bool requires_schema_check = false;

  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view BindErrorCodeName(BindErrorCode code) noexcept;

struct BindOptions {
  std::size_t maximum_parameters = 32'766;
  std::size_t maximum_result_columns = 2'000;
  std::size_t maximum_function_arguments = 1'000;
  bool enable_double_quoted_strings = true;
  std::size_t maximum_aggregate_terms = 2'000;
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
  [[nodiscard]] std::span<const BoundTableSource> table_sources() const noexcept;
  [[nodiscard]] std::span<const BoundSourceColumn> source_columns() const noexcept;
  [[nodiscard]] std::span<const std::string> registered_collations() const noexcept;
  [[nodiscard]] std::span<const BoundCollation> collations() const noexcept;
  [[nodiscard]] std::span<const BoundScalarFunction> functions() const noexcept;
  [[nodiscard]] std::span<const BoundAggregateFunction> aggregate_functions() const noexcept;
  [[nodiscard]] std::span<const BoundAggregateInvocation> aggregate_invocations() const noexcept;
  [[nodiscard]] std::span<const BoundParameter> parameters() const noexcept;
  [[nodiscard]] std::span<const BoundExpression> expressions() const noexcept;
  [[nodiscard]] const BoundExpression& expression(BoundExpressionId id) const noexcept;
  [[nodiscard]] const BoundQuery& root_query() const noexcept;
  [[nodiscard]] const BoundQuery& query(BoundQueryId id) const noexcept;
  [[nodiscard]] std::size_t query_count() const noexcept;
  [[nodiscard]] std::span<const BoundQueryCore> query_cores() const noexcept;
  [[nodiscard]] std::span<const CompoundOperator> compound_operators() const noexcept;
  [[nodiscard]] std::span<const BoundCollationId> compound_collations() const noexcept;
  [[nodiscard]] std::span<const BoundResultColumn> result_columns() const noexcept;
  [[nodiscard]] std::optional<BoundExpressionId> where_expression() const noexcept;
  [[nodiscard]] std::span<const BoundOrderingTerm> order_by() const noexcept;
  [[nodiscard]] const BoundLimit* limit() const noexcept;

 private:
  friend class binder_detail::StatementBinder;

  struct Impl;

  explicit BoundSelect(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

class BoundQueryView final {
 public:
  explicit BoundQueryView(const BoundSelect& owner, BoundQueryId id = BoundQueryId{0}) noexcept
      : owner_(&owner), id_(id) {}

  [[nodiscard]] bool valid() const noexcept {
    return owner_->valid() && id_.value() < owner_->query_count();
  }
  [[nodiscard]] const BoundSelect& owner() const noexcept { return *owner_; }
  [[nodiscard]] const BoundQuery& query() const noexcept { return owner_->query(id_); }
  [[nodiscard]] Utf8View source() const noexcept { return owner_->source(); }
  [[nodiscard]] const CatalogSnapshot* catalog() const noexcept { return owner_->catalog(); }
  [[nodiscard]] std::span<const BoundQueryCore> query_cores() const noexcept {
    return owner_->query_cores().subspan(query().core_begin, query().core_count);
  }
  [[nodiscard]] std::span<const BoundTableSource> table_sources() const noexcept {
    const auto* core = std::get_if<BoundSelectCore>(&query_cores().front());
    return core != nullptr ? std::span<const BoundTableSource>{core->table_sources}
                           : std::span<const BoundTableSource>{};
  }
  [[nodiscard]] std::optional<BoundExpressionId> where_expression() const noexcept {
    const auto* core = std::get_if<BoundSelectCore>(&query_cores().front());
    return core != nullptr ? core->where : std::nullopt;
  }
  [[nodiscard]] std::span<const BoundSourceColumn> source_columns() const noexcept {
    return owner_->source_columns();
  }
  [[nodiscard]] std::span<const BoundParameter> parameters() const noexcept {
    return owner_->parameters();
  }
  [[nodiscard]] std::span<const BoundExpression> expressions() const noexcept {
    return owner_->expressions();
  }
  [[nodiscard]] const BoundExpression& expression(BoundExpressionId id) const noexcept {
    return owner_->expression(id);
  }
  [[nodiscard]] std::span<const std::string> registered_collations() const noexcept {
    return owner_->registered_collations();
  }
  [[nodiscard]] std::span<const BoundCollation> collations() const noexcept {
    return owner_->collations();
  }
  [[nodiscard]] std::span<const BoundScalarFunction> functions() const noexcept {
    return owner_->functions();
  }
  [[nodiscard]] std::span<const BoundAggregateFunction> aggregate_functions() const noexcept {
    return owner_->aggregate_functions();
  }
  [[nodiscard]] std::span<const BoundAggregateInvocation> aggregate_invocations() const noexcept {
    return owner_->aggregate_invocations();
  }
  [[nodiscard]] std::span<const CompoundOperator> compound_operators() const noexcept {
    return query().compound_operators;
  }
  [[nodiscard]] std::span<const BoundCollationId> compound_collations() const noexcept {
    return query().compound_collations;
  }
  [[nodiscard]] std::span<const BoundResultColumn> result_columns() const noexcept {
    return query().output_columns;
  }
  [[nodiscard]] std::span<const BoundOrderingTerm> order_by() const noexcept {
    return query().order_by;
  }
  [[nodiscard]] const BoundLimit* limit() const noexcept {
    return query().limit.has_value() ? &*query().limit : nullptr;
  }

 private:
  const BoundSelect* owner_;
  BoundQueryId id_;
};

using BindSelectResult = std::expected<BoundSelect, BindError>;

[[nodiscard]] BindSelectResult BindSelectStatement(
    SyntaxTree tree, CatalogSnapshotPtr catalog,
    BindEnvironment environment = BindEnvironment::Core(), BindOptions options = {});

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_BINDER_BOUND_SELECT_HPP_
