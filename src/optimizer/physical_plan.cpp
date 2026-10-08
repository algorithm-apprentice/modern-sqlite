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
[[nodiscard]] bool IsNullLiteral(const BoundSelect& bound_select, BoundExpressionId id) noexcept;

[[nodiscard]] OptimizerError OptimizerFailure(OptimizerErrorCode code, std::string_view detail) {
  return OptimizerError{.code = code, .detail = std::string{detail}};
}

[[nodiscard]] OptimizerError InvariantFailure(std::string_view detail) {
  return OptimizerFailure(OptimizerErrorCode::kInternalInvariant, detail);
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
[[nodiscard]] bool ExpressionDependsOnSource(const Bound& bound_select, BoundExpressionId id) {
  if (!IsValidExpressionId(bound_select, id)) {
    return true;
  }
  const BoundExpression& expression = bound_select.expression(id);
  return std::visit(
      [&](const auto& payload) -> bool {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BoundLiteralExpression> ||
                      std::is_same_v<Payload, BoundParameterExpression>) {
          return false;
        } else if constexpr (std::is_same_v<Payload, BoundColumnExpression> ||
                             std::is_same_v<Payload, BoundRowIdExpression>) {
          return true;
        } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                             std::is_same_v<Payload, BoundTruthTestExpression> ||
                             std::is_same_v<Payload, BoundLikelihoodExpression> ||
                             std::is_same_v<Payload, BoundCollateExpression>) {
          return ExpressionDependsOnSource(bound_select, payload.operand);
        } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression> ||
                             std::is_same_v<Payload, BoundComparisonExpression>) {
          return ExpressionDependsOnSource(bound_select, payload.left) ||
                 ExpressionDependsOnSource(bound_select, payload.right);
        } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
          return ExpressionDependsOnSource(bound_select, payload.target);
        } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression> ||
                             std::is_same_v<Payload, BoundCoalesceExpression> ||
                             std::is_same_v<Payload, BoundConditionalExpression>) {
          return std::ranges::any_of(payload.arguments, [&](BoundExpressionId argument) {
            return ExpressionDependsOnSource(bound_select, argument);
          });
        }
        return true;
      },
      expression.payload);
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
  if (std::holds_alternative<BoundRowIdExpression>(expression.payload)) {
    return true;
  }
  const auto* column = std::get_if<BoundColumnExpression>(&expression.payload);
  if (column == nullptr || source.source_kind != BoundSourceKind::kCatalogTable ||
      !source.table.has_value() || column->column.value() >= bound_select.source_columns().size()) {
    return false;
  }
  const CatalogSnapshot* catalog = bound_select.catalog();
  if (catalog == nullptr || source.table->value >= catalog->tables().size()) {
    return false;
  }
  const CatalogTable& table = catalog->table(*source.table);
  return table.rowid_alias.has_value() &&
         bound_select.source_columns()[column->column.value()].catalog_column == table.rowid_alias;
}

template <typename Bound>
[[nodiscard]] std::optional<BoundExpressionId> RowIdLookupKey(const Bound& bound_select,
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
  const BoundExpressionId key = left_rowid ? comparison->right : comparison->left;
  if (ExpressionDependsOnSource(bound_select, key)) {
    return std::nullopt;
  }
  return key;
}

[[nodiscard]] SourceInfo ResolveSource(const LogicalPlan& logical_plan) {
  const BoundSelect& bound_select = logical_plan.bound_select();
  const LogicalNode& source_node = logical_plan.nodes().front();
  if (std::holds_alternative<LogicalSingleRowNode>(source_node.payload)) {
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

  const auto& scan = std::get<LogicalScanNode>(source_node.payload);
  if (scan.source_kind == BoundSourceKind::kSchemaTable) {
    return SourceInfo{
        .single_row = false,
        .source_kind = BoundSourceKind::kSchemaTable,
        .table = std::nullopt,
        .root_page = RootPageId{1},
        .estimated_rows = kDefaultEstimatedRows,
        .estimated_row_size = 1,
        .rowid_eligible = true,
    };
  }

  const CatalogSnapshot* catalog = bound_select.catalog();
  const TableId table_id = scan.table.value_or(TableId{});
  const CatalogTable& table = catalog->table(table_id);
  return SourceInfo{
      .single_row = false,
      .source_kind = BoundSourceKind::kCatalogTable,
      .table = table_id,
      .root_page = table.root_page,
      .estimated_rows = table.statistics.estimated_rows.value_or(kDefaultEstimatedRows),
      .estimated_row_size = table.statistics.average_row_size.value_or(DerivedTableRowSize(table)),
      .rowid_eligible = !table.without_rowid,
  };
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

[[nodiscard]] ReadPredicateAnalysis AnalyzeReadPredicate(const BoundSelect& bound_select,
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

[[nodiscard]] bool IsRegisteredCollation(const BoundSelect& bound_select,
                                         std::string_view name) noexcept {
  return std::ranges::any_of(
      bound_select.registered_collations(),
      [name](const std::string& registered) { return CatalogNamesEqual(registered, name); });
}

[[nodiscard]] bool IsSupportedIndexShape(const BoundSelect& bound_select, const CatalogTable& table,
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

[[nodiscard]] bool IsIndexedColumnReference(const BoundSelect& bound_select, TableId table_id,
                                            ColumnId column_id,
                                            BoundExpressionId expression_id) noexcept {
  expression_id = TransparentRowIdOperandId(bound_select, expression_id);
  if (!IsValidExpressionId(bound_select, expression_id)) {
    return false;
  }
  const auto* column =
      std::get_if<BoundColumnExpression>(&bound_select.expression(expression_id).payload);
  if (column == nullptr || column->column.value() >= bound_select.source_columns().size()) {
    return false;
  }
  const BoundSourceColumn& source = bound_select.source_columns()[column->column.value()];
  return source.catalog_column == column_id && bound_select.table_source() != nullptr &&
         bound_select.table_source()->table == table_id;
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
    const BoundSelect& bound_select, TableId table_id, const CatalogIndexTerm& term,
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
      IsIndexedColumnReference(bound_select, table_id, *indexed_column, comparison->left);
  const bool right_column =
      IsIndexedColumnReference(bound_select, table_id, *indexed_column, comparison->right);
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

void MarkRequiredSourceValues(const BoundSelect& bound_select, BoundExpressionId id,
                              std::vector<bool>* columns, bool* rowid) {
  if (!IsValidExpressionId(bound_select, id)) {
    return;
  }
  const BoundExpression& expression = bound_select.expression(id);
  std::visit(
      [&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BoundColumnExpression>) {
          if (payload.column.value() < bound_select.source_columns().size()) {
            const std::optional<ColumnId> column =
                bound_select.source_columns()[payload.column.value()].catalog_column;
            if (column.has_value() && column->value < columns->size()) {
              (*columns)[column->value] = true;
            }
          }
        } else if constexpr (std::is_same_v<Payload, BoundRowIdExpression>) {
          *rowid = true;
        } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                             std::is_same_v<Payload, BoundTruthTestExpression> ||
                             std::is_same_v<Payload, BoundLikelihoodExpression> ||
                             std::is_same_v<Payload, BoundCollateExpression>) {
          MarkRequiredSourceValues(bound_select, payload.operand, columns, rowid);
        } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression> ||
                             std::is_same_v<Payload, BoundComparisonExpression>) {
          MarkRequiredSourceValues(bound_select, payload.left, columns, rowid);
          MarkRequiredSourceValues(bound_select, payload.right, columns, rowid);
        } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
          MarkRequiredSourceValues(bound_select, payload.target, columns, rowid);
        } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression> ||
                             std::is_same_v<Payload, BoundCoalesceExpression> ||
                             std::is_same_v<Payload, BoundConditionalExpression>) {
          for (const BoundExpressionId argument : payload.arguments) {
            MarkRequiredSourceValues(bound_select, argument, columns, rowid);
          }
        }
      },
      expression.payload);
}

[[nodiscard]] bool IsCoveringIndex(const BoundSelect& bound_select, const CatalogTable& table,
                                   const CatalogIndex& index,
                                   std::span<const ReadPredicateTerm> predicates,
                                   const std::vector<bool>& selected_terms) {
  std::vector<bool> required_columns(table.columns.size(), false);
  bool required_rowid = false;
  for (std::size_t index_value = 0; index_value < predicates.size(); ++index_value) {
    if (index_value >= selected_terms.size() || !selected_terms[index_value]) {
      MarkRequiredSourceValues(bound_select, predicates[index_value].predicate, &required_columns,
                               &required_rowid);
    }
  }
  for (const BoundResultColumn& result : bound_select.result_columns()) {
    MarkRequiredSourceValues(bound_select, result.expression, &required_columns, &required_rowid);
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

[[nodiscard]] bool IsNullLiteral(const BoundSelect& bound_select, BoundExpressionId id) noexcept {
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

[[nodiscard]] std::uint64_t EstimateIndexOutput(const BoundSelect& bound_select,
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

[[nodiscard]] std::optional<IndexCandidatePlan> AnalyzeIndexCandidate(
    const BoundSelect& bound_select, TableId table_id, IndexId index_id,
    std::span<const ReadPredicateTerm> predicates) {
  const CatalogSnapshot& catalog = *bound_select.catalog();
  const CatalogTable& table = catalog.table(table_id);
  const CatalogIndex& index = catalog.index(index_id);
  if (!IsSupportedIndexShape(bound_select, table, index)) {
    return std::nullopt;
  }

  IndexCandidatePlan candidate{
      .node =
          PhysicalIndexScanNode{
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
          MatchIndexConstraint(bound_select, table_id, term, predicates[predicate_index]);
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
          MatchIndexConstraint(bound_select, table_id, term, predicates[predicate_index]);
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
  candidate.node.covering =
      IsCoveringIndex(bound_select, table, index, predicates, candidate.selected_terms);
  if (!constrained && !candidate.node.covering) {
    return std::nullopt;
  }
  return candidate;
}

[[nodiscard]] AccessPathCandidate PublishIndexCandidate(const BoundSelect& bound_select,
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
      break;
  }
  return PhysicalAccessKind::kEmpty;
}

[[nodiscard]] std::expected<void, OptimizerError> ValidateCandidates(
    const BoundSelect& bound_select, const SourceInfo& source, const PhysicalNode& leaf,
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
    const BoundSelect& bound_select, const SourceInfo& source, const PhysicalNode& leaf,
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

[[nodiscard]] std::expected<void, OptimizerError> ValidatePhysicalPlan(
    const LogicalPlan& logical_plan, std::span<const PhysicalNode> nodes, PhysicalNodeId root,
    std::span<const AccessPathCandidate> candidates, std::size_t selected_candidate_index) {
  if (!logical_plan.valid()) {
    return std::unexpected{InvariantFailure("physical plan retained an invalid logical plan")};
  }
  if (nodes.size() < 2U || nodes.size() > 5U || root.value() != nodes.size() - 1U) {
    return std::unexpected{InvariantFailure("physical plan node arena is invalid")};
  }
  if (candidates.empty() || selected_candidate_index >= candidates.size() ||
      AccessKindOf(nodes.front()) != candidates[selected_candidate_index].kind) {
    return std::unexpected{InvariantFailure("physical access candidates are invalid")};
  }

  const BoundSelect& bound_select = logical_plan.bound_select();
  const SourceInfo source = ResolveSource(logical_plan);
  const PhysicalNode& leaf = nodes.front();
  if (std::expected<void, OptimizerError> validated =
          ValidateCandidates(bound_select, source, leaf, candidates, selected_candidate_index);
      !validated.has_value()) {
    return validated;
  }
  if (const auto* scan = std::get_if<PhysicalTableScanNode>(&leaf.payload); scan != nullptr) {
    if (source.single_row || scan->source_kind != source.source_kind ||
        scan->table != source.table || scan->root_page != source.root_page) {
      return std::unexpected{InvariantFailure("physical table scan does not match logical source")};
    }
  } else if (const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&leaf.payload);
             lookup != nullptr) {
    if (!source.rowid_eligible || lookup->source_kind != source.source_kind ||
        lookup->table != source.table || lookup->root_page != source.root_page ||
        !IsValidExpressionId(bound_select, lookup->key) ||
        ExpressionDependsOnSource(bound_select, lookup->key)) {
      return std::unexpected{InvariantFailure("physical rowid lookup is invalid")};
    }
  } else if (const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&leaf.payload);
             index_scan != nullptr) {
    if (source.source_kind != BoundSourceKind::kCatalogTable || !source.table.has_value() ||
        index_scan->table != source.table || index_scan->table_root_page != source.root_page ||
        bound_select.catalog() == nullptr ||
        index_scan->index.value >= bound_select.catalog()->indexes().size()) {
      return std::unexpected{InvariantFailure("physical index scan source is invalid")};
    }
    const CatalogTable& table = bound_select.catalog()->table(index_scan->table);
    const CatalogIndex& index = bound_select.catalog()->index(index_scan->index);
    if (index.table != index_scan->table || index.root_page != index_scan->index_root_page ||
        !IsSupportedIndexShape(bound_select, table, index) ||
        index_scan->equalities.size() > index.key_term_count) {
      return std::unexpected{InvariantFailure("physical index scan metadata is invalid")};
    }
    for (std::size_t term_index = 0; term_index < index_scan->equalities.size(); ++term_index) {
      const PhysicalIndexEquality& equality = index_scan->equalities[term_index];
      const CatalogIndexTerm& term = index.terms[term_index];
      const auto* column = std::get_if<ColumnId>(&term.target);
      const std::optional<IndexConstraintMatch> match =
          MatchIndexConstraint(bound_select, index_scan->table, term,
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
        const std::optional<IndexConstraintMatch> match =
            MatchIndexConstraint(bound_select, index_scan->table, term,
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
    if (filter.input != input || filter.predicates.empty()) {
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
  const auto* projection = std::get_if<PhysicalProjectionNode>(&nodes[index].payload);
  if (projection == nullptr || projection->input != input ||
      projection->logical_projection != logical_plan.root()) {
    return std::unexpected{InvariantFailure("physical projection does not match logical root")};
  }
  if (std::expected<void, OptimizerError> validated =
          ValidatePredicates(bound_select, source, leaf, guard_predicates, filter_predicates);
      !validated.has_value()) {
    return validated;
  }
  return {};
}

[[nodiscard]] std::string_view SourceName(const PhysicalPlan& plan, BoundSourceKind source_kind,
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
  std::vector<PhysicalNode> nodes;
  std::vector<AccessPathCandidate> candidates;
  std::size_t selected_candidate = 0;
  PhysicalNodeId root{0};
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
              logical_plan.bound_select(), *source.table, index_id, predicates.terms);
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

    const std::size_t node_count =
        2U + static_cast<std::size_t>(!predicates.guards.empty()) +
        static_cast<std::size_t>(!residuals.empty()) +
        static_cast<std::size_t>(logical_plan.bound_select().limit() != nullptr);
    auto impl = std::make_unique<PhysicalPlan::Impl>(std::move(logical_plan));
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
              },
      });
      input = PhysicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    }
    if (const BoundLimit* limit = impl->logical_plan.bound_select().limit(); limit != nullptr) {
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
    impl->nodes.push_back(PhysicalNode{
        .payload =
            PhysicalProjectionNode{
                .input = input,
                .logical_projection = impl->logical_plan.root(),
            },
    });
    impl->root = PhysicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};

    const std::span<const AccessPathCandidate> candidates{impl->candidates};
    if (std::expected<void, OptimizerError> validated = ValidatePhysicalPlan(
            impl->logical_plan, impl->nodes, impl->root, candidates, impl->selected_candidate);
        !validated.has_value()) {
      return std::unexpected{std::move(validated.error())};
    }
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPlannerWork,
                                 1U + impl->candidates.size());
    return PhysicalPlan{std::move(impl)};
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
    } else {
      const auto& create_index = std::get<BoundCreateIndex>(statement);
      payload = PhysicalCreateIndexMutation{
          .no_op = create_index.no_op(),
          .atomicity = create_index.no_op() ? MutationAtomicity::kTransaction
                                            : MutationAtomicity::kStatement,
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
  }
  return "unknown";
}

std::string_view OptimizerErrorCodeName(OptimizerErrorCode code) noexcept {
  switch (code) {
    case OptimizerErrorCode::kInvalidInput:
      return "invalid_input";
    case OptimizerErrorCode::kInternalInvariant:
      return "internal_invariant";
  }
  return "unknown";
}

ErrorCode OptimizerError::base_error_code() const noexcept {
  switch (code) {
    case OptimizerErrorCode::kInvalidInput:
      return ErrorCode::kMisuse;
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
  return impl_ != nullptr ? std::span<const PhysicalNode>{impl_->nodes}
                          : std::span<const PhysicalNode>{};
}

const PhysicalNode& PhysicalPlan::node(PhysicalNodeId id) const noexcept {
  return impl_->nodes[id.value()];
}

PhysicalNodeId PhysicalPlan::root() const noexcept { return impl_->root; }

std::span<const AccessPathCandidate> PhysicalPlan::candidates() const noexcept {
  return impl_ != nullptr ? std::span<const AccessPathCandidate>{impl_->candidates}
                          : std::span<const AccessPathCandidate>{};
}

std::size_t PhysicalPlan::selected_candidate_index() const noexcept {
  return impl_->selected_candidate;
}

const AccessPathCandidate& PhysicalPlan::selected_candidate() const noexcept {
  return impl_->candidates[impl_->selected_candidate];
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

std::string ExplainPhysicalPlan(const PhysicalPlan& plan) {
  const std::span<const PhysicalNode> nodes = plan.nodes();
  if (nodes.empty()) {
    std::terminate();
  }
  const PhysicalNode& access = nodes[0];
  if (std::holds_alternative<PhysicalSingleRowNode>(access.payload)) {
    return "SCAN CONSTANT ROW";
  }
  if (std::holds_alternative<PhysicalEmptyNode>(access.payload)) {
    return "EMPTY RESULT";
  }

  std::string explain;
  if (const auto* scan = std::get_if<PhysicalTableScanNode>(&access.payload); scan != nullptr) {
    explain = "SCAN ";
    AppendExplainIdentifier(SourceName(plan, scan->source_kind, scan->table), &explain);
    return explain;
  }
  if (const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&access.payload);
      index_scan != nullptr) {
    const BoundSelect& bound_select = plan.logical_plan().bound_select();
    const CatalogSnapshot& catalog = *bound_select.catalog();
    const CatalogTable& table = catalog.table(index_scan->table);
    const CatalogIndex& index = catalog.index(index_scan->index);
    const bool constrained = !index_scan->equalities.empty() || index_scan->range.has_value();
    explain = constrained ? "SEARCH " : "SCAN ";
    AppendExplainIdentifier(table.name, &explain);
    explain.append(index_scan->covering ? " USING COVERING INDEX " : " USING INDEX ");
    AppendExplainIdentifier(index.name, &explain);
    if (!constrained) {
      return explain;
    }
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
    return explain;
  }

  const auto& lookup = std::get<PhysicalRowIdLookupNode>(access.payload);
  explain = "SEARCH ";
  AppendExplainIdentifier(SourceName(plan, lookup.source_kind, lookup.table), &explain);
  explain.append(" USING INTEGER PRIMARY KEY (rowid=?)");
  return explain;
}

}  // namespace modern_sqlite
