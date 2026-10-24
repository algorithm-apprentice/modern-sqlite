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
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {
namespace {

constexpr std::uint64_t kDefaultEstimatedRows = 1U << 20U;
constexpr std::uint64_t kMinimumDefaultIndexRows = 1'000;
using LogEst = std::int32_t;
using SourceMask = std::uint64_t;
constexpr std::size_t kTopColumn = std::numeric_limits<SourceMask>::digits - 1U;
constexpr std::size_t kMaximumOrderedTerms = std::numeric_limits<SourceMask>::digits - 1U;
constexpr LogEst kLogEstScale = 10;
constexpr LogEst kTableScanCostBias = 16;
constexpr LogEst kIndexScanCostBase = 1;
constexpr LogEst kIndexWidthCostScale = 15;
constexpr LogEst kRowIdShortcutCost = 33;
constexpr LogEst kUniqueShortcutCost = 39;
constexpr LogEst kShortcutOutput = 1;
constexpr LogEst kRangeReduction = 20;
constexpr LogEst kMinimumRangeOutput = 10;
constexpr std::size_t kShortcutIndexTermLimit = 3;
constexpr std::size_t kPlannerSearchLimit = 20'000;
constexpr std::size_t kPlannerSearchLimitIncrement = 1'000;
constexpr std::size_t kMaximumEquivalentColumns = 11;
constexpr LogEst kAutomaticIndexSetupBias = 28;
constexpr LogEst kAutomaticIndexOutput = 43;
constexpr LogEst kMaximumOuterLoopEstimate = 48;
constexpr LogEst kDistinctOutputReduction = 30;
constexpr LogEst kDefaultSelectRows = 320;
constexpr LogEst kPartialSortPercentScale = 66;
constexpr std::string_view kTwoPassNullOrderExplanation = " TWO-PASS NULL ORDER";

[[nodiscard]] std::optional<std::size_t> SourcePosition(std::span<const BoundTableSource> sources,
                                                        BoundSourceId source) noexcept {
  const auto found = std::ranges::find(sources, source, &BoundTableSource::id);
  return found == sources.end() ? std::nullopt
                                : std::optional{static_cast<std::size_t>(found - sources.begin())};
}

[[nodiscard]] constexpr SourceMask SourceBit(std::size_t position) noexcept {
  if (position >= kMaximumSelectSources) {
    std::terminate();
  }
  return SourceMask{1} << position;
}

[[nodiscard]] constexpr SourceMask AllSources(std::size_t count) noexcept {
  return count == kMaximumSelectSources ? ~SourceMask{0} : SourceBit(count) - 1U;
}

[[nodiscard]] constexpr LogEst ToLogEst(std::uint64_t value) noexcept {
  constexpr std::array<LogEst, 8> kFraction{0, 2, 3, 5, 6, 7, 8, 9};
  LogEst exponent = 4 * kLogEstScale;
  if (value < kFraction.size()) {
    if (value < 2U) {
      return 0;
    }
    while (value < kFraction.size()) {
      exponent -= kLogEstScale;
      value <<= 1U;
    }
  } else {
    const auto shift = static_cast<unsigned>(std::bit_width(value)) - 4U;
    exponent += static_cast<LogEst>(shift) * kLogEstScale;
    value >>= shift;
  }
  return kFraction[value & 7U] + exponent - kLogEstScale;
}

[[nodiscard]] constexpr LogEst AddLogEst(LogEst left, LogEst right) noexcept {
  constexpr std::array<LogEst, 32> kCorrection{
      10, 10, 9, 9, 8, 8, 7, 7, 7, 6, 6, 6, 5, 5, 5, 4,
      4,  4,  4, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2,
  };
  if (left < right) {
    std::swap(left, right);
  }
  const LogEst difference = left - right;
  if (difference > 49) {
    return left;
  }
  if (std::cmp_greater_equal(difference, kCorrection.size())) {
    return left + 1;
  }
  return left + kCorrection[static_cast<std::size_t>(difference)];
}

[[nodiscard]] constexpr LogEst LogEstLogarithm(LogEst value) noexcept {
  return value <= kLogEstScale ? 0 : ToLogEst(static_cast<std::uint64_t>(value)) - 33;
}

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

struct LoopColumnReference {
  std::size_t source_position = 0;
  std::optional<ColumnId> column{};

  constexpr auto operator<=>(const LoopColumnReference&) const noexcept = default;
};

struct ReadPredicateTerm {
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
  PhysicalPredicateOrigin origin = PhysicalPredicateOrigin::kWhere;
  std::optional<BoundSourceId> on_source{};
  std::optional<BoundSourceId> constraint_owner{};
  SourceMask prerequisites = 0;
  SourceMask key_floor = 0;
  SourceMask left_prerequisites = 0;
  SourceMask right_prerequisites = 0;
  const BoundComparisonExpression* comparison = nullptr;
  std::optional<LoopColumnReference> left_column{};
  std::optional<LoopColumnReference> right_column{};
  LogEst truth_probability = 1;
  bool outer_on = false;
  bool equivalence = false;
  bool left_is_column = false;
  bool right_is_column = false;
  std::optional<BoundSourceId> evaluation_source{};
  bool consumed_by_access = false;
};

struct ReadPredicateAnalysis {
  bool empty = false;
  std::vector<BoundExpressionId> guards{};
  std::vector<ReadPredicateTerm> terms{};
};

struct RowIdCandidatePlan {
  BoundExpressionId key{0};
  std::size_t selected_term = 0;
  ReadPredicateTerm constraint{};
};

struct RowIdRangeCandidatePlan {
  PhysicalRowIdRangeNode node{};
  std::vector<bool> selected_terms{};
};

struct IndexCandidatePlan {
  PhysicalIndexScanNode node{};
  std::vector<bool> selected_terms{};
};

struct AutomaticCandidatePlan {
  PhysicalAutomaticIndexNode node{};
  std::vector<bool> selected_terms{};
};

using ReadAccessPayload = std::variant<std::monostate, RowIdCandidatePlan, RowIdRangeCandidatePlan,
                                       IndexCandidatePlan, AutomaticCandidatePlan>;

struct ReadAccessCandidate {
  AccessPathCandidate published{};
  ReadAccessPayload payload{};
  bool single_row_shortcut = false;
  std::size_t source_position = 0;
  SourceMask prerequisites = 0;
  std::size_t sort_index = 0;
  bool transitive = false;
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;
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
[[nodiscard]] T& AssumePresent(std::optional<T>& value) noexcept {
  if (!value.has_value()) {
    std::terminate();
  }
  return *value;
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

template <typename Bound>
[[nodiscard]] auto CaptureLookup(const Bound& bound_select) noexcept {
  if constexpr (requires { bound_select.owner().query_count(); }) {
    return
        [&bound_select](BoundQueryId query) -> std::optional<std::span<const BoundQueryCapture>> {
          if (query.value() < bound_select.owner().query_count()) {
            return std::span<const BoundQueryCapture>{bound_select.owner().query(query).captures};
          }
          return std::nullopt;
        };
  } else {
    return [](BoundQueryId) -> std::optional<std::span<const BoundQueryCapture>> {
      return std::nullopt;
    };
  }
}

template <typename Bound>
[[nodiscard]] bool IsCapturedSource(const Bound& bound_select, BoundSourceId source) noexcept {
  if constexpr (requires { bound_select.query().captures; }) {
    return std::ranges::any_of(bound_select.query().captures,
                               [source](const BoundQueryCapture& dependency) {
                                 const auto* capture = std::get_if<BoundSourceCapture>(&dependency);
                                 return capture != nullptr && capture->identity.source == source;
                               });
  }
  return false;
}

template <typename Bound, typename Visitor>
[[nodiscard]] bool VisitExpressionSources(const Bound& bound_select, BoundExpressionId id,
                                          Visitor& visitor) {
  return binder_detail::VisitSourceOwners(bound_select.expressions(), bound_select.source_columns(),
                                          id, CaptureLookup(bound_select), visitor);
}

template <typename Bound>
[[nodiscard]] bool ExpressionDependsOnSource(const Bound& bound_select, BoundExpressionId id) {
  bool depends = false;
  auto visitor = [&](BoundSourceId source) {
    depends = depends || !IsCapturedSource(bound_select, source);
  };
  return !VisitExpressionSources(bound_select, id, visitor) || depends;
}

template <typename Bound>
[[nodiscard]] std::expected<SourceMask, OptimizerError> ExpressionSourceMask(
    const Bound& bound_select, std::span<const BoundTableSource> sources, BoundExpressionId id) {
  SourceMask mask = 0;
  bool valid = true;
  auto visitor = [&](BoundSourceId source) {
    if (IsCapturedSource(bound_select, source)) {
      return;
    }
    const auto position = SourcePosition(sources, source);
    if (!position.has_value()) {
      valid = false;
    } else {
      mask |= SourceBit(*position);
    }
  };
  if (!VisitExpressionSources(bound_select, id, visitor) || !valid) {
    return std::unexpected(InvariantFailure("expression has invalid source dependencies"));
  }
  return mask;
}

[[nodiscard]] LogEst PredicateTruthProbability(const BoundQueryView& bound,
                                               BoundExpressionId id) noexcept {
  constexpr auto kProbabilityScale = std::uint64_t{1} << 27U;
  constexpr LogEst kProbabilityLogScale = 270;
  while (IsValidExpressionId(bound, id)) {
    const auto& payload = bound.expression(id).payload;
    if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&payload);
        likelihood != nullptr) {
      return ToLogEst(static_cast<std::uint64_t>(likelihood->probability *
                                                 static_cast<double>(kProbabilityScale))) -
             kProbabilityLogScale;
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&payload);
        alias != nullptr) {
      id = alias->target;
    } else if (const auto* collate = std::get_if<BoundCollateExpression>(&payload);
               collate != nullptr) {
      id = collate->operand;
    } else {
      break;
    }
  }
  return 1;
}

[[nodiscard]] bool SourceIsLeftOfRightJoin(const BoundSelectCore& core,
                                           std::size_t position) noexcept {
  return std::ranges::any_of(
      std::span{core.table_sources}.subspan(position + 1U),
      [](const BoundTableSource& source) { return SourceJoinPreservesRight(source.join); });
}

[[nodiscard]] bool ConstraintCompatibleWithSource(const BoundSelectCore& core, std::size_t position,
                                                  const ReadPredicateTerm& term) noexcept {
  const auto& source = core.table_sources[position];
  const bool outer = SourceJoinPreservesLeft(source.join) || SourceJoinPreservesRight(source.join);
  if (!outer && !SourceIsLeftOfRightJoin(core, position)) {
    return true;
  }
  return term.constraint_owner == source.id && (!outer || term.outer_on);
}

[[nodiscard]] std::optional<LoopColumnReference> LoopColumnOf(const BoundQueryView& bound,
                                                              const BoundSelectCore& core,
                                                              BoundExpressionId expression);
[[nodiscard]] bool TermIsEquivalence(const BoundQueryView& bound, const BoundSelectCore& core,
                                     const ReadPredicateTerm& term);

[[nodiscard]] std::optional<BoundSourceLeafIdentity> LoopOperandIdentity(
    const BoundQueryView& bound, BoundExpressionId expression) {
  expression = TransparentRowIdOperandId(bound, expression);
  return IsValidExpressionId(bound, expression)
             ? BoundSourceLeafIdentityOf(bound.expression(expression), bound.source_columns())
             : std::nullopt;
}

[[nodiscard]] std::expected<void, OptimizerError> PrepareLoopTerms(
    const BoundQueryView& bound, const BoundSelectCore& core, std::span<ReadPredicateTerm> terms) {
  for (ReadPredicateTerm& term : terms) {
    const auto transparent = TransparentPredicateId(bound, term.predicate);
    if (!IsValidExpressionId(bound, transparent)) {
      return std::unexpected(InvariantFailure("loop term has an invalid expression"));
    }
    term.comparison =
        std::get_if<BoundComparisonExpression>(&bound.expression(transparent).payload);
    if (term.comparison != nullptr) {
      auto left = ExpressionSourceMask(bound, core.table_sources, term.comparison->left);
      auto right = ExpressionSourceMask(bound, core.table_sources, term.comparison->right);
      if (!left.has_value()) {
        return std::unexpected(std::move(left.error()));
      }
      if (!right.has_value()) {
        return std::unexpected(std::move(right.error()));
      }
      term.left_prerequisites = *left;
      term.right_prerequisites = *right;
      term.prerequisites = *left | *right;
      term.left_column = LoopColumnOf(bound, core, term.comparison->left);
      term.right_column = LoopColumnOf(bound, core, term.comparison->right);
      term.left_is_column = LoopOperandIdentity(bound, term.comparison->left).has_value();
      term.right_is_column = LoopOperandIdentity(bound, term.comparison->right).has_value();
    } else {
      auto prerequisites = ExpressionSourceMask(bound, core.table_sources, term.predicate);
      if (!prerequisites.has_value()) {
        return std::unexpected(std::move(prerequisites.error()));
      }
      term.prerequisites = *prerequisites;
    }
    term.truth_probability = PredicateTruthProbability(bound, term.predicate);
    if (term.on_source.has_value()) {
      const auto position = SourcePosition(core.table_sources, *term.on_source);
      if (!position.has_value()) {
        return std::unexpected(InvariantFailure("ON term has no declared source owner"));
      }
      const auto join = core.table_sources[*position].join;
      term.outer_on = SourceJoinPreservesLeft(join) || SourceJoinPreservesRight(join);
      const SourceMask owner = SourceBit(*position);
      if (term.outer_on) {
        term.constraint_owner = term.on_source;
        term.prerequisites |= owner;
        term.key_floor = owner - 1U;
      } else if ((term.prerequisites >> 1U) < owner) {
        term.constraint_owner = term.on_source;
      }
    }
    term.equivalence = TermIsEquivalence(bound, core, term);
  }
  return {};
}

[[nodiscard]] std::vector<ReadPredicateTerm> CollectLoopTerms(const BoundQueryView& bound,
                                                              const BoundSelectCore& core) {
  std::vector<ReadPredicateTerm> terms;
  const auto append = [&](BoundExpressionId expression, std::optional<BoundSourceId> owner) {
    std::size_t position = 0;
    auto visitor = [&](BoundExpressionId predicate) {
      terms.push_back(ReadPredicateTerm{
          .predicate = predicate,
          .conjunct_position = position++,
          .origin =
              owner.has_value() ? PhysicalPredicateOrigin::kOn : PhysicalPredicateOrigin::kWhere,
          .on_source = owner,
      });
      return true;
    };
    static_cast<void>(VisitConjuncts(bound, expression, &visitor));
  };
  if (core.where.has_value()) {
    append(*core.where, std::nullopt);
  }
  for (const BoundTableSource& source : core.table_sources) {
    if (source.on.has_value()) {
      append(*source.on, source.id);
    }
  }
  return terms;
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

template <typename Bound>
[[nodiscard]] bool IsOrderingConstant(const Bound& bound_select, BoundExpressionId id) {
  bool has_source = false;
  auto visitor = [&](BoundSourceId) { has_source = true; };
  // Captures are stable for a WHERE invocation, not ORDER BY elimination.
  return VisitExpressionSources(bound_select, id, visitor) && !has_source &&
         ExpressionIsDeterministic(bound_select, id);
}

[[nodiscard]] bool CanFilterAutomaticIndex(const BoundQueryView& bound, const BoundSelectCore& core,
                                           std::size_t position, const ReadPredicateTerm& term) {
  const auto& source = core.table_sources[position];
  if (SourceIsLeftOfRightJoin(core, position) ||
      (SourceJoinPreservesLeft(source.join) ? !term.outer_on || term.on_source != source.id
                                            : term.outer_on) ||
      !ExpressionIsDeterministic(bound, term.predicate)) {
    return false;
  }
  bool single_table = true;
  auto visitor = [&](BoundSourceId dependency) { single_table &= dependency == source.id; };
  return VisitExpressionSources(bound, term.predicate, visitor) && single_table;
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
[[nodiscard]] std::optional<BoundExpressionId> RowIdEqualityKey(const Bound& bound_select,
                                                                const SourceInfo& source,
                                                                BoundExpressionId predicate) {
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
  return left_rowid ? comparison->right : comparison->left;
}

template <typename Bound>
[[nodiscard]] std::optional<BoundExpressionId> RowIdLookupKey(const Bound& bound_select,
                                                              const SourceInfo& source,
                                                              BoundExpressionId predicate) {
  const auto key = RowIdEqualityKey(bound_select, source, predicate);
  if (!key.has_value() || ExpressionDependsOnSource(bound_select, *key)) {
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
  std::uint64_t estimated_rows = table.statistics.estimated_rows.value_or(kDefaultEstimatedRows);
  if (estimated_rows < kMinimumDefaultIndexRows &&
      std::ranges::any_of(
          bound_select.catalog()->table_indexes(table_id),
          [&](IndexId index) {
            return !bound_select.catalog()->index(index).statistics.has_stat1;
          })) {
    estimated_rows = kMinimumDefaultIndexRows;
  }
  return SourceInfo{
      .single_row = false,
      .id = source.id,
      .source_kind = BoundSourceKind::kCatalogTable,
      .table = table_id,
      .root_page = table.root_page,
      .estimated_rows = estimated_rows,
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
  return BoundQueryHasSingleRowDemand(bound_select.query(), bound_select.expressions());
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

void MarkRequiredSourceValues(const BoundQueryView& bound_select, BoundSourceId source_id,
                              BoundExpressionId id, std::vector<bool>* columns, bool* rowid) {
  auto visitor = [&](const BoundSourceLeafIdentity& identity, std::optional<BoundExpressionId>) {
    if (identity.source != source_id) {
      return true;
    }
    if (!identity.column.has_value()) {
      *rowid = true;
    } else {
      const auto column = bound_select.source_columns()[identity.column->value()].catalog_column;
      if (column.has_value() && column->value < columns->size()) {
        (*columns)[column->value] = true;
      }
    }
    return true;
  };
  if (!binder_detail::VisitSourceLeaves(bound_select.expressions(), bound_select.source_columns(),
                                        id, CaptureLookup(bound_select),
                                        BoundSourceDependency::kValue, visitor)) {
    std::terminate();
  }
}

struct RequiredSourceValues {
  std::vector<bool> columns{};
  bool rowid = false;
};

[[nodiscard]] RequiredSourceValues DiscoverRequiredSourceValues(
    const BoundQueryView& bound_select, BoundSourceId source_id, const CatalogTable& table,
    std::span<const ReadPredicateTerm> predicates, const std::vector<bool>& selected_terms,
    const BoundSelectCore* select_core) {
  RequiredSourceValues required{.columns = std::vector<bool>(table.columns.size(), false)};
  std::vector<bool>& required_columns = required.columns;
  bool& required_rowid = required.rowid;
  const bool joined = select_core != nullptr && select_core->table_sources.size() > 1U;
  for (std::size_t index_value = 0; index_value < predicates.size(); ++index_value) {
    if (index_value >= selected_terms.size() || !selected_terms[index_value]) {
      MarkRequiredSourceValues(bound_select, source_id, predicates[index_value].predicate,
                               &required_columns, &required_rowid);
    }
  }
  if (joined) {
    for (const BoundTableSource& source : select_core->table_sources) {
      if (source.on.has_value()) {
        MarkRequiredSourceValues(bound_select, source_id, *source.on, &required_columns,
                                 &required_rowid);
      }
    }
    if (select_core->where.has_value()) {
      MarkRequiredSourceValues(bound_select, source_id, *select_core->where, &required_columns,
                               &required_rowid);
    }
  }
  if (select_core != nullptr && select_core->aggregate.has_value()) {
    const BoundAggregateProgram& aggregate = *select_core->aggregate;
    if (aggregate.group_by.empty()) {
      for (const BoundAggregateInvocationId id : aggregate.invocations) {
        const BoundAggregateInvocation& invocation =
            bound_select.aggregate_invocations()[id.value()];
        for (const BoundExpressionId argument : invocation.arguments) {
          MarkRequiredSourceValues(bound_select, source_id, argument, &required_columns,
                                   &required_rowid);
        }
        if (invocation.filter.has_value()) {
          MarkRequiredSourceValues(bound_select, source_id, *invocation.filter, &required_columns,
                                   &required_rowid);
        }
        if (bound_select.aggregate_functions()[invocation.function.value()].extremum ==
            BoundAggregateExtremumKind::kNone) {
          for (const BoundOrderingTerm& term : invocation.order_by) {
            MarkRequiredSourceValues(bound_select, source_id, term.expression, &required_columns,
                                     &required_rowid);
          }
        }
      }
    } else {
      for (const BoundExpressionId expression : aggregate.source_leaves) {
        MarkRequiredSourceValues(bound_select, source_id, expression, &required_columns,
                                 &required_rowid);
      }
    }
    for (const BoundExpressionId expression : aggregate.representative_leaves) {
      MarkRequiredSourceValues(bound_select, source_id, expression, &required_columns,
                               &required_rowid);
    }
  } else if (BoundQueryRequiresResultValues(bound_select.query(), bound_select.expressions())) {
    const std::span<const BoundResultColumn> results =
        select_core != nullptr ? std::span<const BoundResultColumn>{select_core->result_columns}
                               : bound_select.result_columns();
    for (const BoundResultColumn& result : results) {
      MarkRequiredSourceValues(bound_select, source_id, result.expression, &required_columns,
                               &required_rowid);
    }
  }
  if (select_core == nullptr || !select_core->aggregate.has_value() || joined) {
    for (const BoundOrderingTerm& ordering : bound_select.order_by()) {
      MarkRequiredSourceValues(bound_select, source_id, ordering.expression, &required_columns,
                               &required_rowid);
    }
  }
  return required;
}

[[nodiscard]] bool IsCoveringIndex(const BoundQueryView& bound_select, BoundSourceId source_id,
                                   const CatalogTable& table, const CatalogIndex& index,
                                   std::span<const ReadPredicateTerm> predicates,
                                   const std::vector<bool>& selected_terms,
                                   const BoundSelectCore* select_core = nullptr) {
  const RequiredSourceValues required = DiscoverRequiredSourceValues(
      bound_select, source_id, table, predicates, selected_terms, select_core);
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
  for (std::size_t column = 0; column < required.columns.size(); ++column) {
    if (required.columns[column] && !covered_columns[column]) {
      return false;
    }
  }
  return !required.rowid || covered_rowid;
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

[[nodiscard]] LogEst DefaultPrefixLogEst(std::size_t equality_count) noexcept {
  constexpr std::array<LogEst, 6> kDefaults{33, 32, 30, 28, 26, 23};
  return kDefaults[std::min(equality_count, kDefaults.size()) - 1U];
}

[[nodiscard]] std::uint64_t IndexCardinality(const CatalogIndex& index,
                                             std::uint64_t table_rows) noexcept {
  if (index.statistics.has_stat1 && !index.statistics.rows_per_prefix.empty()) {
    return index.statistics.rows_per_prefix.front();
  }
  return table_rows;
}

[[nodiscard]] LogEst EstimateEqualityPrefixLogEst(
    const BoundQueryView& bound_select, std::span<const PhysicalIndexEquality> equalities,
    std::uint64_t input_rows, std::span<const std::uint64_t> rows_per_prefix = {}) noexcept {
  LogEst estimate = ToLogEst(input_rows);
  LogEst previous_prefix = estimate;
  for (std::size_t equality = 0; equality < equalities.size(); ++equality) {
    const std::size_t prefix = equality + 1U;
    const LogEst next_prefix = prefix < rows_per_prefix.size() ? ToLogEst(rows_per_prefix[prefix])
                                                               : DefaultPrefixLogEst(prefix);
    estimate += next_prefix - previous_prefix;
    previous_prefix = next_prefix;
    if (!equalities[equality].reject_null &&
        IsNullLiteral(bound_select, equalities[equality].key)) {
      estimate += kLogEstScale;
    }
  }
  return estimate;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] LogEst EstimateRangeOutputLogEst(LogEst input, std::uint32_t bound_count) noexcept {
  const auto bounds = static_cast<LogEst>(bound_count);
  const LogEst reduction = (bounds + static_cast<LogEst>(bound_count == 2U)) * kRangeReduction;
  return std::min(input - bounds, std::max(input - reduction, kMinimumRangeOutput));
}

[[nodiscard]] LogEst EstimateIndexOutputLogEst(const BoundQueryView& bound_select,
                                               const CatalogIndex& index,
                                               const PhysicalIndexScanNode& node,
                                               std::uint64_t index_rows) noexcept {
  const std::span<const std::uint64_t> prefixes =
      index.statistics.has_stat1 ? std::span<const std::uint64_t>{index.statistics.rows_per_prefix}
                                 : std::span<const std::uint64_t>{};
  LogEst estimate =
      EstimateEqualityPrefixLogEst(bound_select, node.equalities, index_rows, prefixes);
  if (node.equalities.size() == index.key_term_count && index.unique) {
    const bool only_ordinary_equalities = std::ranges::all_of(
        node.equalities,
        [](const PhysicalIndexEquality& equality) { return equality.reject_null; });
    if (only_ordinary_equalities || index.unique_not_null) {
      estimate = std::min<LogEst>(estimate, 0);
    }
  }

  if (node.range.has_value()) {
    const auto bounds = static_cast<std::uint32_t>(node.range->lower.has_value()) +
                        static_cast<std::uint32_t>(node.range->upper.has_value());
    estimate = EstimateRangeOutputLogEst(estimate, bounds);
  }
  return estimate;
}

[[nodiscard]] AccessPathCost ScanCost(std::uint64_t rows) noexcept {
  const LogEst output = ToLogEst(rows);
  return AccessPathCost{
      .run_log_est = output + kTableScanCostBias,
      .output_log_est = output,
  };
}

[[nodiscard]] AccessPathCost RowIdCost(std::uint64_t rows, bool shortcut = false) noexcept {
  return AccessPathCost{
      .run_log_est = shortcut ? kRowIdShortcutCost
                              : AddLogEst(LogEstLogarithm(ToLogEst(rows)), kTableScanCostBias),
      .output_log_est = shortcut ? kShortcutOutput : 0,
  };
}

[[nodiscard]] bool IsOneRowIndexLookup(const BoundQueryView& bound_select,
                                       const PhysicalIndexScanNode& node) noexcept {
  const CatalogIndex& index = bound_select.catalog()->index(node.index);
  return index.unique && node.equalities.size() == index.key_term_count &&
         std::ranges::all_of(node.equalities, [&](const PhysicalIndexEquality& equality) {
           return equality.reject_null ||
                  (index.unique_not_null && !IsNullLiteral(bound_select, equality.key));
         });
}

[[nodiscard]] bool IsUniqueLookupShortcut(const BoundQueryView& bound_select,
                                          const PhysicalIndexScanNode& node) noexcept {
  return bound_select.catalog()->index(node.index).key_term_count <= kShortcutIndexTermLimit &&
         IsOneRowIndexLookup(bound_select, node);
}

[[nodiscard]] AccessPathCandidate PublishIndexCandidate(
    const BoundQueryView& bound_select, const SourceInfo& source, const IndexCandidatePlan& plan,
    bool allow_shortcut = false, std::optional<LogEst> output_estimate = std::nullopt) noexcept {
  const CatalogSnapshot& catalog = *bound_select.catalog();
  const CatalogIndex& index = catalog.index(plan.node.index);
  const std::uint64_t index_rows = IndexCardinality(index, source.estimated_rows);
  const std::uint64_t row_size =
      index.statistics.average_row_size.value_or(DerivedIndexRowSize(catalog, index));
  LogEst output = output_estimate.has_value()
                      ? *output_estimate
                      : EstimateIndexOutputLogEst(bound_select, index, plan.node, index_rows);
  const bool constrained = !plan.node.equalities.empty() || plan.node.range.has_value();
  const LogEst table_width = std::max<LogEst>(ToLogEst(source.estimated_row_size), 1);
  LogEst run =
      output + kIndexScanCostBase + (kIndexWidthCostScale * ToLogEst(row_size)) / table_width;
  if (constrained) {
    run = AddLogEst(LogEstLogarithm(ToLogEst(index_rows)), run);
  }
  if (!plan.node.covering) {
    run = AddLogEst(run, output + kTableScanCostBias);
  }
  if (allow_shortcut && IsUniqueLookupShortcut(bound_select, plan.node)) {
    run = kUniqueShortcutCost;
    output = kShortcutOutput;
  }
  return AccessPathCandidate{
      .kind = PhysicalAccessKind::kIndexScan,
      .cost =
          AccessPathCost{
              .run_log_est = run,
              .output_log_est = output,
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

[[nodiscard]] bool PreferCandidate(const AccessPathCandidate& candidate,
                                   const AccessPathCandidate& selected) noexcept {
  return candidate.cost.run_log_est < selected.cost.run_log_est ||
         (candidate.cost.run_log_est == selected.cost.run_log_est &&
          candidate.cost.output_log_est < selected.cost.output_log_est);
}

template <typename Visitor>
void VisitLoopTerms(const ReadAccessCandidate& loop, Visitor&& visitor) {
  std::visit(
      [&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, RowIdCandidatePlan>) {
          visitor(payload.selected_term);
        } else if constexpr (std::is_same_v<Payload, RowIdRangeCandidatePlan> ||
                             std::is_same_v<Payload, IndexCandidatePlan> ||
                             std::is_same_v<Payload, AutomaticCandidatePlan>) {
          for (std::size_t term = 0; term < payload.selected_terms.size(); ++term) {
            if (payload.selected_terms[term]) {
              visitor(term);
            }
          }
        }
      },
      loop.payload);
}

[[nodiscard]] bool LoopUsesTerm(const ReadAccessCandidate& loop, std::size_t term) {
  bool used = false;
  VisitLoopTerms(loop, [&](std::size_t selected) { used = used || selected == term; });
  return used;
}

[[nodiscard]] std::size_t LoopTermCount(const ReadAccessCandidate& loop) noexcept {
  return loop.published.kind == PhysicalAccessKind::kRowIdLookup
             ? 1U
             : static_cast<std::size_t>(loop.published.equality_term_count) +
                   loop.published.range_bound_count;
}

[[nodiscard]] bool LoopIsCheaperProperSubset(const ReadAccessCandidate& left,
                                             const ReadAccessCandidate& right) {
  const auto& left_cost = left.published.cost;
  const auto& right_cost = right.published.cost;
  if (left_cost.run_log_est > right_cost.run_log_est &&
      left_cost.output_log_est > right_cost.output_log_est) {
    return false;
  }
  if (left.published.index == right.published.index &&
      left.published.equality_term_count < right.published.equality_term_count) {
    return true;
  }
  if (LoopTermCount(left) >= LoopTermCount(right) ||
      (left.published.covering && !right.published.covering)) {
    return false;
  }
  bool subset = true;
  VisitLoopTerms(left, [&](std::size_t term) { subset = subset && LoopUsesTerm(right, term); });
  return subset;
}

void AdjustLoopSubsetCost(std::span<const ReadAccessCandidate> loops,
                          ReadAccessCandidate& candidate) {
  if (candidate.published.kind != PhysicalAccessKind::kIndexScan) {
    return;
  }
  for (const ReadAccessCandidate& loop : loops) {
    if (loop.source_position != candidate.source_position ||
        loop.published.kind != PhysicalAccessKind::kIndexScan) {
      continue;
    }
    auto& cost = candidate.published.cost;
    if (LoopIsCheaperProperSubset(loop, candidate)) {
      cost.run_log_est = std::min(cost.run_log_est, loop.published.cost.run_log_est);
      cost.output_log_est = std::min(cost.output_log_est, loop.published.cost.output_log_est - 1);
    } else if (LoopIsCheaperProperSubset(candidate, loop)) {
      cost.run_log_est = std::max(cost.run_log_est, loop.published.cost.run_log_est);
      cost.output_log_est = std::max(cost.output_log_est, loop.published.cost.output_log_est + 1);
    }
  }
}

[[nodiscard]] std::optional<std::size_t> FindLesserLoop(std::span<const ReadAccessCandidate> loops,
                                                        const ReadAccessCandidate& candidate,
                                                        std::size_t start = 0U) {
  for (std::size_t index = start; index < loops.size(); ++index) {
    const ReadAccessCandidate& loop = loops[index];
    if (loop.source_position != candidate.source_position ||
        loop.sort_index != candidate.sort_index) {
      continue;
    }
    const auto& old = loop.published.cost;
    const auto& next = candidate.published.cost;
    const bool candidate_dependencies =
        (loop.prerequisites & candidate.prerequisites) == candidate.prerequisites;
    if (loop.published.kind == PhysicalAccessKind::kAutomaticIndex &&
        candidate.published.kind == PhysicalAccessKind::kIndexScan &&
        candidate.published.equality_term_count != 0U && candidate_dependencies) {
      return index;
    }
    if ((loop.prerequisites & candidate.prerequisites) == loop.prerequisites &&
        old.setup_log_est <= next.setup_log_est && old.run_log_est <= next.run_log_est &&
        old.output_log_est <= next.output_log_est) {
      return std::nullopt;
    }
    if (candidate_dependencies && old.run_log_est >= next.run_log_est &&
        old.output_log_est >= next.output_log_est) {
      return index;
    }
  }
  return loops.size();
}

struct WhereLoopPlan {
  std::vector<ReadAccessCandidate> loops{};
  std::array<std::size_t, kMaximumSelectSources> selected{};
  std::size_t level_count = 0;
  LogEst output_log_est = 0;
  bool search_limited = false;
  std::uint8_t ordered_prefix = 0;
  SourceMask reverse_mask = 0;
  SourceMask null_order_mask = 0;
  bool star_used = false;
  bool ordered_inner_loop = false;
};

[[nodiscard]] bool InsertWhereLoop(WhereLoopPlan& plan, ReadAccessCandidate& candidate,
                                   std::size_t& budget) {
  if (budget == 0U) {
    plan.search_limited = true;
    return false;
  }
  --budget;
  AdjustLoopSubsetCost(plan.loops, candidate);
  const auto replacement = FindLesserLoop(plan.loops, candidate);
  if (!replacement.has_value()) {
    return true;
  }
  if (*replacement == plan.loops.size()) {
    plan.loops.push_back(candidate);
    return true;
  }
  plan.loops[*replacement] = candidate;
  std::size_t tail = *replacement + 1U;
  while (tail < plan.loops.size()) {
    const auto superseded = FindLesserLoop(plan.loops, candidate, tail);
    if (!superseded.has_value() || *superseded == plan.loops.size()) {
      break;
    }
    tail = *superseded;
    plan.loops.erase(plan.loops.begin() + static_cast<std::ptrdiff_t>(tail));
  }
  return true;
}

[[nodiscard]] bool IsDeclaredJoinBarrier(const BoundTableSource& source) noexcept {
  return source.join == SourceJoinKind::kCross || SourceJoinPreservesLeft(source.join) ||
         SourceJoinPreservesRight(source.join);
}

[[nodiscard]] std::size_t ComputePathWidth(WhereLoopPlan& plan, const BoundSelectCore& core) {
  constexpr std::size_t kGeneralPaths = 12;
  constexpr std::size_t kStarPaths = 18;
  const std::size_t count = core.table_sources.size();
  if (count <= 1U) {
    return 1;
  }
  if (count == 2U) {
    return 5;
  }
  if (count < 4U) {
    return kGeneralPaths;
  }
  SourceMask self_joins = 0;
  std::size_t start = 0;
  for (std::size_t fact = 0; fact < count; ++fact) {
    const auto& fact_source = core.table_sources[fact];
    const SourceMask fact_mask = SourceBit(fact);
    if (IsDeclaredJoinBarrier(fact_source)) {
      if (fact + 3U > count) {
        break;
      }
      while (start < plan.loops.size() && plan.loops[start].source_position <= fact) {
        ++start;
      }
    }
    SourceMask dimensions = 0;
    std::size_t dimension_count = 0;
    for (std::size_t index = start; index < plan.loops.size(); ++index) {
      const auto& loop = plan.loops[index];
      const auto& source = core.table_sources[loop.source_position];
      if (IsDeclaredJoinBarrier(source)) {
        break;
      }
      const SourceMask mask = SourceBit(loop.source_position);
      if ((loop.prerequisites & fact_mask) != 0U && (mask & dimensions) == 0U &&
          (mask & self_joins) == 0U) {
        if (source.kind == fact_source.kind && source.table == fact_source.table) {
          self_joins |= fact_mask;
        } else {
          ++dimension_count;
          dimensions |= mask;
        }
      }
    }
    if (dimension_count <= 2U) {
      continue;
    }
    plan.star_used = true;
    LogEst maximum = std::numeric_limits<LogEst>::lowest();
    for (std::size_t index = start; index < plan.loops.size(); ++index) {
      const auto& loop = plan.loops[index];
      if (loop.source_position > fact) {
        break;
      }
      if (loop.source_position == fact) {
        maximum = std::max(maximum, loop.published.cost.run_log_est);
      }
    }
    if (maximum < std::numeric_limits<LogEst>::max()) {
      ++maximum;
    }
    for (std::size_t index = start; index < plan.loops.size(); ++index) {
      auto& loop = plan.loops[index];
      if ((SourceBit(loop.source_position) & dimensions) != 0U && LoopTermCount(loop) == 0U) {
        loop.published.cost.run_log_est = std::max(loop.published.cost.run_log_est, maximum);
      }
    }
  }
  return plan.star_used ? kStarPaths : kGeneralPaths;
}

[[nodiscard]] bool LoopIsNoBetter(const BoundQueryView& bound, const ReadAccessCandidate& candidate,
                                  const ReadAccessCandidate& baseline) noexcept {
  if (candidate.published.kind != PhysicalAccessKind::kIndexScan ||
      baseline.published.kind != PhysicalAccessKind::kIndexScan) {
    return true;
  }
  const auto width = [&](const ReadAccessCandidate& loop) {
    const auto& index = bound.catalog()->index(*loop.published.index);
    return ToLogEst(
        index.statistics.average_row_size.value_or(DerivedIndexRowSize(*bound.catalog(), index)));
  };
  return width(candidate) >= width(baseline);
}

struct WherePathOrder {
  std::optional<std::uint8_t> prefix{0};
  SourceMask reverse_mask = 0;
  SourceMask null_order_mask = 0;
};

enum class PathOrderScope : std::uint8_t {
  FullPath,
  InnermostLoop,
};

template <typename OrderEvaluator, typename SortCost>
[[nodiscard]] std::expected<void, OptimizerError> SolveWherePath(
    WhereLoopPlan& plan, const BoundQueryView& bound, const BoundSelectCore& core,
    LogEst outer_loops, std::span<const BoundOrderingTerm> requested_order, std::size_t width,
    OrderEvaluator& evaluate_order, SortCost& sort_cost) {
  struct Path {
    SourceMask visited = 0;
    LogEst rows = 0;
    LogEst cost = 0;
    LogEst unsorted = 0;
    WherePathOrder order{};
    std::span<std::size_t> loops{};
  };
  constexpr std::size_t kMaximumPaths = 18;
  const std::size_t count = core.table_sources.size();
  const std::size_t order_terms = requested_order.size();
  std::array<Path, 2U * kMaximumPaths> headers{};
  std::array<LogEst, kMaximumOrderedTerms> sorting_costs{};
  std::array<std::size_t, 2> single_source;
  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  std::unique_ptr<std::size_t[]> storage;
  std::span<std::size_t> sequences{single_source};
  if (count != 1U) {
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    storage = std::make_unique_for_overwrite<std::size_t[]>(2U * width * count);
    sequences = std::span{storage.get(), 2U * width * count};
  }
  for (std::size_t index = 0; index < 2U * width; ++index) {
    headers[index].loops = sequences.subspan(index * count, count);
  }
  std::span<Path> from{headers.data() + width, width};
  std::span<Path> to{headers.data(), width};
  from[0].rows = std::min(outer_loops, kMaximumOuterLoopEstimate);
  if (order_terms != 0U) {
    from[0].order.prefix.reset();
  }
  std::size_t from_count = 1;
  std::size_t worst = 0;
  LogEst maximum_cost = 0;
  LogEst maximum_unsorted = 0;
  for (std::size_t level = 0; level < count; ++level) {
    std::size_t to_count = 0;
    for (const Path& prefix : from.first(from_count)) {
      for (std::size_t choice = 0; choice < plan.loops.size(); ++choice) {
        const ReadAccessCandidate& loop = plan.loops[choice];
        const SourceMask self = SourceBit(loop.source_position);
        if ((loop.prerequisites & ~prefix.visited) != 0U || (self & prefix.visited) != 0U ||
            (loop.published.kind == PhysicalAccessKind::kAutomaticIndex && prefix.rows < 3)) {
          continue;
        }
        const auto& loop_cost = loop.published.cost;
        LogEst unsorted = loop_cost.run_log_est + prefix.rows;
        if (loop_cost.setup_log_est != 0) {
          unsorted = AddLogEst(loop_cost.setup_log_est, unsorted);
        }
        unsorted = AddLogEst(unsorted, prefix.unsorted);
        WherePathOrder order = prefix.order;
        if (!order.prefix.has_value()) {
          order = evaluate_order(prefix.loops.first(level), choice);
        }
        LogEst cost = unsorted;
        if (order.prefix.has_value() && *order.prefix < order_terms) {
          const std::size_t sorted_prefix = *order.prefix;
          if (sorting_costs[sorted_prefix] == 0) {
            sorting_costs[sorted_prefix] = sort_cost(sorted_prefix);
          }
          cost = AddLogEst(unsorted, sorting_costs[sorted_prefix]) + 3;
        } else {
          unsorted -= 2;
        }
        const LogEst rows = prefix.rows + loop_cost.output_log_est;
        const SourceMask visited = prefix.visited | self;
        std::size_t target = 0;
        while (target < to_count &&
               (to[target].visited != visited ||
                (to[target].order.prefix.has_value() != order.prefix.has_value() &&
                 level + 1U != count))) {
          ++target;
        }
        if (target == to_count) {
          if (to_count >= width &&
              (cost > maximum_cost || (cost == maximum_cost && unsorted >= maximum_unsorted))) {
            continue;
          }
          target = to_count < width ? to_count++ : worst;
        } else {
          const Path& previous = to[target];
          if (previous.cost < cost || (previous.cost == cost && previous.rows < rows) ||
              (previous.cost == cost && previous.rows == rows && previous.unsorted < unsorted) ||
              (previous.cost == cost && previous.rows == rows && previous.unsorted == unsorted &&
               LoopIsNoBetter(bound, loop, plan.loops[previous.loops[level]]))) {
            continue;
          }
        }
        Path& next = to[target];
        next.visited = visited;
        next.rows = rows;
        next.cost = cost;
        next.unsorted = unsorted;
        next.order = order;
        std::copy_n(prefix.loops.begin(), level, next.loops.begin());
        next.loops[level] = choice;
        if (to_count >= width) {
          worst = 0;
          maximum_cost = to[0].cost;
          maximum_unsorted = to[0].rows;
          for (std::size_t index = 1; index < width; ++index) {
            if (to[index].cost > maximum_cost ||
                (to[index].cost == maximum_cost && to[index].unsorted > maximum_unsorted)) {
              worst = index;
              maximum_cost = to[index].cost;
              maximum_unsorted = to[index].unsorted;
            }
          }
        }
      }
    }
    std::swap(from, to);
    from_count = to_count;
  }
  if (from_count != 1U) {
    return std::unexpected(InvariantFailure("optimizer found no query solution"));
  }
  plan.level_count = count;
  plan.output_log_est = from[0].rows;
  plan.ordered_prefix = from[0].order.prefix.value_or(0);
  plan.reverse_mask = from[0].order.reverse_mask;
  plan.null_order_mask = from[0].order.null_order_mask;
  std::copy_n(from[0].loops.begin(), count, plan.selected.begin());
  return {};
}

[[nodiscard]] bool IsNumericIndexAffinity(TypeAffinity affinity) noexcept {
  return affinity == TypeAffinity::kNumeric || affinity == TypeAffinity::kInteger ||
         affinity == TypeAffinity::kReal;
}

[[nodiscard]] bool IndexAffinityMatches(TypeAffinity comparison, TypeAffinity column) noexcept {
  if (comparison == TypeAffinity::kText) {
    return column == TypeAffinity::kText;
  }
  return !IsNumericIndexAffinity(comparison) || IsNumericIndexAffinity(column);
}

[[nodiscard]] TypeAffinity IndexProbeAffinity(const BoundQueryView& bound, BoundExpressionId key,
                                              TypeAffinity column) noexcept {
  const TypeAffinity operand = bound.expression(key).properties.affinity;
  if (operand != TypeAffinity::kNone && column != TypeAffinity::kNone) {
    return IsNumericIndexAffinity(operand) || IsNumericIndexAffinity(column)
               ? TypeAffinity::kNumeric
               : TypeAffinity::kBlob;
  }
  return operand == TypeAffinity::kNone ? column : operand;
}

[[nodiscard]] std::optional<LoopColumnReference> LoopColumnOf(const BoundQueryView& bound,
                                                              const BoundSelectCore& core,
                                                              BoundExpressionId expression) {
  const auto identity = LoopOperandIdentity(bound, expression);
  if (!identity.has_value()) {
    return std::nullopt;
  }
  const auto source = SourcePosition(core.table_sources, identity->source);
  if (!source.has_value()) {
    return std::nullopt;
  }
  std::optional<ColumnId> column;
  if (identity->column.has_value()) {
    const auto& declaration = core.table_sources[*source];
    const auto& bound_column = bound.source_columns()[identity->column->value()];
    column =
        declaration.kind == BoundSourceKind::kSchemaTable
            ? std::optional{ColumnId{identity->column->value() - declaration.source_column_begin}}
            : bound_column.catalog_column;
    if (!column.has_value()) {
      return std::nullopt;
    }
  }
  return LoopColumnReference{.source_position = *source, .column = column};
}

[[nodiscard]] bool TermIsEquivalence(const BoundQueryView& bound, const BoundSelectCore& core,
                                     const ReadPredicateTerm& term) {
  const BoundExpression& expression =
      bound.expression(TransparentPredicateId(bound, term.predicate));
  const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.payload);
  if (comparison == nullptr || term.outer_on || expression.properties.has_explicit_collation ||
      (comparison->comparison != SqlComparison::kEqual &&
       comparison->comparison != SqlComparison::kIs) ||
      (comparison->comparison == SqlComparison::kIs && !core.table_sources.empty() &&
       SourceIsLeftOfRightJoin(core, 0U))) {
    return false;
  }
  const auto left = TransparentRowIdOperandId(bound, comparison->left);
  const auto right = TransparentRowIdOperandId(bound, comparison->right);
  if (!BoundSourceLeafIdentityOf(bound.expression(left), bound.source_columns()).has_value() ||
      !BoundSourceLeafIdentityOf(bound.expression(right), bound.source_columns()).has_value()) {
    return false;
  }
  const auto& left_properties = bound.expression(left).properties;
  const auto& right_properties = bound.expression(right).properties;
  if (left_properties.affinity != right_properties.affinity &&
      (!IsNumericIndexAffinity(left_properties.affinity) ||
       !IsNumericIndexAffinity(right_properties.affinity))) {
    return false;
  }
  const auto collation = [&](const BoundExpressionProperties& properties) {
    return properties.collation.has_value()
               ? std::string_view{bound.collations()[properties.collation->value()].name}
               : std::string_view{"BINARY"};
  };
  return CatalogNamesEqual(collation(left_properties), collation(right_properties));
}

[[nodiscard]] bool IsSmallIntegerKey(const BoundQueryView& bound, BoundExpressionId key) {
  key = TransparentPredicateId(bound, key);
  if (const auto* unary = std::get_if<BoundUnaryExpression>(&bound.expression(key).payload);
      unary != nullptr && (unary->operation == BoundUnaryOperation::kPositive ||
                           unary->operation == BoundUnaryOperation::kNegative)) {
    key = TransparentPredicateId(bound, unary->operand);
  }
  const auto* literal = std::get_if<BoundLiteralExpression>(&bound.expression(key).payload);
  if (literal == nullptr || literal->value.type() != SqlValueType::kInteger) {
    return false;
  }
  const auto integer = literal->value.integer_value();
  if (!integer.has_value()) {
    return false;
  }
  const auto value = *integer;
  return value >= -1 && value <= 1;
}

[[nodiscard]] bool HasMinMaxOrder(const BoundQueryView& bound, const BoundSelectCore& core) {
  if (!core.aggregate.has_value()) {
    return false;
  }
  const auto& aggregate = *core.aggregate;
  if (!aggregate.group_by.empty()) {
    return true;
  }
  if (aggregate.having.has_value() || aggregate.invocations.size() != 1U) {
    return false;
  }
  const auto& invocation = bound.aggregate_invocations()[aggregate.invocations[0].value()];
  return !invocation.distinct && invocation.order_by.empty() && !invocation.filter.has_value() &&
         invocation.arguments.size() == 1U &&
         bound.aggregate_functions()[invocation.function.value()].extremum !=
             BoundAggregateExtremumKind::kNone;
}

[[nodiscard]] bool RequestsInputDistinct(const BoundQueryView& bound, const BoundSelectCore& core,
                                         std::size_t ordered_terms) noexcept {
  if (core.aggregate.has_value()) {
    const auto& invocations = core.aggregate->invocations;
    return invocations.size() == 1U &&
           bound.aggregate_invocations()[invocations[0].value()].distinct;
  }
  return core.quantifier == SelectQuantifier::kDistinct && ordered_terms <= kMaximumOrderedTerms &&
         BoundQueryRequiresResultValues(bound.query(), bound.expressions());
}

struct CoreOrderRequest {
  std::span<const BoundOrderingTerm> terms;
  // An inherited coroutine row bound is not a syntactic LIMIT for path costing.
  const BoundLimit* syntax_limit;
  bool block_producer;
};

class WhereLoopBuilder final {
 public:
  WhereLoopBuilder(const BoundQueryView& bound, std::size_t core_index,
                   std::span<ReadPredicateTerm> terms, OptimizerOptions options,
                   LogEst outer_loops = 0, std::optional<CoreOrderRequest> order = std::nullopt)
      : bound_(bound),
        core_(std::get<BoundSelectCore>(bound.query_cores()[core_index])),
        core_index_(core_index),
        terms_(terms),
        options_(options),
        outer_loops_(outer_loops),
        order_request_(order.value_or(CoreOrderRequest{
            .terms = bound.order_by(),
            .syntax_limit = bound.limit(),
            .block_producer = bound.query_cores().size() == 1U &&
                              core_.quantifier != SelectQuantifier::kDistinct &&
                              !core_.aggregate.has_value(),
        })),
        order_terms_(!core_.aggregate.has_value() &&
                             order_request_.terms.size() <= kMaximumOrderedTerms
                         ? order_request_.terms
                         : std::span<const BoundOrderingTerm>{}),
        order_candidates_(
            HasMinMaxOrder(bound, core_) ||
            (!order_request_.terms.empty() && order_request_.terms.size() <= kMaximumOrderedTerms)),
        adjust_output_(order_candidates_) {}

  [[nodiscard]] std::expected<WhereLoopPlan, OptimizerError> Run() {
    if (core_.table_sources.empty() || core_.table_sources.size() > kMaximumSelectSources) {
      return std::unexpected(InvariantFailure("loop builder has an invalid source count"));
    }
    if (auto prepared = PrepareLoopTerms(bound_, core_, terms_); !prepared.has_value()) {
      return std::unexpected(std::move(prepared.error()));
    }
    const bool shortcut = core_.table_sources.size() == 1U && TryShortcut();
    if (error_.has_value()) {
      return std::unexpected(std::move(*error_));
    }
    if (shortcut) {
      plan_.level_count = 1U;
      plan_.selected[0] = 0U;
      plan_.output_log_est = kShortcutOutput;
      plan_.ordered_prefix = static_cast<std::uint8_t>(order_terms_.size());
      return std::move(plan_);
    }
    SourceMask prerequisites = 0;
    SourceMask prior = 0;
    bool first_past_right = false;
    bool right_or_cross = false;
    for (std::size_t position = 0; position < core_.table_sources.size(); ++position) {
      budget_ += kPlannerSearchLimitIncrement;
      const auto& declaration = core_.table_sources[position];
      const bool left_of_right = SourceIsLeftOfRightJoin(core_, position);
      if (first_past_right || left_of_right || IsDeclaredJoinBarrier(declaration)) {
        right_or_cross =
            right_or_cross || left_of_right || declaration.join == SourceJoinKind::kCross;
        prerequisites |= prior;
        first_past_right = SourceJoinPreservesRight(declaration.join);
      } else if (!right_or_cross) {
        prerequisites = 0;
      }
      static_cast<void>(AddSource(position, prerequisites));
      if (error_.has_value()) {
        return std::unexpected(std::move(*error_));
      }
      prior |= SourceBit(position);
    }
    const std::size_t width = ComputePathWidth(plan_, core_);
    auto evaluate_order = [&](std::span<const std::size_t> prefix, std::size_t last) {
      return PathSatisfiesOrder(prefix, last, PathOrderScope::FullPath);
    };
    LogEst row_estimate = 0;
    auto sorting_cost = [&](std::size_t prefix) {
      return SortingCost(order_terms_.subspan(order_request_.block_producer ? prefix : 0U),
                         row_estimate);
    };
    if (auto solved = SolveWherePath(plan_, bound_, core_, outer_loops_, {}, width, evaluate_order,
                                     sorting_cost);
        !solved.has_value()) {
      return std::unexpected(std::move(solved.error()));
    }
    if (!order_terms_.empty()) {
      ProtectSelectedSearches();
      row_estimate = plan_.output_log_est < 0 ? 1 : plan_.output_log_est + 1;
      if (auto solved = SolveWherePath(plan_, bound_, core_, outer_loops_, order_terms_, width,
                                       evaluate_order, sorting_cost);
          !solved.has_value()) {
        return std::unexpected(std::move(solved.error()));
      }
      if (plan_.ordered_prefix == 0U && plan_.level_count != 0U) {
        const std::size_t last = plan_.selected[plan_.level_count - 1U];
        if (!IsOneRowLoop(plan_.loops[last])) {
          const auto inner_order = PathSatisfiesOrder(
              std::span<const std::size_t>{plan_.selected}.first(plan_.level_count - 1U), last,
              PathOrderScope::InnermostLoop);
          if (inner_order.prefix.has_value() && *inner_order.prefix == order_terms_.size()) {
            plan_.ordered_inner_loop = true;
            plan_.reverse_mask = inner_order.reverse_mask;
            plan_.null_order_mask = inner_order.null_order_mask;
          }
        }
      }
      if (std::ranges::any_of(core_.table_sources, [](const BoundTableSource& source) {
            return SourceJoinPreservesRight(source.join);
          })) {
        plan_.ordered_prefix = 0;
      }
    }
    SourceMask ready = 0;
    for (std::size_t level = 0; level < plan_.level_count; ++level) {
      auto& loop = plan_.loops[plan_.selected[level]];
      loop.direction = (plan_.reverse_mask & SourceBit(level)) != 0U
                           ? PhysicalScanDirection::kReverse
                           : PhysicalScanDirection::kForward;
      if (auto* index = std::get_if<IndexCandidatePlan>(&loop.payload); index != nullptr) {
        index->node.two_pass_null_order =
            (plan_.null_order_mask & SourceBit(level)) != 0U && !index->node.range.has_value();
      }
      if (loop.published.kind == PhysicalAccessKind::kAutomaticIndex) {
        if (auto finalized = CompleteAutomatic(loop, ready); !finalized.has_value()) {
          return std::unexpected(std::move(finalized.error()));
        }
      }
      ready |= SourceBit(loop.source_position);
    }
    if (RequestsInputDistinct(bound_, core_, order_request_.terms.size())) {
      plan_.output_log_est -= kDistinctOutputReduction;
    }
    return std::move(plan_);
  }

 private:
  struct Match {
    IndexConstraintMatch constraint{};
    std::size_t term = 0;
    SourceMask prerequisites = 0;
    bool transitive = false;
  };

  template <typename Visitor>
  bool VisitIndexes(TableId table, Visitor&& visitor) {
    const auto indexes = bound_.catalog()->table_indexes(table);
    for (const bool replacements : {false, true}) {
      for (const IndexId id : std::views::reverse(indexes)) {
        const auto& index = bound_.catalog()->index(id);
        if ((index.conflict_action == ConflictAction::kReplace) == replacements && !visitor(id)) {
          return false;
        }
      }
    }
    return true;
  }

  [[nodiscard]] LoopColumnReference IndexColumn(std::size_t position,
                                                const CatalogIndexTerm& term) const {
    const auto* column = std::get_if<ColumnId>(&term.target);
    const auto& source = core_.table_sources[position];
    const auto& table = bound_.catalog()->table(AssumePresent(source.table));
    return LoopColumnReference{
        .source_position = position,
        .column = column == nullptr || table.rowid_alias == *column ? std::nullopt
                                                                    : std::optional{*column},
    };
  }

  template <typename Visitor>
  bool VisitComparisons(Visitor&& visitor) const {
    for (std::size_t index = 0; index < terms_.size(); ++index) {
      const auto& term = terms_[index];
      if (term.comparison != nullptr && (term.left_is_column || term.right_is_column) &&
          !visitor(index, !term.left_is_column)) {
        return false;
      }
    }
    for (std::size_t index = terms_.size(); index > 0U;) {
      const auto& term = terms_[--index];
      if (term.left_is_column && term.right_is_column && !visitor(index, true)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::optional<Match> ComparisonMatch(std::size_t index, bool commuted) const {
    const auto& term = terms_[index];
    const auto& comparison = *term.comparison;
    Match match{
        .constraint =
            IndexConstraintMatch{
                .key = commuted ? comparison.left : comparison.right,
                .predicate = term.predicate,
                .conjunct_position = term.conjunct_position,
            },
        .term = index,
        .prerequisites =
            (commuted ? term.left_prerequisites : term.right_prerequisites) | term.key_floor,
    };
    const auto operation =
        commuted ? ReverseComparison(comparison.comparison) : comparison.comparison;
    switch (operation) {
      case SqlComparison::kEqual:
        break;
      case SqlComparison::kIs:
        match.constraint.reject_null = false;
        break;
      case SqlComparison::kGreater:
      case SqlComparison::kGreaterEqual:
        match.constraint.kind = IndexConstraintKind::kLower;
        match.constraint.inclusive = operation == SqlComparison::kGreaterEqual;
        break;
      case SqlComparison::kLess:
      case SqlComparison::kLessEqual:
        match.constraint.kind = IndexConstraintKind::kUpper;
        match.constraint.inclusive = operation == SqlComparison::kLessEqual;
        break;
      case SqlComparison::kNotEqual:
      case SqlComparison::kIsNot:
        return std::nullopt;
    }
    return match;
  }

  template <typename Visitor>
  bool ScanConstraints(LoopColumnReference target, const CatalogIndexTerm* index_term,
                       Visitor&& visitor, SourceMask available) {
    std::array<LoopColumnReference, kMaximumEquivalentColumns> equivalent{};
    equivalent[0] = target;
    std::size_t equivalent_count = 1U;
    for (std::size_t current = 0; current < equivalent_count; ++current) {
      const bool proceed = VisitComparisons([&](std::size_t term_index, bool commuted) {
        const auto& term = terms_[term_index];
        const auto column = commuted ? term.right_column : term.left_column;
        if (column != equivalent[current] || (current != 0U && term.outer_on)) {
          return true;
        }
        const auto other = commuted ? term.left_column : term.right_column;
        if (other.has_value() && equivalent_count < equivalent.size() && term.equivalence &&
            std::ranges::find(std::span{equivalent}.first(equivalent_count), *other) ==
                std::span{equivalent}.first(equivalent_count).end()) {
          equivalent[equivalent_count++] = *other;
        }
        auto match = ComparisonMatch(term_index, commuted);
        if (!match.has_value() || (match->prerequisites & ~available) != 0U ||
            (match->prerequisites & SourceBit(equivalent[current].source_position)) != 0U ||
            (match->prerequisites & SourceBit(target.source_position)) != 0U ||
            !ConstraintCompatibleWithSource(core_, target.source_position, term)) {
          return true;
        }
        const auto& comparison = *term.comparison;
        const bool null_test =
            !match->constraint.reject_null && IsNullLiteral(bound_, match->constraint.key);
        if (index_term != nullptr && !null_test) {
          const auto& table =
              bound_.catalog()->table(*core_.table_sources[target.source_position].table);
          const auto* index_column = std::get_if<ColumnId>(&index_term->target);
          const TypeAffinity affinity = index_column != nullptr
                                            ? table.columns[index_column->value].affinity
                                            : TypeAffinity::kInteger;
          if (!IndexAffinityMatches(comparison.affinity, affinity) ||
              !CatalogNamesEqual(bound_.collations()[comparison.collation.value()].name,
                                 index_term->collation_name)) {
            return true;
          }
        }
        match->transitive = current != 0U;
        return visitor(*match);
      });
      if (!proceed) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] PhysicalIndexEquality Equality(const CatalogIndexTerm& column,
                                               const SourceInfo& source, const Match& match) const {
    const auto id = std::get<ColumnId>(column.target);
    return PhysicalIndexEquality{
        .column = id,
        .key = match.constraint.key,
        .predicate = match.constraint.predicate,
        .conjunct_position = match.constraint.conjunct_position,
        .origin = terms_[match.term].origin,
        .affinity = bound_.catalog()->table(AssumePresent(source.table)).columns[id.value].affinity,
        .collation_name = column.collation_name,
        .order = column.order,
        .reject_null = match.constraint.reject_null,
        .probe_affinity = IndexProbeAffinity(
            bound_, match.constraint.key,
            bound_.catalog()->table(AssumePresent(source.table)).columns[id.value].affinity),
    };
  }

  [[nodiscard]] PhysicalRangeBound RangeBound(
      const Match& match, TypeAffinity affinity = TypeAffinity::kNumeric) const {
    return PhysicalRangeBound{
        .key = match.constraint.key,
        .predicate = match.constraint.predicate,
        .conjunct_position = match.constraint.conjunct_position,
        .origin = terms_[match.term].origin,
        .inclusive = match.constraint.inclusive,
        .probe_affinity = IndexProbeAffinity(bound_, match.constraint.key, affinity),
    };
  }

  [[nodiscard]] IndexCandidatePlan IndexTemplate(std::size_t position, IndexId id) const {
    const auto& declaration = core_.table_sources[position];
    const auto& table = bound_.catalog()->table(AssumePresent(declaration.table));
    const auto& index = bound_.catalog()->index(id);
    return IndexCandidatePlan{
        .node =
            PhysicalIndexScanNode{
                .core_index = core_index_,
                .source = declaration.id,
                .table = AssumePresent(declaration.table),
                .table_root_page = table.root_page,
                .index = id,
                .index_root_page = index.root_page,
            },
        .selected_terms = std::vector<bool>(terms_.size(), false),
    };
  }

  void SetCovering(IndexCandidatePlan& candidate, const SourceInfo& source) const {
    candidate.node.covering = IsCoveringIndex(
        bound_, source.id, bound_.catalog()->table(AssumePresent(source.table)),
        bound_.catalog()->index(candidate.node.index), terms_, candidate.selected_terms, &core_);
  }

  [[nodiscard]] bool TryShortcut() {
    const auto& declaration = core_.table_sources[0];
    const SourceInfo source = ResolveTableSource(bound_, declaration);
    if (!source.rowid_eligible) {
      return false;
    }
    ScanConstraints(
        LoopColumnReference{}, nullptr,
        [&](const Match& match) {
          if (match.constraint.kind != IndexConstraintKind::kEquality ||
              (!match.constraint.reject_null && IsNullLiteral(bound_, match.constraint.key))) {
            return true;
          }
          plan_.loops.push_back(ReadAccessCandidate{
              .published = AccessPathCandidate{.kind = PhysicalAccessKind::kRowIdLookup,
                                               .cost = RowIdCost(source.estimated_rows, true)},
              .payload = RowIdCandidatePlan{.key = match.constraint.key,
                                            .selected_term = match.term,
                                            .constraint = terms_[match.term]},
              .single_row_shortcut = true,
              .transitive = match.transitive,
          });
          return false;
        },
        0);
    if (!plan_.loops.empty() || !source.table.has_value()) {
      return !plan_.loops.empty();
    }
    VisitIndexes(*source.table, [&](IndexId id) {
      const auto& index = bound_.catalog()->index(id);
      const auto& table = bound_.catalog()->table(*source.table);
      if (!index.unique || index.key_term_count > kShortcutIndexTermLimit ||
          !IsSupportedIndexShape(bound_, table, index)) {
        return true;
      }
      IndexCandidatePlan candidate = IndexTemplate(0U, id);
      bool transitive = false;
      for (std::size_t column = 0; column < index.key_term_count; ++column) {
        bool found = false;
        ScanConstraints(
            IndexColumn(0U, index.terms[column]), &index.terms[column],
            [&](const Match& match) {
              if (match.constraint.kind != IndexConstraintKind::kEquality ||
                  (!match.constraint.reject_null &&
                   (!index.unique_not_null || IsNullLiteral(bound_, match.constraint.key)))) {
                return true;
              }
              candidate.node.equalities.push_back(Equality(index.terms[column], source, match));
              candidate.selected_terms[match.term] = true;
              transitive = transitive || match.transitive;
              found = true;
              return false;
            },
            0);
        if (!found) {
          return true;
        }
      }
      SetCovering(candidate, source);
      plan_.loops.push_back(ReadAccessCandidate{
          .published = PublishIndexCandidate(bound_, source, candidate, true),
          .payload = std::move(candidate),
          .single_row_shortcut = true,
          .transitive = transitive,
      });
      return false;
    });
    return !plan_.loops.empty();
  }

  void AdjustOutput(ReadAccessCandidate& loop, LogEst table_rows) const {
    if (core_.table_sources.size() <= 1U && !adjust_output_) {
      return;
    }
    LogEst reduction = 0;
    const SourceMask self = SourceBit(loop.source_position);
    const SourceMask allowed = loop.prerequisites | self;
    for (std::size_t term = 0; term < terms_.size(); ++term) {
      const auto& predicate = terms_[term];
      if ((predicate.prerequisites & ~allowed) != 0U || (predicate.prerequisites & self) == 0U ||
          LoopUsesTerm(loop, term)) {
        continue;
      }
      auto& output = loop.published.cost.output_log_est;
      if (predicate.truth_probability <= 0) {
        output += predicate.truth_probability;
      } else {
        --output;
        const auto* comparison = std::get_if<BoundComparisonExpression>(
            &bound_.expression(TransparentPredicateId(bound_, predicate.predicate)).payload);
        if (comparison != nullptr && (comparison->comparison == SqlComparison::kEqual ||
                                      comparison->comparison == SqlComparison::kIs)) {
          reduction =
              std::max<LogEst>(reduction, IsSmallIntegerKey(bound_, comparison->right) ? 10 : 20);
        }
      }
    }
    loop.published.cost.output_log_est =
        std::min(loop.published.cost.output_log_est, table_rows - reduction);
  }

  [[nodiscard]] LogEst PrefixEstimate(const CatalogIndex& index, const SourceInfo& source,
                                      std::size_t prefix) const {
    if (prefix == 0U) {
      return ToLogEst(IndexCardinality(index, source.estimated_rows));
    }
    if (index.statistics.has_stat1 && prefix < index.statistics.rows_per_prefix.size()) {
      return ToLogEst(index.statistics.rows_per_prefix[prefix]);
    }
    return index.unique && prefix == index.key_term_count ? 0 : DefaultPrefixLogEst(prefix);
  }

  [[nodiscard]] LogEst RangeEstimate(LogEst input, const std::optional<PhysicalRangeBound>& lower,
                                     const std::optional<PhysicalRangeBound>& upper) const {
    LogEst estimate = input;
    const auto probability = [&](const PhysicalRangeBound& bound) {
      return PredicateTruthProbability(bound_, bound.predicate);
    };
    if (lower.has_value()) {
      const LogEst truth = probability(*lower);
      estimate += truth <= 0 ? truth : -kRangeReduction;
    }
    if (upper.has_value()) {
      const LogEst truth = probability(*upper);
      estimate += truth <= 0 ? truth : -kRangeReduction;
    }
    if (lower.has_value() && upper.has_value() && probability(*lower) > 0 &&
        probability(*upper) > 0) {
      estimate -= kRangeReduction;
    }
    return std::min(
        input - static_cast<LogEst>(lower.has_value()) - static_cast<LogEst>(upper.has_value()),
        std::max(estimate, kMinimumRangeOutput));
  }

  [[nodiscard]] BoundExpressionId OrderExpression(const BoundOrderingTerm& term) const {
    return term.result_column.has_value() && *term.result_column < core_.result_columns.size()
               ? core_.result_columns[*term.result_column].expression
               : term.expression;
  }

  [[nodiscard]] bool OrderTermFixed(const BoundOrderingTerm& order, const ReadAccessCandidate& loop,
                                    SourceMask ready) {
    const BoundExpressionId expression = OrderExpression(order);
    const auto column = LoopColumnOf(bound_, core_, expression);
    if (!column.has_value() || column->source_position != loop.source_position) {
      return false;
    }
    bool fixed = false;
    ScanConstraints(
        *column, nullptr,
        [&](const Match& match) {
          if (match.constraint.kind != IndexConstraintKind::kEquality) {
            return true;
          }
          const auto& comparison = std::get<BoundComparisonExpression>(
              bound_.expression(TransparentPredicateId(bound_, terms_[match.term].predicate))
                  .payload);
          const TypeAffinity operand = bound_.expression(match.constraint.key).properties.affinity;
          const TypeAffinity indexed = bound_.expression(expression).properties.affinity;
          const bool null_test =
              !match.constraint.reject_null && IsNullLiteral(bound_, match.constraint.key);
          if (column->column.has_value() && !null_test &&
              (!CatalogNamesEqual(bound_.collations()[comparison.collation.value()].name,
                                  bound_.collations()[order.collation.value()].name) ||
               (operand != TypeAffinity::kNone && operand != indexed))) {
            return true;
          }
          fixed = true;
          return false;
        },
        ready);
    return fixed;
  }

  [[nodiscard]] bool IsOneRowLoop(const ReadAccessCandidate& loop) const {
    const auto* index = std::get_if<IndexCandidatePlan>(&loop.payload);
    return loop.published.kind == PhysicalAccessKind::kRowIdLookup ||
           (index != nullptr && IsOneRowIndexLookup(bound_, index->node));
  }

  [[nodiscard]] WherePathOrder PathSatisfiesOrder(std::span<const std::size_t> prefix,
                                                  std::size_t last, PathOrderScope scope) {
    WherePathOrder result{.prefix = std::nullopt};
    SourceMask satisfied = 0;
    const SourceMask complete = (SourceMask{1} << order_terms_.size()) - 1U;
    SourceMask distinct_sources = 0;
    SourceMask ready = 0;
    bool order_distinct = true;
    for (std::size_t depth = 0; order_distinct && satisfied != complete && depth <= prefix.size();
         ++depth) {
      const auto& loop = plan_.loops[depth == prefix.size() ? last : prefix[depth]];
      if (scope == PathOrderScope::InnermostLoop && depth < prefix.size()) {
        ready |= SourceBit(loop.source_position);
        continue;
      }
      for (std::size_t term = 0; term < order_terms_.size(); ++term) {
        const SourceMask bit = SourceMask{1} << term;
        if ((satisfied & bit) == 0U && OrderTermFixed(order_terms_[term], loop, ready)) {
          satisfied |= bit;
        }
      }
      const auto* index_plan = std::get_if<IndexCandidatePlan>(&loop.payload);
      if (!IsOneRowLoop(loop)) {
        const SourceInfo source =
            ResolveTableSource(bound_, core_.table_sources[loop.source_position]);
        const CatalogIndex* index =
            index_plan != nullptr ? &bound_.catalog()->index(index_plan->node.index) : nullptr;
        if (loop.published.kind == PhysicalAccessKind::kAutomaticIndex ||
            (index == nullptr && !source.rowid_eligible) ||
            (index != nullptr && index->statistics.unordered)) {
          result.prefix = 0;
          return result;
        }
        const std::size_t columns = index != nullptr ? index->terms.size() : 1U;
        const std::size_t equalities =
            index_plan != nullptr ? index_plan->node.equalities.size() : 0U;
        if (index != nullptr) {
          order_distinct = index->unique;
        }
        std::optional<bool> reverse;
        bool matched_rowid = false;
        for (std::size_t position = 0; position < columns; ++position) {
          if (position < equalities) {
            if (!index_plan->node.equalities[position].reject_null) {
              order_distinct = false;
            }
            continue;
          }
          const LoopColumnReference available =
              index != nullptr ? IndexColumn(loop.source_position, index->terms[position])
                               : LoopColumnReference{.source_position = loop.source_position};
          const CatalogColumn* column =
              available.column.has_value()
                  ? &bound_.catalog()->table(*source.table).columns[available.column->value]
                  : nullptr;
          if (column != nullptr && !column->effective_not_null_conflict.has_value()) {
            order_distinct = false;
          }
          std::size_t requested = 0;
          while (requested < order_terms_.size() &&
                 (satisfied & (SourceMask{1} << requested)) != 0U) {
            ++requested;
          }
          bool match = requested < order_terms_.size();
          if (match) {
            const auto& order = order_terms_[requested];
            const auto needed = LoopColumnOf(bound_, core_, OrderExpression(order));
            match = needed.has_value() && *needed == available;
            if (match && column != nullptr) {
              match = CatalogNamesEqual(bound_.collations()[order.collation.value()].name,
                                        index->terms[position].collation_name);
            }
            if (match) {
              const bool backwards =
                  (index != nullptr && index->terms[position].order == SortOrder::kDescending) !=
                  (order.order == SortOrder::kDescending);
              if (reverse.has_value()) {
                match = *reverse == backwards;
              } else {
                reverse = backwards;
                if (backwards) {
                  result.reverse_mask |= SourceBit(depth);
                }
              }
            }
            if (match && index != nullptr) {
              const auto natural = order.order == SortOrder::kDescending
                                       ? BoundNullPlacement::kLast
                                       : BoundNullPlacement::kFirst;
              if (order.null_placement != natural) {
                match = position == equalities;
                if (match) {
                  result.null_order_mask |= SourceBit(depth);
                }
              }
            }
          }
          if (!match) {
            if (position == 0U || (index != nullptr && position < index->key_term_count)) {
              order_distinct = false;
            }
            break;
          }
          matched_rowid = matched_rowid || !available.column.has_value();
          satisfied |= SourceMask{1} << requested;
        }
        order_distinct = order_distinct || matched_rowid;
      }
      if (order_distinct) {
        distinct_sources |= SourceBit(loop.source_position);
        for (std::size_t term = 0; term < order_terms_.size(); ++term) {
          const SourceMask bit = SourceMask{1} << term;
          if ((satisfied & bit) != 0U) {
            continue;
          }
          const auto expression = OrderExpression(order_terms_[term]);
          const auto dependencies = ExpressionSourceMask(bound_, core_.table_sources, expression);
          if (dependencies.has_value() && (*dependencies & ~distinct_sources) == 0U &&
              (*dependencies != 0U || IsOrderingConstant(bound_, expression))) {
            satisfied |= bit;
          }
        }
      }
      ready |= SourceBit(loop.source_position);
    }
    if (satisfied == complete) {
      result.prefix = static_cast<std::uint8_t>(order_terms_.size());
    } else if (!order_distinct) {
      std::uint8_t count = 0;
      while (count < order_terms_.size() && (satisfied & (SourceMask{1} << count)) != 0U) {
        ++count;
      }
      result.prefix = count;
    }
    return result;
  }

  [[nodiscard]] std::optional<LogEst> FixedLimitEstimate() const {
    const BoundLimit* limit = order_request_.syntax_limit;
    if (limit == nullptr) {
      return std::nullopt;
    }
    const auto count = BoundExpressionIntegerLiteralValue(bound_.expressions(), limit->limit);
    if (!count.has_value() || *count <= 0) {
      return std::nullopt;
    }
    const LogEst estimate = ToLogEst(static_cast<std::uint64_t>(*count));
    return estimate < kDefaultSelectRows ? std::optional{estimate} : std::nullopt;
  }

  [[nodiscard]] LogEst SortingCost(std::span<const BoundOrderingTerm> unsorted_order,
                                   LogEst rows) const {
    constexpr std::size_t kColumnWidthPadding = 59;
    constexpr std::size_t kColumnsPerWidthUnit = 30;
    LogEst cost =
        rows + ToLogEst((core_.result_columns.size() + kColumnWidthPadding) / kColumnsPerWidthUnit);
    const std::size_t prefix = order_terms_.size() - unsorted_order.size();
    if (prefix != 0U) {
      constexpr std::size_t kPercent = 100;
      cost += ToLogEst((order_terms_.size() - prefix) * kPercent / order_terms_.size()) -
              kPartialSortPercentScale;
    }
    if (const auto limit = FixedLimitEstimate(); limit.has_value()) {
      cost += kLogEstScale;
      if (prefix != 0U) {
        constexpr LogEst kPartialLimitSortBias = 6;
        cost += kPartialLimitSortBias;
      }
      rows = std::min(rows, *limit);
    } else if (RequestsInputDistinct(bound_, core_, order_request_.terms.size()) &&
               rows > kLogEstScale) {
      rows -= kLogEstScale;
    }
    return cost + LogEstLogarithm(rows);
  }

  void ProtectSelectedSearches() {
    for (std::size_t level = 0; level < plan_.level_count; ++level) {
      const auto& selected = plan_.loops[plan_.selected[level]];
      if (selected.published.kind != PhysicalAccessKind::kRowIdLookup &&
          !(selected.published.kind == PhysicalAccessKind::kIndexScan &&
            selected.published.equality_term_count != 0U)) {
        break;
      }
      for (auto& loop : plan_.loops) {
        if (loop.source_position == selected.source_position &&
            loop.published.kind != PhysicalAccessKind::kAutomaticIndex &&
            LoopTermCount(loop) == 0U) {
          loop.prerequisites = std::numeric_limits<SourceMask>::max();
        }
      }
    }
  }

  bool AddIndexTerms(ReadAccessCandidate& loop, const SourceInfo& source, LogEst input) {
    auto& candidate = std::get<IndexCandidatePlan>(loop.payload);
    const auto& index = bound_.catalog()->index(candidate.node.index);
    const std::size_t column = candidate.node.equalities.size();
    if (column >= index.key_term_count ||
        (candidate.node.range.has_value() && candidate.node.range->upper.has_value())) {
      return true;
    }
    return ScanConstraints(
        IndexColumn(loop.source_position, index.terms[column]), &index.terms[column],
        [&](const Match& match) {
          const bool equality = match.constraint.kind == IndexConstraintKind::kEquality;
          if ((candidate.node.range.has_value() &&
               (equality || match.constraint.kind != IndexConstraintKind::kUpper)) ||
              (!equality && index.statistics.unordered)) {
            return true;
          }
          const auto column_id = std::get<ColumnId>(index.terms[column].target);
          if (equality && !match.constraint.reject_null &&
              IsNullLiteral(bound_, match.constraint.key) &&
              bound_.catalog()
                  ->table(*source.table)
                  .columns[column_id.value]
                  .effective_not_null_conflict.has_value()) {
            return true;
          }
          const SourceMask previous_prerequisites = loop.prerequisites;
          const bool previous_transitive = loop.transitive;
          const bool previously_selected = candidate.selected_terms[match.term];
          loop.prerequisites |= match.prerequisites;
          candidate.selected_terms[match.term] = true;
          LogEst output = input;
          if (equality) {
            candidate.node.equalities.push_back(Equality(index.terms[column], source, match));
            loop.transitive = loop.transitive || match.transitive;
            const LogEst truth = terms_[match.term].truth_probability;
            output += truth <= 0 ? truth
                                 : PrefixEstimate(index, source, column + 1U) -
                                       PrefixEstimate(index, source, column);
            if (!match.constraint.reject_null && IsNullLiteral(bound_, match.constraint.key) &&
                truth > 0) {
              output += kLogEstScale;
            }
          } else {
            if (!candidate.node.range.has_value()) {
              candidate.node.range = PhysicalIndexRange{
                  .column = column_id,
                  .affinity =
                      bound_.catalog()->table(*source.table).columns[column_id.value].affinity,
                  .collation_name = index.terms[column].collation_name,
                  .order = index.terms[column].order,
              };
            }
            (match.constraint.kind == IndexConstraintKind::kLower ? candidate.node.range->lower
                                                                  : candidate.node.range->upper) =
                RangeBound(match, candidate.node.range->affinity);
            output = RangeEstimate(input, candidate.node.range->lower, candidate.node.range->upper);
          }
          loop.published = PublishIndexCandidate(bound_, source, candidate, false, output);
          AdjustOutput(loop, ToLogEst(source.estimated_rows));
          bool proceed = InsertWhereLoop(plan_, loop, budget_);
          if (proceed &&
              (!candidate.node.range.has_value() || !candidate.node.range->upper.has_value())) {
            proceed = AddIndexTerms(loop, source, equality ? output : input);
          }
          if (equality) {
            candidate.node.equalities.pop_back();
          } else if (match.constraint.kind == IndexConstraintKind::kUpper &&
                     candidate.node.range->lower.has_value()) {
            candidate.node.range->upper.reset();
          } else {
            candidate.node.range.reset();
          }
          candidate.selected_terms[match.term] = previously_selected;
          loop.prerequisites = previous_prerequisites;
          loop.transitive = previous_transitive;
          return proceed;
        },
        AllSources(core_.table_sources.size()) & ~SourceBit(loop.source_position));
  }

  bool AddRowIdTerms(std::size_t position, const SourceInfo& source, SourceMask prerequisites) {
    const SourceMask available = AllSources(core_.table_sources.size()) & ~SourceBit(position);
    return ScanConstraints(
        LoopColumnReference{.source_position = position}, nullptr,
        [&](const Match& match) {
          if (match.constraint.kind != IndexConstraintKind::kEquality) {
            if (!AddRowIdRange(position, source, prerequisites, match, std::nullopt)) {
              return false;
            }
            if (match.constraint.kind != IndexConstraintKind::kLower) {
              return true;
            }
            return ScanConstraints(
                LoopColumnReference{.source_position = position}, nullptr,
                [&](const Match& upper) {
                  return upper.constraint.kind != IndexConstraintKind::kUpper ||
                         AddRowIdRange(position, source, prerequisites, match, upper);
                },
                available);
          }
          if (!match.constraint.reject_null && IsNullLiteral(bound_, match.constraint.key)) {
            return true;
          }
          ReadAccessCandidate loop{
              .published = AccessPathCandidate{.kind = PhysicalAccessKind::kRowIdLookup,
                                               .cost = RowIdCost(source.estimated_rows)},
              .payload = RowIdCandidatePlan{.key = match.constraint.key,
                                            .selected_term = match.term,
                                            .constraint = terms_[match.term]},
              .source_position = position,
              .prerequisites = prerequisites | match.prerequisites,
              .sort_index = order_candidates_ ? 1U : 0U,
              .transitive = match.transitive,
          };
          AdjustOutput(loop, ToLogEst(source.estimated_rows));
          return InsertWhereLoop(plan_, loop, budget_);
        },
        available);
  }

  bool AddRowIdRange(std::size_t position, const SourceInfo& source, SourceMask prerequisites,
                     const Match& first, const std::optional<Match>& second) {
    RowIdRangeCandidatePlan range{
        .node =
            PhysicalRowIdRangeNode{
                .core_index = core_index_,
                .source = source.id,
                .source_kind = source.source_kind,
                .table = source.table,
                .root_page = source.root_page,
            },
        .selected_terms = std::vector<bool>(terms_.size(), false),
    };
    const auto add = [&](const Match& match) {
      (match.constraint.kind == IndexConstraintKind::kLower ? range.node.lower : range.node.upper) =
          RangeBound(match);
      range.selected_terms[match.term] = true;
    };
    add(first);
    if (second.has_value()) {
      add(*second);
    }
    const LogEst input = ToLogEst(source.estimated_rows);
    const LogEst output = RangeEstimate(input, range.node.lower, range.node.upper);
    ReadAccessCandidate loop{
        .published =
            AccessPathCandidate{
                .kind = PhysicalAccessKind::kRowIdRange,
                .cost =
                    AccessPathCost{
                        .run_log_est =
                            AddLogEst(LogEstLogarithm(input), output + kTableScanCostBias),
                        .output_log_est = output,
                    },
                .range_bound_count = second.has_value() ? 2U : 1U,
            },
        .payload = std::move(range),
        .source_position = position,
        .prerequisites = prerequisites | first.prerequisites |
                         (second.has_value() ? second->prerequisites : SourceMask{0}),
        .sort_index = order_candidates_ ? 1U : 0U,
    };
    AdjustOutput(loop, input);
    return InsertWhereLoop(plan_, loop, budget_);
  }

  [[nodiscard]] bool GoodAutomaticColumn(const SourceInfo& source, ColumnId column) const {
    for (const IndexId id : bound_.catalog()->table_indexes(AssumePresent(source.table))) {
      const auto& index = bound_.catalog()->index(id);
      for (std::size_t position = 0; position < index.key_term_count; ++position) {
        if (std::get_if<ColumnId>(&index.terms[position].target) != nullptr &&
            std::get<ColumnId>(index.terms[position].target) == column) {
          if (position == 0U ||
              (index.statistics.has_stat1 && PrefixEstimate(index, source, position + 1U) > 20)) {
            return false;
          }
          break;
        }
      }
    }
    return true;
  }

  template <typename Visitor>
  bool VisitAutomaticDrivers(std::size_t position, const SourceInfo& source, SourceMask ready,
                             Visitor&& visitor) const {
    const auto& table = bound_.catalog()->table(AssumePresent(source.table));
    return VisitComparisons([&](std::size_t term_index, bool commuted) {
      const auto& term = terms_[term_index];
      const auto column = commuted ? term.right_column : term.left_column;
      if (!column.has_value() || column->source_position != position ||
          !column->column.has_value() || table.rowid_alias == column->column ||
          !ConstraintCompatibleWithSource(core_, position, term)) {
        return true;
      }
      auto match = ComparisonMatch(term_index, commuted);
      if (!match.has_value() || match->constraint.kind != IndexConstraintKind::kEquality ||
          (match->prerequisites & ~ready) != 0U ||
          (!match->constraint.reject_null && IsNullLiteral(bound_, match->constraint.key)) ||
          !IndexAffinityMatches(term.comparison->affinity,
                                table.columns[column->column->value].affinity) ||
          !GoodAutomaticColumn(source, *column->column)) {
        return true;
      }
      return visitor(*column->column, *match);
    });
  }

  bool AddAutomatic(std::size_t position, const SourceInfo& source, SourceMask prerequisites) {
    if (!options_.allow_file_backed_automatic_indexes || core_.table_sources.size() < 2U ||
        !source.table.has_value() || !source.rowid_eligible ||
        std::ranges::any_of(core_.table_sources, [](const auto& declaration) {
          return SourceJoinPreservesRight(declaration.join);
        })) {
      return true;
    }
    return VisitAutomaticDrivers(
        position, source, AllSources(core_.table_sources.size()) & ~SourceBit(position),
        [&](ColumnId column, const Match& match) {
          const auto& comparison = *terms_[match.term].comparison;
          const CatalogIndexTerm key{
              .target = column,
              .collation_name = bound_.collations()[comparison.collation.value()].name,
          };
          PhysicalIndexEquality equality = Equality(key, source, match);
          equality.collation_name = bound_.collations()[comparison.collation.value()].name;
          AutomaticCandidatePlan automatic{
              .node =
                  PhysicalAutomaticIndexNode{
                      .core_index = core_index_,
                      .source = source.id,
                      .table = *source.table,
                      .table_root_page = source.root_page,
                      .equalities = {equality},
                  },
              .selected_terms = std::vector<bool>(terms_.size(), false),
          };
          automatic.selected_terms[match.term] = true;
          const LogEst rows = ToLogEst(source.estimated_rows);
          ReadAccessCandidate loop{
              .published =
                  AccessPathCandidate{
                      .kind = PhysicalAccessKind::kAutomaticIndex,
                      .cost =
                          AccessPathCost{
                              .setup_log_est = std::max<LogEst>(
                                  0, LogEstLogarithm(rows) + rows + kAutomaticIndexSetupBias),
                              .run_log_est =
                                  AddLogEst(LogEstLogarithm(rows), kAutomaticIndexOutput),
                              .output_log_est = kAutomaticIndexOutput,
                          },
                      .covering = true,
                      .equality_term_count = 1,
                  },
              .payload = std::move(automatic),
              .source_position = position,
              .prerequisites = prerequisites | match.prerequisites,
          };
          return InsertWhereLoop(plan_, loop, budget_);
        });
  }

  bool AddSource(std::size_t position, SourceMask prerequisites) {
    const auto& declaration = core_.table_sources[position];
    const SourceInfo source = ResolveTableSource(bound_, declaration);
    if (!AddAutomatic(position, source, prerequisites)) {
      return false;
    }
    ReadAccessCandidate scan{
        .published = AccessPathCandidate{.kind = PhysicalAccessKind::kTableScan,
                                         .cost = ScanCost(source.estimated_rows)},
        .source_position = position,
        .prerequisites = prerequisites,
        .sort_index = order_candidates_ ? 1U : 0U,
    };
    AdjustOutput(scan, ToLogEst(source.estimated_rows));
    if (!InsertWhereLoop(plan_, scan, budget_)) {
      return false;
    }
    if (!source.rowid_eligible) {
      return true;
    }
    if (!AddRowIdTerms(position, source, prerequisites)) {
      return false;
    }
    if (!source.table.has_value()) {
      return true;
    }
    const auto& table = bound_.catalog()->table(AssumePresent(source.table));
    return VisitIndexes(*source.table, [&](IndexId id) {
      const auto& index = bound_.catalog()->index(id);
      if (!IsSupportedIndexShape(bound_, table, index)) {
        return true;
      }
      ReadAccessCandidate loop{
          .payload = IndexTemplate(position, id),
          .source_position = position,
          .prerequisites = prerequisites,
          .sort_index = order_candidates_ && !index.statistics.unordered ? id.value + 2U : 0U,
      };
      auto& candidate = std::get<IndexCandidatePlan>(loop.payload);
      candidate.node.equalities.reserve(index.key_term_count);
      SetCovering(candidate, source);
      const LogEst index_width = ToLogEst(index.statistics.average_row_size.value_or(
          DerivedIndexRowSize(*bound_.catalog(), index)));
      if (!index.statistics.unordered &&
          (order_candidates_ ||
           (candidate.node.covering && index_width < ToLogEst(source.estimated_row_size)))) {
        loop.published = PublishIndexCandidate(bound_, source, candidate);
        AdjustOutput(loop, ToLogEst(source.estimated_rows));
        if (!InsertWhereLoop(plan_, loop, budget_)) {
          return false;
        }
      }
      return AddIndexTerms(loop, source, PrefixEstimate(index, source, 0U));
    });
  }

  [[nodiscard]] std::expected<void, OptimizerError> CompleteAutomatic(ReadAccessCandidate& loop,
                                                                      SourceMask ready) {
    auto& automatic = std::get<AutomaticCandidatePlan>(loop.payload);
    const auto& source = core_.table_sources[loop.source_position];
    const auto& table = bound_.catalog()->table(AssumePresent(source.table));
    const SourceInfo information = ResolveTableSource(bound_, source);
    automatic.node.equalities.clear();
    std::ranges::fill(automatic.selected_terms, false);
    for (const auto& term : terms_) {
      if (CanFilterAutomaticIndex(bound_, core_, loop.source_position, term)) {
        automatic.node.construction_predicates.push_back(term.predicate);
      }
    }
    SourceMask indexed_columns = 0;
    VisitAutomaticDrivers(
        loop.source_position, information, ready, [&](ColumnId column, const Match& match) {
          const SourceMask bit = SourceBit(std::min(column.value, kTopColumn));
          if ((indexed_columns & bit) != 0U) {
            return true;
          }
          const auto& comparison = *terms_[match.term].comparison;
          const CatalogIndexTerm key{
              .target = column,
              .collation_name = bound_.collations()[comparison.collation.value()].name,
          };
          PhysicalIndexEquality equality = Equality(key, information, match);
          equality.collation_name = bound_.collations()[comparison.collation.value()].name;
          automatic.node.equalities.push_back(equality);
          automatic.selected_terms[match.term] = true;
          indexed_columns |= bit;
          return true;
        });
    if (automatic.node.equalities.empty()) {
      return std::unexpected(InvariantFailure("selected automatic loop has no available key"));
    }
    auto required = DiscoverRequiredSourceValues(bound_, source.id, table, terms_, {}, &core_);
    for (const auto& equality : automatic.node.equalities) {
      if (equality.column.value < kTopColumn) {
        required.columns[equality.column.value] = false;
      }
    }
    const bool wide_columns = std::ranges::any_of(required.columns | std::views::drop(kTopColumn),
                                                  [](bool used) { return used; });
    for (std::size_t column = 0; column < required.columns.size(); ++column) {
      if ((required.columns[column] || (wide_columns && column >= kTopColumn)) &&
          table.rowid_alias != ColumnId{column}) {
        automatic.node.covering_columns.push_back(ColumnId{column});
      }
    }
    if (automatic.node.equalities.size() + automatic.node.covering_columns.size() + 1U >
        kMaximumRecordFieldCount) {
      return std::unexpected(OptimizerFailure(OptimizerErrorCode::kUnsupportedFeature,
                                              "automatic index exceeds record field limit"));
    }
    loop.published.equality_term_count =
        static_cast<std::uint32_t>(automatic.node.equalities.size());
    return {};
  }

  const BoundQueryView& bound_;
  const BoundSelectCore& core_;
  std::size_t core_index_;
  std::span<ReadPredicateTerm> terms_;
  OptimizerOptions options_;
  LogEst outer_loops_;
  CoreOrderRequest order_request_;
  std::span<const BoundOrderingTerm> order_terms_;
  bool order_candidates_;
  bool adjust_output_;
  std::size_t budget_ = kPlannerSearchLimit;
  WhereLoopPlan plan_{};
  std::optional<OptimizerError> error_{};
};

struct NonjoinedCoreAccessAnalysis {
  std::size_t core_index = 0;
  SourceInfo source{};
  ReadPredicateAnalysis predicates{};
  std::vector<ReadAccessCandidate> access_candidates{};
  std::size_t selected_access = 0;
  std::vector<bool> selected_terms{};
  std::vector<BoundExpressionId> residuals{};
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;
  std::uint32_t ordered_prefix = 0;
  LogEst output_log_est = 0;
  bool search_limited = false;
  // These buffers remain stable when ownership moves into the published nodes.
  std::span<const BoundExpressionId> published_guards{};
  std::span<const BoundExpressionId> published_residuals{};
  std::span<const PhysicalIndexEquality> published_equalities{};
};

[[nodiscard]] std::expected<NonjoinedCoreAccessAnalysis, OptimizerError> AnalyzeNonjoinedCoreAccess(
    const BoundQueryView& bound_select, std::size_t core_index, OptimizerOptions options = {},
    LogEst outer_loops = 0, std::optional<CoreOrderRequest> order = std::nullopt) {
  const auto& core = std::get<BoundSelectCore>(bound_select.query_cores()[core_index]);
  NonjoinedCoreAccessAnalysis analysis{
      .core_index = core_index,
      .source = ResolveCoreSource(bound_select, core_index),
  };
  analysis.predicates = AnalyzeReadPredicate(bound_select, analysis.source, core.where);
  if (analysis.predicates.empty) {
    analysis.access_candidates.push_back(ReadAccessCandidate{
        .published =
            AccessPathCandidate{
                .kind = PhysicalAccessKind::kEmpty,
                .cost = {},
            },
    });
    return analysis;
  }
  if (analysis.source.single_row) {
    analysis.access_candidates.push_back(ReadAccessCandidate{
        .published =
            AccessPathCandidate{
                .kind = PhysicalAccessKind::kSingleRow,
                .cost = AccessPathCost{},
            },
    });
    analysis.output_log_est =
        RequestsInputDistinct(
            bound_select, core,
            order.has_value() ? order->terms.size() : bound_select.order_by().size())
            ? -kDistinctOutputReduction
            : 0;
    return analysis;
  }

  auto planned = WhereLoopBuilder{bound_select, core_index,  analysis.predicates.terms,
                                  options,      outer_loops, order}
                     .Run();
  if (!planned.has_value()) {
    return std::unexpected(std::move(planned.error()));
  }
  analysis.selected_access = planned->selected[0];
  analysis.direction = planned->loops[analysis.selected_access].direction;
  analysis.ordered_prefix = planned->ordered_prefix;
  analysis.output_log_est = planned->output_log_est;
  analysis.search_limited = planned->search_limited;
  analysis.access_candidates = std::move(planned->loops);

  analysis.selected_terms.assign(analysis.predicates.terms.size(), false);
  const ReadAccessCandidate& selected = analysis.access_candidates[analysis.selected_access];
  VisitLoopTerms(selected, [&](std::size_t term) {
    analysis.selected_terms[term] =
        !selected.transitive || !analysis.predicates.terms[term].equivalence;
  });
  analysis.residuals.reserve(
      static_cast<std::size_t>(std::ranges::count(analysis.selected_terms, false)));
  for (std::size_t term = 0; term < analysis.predicates.terms.size(); ++term) {
    if (!analysis.selected_terms[term]) {
      analysis.residuals.push_back(analysis.predicates.terms[term].predicate);
    }
  }
  return analysis;
}

[[nodiscard]] PhysicalCoreAccessPlan PublishCoreAccessPlan(
    const NonjoinedCoreAccessAnalysis& analysis) {
  PhysicalCoreAccessPlan published{
      .core_index = analysis.core_index,
      .candidates = {},
      .selected_candidate = analysis.selected_access,
      .search_limited = analysis.search_limited,
  };
  published.candidates.reserve(analysis.access_candidates.size());
  for (const ReadAccessCandidate& candidate : analysis.access_candidates) {
    published.candidates.push_back(candidate.published);
  }
  return published;
}

[[nodiscard]] PhysicalNodePayload BuildNonjoinedAccessLeaf(
    const NonjoinedCoreAccessAnalysis& analysis) {
  if (analysis.predicates.empty) {
    return PhysicalEmptyNode{.core_index = analysis.core_index};
  }
  if (analysis.source.single_row) {
    return PhysicalSingleRowNode{.core_index = analysis.core_index};
  }
  const ReadAccessCandidate& selected = analysis.access_candidates[analysis.selected_access];
  if (const auto* rowid = std::get_if<RowIdCandidatePlan>(&selected.payload); rowid != nullptr) {
    return PhysicalRowIdLookupNode{
        .core_index = analysis.core_index,
        .source = analysis.source.id,
        .source_kind = analysis.source.source_kind,
        .table = analysis.source.table,
        .root_page = analysis.source.root_page,
        .key = rowid->key,
        .constraint =
            PhysicalAccessPredicate{
                .predicate = rowid->constraint.predicate,
                .conjunct_position = rowid->constraint.conjunct_position,
                .origin = rowid->constraint.origin,
            },
    };
  }
  if (const auto* range = std::get_if<RowIdRangeCandidatePlan>(&selected.payload);
      range != nullptr) {
    PhysicalRowIdRangeNode node = range->node;
    node.core_index = analysis.core_index;
    node.direction = analysis.direction;
    return node;
  }
  if (const auto* index = std::get_if<IndexCandidatePlan>(&selected.payload); index != nullptr) {
    PhysicalIndexScanNode node = index->node;
    node.core_index = analysis.core_index;
    node.direction = analysis.direction;
    return node;
  }
  return PhysicalTableScanNode{
      .core_index = analysis.core_index,
      .source = analysis.source.id,
      .source_kind = analysis.source.source_kind,
      .table = analysis.source.table,
      .root_page = analysis.source.root_page,
      .direction = analysis.direction,
  };
}

struct JoinedCoreAccessAnalysis {
  std::size_t core_index = 0;
  std::vector<ReadPredicateTerm> terms{};
  WhereLoopPlan plan{};
};

[[nodiscard]] std::optional<std::size_t> JoinedSourceLevel(const JoinedCoreAccessAnalysis& analysis,
                                                           std::size_t source_position) noexcept {
  for (std::size_t level = 0; level < analysis.plan.level_count; ++level) {
    if (analysis.plan.loops[analysis.plan.selected[level]].source_position == source_position) {
      return level;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::expected<void, OptimizerError> AssignJoinedTermPhases(
    JoinedCoreAccessAnalysis& analysis, const BoundQueryView& bound) {
  const auto& core = std::get<BoundSelectCore>(bound.query_cores()[analysis.core_index]);
  const auto& plan = analysis.plan;
  for (ReadPredicateTerm& term : analysis.terms) {
    std::size_t floor = 0;
    std::optional<std::size_t> owner;
    if (term.on_source.has_value()) {
      owner = SourcePosition(core.table_sources, *term.on_source);
      if (!owner.has_value()) {
        return std::unexpected(InvariantFailure("joined term has no declared ON owner"));
      }
    }
    for (std::size_t position = 0; position < core.table_sources.size(); ++position) {
      if (SourceJoinPreservesRight(core.table_sources[position].join) &&
          (!term.constraint_owner.has_value() || !owner.has_value() || position <= *owner)) {
        const auto level = JoinedSourceLevel(analysis, position);
        if (!level.has_value()) {
          return std::unexpected(InvariantFailure("RIGHT source is absent from selected levels"));
        }
        floor = std::max(floor, *level);
      }
    }
    SourceMask ready = 0;
    std::optional<std::size_t> evaluation;
    for (std::size_t level = 0; level < plan.level_count; ++level) {
      ready |= SourceBit(plan.loops[plan.selected[level]].source_position);
      if (level >= floor && (term.prerequisites & ~ready) == 0U) {
        evaluation = level;
        break;
      }
    }
    if (!evaluation.has_value()) {
      return std::unexpected(
          InvariantFailure("joined predicate has no available evaluation level"));
    }
    if (owner.has_value()) {
      const auto owner_level = JoinedSourceLevel(analysis, *owner);
      if (!owner_level.has_value()) {
        return std::unexpected(InvariantFailure("ON owner is absent from selected levels"));
      }
      if (term.outer_on || (SourceIsLeftOfRightJoin(core, *owner) && *evaluation <= *owner_level)) {
        evaluation = *owner_level;
      }
    }
    const bool preopen = term.on_source.has_value() && !term.outer_on && *evaluation == 0U &&
                         IsStatementGuard(bound, term.predicate) &&
                         (!owner.has_value() || !SourceIsLeftOfRightJoin(core, *owner));
    if (!preopen) {
      term.evaluation_source =
          core.table_sources[plan.loops[plan.selected[*evaluation]].source_position].id;
    }
    if (preopen) {
      continue;
    }
    ready = 0;
    for (std::size_t level = 0; level <= *evaluation; ++level) {
      const auto& loop = plan.loops[plan.selected[level]];
      ready |= SourceBit(loop.source_position);
      const auto& source = core.table_sources[loop.source_position];
      if ((term.prerequisites & ~ready) == 0U &&
          (!SourceJoinPreservesLeft(source.join) || term.outer_on) &&
          (!loop.transitive || !term.equivalence)) {
        const auto term_index = static_cast<std::size_t>(&term - analysis.terms.data());
        term.consumed_by_access = term.consumed_by_access || LoopUsesTerm(loop, term_index);
      }
    }
  }
  return {};
}

[[nodiscard]] std::expected<JoinedCoreAccessAnalysis, OptimizerError> AnalyzeJoinedCoreAccess(
    const BoundQueryView& bound, std::size_t core_index, OptimizerOptions options,
    LogEst outer_loops = 0, std::optional<CoreOrderRequest> order = std::nullopt) {
  const auto& core = std::get<BoundSelectCore>(bound.query_cores()[core_index]);
  JoinedCoreAccessAnalysis analysis{
      .core_index = core_index,
      .terms = CollectLoopTerms(bound, core),
  };
  auto planned =
      WhereLoopBuilder{bound, core_index, analysis.terms, options, outer_loops, order}.Run();
  if (!planned.has_value()) {
    return std::unexpected(std::move(planned.error()));
  }
  analysis.plan = std::move(*planned);
  if (auto assigned = AssignJoinedTermPhases(analysis, bound); !assigned.has_value()) {
    return std::unexpected(std::move(assigned.error()));
  }
  return analysis;
}

[[nodiscard]] PhysicalCoreAccessPlan PublishJoinedCoreAccessPlan(
    const JoinedCoreAccessAnalysis& analysis, const BoundQueryView& bound) {
  const auto& core = std::get<BoundSelectCore>(bound.query_cores()[analysis.core_index]);
  PhysicalCoreAccessPlan published{
      .core_index = analysis.core_index,
      .candidates = {},
      .search_limited = analysis.plan.search_limited,
      .ordered_inner_loop = analysis.plan.ordered_inner_loop,
  };
  published.source_order.reserve(analysis.plan.level_count);
  for (std::size_t level = 0; level < analysis.plan.level_count; ++level) {
    published.source_order.push_back(
        core.table_sources[analysis.plan.loops[analysis.plan.selected[level]].source_position].id);
  }
  return published;
}

[[nodiscard]] std::expected<PhysicalNodePayload, OptimizerError> BuildJoinedAccessLeaf(
    const JoinedCoreAccessAnalysis& analysis, const BoundQueryView& bound,
    BoundSourceId source_id) {
  const auto& core = std::get<BoundSelectCore>(bound.query_cores()[analysis.core_index]);
  const auto position = SourcePosition(core.table_sources, source_id);
  const auto level = position.has_value() ? JoinedSourceLevel(analysis, *position) : std::nullopt;
  if (!level.has_value()) {
    return std::unexpected(InvariantFailure("joined leaf has no selected access"));
  }
  const auto& selected = analysis.plan.loops[analysis.plan.selected[*level]];
  const SourceInfo source = ResolveTableSource(bound, core.table_sources[*position]);
  if (const auto* rowid = std::get_if<RowIdCandidatePlan>(&selected.payload); rowid != nullptr) {
    return PhysicalRowIdLookupNode{
        .core_index = analysis.core_index,
        .source = source_id,
        .source_kind = source.source_kind,
        .table = source.table,
        .root_page = source.root_page,
        .key = rowid->key,
        .constraint =
            PhysicalAccessPredicate{.predicate = rowid->constraint.predicate,
                                    .conjunct_position = rowid->constraint.conjunct_position,
                                    .origin = rowid->constraint.origin},
    };
  }
  if (const auto* range = std::get_if<RowIdRangeCandidatePlan>(&selected.payload);
      range != nullptr) {
    PhysicalRowIdRangeNode node = range->node;
    node.direction = selected.direction;
    return node;
  }
  if (const auto* index = std::get_if<IndexCandidatePlan>(&selected.payload); index != nullptr) {
    PhysicalIndexScanNode node = index->node;
    node.direction = selected.direction;
    return node;
  }
  if (const auto* automatic = std::get_if<AutomaticCandidatePlan>(&selected.payload);
      automatic != nullptr) {
    return automatic->node;
  }
  if (selected.published.kind != PhysicalAccessKind::kTableScan) {
    return std::unexpected(InvariantFailure("joined selected access has an unsupported payload"));
  }
  return PhysicalTableScanNode{
      .core_index = analysis.core_index,
      .source = source_id,
      .source_kind = source.source_kind,
      .table = source.table,
      .root_page = source.root_page,
      .direction = selected.direction,
  };
}

[[nodiscard]] std::vector<PhysicalJoinPredicatePlacement> PublishJoinedPredicates(
    const JoinedCoreAccessAnalysis& analysis, BoundSourceId owner) {
  std::vector<PhysicalJoinPredicatePlacement> predicates;
  for (const ReadPredicateTerm& term : analysis.terms) {
    if (term.on_source == owner) {
      predicates.push_back(PhysicalJoinPredicatePlacement{
          .predicate = term.predicate,
          .conjunct_position = term.conjunct_position,
          .source = term.evaluation_source,
          .consumed_by_access = term.consumed_by_access,
      });
    }
  }
  return predicates;
}

[[nodiscard]] std::expected<PhysicalFilterNode, OptimizerError> PublishJoinedFilter(
    const JoinedCoreAccessAnalysis& analysis, const BoundQueryView& bound, PhysicalNodeId input) {
  PhysicalFilterNode filter{.input = input};
  for (const ReadPredicateTerm& term : analysis.terms) {
    if (term.origin != PhysicalPredicateOrigin::kWhere) {
      continue;
    }
    if (IsStatementGuard(bound, term.predicate)) {
      filter.guards.push_back(term.predicate);
    } else if (!term.consumed_by_access) {
      if (!term.evaluation_source.has_value()) {
        return std::unexpected(InvariantFailure("joined WHERE term has no evaluation source"));
      }
      filter.predicates.push_back(term.predicate);
      filter.predicate_sources.push_back(*term.evaluation_source);
    }
  }
  return filter;
}

[[nodiscard]] bool MatchesJoinedAccess(const PhysicalNode& actual,
                                       const PhysicalNodePayload& expected) {
  return std::visit(
      [&](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, PhysicalTableScanNode> ||
                      std::is_same_v<Node, PhysicalRowIdLookupNode> ||
                      std::is_same_v<Node, PhysicalRowIdRangeNode> ||
                      std::is_same_v<Node, PhysicalIndexScanNode> ||
                      std::is_same_v<Node, PhysicalAutomaticIndexNode>) {
          const auto* value = std::get_if<Node>(&actual.payload);
          return value != nullptr && *value == node;
        }
        return false;
      },
      expected);
}

[[nodiscard]] bool MatchesNonjoinedAccess(const PhysicalNode& actual,
                                          const PhysicalNodePayload& expected) {
  return std::visit(
      [&](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, PhysicalEmptyNode> ||
                      std::is_same_v<Node, PhysicalSingleRowNode> ||
                      std::is_same_v<Node, PhysicalTableScanNode> ||
                      std::is_same_v<Node, PhysicalRowIdLookupNode> ||
                      std::is_same_v<Node, PhysicalRowIdRangeNode> ||
                      std::is_same_v<Node, PhysicalIndexScanNode>) {
          const auto* value = std::get_if<Node>(&actual.payload);
          return value != nullptr && *value == node;
        }
        return false;
      },
      expected);
}

[[nodiscard]] bool MatchesIndexScan(const PhysicalIndexScanNode& actual,
                                    const PhysicalIndexScanNode& expected, std::size_t core_index,
                                    PhysicalScanDirection direction,
                                    std::span<const PhysicalIndexEquality> equalities) noexcept {
  return actual.core_index == core_index && actual.source == expected.source &&
         actual.table == expected.table && actual.table_root_page == expected.table_root_page &&
         actual.index == expected.index && actual.index_root_page == expected.index_root_page &&
         std::ranges::equal(actual.equalities, equalities) && actual.range == expected.range &&
         actual.covering == expected.covering && actual.direction == direction &&
         actual.two_pass_null_order == expected.two_pass_null_order;
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
  LogEst output_log_est = 0;
  bool search_limited = false;
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
                                        const BoundAggregateProgram& aggregate,
                                        std::size_t core_index,
                                        std::span<const BoundOrderingTerm> terms,
                                        bool merge_collation) {
  if (terms.size() != aggregate.group_by.size()) {
    return false;
  }
  const SourceInfo source = ResolveCoreSource(bound_select, core_index);
  const auto without_alias = [&](BoundExpressionId expression) {
    while (const auto* alias = std::get_if<BoundAliasReferenceExpression>(
               &bound_select.expression(expression).payload)) {
      expression = alias->target;
    }
    return expression;
  };
  for (std::size_t term = 0; term < aggregate.group_by.size(); ++term) {
    const BoundOrderingTerm& ordering = terms[term];
    const BoundGroupingTerm& grouping = aggregate.group_by[term];
    bool equivalent = false;
    if (merge_collation) {
      // Native merge keys acquire COLLATE before each arm's alias resolution.
      BoundExpressionId grouped_value = grouping.expression;
      const auto* grouped_collation =
          std::get_if<BoundCollateExpression>(&bound_select.expression(grouped_value).payload);
      if (grouping.ordinal_reference && grouped_collation != nullptr) {
        BoundExpressionId target = grouped_collation->operand;
        while (const auto* nested =
                   std::get_if<BoundCollateExpression>(&bound_select.expression(target).payload)) {
          target = nested->operand;
        }
        const auto* reference =
            std::get_if<BoundAliasReferenceExpression>(&bound_select.expression(target).payload);
        if (reference == nullptr) {
          return false;
        }
        grouped_value = reference->target;
      } else {
        grouped_value = without_alias(grouped_value);
        grouped_collation =
            std::get_if<BoundCollateExpression>(&bound_select.expression(grouped_value).payload);
        if (grouped_collation == nullptr) {
          return false;
        }
        const bool resolves_result =
            !grouping.ordinal_reference &&
            std::ranges::any_of(BoundCoreResultColumns(bound_select.query_cores()[core_index]),
                                [&](const BoundResultColumn& result) {
                                  return BoundExpressionsEquivalent(bound_select, grouped_value,
                                                                    result.expression);
                                });
        if (!resolves_result) {
          grouped_value = grouped_collation->operand;
        }
      }
      equivalent = grouped_collation->collation == ordering.collation &&
                   BoundExpressionsEquivalent(bound_select, ordering.expression, grouped_value);
    } else {
      const std::optional<DirectSourceIdentity> ordering_identity =
          ResolveDirectSourceIdentity(bound_select, source, ordering.expression);
      const std::optional<DirectSourceIdentity> grouping_identity =
          ResolveDirectSourceIdentity(bound_select, source, grouping.expression);
      equivalent =
          ordering_identity.has_value() && grouping_identity.has_value()
              ? *ordering_identity == *grouping_identity
              : BoundExpressionsEquivalent(bound_select, ordering.expression, grouping.expression);
    }
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
                             const BoundAggregateProgram& aggregate, std::size_t core_index,
                             std::optional<CoreOrderRequest> request, AggregateCorePlan* plan) {
  const auto terms = request.has_value() ? request->terms : bound_select.order_by();
  if ((!request.has_value() &&
       (bound_select.query_cores().size() != 1U || !bound_select.compound_operators().empty())) ||
      terms.empty()) {
    return;
  }
  if (plan->strategy == PhysicalAggregateStrategy::kGlobal ||
      plan->strategy == PhysicalAggregateStrategy::kIndexedMinMax) {
    plan->order_satisfied = true;
    return;
  }
  if (!OrderMatchesGrouping(bound_select, aggregate, core_index, terms, request.has_value())) {
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
      const BoundOrderingTerm& ordering = terms[term];
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
    const BoundOrderingTerm& requested = terms[group_term];
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

  if (candidate.published.kind == PhysicalAccessKind::kTableScan ||
      candidate.published.kind == PhysicalAccessKind::kRowIdRange) {
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
  } else if (identity->rowid && (candidate.published.kind == PhysicalAccessKind::kRowIdRange ||
                                 (candidate.published.kind == PhysicalAccessKind::kTableScan &&
                                  source.rowid_eligible))) {
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

[[nodiscard]] std::expected<AggregateCorePlan, OptimizerError> BuildAggregateCorePlan(
    const BoundQueryView& bound_select, std::size_t core_index, OptimizerOptions options = {},
    LogEst outer_loops = 0, std::optional<CoreOrderRequest> order = std::nullopt) {
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
    ConfigureAggregateOrder(bound_select, aggregate, core_index, order, &plan);
    return plan;
  }
  auto analyzed = AnalyzeNonjoinedCoreAccess(bound_select, core_index, options, outer_loops, order);
  if (!analyzed.has_value()) {
    return std::unexpected(std::move(analyzed.error()));
  }
  NonjoinedCoreAccessAnalysis access = std::move(*analyzed);
  plan.source = access.source;
  plan.predicates = std::move(access.predicates);
  const std::size_t normal_access = access.selected_access;
  plan.output_log_est = access.output_log_est;
  plan.search_limited = access.search_limited;
  plan.access_candidates = std::move(access.access_candidates);
  if (plan.predicates.empty) {
    plan.strategy = aggregate.group_by.empty() ? PhysicalAggregateStrategy::kGlobal
                                               : PhysicalAggregateStrategy::kOrderedGroup;
    plan.at_most_one_row = true;
    ConfigureAggregateOrder(bound_select, aggregate, core_index, order, &plan);
    return plan;
  }
  if (plan.source.single_row) {
    plan.strategy = aggregate.group_by.empty() ? PhysicalAggregateStrategy::kGlobal
                                               : PhysicalAggregateStrategy::kOrderedGroup;
    plan.at_most_one_row = true;
    ConfigureAggregateOrder(bound_select, aggregate, core_index, order, &plan);
    return plan;
  }
  std::optional<std::size_t> selected_ordered;
  std::optional<AggregateMinMaxChoice> min_max;
  const bool shortcut = plan.access_candidates[normal_access].single_row_shortcut;
  const std::size_t first_candidate = shortcut ? normal_access : 0U;
  const std::size_t candidate_end = shortcut ? normal_access + 1U : plan.access_candidates.size();
  if (aggregate.group_by.empty()) {
    for (std::size_t candidate = first_candidate; candidate < candidate_end; ++candidate) {
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
    for (std::size_t candidate = first_candidate; candidate < candidate_end; ++candidate) {
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
  VisitLoopTerms(selected, [&](std::size_t term) {
    selected_terms[term] = !selected.transitive || !plan.predicates.terms[term].equivalence;
  });
  plan.output_log_est = selected.single_row_shortcut
                            ? kShortcutOutput
                            : std::min(outer_loops, kMaximumOuterLoopEstimate) +
                                  selected.published.cost.output_log_est;
  if (!selected.single_row_shortcut &&
      RequestsInputDistinct(
          bound_select, core,
          order.has_value() ? order->terms.size() : bound_select.order_by().size())) {
    plan.output_log_est -= kDistinctOutputReduction;
  }
  plan.residuals.reserve(plan.predicates.terms.size());
  for (std::size_t term = 0; term < plan.predicates.terms.size(); ++term) {
    if (!selected_terms[term]) {
      plan.residuals.push_back(plan.predicates.terms[term].predicate);
    }
  }
  ConfigureAggregateOrder(bound_select, aggregate, core_index, order, &plan);
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
    case PhysicalNodeKind::kRowIdRange:
      return PhysicalAccessKind::kRowIdRange;
    case PhysicalNodeKind::kIndexScan:
      return PhysicalAccessKind::kIndexScan;
    case PhysicalNodeKind::kAutomaticIndex:
      return PhysicalAccessKind::kAutomaticIndex;
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

[[nodiscard]] std::uint32_t InputOrderedPrefix(
    const BoundQueryView& bound, std::span<const std::optional<AggregateCorePlan>> aggregate_plans,
    std::span<const std::optional<JoinedCoreAccessAnalysis>> joined_plans) {
  if (aggregate_plans.size() == 1U && aggregate_plans.front().has_value() &&
      aggregate_plans.front()->order_satisfied) {
    return static_cast<std::uint32_t>(bound.order_by().size());
  }
  if (bound.query_cores().size() != 1U || bound.order_by().empty() || joined_plans.size() != 1U ||
      !joined_plans.front().has_value()) {
    return 0;
  }
  const auto* core = std::get_if<BoundSelectCore>(&bound.query_cores().front());
  return core != nullptr && !core->aggregate.has_value() &&
                 core->quantifier != SelectQuantifier::kDistinct
             ? AssumePresent(joined_plans.front()).plan.ordered_prefix
             : 0U;
}

[[nodiscard]] std::expected<PhysicalSortNode, OptimizerError> BuildPhysicalSortNode(
    const LogicalOrderNode& logical, PhysicalNodeId input, const BoundQueryView& bound,
    std::uint32_t ordered_prefix) {
  if (ordered_prefix > logical.terms.size()) {
    return std::unexpected(InvariantFailure("ordered prefix exceeds the requested order"));
  }
  PhysicalSortNode node{
      .input = input,
      .terms = logical.terms,
      .schedule = logical.schedule,
      .strategy = bound.limit() == nullptr ? PhysicalSortStrategy::kExternal
                                           : PhysicalSortStrategy::kRuntimeLimit,
      .ordered_prefix = ordered_prefix,
  };
  if (ordered_prefix == 0U || node.input_order_satisfied()) {
    node.payload_expressions = logical.payload_expressions;
    node.output_fields = logical.output_fields;
    return node;
  }
  const auto results = bound.result_columns();
  auto fields = planner_detail::BuildSortOutputFields(node.terms, ordered_prefix, results);
  if (!fields.has_value()) {
    return std::unexpected(InvariantFailure(fields.error().detail));
  }
  node.output_fields = std::move(*fields);
  node.payload_expressions.reserve(results.size());
  for (std::size_t column = 0; column < results.size(); ++column) {
    if (node.output_fields[column].kind == SortOutputFieldKind::kPayload) {
      node.payload_expressions.push_back(results[column].expression);
    }
  }
  return node;
}

[[nodiscard]] std::expected<LogicalCoreOrderLayout, OptimizerError> BuildPhysicalOrderLayout(
    const BoundQueryView& bound, const LogicalCoreOrderLayout& logical,
    std::uint32_t ordered_prefix, std::span<const BoundOrderingTerm> terms) {
  if (ordered_prefix > logical.key_values.size()) {
    return std::unexpected(InvariantFailure("ordered prefix exceeds the core key layout"));
  }
  if (ordered_prefix == 0U || ordered_prefix == logical.key_values.size()) {
    return logical;
  }
  const auto* core = std::get_if<BoundSelectCore>(&bound.query_cores()[logical.core_index]);
  if (core == nullptr || core->aggregate.has_value()) {
    return std::unexpected(InvariantFailure("partial order requires an ordinary query core"));
  }
  auto fields = planner_detail::BuildSortOutputFields(terms, ordered_prefix, core->result_columns);
  if (!fields.has_value()) {
    return std::unexpected(InvariantFailure(fields.error().detail));
  }
  LogicalCoreOrderLayout layout{
      .core_index = logical.core_index,
      .key_values = logical.key_values,
      .output_fields = std::move(*fields),
      .schedule = logical.schedule,
  };
  layout.payload_values.reserve(core->result_columns.size());
  for (std::size_t column = 0; column < core->result_columns.size(); ++column) {
    if (layout.output_fields[column].kind == SortOutputFieldKind::kPayload) {
      const SortOutputField& original = logical.output_fields[column];
      layout.payload_values.push_back(original.kind == SortOutputFieldKind::kKey
                                          ? logical.key_values[original.field_index]
                                          : logical.payload_values[original.field_index]);
    }
  }
  return layout;
}

struct NativeCompoundPlan {
  std::size_t first_compound = 0;
  std::vector<PhysicalCompoundNode> compounds;
  std::vector<PhysicalCoreOrderPlan> core_orders;

  [[nodiscard]] static std::expected<std::optional<NativeCompoundPlan>, OptimizerError> Build(
      const LogicalQueryView& logical) {
    const auto bound = logical.bound_select();
    const auto operations = bound.compound_operators();
    if (operations.empty()) {
      return std::nullopt;
    }
    const bool limit_one = BoundQueryHasSingleRowDemand(bound.query(), bound.expressions());
    if (bound.order_by().empty() &&
        std::ranges::all_of(operations, [&](CompoundOperator operation) {
          return !BoundCompoundRequiresMerge(operation, false, limit_one);
        })) {
      return std::nullopt;
    }
    const auto cores = bound.query_cores();
    std::vector<PhysicalNodeId> core_roots(cores.size(), PhysicalNodeId{0});
    NativeCompoundPlan result;
    result.first_compound = logical.nodes().size();
    for (std::size_t index = 0; index < logical.nodes().size(); ++index) {
      const auto& payload = logical.nodes()[index].payload;
      const PhysicalNodeId id{static_cast<std::uint32_t>(index)};
      if (const auto* values = std::get_if<LogicalValuesNode>(&payload); values != nullptr) {
        core_roots[values->core_index] = id;
      } else if (const auto* projection = std::get_if<LogicalProjectionNode>(&payload);
                 projection != nullptr) {
        core_roots[projection->core_index] = id;
      } else if (const auto* distinct = std::get_if<LogicalDistinctNode>(&payload);
                 distinct != nullptr) {
        core_roots[distinct->core_index] = id;
      } else if (std::holds_alternative<LogicalCompoundNode>(payload)) {
        result.first_compound = std::min(result.first_compound, index);
      }
    }
    if (result.first_compound == logical.nodes().size()) {
      return std::unexpected(InvariantFailure("compound query has no logical compound nodes"));
    }
    const auto local_collations =
        [&](std::size_t first,
            std::size_t last) -> std::expected<std::vector<BoundCollationId>, OptimizerError> {
      const auto local = cores.subspan(first, last - first + 1U);
      const auto results = BoundCoreResultColumns(local.front());
      std::vector<BoundCollationId> collations;
      collations.reserve(results.size());
      for (std::size_t column = 0; column < results.size(); ++column) {
        const auto selected = BoundCompoundResultCollation(local, bound.expressions(), column);
        if (selected.has_value()) {
          collations.push_back(*selected);
          continue;
        }
        const auto binary = std::ranges::find_if(
            bound.collations(),
            [](const BoundCollation& value) { return CatalogNamesEqual(value.name, "BINARY"); });
        if (binary == bound.collations().end()) {
          return std::unexpected(InvariantFailure("compound comparison has no BINARY collation"));
        }
        collations.emplace_back(static_cast<std::uint32_t>(binary - bound.collations().begin()));
      }
      return collations;
    };
    struct Frame {
      std::size_t first = 0;
      std::size_t last = 0;
      std::vector<BoundOrderingTerm> order{};
      std::optional<BoundLimit> syntax_limit{};
      bool runtime_bound = false;
      std::size_t phase = 0;
      std::size_t middle = 0;
      bool merge = false;
      CompoundOperator operation = CompoundOperator::kUnionAll;
      PhysicalNodeId left{0};
      std::vector<BoundCollationId> collations{};
    };
    const bool wrapper = BoundQueryHasCompoundOrderWrapper(bound.query());
    Frame root{
        .last = cores.size() - 1U,
        .syntax_limit = !wrapper && bound.limit() != nullptr
                            ? std::optional<BoundLimit>{*bound.limit()}
                            : std::nullopt,
        .runtime_bound = !wrapper && bound.limit() != nullptr,
    };
    if (!wrapper) {
      root.order.assign(bound.order_by().begin(), bound.order_by().end());
    }
    std::vector<Frame> stack;
    stack.push_back(std::move(root));
    result.compounds.reserve(operations.size());
    result.core_orders.resize(cores.size());
    PhysicalNodeId completed{0};
    while (!stack.empty()) {
      Frame& frame = stack.back();
      if (frame.first == frame.last) {
        const std::size_t core = frame.first;
        PhysicalCoreOrderPlan& plan = result.core_orders[core];
        plan.layout.core_index = core;
        plan.terms = std::move(frame.order);
        plan.syntax_limit = frame.syntax_limit;
        plan.strategy = frame.runtime_bound ? PhysicalSortStrategy::kRuntimeLimit
                                            : PhysicalSortStrategy::kExternal;
        const auto results = BoundCoreResultColumns(cores[core]);
        for (BoundOrderingTerm& term : plan.terms) {
          if (!term.result_column.has_value() || *term.result_column >= results.size()) {
            return std::unexpected(InvariantFailure("compound order has no result-field identity"));
          }
          term.expression = results[*term.result_column].expression;
        }
        if (!plan.terms.empty()) {
          const auto* select = std::get_if<BoundSelectCore>(&cores[core]);
          const bool complete_rows =
              select == nullptr || select->quantifier == SelectQuantifier::kDistinct;
          auto layout = planner_detail::BuildCoreOrderLayout(
              core, results, plan.terms,
              frame.runtime_bound && !complete_rows
                  ? OrderEvaluationSchedule::kKeysThenAdmissionThenPayload
                  : OrderEvaluationSchedule::kPayloadThenKeys,
              complete_rows);
          if (!layout.has_value()) {
            return std::unexpected(InvariantFailure(layout.error().detail));
          }
          plan.layout = std::move(*layout);
        }
        completed = core_roots[core];
        stack.pop_back();
        continue;
      }
      if (frame.phase == 0U) {
        frame.operation = operations[frame.last - 1U];
        const bool one =
            frame.syntax_limit.has_value() && !frame.syntax_limit->offset.has_value() &&
            BoundExpressionIsIntegerOne(bound.expressions(), frame.syntax_limit->limit);
        frame.merge = BoundCompoundRequiresMerge(frame.operation, !frame.order.empty(), one);
        if (frame.merge && frame.operation != CompoundOperator::kUnionAll) {
          auto collations = local_collations(frame.first, frame.last);
          if (!collations.has_value()) {
            return std::unexpected(std::move(collations.error()));
          }
          frame.collations = std::move(*collations);
          const auto results = BoundCoreResultColumns(cores[frame.last]);
          for (std::size_t column = 0; column < results.size(); ++column) {
            const bool present = std::ranges::any_of(
                frame.order,
                [&](const BoundOrderingTerm& term) { return term.result_column == column; });
            if (!present) {
              frame.order.push_back(BoundOrderingTerm{.expression = results[column].expression,
                                                      .collation = frame.collations[column],
                                                      .result_column = column});
            }
          }
        }
        const auto split = BoundCompoundSplit(
            operations.subspan(frame.first, frame.last - frame.first), frame.merge);
        if (!split.has_value()) {
          return std::unexpected(InvariantFailure("compound split lost a nonempty range"));
        }
        frame.middle = frame.first + *split;
        frame.phase = 1;
      } else if (frame.phase == 1U) {
        frame.left = completed;
        frame.phase = 2;
      } else {
        const PhysicalNodeId id{
            static_cast<std::uint32_t>(result.first_compound + result.compounds.size())};
        result.compounds.push_back(PhysicalCompoundNode{
            .left = frame.left,
            .right = completed,
            .operation = frame.operation,
            .collations = std::move(frame.collations),
            .order_by = std::move(frame.order),
            .strategy = frame.merge ? PhysicalCompoundStrategy::kOrderedMerge
                        : frame.operation == CompoundOperator::kUnion
                            ? PhysicalCompoundStrategy::kUnionLimitOne
                            : PhysicalCompoundStrategy::kConcatenate,
        });
        completed = id;
        stack.pop_back();
        continue;
      }
      const bool one = frame.syntax_limit.has_value() && !frame.syntax_limit->offset.has_value() &&
                       BoundExpressionIsIntegerOne(bound.expressions(), frame.syntax_limit->limit);
      stack.push_back(Frame{
          .first = frame.phase == 1U ? frame.first : frame.middle,
          .last = frame.phase == 1U ? frame.middle - 1U : frame.last,
          .order = frame.merge ? frame.order : std::vector<BoundOrderingTerm>{},
          .syntax_limit = frame.merge ? std::nullopt : frame.syntax_limit,
          .runtime_bound = frame.runtime_bound &&
                           (!frame.merge || frame.operation == CompoundOperator::kUnionAll ||
                            (frame.operation == CompoundOperator::kUnion && one)),
      });
    }
    if (result.compounds.size() != operations.size()) {
      return std::unexpected(InvariantFailure("native compound topology lost an operator"));
    }
    return std::optional<NativeCompoundPlan>{std::move(result)};
  }
};

[[nodiscard]] std::expected<void, OptimizerError> QualifyNativeCoreOrders(
    NativeCompoundPlan& native, const BoundQueryView& bound,
    std::span<const std::optional<AggregateCorePlan>> aggregates,
    std::span<const std::optional<NonjoinedCoreAccessAnalysis>> nonjoined,
    std::span<const std::optional<JoinedCoreAccessAnalysis>> joined) {
  if (native.core_orders.size() != bound.query_cores().size() ||
      aggregates.size() != native.core_orders.size() ||
      nonjoined.size() != native.core_orders.size() ||
      (!joined.empty() && joined.size() != native.core_orders.size())) {
    return std::unexpected(InvariantFailure("native core ordering has invalid access ownership"));
  }
  for (std::size_t core = 0; core < native.core_orders.size(); ++core) {
    PhysicalCoreOrderPlan& order = native.core_orders[core];
    std::uint32_t prefix = 0;
    if (!order.terms.empty()) {
      const auto& bound_core = bound.query_cores()[core];
      if (const auto& aggregate = aggregates[core]; aggregate.has_value()) {
        if (aggregate->order_satisfied) {
          prefix = static_cast<std::uint32_t>(order.terms.size());
        }
      } else if (const auto* select = std::get_if<BoundSelectCore>(&bound_core)) {
        if (select->table_sources.empty()) {
          prefix = static_cast<std::uint32_t>(order.terms.size());
        } else if (const auto& nonjoined_order = nonjoined[core]; nonjoined_order.has_value()) {
          prefix = nonjoined_order->ordered_prefix;
        } else if (!joined.empty()) {
          if (const auto& joined_order = joined[core]; joined_order.has_value()) {
            prefix = joined_order->plan.ordered_prefix;
          }
        }
      } else if (std::get<BoundValuesCore>(bound_core).rows.size() == 1U) {
        prefix = static_cast<std::uint32_t>(order.terms.size());
      }
    }
    if (prefix > order.terms.size()) {
      return std::unexpected(InvariantFailure("native input-order proof exceeds requested keys"));
    }
    order.ordered_prefix = prefix;
    if (prefix != 0U && prefix < order.terms.size()) {
      auto layout = BuildPhysicalOrderLayout(bound, order.layout, prefix, order.terms);
      if (!layout.has_value()) {
        return std::unexpected(std::move(layout.error()));
      }
      order.layout = std::move(*layout);
    }
  }
  return {};
}

[[nodiscard]] std::expected<void, OptimizerError> ValidateAdvancedPhysicalPlan(
    const LogicalQueryView& logical_plan, std::span<const PhysicalNode> nodes, PhysicalNodeId root,
    std::span<const AccessPathCandidate> candidates, std::size_t selected_candidate_index,
    std::span<const PhysicalCoreAccessPlan> core_accesses,
    std::span<const std::optional<AggregateCorePlan>> aggregate_plans,
    std::span<const std::optional<NonjoinedCoreAccessAnalysis>> nonjoined_plans,
    std::span<const std::optional<JoinedCoreAccessAnalysis>> joined_plans,
    std::span<const PhysicalCoreOrderPlan> core_orders) {
  const std::size_t core_count = logical_plan.bound_select().query_cores().size();
  if (nodes.size() != logical_plan.nodes().size() || root.value() != logical_plan.root().value() ||
      aggregate_plans.size() != core_count || nonjoined_plans.size() != core_count ||
      (!joined_plans.empty() && joined_plans.size() != core_count)) {
    return std::unexpected{InvariantFailure("advanced physical plan arena is invalid")};
  }
  auto native = NativeCompoundPlan::Build(logical_plan);
  if (!native.has_value()) {
    return std::unexpected(std::move(native.error()));
  }
  if (native->has_value()) {
    if (auto qualified = QualifyNativeCoreOrders(**native, logical_plan.bound_select(),
                                                 aggregate_plans, nonjoined_plans, joined_plans);
        !qualified.has_value()) {
      return qualified;
    }
    if (!std::ranges::equal(core_orders, (**native).core_orders)) {
      return std::unexpected(InvariantFailure("native core order publication is invalid"));
    }
  }
  std::vector<PhysicalCoreAccessPlan> expected_core_accesses;
  for (std::size_t core = 0; core < nonjoined_plans.size(); ++core) {
    if (nonjoined_plans[core].has_value()) {
      expected_core_accesses.push_back(PublishCoreAccessPlan(AssumePresent(nonjoined_plans[core])));
    } else if (aggregate_plans[core].has_value() && !AssumePresent(aggregate_plans[core]).joined) {
      const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans[core]);
      PhysicalCoreAccessPlan published{
          .core_index = core,
          .candidates = {},
          .selected_candidate = aggregate.selected_access.value_or(0U),
          .search_limited = aggregate.search_limited,
      };
      published.candidates.reserve(aggregate.access_candidates.size());
      for (const ReadAccessCandidate& candidate : aggregate.access_candidates) {
        published.candidates.push_back(candidate.published);
      }
      expected_core_accesses.push_back(std::move(published));
    } else if (!joined_plans.empty() && joined_plans[core].has_value()) {
      expected_core_accesses.push_back(PublishJoinedCoreAccessPlan(
          AssumePresent(joined_plans[core]), logical_plan.bound_select()));
    }
  }
  if (!std::ranges::equal(core_accesses, expected_core_accesses)) {
    return std::unexpected{InvariantFailure("advanced core access publication is invalid")};
  }
  const std::span<const AccessPathCandidate> expected_root_candidates =
      !expected_core_accesses.empty() && expected_core_accesses.front().core_index == 0U
          ? std::span<const AccessPathCandidate>{expected_core_accesses.front().candidates}
          : std::span<const AccessPathCandidate>{};
  const std::size_t expected_selected =
      expected_root_candidates.empty() ? 0U : expected_core_accesses.front().selected_candidate;
  if (!std::ranges::equal(candidates, expected_root_candidates) ||
      selected_candidate_index != expected_selected) {
    return std::unexpected{
        InvariantFailure("advanced compatibility access view does not match its root core")};
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
            if (nonjoined_plans[node.core_index].has_value()) {
              return MatchesNonjoinedAccess(
                  nodes[index],
                  BuildNonjoinedAccessLeaf(AssumePresent(nonjoined_plans[node.core_index])));
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
                         value->root_page == aggregate.source.root_page &&
                         value->key == rowid->key &&
                         value->constraint ==
                             std::optional{PhysicalAccessPredicate{
                                 .predicate = rowid->constraint.predicate,
                                 .conjunct_position = rowid->constraint.conjunct_position,
                                 .origin = rowid->constraint.origin,
                             }};
                }
                if (const auto* range = std::get_if<RowIdRangeCandidatePlan>(&selected.payload);
                    range != nullptr) {
                  PhysicalRowIdRangeNode expected_range = range->node;
                  expected_range.core_index = node.core_index;
                  expected_range.direction = aggregate.direction;
                  const auto* value = std::get_if<PhysicalRowIdRangeNode>(&physical);
                  return value != nullptr && *value == expected_range;
                }
                if (const auto* index_plan = std::get_if<IndexCandidatePlan>(&selected.payload);
                    index_plan != nullptr) {
                  const auto* value = std::get_if<PhysicalIndexScanNode>(&physical);
                  return value != nullptr &&
                         MatchesIndexScan(*value, index_plan->node, node.core_index,
                                          aggregate.direction, index_plan->node.equalities);
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
            if (nonjoined_plans[node.core_index].has_value()) {
              return MatchesNonjoinedAccess(
                  nodes[index],
                  BuildNonjoinedAccessLeaf(AssumePresent(nonjoined_plans[node.core_index])));
            }
            if (joined_plans.empty() || !joined_plans[node.core_index].has_value()) {
              return false;
            }
            const auto expected = BuildJoinedAccessLeaf(*joined_plans[node.core_index],
                                                        logical_plan.bound_select(), node.source);
            return expected.has_value() && MatchesJoinedAccess(nodes[index], *expected);
          } else if constexpr (std::is_same_v<Node, LogicalJoinNode>) {
            const auto* value = std::get_if<PhysicalNestedLoopJoinNode>(&physical);
            const auto* right =
                std::get_if<LogicalScanNode>(&logical_plan.node(node.right).payload);
            if (right == nullptr || joined_plans.empty() ||
                !joined_plans[node.core_index].has_value()) {
              return false;
            }
            const auto placements =
                PublishJoinedPredicates(*joined_plans[node.core_index], right->source);
            return value != nullptr && value->core_index == node.core_index &&
                   value->left.value() == node.left.value() &&
                   value->right.value() == node.right.value() && value->join == node.join &&
                   value->on == node.on && value->predicates == placements;
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
            if (core_index.has_value() && nonjoined_plans[*core_index].has_value()) {
              const NonjoinedCoreAccessAnalysis& access =
                  AssumePresent(nonjoined_plans[*core_index]);
              return value->predicates == access.residuals &&
                     value->guards == access.predicates.guards && value->predicate_sources.empty();
            }
            if (core_index.has_value() && !joined_plans.empty() &&
                joined_plans[*core_index].has_value()) {
              const auto expected = PublishJoinedFilter(*joined_plans[*core_index],
                                                        logical_plan.bound_select(), value->input);
              return expected.has_value() && value->predicates == expected->predicates &&
                     value->guards == expected->guards &&
                     value->predicate_sources == expected->predicate_sources;
            }
            return false;
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
            const std::uint32_t ordered_prefix =
                InputOrderedPrefix(logical_plan.bound_select(), aggregate_plans, joined_plans);
            const PhysicalSortStrategy strategy = logical_plan.bound_select().limit() == nullptr
                                                      ? PhysicalSortStrategy::kExternal
                                                      : PhysicalSortStrategy::kRuntimeLimit;
            if (value == nullptr || value->input.value() != node.input.value() ||
                value->payload_expressions != node.payload_expressions ||
                value->output_fields != node.output_fields || value->schedule != node.schedule ||
                value->strategy != strategy || value->ordered_prefix != ordered_prefix ||
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
            if (native->has_value()) {
              const PhysicalCompoundNode& expected = (**native).compounds[current_compound];
              return value != nullptr && value->left == expected.left &&
                     value->right == expected.right && value->operation == expected.operation &&
                     value->collations == expected.collations &&
                     value->order_by == expected.order_by && value->strategy == expected.strategy;
            }
            if (value == nullptr || value->left.value() != node.left.value() ||
                value->right.value() != node.right.value() || value->operation != node.operation ||
                value->collations != node.collations) {
              return false;
            }
            if (node.operation != CompoundOperator::kUnionAll &&
                !UsesUnionLimitOne(logical_plan.bound_select(), current_compound)) {
              return false;
            }
            const PhysicalCompoundStrategy expected =
                node.operation == CompoundOperator::kUnionAll
                    ? PhysicalCompoundStrategy::kConcatenate
                    : PhysicalCompoundStrategy::kUnionLimitOne;
            return value->strategy == expected;
          } else if constexpr (std::is_same_v<Node, LogicalAdvancedOrderNode>) {
            const auto* value = std::get_if<PhysicalAdvancedOrderNode>(&physical);
            const std::uint32_t ordered_prefix =
                InputOrderedPrefix(logical_plan.bound_select(), aggregate_plans, joined_plans);
            if (value == nullptr || value->input.value() != node.input.value() ||
                value->terms.size() != node.terms.size() ||
                core_orders.size() != node.core_layouts.size() ||
                value->set_then_order != node.set_then_order ||
                value->ordered_prefix != ordered_prefix) {
              return false;
            }
            for (std::size_t term = 0; term < node.terms.size(); ++term) {
              if (!OrderingTermsEqual(value->terms[term], node.terms[term])) {
                return false;
              }
            }
            for (std::size_t core = 0; !native->has_value() && core < node.core_layouts.size();
                 ++core) {
              const bool bounded = ordered_union_all_runtime_limit ||
                                   node.core_layouts[core].schedule ==
                                       OrderEvaluationSchedule::kKeysThenAdmissionThenPayload ||
                                   (ordered_union_limit_one_core.has_value() &&
                                    core >= *ordered_union_limit_one_core);
              const auto& layout = node.core_layouts[core];
              if (ordered_prefix != 0U && ordered_prefix < node.terms.size()) {
                auto expected =
                    BuildPhysicalOrderLayout(logical_plan.bound_select(), layout, ordered_prefix,
                                             logical_plan.bound_select().order_by());
                if (!expected.has_value() || core_orders[core].layout != *expected) {
                  return false;
                }
              } else if (core_orders[core].layout != layout) {
                return false;
              }
              if (core_orders[core].strategy != (bounded ? PhysicalSortStrategy::kRuntimeLimit
                                                         : PhysicalSortStrategy::kExternal)) {
                return false;
              }
              const BoundLimit* limit = logical_plan.bound_select().limit();
              if (core_orders[core].ordered_prefix != ordered_prefix ||
                  !std::ranges::equal(core_orders[core].terms,
                                      logical_plan.bound_select().order_by()) ||
                  core_orders[core].syntax_limit !=
                      (limit == nullptr ? std::nullopt : std::optional{*limit})) {
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
    std::span<const AccessPathCandidate> candidates, std::size_t selected_candidate_index,
    std::span<const PhysicalCoreAccessPlan> core_accesses,
    const NonjoinedCoreAccessAnalysis* root_analysis,
    std::span<const std::optional<AggregateCorePlan>> aggregate_plans = {},
    std::span<const std::optional<NonjoinedCoreAccessAnalysis>> nonjoined_plans = {},
    std::span<const std::optional<JoinedCoreAccessAnalysis>> joined_plans = {},
    std::span<const PhysicalCoreOrderPlan> core_orders = {}) {
  if (!logical_plan.valid()) {
    return std::unexpected{InvariantFailure("physical plan retained an invalid logical plan")};
  }
  if (IsAdvancedLogicalPlan(logical_plan)) {
    return ValidateAdvancedPhysicalPlan(logical_plan, nodes, root, candidates,
                                        selected_candidate_index, core_accesses, aggregate_plans,
                                        nonjoined_plans, joined_plans, core_orders);
  }
  if (nodes.size() < 2U || nodes.size() > 6U || root.value() != nodes.size() - 1U) {
    return std::unexpected{InvariantFailure("physical plan node arena is invalid")};
  }
  if (candidates.empty() || selected_candidate_index >= candidates.size() ||
      AccessKindOf(nodes.front()) != candidates[selected_candidate_index].kind) {
    return std::unexpected{InvariantFailure("physical access candidates are invalid")};
  }
  if (core_accesses.size() != 1U || core_accesses.front().core_index != 0U ||
      core_accesses.front().selected_candidate != selected_candidate_index ||
      !std::ranges::equal(core_accesses.front().candidates, candidates)) {
    return std::unexpected{InvariantFailure("physical core access publication is invalid")};
  }

  const BoundQueryView& bound_select = logical_plan.bound_select();
  if (root_analysis == nullptr || root_analysis->selected_access != selected_candidate_index ||
      root_analysis->access_candidates.size() != candidates.size() ||
      core_accesses.front().search_limited != root_analysis->search_limited ||
      !core_accesses.front().source_order.empty()) {
    return std::unexpected(InvariantFailure("physical validation has no matching core analysis"));
  }
  const SourceInfo& source = root_analysis->source;
  const PhysicalNode& leaf = nodes.front();
  const auto expected_candidates =
      root_analysis->access_candidates |
      std::views::transform([](const ReadAccessCandidate& loop) { return loop.published; });
  bool matches_leaf = false;
  const auto* index_leaf = std::get_if<PhysicalIndexScanNode>(&leaf.payload);
  const auto* index_expected = std::get_if<IndexCandidatePlan>(
      &root_analysis->access_candidates[root_analysis->selected_access].payload);
  if (index_leaf != nullptr && index_expected != nullptr) {
    matches_leaf = MatchesIndexScan(*index_leaf, index_expected->node, root_analysis->core_index,
                                    root_analysis->direction, root_analysis->published_equalities);
  } else {
    matches_leaf = MatchesNonjoinedAccess(leaf, BuildNonjoinedAccessLeaf(*root_analysis));
  }
  if (!std::ranges::equal(candidates, expected_candidates) || !matches_leaf) {
    return std::unexpected(InvariantFailure("physical leaf differs from selected native loop"));
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
        sort->schedule != logical_order->schedule || sort->strategy != expected_strategy ||
        sort->ordered_prefix != root_analysis->ordered_prefix) {
      return std::unexpected{InvariantFailure("physical sort does not match logical ordering")};
    }
    if (sort->ordered_prefix != 0U && !sort->input_order_satisfied()) {
      auto expected =
          BuildPhysicalSortNode(*logical_order, input, bound_select, sort->ordered_prefix);
      if (!expected.has_value()) {
        return std::unexpected(std::move(expected.error()));
      }
      if (sort->payload_expressions != expected->payload_expressions ||
          sort->output_fields != expected->output_fields) {
        return std::unexpected(InvariantFailure("partial order payload mapping is invalid"));
      }
    } else if (sort->payload_expressions != logical_order->payload_expressions ||
               sort->output_fields != logical_order->output_fields) {
      return std::unexpected(InvariantFailure("physical sort output mapping is invalid"));
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
  if (!std::ranges::equal(guard_predicates, root_analysis->published_guards) ||
      !std::ranges::equal(filter_predicates, root_analysis->published_residuals)) {
    return std::unexpected(InvariantFailure("physical predicates differ from native term coding"));
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
  Impl(LogicalPlan input, OptimizerOptions configuration) noexcept
      : logical_plan(std::move(input)), options(configuration) {}

  LogicalPlan logical_plan;
  OptimizerOptions options;
  PhysicalQueryPlan root_query;
  std::vector<PhysicalQueryPlan> nested_queries;
};

struct PhysicalMutationPlan::Impl {
  Impl(LogicalMutationPlan input, PhysicalMutationPayload mutation) noexcept
      : logical_plan(std::move(input)), payload(std::move(mutation)) {}

  LogicalMutationPlan logical_plan;
  PhysicalMutationPayload payload;
};

[[nodiscard]] std::span<const PhysicalCoreAccessPlan> CoreAccessPlans(
    const PhysicalQueryPlan& plan) noexcept {
  if (!plan.core_accesses.empty()) {
    return plan.core_accesses;
  }
  return plan.root_core_access.has_value()
             ? std::span<const PhysicalCoreAccessPlan>{&*plan.root_core_access, 1U}
             : std::span<const PhysicalCoreAccessPlan>{};
}

namespace optimizer_detail {

class PhysicalPlanBuilder final {
 public:
  [[nodiscard]] static OptimizeLogicalPlanResult Build(LogicalPlan logical_plan,
                                                       OptimizerOptions options) {
    if (!logical_plan.valid()) {
      return std::unexpected{
          OptimizerFailure(OptimizerErrorCode::kInvalidInput, "logical plan is invalid")};
    }
    auto impl = std::make_unique<PhysicalPlan::Impl>(std::move(logical_plan), options);
    const std::size_t count = impl->logical_plan.bound_select().query_count();
    impl->nested_queries.resize(count - 1U);
    std::vector<std::optional<LogEst>> invocation_estimates;
    if (count > 1U) {
      invocation_estimates.resize(count);
    }
    for (std::size_t index = 0; index < count; ++index) {
      const BoundQueryId id{static_cast<std::uint32_t>(index)};
      PhysicalQueryPlan& query = index == 0U ? impl->root_query : impl->nested_queries[index - 1U];
      query.query = id;
      const auto& declaration = impl->logical_plan.bound_select().query(id);
      if (declaration.parent.has_value() && declaration.parent->value() >= index) {
        return std::unexpected(InvariantFailure("query parent is not planned before its child"));
      }
      // Unemitted binding-only owners retain the native initial context.
      const LogEst outer_loops =
          invocation_estimates.empty() ? 0 : invocation_estimates[index].value_or(0);
      if (auto built = BuildQuery(LogicalQueryView{impl->logical_plan, id}, &query, impl->options,
                                  outer_loops, invocation_estimates);
          !built.has_value()) {
        return std::unexpected(std::move(built.error()));
      }
    }
    return PhysicalPlan{std::move(impl)};
  }

 private:
  [[nodiscard]] static bool RecordExpressionLoops(const BoundQueryView& bound,
                                                  BoundExpressionId expression, LogEst loops,
                                                  std::span<std::optional<LogEst>> invocations) {
    if (invocations.empty()) {
      return true;
    }
    const auto query = [&](BoundQueryId id) -> std::optional<std::span<const BoundQueryCapture>> {
      if (id.value() >= invocations.size()) {
        return std::nullopt;
      }
      if (bound.owner().query(id).parent == bound.query().id &&
          !invocations[id.value()].has_value()) {
        invocations[id.value()] = loops;
      }
      return std::span<const BoundQueryCapture>{};
    };
    auto dependency = [](const BoundQueryCapture&) { return true; };
    return binder_detail::VisitQueryDependencies(bound.expressions(), bound.source_columns(),
                                                 expression, query, BoundSourceDependency::kValue,
                                                 dependency);
  }

  [[nodiscard]] static std::expected<void, OptimizerError> RecordCoreLoops(
      const BoundQueryView& bound, std::size_t core_index,
      std::optional<PhysicalAggregateStrategy> strategy, LogEst outer_loops, LogEst output,
      std::span<std::optional<LogEst>> invocations) {
    if (invocations.empty()) {
      return {};
    }
    const auto record = [&](BoundExpressionId expression, LogEst loops) {
      return RecordExpressionLoops(bound, expression, loops, invocations);
    };
    bool valid = true;
    if (const auto* core = std::get_if<BoundSelectCore>(&bound.query_cores()[core_index]);
        core != nullptr) {
      const LogEst active = outer_loops + output;
      if (core->where.has_value()) {
        valid &= record(*core->where, active);
      }
      for (const auto& source : core->table_sources) {
        if (source.on.has_value()) {
          valid &= record(*source.on, active);
        }
      }
      if (core->aggregate.has_value()) {
        const auto& aggregate = *core->aggregate;
        for (const auto& term : aggregate.group_by) {
          valid &= record(term.expression, active);
        }
        const LogEst input =
            strategy == PhysicalAggregateStrategy::kSortedGroup ? outer_loops : active;
        for (const auto id : aggregate.invocations) {
          const auto& invocation = bound.aggregate_invocations()[id.value()];
          if (invocation.filter.has_value()) {
            valid &= record(*invocation.filter, input);
          }
          for (const auto& term : invocation.order_by) {
            valid &= record(term.expression, input);
          }
          for (const auto argument : invocation.arguments) {
            valid &= record(argument, input);
          }
        }
        if (aggregate.having.has_value()) {
          valid &= record(*aggregate.having, outer_loops);
        }
      }
      const LogEst result = !BoundQueryRequiresResultValues(bound.query(), bound.expressions())
                                ? 0
                                : (core->aggregate.has_value() ? outer_loops : active);
      for (const auto& column : core->result_columns) {
        valid &= record(column.expression, result);
      }
      for (const auto& term : bound.order_by()) {
        valid &= record(term.expression, result);
      }
    } else {
      const auto& values = std::get<BoundValuesCore>(bound.query_cores()[core_index]);
      for (const auto& row : values.rows) {
        for (const auto expression : row) {
          valid &= record(expression, outer_loops);
        }
      }
    }
    if (!valid) {
      return std::unexpected(InvariantFailure("nested query context has invalid dependencies"));
    }
    return {};
  }

  [[nodiscard]] static std::expected<void, OptimizerError> BuildQuery(
      const LogicalQueryView& logical_plan, PhysicalQueryPlan* impl, OptimizerOptions options,
      LogEst outer_loops, std::span<std::optional<LogEst>> invocations) {
    if (const auto* limit = logical_plan.bound_select().limit();
        limit != nullptr && (!RecordExpressionLoops(logical_plan.bound_select(), limit->limit,
                                                    outer_loops, invocations) ||
                             (limit->offset.has_value() &&
                              !RecordExpressionLoops(logical_plan.bound_select(), *limit->offset,
                                                     outer_loops, invocations)))) {
      return std::unexpected(InvariantFailure("query limit has invalid nested dependencies"));
    }
    if (IsAdvancedLogicalPlan(logical_plan)) {
      return BuildAdvanced(logical_plan, impl, options, outer_loops, invocations);
    }

    auto analyzed =
        AnalyzeNonjoinedCoreAccess(logical_plan.bound_select(), 0U, options, outer_loops);
    if (!analyzed.has_value()) {
      return std::unexpected(std::move(analyzed.error()));
    }
    NonjoinedCoreAccessAnalysis access = std::move(*analyzed);
    if (auto recorded = RecordCoreLoops(logical_plan.bound_select(), 0U, std::nullopt, outer_loops,
                                        access.output_log_est, invocations);
        !recorded.has_value()) {
      return recorded;
    }
    ReadPredicateAnalysis& predicates = access.predicates;
    std::vector<BoundExpressionId>& residuals = access.residuals;
    access.published_guards = predicates.guards;
    access.published_residuals = residuals;

    const bool ordered = !logical_plan.bound_select().order_by().empty();
    const std::size_t node_count =
        2U + static_cast<std::size_t>(!predicates.guards.empty()) +
        static_cast<std::size_t>(!residuals.empty()) +
        static_cast<std::size_t>(logical_plan.bound_select().limit() != nullptr) +
        static_cast<std::size_t>(ordered);
    impl->nodes.reserve(node_count);

    impl->root_core_access.emplace(PublishCoreAccessPlan(access));
    auto* index =
        std::get_if<IndexCandidatePlan>(&access.access_candidates[access.selected_access].payload);
    if (index != nullptr) {
      access.published_equalities = index->node.equalities;
      index->node.direction = access.direction;
      impl->nodes.push_back(PhysicalNode{.payload = std::move(index->node)});
    } else {
      impl->nodes.push_back(PhysicalNode{.payload = BuildNonjoinedAccessLeaf(access)});
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
      auto physical_order = BuildPhysicalSortNode(
          *logical_order, input, logical_plan.bound_select(), access.ordered_prefix);
      if (!physical_order.has_value()) {
        return std::unexpected(std::move(physical_order.error()));
      }
      impl->nodes.push_back(PhysicalNode{.payload = std::move(*physical_order)});
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
    const PhysicalCoreAccessPlan& root_access = AssumePresent(impl->root_core_access);
    const std::span<const AccessPathCandidate> candidates{root_access.candidates};
    const std::span<const PhysicalCoreAccessPlan> core_accesses = CoreAccessPlans(*impl);
    if (std::expected<void, OptimizerError> validated =
            ValidatePhysicalPlan(logical_plan, impl->nodes, impl->root, candidates,
                                 root_access.selected_candidate, core_accesses, &access);
        !validated.has_value()) {
      return std::unexpected{std::move(validated.error())};
    }
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPlannerWork,
                                 1U + root_access.candidates.size());
    return {};
  }

  [[nodiscard]] static std::expected<void, OptimizerError> BuildAdvanced(
      const LogicalQueryView& logical_plan, PhysicalQueryPlan* impl, OptimizerOptions options,
      LogEst outer_loops, std::span<std::optional<LogEst>> invocations) {
    auto native = NativeCompoundPlan::Build(logical_plan);
    if (!native.has_value()) {
      return std::unexpected(std::move(native.error()));
    }
    impl->nodes.reserve(logical_plan.nodes().size());
    std::vector<PhysicalNodeId> mapped;
    mapped.reserve(logical_plan.nodes().size());
    std::vector<std::optional<AggregateCorePlan>> aggregate_plans(
        logical_plan.bound_select().query_cores().size());
    std::vector<std::optional<NonjoinedCoreAccessAnalysis>> nonjoined_plans(
        logical_plan.bound_select().query_cores().size());
    std::vector<std::optional<JoinedCoreAccessAnalysis>> joined_plans;
    if (std::ranges::any_of(logical_plan.bound_select().query_cores(), [](const auto& core) {
          const auto* select = std::get_if<BoundSelectCore>(&core);
          return select != nullptr && select->table_sources.size() > 1U;
        })) {
      joined_plans.resize(aggregate_plans.size());
    }
    for (std::size_t core = 0; core < aggregate_plans.size(); ++core) {
      const auto* select =
          std::get_if<BoundSelectCore>(&logical_plan.bound_select().query_cores()[core]);
      std::optional<CoreOrderRequest> order;
      if (native->has_value()) {
        const PhysicalCoreOrderPlan& requested = (**native).core_orders[core];
        order = CoreOrderRequest{
            .terms = requested.terms,
            .syntax_limit = requested.syntax_limit.has_value() ? &*requested.syntax_limit : nullptr,
            .block_producer = select != nullptr && !select->aggregate.has_value() &&
                              select->quantifier != SelectQuantifier::kDistinct,
        };
      }
      if (select != nullptr && select->aggregate.has_value()) {
        auto aggregate =
            BuildAggregateCorePlan(logical_plan.bound_select(), core, options, outer_loops, order);
        if (!aggregate.has_value()) {
          return std::unexpected(std::move(aggregate.error()));
        }
        aggregate_plans[core] = std::move(*aggregate);
      } else if (select != nullptr && select->table_sources.size() <= 1U) {
        auto access = AnalyzeNonjoinedCoreAccess(logical_plan.bound_select(), core, options,
                                                 outer_loops, order);
        if (!access.has_value()) {
          return std::unexpected(std::move(access.error()));
        }
        nonjoined_plans[core] = std::move(*access);
      }
      if (select != nullptr && select->table_sources.size() > 1U) {
        auto joined =
            AnalyzeJoinedCoreAccess(logical_plan.bound_select(), core, options, outer_loops, order);
        if (!joined.has_value()) {
          return std::unexpected(std::move(joined.error()));
        }
        joined_plans[core] = std::move(*joined);
      }
      LogEst output = 0;
      if (!joined_plans.empty() && joined_plans[core].has_value()) {
        output = joined_plans[core]->plan.output_log_est;
      } else if (aggregate_plans[core].has_value()) {
        output = aggregate_plans[core]->output_log_est;
      } else if (nonjoined_plans[core].has_value()) {
        output = nonjoined_plans[core]->output_log_est;
      }
      if (auto recorded = RecordCoreLoops(logical_plan.bound_select(), core,
                                          aggregate_plans[core].has_value()
                                              ? std::optional{aggregate_plans[core]->strategy}
                                              : std::nullopt,
                                          outer_loops, output, invocations);
          !recorded.has_value()) {
        return recorded;
      }
    }

    if (native->has_value()) {
      if (auto qualified = QualifyNativeCoreOrders(**native, logical_plan.bound_select(),
                                                   aggregate_plans, nonjoined_plans, joined_plans);
          !qualified.has_value()) {
        return qualified;
      }
      impl->core_orders = std::move((**native).core_orders);
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
      std::vector<PhysicalJoinPredicatePlacement> join_predicates;
      if (const auto* join =
              std::get_if<LogicalJoinNode>(&logical_plan.nodes()[logical_index].payload);
          join != nullptr) {
        const auto* right = std::get_if<LogicalScanNode>(&logical_plan.node(join->right).payload);
        if (right == nullptr || joined_plans.empty() ||
            !joined_plans[join->core_index].has_value()) {
          return std::unexpected(InvariantFailure("join has no selected source analysis"));
        }
        join_predicates = PublishJoinedPredicates(*joined_plans[join->core_index], right->source);
      }
      std::optional<PhysicalFilterNode> joined_filter;
      if (const auto* filter =
              std::get_if<LogicalFilterNode>(&logical_plan.nodes()[logical_index].payload);
          filter != nullptr) {
        const auto* join = std::get_if<LogicalJoinNode>(&logical_plan.node(filter->input).payload);
        if (join != nullptr) {
          auto published =
              PublishJoinedFilter(*joined_plans[join->core_index], logical_plan.bound_select(),
                                  mapped[filter->input.value()]);
          if (!published.has_value()) {
            return std::unexpected(std::move(published.error()));
          }
          joined_filter = std::move(*published);
        }
      }
      const LogicalNode& logical = logical_plan.nodes()[logical_index];
      std::optional<PhysicalNodePayload> joined_access;
      if (const auto* scan = std::get_if<LogicalScanNode>(&logical.payload);
          scan != nullptr && !nonjoined_plans[scan->core_index].has_value() &&
          (!aggregate_plans[scan->core_index].has_value() ||
           AssumePresent(aggregate_plans[scan->core_index]).joined)) {
        if (joined_plans.empty() || !joined_plans[scan->core_index].has_value()) {
          return std::unexpected(InvariantFailure("joined scan has no core access analysis"));
        }
        auto selected = BuildJoinedAccessLeaf(*joined_plans[scan->core_index],
                                              logical_plan.bound_select(), scan->source);
        if (!selected.has_value()) {
          return std::unexpected(std::move(selected.error()));
        }
        joined_access.emplace(std::move(*selected));
      }
      std::optional<PhysicalSortNode> ordered_node;
      const auto ordered_prefix =
          InputOrderedPrefix(logical_plan.bound_select(), aggregate_plans, joined_plans);
      if (const auto* order = std::get_if<LogicalOrderNode>(&logical.payload); order != nullptr) {
        auto selected = BuildPhysicalSortNode(*order, mapped[order->input.value()],
                                              logical_plan.bound_select(), ordered_prefix);
        if (!selected.has_value()) {
          return std::unexpected(std::move(selected.error()));
        }
        ordered_node.emplace(std::move(*selected));
      } else if (const auto* advanced = std::get_if<LogicalAdvancedOrderNode>(&logical.payload);
                 advanced != nullptr && !native->has_value()) {
        impl->core_orders.reserve(advanced->core_layouts.size());
        for (std::size_t core = 0; core < advanced->core_layouts.size(); ++core) {
          const auto& layout = advanced->core_layouts[core];
          auto selected =
              BuildPhysicalOrderLayout(logical_plan.bound_select(), layout, ordered_prefix,
                                       logical_plan.bound_select().order_by());
          if (!selected.has_value()) {
            return std::unexpected(std::move(selected.error()));
          }
          const bool bounded =
              ordered_union_all_runtime_limit ||
              layout.schedule == OrderEvaluationSchedule::kKeysThenAdmissionThenPayload ||
              (ordered_union_limit_one_core.has_value() && core >= *ordered_union_limit_one_core);
          impl->core_orders.push_back(PhysicalCoreOrderPlan{
              .layout = std::move(*selected),
              .terms = {logical_plan.bound_select().order_by().begin(),
                        logical_plan.bound_select().order_by().end()},
              .syntax_limit = logical_plan.bound_select().limit() == nullptr
                                  ? std::nullopt
                                  : std::optional{*logical_plan.bound_select().limit()},
              .strategy =
                  bounded ? PhysicalSortStrategy::kRuntimeLimit : PhysicalSortStrategy::kExternal,
              .ordered_prefix = ordered_prefix,
          });
        }
      }
      PhysicalNodePayload payload = std::visit(
          [&](const auto& node) -> PhysicalNodePayload {
            using Node = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<Node, LogicalSingleRowNode>) {
              if (aggregate_plans[node.core_index].has_value() &&
                  aggregate_plans[node.core_index]->predicates.empty) {
                return PhysicalEmptyNode{.core_index = node.core_index};
              }
              if (nonjoined_plans[node.core_index].has_value()) {
                return BuildNonjoinedAccessLeaf(AssumePresent(nonjoined_plans[node.core_index]));
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
                        .constraint =
                            PhysicalAccessPredicate{
                                .predicate = rowid->constraint.predicate,
                                .conjunct_position = rowid->constraint.conjunct_position,
                                .origin = rowid->constraint.origin,
                            },
                    };
                  }
                  if (const auto* range = std::get_if<RowIdRangeCandidatePlan>(&selected.payload);
                      range != nullptr) {
                    PhysicalRowIdRangeNode scan = range->node;
                    scan.core_index = node.core_index;
                    scan.direction = aggregate.direction;
                    return scan;
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
              if (nonjoined_plans[node.core_index].has_value()) {
                return BuildNonjoinedAccessLeaf(AssumePresent(nonjoined_plans[node.core_index]));
              }
              if (!joined_access.has_value()) {
                std::terminate();
              }
              return std::move(*joined_access);
            } else if constexpr (std::is_same_v<Node, LogicalJoinNode>) {
              return PhysicalNestedLoopJoinNode{
                  .left = mapped[node.left.value()],
                  .right = mapped[node.right.value()],
                  .core_index = node.core_index,
                  .join = node.join,
                  .on = node.on,
                  .predicates = std::move(join_predicates),
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
              if (core_index.has_value() && nonjoined_plans[*core_index].has_value()) {
                const NonjoinedCoreAccessAnalysis& access =
                    AssumePresent(nonjoined_plans[*core_index]);
                return PhysicalFilterNode{
                    .input = mapped[node.input.value()],
                    .predicates = access.residuals,
                    .guards = access.predicates.guards,
                };
              }
              if (joined_filter.has_value()) {
                return std::move(*joined_filter);
              }
              std::terminate();
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
              return std::move(AssumePresent(ordered_node));
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
              if (native->has_value()) {
                return std::move((**native).compounds[current_compound]);
              }
              const PhysicalCompoundStrategy strategy =
                  node.operation == CompoundOperator::kUnionAll
                      ? PhysicalCompoundStrategy::kConcatenate
                      : PhysicalCompoundStrategy::kUnionLimitOne;
              return PhysicalCompoundNode{
                  .left = mapped[node.left.value()],
                  .right = mapped[node.right.value()],
                  .operation = node.operation,
                  .collations = node.collations,
                  .strategy = strategy,
              };
            } else if constexpr (std::is_same_v<Node, LogicalAdvancedOrderNode>) {
              return PhysicalAdvancedOrderNode{
                  .input = mapped[node.input.value()],
                  .terms = node.terms,
                  .set_then_order = node.set_then_order,
                  .final_strategy =
                      node.set_then_order && logical_plan.bound_select().limit() != nullptr
                          ? PhysicalSortStrategy::kRuntimeLimit
                          : PhysicalSortStrategy::kExternal,
                  .ordered_prefix = ordered_prefix,
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
    std::optional<PhysicalCoreAccessPlan> single_access;
    std::vector<PhysicalCoreAccessPlan> multiple_accesses;
    const auto append_access = [&](PhysicalCoreAccessPlan access) {
      if (!single_access.has_value() && multiple_accesses.empty()) {
        single_access.emplace(std::move(access));
        return;
      }
      if (multiple_accesses.empty()) {
        multiple_accesses.reserve(nonjoined_plans.size());
        multiple_accesses.push_back(std::move(*single_access));
        single_access.reset();
      }
      multiple_accesses.push_back(std::move(access));
    };
    for (std::size_t core = 0; core < nonjoined_plans.size(); ++core) {
      if (nonjoined_plans[core].has_value()) {
        append_access(PublishCoreAccessPlan(AssumePresent(nonjoined_plans[core])));
      } else if (aggregate_plans[core].has_value() &&
                 !AssumePresent(aggregate_plans[core]).joined) {
        const AggregateCorePlan& aggregate = AssumePresent(aggregate_plans[core]);
        PhysicalCoreAccessPlan published{
            .core_index = core,
            .candidates = {},
            .selected_candidate = aggregate.selected_access.value_or(0U),
            .search_limited = aggregate.search_limited,
        };
        published.candidates.reserve(aggregate.access_candidates.size());
        for (const ReadAccessCandidate& candidate : aggregate.access_candidates) {
          published.candidates.push_back(candidate.published);
        }
        append_access(std::move(published));
      } else if (!joined_plans.empty() && joined_plans[core].has_value()) {
        append_access(
            PublishJoinedCoreAccessPlan(*joined_plans[core], logical_plan.bound_select()));
      }
    }
    if (multiple_accesses.empty()) {
      impl->root_core_access = std::move(single_access);
    } else {
      impl->core_accesses = std::move(multiple_accesses);
    }
    const std::span<const PhysicalCoreAccessPlan> core_accesses = CoreAccessPlans(*impl);
    std::span<const AccessPathCandidate> candidates;
    std::size_t selected_candidate = 0;
    if (!core_accesses.empty() && core_accesses.front().core_index == 0U) {
      candidates = core_accesses.front().candidates;
      selected_candidate = core_accesses.front().selected_candidate;
    }
    std::expected<void, OptimizerError> validated = ValidatePhysicalPlan(
        logical_plan, impl->nodes, impl->root, candidates, selected_candidate, core_accesses,
        nullptr, aggregate_plans, nonjoined_plans, joined_plans, impl->core_orders);
    if (!validated.has_value()) {
      return std::unexpected{std::move(validated.error())};
    }
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPlannerWork, impl->nodes.size());
    return {};
  }
};

class PhysicalStatementPlanBuilder final {
 public:
  [[nodiscard]] static OptimizeLogicalStatementPlanResult Build(LogicalStatementPlan logical_plan,
                                                                OptimizerOptions options) {
    if (!LogicalStatementIsValid(logical_plan)) {
      return std::unexpected{
          OptimizerFailure(OptimizerErrorCode::kInvalidInput, "logical statement plan is invalid")};
    }
    if (std::holds_alternative<LogicalPlan>(logical_plan)) {
      OptimizeLogicalPlanResult select =
          OptimizeLogicalPlan(std::get<LogicalPlan>(std::move(logical_plan)), options);
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
    case PhysicalAccessKind::kRowIdRange:
      return "rowid_range";
    case PhysicalAccessKind::kIndexScan:
      return "index_scan";
    case PhysicalAccessKind::kAutomaticIndex:
      return "automatic_index";
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
    case PhysicalNodeKind::kRowIdRange:
      return "rowid_range";
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
    case PhysicalNodeKind::kAutomaticIndex:
      return "automatic_index";
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

OptimizerOptions PhysicalPlan::options() const noexcept { return impl_->options; }

std::span<const PhysicalNode> PhysicalPlan::nodes() const noexcept {
  return impl_ != nullptr ? std::span<const PhysicalNode>{impl_->root_query.nodes}
                          : std::span<const PhysicalNode>{};
}

const PhysicalNode& PhysicalPlan::node(PhysicalNodeId id) const noexcept {
  return impl_->root_query.nodes[id.value()];
}

PhysicalNodeId PhysicalPlan::root() const noexcept { return impl_->root_query.root; }

std::span<const AccessPathCandidate> PhysicalPlan::candidates() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  const PhysicalQueryPlan& plan = impl_->root_query;
  const PhysicalCoreAccessPlan* root_access =
      !plan.core_accesses.empty()
          ? &plan.core_accesses.front()
          : (plan.root_core_access.has_value() ? &*plan.root_core_access : nullptr);
  return root_access != nullptr && root_access->core_index == 0U
             ? std::span<const AccessPathCandidate>{root_access->candidates}
             : std::span<const AccessPathCandidate>{};
}

std::optional<std::size_t> PhysicalPlan::selected_candidate_index() const noexcept {
  const auto candidates_view = candidates();
  const auto plans = core_access_plans();
  if (candidates_view.empty() || plans.empty() ||
      plans.front().selected_candidate >= candidates_view.size()) {
    return std::nullopt;
  }
  return plans.front().selected_candidate;
}

const AccessPathCandidate* PhysicalPlan::selected_candidate() const noexcept {
  const auto selected = selected_candidate_index();
  if (!selected.has_value()) {
    return nullptr;
  }
  return &candidates()[*selected];
}

std::span<const PhysicalCoreAccessPlan> PhysicalPlan::core_access_plans() const noexcept {
  return impl_ != nullptr ? CoreAccessPlans(impl_->root_query)
                          : std::span<const PhysicalCoreAccessPlan>{};
}

std::span<const PhysicalCoreOrderPlan> PhysicalPlan::core_order_plans() const noexcept {
  return impl_ != nullptr ? std::span<const PhysicalCoreOrderPlan>{impl_->root_query.core_orders}
                          : std::span<const PhysicalCoreOrderPlan>{};
}

const PhysicalQueryPlan& PhysicalPlan::query_plan(BoundQueryId id) const noexcept {
  if (impl_ == nullptr || id.value() >= impl_->logical_plan.bound_select().query_count()) {
    std::terminate();
  }
  return id.value() == 0U ? impl_->root_query : impl_->nested_queries[id.value() - 1U];
}

OptimizeLogicalPlanResult OptimizeLogicalPlan(LogicalPlan logical_plan, OptimizerOptions options) {
  return optimizer_detail::PhysicalPlanBuilder::Build(std::move(logical_plan), options);
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

OptimizeLogicalStatementPlanResult OptimizeLogicalStatementPlan(LogicalStatementPlan logical_plan,
                                                                OptimizerOptions options) {
  return optimizer_detail::PhysicalStatementPlanBuilder::Build(std::move(logical_plan), options);
}

namespace {

[[nodiscard]] std::string ExplainPhysicalQueryNodes(const PhysicalQueryView& plan) {
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
        const auto query_cores = plan.logical_plan().bound_select().query_cores();
        const auto& core = std::get<BoundSelectCore>(query_cores[scan->core_index]);
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
      } else if (const auto* range = std::get_if<PhysicalRowIdRangeNode>(&node.payload);
                 range != nullptr) {
        std::string line = "SEARCH ";
        AppendExplainIdentifier(SourceName(plan, range->source_kind, range->table), &line);
        line.append(" USING INTEGER PRIMARY KEY (");
        if (range->lower.has_value()) {
          line.append(range->lower->inclusive ? "rowid>=?" : "rowid>?");
        }
        if (range->lower.has_value() && range->upper.has_value()) {
          line.append(" AND ");
        }
        if (range->upper.has_value()) {
          line.append(range->upper->inclusive ? "rowid<=?" : "rowid<?");
        }
        line.push_back(')');
        if (range->direction == PhysicalScanDirection::kReverse) {
          line.append(" REVERSE");
        }
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
        if (index_scan->two_pass_null_order) {
          line.append(kTwoPassNullOrderExplanation);
        }
        append_line(line);
      } else if (const auto* automatic = std::get_if<PhysicalAutomaticIndexNode>(&node.payload);
                 automatic != nullptr) {
        const CatalogTable& table =
            plan.logical_plan().bound_select().catalog()->table(automatic->table);
        std::string line = "CORE " + std::to_string(automatic->core_index) + " SEARCH ";
        AppendExplainIdentifier(table.name, &line);
        line.append(" SOURCE ").append(std::to_string(automatic->source.value()));
        line.append(automatic->construction_predicates.empty()
                        ? " USING AUTOMATIC COVERING INDEX"
                        : " USING AUTOMATIC PARTIAL COVERING INDEX");
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
        if (sort->input_order_satisfied()) {
          append_line("ORDER USING INPUT ORDER");
        } else if (sort->ordered_prefix != 0U) {
          append_line("ORDER USING INPUT PREFIX " + std::to_string(sort->ordered_prefix));
        } else {
          append_line("ORDER USING SORTER");
        }
      } else if (const auto* distinct = std::get_if<PhysicalDistinctNode>(&node.payload);
                 distinct != nullptr) {
        append_line("DISTINCT CORE " + std::to_string(distinct->core_index) +
                    " USING EPHEMERAL RELATION");
      } else if (const auto* compound = std::get_if<PhysicalCompoundNode>(&node.payload);
                 compound != nullptr) {
        std::string strategy;
        switch (compound->strategy) {
          case PhysicalCompoundStrategy::kConcatenate:
            strategy = "CONCATENATE";
            break;
          case PhysicalCompoundStrategy::kUnionLimitOne:
            strategy = "UNION LIMIT ONE";
            break;
          case PhysicalCompoundStrategy::kOrderedMerge:
            strategy = "ORDERED MERGE";
            break;
        }
        append_line("COMPOUND USING " + strategy);
      } else if (const auto* order = std::get_if<PhysicalAdvancedOrderNode>(&node.payload);
                 order != nullptr) {
        if (order->input_order_satisfied()) {
          append_line("ORDER USING INPUT ORDER");
        } else if (order->ordered_prefix != 0U) {
          append_line("ORDER USING INPUT PREFIX " + std::to_string(order->ordered_prefix));
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
  } else if (const auto* range = std::get_if<PhysicalRowIdRangeNode>(&access.payload);
             range != nullptr) {
    explain = "SEARCH ";
    AppendExplainIdentifier(SourceName(plan, range->source_kind, range->table), &explain);
    explain.append(" USING INTEGER PRIMARY KEY (");
    if (range->lower.has_value()) {
      explain.append(range->lower->inclusive ? "rowid>=?" : "rowid>?");
    }
    if (range->lower.has_value() && range->upper.has_value()) {
      explain.append(" AND ");
    }
    if (range->upper.has_value()) {
      explain.append(range->upper->inclusive ? "rowid<=?" : "rowid<?");
    }
    explain.push_back(')');
    if (range->direction == PhysicalScanDirection::kReverse) {
      explain.append(" REVERSE");
    }
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
    if (index_scan->two_pass_null_order) {
      explain.append(kTwoPassNullOrderExplanation);
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
    if (sort->input_order_satisfied()) {
      explain.append(") USING INPUT ORDER");
    } else if (sort->ordered_prefix != 0U) {
      explain.append(") USING INPUT PREFIX ").append(std::to_string(sort->ordered_prefix));
    } else {
      explain.append(") USING SORTER");
    }
    break;
  }
  return explain;
}

[[nodiscard]] std::string ExplainPhysicalQuery(const PhysicalQueryView& plan) {
  std::string explain = ExplainPhysicalQueryNodes(plan);
  const auto cores = plan.logical_plan().bound_select().query_cores();
  for (const PhysicalCoreAccessPlan& access : plan.core_access_plans()) {
    if (access.search_limited) {
      explain.append("\nCORE ").append(std::to_string(access.core_index)).append(" SEARCH LIMITED");
    }
    if (access.ordered_inner_loop) {
      explain.append("\nCORE ")
          .append(std::to_string(access.core_index))
          .append(" ORDERED INNER LOOP");
    }
    if (access.source_order.empty()) {
      continue;
    }
    const auto& core = std::get<BoundSelectCore>(cores[access.core_index]);
    const bool declared_order = std::ranges::equal(
        access.source_order,
        core.table_sources |
            std::views::transform([](const BoundTableSource& source) { return source.id; }));
    if (!declared_order) {
      explain.append("\nCORE ").append(std::to_string(access.core_index)).append(" LOOP ORDER");
      for (const BoundSourceId source : access.source_order) {
        explain.push_back(' ');
        explain.append(std::to_string(source.value()));
      }
    }
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
