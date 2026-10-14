#include "modern_sqlite/optimizer/physical_plan.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/instrumentation/counters.hpp"

namespace modern_sqlite {
namespace {

constexpr std::uint64_t kDefaultEstimatedRows = 1U << 20U;

struct SourceInfo {
  bool single_row = false;
  BoundSourceId id{0};
  BoundSourceKind source_kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
  RootPageId root_page{};
  std::uint64_t estimated_rows = 0;
  std::uint64_t estimated_row_size = 1;
  bool rowid_eligible = false;
};

struct PredicateAnalysis {
  bool empty = false;
  std::optional<BoundExpressionId> rowid_key{};
  std::vector<BoundExpressionId> guards{};
  std::vector<BoundExpressionId> residuals{};
};

struct ReadPredicateTerm {
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
};

struct ReadPredicateAnalysis {
  bool empty = false;
  std::vector<BoundExpressionId> guards{};
  std::vector<ReadPredicateTerm> terms{};
};

struct RowIdCandidatePlan {
  BoundExpressionId key{0};
  std::size_t selected_term = 0;
};

struct IndexCandidatePlan {
  PhysicalIndexScanNode node{};
  std::vector<bool> selected_terms{};
};

using ReadAccessPayload = std::variant<std::monostate, RowIdCandidatePlan, IndexCandidatePlan>;

struct ReadAccessCandidate {
  AccessPathCandidate published{};
  ReadAccessPayload payload{};
};

[[nodiscard]] std::uint64_t DerivedTableRowSize(const CatalogTable& table) noexcept;
[[nodiscard]] bool IsNullLiteral(const BoundQueryView& bound_select, BoundExpressionId id) noexcept;

[[nodiscard]] OptimizerError OptimizerFailure(OptimizerErrorCode code, std::string_view detail) {
  return OptimizerError{.code = code, .detail = std::string{detail}};
}

[[nodiscard]] OptimizerError InvariantFailure(std::string_view detail) {
  return OptimizerFailure(OptimizerErrorCode::kInternalInvariant, detail);
}

template <typename T>
[[nodiscard]] const T& AssumePresent(const std::optional<T>& value) noexcept {
  if (!value.has_value()) {
    std::terminate();
  }
  return *value;
}

[[nodiscard]] bool OrderingTermsEqual(const BoundOrderingTerm& left,
                                      const BoundOrderingTerm& right) noexcept {
  return left.expression == right.expression && left.collation == right.collation &&
         left.order == right.order && left.null_placement == right.null_placement &&
         left.result_column == right.result_column &&
         left.explicit_collation == right.explicit_collation;
}

template <typename Bound>
[[nodiscard]] bool IsValidExpressionId(const Bound& bound, BoundExpressionId id) noexcept {
  return id.value() < bound.expressions().size();
}

template <typename Bound>
[[nodiscard]] BoundExpressionId TransparentPredicateId(const Bound& bound_select,
                                                       BoundExpressionId id) noexcept {
  while (IsValidExpressionId(bound_select, id)) {
    const BoundExpression& expression = bound_select.expression(id);
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      id = alias->target;
      continue;
    }
    if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
        collate != nullptr) {
      id = collate->operand;
      continue;
    }
    if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&expression.payload);
        likelihood != nullptr) {
      id = likelihood->operand;
      continue;
    }
    break;
  }
  return id;
}

template <typename Bound>
[[nodiscard]] BoundExpressionId TransparentRowIdOperandId(const Bound& bound_select,
                                                          BoundExpressionId id) noexcept {
  while (IsValidExpressionId(bound_select, id)) {
    const BoundExpression& expression = bound_select.expression(id);
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      id = alias->target;
      continue;
    }
    if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
        collate != nullptr) {
      id = collate->operand;
      continue;
    }
    break;
  }
  return id;
}

template <typename Bound>
void CollectConjuncts(const Bound& bound_select, BoundExpressionId id,
                      std::vector<BoundExpressionId>* output) {
  const BoundExpressionId transparent = TransparentPredicateId(bound_select, id);
  if (IsValidExpressionId(bound_select, transparent)) {
    const BoundExpression& expression = bound_select.expression(transparent);
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
        binary != nullptr && binary->operation == BoundBinaryOperation::kLogicalAnd) {
      CollectConjuncts(bound_select, binary->left, output);
      CollectConjuncts(bound_select, binary->right, output);
      return;
    }
  }
  output->push_back(id);
}

template <typename Bound>
[[nodiscard]] std::size_t CountConjuncts(const Bound& bound_select, BoundExpressionId id) noexcept {
  const BoundExpressionId transparent = TransparentPredicateId(bound_select, id);
  if (IsValidExpressionId(bound_select, transparent)) {
    const BoundExpression& expression = bound_select.expression(transparent);
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
        binary != nullptr && binary->operation == BoundBinaryOperation::kLogicalAnd) {
      return CountConjuncts(bound_select, binary->left) +
             CountConjuncts(bound_select, binary->right);
    }
  }
  return 1;
}

template <typename Bound, typename Visitor>
[[nodiscard]] bool VisitConjuncts(const Bound& bound_select, BoundExpressionId id,
                                  Visitor* visitor) {
  const BoundExpressionId transparent = TransparentPredicateId(bound_select, id);
  if (IsValidExpressionId(bound_select, transparent)) {
    const BoundExpression& expression = bound_select.expression(transparent);
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
        binary != nullptr && binary->operation == BoundBinaryOperation::kLogicalAnd) {
      return VisitConjuncts(bound_select, binary->left, visitor) &&
             VisitConjuncts(bound_select, binary->right, visitor);
    }
  }
  return (*visitor)(id);
}

[[nodiscard]] SqlTruthValue LogicalNot(SqlTruthValue value) noexcept {
  switch (value) {
    case SqlTruthValue::kFalse:
      return SqlTruthValue::kTrue;
    case SqlTruthValue::kTrue:
      return SqlTruthValue::kFalse;
    case SqlTruthValue::kNull:
      return SqlTruthValue::kNull;
  }
  return SqlTruthValue::kNull;
}

[[nodiscard]] SqlTruthValue LogicalAnd(SqlTruthValue left, SqlTruthValue right) noexcept {
  if (left == SqlTruthValue::kFalse || right == SqlTruthValue::kFalse) {
    return SqlTruthValue::kFalse;
  }
  if (left == SqlTruthValue::kTrue && right == SqlTruthValue::kTrue) {
    return SqlTruthValue::kTrue;
  }
  return SqlTruthValue::kNull;
}

[[nodiscard]] SqlTruthValue LogicalOr(SqlTruthValue left, SqlTruthValue right) noexcept {
  if (left == SqlTruthValue::kTrue || right == SqlTruthValue::kTrue) {
    return SqlTruthValue::kTrue;
  }
  if (left == SqlTruthValue::kFalse && right == SqlTruthValue::kFalse) {
    return SqlTruthValue::kFalse;
  }
  return SqlTruthValue::kNull;
}

template <typename Bound>
[[nodiscard]] std::optional<SqlTruthValue> ConstantTruth(const Bound& bound_select,
                                                         BoundExpressionId id) noexcept {
  id = TransparentPredicateId(bound_select, id);
  if (!IsValidExpressionId(bound_select, id)) {
    return std::nullopt;
  }
  const BoundExpression& expression = bound_select.expression(id);
  if (const auto* literal = std::get_if<BoundLiteralExpression>(&expression.payload);
      literal != nullptr) {
    return EvaluateSqlTruth(literal->value);
  }
  if (const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
      unary != nullptr && unary->operation == BoundUnaryOperation::kLogicalNot) {
    const std::optional<SqlTruthValue> operand = ConstantTruth(bound_select, unary->operand);
    return operand.has_value() ? std::optional<SqlTruthValue>{LogicalNot(*operand)} : std::nullopt;
  }
  if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
      binary != nullptr) {
    const std::optional<SqlTruthValue> left = ConstantTruth(bound_select, binary->left);
    const std::optional<SqlTruthValue> right = ConstantTruth(bound_select, binary->right);
    if (binary->operation == BoundBinaryOperation::kLogicalAnd) {
      if ((left.has_value() && *left == SqlTruthValue::kFalse) ||
          (right.has_value() && *right == SqlTruthValue::kFalse)) {
        return SqlTruthValue::kFalse;
      }
      if (left.has_value() && right.has_value()) {
        return LogicalAnd(*left, *right);
      }
    }
    if (binary->operation == BoundBinaryOperation::kLogicalOr && left.has_value() &&
        right.has_value()) {
      return LogicalOr(*left, *right);
    }
    return std::nullopt;
  }
  if (const auto* truth = std::get_if<BoundTruthTestExpression>(&expression.payload);
      truth != nullptr) {
    const std::optional<SqlTruthValue> operand = ConstantTruth(bound_select, truth->operand);
    if (!operand.has_value()) {
      return std::nullopt;
    }
    const bool expected = truth->expected == BoundTruthValue::kTrue
                              ? *operand == SqlTruthValue::kTrue
                              : *operand == SqlTruthValue::kFalse;
    const bool result = truth->negated ? !expected : expected;
    return result ? SqlTruthValue::kTrue : SqlTruthValue::kFalse;
  }
  return std::nullopt;
}

template <typename Bound, typename Visitor>
[[nodiscard]] bool VisitExpressionSources(const Bound& bound_select, BoundExpressionId id,
                                          Visitor& visitor) {
  return binder_detail::VisitSourceOwners(bound_select.expressions(), bound_select.source_columns(),
                                          id, visitor);
}

template <typename Bound>
[[nodiscard]] bool ExpressionDependsOnSource(const Bound& bound_select, BoundExpressionId id) {
  bool depends = false;
  auto visitor = [&](BoundSourceId) { depends = true; };
  return !VisitExpressionSources(bound_select, id, visitor) || depends;
}

[[nodiscard]] std::expected<std::vector<BoundSourceId>, OptimizerError> JoinPredicateSources(
    const BoundQueryView& bound_select, const BoundSelectCore& core,
    std::span<const BoundExpressionId> predicates,
    std::optional<BoundSourceId> condition_owner = std::nullopt) {
  std::vector<BoundSourceId> sources;
  if (core.table_sources.size() < 2U) {
    return sources;
  }
  sources.reserve(predicates.size());
  BoundSourceId floor = core.table_sources.front().id;
  for (const BoundTableSource& source : core.table_sources) {
    if (SourceJoinPreservesRight(source.join) &&
        (!condition_owner.has_value() || source.id <= *condition_owner)) {
      floor = source.id;
    }
  }
  for (const BoundExpressionId predicate : predicates) {
    BoundSourceId ready = floor;
    bool valid = true;
    auto visitor = [&](BoundSourceId source) {
      if (std::ranges::find(core.table_sources, source, &BoundTableSource::id) ==
          core.table_sources.end()) {
        valid = false;
      }
      ready = std::max(ready, source);
    };
    if (!VisitExpressionSources(bound_select, predicate, visitor) || !valid) {
      return std::unexpected(InvariantFailure("join predicate has invalid source dependencies"));
    }
    sources.push_back(ready);
  }
  return sources;
}

[[nodiscard]] std::expected<std::optional<BoundSourceId>, OptimizerError> JoinConditionSource(
    const LogicalQueryView& plan, const LogicalJoinNode& join) {
  if (!join.on.has_value()) {
    return std::optional<BoundSourceId>{};
  }
  const auto* right = std::get_if<LogicalScanNode>(&plan.node(join.right).payload);
  if (right == nullptr) {
    return std::unexpected(InvariantFailure("join condition has no right source"));
  }
  const BoundQueryView& bound = plan.bound_select();
  const auto& core = std::get<BoundSelectCore>(bound.query_cores()[join.core_index]);
  const std::array predicates{*join.on};
  auto sources = JoinPredicateSources(bound, core, predicates, right->source);
  if (!sources.has_value()) {
    return std::unexpected(std::move(sources.error()));
  }
  if (sources->size() != 1U) {
    return std::unexpected(InvariantFailure("join condition has invalid source placement"));
  }
  return std::optional{SourceJoinPreservesLeft(join.join) || SourceJoinPreservesRight(join.join)
                           ? right->source
                           : sources->front()};
}

template <typename Bound>
[[nodiscard]] bool ExpressionIsDeterministic(const Bound& bound_select, BoundExpressionId id) {
  if (!IsValidExpressionId(bound_select, id)) {
    return false;
  }
  const BoundExpression& expression = bound_select.expression(id);
  return std::visit(
      [&](const auto& payload) -> bool {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BoundLiteralExpression> ||
                      std::is_same_v<Payload, BoundColumnExpression> ||
                      std::is_same_v<Payload, BoundRowIdExpression> ||
                      std::is_same_v<Payload, BoundParameterExpression>) {
          return true;
        } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                             std::is_same_v<Payload, BoundTruthTestExpression> ||
                             std::is_same_v<Payload, BoundLikelihoodExpression> ||
                             std::is_same_v<Payload, BoundCollateExpression>) {
          return ExpressionIsDeterministic(bound_select, payload.operand);
        } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression> ||
                             std::is_same_v<Payload, BoundComparisonExpression>) {
          return ExpressionIsDeterministic(bound_select, payload.left) &&
                 ExpressionIsDeterministic(bound_select, payload.right);
        } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
          return ExpressionIsDeterministic(bound_select, payload.target);
        } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression>) {
          if (payload.function.value() >= bound_select.functions().size() ||
              !bound_select.functions()[payload.function.value()].deterministic) {
            return false;
          }
          return std::ranges::all_of(payload.arguments, [&](BoundExpressionId argument) {
            return ExpressionIsDeterministic(bound_select, argument);
          });
        } else if constexpr (std::is_same_v<Payload, BoundCoalesceExpression> ||
                             std::is_same_v<Payload, BoundConditionalExpression>) {
          return std::ranges::all_of(payload.arguments, [&](BoundExpressionId argument) {
            return ExpressionIsDeterministic(bound_select, argument);
          });
        }
        return false;
      },
      expression.payload);
}

template <typename Bound>
[[nodiscard]] bool IsStatementGuard(const Bound& bound_select, BoundExpressionId id) {
  return !ExpressionDependsOnSource(bound_select, id) &&
         ExpressionIsDeterministic(bound_select, id);
}

struct AdvancedPredicateParts {
  std::vector<BoundExpressionId> predicates{};
  std::vector<BoundExpressionId> guards{};
};

[[nodiscard]] AdvancedPredicateParts SplitAdvancedPredicate(const BoundQueryView& bound_select,
                                                            BoundExpressionId predicate) {
  AdvancedPredicateParts parts;
  const auto visit = [&](BoundExpressionId conjunct) {
    (IsStatementGuard(bound_select, conjunct) ? parts.guards : parts.predicates)
        .push_back(conjunct);
    return true;
  };
  auto visitor = visit;
  static_cast<void>(VisitConjuncts(bound_select, predicate, &visitor));
  return parts;
}

template <typename Bound>
[[nodiscard]] bool IsRowIdReference(const Bound& bound_select, const SourceInfo& source,
                                    BoundExpressionId id) noexcept {
  if (!source.rowid_eligible) {
    return false;
  }
  id = TransparentRowIdOperandId(bound_select, id);
  if (!IsValidExpressionId(bound_select, id)) {
    return false;
  }
  const BoundExpression& expression = bound_select.expression(id);
  const auto identity = BoundSourceLeafIdentityOf(expression, bound_select.source_columns());
  return identity.has_value() && identity->source == source.id && identity->is_rowid();
}

template <typename Bound>
[[nodiscard]] bool ExpressionAvailableBeforeSource(const Bound& bound_select,
                                                   BoundExpressionId expression,
                                                   BoundSourceId source) {
  bool available = ExpressionIsDeterministic(bound_select, expression);
  auto visitor = [&](BoundSourceId owner) { available = available && owner < source; };
  return VisitExpressionSources(bound_select, expression, visitor) && available;
}

template <typename Bound>
[[nodiscard]] bool IsJoinedLookupValue(const Bound& bound_select, BoundExpressionId id) {
  id = TransparentRowIdOperandId(bound_select, id);
  if (!IsValidExpressionId(bound_select, id)) {
    return false;
  }
  const BoundExpression& expression = bound_select.expression(id);
  if (std::holds_alternative<BoundLiteralExpression>(expression.payload) ||
      std::holds_alternative<BoundParameterExpression>(expression.payload) ||
      std::holds_alternative<BoundColumnExpression>(expression.payload) ||
      std::holds_alternative<BoundRowIdExpression>(expression.payload)) {
    return true;
  }
  const auto* coalesce = std::get_if<BoundCoalesceExpression>(&expression.payload);
  return coalesce != nullptr &&
         std::ranges::all_of(coalesce->arguments, [&](BoundExpressionId argument) {
           return IsJoinedLookupValue(bound_select, argument);
         });
}

template <typename Bound>
[[nodiscard]] std::optional<BoundExpressionId> RowIdLookupKey(
    const Bound& bound_select, const SourceInfo& source, BoundExpressionId predicate,
    std::optional<BoundSourceId> available_before = std::nullopt) {
  const BoundExpressionId transparent = TransparentPredicateId(bound_select, predicate);
  if (!IsValidExpressionId(bound_select, transparent)) {
    return std::nullopt;
  }
  const auto* comparison =
      std::get_if<BoundComparisonExpression>(&bound_select.expression(transparent).payload);
  if (comparison == nullptr || (comparison->comparison != SqlComparison::kEqual &&
                                comparison->comparison != SqlComparison::kIs)) {
    return std::nullopt;
  }
  const bool left_rowid = IsRowIdReference(bound_select, source, comparison->left);
  const bool right_rowid = IsRowIdReference(bound_select, source, comparison->right);
  if (left_rowid == right_rowid) {
    return std::nullopt;
  }
  const BoundExpressionId key = left_rowid ? comparison->right : comparison->left;
  if (available_before.has_value()) {
    if (!IsJoinedLookupValue(bound_select, key) ||
        !ExpressionAvailableBeforeSource(bound_select, key, *available_before)) {
      return std::nullopt;
    }
  } else if (ExpressionDependsOnSource(bound_select, key)) {
    return std::nullopt;
  }
  return key;
}

[[nodiscard]] SourceInfo ResolveTableSource(const BoundQueryView& bound_select,
                                            const BoundTableSource& source) {
  if (source.kind == BoundSourceKind::kSchemaTable) {
    return SourceInfo{
        .single_row = false,
        .id = source.id,
        .source_kind = BoundSourceKind::kSchemaTable,
        .table = std::nullopt,
        .root_page = RootPageId{1},
        .estimated_rows = kDefaultEstimatedRows,
        .estimated_row_size = 1,
        .rowid_eligible = true,
    };
  }

  const TableId table_id = source.table.value_or(TableId{});
  const CatalogTable& table = bound_select.catalog()->table(table_id);
  return SourceInfo{
      .single_row = false,
      .id = source.id,
      .source_kind = BoundSourceKind::kCatalogTable,
      .table = table_id,
      .root_page = table.root_page,
      .estimated_rows = table.statistics.estimated_rows.value_or(kDefaultEstimatedRows),
      .estimated_row_size = table.statistics.average_row_size.value_or(DerivedTableRowSize(table)),
      .rowid_eligible = !table.without_rowid,
  };
}

[[nodiscard]] SourceInfo ResolveCoreSource(const BoundQueryView& bound_select,
                                           std::size_t core_index) {
  const auto& core = std::get<BoundSelectCore>(bound_select.query_cores()[core_index]);
  if (core.table_sources.empty()) {
    return SourceInfo{
        .single_row = true,
        .source_kind = BoundSourceKind::kCatalogTable,
        .table = std::nullopt,
        .root_page = RootPageId{},
        .estimated_rows = 1,
        .estimated_row_size = 1,
        .rowid_eligible = false,
    };
  }
  return ResolveTableSource(bound_select, core.table_sources.front());
}

[[nodiscard]] SourceInfo ResolveSource(const LogicalQueryView& logical_plan) {
  return ResolveCoreSource(logical_plan.bound_select(), 0);
}

[[nodiscard]] bool IsAdvancedLogicalPlan(const LogicalQueryView& logical_plan) {
  return std::ranges::any_of(logical_plan.nodes(), [](const LogicalNode& node) {
    return std::holds_alternative<LogicalValuesNode>(node.payload) ||
           std::holds_alternative<LogicalDistinctNode>(node.payload) ||
           std::holds_alternative<LogicalCompoundNode>(node.payload) ||
           std::holds_alternative<LogicalAdvancedOrderNode>(node.payload) ||
           std::holds_alternative<LogicalAggregateNode>(node.payload) ||
           std::holds_alternative<LogicalHavingNode>(node.payload) ||
           std::holds_alternative<LogicalJoinNode>(node.payload);
  });
}

[[nodiscard]] bool IsUnionLimitOne(const BoundQueryView& bound_select) {
  const BoundLimit* limit = bound_select.limit();
  return limit != nullptr && !limit->offset.has_value() &&
         BoundExpressionIsIntegerOne(bound_select.expressions(), limit->limit);
}

[[nodiscard]] bool IsSetBarrier(CompoundOperator operation) noexcept {
  return operation == CompoundOperator::kExcept || operation == CompoundOperator::kIntersect;
}

[[nodiscard]] bool UsesUnionLimitOne(const BoundQueryView& bound_select,
                                     std::size_t compound_index) {
  const std::span<const CompoundOperator> operations = bound_select.compound_operators();
  if (!bound_select.order_by().empty() || !IsUnionLimitOne(bound_select) ||
      compound_index >= operations.size() ||
      operations[compound_index] != CompoundOperator::kUnion) {
    return false;
  }
  return std::ranges::none_of(operations.subspan(compound_index + 1U), IsSetBarrier);
}

[[nodiscard]] std::optional<std::size_t> OrderedUnionLimitOneCoreStart(
    const BoundQueryView& bound_select, bool set_then_order) {
  if (set_then_order || bound_select.order_by().empty() || !IsUnionLimitOne(bound_select)) {
    return std::nullopt;
  }

  const std::span<const CompoundOperator> operations = bound_select.compound_operators();
  std::size_t operation_start = 0;
  std::size_t core_start = 0;
  for (std::size_t index = 0; index < operations.size(); ++index) {
    if (IsSetBarrier(operations[index])) {
      operation_start = index + 1U;
      core_start = index + 2U;
    }
  }
  if (std::ranges::none_of(operations.subspan(operation_start), [](CompoundOperator operation) {
        return operation == CompoundOperator::kUnion;
      })) {
    return std::nullopt;
  }
  return core_start;
}

[[nodiscard]] bool UsesOrderedUnionAllRuntimeLimit(const BoundQueryView& bound_select,
                                                   bool set_then_order) {
  const std::span<const CompoundOperator> operations = bound_select.compound_operators();
  return !set_then_order && !bound_select.order_by().empty() && bound_select.limit() != nullptr &&
         !operations.empty() && std::ranges::all_of(operations, [](CompoundOperator operation) {
           return operation == CompoundOperator::kUnionAll;
         });
}

[[nodiscard]] const LogicalOrderNode* LogicalOrderOf(
    const LogicalQueryView& logical_plan) noexcept {
  if (logical_plan.bound_select().order_by().empty()) {
    return nullptr;
  }
  const auto* output =
      std::get_if<LogicalOutputNode>(&logical_plan.node(logical_plan.root()).payload);
  if (output == nullptr || output->input.value() >= logical_plan.nodes().size()) {
    return nullptr;
  }
  LogicalNodeId input = output->input;
  if (logical_plan.bound_select().limit() != nullptr) {
    const auto* limit = std::get_if<LogicalLimitNode>(&logical_plan.node(input).payload);
    if (limit == nullptr || limit->input.value() >= logical_plan.nodes().size()) {
      return nullptr;
    }
    input = limit->input;
  }
  return std::get_if<LogicalOrderNode>(&logical_plan.node(input).payload);
}

template <typename Bound>
[[nodiscard]] PredicateAnalysis AnalyzePredicate(const Bound& bound_select,
                                                 const SourceInfo& source,
                                                 std::optional<BoundExpressionId> where) {
  PredicateAnalysis analysis;
  if (!where.has_value()) {
    return analysis;
  }

  std::vector<BoundExpressionId> terms;
  terms.reserve(CountConjuncts(bound_select, *where));
  CollectConjuncts(bound_select, *where, &terms);

  if (source.single_row) {
    analysis.guards.reserve(terms.size());
    for (const BoundExpressionId term : terms) {
      const std::optional<SqlTruthValue> truth = ConstantTruth(bound_select, term);
      if (truth.has_value() && *truth == SqlTruthValue::kTrue) {
        continue;
      }
      if (truth.has_value()) {
        analysis.empty = true;
        break;
      }
      analysis.guards.push_back(term);
    }
    return analysis;
  }

  std::size_t cutoff = terms.size();
  std::size_t guard_count = 0;
  std::size_t unknown_count = 0;
  for (std::size_t index = 0; index < terms.size(); ++index) {
    const std::optional<SqlTruthValue> truth = ConstantTruth(bound_select, terms[index]);
    if (truth.has_value() && *truth == SqlTruthValue::kTrue) {
      continue;
    }
    if (truth.has_value()) {
      analysis.empty = true;
      cutoff = index;
      break;
    }
    if (IsStatementGuard(bound_select, terms[index])) {
      ++guard_count;
    } else {
      ++unknown_count;
    }
  }

  std::optional<std::size_t> selected_term;
  if (!analysis.empty && source.rowid_eligible) {
    for (std::size_t index = 0; index < cutoff; ++index) {
      const std::optional<SqlTruthValue> truth = ConstantTruth(bound_select, terms[index]);
      if (truth.has_value() || IsStatementGuard(bound_select, terms[index])) {
        continue;
      }
      const std::optional<BoundExpressionId> key =
          RowIdLookupKey(bound_select, source, terms[index]);
      if (key.has_value()) {
        selected_term = index;
        analysis.rowid_key = *key;
        --unknown_count;
        break;
      }
    }
  } else if (analysis.empty) {
    unknown_count = 0;
  }

  if (guard_count > 0U && unknown_count > 0U) {
    analysis.guards.reserve(guard_count);
    std::size_t residual_write = 0;
    for (std::size_t index = 0; index < cutoff; ++index) {
      if (const std::optional<SqlTruthValue> truth = ConstantTruth(bound_select, terms[index]);
          truth.has_value()) {
        continue;
      }
      if (IsStatementGuard(bound_select, terms[index])) {
        analysis.guards.push_back(terms[index]);
      } else if (!selected_term.has_value() || index != *selected_term) {
        terms[residual_write++] = terms[index];
      }
    }
    terms.erase(terms.begin() + static_cast<std::ptrdiff_t>(residual_write), terms.end());
    analysis.residuals = std::move(terms);
  } else if (guard_count > 0U) {
    std::size_t guard_write = 0;
    for (std::size_t index = 0; index < cutoff; ++index) {
      if (!ConstantTruth(bound_select, terms[index]).has_value() &&
          IsStatementGuard(bound_select, terms[index])) {
        terms[guard_write++] = terms[index];
      }
    }
    terms.erase(terms.begin() + static_cast<std::ptrdiff_t>(guard_write), terms.end());
    analysis.guards = std::move(terms);
  } else if (unknown_count > 0U) {
    std::size_t residual_write = 0;
    for (std::size_t index = 0; index < cutoff; ++index) {
      if (ConstantTruth(bound_select, terms[index]).has_value() ||
          (selected_term.has_value() && index == *selected_term)) {
        continue;
      }
      terms[residual_write++] = terms[index];
    }
    terms.erase(terms.begin() + static_cast<std::ptrdiff_t>(residual_write), terms.end());
    analysis.residuals = std::move(terms);
  }
  return analysis;
}

[[nodiscard]] ReadPredicateAnalysis AnalyzeReadPredicate(const BoundQueryView& bound_select,
                                                         const SourceInfo& source,
                                                         std::optional<BoundExpressionId> where) {
  ReadPredicateAnalysis analysis;
  if (!where.has_value()) {
    return analysis;
  }

  std::vector<BoundExpressionId> terms;
  terms.reserve(CountConjuncts(bound_select, *where));
  CollectConjuncts(bound_select, *where, &terms);

  if (source.single_row) {
    analysis.guards.reserve(terms.size());
    for (const BoundExpressionId term : terms) {
      const std::optional<SqlTruthValue> truth = ConstantTruth(bound_select, term);
      if (truth.has_value() && *truth == SqlTruthValue::kTrue) {
        continue;
      }
      if (truth.has_value()) {
        analysis.empty = true;
        break;
      }
      analysis.guards.push_back(term);
    }
    return analysis;
  }

  analysis.guards.reserve(terms.size());
  analysis.terms.reserve(terms.size());
  for (std::size_t index = 0; index < terms.size(); ++index) {
    const BoundExpressionId term = terms[index];
    const std::optional<SqlTruthValue> truth = ConstantTruth(bound_select, term);
    if (truth.has_value() && *truth == SqlTruthValue::kTrue) {
      continue;
    }
    if (truth.has_value()) {
      analysis.empty = true;
      analysis.terms.clear();
      break;
    }
    if (IsStatementGuard(bound_select, term)) {
      analysis.guards.push_back(term);
    } else {
      analysis.terms.push_back(ReadPredicateTerm{
          .predicate = term,
          .conjunct_position = index,
      });
    }
  }
  return analysis;
}

[[nodiscard]] std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

[[nodiscard]] std::uint64_t SaturatingMultiply(std::uint64_t left, std::uint64_t right) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left * right;
}

[[nodiscard]] std::uint64_t AffinityWidthUnits(TypeAffinity affinity) noexcept {
  switch (affinity) {
    case TypeAffinity::kNone:
    case TypeAffinity::kText:
    case TypeAffinity::kBlob:
      return 5;
    case TypeAffinity::kInteger:
    case TypeAffinity::kReal:
    case TypeAffinity::kNumeric:
      return 1;
  }
  return 5;
}

[[nodiscard]] std::uint64_t DerivedTableRowSize(const CatalogTable& table) noexcept {
  std::uint64_t units = 0;
  for (const CatalogColumn& column : table.columns) {
    units = SaturatingAdd(units, AffinityWidthUnits(column.affinity));
  }
  if (!table.rowid_alias.has_value()) {
    units = SaturatingAdd(units, 1);
  }
  return SaturatingMultiply(units, 4);
}

[[nodiscard]] std::uint64_t DerivedIndexRowSize(const CatalogSnapshot& catalog,
                                                const CatalogIndex& index) noexcept {
  const CatalogTable& table = catalog.table(index.table);
  std::uint64_t units = 0;
  for (const CatalogIndexTerm& term : index.terms) {
    if (const auto* column = std::get_if<ColumnId>(&term.target);
        column != nullptr && column->value < table.columns.size()) {
      units = SaturatingAdd(units, AffinityWidthUnits(table.columns[column->value].affinity));
    } else if (std::holds_alternative<RowIdIndexTerm>(term.target)) {
      units = SaturatingAdd(units, 1);
    } else {
      units = SaturatingAdd(units, 5);
    }
  }
  return SaturatingMultiply(units, 4);
}

[[nodiscard]] bool IsRegisteredCollation(const BoundQueryView& bound_select,
                                         std::string_view name) noexcept {
  return std::ranges::any_of(
      bound_select.registered_collations(),
      [name](const std::string& registered) { return CatalogNamesEqual(registered, name); });
}

[[nodiscard]] bool IsSupportedIndexShape(const BoundQueryView& bound_select,
                                         const CatalogTable& table,
                                         const CatalogIndex& index) noexcept {
  if (table.without_rowid || index.partial_predicate.has_value() || index.key_term_count == 0U ||
      index.key_term_count > std::numeric_limits<std::uint32_t>::max() ||
      index.key_term_count >= index.terms.size() ||
      index.terms.size() != index.key_term_count + 1U ||
      !std::holds_alternative<RowIdIndexTerm>(index.terms.back().target)) {
    return false;
  }
  for (std::size_t term_index = 0; term_index < index.terms.size(); ++term_index) {
    const CatalogIndexTerm& term = index.terms[term_index];
    if (!IsRegisteredCollation(bound_select, term.collation_name)) {
      return false;
    }
    if (term_index < index.key_term_count) {
      const auto* column = std::get_if<ColumnId>(&term.target);
      if (column == nullptr || column->value >= table.columns.size()) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool IsIndexedColumnReference(const BoundQueryView& bound_select,
                                            BoundSourceId source_id,
                                            std::size_t source_column_begin,
                                            std::size_t source_column_count, ColumnId column_id,
                                            BoundExpressionId expression_id) noexcept {
  expression_id = TransparentRowIdOperandId(bound_select, expression_id);
  if (!IsValidExpressionId(bound_select, expression_id)) {
    return false;
  }
  const auto* column =
      std::get_if<BoundColumnExpression>(&bound_select.expression(expression_id).payload);
  const std::size_t source_index = column != nullptr ? column->column.value() : 0U;
  if (column == nullptr || source_index < source_column_begin ||
      source_index - source_column_begin >= source_column_count ||
      source_index >= bound_select.source_columns().size()) {
    return false;
  }
  const BoundSourceColumn& source = bound_select.source_columns()[source_index];
  return source.source == source_id && source.catalog_column == column_id;
}

[[nodiscard]] SqlComparison ReverseComparison(SqlComparison comparison) noexcept {
  switch (comparison) {
    case SqlComparison::kLess:
      return SqlComparison::kGreater;
    case SqlComparison::kLessEqual:
      return SqlComparison::kGreaterEqual;
    case SqlComparison::kGreater:
      return SqlComparison::kLess;
    case SqlComparison::kGreaterEqual:
      return SqlComparison::kLessEqual;
    case SqlComparison::kEqual:
    case SqlComparison::kNotEqual:
    case SqlComparison::kIs:
    case SqlComparison::kIsNot:
      return comparison;
  }
  return comparison;
}

enum class IndexConstraintKind : std::uint8_t {
  kEquality,
  kLower,
  kUpper,
};

struct IndexConstraintMatch {
  IndexConstraintKind kind = IndexConstraintKind::kEquality;
  BoundExpressionId key{0};
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
  bool inclusive = false;
  bool reject_null = true;
};

[[nodiscard]] std::optional<IndexConstraintMatch> MatchIndexConstraint(
    const BoundQueryView& bound_select, BoundSourceId source_id, std::size_t source_column_begin,
    std::size_t source_column_count, const CatalogIndexTerm& term,
    const ReadPredicateTerm& predicate) {
  const auto* indexed_column = std::get_if<ColumnId>(&term.target);
  if (indexed_column == nullptr) {
    return std::nullopt;
  }
  const BoundExpressionId transparent = TransparentPredicateId(bound_select, predicate.predicate);
  if (!IsValidExpressionId(bound_select, transparent)) {
    return std::nullopt;
  }
  const auto* comparison =
      std::get_if<BoundComparisonExpression>(&bound_select.expression(transparent).payload);
  if (comparison == nullptr || comparison->collation.value() >= bound_select.collations().size()) {
    return std::nullopt;
  }

  const bool left_column =
      IsIndexedColumnReference(bound_select, source_id, source_column_begin, source_column_count,
                               *indexed_column, comparison->left);
  const bool right_column =
      IsIndexedColumnReference(bound_select, source_id, source_column_begin, source_column_count,
                               *indexed_column, comparison->right);
  if (left_column == right_column) {
    return std::nullopt;
  }
  const BoundExpressionId key = left_column ? comparison->right : comparison->left;
  if (ExpressionDependsOnSource(bound_select, key)) {
    return std::nullopt;
  }
  const SqlComparison normalized =
      left_column ? comparison->comparison : ReverseComparison(comparison->comparison);
  const bool is_null_test = normalized == SqlComparison::kIs && IsNullLiteral(bound_select, key);
  if (!is_null_test &&
      !CatalogNamesEqual(bound_select.collations()[comparison->collation.value()].name,
                         term.collation_name)) {
    return std::nullopt;
  }
  IndexConstraintMatch match{
      .key = key,
      .predicate = predicate.predicate,
      .conjunct_position = predicate.conjunct_position,
  };
  switch (normalized) {
    case SqlComparison::kEqual:
      match.kind = IndexConstraintKind::kEquality;
      match.reject_null = true;
      return match;
    case SqlComparison::kIs:
      match.kind = IndexConstraintKind::kEquality;
      match.reject_null = false;
      return match;
    case SqlComparison::kGreater:
      match.kind = IndexConstraintKind::kLower;
      match.inclusive = false;
      return match;
    case SqlComparison::kGreaterEqual:
      match.kind = IndexConstraintKind::kLower;
      match.inclusive = true;
      return match;
    case SqlComparison::kLess:
      match.kind = IndexConstraintKind::kUpper;
      match.inclusive = false;
      return match;
    case SqlComparison::kLessEqual:
      match.kind = IndexConstraintKind::kUpper;
      match.inclusive = true;
      return match;
    case SqlComparison::kNotEqual:
    case SqlComparison::kIsNot:
      return std::nullopt;
  }
  return std::nullopt;
}

void MarkRequiredSourceValues(const BoundQueryView& bound_select, BoundSourceId source_id,
                              BoundExpressionId id, std::vector<bool>* columns, bool* rowid) {
  if (!IsValidExpressionId(bound_select, id)) {
    return;
  }
  const BoundExpression& expression = bound_select.expression(id);
  if (const auto identity = BoundSourceLeafIdentityOf(expression, bound_select.source_columns());
      identity.has_value()) {
    if (identity->source != source_id) {
      return;
    }
    if (!identity->column.has_value()) {
      *rowid = true;
    } else {
      const auto column = bound_select.source_columns()[identity->column->value()].catalog_column;
      if (column.has_value() && column->value < columns->size()) {
        (*columns)[column->value] = true;
      }
    }
    return;
  }
  std::visit(
      [&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                      std::is_same_v<Payload, BoundTruthTestExpression> ||
                      std::is_same_v<Payload, BoundLikelihoodExpression> ||
                      std::is_same_v<Payload, BoundCollateExpression>) {
          MarkRequiredSourceValues(bound_select, source_id, payload.operand, columns, rowid);
        } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression> ||
                             std::is_same_v<Payload, BoundComparisonExpression>) {
          MarkRequiredSourceValues(bound_select, source_id, payload.left, columns, rowid);
          MarkRequiredSourceValues(bound_select, source_id, payload.right, columns, rowid);
        } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
          MarkRequiredSourceValues(bound_select, source_id, payload.target, columns, rowid);
        } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression> ||
                             std::is_same_v<Payload, BoundCoalesceExpression> ||
                             std::is_same_v<Payload, BoundConditionalExpression>) {
          for (const BoundExpressionId argument : payload.arguments) {
            MarkRequiredSourceValues(bound_select, source_id, argument, columns, rowid);
          }
        }
      },
      expression.payload);
}

[[nodiscard]] bool IsCoveringIndex(const BoundQueryView& bound_select, BoundSourceId source_id,
                                   const CatalogTable& table, const CatalogIndex& index,
                                   std::span<const ReadPredicateTerm> predicates,
                                   const std::vector<bool>& selected_terms,
                                   const BoundSelectCore* aggregate_core = nullptr) {
  std::vector<bool> required_columns(table.columns.size(), false);
  bool required_rowid = false;
  for (std::size_t index_value = 0; index_value < predicates.size(); ++index_value) {
    if (index_value >= selected_terms.size() || !selected_terms[index_value]) {
      MarkRequiredSourceValues(bound_select, source_id, predicates[index_value].predicate,
                               &required_columns, &required_rowid);
    }
  }
  if (aggregate_core != nullptr && aggregate_core->aggregate.has_value()) {
    const BoundAggregateProgram& aggregate = *aggregate_core->aggregate;
    for (const BoundExpressionId expression : aggregate.source_leaves) {
      MarkRequiredSourceValues(bound_select, source_id, expression, &required_columns,
                               &required_rowid);
    }
    if (aggregate.group_by.empty()) {
      for (const BoundResultColumn& result : aggregate_core->result_columns) {
        MarkRequiredSourceValues(bound_select, source_id, result.expression, &required_columns,
                                 &required_rowid);
      }
      if (aggregate.having.has_value()) {
        MarkRequiredSourceValues(bound_select, source_id, *aggregate.having, &required_columns,
                                 &required_rowid);
      }
    } else {
      for (const BoundExpressionId expression : aggregate.representative_leaves) {
        MarkRequiredSourceValues(bound_select, source_id, expression, &required_columns,
                                 &required_rowid);
      }
    }
  } else {
    for (const BoundResultColumn& result : bound_select.result_columns()) {
      MarkRequiredSourceValues(bound_select, source_id, result.expression, &required_columns,
                               &required_rowid);
    }
    for (const BoundOrderingTerm& ordering : bound_select.order_by()) {
      MarkRequiredSourceValues(bound_select, source_id, ordering.expression, &required_columns,
                               &required_rowid);
    }
  }

  std::vector<bool> covered_columns(table.columns.size(), false);
  bool covered_rowid = false;
  for (const CatalogIndexTerm& term : index.terms) {
    if (const auto* column = std::get_if<ColumnId>(&term.target);
        column != nullptr && column->value < covered_columns.size()) {
      covered_columns[column->value] = true;
    } else if (std::holds_alternative<RowIdIndexTerm>(term.target)) {
      covered_rowid = true;
    }
  }
  if (table.rowid_alias.has_value() && covered_rowid) {
    covered_columns[table.rowid_alias->value] = true;
  }
  for (std::size_t column = 0; column < required_columns.size(); ++column) {
    if (required_columns[column] && !covered_columns[column]) {
      return false;
    }
  }
  return !required_rowid || covered_rowid;
}

[[nodiscard]] bool IsNullLiteral(const BoundQueryView& bound_select,
                                 BoundExpressionId id) noexcept {
  id = TransparentPredicateId(bound_select, id);
  if (!IsValidExpressionId(bound_select, id)) {
    return false;
  }
  const auto* literal = std::get_if<BoundLiteralExpression>(&bound_select.expression(id).payload);
  return literal != nullptr && literal->value.type() == SqlValueType::kNull;
}

[[nodiscard]] std::uint64_t DefaultPrefixRows(std::size_t equality_count) noexcept {
  constexpr std::array<std::uint64_t, 6> kDefaults{10, 9, 8, 7, 6, 5};
  return kDefaults[std::min(equality_count, kDefaults.size()) - 1U];
}

[[nodiscard]] std::uint64_t IndexCardinality(const CatalogIndex& index,
                                             std::uint64_t table_rows) noexcept {
  if (index.statistics.has_stat1 && !index.statistics.rows_per_prefix.empty()) {
    return index.statistics.rows_per_prefix.front();
  }
  return table_rows;
}

[[nodiscard]] std::uint64_t EstimateIndexOutput(const BoundQueryView& bound_select,
                                                const CatalogIndex& index,
                                                const PhysicalIndexScanNode& node,
                                                std::uint64_t index_rows) noexcept {
  std::uint64_t estimate = index_rows;
  for (std::size_t equality = 0; equality < node.equalities.size(); ++equality) {
    const std::size_t prefix = equality + 1U;
    const std::uint64_t incoming = estimate;
    const bool supplied =
        index.statistics.has_stat1 && prefix < index.statistics.rows_per_prefix.size();
    estimate = supplied ? index.statistics.rows_per_prefix[prefix]
                        : std::min(DefaultPrefixRows(prefix), estimate);
    if (!supplied && !node.equalities[equality].reject_null &&
        IsNullLiteral(bound_select, node.equalities[equality].key)) {
      estimate = std::min(SaturatingMultiply(estimate, 2), incoming);
    }
  }

  if (node.equalities.size() == index.key_term_count && index.unique) {
    const bool only_ordinary_equalities = std::ranges::all_of(
        node.equalities,
        [](const PhysicalIndexEquality& equality) { return equality.reject_null; });
    if (only_ordinary_equalities || index.unique_not_null) {
      estimate = std::min<std::uint64_t>(index_rows, 1);
    }
  }

  if (node.range.has_value()) {
    const std::uint64_t divisor =
        node.range->lower.has_value() && node.range->upper.has_value() ? 64U : 4U;
    if (estimate != 0U) {
      estimate = std::max<std::uint64_t>(estimate / divisor, 1U);
    }
  }
  return estimate;
}

[[nodiscard]] AccessPathCost ScanCost(std::uint64_t rows, std::uint64_t row_size) noexcept {
  return AccessPathCost{
      .estimated_input_rows = rows,
      .estimated_output_rows = rows,
      .work_units = SaturatingMultiply(rows, row_size),
  };
}

[[nodiscard]] AccessPathCost RowIdCost(std::uint64_t rows) noexcept {
  return AccessPathCost{
      .estimated_input_rows = rows,
      .estimated_output_rows = std::min<std::uint64_t>(rows, 1U),
      .work_units = static_cast<std::uint64_t>(std::bit_width(std::max<std::uint64_t>(rows, 1U))),
  };
}

[[nodiscard]] std::optional<BoundExpressionId> JoinedRowIdLookupKey(
    const BoundQueryView& bound_select, std::size_t core_index, BoundSourceId source_id) {
  const auto* core = std::get_if<BoundSelectCore>(&bound_select.query_cores()[core_index]);
  if (core == nullptr || core->table_sources.size() < 2U) {
    return std::nullopt;
  }
  const auto source = std::ranges::find(core->table_sources, source_id, &BoundTableSource::id);
  if (source == core->table_sources.end()) {
    return std::nullopt;
  }
  const SourceInfo info = ResolveTableSource(bound_select, *source);
  if (!info.rowid_eligible ||
      RowIdCost(info.estimated_rows).work_units >=
          ScanCost(info.estimated_rows, info.estimated_row_size).work_units) {
    return std::nullopt;
  }
  std::optional<BoundExpressionId> key;
  auto visitor = [&](BoundExpressionId predicate) {
    key = RowIdLookupKey(bound_select, info, predicate, source_id);
    return !key.has_value();
  };
  if (source->on.has_value()) {
    static_cast<void>(VisitConjuncts(bound_select, *source->on, &visitor));
  }
  const bool right_core = std::ranges::any_of(
      core->table_sources,
      [](const BoundTableSource& item) { return SourceJoinPreservesRight(item.join); });
  if (!key.has_value() && !SourceJoinPreservesLeft(source->join) && !right_core &&
      core->where.has_value()) {
    static_cast<void>(VisitConjuncts(bound_select, *core->where, &visitor));
  }
  return key;
}

[[nodiscard]] std::optional<IndexCandidatePlan> AnalyzeIndexCandidate(
    const BoundQueryView& bound_select, BoundSourceId source_id, TableId table_id,
    std::size_t source_column_begin, std::size_t source_column_count, IndexId index_id,
    std::span<const ReadPredicateTerm> predicates, const BoundSelectCore* aggregate_core = nullptr,
    bool allow_noncovering_full_scan = false) {
  const CatalogSnapshot& catalog = *bound_select.catalog();
  const CatalogTable& table = catalog.table(table_id);
  const CatalogIndex& index = catalog.index(index_id);
  if (!IsSupportedIndexShape(bound_select, table, index)) {
    return std::nullopt;
  }

  IndexCandidatePlan candidate{
      .node =
          PhysicalIndexScanNode{
              .source = source_id,
              .table = table_id,
              .table_root_page = table.root_page,
              .index = index_id,
              .index_root_page = index.root_page,
              .equalities = {},
              .range = std::nullopt,
              .covering = false,
          },
      .selected_terms = std::vector<bool>(predicates.size(), false),
  };
  candidate.node.equalities.reserve(index.key_term_count);
  std::size_t equality_count = 0;
  for (; equality_count < index.key_term_count; ++equality_count) {
    const CatalogIndexTerm& term = index.terms[equality_count];
    std::optional<std::size_t> selected;
    std::optional<IndexConstraintMatch> selected_match;
    for (std::size_t predicate_index = 0; predicate_index < predicates.size(); ++predicate_index) {
      if (candidate.selected_terms[predicate_index]) {
        continue;
      }
      const std::optional<IndexConstraintMatch> match =
          MatchIndexConstraint(bound_select, source_id, source_column_begin, source_column_count,
                               term, predicates[predicate_index]);
      if (match.has_value() && match->kind == IndexConstraintKind::kEquality) {
        selected = predicate_index;
        selected_match = match;
        break;
      }
    }
    if (!selected.has_value() || !selected_match.has_value()) {
      break;
    }
    const auto* column = std::get_if<ColumnId>(&term.target);
    if (column == nullptr || column->value >= table.columns.size()) {
      return std::nullopt;
    }
    candidate.selected_terms[*selected] = true;
    candidate.node.equalities.push_back(PhysicalIndexEquality{
        .column = *column,
        .key = selected_match->key,
        .predicate = selected_match->predicate,
        .conjunct_position = selected_match->conjunct_position,
        .affinity = table.columns[column->value].affinity,
        .collation_name = term.collation_name,
        .order = term.order,
        .reject_null = selected_match->reject_null,
    });
  }

  if (equality_count < index.key_term_count && !index.statistics.unordered) {
    const CatalogIndexTerm& term = index.terms[equality_count];
    std::optional<std::size_t> lower_index;
    std::optional<std::size_t> upper_index;
    std::optional<IndexConstraintMatch> lower;
    std::optional<IndexConstraintMatch> upper;
    for (std::size_t predicate_index = 0; predicate_index < predicates.size(); ++predicate_index) {
      if (candidate.selected_terms[predicate_index]) {
        continue;
      }
      const std::optional<IndexConstraintMatch> match =
          MatchIndexConstraint(bound_select, source_id, source_column_begin, source_column_count,
                               term, predicates[predicate_index]);
      if (!match.has_value()) {
        continue;
      }
      if (match->kind == IndexConstraintKind::kLower && !lower.has_value()) {
        lower_index = predicate_index;
        lower = match;
      } else if (match->kind == IndexConstraintKind::kUpper && !upper.has_value()) {
        upper_index = predicate_index;
        upper = match;
      }
      if (lower.has_value() && upper.has_value()) {
        break;
      }
    }
    if (lower.has_value() || upper.has_value()) {
      const auto* column = std::get_if<ColumnId>(&term.target);
      if (column == nullptr || column->value >= table.columns.size() ||
          (lower.has_value() && !lower_index.has_value()) ||
          (upper.has_value() && !upper_index.has_value())) {
        return std::nullopt;
      }
      PhysicalIndexRange range{
          .column = *column,
          .affinity = table.columns[column->value].affinity,
          .collation_name = term.collation_name,
          .order = term.order,
      };
      if (lower.has_value()) {
        candidate.selected_terms[*lower_index] = true;
        range.lower = PhysicalIndexBound{
            .key = lower->key,
            .predicate = lower->predicate,
            .conjunct_position = lower->conjunct_position,
            .inclusive = lower->inclusive,
        };
      }
      if (upper.has_value()) {
        candidate.selected_terms[*upper_index] = true;
        range.upper = PhysicalIndexBound{
            .key = upper->key,
            .predicate = upper->predicate,
            .conjunct_position = upper->conjunct_position,
            .inclusive = upper->inclusive,
        };
      }
      candidate.node.range = range;
    }
  }

  const bool constrained = !candidate.node.equalities.empty() || candidate.node.range.has_value();
  if (!constrained && index.statistics.unordered) {
    return std::nullopt;
  }
  candidate.node.covering = IsCoveringIndex(bound_select, source_id, table, index, predicates,
                                            candidate.selected_terms, aggregate_core);
  if (!constrained && !candidate.node.covering && !allow_noncovering_full_scan) {
    return std::nullopt;
  }
  return candidate;
}

[[nodiscard]] AccessPathCandidate PublishIndexCandidate(const BoundQueryView& bound_select,
                                                        const SourceInfo& source,
                                                        const IndexCandidatePlan& plan) noexcept {
  const CatalogSnapshot& catalog = *bound_select.catalog();
  const CatalogIndex& index = catalog.index(plan.node.index);
  const std::uint64_t index_rows = IndexCardinality(index, source.estimated_rows);
  const std::uint64_t row_size =
      index.statistics.average_row_size.value_or(DerivedIndexRowSize(catalog, index));
  const std::uint64_t output_rows = EstimateIndexOutput(bound_select, index, plan.node, index_rows);
  const bool constrained = !plan.node.equalities.empty() || plan.node.range.has_value();
  const std::uint64_t seek_work =
      constrained
          ? static_cast<std::uint64_t>(std::bit_width(std::max<std::uint64_t>(index_rows, 1U)))
          : 0U;
  std::uint64_t scan_work = SaturatingMultiply(output_rows, row_size);
  if (!plan.node.covering) {
    const std::uint64_t table_seek = static_cast<std::uint64_t>(
        std::bit_width(std::max<std::uint64_t>(source.estimated_rows, 1U)));
    const std::uint64_t table_lookup = SaturatingAdd(table_seek, source.estimated_row_size);
    scan_work = SaturatingAdd(scan_work, SaturatingMultiply(output_rows, table_lookup));
  }
  return AccessPathCandidate{
      .kind = PhysicalAccessKind::kIndexScan,
      .cost =
          AccessPathCost{
              .estimated_input_rows = index_rows,
              .estimated_output_rows = output_rows,
              .work_units = SaturatingAdd(seek_work, scan_work),
          },
      .index = plan.node.index,
      .covering = plan.node.covering,
      .equality_term_count = static_cast<std::uint32_t>(plan.node.equalities.size()),
      .range_bound_count = static_cast<std::uint32_t>(
          plan.node.range.has_value()
              ? static_cast<std::size_t>(plan.node.range->lower.has_value()) +
                    static_cast<std::size_t>(plan.node.range->upper.has_value())
              : 0U),
  };
}

[[nodiscard]] bool CandidateCoversRows(const AccessPathCandidate& candidate) noexcept {
  return candidate.kind != PhysicalAccessKind::kIndexScan || candidate.covering;
}

[[nodiscard]] bool PreferCandidate(const AccessPathCandidate& candidate,
                                   const AccessPathCandidate& selected) noexcept {
  if (candidate.cost.work_units != selected.cost.work_units) {
    return candidate.cost.work_units < selected.cost.work_units;
  }
  if (candidate.cost.estimated_output_rows != selected.cost.estimated_output_rows) {
    return candidate.cost.estimated_output_rows < selected.cost.estimated_output_rows;
  }
  if (CandidateCoversRows(candidate) != CandidateCoversRows(selected)) {
    return CandidateCoversRows(candidate);
  }
  const std::uint32_t candidate_prefix =
      candidate.equality_term_count + static_cast<std::uint32_t>(candidate.range_bound_count > 0U);
  const std::uint32_t selected_prefix =
      selected.equality_term_count + static_cast<std::uint32_t>(selected.range_bound_count > 0U);
  if (candidate_prefix != selected_prefix) {
    return candidate_prefix > selected_prefix;
  }
  return candidate.kind == PhysicalAccessKind::kRowIdLookup &&
         selected.kind != PhysicalAccessKind::kRowIdLookup;
}

struct DirectSourceIdentity {
  bool rowid = false;
  std::optional<ColumnId> column{};

  constexpr auto operator<=>(const DirectSourceIdentity&) const noexcept = default;
};

struct AggregateMinMaxChoice {
  PhysicalMinMaxPlan plan;
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;
};

struct AggregateGroupingChoice {
  std::vector<PhysicalGroupingOrderTerm> order{};
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;
  bool at_most_one_row = false;
};

struct AggregateCorePlan {
  bool joined = false;
  SourceInfo source{};
  ReadPredicateAnalysis predicates{};
  std::vector<ReadAccessCandidate> access_candidates{};
  std::optional<std::size_t> selected_access{};
  std::vector<BoundExpressionId> residuals{};
  PhysicalAggregateStrategy strategy = PhysicalAggregateStrategy::kGlobal;
  std::optional<PhysicalMinMaxPlan> min_max{};
  std::vector<PhysicalGroupingOrderTerm> grouping_order{};
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;
  bool at_most_one_row = false;
  bool order_satisfied = false;
};

[[nodiscard]] std::optional<DirectSourceIdentity> ResolveDirectSourceIdentity(
    const BoundQueryView& bound_select, const SourceInfo& source, BoundExpressionId id) noexcept {
  id = TransparentRowIdOperandId(bound_select, id);
  if (!IsValidExpressionId(bound_select, id)) {
    return std::nullopt;
  }
  const BoundExpression& expression = bound_select.expression(id);
  const auto identity = BoundSourceLeafIdentityOf(expression, bound_select.source_columns());
  if (!identity.has_value() || identity->source != source.id) {
    return std::nullopt;
  }
  if (!identity->column.has_value()) {
    return DirectSourceIdentity{.rowid = true, .column = std::nullopt};
  }
  const std::optional<ColumnId> catalog_column =
      bound_select.source_columns()[identity->column->value()].catalog_column;
  if (!catalog_column.has_value()) {
    return std::nullopt;
  }
  return DirectSourceIdentity{.rowid = false, .column = catalog_column};
}

[[nodiscard]] bool BoundExpressionsEquivalent(const BoundQueryView& bound_select,
                                              BoundExpressionId left_id,
                                              BoundExpressionId right_id);

[[nodiscard]] bool BoundArgumentListsEquivalent(const BoundQueryView& bound_select,
                                                std::span<const BoundExpressionId> left,
                                                std::span<const BoundExpressionId> right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!BoundExpressionsEquivalent(bound_select, left[index], right[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool LiteralValuesEquivalent(const SqlValue& left, const SqlValue& right) {
  if (left.type() != right.type()) {
    return false;
  }
  switch (left.type()) {
    case SqlValueType::kNull:
      return true;
    case SqlValueType::kInteger:
      return left.integer_value() == right.integer_value();
    case SqlValueType::kReal:
      return left.real_value() == right.real_value();
    case SqlValueType::kText:
      return left.text_value().value_or(Utf8View{}).bytes() ==
             right.text_value().value_or(Utf8View{}).bytes();
    case SqlValueType::kBlob:
      return std::ranges::equal(left.blob_value().value_or(ByteView{}),
                                right.blob_value().value_or(ByteView{}));
  }
  return false;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] bool BoundExpressionsEquivalent(const BoundQueryView& bound_select,
                                              BoundExpressionId left_id,
                                              BoundExpressionId right_id) {
  const BoundExpression& left = bound_select.expression(left_id);
  const BoundExpression& right = bound_select.expression(right_id);
  if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&left.payload);
      alias != nullptr) {
    return BoundExpressionsEquivalent(bound_select, alias->target, right_id);
  }
  if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&right.payload);
      alias != nullptr) {
    return BoundExpressionsEquivalent(bound_select, left_id, alias->target);
  }
  if (left.payload.index() != right.payload.index()) {
    return false;
  }
  if (const auto* literal = std::get_if<BoundLiteralExpression>(&left.payload);
      literal != nullptr) {
    return LiteralValuesEquivalent(literal->value,
                                   std::get<BoundLiteralExpression>(right.payload).value);
  }
  if (const auto* rowid = std::get_if<BoundRowIdExpression>(&left.payload); rowid != nullptr) {
    return rowid->source == std::get<BoundRowIdExpression>(right.payload).source;
  }
  if (const auto* column = std::get_if<BoundColumnExpression>(&left.payload); column != nullptr) {
    return column->column == std::get<BoundColumnExpression>(right.payload).column;
  }
  if (const auto* parameter = std::get_if<BoundParameterExpression>(&left.payload);
      parameter != nullptr) {
    return parameter->parameter == std::get<BoundParameterExpression>(right.payload).parameter;
  }
  if (const auto* unary = std::get_if<BoundUnaryExpression>(&left.payload); unary != nullptr) {
    const auto& other = std::get<BoundUnaryExpression>(right.payload);
    return unary->operation == other.operation &&
           BoundExpressionsEquivalent(bound_select, unary->operand, other.operand);
  }
  if (const auto* binary = std::get_if<BoundBinaryExpression>(&left.payload); binary != nullptr) {
    const auto& other = std::get<BoundBinaryExpression>(right.payload);
    return binary->operation == other.operation &&
           BoundExpressionsEquivalent(bound_select, binary->left, other.left) &&
           BoundExpressionsEquivalent(bound_select, binary->right, other.right);
  }
  if (const auto* comparison = std::get_if<BoundComparisonExpression>(&left.payload);
      comparison != nullptr) {
    const auto& other = std::get<BoundComparisonExpression>(right.payload);
    return comparison->comparison == other.comparison && comparison->collation == other.collation &&
           BoundExpressionsEquivalent(bound_select, comparison->left, other.left) &&
           BoundExpressionsEquivalent(bound_select, comparison->right, other.right);
  }
  if (const auto* truth = std::get_if<BoundTruthTestExpression>(&left.payload); truth != nullptr) {
    const auto& other = std::get<BoundTruthTestExpression>(right.payload);
    return truth->expected == other.expected && truth->negated == other.negated &&
           BoundExpressionsEquivalent(bound_select, truth->operand, other.operand);
  }
  if (const auto* call = std::get_if<BoundScalarCallExpression>(&left.payload); call != nullptr) {
    const auto& other = std::get<BoundScalarCallExpression>(right.payload);
    return call->function == other.function && call->collation == other.collation &&
           BoundArgumentListsEquivalent(bound_select, call->arguments, other.arguments);
  }
  if (const auto* call = std::get_if<BoundAggregateCallExpression>(&left.payload);
      call != nullptr) {
    return call->invocation == std::get<BoundAggregateCallExpression>(right.payload).invocation;
  }
  if (const auto* coalesce = std::get_if<BoundCoalesceExpression>(&left.payload);
      coalesce != nullptr) {
    return BoundArgumentListsEquivalent(bound_select, coalesce->arguments,
                                        std::get<BoundCoalesceExpression>(right.payload).arguments);
  }
  if (const auto* conditional = std::get_if<BoundConditionalExpression>(&left.payload);
      conditional != nullptr) {
    return BoundArgumentListsEquivalent(
        bound_select, conditional->arguments,
        std::get<BoundConditionalExpression>(right.payload).arguments);
  }
  if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&left.payload);
      likelihood != nullptr) {
    const auto& other = std::get<BoundLikelihoodExpression>(right.payload);
    return likelihood->probability == other.probability &&
           BoundExpressionsEquivalent(bound_select, likelihood->operand, other.operand);
  }
  if (const auto* subquery = std::get_if<BoundSubqueryExpression>(&left.payload);
      subquery != nullptr) {
    return subquery->query == std::get<BoundSubqueryExpression>(right.payload).query;
  }
  const auto& collate = std::get<BoundCollateExpression>(left.payload);
  const auto& other = std::get<BoundCollateExpression>(right.payload);
  return collate.collation == other.collation &&
         BoundExpressionsEquivalent(bound_select, collate.operand, other.operand);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] bool UsesDefaultNullPlacement(const BoundOrderingTerm& term) noexcept {
  return term.null_placement == (term.order == SortOrder::kDescending ? BoundNullPlacement::kLast
                                                                      : BoundNullPlacement::kFirst);
}

[[nodiscard]] bool OrderMatchesGrouping(const BoundQueryView& bound_select,
                                        const BoundAggregateProgram& aggregate) {
  if (bound_select.order_by().size() != aggregate.group_by.size()) {
    return false;
  }
  const SourceInfo source = ResolveCoreSource(bound_select, 0);
  for (std::size_t term = 0; term < aggregate.group_by.size(); ++term) {
    const BoundOrderingTerm& ordering = bound_select.order_by()[term];
    const BoundGroupingTerm& grouping = aggregate.group_by[term];
    const std::optional<DirectSourceIdentity> ordering_identity =
        ResolveDirectSourceIdentity(bound_select, source, ordering.expression);
    const std::optional<DirectSourceIdentity> grouping_identity =
        ResolveDirectSourceIdentity(bound_select, source, grouping.expression);
    const bool equivalent =
        ordering_identity.has_value() && grouping_identity.has_value()
            ? *ordering_identity == *grouping_identity
            : BoundExpressionsEquivalent(bound_select, ordering.expression, grouping.expression);
    if (ordering.collation != grouping.collation || !UsesDefaultNullPlacement(ordering) ||
        !equivalent) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool GroupTermFixedByCandidate(const BoundQueryView& bound_select,
                                             const SourceInfo& source,
                                             const BoundGroupingTerm& group_term,
                                             const ReadAccessCandidate& candidate);

void ConfigureSortedGrouping(const BoundAggregateProgram& aggregate, AggregateCorePlan& plan) {
  plan.strategy = PhysicalAggregateStrategy::kSortedGroup;
  plan.grouping_order.clear();
  plan.grouping_order.reserve(aggregate.group_by.size());
  for (std::size_t term = 0; term < aggregate.group_by.size(); ++term) {
    plan.grouping_order.push_back(PhysicalGroupingOrderTerm{
        .group_term_index = static_cast<std::uint32_t>(term),
        .order = SortOrder::kAscending,
        .null_placement = BoundNullPlacement::kFirst,
    });
  }
}

void ConfigureAggregateOrder(const BoundQueryView& bound_select,
                             const BoundAggregateProgram& aggregate, AggregateCorePlan* plan) {
  if (bound_select.query_cores().size() != 1U || !bound_select.compound_operators().empty() ||
      bound_select.order_by().empty()) {
    return;
  }
  if (plan->strategy == PhysicalAggregateStrategy::kGlobal ||
      plan->strategy == PhysicalAggregateStrategy::kIndexedMinMax) {
    plan->order_satisfied = true;
    return;
  }
  if (!OrderMatchesGrouping(bound_select, aggregate)) {
    return;
  }
  if (plan->at_most_one_row) {
    plan->order_satisfied = true;
    return;
  }
  if (plan->strategy == PhysicalAggregateStrategy::kSortedGroup) {
    plan->grouping_order.clear();
    plan->grouping_order.reserve(aggregate.group_by.size());
    for (std::size_t term = 0; term < aggregate.group_by.size(); ++term) {
      const BoundOrderingTerm& ordering = bound_select.order_by()[term];
      plan->grouping_order.push_back(PhysicalGroupingOrderTerm{
          .group_term_index = static_cast<std::uint32_t>(term),
          .order = ordering.order,
          .null_placement = ordering.null_placement,
      });
    }
    plan->order_satisfied = true;
    return;
  }
  if (plan->strategy != PhysicalAggregateStrategy::kOrderedGroup ||
      !plan->selected_access.has_value()) {
    return;
  }

  const ReadAccessCandidate& selected = plan->access_candidates[*plan->selected_access];
  std::vector<std::size_t> varying_terms;
  varying_terms.reserve(aggregate.group_by.size());
  for (std::size_t term = 0; term < aggregate.group_by.size(); ++term) {
    if (!GroupTermFixedByCandidate(bound_select, plan->source, aggregate.group_by[term],
                                   selected)) {
      varying_terms.push_back(term);
    }
  }
  if (plan->grouping_order.size() != varying_terms.size()) {
    return;
  }
  bool forward = true;
  bool reverse = true;
  for (std::size_t order_index = 0; order_index < plan->grouping_order.size(); ++order_index) {
    const PhysicalGroupingOrderTerm& delivered = plan->grouping_order[order_index];
    const std::size_t group_term = varying_terms[order_index];
    const BoundOrderingTerm& requested = bound_select.order_by()[group_term];
    if (delivered.group_term_index != group_term) {
      forward = false;
      reverse = false;
      break;
    }
    forward = forward && delivered.order == requested.order &&
              delivered.null_placement == requested.null_placement;
    const SortOrder reverse_order =
        delivered.order == SortOrder::kDescending ? SortOrder::kAscending : SortOrder::kDescending;
    const BoundNullPlacement reverse_nulls = reverse_order == SortOrder::kDescending
                                                 ? BoundNullPlacement::kLast
                                                 : BoundNullPlacement::kFirst;
    reverse =
        reverse && reverse_order == requested.order && reverse_nulls == requested.null_placement;
  }
  if (forward) {
    plan->order_satisfied = true;
  } else if (reverse) {
    plan->direction = PhysicalScanDirection::kReverse;
    for (PhysicalGroupingOrderTerm& term : plan->grouping_order) {
      term.order =
          term.order == SortOrder::kDescending ? SortOrder::kAscending : SortOrder::kDescending;
      term.null_placement = term.order == SortOrder::kDescending ? BoundNullPlacement::kLast
                                                                 : BoundNullPlacement::kFirst;
    }
    plan->order_satisfied = true;
  }
}

[[nodiscard]] bool CandidateHasExactOneRow(const BoundQueryView& bound_select,
                                           const ReadAccessCandidate& candidate) noexcept {
  if (candidate.published.kind == PhysicalAccessKind::kRowIdLookup) {
    return true;
  }
  const auto* index = std::get_if<IndexCandidatePlan>(&candidate.payload);
  if (index == nullptr || (!index->node.range.has_value() && index->node.equalities.empty())) {
    return false;
  }
  const CatalogIndex& catalog_index = bound_select.catalog()->index(index->node.index);
  if (!catalog_index.unique || index->node.equalities.size() != catalog_index.key_term_count) {
    return false;
  }
  return catalog_index.unique_not_null ||
         std::ranges::all_of(index->node.equalities, [](const PhysicalIndexEquality& equality) {
           return equality.reject_null;
         });
}

[[nodiscard]] bool GroupTermFixedByCandidate(const BoundQueryView& bound_select,
                                             const SourceInfo& source,
                                             const BoundGroupingTerm& group_term,
                                             const ReadAccessCandidate& candidate) {
  if (!ExpressionDependsOnSource(bound_select, group_term.expression)) {
    return ExpressionIsDeterministic(bound_select, group_term.expression);
  }
  const std::optional<DirectSourceIdentity> identity =
      ResolveDirectSourceIdentity(bound_select, source, group_term.expression);
  if (!identity.has_value()) {
    return false;
  }
  if (identity->rowid && candidate.published.kind == PhysicalAccessKind::kRowIdLookup) {
    return true;
  }
  const auto* index = std::get_if<IndexCandidatePlan>(&candidate.payload);
  if (index == nullptr || !identity->column.has_value()) {
    return false;
  }
  const std::string& grouping_collation =
      bound_select.collations()[group_term.collation.value()].name;
  return std::ranges::any_of(index->node.equalities, [&](const PhysicalIndexEquality& equality) {
    return equality.column == *identity->column &&
           CatalogNamesEqual(equality.collation_name, grouping_collation);
  });
}

[[nodiscard]] std::optional<AggregateGroupingChoice> AnalyzeGroupingCandidate(
    const BoundQueryView& bound_select, const SourceInfo& source,
    const BoundAggregateProgram& aggregate, const ReadAccessCandidate& candidate) {
  std::vector<bool> fixed(aggregate.group_by.size(), false);
  bool all_fixed = true;
  for (std::size_t term = 0; term < aggregate.group_by.size(); ++term) {
    fixed[term] =
        GroupTermFixedByCandidate(bound_select, source, aggregate.group_by[term], candidate);
    all_fixed = all_fixed && fixed[term];
  }
  if (all_fixed || CandidateHasExactOneRow(bound_select, candidate)) {
    return AggregateGroupingChoice{
        .order = {},
        .direction = PhysicalScanDirection::kForward,
        .at_most_one_row = true,
    };
  }

  std::vector<std::size_t> remaining;
  remaining.reserve(aggregate.group_by.size());
  for (std::size_t term = 0; term < fixed.size(); ++term) {
    if (!fixed[term]) {
      remaining.push_back(term);
    }
  }

  if (candidate.published.kind == PhysicalAccessKind::kTableScan) {
    if (remaining.size() != 1U) {
      return std::nullopt;
    }
    const std::optional<DirectSourceIdentity> identity = ResolveDirectSourceIdentity(
        bound_select, source, aggregate.group_by[remaining.front()].expression);
    if (!identity.has_value() || !identity->rowid || !source.rowid_eligible) {
      return std::nullopt;
    }
    return AggregateGroupingChoice{
        .order =
            {
                PhysicalGroupingOrderTerm{
                    .group_term_index = static_cast<std::uint32_t>(remaining.front()),
                    .order = SortOrder::kAscending,
                    .null_placement = BoundNullPlacement::kFirst,
                },
            },
        .direction = PhysicalScanDirection::kForward,
        .at_most_one_row = false,
    };
  }

  const auto* index_plan = std::get_if<IndexCandidatePlan>(&candidate.payload);
  if (index_plan == nullptr) {
    return std::nullopt;
  }
  const CatalogIndex& index = bound_select.catalog()->index(index_plan->node.index);
  if (index.statistics.unordered) {
    return std::nullopt;
  }
  const std::size_t prefix = index_plan->node.equalities.size();
  if (remaining.size() > index.key_term_count - std::min(prefix, index.key_term_count)) {
    return std::nullopt;
  }

  std::vector<bool> matched(aggregate.group_by.size(), false);
  std::vector<PhysicalGroupingOrderTerm> order;
  order.reserve(remaining.size());
  for (std::size_t offset = 0; offset < remaining.size(); ++offset) {
    const std::size_t index_term = prefix + offset;
    if (index_term >= index.key_term_count) {
      return std::nullopt;
    }
    const CatalogIndexTerm& catalog_term = index.terms[index_term];
    const auto* column = std::get_if<ColumnId>(&catalog_term.target);
    if (column == nullptr) {
      return std::nullopt;
    }
    std::optional<std::size_t> selected_group;
    for (const std::size_t group_term : remaining) {
      if (matched[group_term]) {
        continue;
      }
      const BoundGroupingTerm& grouping = aggregate.group_by[group_term];
      const std::optional<DirectSourceIdentity> identity =
          ResolveDirectSourceIdentity(bound_select, source, grouping.expression);
      if (identity.has_value() && identity->column == *column &&
          CatalogNamesEqual(bound_select.collations()[grouping.collation.value()].name,
                            catalog_term.collation_name)) {
        selected_group = group_term;
        break;
      }
    }
    if (!selected_group.has_value()) {
      return std::nullopt;
    }
    matched[*selected_group] = true;
    const bool descending = catalog_term.order == SortOrder::kDescending;
    order.push_back(PhysicalGroupingOrderTerm{
        .group_term_index = static_cast<std::uint32_t>(*selected_group),
        .order = descending ? SortOrder::kDescending : SortOrder::kAscending,
        .null_placement = descending ? BoundNullPlacement::kLast : BoundNullPlacement::kFirst,
    });
  }
  return AggregateGroupingChoice{
      .order = std::move(order),
      .direction = PhysicalScanDirection::kForward,
      .at_most_one_row = false,
  };
}

[[nodiscard]] std::optional<AggregateMinMaxChoice> AnalyzeMinMaxCandidate(
    const BoundQueryView& bound_select, const SourceInfo& source,
    const BoundAggregateProgram& aggregate, const ReadAccessCandidate& candidate) {
  if (aggregate.having.has_value() || aggregate.invocations.size() != 1U) {
    return std::nullopt;
  }
  const BoundAggregateInvocationId invocation_id = aggregate.invocations.front();
  const BoundAggregateInvocation& invocation =
      bound_select.aggregate_invocations()[invocation_id.value()];
  const BoundAggregateFunction& function =
      bound_select.aggregate_functions()[invocation.function.value()];
  if (function.extremum == BoundAggregateExtremumKind::kNone || invocation.arguments.size() != 1U ||
      invocation.distinct || !invocation.order_by.empty() || invocation.filter.has_value()) {
    return std::nullopt;
  }
  const std::optional<DirectSourceIdentity> identity =
      ResolveDirectSourceIdentity(bound_select, source, invocation.arguments.front());
  if (!identity.has_value()) {
    return std::nullopt;
  }
  const bool minimum = function.extremum == BoundAggregateExtremumKind::kMinimum;
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;
  bool fixed = false;

  if (candidate.published.kind == PhysicalAccessKind::kRowIdLookup && identity->rowid) {
    fixed = true;
  } else if (candidate.published.kind == PhysicalAccessKind::kTableScan && identity->rowid &&
             source.rowid_eligible) {
    direction = minimum ? PhysicalScanDirection::kForward : PhysicalScanDirection::kReverse;
  } else {
    const auto* index_plan = std::get_if<IndexCandidatePlan>(&candidate.payload);
    if (index_plan == nullptr || !identity->column.has_value()) {
      return std::nullopt;
    }
    const CatalogIndex& index = bound_select.catalog()->index(index_plan->node.index);
    const std::string& invocation_collation =
        bound_select.collations()[invocation.collation.value()].name;
    for (const PhysicalIndexEquality& equality : index_plan->node.equalities) {
      if (equality.column == *identity->column &&
          CatalogNamesEqual(equality.collation_name, invocation_collation)) {
        fixed = true;
        break;
      }
    }
    if (!fixed) {
      const std::size_t term_index = index_plan->node.equalities.size();
      if (term_index >= index.key_term_count || index.statistics.unordered) {
        return std::nullopt;
      }
      const CatalogIndexTerm& term = index.terms[term_index];
      const auto* column = std::get_if<ColumnId>(&term.target);
      if (column == nullptr || *column != *identity->column ||
          !CatalogNamesEqual(term.collation_name, invocation_collation)) {
        return std::nullopt;
      }
      const bool physical_descending = term.order == SortOrder::kDescending;
      const bool desired_descending = !minimum;
      direction = physical_descending != desired_descending ? PhysicalScanDirection::kReverse
                                                            : PhysicalScanDirection::kForward;
    }
  }

  PhysicalMinMaxNullStrategy null_strategy = PhysicalMinMaxNullStrategy::kNone;
  if (!identity->rowid && identity->column.has_value() && source.table.has_value()) {
    const CatalogColumn& column =
        bound_select.catalog()->table(*source.table).columns[identity->column->value];
    if (!column.effective_not_null_conflict.has_value()) {
      null_strategy = PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback;
    }
  }
  return AggregateMinMaxChoice{
      .plan =
          PhysicalMinMaxPlan{
              .invocation = invocation_id,
              .null_strategy = null_strategy,
              .argument_fixed_by_equality = fixed,
          },
      .direction = direction,
  };
}

[[nodiscard]] AggregateCorePlan BuildAggregateCorePlan(const BoundQueryView& bound_select,
                                                       std::size_t core_index) {
  const auto& core = std::get<BoundSelectCore>(bound_select.query_cores()[core_index]);
  const BoundAggregateProgram& aggregate = AssumePresent(core.aggregate);
  AggregateCorePlan plan;
  if (core.table_sources.size() > 1U) {
    plan.joined = true;
    if (aggregate.group_by.empty()) {
      plan.strategy = PhysicalAggregateStrategy::kGlobal;
    } else {
      ConfigureSortedGrouping(aggregate, plan);
    }
    plan.at_most_one_row = aggregate.group_by.empty();
    ConfigureAggregateOrder(bound_select, aggregate, &plan);
    return plan;
  }
  plan.source = ResolveCoreSource(bound_select, core_index);
  plan.predicates = AnalyzeReadPredicate(bound_select, plan.source, core.where);
  if (plan.predicates.empty) {
    plan.strategy = aggregate.group_by.empty() ? PhysicalAggregateStrategy::kGlobal
                                               : PhysicalAggregateStrategy::kOrderedGroup;
    plan.at_most_one_row = true;
    ConfigureAggregateOrder(bound_select, aggregate, &plan);
    return plan;
  }
  if (plan.source.single_row) {
    plan.strategy = aggregate.group_by.empty() ? PhysicalAggregateStrategy::kGlobal
                                               : PhysicalAggregateStrategy::kOrderedGroup;
    plan.at_most_one_row = true;
    ConfigureAggregateOrder(bound_select, aggregate, &plan);
    return plan;
  }

  std::size_t index_count = 0;
  if (plan.source.source_kind == BoundSourceKind::kCatalogTable && plan.source.table.has_value()) {
    index_count = bound_select.catalog()->table_indexes(*plan.source.table).size();
  }
  plan.access_candidates.reserve(2U + index_count);
  plan.access_candidates.push_back(ReadAccessCandidate{
      .published =
          AccessPathCandidate{
              .kind = PhysicalAccessKind::kTableScan,
              .cost = ScanCost(plan.source.estimated_rows, plan.source.estimated_row_size),
          },
  });
  for (std::size_t term = 0; term < plan.predicates.terms.size(); ++term) {
    const std::optional<BoundExpressionId> key =
        RowIdLookupKey(bound_select, plan.source, plan.predicates.terms[term].predicate);
    if (key.has_value()) {
      plan.access_candidates.push_back(ReadAccessCandidate{
          .published =
              AccessPathCandidate{
                  .kind = PhysicalAccessKind::kRowIdLookup,
                  .cost = RowIdCost(plan.source.estimated_rows),
              },
          .payload =
              RowIdCandidatePlan{
                  .key = *key,
                  .selected_term = term,
              },
      });
      break;
    }
  }
  if (plan.source.source_kind == BoundSourceKind::kCatalogTable && plan.source.table.has_value() &&
      plan.source.rowid_eligible) {
    for (const IndexId index_id : bound_select.catalog()->table_indexes(*plan.source.table)) {
      std::optional<IndexCandidatePlan> candidate = AnalyzeIndexCandidate(
          bound_select, plan.source.id, *plan.source.table, core.source_column_begin,
          core.source_column_count, index_id, plan.predicates.terms, &core, true);
      if (!candidate.has_value()) {
        continue;
      }
      plan.access_candidates.push_back(ReadAccessCandidate{
          .published = PublishIndexCandidate(bound_select, plan.source, *candidate),
          .payload = std::move(*candidate),
      });
    }
  }

  std::size_t normal_access = 0;
  for (std::size_t candidate = 1; candidate < plan.access_candidates.size(); ++candidate) {
    const auto* index = std::get_if<IndexCandidatePlan>(&plan.access_candidates[candidate].payload);
    const bool normal_eligible = index == nullptr || index->node.covering ||
                                 !index->node.equalities.empty() || index->node.range.has_value();
    if (normal_eligible && PreferCandidate(plan.access_candidates[candidate].published,
                                           plan.access_candidates[normal_access].published)) {
      normal_access = candidate;
    }
  }

  std::optional<std::size_t> selected_ordered;
  std::optional<AggregateMinMaxChoice> min_max;
  if (aggregate.group_by.empty()) {
    for (std::size_t candidate = 0; candidate < plan.access_candidates.size(); ++candidate) {
      const std::optional<AggregateMinMaxChoice> choice = AnalyzeMinMaxCandidate(
          bound_select, plan.source, aggregate, plan.access_candidates[candidate]);
      if (!choice.has_value()) {
        continue;
      }
      if (!selected_ordered.has_value() ||
          PreferCandidate(plan.access_candidates[candidate].published,
                          plan.access_candidates[*selected_ordered].published)) {
        selected_ordered = candidate;
        min_max = choice;
      }
    }
    if (selected_ordered.has_value()) {
      plan.strategy = PhysicalAggregateStrategy::kIndexedMinMax;
      const AggregateMinMaxChoice& selected_min_max = AssumePresent(min_max);
      plan.min_max = selected_min_max.plan;
      plan.direction = selected_min_max.direction;
      plan.at_most_one_row = true;
    } else {
      plan.strategy = PhysicalAggregateStrategy::kGlobal;
      plan.at_most_one_row = true;
    }
  } else {
    std::optional<AggregateGroupingChoice> grouping;
    for (std::size_t candidate = 0; candidate < plan.access_candidates.size(); ++candidate) {
      std::optional<AggregateGroupingChoice> choice = AnalyzeGroupingCandidate(
          bound_select, plan.source, aggregate, plan.access_candidates[candidate]);
      if (!choice.has_value()) {
        continue;
      }
      if (!selected_ordered.has_value() ||
          PreferCandidate(plan.access_candidates[candidate].published,
                          plan.access_candidates[*selected_ordered].published)) {
        selected_ordered = candidate;
        grouping = std::move(choice);
      }
    }
    if (selected_ordered.has_value()) {
      if (!grouping.has_value()) {
        std::terminate();
      }
      AggregateGroupingChoice& selected_grouping = *grouping;
      plan.strategy = PhysicalAggregateStrategy::kOrderedGroup;
      plan.grouping_order = std::move(selected_grouping.order);
      plan.direction = selected_grouping.direction;
      plan.at_most_one_row = selected_grouping.at_most_one_row;
    } else {
      ConfigureSortedGrouping(aggregate, plan);
    }
  }

  plan.selected_access = selected_ordered.value_or(normal_access);
  std::vector<bool> selected_terms(plan.predicates.terms.size(), false);
  const ReadAccessCandidate& selected = plan.access_candidates[*plan.selected_access];
  if (const auto* rowid = std::get_if<RowIdCandidatePlan>(&selected.payload); rowid != nullptr) {
    selected_terms[rowid->selected_term] = true;
  } else if (const auto* index = std::get_if<IndexCandidatePlan>(&selected.payload);
             index != nullptr) {
    selected_terms = index->selected_terms;
  }
  plan.residuals.reserve(plan.predicates.terms.size());
  for (std::size_t term = 0; term < plan.predicates.terms.size(); ++term) {
    if (!selected_terms[term]) {
      plan.residuals.push_back(plan.predicates.terms[term].predicate);
    }
  }
  ConfigureAggregateOrder(bound_select, aggregate, &plan);
  return plan;
}

[[nodiscard]] PhysicalAccessKind AccessKindOf(const PhysicalNode& node) noexcept {
  switch (PhysicalNodeKindOf(node)) {
    case PhysicalNodeKind::kSingleRow:
      return PhysicalAccessKind::kSingleRow;
    case PhysicalNodeKind::kEmpty:
      return PhysicalAccessKind::kEmpty;
    case PhysicalNodeKind::kTableScan:
      return PhysicalAccessKind::kTableScan;
    case PhysicalNodeKind::kRowIdLookup:
      return PhysicalAccessKind::kRowIdLookup;
    case PhysicalNodeKind::kIndexScan:
      return PhysicalAccessKind::kIndexScan;
    case PhysicalNodeKind::kGuard:
    case PhysicalNodeKind::kFilter:
    case PhysicalNodeKind::kLimit:
    case PhysicalNodeKind::kProjection:
    case PhysicalNodeKind::kSort:
    case PhysicalNodeKind::kOutput:
    case PhysicalNodeKind::kValues:
    case PhysicalNodeKind::kDistinct:
    case PhysicalNodeKind::kCompound:
    case PhysicalNodeKind::kAdvancedOrder:
    case PhysicalNodeKind::kAggregate:
    case PhysicalNodeKind::kHaving:
    case PhysicalNodeKind::kNestedLoopJoin:
      break;
  }
  return PhysicalAccessKind::kEmpty;
}

[[nodiscard]] std::expected<void, OptimizerError> ValidateCandidates(
    const BoundQueryView& bound_select, const SourceInfo& source, const PhysicalNode& leaf,
    std::span<const AccessPathCandidate> candidates, std::size_t selected_candidate_index) {
  const auto matches = [&](std::size_t index, PhysicalAccessKind kind, AccessPathCost cost) {
    return index < candidates.size() && candidates[index].kind == kind &&
           candidates[index].cost == cost;
  };

  if (std::holds_alternative<PhysicalEmptyNode>(leaf.payload)) {
    if (candidates.size() != 1U || selected_candidate_index != 0U ||
        !matches(0, PhysicalAccessKind::kEmpty, AccessPathCost{})) {
      return std::unexpected{InvariantFailure("empty access candidate is invalid")};
    }
    return {};
  }
  if (std::holds_alternative<PhysicalSingleRowNode>(leaf.payload)) {
    constexpr AccessPathCost kSingleRowCost{
        .estimated_input_rows = 1,
        .estimated_output_rows = 1,
        .work_units = 1,
    };
    if (candidates.size() != 1U || selected_candidate_index != 0U ||
        !matches(0, PhysicalAccessKind::kSingleRow, kSingleRowCost)) {
      return std::unexpected{InvariantFailure("single-row access candidate is invalid")};
    }
    return {};
  }
  if (candidates.empty() || selected_candidate_index >= candidates.size() ||
      !matches(0, PhysicalAccessKind::kTableScan,
               ScanCost(source.estimated_rows, source.estimated_row_size))) {
    return std::unexpected{InvariantFailure("table-scan access candidate is invalid")};
  }

  std::size_t candidate_index = 1;
  if (candidate_index < candidates.size() &&
      candidates[candidate_index].kind == PhysicalAccessKind::kRowIdLookup) {
    if (!source.rowid_eligible ||
        candidates[candidate_index].cost != RowIdCost(source.estimated_rows) ||
        candidates[candidate_index].index.has_value()) {
      return std::unexpected{InvariantFailure("rowid access candidates are invalid")};
    }
    ++candidate_index;
  }
  std::size_t table_index_position = 0;
  const CatalogSnapshot* catalog = bound_select.catalog();
  const std::span<const IndexId> table_indexes = source.table.has_value() && catalog != nullptr
                                                     ? catalog->table_indexes(*source.table)
                                                     : std::span<const IndexId>{};
  for (; candidate_index < candidates.size(); ++candidate_index) {
    const AccessPathCandidate& candidate = candidates[candidate_index];
    if (candidate.kind != PhysicalAccessKind::kIndexScan || !candidate.index.has_value()) {
      return std::unexpected{InvariantFailure("index access candidate is invalid")};
    }
    while (table_index_position < table_indexes.size() &&
           table_indexes[table_index_position] != *candidate.index) {
      ++table_index_position;
    }
    if (table_index_position >= table_indexes.size()) {
      return std::unexpected{InvariantFailure("index access candidate order is invalid")};
    }
    ++table_index_position;
  }

  const AccessPathCandidate& selected = candidates[selected_candidate_index];
  if (AccessKindOf(leaf) != selected.kind) {
    return std::unexpected{InvariantFailure("selected access candidate does not match its leaf")};
  }
  if (std::holds_alternative<PhysicalTableScanNode>(leaf.payload)) {
    if (selected_candidate_index != 0U) {
      return std::unexpected{InvariantFailure("selected table scan is not the first candidate")};
    }
    return {};
  }
  if (std::holds_alternative<PhysicalRowIdLookupNode>(leaf.payload)) {
    if (selected.kind != PhysicalAccessKind::kRowIdLookup ||
        selected.cost != RowIdCost(source.estimated_rows)) {
      return std::unexpected{InvariantFailure("selected rowid candidate is invalid")};
    }
    return {};
  }
  if (const auto* index = std::get_if<PhysicalIndexScanNode>(&leaf.payload); index != nullptr) {
    if (selected.kind != PhysicalAccessKind::kIndexScan || selected.index != index->index ||
        selected.covering != index->covering ||
        selected.equality_term_count != index->equalities.size() ||
        selected.range_bound_count !=
            static_cast<std::uint32_t>(
                index->range.has_value()
                    ? static_cast<std::size_t>(index->range->lower.has_value()) +
                          static_cast<std::size_t>(index->range->upper.has_value())
                    : 0U)) {
      return std::unexpected{InvariantFailure("selected index candidate is invalid")};
    }
    return {};
  }
  return std::unexpected{InvariantFailure("physical plan does not begin with an access node")};
}

[[nodiscard]] std::expected<void, OptimizerError> ValidatePredicates(
    const BoundQueryView& bound_select, const SourceInfo& source, const PhysicalNode& leaf,
    std::span<const BoundExpressionId> guard_predicates,
    std::span<const BoundExpressionId> filter_predicates) {
  const bool empty_access = std::holds_alternative<PhysicalEmptyNode>(leaf.payload);
  const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&leaf.payload);
  const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&leaf.payload);
  std::vector<std::pair<std::size_t, BoundExpressionId>> selected_index_predicates;
  if (index_scan != nullptr) {
    selected_index_predicates.reserve(
        index_scan->equalities.size() +
        static_cast<std::size_t>(index_scan->range.has_value() &&
                                 index_scan->range->lower.has_value()) +
        static_cast<std::size_t>(index_scan->range.has_value() &&
                                 index_scan->range->upper.has_value()));
    for (const PhysicalIndexEquality& equality : index_scan->equalities) {
      selected_index_predicates.emplace_back(equality.conjunct_position, equality.predicate);
    }
    if (index_scan->range.has_value() && index_scan->range->lower.has_value()) {
      selected_index_predicates.emplace_back(index_scan->range->lower->conjunct_position,
                                             index_scan->range->lower->predicate);
    }
    if (index_scan->range.has_value() && index_scan->range->upper.has_value()) {
      selected_index_predicates.emplace_back(index_scan->range->upper->conjunct_position,
                                             index_scan->range->upper->predicate);
    }
    std::ranges::sort(selected_index_predicates, {}, [](const auto& entry) { return entry.first; });
  }
  std::size_t guard_index = 0;
  std::size_t filter_index = 0;
  std::size_t selected_index_predicate = 0;
  bool known_false = false;
  bool selected_rowid = false;
  bool invalid = false;

  const auto visit = [&](BoundExpressionId predicate) {
    const std::optional<SqlTruthValue> truth = ConstantTruth(bound_select, predicate);
    if (truth.has_value() && *truth == SqlTruthValue::kTrue) {
      return true;
    }
    if (truth.has_value()) {
      known_false = true;
      return false;
    }
    if (source.single_row) {
      if (guard_index >= guard_predicates.size() || guard_predicates[guard_index] != predicate) {
        invalid = true;
        return false;
      }
      ++guard_index;
      return true;
    }
    if (IsStatementGuard(bound_select, predicate)) {
      if (guard_index >= guard_predicates.size() || guard_predicates[guard_index] != predicate) {
        invalid = true;
        return false;
      }
      ++guard_index;
      return true;
    }
    if (empty_access) {
      return true;
    }
    if (selected_index_predicate < selected_index_predicates.size() &&
        selected_index_predicates[selected_index_predicate].second == predicate) {
      ++selected_index_predicate;
      return true;
    }
    if (!selected_rowid) {
      const std::optional<BoundExpressionId> key = RowIdLookupKey(bound_select, source, predicate);
      if (key.has_value()) {
        if (lookup == nullptr || lookup->key != *key) {
          invalid = true;
          return false;
        }
        selected_rowid = true;
        return true;
      }
    }
    if (filter_index >= filter_predicates.size() || filter_predicates[filter_index] != predicate) {
      invalid = true;
      return false;
    }
    ++filter_index;
    return true;
  };

  if (const std::optional<BoundExpressionId> where = bound_select.where_expression();
      where.has_value()) {
    static_cast<void>(VisitConjuncts(bound_select, *where, &visit));
  }
  if (invalid || guard_index != guard_predicates.size() ||
      filter_index != filter_predicates.size() || empty_access != known_false ||
      (lookup != nullptr) != selected_rowid ||
      selected_index_predicate != selected_index_predicates.size()) {
    return std::unexpected{InvariantFailure("physical predicate classification is invalid")};
  }
  return {};
}

[[nodiscard]] std::expected<void, OptimizerError> ValidateAdvancedPhysicalPlan(
    const LogicalQueryView& logical_plan, std::span<const PhysicalNode> nodes, PhysicalNodeId root,
    std::span<const AccessPathCandidate> candidates, std::size_t selected_candidate_index) {
  if (nodes.size() != logical_plan.nodes().size() || root.value() != logical_plan.root().value()) {
    return std::unexpected{InvariantFailure("advanced physical plan arena is invalid")};
  }
  std::vector<std::optional<AggregateCorePlan>> aggregate_plans(
      logical_plan.bound_select().query_cores().size());
  for (std::size_t core = 0; core < aggregate_plans.size(); ++core) {
    const auto* select =
        std::get_if<BoundSelectCore>(&logical_plan.bound_select().query_cores()[core]);
    if (select != nullptr && select->aggregate.has_value()) {
      aggregate_plans[core] = BuildAggregateCorePlan(logical_plan.bound_select(), core);
    }
  }
  if (aggregate_plans.size() == 1U && aggregate_plans.front().has_value() &&
      !AssumePresent(aggregate_plans.front()).joined) {
    const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans.front());
    std::vector<AccessPathCandidate> expected;
    if (aggregate.predicates.empty) {
      expected.push_back(AccessPathCandidate{
          .kind = PhysicalAccessKind::kEmpty,
          .cost = {},
      });
    } else if (aggregate.source.single_row) {
      expected.push_back(AccessPathCandidate{
          .kind = PhysicalAccessKind::kSingleRow,
          .cost =
              AccessPathCost{
                  .estimated_input_rows = 1,
                  .estimated_output_rows = 1,
                  .work_units = 1,
              },
      });
    } else {
      expected.reserve(aggregate.access_candidates.size());
      for (const ReadAccessCandidate& candidate : aggregate.access_candidates) {
        expected.push_back(candidate.published);
      }
    }
    if (!std::ranges::equal(candidates, expected) ||
        selected_candidate_index != aggregate.selected_access.value_or(0U)) {
      return std::unexpected{
          InvariantFailure("aggregate access candidates do not match the selected core")};
    }
  } else if (!candidates.empty() || selected_candidate_index != 0U) {
    return std::unexpected{InvariantFailure("advanced physical plan retained candidates")};
  }

  const LogicalAdvancedOrderNode* advanced_order = nullptr;
  for (const LogicalNode& node : logical_plan.nodes()) {
    if (const auto* order = std::get_if<LogicalAdvancedOrderNode>(&node.payload);
        order != nullptr) {
      advanced_order = order;
      break;
    }
  }
  const std::optional<std::size_t> ordered_union_limit_one_core =
      advanced_order != nullptr ? OrderedUnionLimitOneCoreStart(logical_plan.bound_select(),
                                                                advanced_order->set_then_order)
                                : std::nullopt;
  const bool ordered_union_all_runtime_limit =
      advanced_order != nullptr &&
      UsesOrderedUnionAllRuntimeLimit(logical_plan.bound_select(), advanced_order->set_then_order);

  std::size_t compound_index = 0;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const LogicalNodePayload& logical = logical_plan.nodes()[index].payload;
    const PhysicalNodePayload& physical = nodes[index].payload;
    const bool valid = std::visit(
        [&](const auto& node) {
          using Node = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<Node, LogicalSingleRowNode>) {
            if (aggregate_plans[node.core_index].has_value() &&
                aggregate_plans[node.core_index]->predicates.empty) {
              return std::holds_alternative<PhysicalEmptyNode>(physical);
            }
            const auto* value = std::get_if<PhysicalSingleRowNode>(&physical);
            return value != nullptr && value->core_index == node.core_index;
          } else if constexpr (std::is_same_v<Node, LogicalScanNode>) {
            if (aggregate_plans[node.core_index].has_value() &&
                !aggregate_plans[node.core_index]->joined) {
              const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans[node.core_index]);
              if (aggregate.predicates.empty) {
                return std::holds_alternative<PhysicalEmptyNode>(physical);
              }
              if (aggregate.selected_access.has_value()) {
                const ReadAccessCandidate& selected =
                    aggregate.access_candidates[*aggregate.selected_access];
                if (const auto* rowid = std::get_if<RowIdCandidatePlan>(&selected.payload);
                    rowid != nullptr) {
                  const auto* value = std::get_if<PhysicalRowIdLookupNode>(&physical);
                  return value != nullptr && value->source == aggregate.source.id &&
                         value->source_kind == aggregate.source.source_kind &&
                         value->table == aggregate.source.table &&
                         value->root_page == aggregate.source.root_page && value->key == rowid->key;
                }
                if (const auto* index_plan = std::get_if<IndexCandidatePlan>(&selected.payload);
                    index_plan != nullptr) {
                  const auto* value = std::get_if<PhysicalIndexScanNode>(&physical);
                  return value != nullptr && value->source == index_plan->node.source &&
                         value->table == index_plan->node.table &&
                         value->table_root_page == index_plan->node.table_root_page &&
                         value->index == index_plan->node.index &&
                         value->index_root_page == index_plan->node.index_root_page &&
                         value->equalities == index_plan->node.equalities &&
                         value->range == index_plan->node.range &&
                         value->covering == index_plan->node.covering &&
                         value->direction == aggregate.direction;
                }
              }
              const auto* value = std::get_if<PhysicalTableScanNode>(&physical);
              return value != nullptr && value->core_index == node.core_index &&
                     value->source == aggregate.source.id &&
                     value->source_kind == aggregate.source.source_kind &&
                     value->table == aggregate.source.table &&
                     value->root_page == aggregate.source.root_page &&
                     value->direction == aggregate.direction;
            }
            const RootPageId expected_root =
                node.source_kind == BoundSourceKind::kCatalogTable && node.table.has_value()
                    ? logical_plan.bound_select().catalog()->table(*node.table).root_page
                    : RootPageId{1};
            if (const auto key =
                    JoinedRowIdLookupKey(logical_plan.bound_select(), node.core_index, node.source);
                key.has_value()) {
              const auto* value = std::get_if<PhysicalRowIdLookupNode>(&physical);
              return value != nullptr && value->core_index == node.core_index &&
                     value->source == node.source && value->source_kind == node.source_kind &&
                     value->table == node.table && value->root_page == expected_root &&
                     value->key == *key;
            }
            const auto* value = std::get_if<PhysicalTableScanNode>(&physical);
            return value != nullptr && value->core_index == node.core_index &&
                   value->source == node.source && value->source_kind == node.source_kind &&
                   value->table == node.table && value->root_page == expected_root &&
                   value->direction == PhysicalScanDirection::kForward;
          } else if constexpr (std::is_same_v<Node, LogicalJoinNode>) {
            const auto* value = std::get_if<PhysicalNestedLoopJoinNode>(&physical);
            const auto ready = JoinConditionSource(logical_plan, node);
            return value != nullptr && value->core_index == node.core_index &&
                   value->left.value() == node.left.value() &&
                   value->right.value() == node.right.value() && value->join == node.join &&
                   value->on == node.on && ready.has_value() && value->condition_source == *ready;
          } else if constexpr (std::is_same_v<Node, LogicalFilterNode>) {
            const auto* value = std::get_if<PhysicalFilterNode>(&physical);
            if (value == nullptr || value->input.value() != node.input.value()) {
              return false;
            }
            const LogicalNodePayload& source = logical_plan.nodes()[node.input.value()].payload;
            std::optional<std::size_t> core_index;
            if (const auto* scan = std::get_if<LogicalScanNode>(&source); scan != nullptr) {
              core_index = scan->core_index;
            } else if (const auto* single = std::get_if<LogicalSingleRowNode>(&source);
                       single != nullptr) {
              core_index = single->core_index;
            } else if (const auto* join = std::get_if<LogicalJoinNode>(&source); join != nullptr) {
              core_index = join->core_index;
            }
            if (core_index.has_value() && aggregate_plans[*core_index].has_value() &&
                !aggregate_plans[*core_index]->joined) {
              const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans[*core_index]);
              return value->predicates == aggregate.residuals &&
                     value->guards == aggregate.predicates.guards &&
                     value->predicate_sources.empty();
            }
            const AdvancedPredicateParts expected =
                SplitAdvancedPredicate(logical_plan.bound_select(), node.predicate);
            std::vector<BoundSourceId> placements;
            if (core_index.has_value()) {
              const auto& core =
                  std::get<BoundSelectCore>(logical_plan.bound_select().query_cores()[*core_index]);
              auto ready =
                  JoinPredicateSources(logical_plan.bound_select(), core, expected.predicates);
              if (!ready.has_value()) {
                return false;
              }
              placements = std::move(*ready);
            }
            return value->predicates == expected.predicates && value->guards == expected.guards &&
                   value->predicate_sources == placements;
          } else if constexpr (std::is_same_v<Node, LogicalLimitNode>) {
            const auto* value = std::get_if<PhysicalLimitNode>(&physical);
            return value != nullptr && value->input.value() == node.input.value() &&
                   value->limit == node.limit && value->offset == node.offset;
          } else if constexpr (std::is_same_v<Node, LogicalProjectionNode>) {
            const auto* value = std::get_if<PhysicalProjectionNode>(&physical);
            return value != nullptr && value->input.value() == node.input.value() &&
                   value->logical_projection.value() == index &&
                   value->core_index == node.core_index;
          } else if constexpr (std::is_same_v<Node, LogicalOrderNode>) {
            const auto* value = std::get_if<PhysicalSortNode>(&physical);
            const bool input_order_satisfied = aggregate_plans.size() == 1U &&
                                               aggregate_plans.front().has_value() &&
                                               aggregate_plans.front()->order_satisfied;
            const PhysicalSortStrategy strategy = logical_plan.bound_select().limit() == nullptr
                                                      ? PhysicalSortStrategy::kExternal
                                                      : PhysicalSortStrategy::kRuntimeLimit;
            if (value == nullptr || value->input.value() != node.input.value() ||
                value->payload_expressions != node.payload_expressions ||
                value->output_fields != node.output_fields || value->schedule != node.schedule ||
                value->strategy != strategy ||
                value->input_order_satisfied != input_order_satisfied ||
                value->terms.size() != node.terms.size()) {
              return false;
            }
            for (std::size_t term = 0; term < node.terms.size(); ++term) {
              if (!OrderingTermsEqual(value->terms[term], node.terms[term])) {
                return false;
              }
            }
            return true;
          } else if constexpr (std::is_same_v<Node, LogicalOutputNode>) {
            const auto* value = std::get_if<PhysicalOutputNode>(&physical);
            return value != nullptr && value->input.value() == node.input.value() &&
                   value->logical_output.value() == index;
          } else if constexpr (std::is_same_v<Node, LogicalValuesNode>) {
            const auto* value = std::get_if<PhysicalValuesNode>(&physical);
            return value != nullptr && value->core_index == node.core_index;
          } else if constexpr (std::is_same_v<Node, LogicalDistinctNode>) {
            const auto* value = std::get_if<PhysicalDistinctNode>(&physical);
            return value != nullptr && value->input.value() == node.input.value() &&
                   value->core_index == node.core_index && value->collations == node.collations &&
                   value->strategy == PhysicalDistinctStrategy::kEphemeralMembership;
          } else if constexpr (std::is_same_v<Node, LogicalCompoundNode>) {
            const auto* value = std::get_if<PhysicalCompoundNode>(&physical);
            const std::size_t current_compound = compound_index++;
            if (value == nullptr || value->left.value() != node.left.value() ||
                value->right.value() != node.right.value() || value->operation != node.operation ||
                value->collations != node.collations) {
              return false;
            }
            PhysicalCompoundStrategy expected = PhysicalCompoundStrategy::kEphemeralSet;
            if (advanced_order != nullptr && !advanced_order->set_then_order) {
              expected = PhysicalCompoundStrategy::kOrderedMerge;
            } else if (node.operation == CompoundOperator::kUnionAll) {
              expected = PhysicalCompoundStrategy::kConcatenate;
            } else if (advanced_order != nullptr && advanced_order->set_then_order) {
              expected = PhysicalCompoundStrategy::kSetThenOrder;
            } else if (UsesUnionLimitOne(logical_plan.bound_select(), current_compound)) {
              expected = PhysicalCompoundStrategy::kUnionLimitOne;
            }
            return value->strategy == expected;
          } else if constexpr (std::is_same_v<Node, LogicalAdvancedOrderNode>) {
            const auto* value = std::get_if<PhysicalAdvancedOrderNode>(&physical);
            const bool input_order_satisfied = aggregate_plans.size() == 1U &&
                                               aggregate_plans.front().has_value() &&
                                               aggregate_plans.front()->order_satisfied;
            if (value == nullptr || value->input.value() != node.input.value() ||
                value->terms.size() != node.terms.size() ||
                value->core_plans.size() != node.core_layouts.size() ||
                value->set_then_order != node.set_then_order ||
                value->input_order_satisfied != input_order_satisfied) {
              return false;
            }
            for (std::size_t term = 0; term < node.terms.size(); ++term) {
              if (!OrderingTermsEqual(value->terms[term], node.terms[term])) {
                return false;
              }
            }
            for (std::size_t core = 0; core < node.core_layouts.size(); ++core) {
              const bool bounded = ordered_union_all_runtime_limit ||
                                   node.core_layouts[core].schedule ==
                                       OrderEvaluationSchedule::kKeysThenAdmissionThenPayload ||
                                   (ordered_union_limit_one_core.has_value() &&
                                    core >= *ordered_union_limit_one_core);
              if (value->core_plans[core].layout != node.core_layouts[core] ||
                  value->core_plans[core].strategy != (bounded ? PhysicalSortStrategy::kRuntimeLimit
                                                               : PhysicalSortStrategy::kExternal)) {
                return false;
              }
            }
            const PhysicalSortStrategy final_strategy =
                node.set_then_order && logical_plan.bound_select().limit() != nullptr
                    ? PhysicalSortStrategy::kRuntimeLimit
                    : PhysicalSortStrategy::kExternal;
            return value->final_strategy == final_strategy;
          } else if constexpr (std::is_same_v<Node, LogicalAggregateNode>) {
            const auto* value = std::get_if<PhysicalAggregateNode>(&physical);
            if (value == nullptr || value->input.value() != node.input.value() ||
                value->core_index != node.core_index || node.core_index >= aggregate_plans.size() ||
                !aggregate_plans[node.core_index].has_value()) {
              return false;
            }
            const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans[node.core_index]);
            return value->strategy == aggregate.strategy && value->min_max == aggregate.min_max &&
                   value->grouping_order == aggregate.grouping_order &&
                   value->at_most_one_row == aggregate.at_most_one_row;
          } else if constexpr (std::is_same_v<Node, LogicalHavingNode>) {
            const auto* value = std::get_if<PhysicalHavingNode>(&physical);
            if (value == nullptr || value->input.value() != node.input.value() ||
                value->core_index != node.core_index || value->predicate != node.predicate ||
                !IsValidExpressionId(logical_plan.bound_select(), value->predicate) ||
                node.core_index >= logical_plan.bound_select().query_cores().size()) {
              return false;
            }
            const auto* core = std::get_if<BoundSelectCore>(
                &logical_plan.bound_select().query_cores()[node.core_index]);
            return core != nullptr && core->aggregate.has_value() &&
                   core->aggregate->having == value->predicate;
          } else {
            return false;
          }
        },
        logical);
    if (!valid) {
      return std::unexpected{InvariantFailure("advanced physical node is invalid")};
    }
  }
  return {};
}

[[nodiscard]] std::expected<void, OptimizerError> ValidatePhysicalPlan(
    const LogicalQueryView& logical_plan, std::span<const PhysicalNode> nodes, PhysicalNodeId root,
    std::span<const AccessPathCandidate> candidates, std::size_t selected_candidate_index) {
  if (!logical_plan.valid()) {
    return std::unexpected{InvariantFailure("physical plan retained an invalid logical plan")};
  }
  if (IsAdvancedLogicalPlan(logical_plan)) {
    return ValidateAdvancedPhysicalPlan(logical_plan, nodes, root, candidates,
                                        selected_candidate_index);
  }
  if (nodes.size() < 2U || nodes.size() > 6U || root.value() != nodes.size() - 1U) {
    return std::unexpected{InvariantFailure("physical plan node arena is invalid")};
  }
  if (candidates.empty() || selected_candidate_index >= candidates.size() ||
      AccessKindOf(nodes.front()) != candidates[selected_candidate_index].kind) {
    return std::unexpected{InvariantFailure("physical access candidates are invalid")};
  }

  const BoundQueryView& bound_select = logical_plan.bound_select();
  const SourceInfo source = ResolveSource(logical_plan);
  const PhysicalNode& leaf = nodes.front();
  if (std::expected<void, OptimizerError> validated =
          ValidateCandidates(bound_select, source, leaf, candidates, selected_candidate_index);
      !validated.has_value()) {
    return validated;
  }
  if (const auto* scan = std::get_if<PhysicalTableScanNode>(&leaf.payload); scan != nullptr) {
    if (source.single_row || scan->source != source.id || scan->source_kind != source.source_kind ||
        scan->table != source.table || scan->root_page != source.root_page ||
        scan->direction != PhysicalScanDirection::kForward) {
      return std::unexpected{InvariantFailure("physical table scan does not match logical source")};
    }
  } else if (const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&leaf.payload);
             lookup != nullptr) {
    if (!source.rowid_eligible || lookup->source != source.id ||
        lookup->source_kind != source.source_kind || lookup->table != source.table ||
        lookup->root_page != source.root_page || !IsValidExpressionId(bound_select, lookup->key) ||
        ExpressionDependsOnSource(bound_select, lookup->key)) {
      return std::unexpected{InvariantFailure("physical rowid lookup is invalid")};
    }
  } else if (const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&leaf.payload);
             index_scan != nullptr) {
    if (source.source_kind != BoundSourceKind::kCatalogTable || !source.table.has_value() ||
        index_scan->source != source.id || index_scan->table != source.table ||
        index_scan->table_root_page != source.root_page || bound_select.catalog() == nullptr ||
        index_scan->index.value >= bound_select.catalog()->indexes().size()) {
      return std::unexpected{InvariantFailure("physical index scan source is invalid")};
    }
    const CatalogTable& table = bound_select.catalog()->table(index_scan->table);
    const CatalogIndex& index = bound_select.catalog()->index(index_scan->index);
    if (index.table != index_scan->table || index.root_page != index_scan->index_root_page ||
        !IsSupportedIndexShape(bound_select, table, index) ||
        index_scan->equalities.size() > index.key_term_count ||
        index_scan->direction != PhysicalScanDirection::kForward) {
      return std::unexpected{InvariantFailure("physical index scan metadata is invalid")};
    }
    for (std::size_t term_index = 0; term_index < index_scan->equalities.size(); ++term_index) {
      const PhysicalIndexEquality& equality = index_scan->equalities[term_index];
      const CatalogIndexTerm& term = index.terms[term_index];
      const auto* column = std::get_if<ColumnId>(&term.target);
      const std::optional<IndexConstraintMatch> match = MatchIndexConstraint(
          bound_select, source.id, 0U, bound_select.source_columns().size(), term,
          ReadPredicateTerm{
              .predicate = equality.predicate,
              .conjunct_position = equality.conjunct_position,
          });
      if (column == nullptr || equality.column != *column ||
          equality.affinity != table.columns[column->value].affinity ||
          !CatalogNamesEqual(equality.collation_name, term.collation_name) ||
          equality.order != term.order || !match.has_value() ||
          match->kind != IndexConstraintKind::kEquality || match->key != equality.key ||
          match->reject_null != equality.reject_null) {
        return std::unexpected{InvariantFailure("physical index equality is invalid")};
      }
    }
    if (index_scan->range.has_value()) {
      const std::size_t term_index = index_scan->equalities.size();
      if (term_index >= index.key_term_count || index.statistics.unordered) {
        return std::unexpected{InvariantFailure("physical index range position is invalid")};
      }
      const CatalogIndexTerm& term = index.terms[term_index];
      const auto* column = std::get_if<ColumnId>(&term.target);
      const PhysicalIndexRange& range = *index_scan->range;
      if (column == nullptr || range.column != *column ||
          range.affinity != table.columns[column->value].affinity ||
          !CatalogNamesEqual(range.collation_name, term.collation_name) ||
          range.order != term.order || (!range.lower.has_value() && !range.upper.has_value())) {
        return std::unexpected{InvariantFailure("physical index range metadata is invalid")};
      }
      const auto validate_bound = [&](const PhysicalIndexBound& bound,
                                      IndexConstraintKind expected) {
        const std::optional<IndexConstraintMatch> match = MatchIndexConstraint(
            bound_select, source.id, 0U, bound_select.source_columns().size(), term,
            ReadPredicateTerm{
                .predicate = bound.predicate,
                .conjunct_position = bound.conjunct_position,
            });
        return match.has_value() && match->kind == expected && match->key == bound.key &&
               match->inclusive == bound.inclusive;
      };
      if ((range.lower.has_value() && !validate_bound(*range.lower, IndexConstraintKind::kLower)) ||
          (range.upper.has_value() && !validate_bound(*range.upper, IndexConstraintKind::kUpper))) {
        return std::unexpected{InvariantFailure("physical index range bound is invalid")};
      }
    } else if (index_scan->equalities.empty() &&
               (index.statistics.unordered || !index_scan->covering)) {
      return std::unexpected{InvariantFailure("index cannot provide the selected full scan")};
    }
  } else if (std::holds_alternative<PhysicalSingleRowNode>(leaf.payload)) {
    if (!source.single_row) {
      return std::unexpected{InvariantFailure("single-row access does not match logical source")};
    }
  } else if (std::holds_alternative<PhysicalEmptyNode>(leaf.payload)) {
    if (!bound_select.where_expression().has_value()) {
      return std::unexpected{InvariantFailure("empty access has no WHERE predicate")};
    }
  } else {
    return std::unexpected{InvariantFailure("physical plan does not begin with an access node")};
  }

  std::size_t index = 1;
  PhysicalNodeId input{0};
  std::span<const BoundExpressionId> guard_predicates{};
  std::span<const BoundExpressionId> filter_predicates{};
  if (index < nodes.size() && std::holds_alternative<PhysicalGuardNode>(nodes[index].payload)) {
    const auto& guard = std::get<PhysicalGuardNode>(nodes[index].payload);
    if (guard.input != input || guard.predicates.empty()) {
      return std::unexpected{InvariantFailure("physical guard shape is invalid")};
    }
    for (const BoundExpressionId predicate : guard.predicates) {
      if (!IsValidExpressionId(bound_select, predicate) ||
          (source.single_row ? ExpressionDependsOnSource(bound_select, predicate)
                             : !IsStatementGuard(bound_select, predicate))) {
        return std::unexpected{InvariantFailure("physical guard predicate is invalid")};
      }
    }
    guard_predicates = guard.predicates;
    input = PhysicalNodeId{static_cast<std::uint32_t>(index++)};
  }
  if (index < nodes.size() && std::holds_alternative<PhysicalFilterNode>(nodes[index].payload)) {
    const auto& filter = std::get<PhysicalFilterNode>(nodes[index].payload);
    if (filter.input != input || filter.predicates.empty() || !filter.guards.empty()) {
      return std::unexpected{InvariantFailure("physical filter shape is invalid")};
    }
    for (const BoundExpressionId predicate : filter.predicates) {
      if (!IsValidExpressionId(bound_select, predicate)) {
        return std::unexpected{InvariantFailure("physical filter predicate is invalid")};
      }
    }
    filter_predicates = filter.predicates;
    input = PhysicalNodeId{static_cast<std::uint32_t>(index++)};
  }

  const LogicalOrderNode* logical_order = LogicalOrderOf(logical_plan);
  if (logical_order != nullptr) {
    if (index >= nodes.size()) {
      return std::unexpected{InvariantFailure("physical sort is missing")};
    }
    const auto* sort = std::get_if<PhysicalSortNode>(&nodes[index].payload);
    const PhysicalSortStrategy expected_strategy = bound_select.limit() == nullptr
                                                       ? PhysicalSortStrategy::kExternal
                                                       : PhysicalSortStrategy::kRuntimeLimit;
    if (sort == nullptr || sort->input != input ||
        sort->terms.size() != logical_order->terms.size() ||
        sort->payload_expressions != logical_order->payload_expressions ||
        sort->output_fields != logical_order->output_fields ||
        sort->schedule != logical_order->schedule || sort->strategy != expected_strategy ||
        sort->input_order_satisfied) {
      return std::unexpected{InvariantFailure("physical sort does not match logical ordering")};
    }
    for (std::size_t term_index = 0; term_index < sort->terms.size(); ++term_index) {
      if (!OrderingTermsEqual(sort->terms[term_index], logical_order->terms[term_index])) {
        return std::unexpected{
            InvariantFailure("physical sort term does not match logical ordering")};
      }
    }
    input = PhysicalNodeId{static_cast<std::uint32_t>(index++)};
  } else if (!bound_select.order_by().empty()) {
    return std::unexpected{InvariantFailure("logical ordering shape is invalid")};
  }

  const BoundLimit* logical_limit = bound_select.limit();
  if (logical_limit != nullptr) {
    if (index >= nodes.size()) {
      return std::unexpected{InvariantFailure("physical limit is missing")};
    }
    const auto* limit = std::get_if<PhysicalLimitNode>(&nodes[index].payload);
    if (limit == nullptr || limit->input != input || limit->limit != logical_limit->limit ||
        limit->offset != logical_limit->offset) {
      return std::unexpected{InvariantFailure("physical limit does not match logical limit")};
    }
    input = PhysicalNodeId{static_cast<std::uint32_t>(index++)};
  }

  if (index + 1U != nodes.size()) {
    return std::unexpected{InvariantFailure("physical plan contains unexpected nodes")};
  }
  if (logical_order != nullptr) {
    const auto* output = std::get_if<PhysicalOutputNode>(&nodes[index].payload);
    if (output == nullptr || output->input != input ||
        output->logical_output != logical_plan.root()) {
      return std::unexpected{
          InvariantFailure("physical output does not match logical ordered output")};
    }
  } else {
    const auto* projection = std::get_if<PhysicalProjectionNode>(&nodes[index].payload);
    if (projection == nullptr || projection->input != input ||
        projection->logical_projection != logical_plan.root()) {
      return std::unexpected{InvariantFailure("physical projection does not match logical root")};
    }
  }
  if (std::expected<void, OptimizerError> validated =
          ValidatePredicates(bound_select, source, leaf, guard_predicates, filter_predicates);
      !validated.has_value()) {
    return validated;
  }
  return {};
}

[[nodiscard]] std::string_view SourceName(const PhysicalQueryView& plan,
                                          BoundSourceKind source_kind,
                                          std::optional<TableId> table) {
  if (source_kind == BoundSourceKind::kSchemaTable) {
    return "sqlite_schema";
  }
  return plan.logical_plan().bound_select().catalog()->table(table.value_or(TableId{})).name;
}

void AppendExplainIdentifier(std::string_view name, std::string* output) {
  constexpr std::string_view kHex{"0123456789ABCDEF"};
  output->push_back('"');
  for (const char value : name) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte == static_cast<unsigned char>('"') || byte == static_cast<unsigned char>('\\')) {
      output->push_back('\\');
      output->push_back(static_cast<char>(byte));
    } else if (byte >= 0x20U && byte <= 0x7EU) {
      output->push_back(static_cast<char>(byte));
    } else {
      output->append("\\x");
      output->push_back(kHex[byte >> 4U]);
      output->push_back(kHex[byte & 0x0FU]);
    }
  }
  output->push_back('"');
}

}  // namespace

struct PhysicalPlan::Impl {
  explicit Impl(LogicalPlan input) noexcept : logical_plan(std::move(input)) {}

  LogicalPlan logical_plan;
  PhysicalQueryPlan root_query;
  std::vector<PhysicalQueryPlan> nested_queries;
};

struct PhysicalMutationPlan::Impl {
  Impl(LogicalMutationPlan input, PhysicalMutationPayload mutation) noexcept
      : logical_plan(std::move(input)), payload(std::move(mutation)) {}

  LogicalMutationPlan logical_plan;
  PhysicalMutationPayload payload;
};

namespace optimizer_detail {

class PhysicalPlanBuilder final {
 public:
  [[nodiscard]] static OptimizeLogicalPlanResult Build(LogicalPlan logical_plan) {
    if (!logical_plan.valid()) {
      return std::unexpected{
          OptimizerFailure(OptimizerErrorCode::kInvalidInput, "logical plan is invalid")};
    }
    auto impl = std::make_unique<PhysicalPlan::Impl>(std::move(logical_plan));
    const std::size_t count = impl->logical_plan.bound_select().query_count();
    impl->nested_queries.resize(count - 1U);
    for (std::size_t index = 0; index < count; ++index) {
      const BoundQueryId id{static_cast<std::uint32_t>(index)};
      PhysicalQueryPlan& query = index == 0U ? impl->root_query : impl->nested_queries[index - 1U];
      query.query = id;
      if (auto built = BuildQuery(LogicalQueryView{impl->logical_plan, id}, &query);
          !built.has_value()) {
        return std::unexpected(std::move(built.error()));
      }
    }
    return PhysicalPlan{std::move(impl)};
  }

 private:
  [[nodiscard]] static std::expected<void, OptimizerError> BuildQuery(
      const LogicalQueryView& logical_plan, PhysicalQueryPlan* impl) {
    if (IsAdvancedLogicalPlan(logical_plan)) {
      return BuildAdvanced(logical_plan, impl);
    }

    const SourceInfo source = ResolveSource(logical_plan);
    ReadPredicateAnalysis predicates = AnalyzeReadPredicate(
        logical_plan.bound_select(), source, logical_plan.bound_select().where_expression());
    std::vector<BoundExpressionId> residuals;
    std::vector<ReadAccessCandidate> access_candidates;
    if (!predicates.empty && !source.single_row) {
      std::size_t index_count = 0;
      if (source.source_kind == BoundSourceKind::kCatalogTable && source.table.has_value()) {
        index_count = logical_plan.bound_select().catalog()->table_indexes(*source.table).size();
      }
      access_candidates.reserve(2U + index_count);
      access_candidates.push_back(ReadAccessCandidate{
          .published =
              AccessPathCandidate{
                  .kind = PhysicalAccessKind::kTableScan,
                  .cost = ScanCost(source.estimated_rows, source.estimated_row_size),
              },
      });
      for (std::size_t term_index = 0; term_index < predicates.terms.size(); ++term_index) {
        const std::optional<BoundExpressionId> key = RowIdLookupKey(
            logical_plan.bound_select(), source, predicates.terms[term_index].predicate);
        if (key.has_value()) {
          access_candidates.push_back(ReadAccessCandidate{
              .published =
                  AccessPathCandidate{
                      .kind = PhysicalAccessKind::kRowIdLookup,
                      .cost = RowIdCost(source.estimated_rows),
                  },
              .payload =
                  RowIdCandidatePlan{
                      .key = *key,
                      .selected_term = term_index,
                  },
          });
          break;
        }
      }
      if (source.source_kind == BoundSourceKind::kCatalogTable && source.table.has_value() &&
          source.rowid_eligible) {
        const CatalogSnapshot& catalog = *logical_plan.bound_select().catalog();
        for (const IndexId index_id : catalog.table_indexes(*source.table)) {
          std::optional<IndexCandidatePlan> candidate = AnalyzeIndexCandidate(
              logical_plan.bound_select(), source.id, *source.table, 0U,
              logical_plan.bound_select().source_columns().size(), index_id, predicates.terms);
          if (!candidate.has_value()) {
            continue;
          }
          const AccessPathCandidate published =
              PublishIndexCandidate(logical_plan.bound_select(), source, *candidate);
          const bool constrained =
              !candidate->node.equalities.empty() || candidate->node.range.has_value();
          if (!constrained &&
              published.cost.work_units >= access_candidates.front().published.cost.work_units) {
            continue;
          }
          access_candidates.push_back(ReadAccessCandidate{
              .published = published,
              .payload = std::move(*candidate),
          });
        }
      }
    }

    std::size_t selected_access = 0;
    for (std::size_t index = 1; index < access_candidates.size(); ++index) {
      if (PreferCandidate(access_candidates[index].published,
                          access_candidates[selected_access].published)) {
        selected_access = index;
      }
    }

    std::vector<bool> selected_terms(predicates.terms.size(), false);
    if (!access_candidates.empty()) {
      if (const auto* rowid =
              std::get_if<RowIdCandidatePlan>(&access_candidates[selected_access].payload);
          rowid != nullptr) {
        selected_terms[rowid->selected_term] = true;
      } else if (const auto* index =
                     std::get_if<IndexCandidatePlan>(&access_candidates[selected_access].payload);
                 index != nullptr) {
        selected_terms = index->selected_terms;
      }
    }
    residuals.reserve(predicates.terms.size());
    for (std::size_t index = 0; index < predicates.terms.size(); ++index) {
      if (!selected_terms[index]) {
        residuals.push_back(predicates.terms[index].predicate);
      }
    }

    const bool ordered = !logical_plan.bound_select().order_by().empty();
    const std::size_t node_count =
        2U + static_cast<std::size_t>(!predicates.guards.empty()) +
        static_cast<std::size_t>(!residuals.empty()) +
        static_cast<std::size_t>(logical_plan.bound_select().limit() != nullptr) +
        static_cast<std::size_t>(ordered);
    impl->nodes.reserve(node_count);

    if (predicates.empty) {
      impl->candidates.push_back(AccessPathCandidate{
          .kind = PhysicalAccessKind::kEmpty,
          .cost = {},
      });
      impl->nodes.push_back(PhysicalNode{.payload = PhysicalEmptyNode{}});
    } else if (source.single_row) {
      impl->candidates.push_back(AccessPathCandidate{
          .kind = PhysicalAccessKind::kSingleRow,
          .cost =
              AccessPathCost{
                  .estimated_input_rows = 1,
                  .estimated_output_rows = 1,
                  .work_units = 1,
              },
      });
      impl->nodes.push_back(PhysicalNode{.payload = PhysicalSingleRowNode{}});
    } else {
      impl->candidates.reserve(access_candidates.size());
      for (const ReadAccessCandidate& candidate : access_candidates) {
        impl->candidates.push_back(candidate.published);
      }
      impl->selected_candidate = selected_access;
      ReadAccessCandidate& selected = access_candidates[selected_access];
      if (const auto* rowid = std::get_if<RowIdCandidatePlan>(&selected.payload);
          rowid != nullptr) {
        impl->nodes.push_back(PhysicalNode{
            .payload =
                PhysicalRowIdLookupNode{
                    .source = source.id,
                    .source_kind = source.source_kind,
                    .table = source.table,
                    .root_page = source.root_page,
                    .key = rowid->key,
                },
        });
      } else if (auto* index = std::get_if<IndexCandidatePlan>(&selected.payload);
                 index != nullptr) {
        impl->nodes.push_back(PhysicalNode{.payload = std::move(index->node)});
      } else {
        impl->nodes.push_back(PhysicalNode{
            .payload =
                PhysicalTableScanNode{
                    .source = source.id,
                    .source_kind = source.source_kind,
                    .table = source.table,
                    .root_page = source.root_page,
                },
        });
      }
    }

    PhysicalNodeId input{0};
    if (!predicates.guards.empty()) {
      impl->nodes.push_back(PhysicalNode{
          .payload =
              PhysicalGuardNode{
                  .input = input,
                  .predicates = std::move(predicates.guards),
              },
      });
      input = PhysicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    }
    if (!residuals.empty()) {
      impl->nodes.push_back(PhysicalNode{
          .payload =
              PhysicalFilterNode{
                  .input = input,
                  .predicates = std::move(residuals),
                  .guards = {},
              },
      });
      input = PhysicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    }
    if (ordered) {
      const LogicalOrderNode* logical_order = LogicalOrderOf(logical_plan);
      if (logical_order == nullptr) {
        return std::unexpected{InvariantFailure("ordered logical plan has no order node")};
      }
      impl->nodes.push_back(PhysicalNode{
          .payload =
              PhysicalSortNode{
                  .input = input,
                  .terms = logical_order->terms,
                  .payload_expressions = logical_order->payload_expressions,
                  .output_fields = logical_order->output_fields,
                  .schedule = logical_order->schedule,
                  .strategy = logical_plan.bound_select().limit() == nullptr
                                  ? PhysicalSortStrategy::kExternal
                                  : PhysicalSortStrategy::kRuntimeLimit,
                  .input_order_satisfied = false,
              },
      });
      input = PhysicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    }
    if (const BoundLimit* limit = logical_plan.bound_select().limit(); limit != nullptr) {
      impl->nodes.push_back(PhysicalNode{
          .payload =
              PhysicalLimitNode{
                  .input = input,
                  .limit = limit->limit,
                  .offset = limit->offset,
              },
      });
      input = PhysicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    }
    if (ordered) {
      impl->nodes.push_back(PhysicalNode{
          .payload =
              PhysicalOutputNode{
                  .input = input,
                  .logical_output = logical_plan.root(),
              },
      });
    } else {
      impl->nodes.push_back(PhysicalNode{
          .payload =
              PhysicalProjectionNode{
                  .input = input,
                  .logical_projection = logical_plan.root(),
              },
      });
    }
    impl->root = PhysicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};

    const std::span<const AccessPathCandidate> candidates{impl->candidates};
    if (std::expected<void, OptimizerError> validated = ValidatePhysicalPlan(
            logical_plan, impl->nodes, impl->root, candidates, impl->selected_candidate);
        !validated.has_value()) {
      return std::unexpected{std::move(validated.error())};
    }
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPlannerWork,
                                 1U + impl->candidates.size());
    return {};
  }

  [[nodiscard]] static std::expected<void, OptimizerError> BuildAdvanced(
      const LogicalQueryView& logical_plan, PhysicalQueryPlan* impl) {
    impl->nodes.reserve(logical_plan.nodes().size());
    std::vector<PhysicalNodeId> mapped;
    mapped.reserve(logical_plan.nodes().size());
    std::vector<std::optional<AggregateCorePlan>> aggregate_plans(
        logical_plan.bound_select().query_cores().size());
    for (std::size_t core = 0; core < aggregate_plans.size(); ++core) {
      const auto* select =
          std::get_if<BoundSelectCore>(&logical_plan.bound_select().query_cores()[core]);
      if (select != nullptr && select->aggregate.has_value()) {
        aggregate_plans[core] = BuildAggregateCorePlan(logical_plan.bound_select(), core);
      }
    }

    const LogicalAdvancedOrderNode* advanced_order = nullptr;
    for (const LogicalNode& node : logical_plan.nodes()) {
      if (const auto* order = std::get_if<LogicalAdvancedOrderNode>(&node.payload);
          order != nullptr) {
        advanced_order = order;
        break;
      }
    }
    const std::optional<std::size_t> ordered_union_limit_one_core =
        advanced_order != nullptr ? OrderedUnionLimitOneCoreStart(logical_plan.bound_select(),
                                                                  advanced_order->set_then_order)
                                  : std::nullopt;
    const bool ordered_union_all_runtime_limit =
        advanced_order != nullptr &&
        UsesOrderedUnionAllRuntimeLimit(logical_plan.bound_select(),
                                        advanced_order->set_then_order);

    std::size_t compound_index = 0;
    for (std::size_t logical_index = 0; logical_index < logical_plan.nodes().size();
         ++logical_index) {
      std::optional<BoundSourceId> condition_source;
      if (const auto* join =
              std::get_if<LogicalJoinNode>(&logical_plan.nodes()[logical_index].payload);
          join != nullptr) {
        auto ready = JoinConditionSource(logical_plan, *join);
        if (!ready.has_value()) {
          return std::unexpected(std::move(ready.error()));
        }
        condition_source = *ready;
      }
      std::vector<BoundSourceId> predicate_sources;
      if (const auto* filter =
              std::get_if<LogicalFilterNode>(&logical_plan.nodes()[logical_index].payload);
          filter != nullptr) {
        const auto* join = std::get_if<LogicalJoinNode>(&logical_plan.node(filter->input).payload);
        if (join != nullptr) {
          const auto predicates =
              SplitAdvancedPredicate(logical_plan.bound_select(), filter->predicate);
          auto ready =
              JoinPredicateSources(logical_plan.bound_select(),
                                   std::get<BoundSelectCore>(
                                       logical_plan.bound_select().query_cores()[join->core_index]),
                                   predicates.predicates);
          if (!ready.has_value()) {
            return std::unexpected(std::move(ready.error()));
          }
          predicate_sources = std::move(*ready);
        }
      }
      const LogicalNode& logical = logical_plan.nodes()[logical_index];
      PhysicalNodePayload payload = std::visit(
          [&](const auto& node) -> PhysicalNodePayload {
            using Node = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<Node, LogicalSingleRowNode>) {
              if (aggregate_plans[node.core_index].has_value() &&
                  aggregate_plans[node.core_index]->predicates.empty) {
                return PhysicalEmptyNode{.core_index = node.core_index};
              }
              return PhysicalSingleRowNode{.core_index = node.core_index};
            } else if constexpr (std::is_same_v<Node, LogicalScanNode>) {
              if (aggregate_plans[node.core_index].has_value() &&
                  !aggregate_plans[node.core_index]->joined) {
                const AggregateCorePlan& aggregate =
                    AssumePresent(aggregate_plans[node.core_index]);
                if (aggregate.predicates.empty) {
                  return PhysicalEmptyNode{.core_index = node.core_index};
                }
                if (aggregate.selected_access.has_value()) {
                  const ReadAccessCandidate& selected =
                      aggregate.access_candidates[*aggregate.selected_access];
                  if (const auto* rowid = std::get_if<RowIdCandidatePlan>(&selected.payload);
                      rowid != nullptr) {
                    return PhysicalRowIdLookupNode{
                        .core_index = node.core_index,
                        .source = aggregate.source.id,
                        .source_kind = aggregate.source.source_kind,
                        .table = aggregate.source.table,
                        .root_page = aggregate.source.root_page,
                        .key = rowid->key,
                    };
                  }
                  if (const auto* index = std::get_if<IndexCandidatePlan>(&selected.payload);
                      index != nullptr) {
                    PhysicalIndexScanNode scan = index->node;
                    scan.core_index = node.core_index;
                    scan.direction = aggregate.direction;
                    return scan;
                  }
                }
                return PhysicalTableScanNode{
                    .core_index = node.core_index,
                    .source = aggregate.source.id,
                    .source_kind = aggregate.source.source_kind,
                    .table = aggregate.source.table,
                    .root_page = aggregate.source.root_page,
                    .direction = aggregate.direction,
                };
              }
              RootPageId root_page{1};
              if (node.source_kind == BoundSourceKind::kCatalogTable && node.table.has_value()) {
                root_page = logical_plan.bound_select().catalog()->table(*node.table).root_page;
              }
              if (const auto key = JoinedRowIdLookupKey(logical_plan.bound_select(),
                                                        node.core_index, node.source);
                  key.has_value()) {
                return PhysicalRowIdLookupNode{
                    .core_index = node.core_index,
                    .source = node.source,
                    .source_kind = node.source_kind,
                    .table = node.table,
                    .root_page = root_page,
                    .key = *key,
                };
              }
              return PhysicalTableScanNode{
                  .core_index = node.core_index,
                  .source = node.source,
                  .source_kind = node.source_kind,
                  .table = node.table,
                  .root_page = root_page,
              };
            } else if constexpr (std::is_same_v<Node, LogicalJoinNode>) {
              return PhysicalNestedLoopJoinNode{
                  .left = mapped[node.left.value()],
                  .right = mapped[node.right.value()],
                  .core_index = node.core_index,
                  .join = node.join,
                  .on = node.on,
                  .condition_source = condition_source,
              };
            } else if constexpr (std::is_same_v<Node, LogicalFilterNode>) {
              const LogicalNodePayload& source = logical_plan.nodes()[node.input.value()].payload;
              std::optional<std::size_t> core_index;
              if (const auto* scan = std::get_if<LogicalScanNode>(&source); scan != nullptr) {
                core_index = scan->core_index;
              } else if (const auto* single = std::get_if<LogicalSingleRowNode>(&source);
                         single != nullptr) {
                core_index = single->core_index;
              } else if (const auto* join = std::get_if<LogicalJoinNode>(&source);
                         join != nullptr) {
                core_index = join->core_index;
              }
              if (core_index.has_value() && aggregate_plans[*core_index].has_value() &&
                  !aggregate_plans[*core_index]->joined) {
                const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans[*core_index]);
                return PhysicalFilterNode{
                    .input = mapped[node.input.value()],
                    .predicates = aggregate.residuals,
                    .guards = aggregate.predicates.guards,
                };
              }
              AdvancedPredicateParts predicates =
                  SplitAdvancedPredicate(logical_plan.bound_select(), node.predicate);
              return PhysicalFilterNode{
                  .input = mapped[node.input.value()],
                  .predicates = std::move(predicates.predicates),
                  .guards = std::move(predicates.guards),
                  .predicate_sources = std::move(predicate_sources),
              };
            } else if constexpr (std::is_same_v<Node, LogicalLimitNode>) {
              return PhysicalLimitNode{
                  .input = mapped[node.input.value()],
                  .limit = node.limit,
                  .offset = node.offset,
              };
            } else if constexpr (std::is_same_v<Node, LogicalProjectionNode>) {
              return PhysicalProjectionNode{
                  .input = mapped[node.input.value()],
                  .logical_projection = LogicalNodeId{static_cast<std::uint32_t>(logical_index)},
                  .core_index = node.core_index,
              };
            } else if constexpr (std::is_same_v<Node, LogicalOrderNode>) {
              const bool input_order_satisfied = aggregate_plans.size() == 1U &&
                                                 aggregate_plans.front().has_value() &&
                                                 aggregate_plans.front()->order_satisfied;
              return PhysicalSortNode{
                  .input = mapped[node.input.value()],
                  .terms = node.terms,
                  .payload_expressions = node.payload_expressions,
                  .output_fields = node.output_fields,
                  .schedule = node.schedule,
                  .strategy = logical_plan.bound_select().limit() == nullptr
                                  ? PhysicalSortStrategy::kExternal
                                  : PhysicalSortStrategy::kRuntimeLimit,
                  .input_order_satisfied = input_order_satisfied,
              };
            } else if constexpr (std::is_same_v<Node, LogicalOutputNode>) {
              return PhysicalOutputNode{
                  .input = mapped[node.input.value()],
                  .logical_output = LogicalNodeId{static_cast<std::uint32_t>(logical_index)},
              };
            } else if constexpr (std::is_same_v<Node, LogicalValuesNode>) {
              return PhysicalValuesNode{.core_index = node.core_index};
            } else if constexpr (std::is_same_v<Node, LogicalDistinctNode>) {
              return PhysicalDistinctNode{
                  .input = mapped[node.input.value()],
                  .core_index = node.core_index,
                  .collations = node.collations,
              };
            } else if constexpr (std::is_same_v<Node, LogicalCompoundNode>) {
              const std::size_t current_compound = compound_index++;
              PhysicalCompoundStrategy strategy = PhysicalCompoundStrategy::kEphemeralSet;
              if (advanced_order != nullptr && !advanced_order->set_then_order) {
                strategy = PhysicalCompoundStrategy::kOrderedMerge;
              } else if (node.operation == CompoundOperator::kUnionAll) {
                strategy = PhysicalCompoundStrategy::kConcatenate;
              } else if (advanced_order != nullptr && advanced_order->set_then_order) {
                strategy = PhysicalCompoundStrategy::kSetThenOrder;
              } else if (UsesUnionLimitOne(logical_plan.bound_select(), current_compound)) {
                strategy = PhysicalCompoundStrategy::kUnionLimitOne;
              }
              return PhysicalCompoundNode{
                  .left = mapped[node.left.value()],
                  .right = mapped[node.right.value()],
                  .operation = node.operation,
                  .collations = node.collations,
                  .strategy = strategy,
              };
            } else if constexpr (std::is_same_v<Node, LogicalAdvancedOrderNode>) {
              std::vector<PhysicalCoreOrderPlan> core_plans;
              core_plans.reserve(node.core_layouts.size());
              for (std::size_t core = 0; core < node.core_layouts.size(); ++core) {
                const LogicalCoreOrderLayout& layout = node.core_layouts[core];
                const bool bounded =
                    ordered_union_all_runtime_limit ||
                    layout.schedule == OrderEvaluationSchedule::kKeysThenAdmissionThenPayload ||
                    (ordered_union_limit_one_core.has_value() &&
                     core >= *ordered_union_limit_one_core);
                core_plans.push_back(PhysicalCoreOrderPlan{
                    .layout = layout,
                    .strategy = bounded ? PhysicalSortStrategy::kRuntimeLimit
                                        : PhysicalSortStrategy::kExternal,
                });
              }
              return PhysicalAdvancedOrderNode{
                  .input = mapped[node.input.value()],
                  .terms = node.terms,
                  .core_plans = std::move(core_plans),
                  .set_then_order = node.set_then_order,
                  .final_strategy =
                      node.set_then_order && logical_plan.bound_select().limit() != nullptr
                          ? PhysicalSortStrategy::kRuntimeLimit
                          : PhysicalSortStrategy::kExternal,
                  .input_order_satisfied = aggregate_plans.size() == 1U &&
                                           aggregate_plans.front().has_value() &&
                                           aggregate_plans.front()->order_satisfied,
              };
            } else if constexpr (std::is_same_v<Node, LogicalAggregateNode>) {
              const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans[node.core_index]);
              return PhysicalAggregateNode{
                  .input = mapped[node.input.value()],
                  .core_index = node.core_index,
                  .strategy = aggregate.strategy,
                  .min_max = aggregate.min_max,
                  .grouping_order = aggregate.grouping_order,
                  .at_most_one_row = aggregate.at_most_one_row,
              };
            } else if constexpr (std::is_same_v<Node, LogicalHavingNode>) {
              return PhysicalHavingNode{
                  .input = mapped[node.input.value()],
                  .core_index = node.core_index,
                  .predicate = node.predicate,
              };
            } else {
              return PhysicalEmptyNode{};
            }
          },
          logical.payload);
      impl->nodes.push_back(PhysicalNode{.payload = std::move(payload)});
      mapped.emplace_back(static_cast<std::uint32_t>(impl->nodes.size() - 1U));
    }

    impl->root = mapped[logical_plan.root().value()];
    if (aggregate_plans.size() == 1U && aggregate_plans.front().has_value() &&
        !AssumePresent(aggregate_plans.front()).joined) {
      const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans.front());
      if (aggregate.predicates.empty) {
        impl->candidates.push_back(AccessPathCandidate{
            .kind = PhysicalAccessKind::kEmpty,
            .cost = {},
        });
      } else if (aggregate.source.single_row) {
        impl->candidates.push_back(AccessPathCandidate{
            .kind = PhysicalAccessKind::kSingleRow,
            .cost =
                AccessPathCost{
                    .estimated_input_rows = 1,
                    .estimated_output_rows = 1,
                    .work_units = 1,
                },
        });
      } else {
        impl->candidates.reserve(aggregate.access_candidates.size());
        for (const ReadAccessCandidate& candidate : aggregate.access_candidates) {
          impl->candidates.push_back(candidate.published);
        }
        impl->selected_candidate = aggregate.selected_access.value_or(0U);
      }
    }
    const std::span<const AccessPathCandidate> candidates{impl->candidates};
    std::expected<void, OptimizerError> validated = ValidatePhysicalPlan(
        logical_plan, impl->nodes, impl->root, candidates, impl->selected_candidate);
    if (!validated.has_value()) {
      return std::unexpected{std::move(validated.error())};
    }
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPlannerWork, impl->nodes.size());
    return {};
  }
};

class PhysicalStatementPlanBuilder final {
 public:
  [[nodiscard]] static OptimizeLogicalStatementPlanResult Build(LogicalStatementPlan logical_plan) {
    if (!LogicalStatementIsValid(logical_plan)) {
      return std::unexpected{
          OptimizerFailure(OptimizerErrorCode::kInvalidInput, "logical statement plan is invalid")};
    }
    if (std::holds_alternative<LogicalPlan>(logical_plan)) {
      OptimizeLogicalPlanResult select =
          OptimizeLogicalPlan(std::get<LogicalPlan>(std::move(logical_plan)));
      if (!select.has_value()) {
        return std::unexpected{std::move(select.error())};
      }
      return PhysicalStatementPlan{std::in_place_type<PhysicalPlan>, std::move(*select)};
    }
    if (std::holds_alternative<LogicalMutationPlan>(logical_plan)) {
      return BuildMutation(std::get<LogicalMutationPlan>(std::move(logical_plan)));
    }
    if (std::holds_alternative<BoundBeginTransaction>(logical_plan)) {
      return PhysicalStatementPlan{
          std::in_place_type<BoundBeginTransaction>,
          std::get<BoundBeginTransaction>(std::move(logical_plan)),
      };
    }
    if (std::holds_alternative<BoundCommitTransaction>(logical_plan)) {
      return PhysicalStatementPlan{
          std::in_place_type<BoundCommitTransaction>,
          std::get<BoundCommitTransaction>(std::move(logical_plan)),
      };
    }
    if (std::holds_alternative<BoundRollbackTransaction>(logical_plan)) {
      return PhysicalStatementPlan{
          std::in_place_type<BoundRollbackTransaction>,
          std::get<BoundRollbackTransaction>(std::move(logical_plan)),
      };
    }
    if (std::holds_alternative<BoundSavepoint>(logical_plan)) {
      return PhysicalStatementPlan{
          std::in_place_type<BoundSavepoint>,
          std::get<BoundSavepoint>(std::move(logical_plan)),
      };
    }
    if (std::holds_alternative<BoundReleaseSavepoint>(logical_plan)) {
      return PhysicalStatementPlan{
          std::in_place_type<BoundReleaseSavepoint>,
          std::get<BoundReleaseSavepoint>(std::move(logical_plan)),
      };
    }
    return PhysicalStatementPlan{
        std::in_place_type<BoundRollbackToSavepoint>,
        std::get<BoundRollbackToSavepoint>(std::move(logical_plan)),
    };
  }

 private:
  [[nodiscard]] static bool LogicalStatementIsValid(const LogicalStatementPlan& plan) {
    if (plan.valueless_by_exception()) {
      return false;
    }
    return std::visit(
        [](const auto& value) noexcept {
          if constexpr (requires { value.valid(); }) {
            return value.valid();
          }
          return true;
        },
        plan);
  }

  template <typename Bound>
  [[nodiscard]] static PhysicalMutationAccess ChooseAccess(
      const Bound& bound, const BoundMutationTarget& target,
      std::optional<BoundExpressionId> predicate) {
    const CatalogTable& table = bound.catalog()->table(target.table);
    const SourceInfo source{
        .single_row = false,
        .source_kind = BoundSourceKind::kCatalogTable,
        .table = target.table,
        .root_page = target.root_page,
        .estimated_rows = table.statistics.estimated_rows.value_or(kDefaultEstimatedRows),
        .estimated_row_size =
            table.statistics.average_row_size.value_or(DerivedTableRowSize(table)),
        .rowid_eligible = true,
    };
    PredicateAnalysis analysis = AnalyzePredicate(bound, source, predicate);
    PhysicalMutationAccess access{
        .kind = analysis.empty ? MutationAccessKind::kEmpty
                               : (analysis.rowid_key.has_value() ? MutationAccessKind::kRowIdLookup
                                                                 : MutationAccessKind::kTableScan),
        .root_page = target.root_page,
        .key = analysis.rowid_key,
        .guards = std::move(analysis.guards),
        .residuals = std::move(analysis.residuals),
    };
    return access;
  }

  [[nodiscard]] static OptimizeLogicalStatementPlanResult BuildMutation(
      LogicalMutationPlan logical_plan) {
    const BoundStatement& statement = logical_plan.bound_statement();
    PhysicalMutationPayload payload;
    if (std::holds_alternative<BoundInsert>(statement)) {
      payload = PhysicalInsertMutation{
          .atomicity = MutationAtomicity::kStatement,
      };
    } else if (const auto* update = std::get_if<BoundUpdate>(&statement); update != nullptr) {
      PhysicalMutationAccess access =
          ChooseAccess(*update, update->target(), update->where_expression());
      const bool empty = access.kind == MutationAccessKind::kEmpty;
      const bool scan = access.kind == MutationAccessKind::kTableScan;
      payload = PhysicalUpdateMutation{
          .access = std::move(access),
          .atomicity = empty ? MutationAtomicity::kTransaction : MutationAtomicity::kStatement,
          .collect_original_rowids =
              scan && (update->changes_rowid() || !update->target().indexes.empty()),
      };
    } else if (const auto* delete_statement = std::get_if<BoundDelete>(&statement);
               delete_statement != nullptr) {
      PhysicalMutationAccess access = ChooseAccess(*delete_statement, delete_statement->target(),
                                                   delete_statement->where_expression());
      const bool empty = access.kind == MutationAccessKind::kEmpty;
      const bool scan = access.kind == MutationAccessKind::kTableScan;
      payload = PhysicalDeleteMutation{
          .access = std::move(access),
          .atomicity = empty ? MutationAtomicity::kTransaction : MutationAtomicity::kStatement,
          .collect_original_rowids = scan && !delete_statement->target().indexes.empty(),
      };
    } else if (const auto* create = std::get_if<BoundCreateTable>(&statement); create != nullptr) {
      payload = PhysicalCreateTableMutation{
          .no_op = create->no_op(),
          .atomicity =
              create->no_op() ? MutationAtomicity::kTransaction : MutationAtomicity::kStatement,
      };
    } else if (const auto* create_index = std::get_if<BoundCreateIndex>(&statement);
               create_index != nullptr) {
      payload = PhysicalCreateIndexMutation{
          .no_op = create_index->no_op(),
          .atomicity = create_index->no_op() ? MutationAtomicity::kTransaction
                                             : MutationAtomicity::kStatement,
      };
    } else {
      const auto& analyze = std::get<BoundAnalyze>(statement);
      payload = PhysicalAnalyzeMutation{
          .creates_stat1 = !analyze.stat1_root_page().has_value(),
          .atomicity = MutationAtomicity::kStatement,
      };
    }

    auto impl =
        std::make_unique<PhysicalMutationPlan::Impl>(std::move(logical_plan), std::move(payload));
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPlannerWork, 1U);
    return PhysicalStatementPlan{
        std::in_place_type<PhysicalMutationPlan>,
        PhysicalMutationPlan{std::move(impl)},
    };
  }
};

}  // namespace optimizer_detail

std::string_view PhysicalAccessKindName(PhysicalAccessKind kind) noexcept {
  switch (kind) {
    case PhysicalAccessKind::kSingleRow:
      return "single_row";
    case PhysicalAccessKind::kEmpty:
      return "empty";
    case PhysicalAccessKind::kTableScan:
      return "table_scan";
    case PhysicalAccessKind::kRowIdLookup:
      return "rowid_lookup";
    case PhysicalAccessKind::kIndexScan:
      return "index_scan";
  }
  return "unknown";
}

std::string_view MutationAccessKindName(MutationAccessKind kind) noexcept {
  switch (kind) {
    case MutationAccessKind::kEmpty:
      return "empty";
    case MutationAccessKind::kTableScan:
      return "table_scan";
    case MutationAccessKind::kRowIdLookup:
      return "rowid_lookup";
  }
  return "unknown";
}

std::string_view MutationAtomicityName(MutationAtomicity atomicity) noexcept {
  switch (atomicity) {
    case MutationAtomicity::kTransaction:
      return "transaction";
    case MutationAtomicity::kStatement:
      return "statement";
  }
  return "unknown";
}

PhysicalNodeKind PhysicalNodeKindOf(const PhysicalNode& node) noexcept {
  return static_cast<PhysicalNodeKind>(node.payload.index());
}

std::string_view PhysicalNodeKindName(PhysicalNodeKind kind) noexcept {
  switch (kind) {
    case PhysicalNodeKind::kSingleRow:
      return "single_row";
    case PhysicalNodeKind::kEmpty:
      return "empty";
    case PhysicalNodeKind::kTableScan:
      return "table_scan";
    case PhysicalNodeKind::kRowIdLookup:
      return "rowid_lookup";
    case PhysicalNodeKind::kIndexScan:
      return "index_scan";
    case PhysicalNodeKind::kGuard:
      return "guard";
    case PhysicalNodeKind::kFilter:
      return "filter";
    case PhysicalNodeKind::kLimit:
      return "limit";
    case PhysicalNodeKind::kProjection:
      return "projection";
    case PhysicalNodeKind::kSort:
      return "sort";
    case PhysicalNodeKind::kOutput:
      return "output";
    case PhysicalNodeKind::kValues:
      return "values";
    case PhysicalNodeKind::kDistinct:
      return "distinct";
    case PhysicalNodeKind::kCompound:
      return "compound";
    case PhysicalNodeKind::kAdvancedOrder:
      return "advanced_order";
    case PhysicalNodeKind::kAggregate:
      return "aggregate";
    case PhysicalNodeKind::kHaving:
      return "having";
    case PhysicalNodeKind::kNestedLoopJoin:
      return "nested_loop_join";
  }
  return "unknown";
}

std::string_view OptimizerErrorCodeName(OptimizerErrorCode code) noexcept {
  switch (code) {
    case OptimizerErrorCode::kInvalidInput:
      return "invalid_input";
    case OptimizerErrorCode::kUnsupportedFeature:
      return "unsupported_feature";
    case OptimizerErrorCode::kInternalInvariant:
      return "internal_invariant";
  }
  return "unknown";
}

ErrorCode OptimizerError::base_error_code() const noexcept {
  switch (code) {
    case OptimizerErrorCode::kInvalidInput:
      return ErrorCode::kMisuse;
    case OptimizerErrorCode::kUnsupportedFeature:
      return ErrorCode::kGeneric;
    case OptimizerErrorCode::kInternalInvariant:
      return ErrorCode::kInternal;
  }
  return ErrorCode::kInternal;
}

PhysicalPlan::PhysicalPlan(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PhysicalPlan::PhysicalPlan(PhysicalPlan&&) noexcept = default;

PhysicalPlan& PhysicalPlan::operator=(PhysicalPlan&&) noexcept = default;

PhysicalPlan::~PhysicalPlan() = default;

bool PhysicalPlan::valid() const noexcept { return impl_ != nullptr; }

const LogicalPlan& PhysicalPlan::logical_plan() const noexcept { return impl_->logical_plan; }

std::span<const PhysicalNode> PhysicalPlan::nodes() const noexcept {
  return impl_ != nullptr ? std::span<const PhysicalNode>{impl_->root_query.nodes}
                          : std::span<const PhysicalNode>{};
}

const PhysicalNode& PhysicalPlan::node(PhysicalNodeId id) const noexcept {
  return impl_->root_query.nodes[id.value()];
}

PhysicalNodeId PhysicalPlan::root() const noexcept { return impl_->root_query.root; }

std::span<const AccessPathCandidate> PhysicalPlan::candidates() const noexcept {
  return impl_ != nullptr ? std::span<const AccessPathCandidate>{impl_->root_query.candidates}
                          : std::span<const AccessPathCandidate>{};
}

std::optional<std::size_t> PhysicalPlan::selected_candidate_index() const noexcept {
  if (impl_ == nullptr ||
      impl_->root_query.selected_candidate >= impl_->root_query.candidates.size()) {
    return std::nullopt;
  }
  return impl_->root_query.selected_candidate;
}

const AccessPathCandidate* PhysicalPlan::selected_candidate() const noexcept {
  if (impl_ == nullptr ||
      impl_->root_query.selected_candidate >= impl_->root_query.candidates.size()) {
    return nullptr;
  }
  return &impl_->root_query.candidates[impl_->root_query.selected_candidate];
}

const PhysicalQueryPlan& PhysicalPlan::query_plan(BoundQueryId id) const noexcept {
  if (impl_ == nullptr || id.value() >= impl_->logical_plan.bound_select().query_count()) {
    std::terminate();
  }
  return id.value() == 0U ? impl_->root_query : impl_->nested_queries[id.value() - 1U];
}

OptimizeLogicalPlanResult OptimizeLogicalPlan(LogicalPlan logical_plan) {
  return optimizer_detail::PhysicalPlanBuilder::Build(std::move(logical_plan));
}

PhysicalMutationPlan::PhysicalMutationPlan(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PhysicalMutationPlan::PhysicalMutationPlan(PhysicalMutationPlan&&) noexcept = default;

PhysicalMutationPlan& PhysicalMutationPlan::operator=(PhysicalMutationPlan&&) noexcept = default;

PhysicalMutationPlan::~PhysicalMutationPlan() = default;

bool PhysicalMutationPlan::valid() const noexcept { return impl_ != nullptr; }

const LogicalMutationPlan& PhysicalMutationPlan::logical_plan() const noexcept {
  return impl_->logical_plan;
}

const PhysicalMutationPayload& PhysicalMutationPlan::payload() const noexcept {
  return impl_->payload;
}

OptimizeLogicalStatementPlanResult OptimizeLogicalStatementPlan(LogicalStatementPlan logical_plan) {
  return optimizer_detail::PhysicalStatementPlanBuilder::Build(std::move(logical_plan));
}

namespace {

[[nodiscard]] std::string ExplainPhysicalQuery(const PhysicalQueryView& plan) {
  const std::span<const PhysicalNode> nodes = plan.nodes();
  if (nodes.empty()) {
    std::terminate();
  }
  if (IsAdvancedLogicalPlan(plan.logical_plan())) {
    std::string explain;
    const auto append_line = [&explain](std::string_view line) {
      if (!explain.empty()) {
        explain.push_back('\n');
      }
      explain.append(line);
    };
    for (const PhysicalNode& node : nodes) {
      if (const auto* values = std::get_if<PhysicalValuesNode>(&node.payload); values != nullptr) {
        append_line("VALUES CORE " + std::to_string(values->core_index));
      } else if (const auto* single = std::get_if<PhysicalSingleRowNode>(&node.payload);
                 single != nullptr) {
        append_line("CORE " + std::to_string(single->core_index) + " SCAN CONSTANT ROW");
      } else if (const auto* scan = std::get_if<PhysicalTableScanNode>(&node.payload);
                 scan != nullptr) {
        std::string line = "CORE " + std::to_string(scan->core_index) + " SCAN ";
        AppendExplainIdentifier(SourceName(plan, scan->source_kind, scan->table), &line);
        const auto& core = std::get<BoundSelectCore>(
            plan.logical_plan().bound_select().query_cores()[scan->core_index]);
        if (core.table_sources.size() > 1U) {
          line.append(" SOURCE ").append(std::to_string(scan->source.value()));
        }
        if (scan->direction == PhysicalScanDirection::kReverse) {
          line.append(" REVERSE");
        }
        append_line(line);
      } else if (const auto* join = std::get_if<PhysicalNestedLoopJoinNode>(&node.payload);
                 join != nullptr) {
        append_line("CORE " + std::to_string(join->core_index) + " NESTED LOOP " +
                    (join->join == SourceJoinKind::kComma   ? "COMMA"
                     : join->join == SourceJoinKind::kCross ? "CROSS JOIN"
                     : join->join == SourceJoinKind::kLeft  ? "LEFT JOIN"
                     : join->join == SourceJoinKind::kRight ? "RIGHT JOIN"
                     : join->join == SourceJoinKind::kFull  ? "FULL JOIN"
                                                            : "INNER JOIN"));
      } else if (std::holds_alternative<PhysicalEmptyNode>(node.payload)) {
        append_line("EMPTY INPUT");
      } else if (const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&node.payload);
                 lookup != nullptr) {
        std::string line = "SEARCH ";
        AppendExplainIdentifier(SourceName(plan, lookup->source_kind, lookup->table), &line);
        line.append(" USING INTEGER PRIMARY KEY (rowid=?)");
        append_line(line);
      } else if (const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&node.payload);
                 index_scan != nullptr) {
        const CatalogSnapshot& catalog = *plan.logical_plan().bound_select().catalog();
        const CatalogTable& table = catalog.table(index_scan->table);
        const CatalogIndex& index = catalog.index(index_scan->index);
        std::string line =
            index_scan->equalities.empty() && !index_scan->range.has_value() ? "SCAN " : "SEARCH ";
        AppendExplainIdentifier(table.name, &line);
        line.append(index_scan->covering ? " USING COVERING INDEX " : " USING INDEX ");
        AppendExplainIdentifier(index.name, &line);
        if (index_scan->direction == PhysicalScanDirection::kReverse) {
          line.append(" REVERSE");
        }
        append_line(line);
      } else if (const auto* aggregate = std::get_if<PhysicalAggregateNode>(&node.payload);
                 aggregate != nullptr) {
        std::string line = "AGGREGATE CORE " + std::to_string(aggregate->core_index) + " ";
        switch (aggregate->strategy) {
          case PhysicalAggregateStrategy::kGlobal:
            line.append("GLOBAL");
            break;
          case PhysicalAggregateStrategy::kIndexedMinMax:
            line.append("INDEXED MIN/MAX");
            break;
          case PhysicalAggregateStrategy::kOrderedGroup:
            line.append("GROUPS USING INPUT ORDER");
            break;
          case PhysicalAggregateStrategy::kSortedGroup:
            line.append("GROUPS USING TEMP SORTER");
            break;
        }
        append_line(line);
      } else if (const auto* having = std::get_if<PhysicalHavingNode>(&node.payload);
                 having != nullptr) {
        append_line("HAVING CORE " + std::to_string(having->core_index));
      } else if (const auto* sort = std::get_if<PhysicalSortNode>(&node.payload); sort != nullptr) {
        append_line(sort->input_order_satisfied ? "ORDER USING INPUT ORDER" : "ORDER USING SORTER");
      } else if (const auto* distinct = std::get_if<PhysicalDistinctNode>(&node.payload);
                 distinct != nullptr) {
        append_line("DISTINCT CORE " + std::to_string(distinct->core_index) +
                    " USING EPHEMERAL RELATION");
      } else if (const auto* compound = std::get_if<PhysicalCompoundNode>(&node.payload);
                 compound != nullptr) {
        std::string strategy = "EPHEMERAL SET";
        switch (compound->strategy) {
          case PhysicalCompoundStrategy::kConcatenate:
            strategy = "CONCATENATE";
            break;
          case PhysicalCompoundStrategy::kUnionLimitOne:
            strategy = "UNION LIMIT ONE";
            break;
          case PhysicalCompoundStrategy::kEphemeralSet:
            break;
          case PhysicalCompoundStrategy::kOrderedMerge:
            strategy = "ORDERED MERGE";
            break;
          case PhysicalCompoundStrategy::kSetThenOrder:
            strategy = "SET THEN ORDER";
            break;
        }
        append_line("COMPOUND USING " + strategy);
      } else if (const auto* order = std::get_if<PhysicalAdvancedOrderNode>(&node.payload);
                 order != nullptr) {
        if (order->input_order_satisfied) {
          append_line("ORDER USING INPUT ORDER");
        } else {
          append_line(order->set_then_order ? "ORDER SET RESULT" : "ORDER ARM STREAMS");
        }
      }
    }
    return explain;
  }
  const PhysicalNode& access = nodes[0];
  std::string explain;
  if (std::holds_alternative<PhysicalSingleRowNode>(access.payload)) {
    explain = "SCAN CONSTANT ROW";
  } else if (std::holds_alternative<PhysicalEmptyNode>(access.payload)) {
    explain = "EMPTY RESULT";
  } else if (const auto* scan = std::get_if<PhysicalTableScanNode>(&access.payload);
             scan != nullptr) {
    explain = "SCAN ";
    AppendExplainIdentifier(SourceName(plan, scan->source_kind, scan->table), &explain);
  } else if (const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&access.payload);
             index_scan != nullptr) {
    const BoundQueryView bound_select = plan.logical_plan().bound_select();
    const CatalogSnapshot& catalog = *bound_select.catalog();
    const CatalogTable& table = catalog.table(index_scan->table);
    const CatalogIndex& index = catalog.index(index_scan->index);
    const bool constrained = !index_scan->equalities.empty() || index_scan->range.has_value();
    explain = constrained ? "SEARCH " : "SCAN ";
    AppendExplainIdentifier(table.name, &explain);
    explain.append(index_scan->covering ? " USING COVERING INDEX " : " USING INDEX ");
    AppendExplainIdentifier(index.name, &explain);
    if (constrained) {
      explain.append(" (");
      bool first = true;
      const auto append_constraint = [&](ColumnId column, std::string_view operation) {
        if (!first) {
          explain.append(" AND ");
        }
        first = false;
        AppendExplainIdentifier(table.columns[column.value].name, &explain);
        explain.append(operation);
      };
      for (const PhysicalIndexEquality& equality : index_scan->equalities) {
        append_constraint(equality.column, equality.reject_null ? "=?" : " IS ?");
      }
      if (index_scan->range.has_value() && index_scan->range->lower.has_value()) {
        append_constraint(index_scan->range->column,
                          index_scan->range->lower->inclusive ? ">=?" : ">?");
      }
      if (index_scan->range.has_value() && index_scan->range->upper.has_value()) {
        append_constraint(index_scan->range->column,
                          index_scan->range->upper->inclusive ? "<=?" : "<?");
      }
      explain.push_back(')');
    }
  } else {
    const auto& lookup = std::get<PhysicalRowIdLookupNode>(access.payload);
    explain = "SEARCH ";
    AppendExplainIdentifier(SourceName(plan, lookup.source_kind, lookup.table), &explain);
    explain.append(" USING INTEGER PRIMARY KEY (rowid=?)");
  }

  const BoundQueryView bound_select = plan.logical_plan().bound_select();
  for (const PhysicalNode& node : nodes) {
    const auto* sort = std::get_if<PhysicalSortNode>(&node.payload);
    if (sort == nullptr) {
      continue;
    }
    explain.append("\nSORT ");
    explain.append(std::to_string(sort->terms.size()));
    explain.append(sort->terms.size() == 1U ? " TERM (" : " TERMS (");
    for (std::size_t index = 0; index < sort->terms.size(); ++index) {
      if (index != 0U) {
        explain.append(", ");
      }
      const BoundOrderingTerm& term = sort->terms[index];
      explain.append("COLLATE ");
      AppendExplainIdentifier(bound_select.collations()[term.collation.value()].name, &explain);
      explain.append(term.order == SortOrder::kDescending ? " DESC " : " ASC ");
      explain.append(term.null_placement == BoundNullPlacement::kLast ? "NULLS LAST"
                                                                      : "NULLS FIRST");
    }
    explain.append(sort->input_order_satisfied ? ") USING INPUT ORDER" : ") USING SORTER");
    break;
  }
  return explain;
}

}  // namespace

std::string ExplainPhysicalPlan(const PhysicalPlan& plan) {
  std::string explain = ExplainPhysicalQuery(PhysicalQueryView{plan});
  const BoundSelect& bound = plan.logical_plan().bound_select();
  for (std::size_t index = 1; index < bound.query_count(); ++index) {
    const BoundQueryId id{static_cast<std::uint32_t>(index)};
    explain.append(bound.query(id).destination == BoundQueryDestination::kExists
                       ? "\nEXISTS SUBQUERY "
                       : "\nSCALAR SUBQUERY ");
    explain.append(std::to_string(index)).push_back('\n');
    explain.append(ExplainPhysicalQuery(PhysicalQueryView{plan, id}));
  }
  return explain;
}

}  // namespace modern_sqlite
