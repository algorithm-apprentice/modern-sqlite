#include "modern_sqlite/planner/logical_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace modern_sqlite {
namespace {

[[nodiscard]] LogicalPlanError PlanError(LogicalPlanErrorCode code, std::string_view detail) {
  return LogicalPlanError{.code = code, .detail = std::string{detail}};
}

[[nodiscard]] LogicalPlanError InvariantError(std::string_view detail) {
  return PlanError(LogicalPlanErrorCode::kInternalInvariant, detail);
}

[[nodiscard]] bool IsValidExpressionId(const BoundSelect& bound_select,
                                       BoundExpressionId id) noexcept {
  return id.value() < bound_select.expressions().size();
}

[[nodiscard]] bool IsValidCollationId(const BoundSelect& bound_select,
                                      BoundCollationId id) noexcept {
  return id.value() < bound_select.collations().size();
}

[[nodiscard]] bool OrderingTermsEqual(const BoundOrderingTerm& left,
                                      const BoundOrderingTerm& right) noexcept {
  return left.expression == right.expression && left.collation == right.collation &&
         left.order == right.order && left.null_placement == right.null_placement &&
         left.result_column == right.result_column &&
         left.explicit_collation == right.explicit_collation;
}

[[nodiscard]] bool IsValidSource(const BoundTableSource& source) noexcept {
  switch (source.kind) {
    case BoundSourceKind::kCatalogTable:
      return source.table.has_value();
    case BoundSourceKind::kSchemaTable:
      return !source.table.has_value();
  }
  return false;
}

[[nodiscard]] bool EqualsAsciiCaseInsensitive(std::string_view left,
                                              std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto lower = [](char value) {
      return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
    };
    if (lower(left[index]) != lower(right[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::expected<std::vector<BoundCollationId>, LogicalPlanError> DistinctCollations(
    const BoundSelect& bound_select, const BoundSelectCore& core) {
  std::optional<BoundCollationId> binary;
  for (std::size_t index = 0; index < bound_select.collations().size(); ++index) {
    if (EqualsAsciiCaseInsensitive(bound_select.collations()[index].name, "BINARY")) {
      binary = BoundCollationId{static_cast<std::uint32_t>(index)};
      break;
    }
  }
  std::vector<BoundCollationId> collations;
  collations.reserve(core.result_columns.size());
  for (const BoundResultColumn& result : core.result_columns) {
    const BoundExpressionProperties& properties =
        bound_select.expression(result.expression).properties;
    if (properties.collation.has_value()) {
      collations.push_back(*properties.collation);
    } else if (binary.has_value()) {
      collations.push_back(*binary);
    } else {
      return std::unexpected{InvariantError("DISTINCT result has no BINARY collation")};
    }
  }
  return collations;
}

struct OrderLayout {
  std::vector<BoundOrderingTerm> terms;
  std::vector<BoundExpressionId> payload_expressions;
  std::vector<SortOutputField> output_fields;
};

struct AdvancedOrderLayout {
  std::vector<BoundOrderingTerm> terms;
  std::vector<LogicalCoreOrderLayout> core_layouts;
  bool set_then_order = false;
};

[[nodiscard]] std::span<const BoundResultColumn> CoreResultColumns(const BoundQueryCore& core) {
  return std::visit(
      [](const auto& value) -> std::span<const BoundResultColumn> { return value.result_columns; },
      core);
}

[[nodiscard]] bool RequiresFullRowsBeforeOrder(const BoundSelect& bound_select) {
  for (const BoundQueryCore& core : bound_select.query_cores()) {
    if (const auto* select = std::get_if<BoundSelectCore>(&core);
        select != nullptr && select->quantifier == SelectQuantifier::kDistinct) {
      return true;
    }
  }
  return std::ranges::any_of(bound_select.compound_operators(), [](CompoundOperator operation) {
    return operation != CompoundOperator::kUnionAll;
  });
}

[[nodiscard]] OrderEvaluationSchedule ExpectedOrderSchedule(const BoundSelect& bound_select) {
  return bound_select.limit() != nullptr && !RequiresFullRowsBeforeOrder(bound_select)
             ? OrderEvaluationSchedule::kKeysThenAdmissionThenPayload
             : OrderEvaluationSchedule::kPayloadThenKeys;
}

[[nodiscard]] std::expected<OrderLayout, LogicalPlanError> BuildOrderLayout(
    const BoundSelect& bound_select) {
  const std::span<const BoundOrderingTerm> terms = bound_select.order_by();
  const std::span<const BoundResultColumn> results = bound_select.result_columns();
  if (terms.size() > std::numeric_limits<std::uint32_t>::max() ||
      results.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{InvariantError("ORDER BY layout exceeds plan field identities")};
  }

  OrderLayout layout;
  layout.terms.assign(terms.begin(), terms.end());
  layout.payload_expressions.reserve(results.size());
  layout.output_fields.resize(results.size());

  for (std::size_t term_index = 0; term_index < terms.size(); ++term_index) {
    if (!terms[term_index].result_column.has_value()) {
      continue;
    }
    const std::size_t result_index = terms[term_index].result_column.value_or(results.size());
    if (result_index >= results.size()) {
      return std::unexpected{InvariantError("ORDER BY term references an invalid result column")};
    }
    layout.output_fields[result_index] = SortOutputField{
        .kind = SortOutputFieldKind::kKey,
        .field_index = static_cast<std::uint32_t>(term_index),
    };
  }

  for (std::size_t result_index = 0; result_index < results.size(); ++result_index) {
    if (layout.output_fields[result_index].kind == SortOutputFieldKind::kKey) {
      continue;
    }
    layout.output_fields[result_index].field_index =
        static_cast<std::uint32_t>(layout.payload_expressions.size());
    layout.payload_expressions.push_back(results[result_index].expression);
  }
  return layout;
}

[[nodiscard]] std::expected<AdvancedOrderLayout, LogicalPlanError> BuildAdvancedOrderLayout(
    const BoundSelect& bound_select) {
  const std::span<const BoundOrderingTerm> terms = bound_select.order_by();
  const std::span<const BoundQueryCore> cores = bound_select.query_cores();
  if (terms.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{InvariantError("advanced ORDER BY exceeds key identities")};
  }

  const bool has_set_operator = std::ranges::any_of(
      bound_select.compound_operators(),
      [](CompoundOperator operation) { return operation != CompoundOperator::kUnionAll; });
  AdvancedOrderLayout layout{
      .terms = std::vector<BoundOrderingTerm>{terms.begin(), terms.end()},
      .set_then_order = has_set_operator && std::ranges::any_of(terms,
                                                                [](const BoundOrderingTerm& term) {
                                                                  return term.explicit_collation;
                                                                }),
  };

  std::vector<bool> complete_rows(cores.size(), false);
  for (std::size_t core_index = 0; core_index < cores.size(); ++core_index) {
    if (std::holds_alternative<BoundValuesCore>(cores[core_index])) {
      complete_rows[core_index] = true;
    } else {
      complete_rows[core_index] =
          std::get<BoundSelectCore>(cores[core_index]).quantifier == SelectQuantifier::kDistinct;
    }
  }
  for (std::size_t operation = 0; operation < bound_select.compound_operators().size();
       ++operation) {
    if (bound_select.compound_operators()[operation] != CompoundOperator::kUnionAll) {
      std::fill_n(complete_rows.begin(), operation + 2U, true);
    }
  }
  if (layout.set_then_order) {
    std::ranges::fill(complete_rows, true);
  }

  layout.core_layouts.reserve(cores.size());
  for (std::size_t core_index = 0; core_index < cores.size(); ++core_index) {
    const std::span<const BoundResultColumn> results = CoreResultColumns(cores[core_index]);
    if (results.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected{InvariantError("advanced ORDER BY exceeds output identities")};
    }
    LogicalCoreOrderLayout core_layout{
        .core_index = core_index,
        .output_fields = std::vector<SortOutputField>(results.size()),
        .schedule = bound_select.limit() != nullptr && !complete_rows[core_index]
                        ? OrderEvaluationSchedule::kKeysThenAdmissionThenPayload
                        : OrderEvaluationSchedule::kPayloadThenKeys,
    };
    core_layout.key_values.reserve(terms.size());

    for (std::size_t term_index = 0; term_index < terms.size(); ++term_index) {
      const BoundOrderingTerm& term = terms[term_index];
      if (term.result_column.has_value()) {
        const std::size_t column = *term.result_column;
        if (column >= results.size()) {
          return std::unexpected{
              InvariantError("advanced ORDER BY term references an invalid field")};
        }
        core_layout.key_values.push_back(
            complete_rows[core_index]
                ? LogicalOrderValue{
                      .kind = LogicalOrderValueKind::kInputField,
                      .expression = std::nullopt,
                      .field_index = static_cast<std::uint32_t>(column),
                  }
                : LogicalOrderValue{
                      .kind = LogicalOrderValueKind::kExpression,
                      .expression = results[column].expression,
                  });
        core_layout.output_fields[column] = SortOutputField{
            .kind = SortOutputFieldKind::kKey,
            .field_index = static_cast<std::uint32_t>(term_index),
        };
      } else {
        if (core_index != 0U) {
          return std::unexpected{
              InvariantError("hidden ORDER BY expression escaped the first query core")};
        }
        core_layout.key_values.push_back(LogicalOrderValue{
            .kind = LogicalOrderValueKind::kExpression,
            .expression = term.expression,
        });
      }
    }

    for (std::size_t column = 0; column < results.size(); ++column) {
      if (core_layout.output_fields[column].kind == SortOutputFieldKind::kKey) {
        continue;
      }
      core_layout.output_fields[column].field_index =
          static_cast<std::uint32_t>(core_layout.payload_values.size());
      core_layout.payload_values.push_back(
          complete_rows[core_index]
              ? LogicalOrderValue{
                    .kind = LogicalOrderValueKind::kInputField,
                    .expression = std::nullopt,
                    .field_index = static_cast<std::uint32_t>(column),
                }
              : LogicalOrderValue{
                    .kind = LogicalOrderValueKind::kExpression,
                    .expression = results[column].expression,
                });
    }
    layout.core_layouts.push_back(std::move(core_layout));
  }
  return layout;
}

[[nodiscard]] std::expected<void, LogicalPlanError> ValidateLogicalOrder(
    const BoundSelect& bound_select, const LogicalOrderNode& order, LogicalNodeId input) {
  const std::span<const BoundOrderingTerm> bound_terms = bound_select.order_by();
  const std::span<const BoundResultColumn> results = bound_select.result_columns();
  const OrderEvaluationSchedule expected_schedule = ExpectedOrderSchedule(bound_select);
  if (order.input != input || order.terms.size() != bound_terms.size() ||
      order.output_fields.size() != results.size() || order.schedule != expected_schedule) {
    return std::unexpected{InvariantError("logical order shape does not match bound ordering")};
  }
  for (std::size_t term_index = 0; term_index < bound_terms.size(); ++term_index) {
    const BoundOrderingTerm& term = order.terms[term_index];
    if (!OrderingTermsEqual(term, bound_terms[term_index]) ||
        !IsValidExpressionId(bound_select, term.expression) ||
        !IsValidCollationId(bound_select, term.collation) ||
        (term.result_column.has_value() && *term.result_column >= results.size())) {
      return std::unexpected{InvariantError("logical order term does not match bound ordering")};
    }
  }

  std::size_t payload_index = 0;
  for (std::size_t result_index = 0; result_index < results.size(); ++result_index) {
    std::optional<std::size_t> key_index;
    for (std::size_t term_index = 0; term_index < bound_terms.size(); ++term_index) {
      if (bound_terms[term_index].result_column == result_index) {
        key_index = term_index;
      }
    }
    const SortOutputField& field = order.output_fields[result_index];
    if (key_index.has_value()) {
      if (field.kind != SortOutputFieldKind::kKey || field.field_index != *key_index) {
        return std::unexpected{InvariantError("logical order key output mapping is invalid")};
      }
      continue;
    }
    if (field.kind != SortOutputFieldKind::kPayload || field.field_index != payload_index ||
        payload_index >= order.payload_expressions.size() ||
        order.payload_expressions[payload_index] != results[result_index].expression ||
        !IsValidExpressionId(bound_select, order.payload_expressions[payload_index])) {
      return std::unexpected{InvariantError("logical order payload output mapping is invalid")};
    }
    ++payload_index;
  }
  if (payload_index != order.payload_expressions.size()) {
    return std::unexpected{InvariantError("logical order retained unexpected payload fields")};
  }
  return {};
}

[[nodiscard]] bool IsAdvancedQuery(const BoundSelect& bound_select) {
  if (bound_select.query_cores().size() != 1U) {
    return true;
  }
  const auto* core = std::get_if<BoundSelectCore>(&bound_select.query_cores().front());
  return core == nullptr || core->quantifier == SelectQuantifier::kDistinct;
}

[[nodiscard]] std::expected<void, LogicalPlanError> ValidateAdvancedLogicalPlan(
    const BoundSelect& bound_select, std::span<const LogicalNode> nodes, LogicalNodeId root) {
  if (nodes.empty() || root.value() >= nodes.size()) {
    return std::unexpected{InvariantError("advanced logical plan root is invalid")};
  }
  std::size_t index = 0;
  std::vector<LogicalNodeId> core_roots;
  core_roots.reserve(bound_select.query_cores().size());

  for (std::size_t core_index = 0; core_index < bound_select.query_cores().size(); ++core_index) {
    const BoundQueryCore& core = bound_select.query_cores()[core_index];
    if (std::holds_alternative<BoundValuesCore>(core)) {
      if (index >= nodes.size()) {
        return std::unexpected{InvariantError("VALUES core node is missing")};
      }
      const auto* values = std::get_if<LogicalValuesNode>(&nodes[index].payload);
      if (values == nullptr || values->core_index != core_index) {
        return std::unexpected{InvariantError("logical VALUES node does not match bound core")};
      }
      core_roots.emplace_back(static_cast<std::uint32_t>(index++));
      continue;
    }

    const auto& select = std::get<BoundSelectCore>(core);
    if (index >= nodes.size()) {
      return std::unexpected{InvariantError("SELECT core source node is missing")};
    }
    LogicalNodeId input{static_cast<std::uint32_t>(index)};
    if (select.table_source.has_value()) {
      const auto* scan = std::get_if<LogicalScanNode>(&nodes[index].payload);
      if (scan == nullptr || scan->core_index != core_index ||
          scan->source_kind != select.table_source->kind ||
          scan->table != select.table_source->table || !IsValidSource(*select.table_source)) {
        return std::unexpected{InvariantError("advanced logical scan does not match bound core")};
      }
    } else {
      const auto* single = std::get_if<LogicalSingleRowNode>(&nodes[index].payload);
      if (single == nullptr || single->core_index != core_index) {
        return std::unexpected{InvariantError("advanced source-free core has no single-row input")};
      }
    }
    ++index;

    if (select.where.has_value()) {
      if (index >= nodes.size()) {
        return std::unexpected{InvariantError("advanced logical filter is missing")};
      }
      const auto* filter = std::get_if<LogicalFilterNode>(&nodes[index].payload);
      if (filter == nullptr || filter->input != input || filter->predicate != *select.where ||
          !IsValidExpressionId(bound_select, filter->predicate)) {
        return std::unexpected{InvariantError("advanced logical filter is invalid")};
      }
      input = LogicalNodeId{static_cast<std::uint32_t>(index++)};
    }

    if (index >= nodes.size()) {
      return std::unexpected{InvariantError("advanced logical projection is missing")};
    }
    const auto* projection = std::get_if<LogicalProjectionNode>(&nodes[index].payload);
    if (projection == nullptr || projection->input != input ||
        projection->core_index != core_index ||
        projection->expressions.size() != select.result_columns.size()) {
      return std::unexpected{InvariantError("advanced logical projection is invalid")};
    }
    for (std::size_t column = 0; column < select.result_columns.size(); ++column) {
      if (projection->expressions[column] != select.result_columns[column].expression ||
          !IsValidExpressionId(bound_select, projection->expressions[column])) {
        return std::unexpected{InvariantError("advanced logical projection expression is invalid")};
      }
    }
    input = LogicalNodeId{static_cast<std::uint32_t>(index++)};

    if (select.quantifier == SelectQuantifier::kDistinct) {
      if (index >= nodes.size()) {
        return std::unexpected{InvariantError("logical DISTINCT node is missing")};
      }
      const auto* distinct = std::get_if<LogicalDistinctNode>(&nodes[index].payload);
      std::expected<std::vector<BoundCollationId>, LogicalPlanError> collations =
          DistinctCollations(bound_select, select);
      if (!collations.has_value()) {
        return std::unexpected{std::move(collations.error())};
      }
      if (distinct == nullptr || distinct->input != input || distinct->core_index != core_index ||
          distinct->collations != *collations ||
          !std::ranges::all_of(distinct->collations, [&bound_select](BoundCollationId id) {
            return IsValidCollationId(bound_select, id);
          })) {
        return std::unexpected{InvariantError("logical DISTINCT node is invalid")};
      }
      input = LogicalNodeId{static_cast<std::uint32_t>(index++)};
    }
    core_roots.push_back(input);
  }

  LogicalNodeId input = core_roots.front();
  for (std::size_t compound = 0; compound < bound_select.compound_operators().size(); ++compound) {
    if (index >= nodes.size()) {
      return std::unexpected{InvariantError("logical compound node is missing")};
    }
    const auto* node = std::get_if<LogicalCompoundNode>(&nodes[index].payload);
    if (node == nullptr || node->left != input || node->right != core_roots[compound + 1U] ||
        node->operation != bound_select.compound_operators()[compound] ||
        node->collations.size() != bound_select.compound_collations().size() ||
        !std::ranges::equal(node->collations, bound_select.compound_collations()) ||
        !std::ranges::all_of(node->collations, [&bound_select](BoundCollationId id) {
          return IsValidCollationId(bound_select, id);
        })) {
      return std::unexpected{InvariantError("logical compound node is invalid")};
    }
    input = LogicalNodeId{static_cast<std::uint32_t>(index++)};
  }

  if (!bound_select.order_by().empty()) {
    if (index >= nodes.size()) {
      return std::unexpected{InvariantError("advanced logical order node is missing")};
    }
    const auto* order = std::get_if<LogicalAdvancedOrderNode>(&nodes[index].payload);
    if (order == nullptr) {
      return std::unexpected{InvariantError("advanced logical order node is invalid")};
    }
    std::expected<AdvancedOrderLayout, LogicalPlanError> expected =
        BuildAdvancedOrderLayout(bound_select);
    if (!expected.has_value()) {
      return std::unexpected{std::move(expected.error())};
    }
    if (order->input != input || order->terms.size() != expected->terms.size() ||
        order->core_layouts != expected->core_layouts ||
        order->set_then_order != expected->set_then_order) {
      return std::unexpected{InvariantError("advanced logical order layout is invalid")};
    }
    for (std::size_t term = 0; term < order->terms.size(); ++term) {
      if (!OrderingTermsEqual(order->terms[term], expected->terms[term]) ||
          !IsValidExpressionId(bound_select, order->terms[term].expression) ||
          !IsValidCollationId(bound_select, order->terms[term].collation)) {
        return std::unexpected{InvariantError("advanced logical order term is invalid")};
      }
    }
    input = LogicalNodeId{static_cast<std::uint32_t>(index++)};
  }

  if (const BoundLimit* limit = bound_select.limit(); limit != nullptr) {
    if (index >= nodes.size()) {
      return std::unexpected{InvariantError("advanced logical limit node is missing")};
    }
    const auto* node = std::get_if<LogicalLimitNode>(&nodes[index].payload);
    if (node == nullptr || node->input != input || node->limit != limit->limit ||
        node->offset != limit->offset || !IsValidExpressionId(bound_select, node->limit) ||
        (node->offset.has_value() && !IsValidExpressionId(bound_select, *node->offset))) {
      return std::unexpected{InvariantError("advanced logical limit node is invalid")};
    }
    input = LogicalNodeId{static_cast<std::uint32_t>(index++)};
  }

  if (!bound_select.order_by().empty()) {
    if (index >= nodes.size()) {
      return std::unexpected{InvariantError("advanced logical output node is missing")};
    }
    const auto* output = std::get_if<LogicalOutputNode>(&nodes[index].payload);
    if (output == nullptr || output->input != input) {
      return std::unexpected{InvariantError("advanced logical output node is invalid")};
    }
    input = LogicalNodeId{static_cast<std::uint32_t>(index++)};
  }

  if (index != nodes.size() || root != input) {
    return std::unexpected{InvariantError("advanced logical plan retained unexpected nodes")};
  }
  return {};
}

[[nodiscard]] std::expected<void, LogicalPlanError> ValidateLogicalPlan(
    const BoundSelect& bound_select, std::span<const LogicalNode> nodes, LogicalNodeId root) {
  if (!bound_select.valid()) {
    return std::unexpected{InvariantError("logical plan retained an invalid bound select")};
  }
  if (IsAdvancedQuery(bound_select)) {
    return ValidateAdvancedLogicalPlan(bound_select, nodes, root);
  }

  const bool ordered = !bound_select.order_by().empty();
  const std::size_t expected_node_count =
      2U + static_cast<std::size_t>(bound_select.where_expression().has_value()) +
      static_cast<std::size_t>(bound_select.limit() != nullptr) + static_cast<std::size_t>(ordered);
  if (nodes.size() != expected_node_count) {
    return std::unexpected{
        InvariantError("logical plan node count does not match bound statement")};
  }
  if (nodes.empty() || root.value() != nodes.size() - 1U) {
    return std::unexpected{InvariantError("logical plan root is not the final node")};
  }

  const BoundTableSource* source = bound_select.table_source();
  if (source == nullptr) {
    const auto* single = std::get_if<LogicalSingleRowNode>(&nodes[0].payload);
    if (single == nullptr || single->core_index != 0U) {
      return std::unexpected{
          InvariantError("source-free SELECT does not begin with single-row input")};
    }
  } else {
    if (!IsValidSource(*source)) {
      return std::unexpected{InvariantError("bound table source violates source-kind invariants")};
    }
    const auto* scan = std::get_if<LogicalScanNode>(&nodes[0].payload);
    if (scan == nullptr || scan->core_index != 0U || scan->source_kind != source->kind ||
        scan->table != source->table) {
      return std::unexpected{InvariantError("logical scan does not match the bound table source")};
    }
  }

  std::size_t index = 1;
  LogicalNodeId input{0};
  if (const std::optional<BoundExpressionId> predicate = bound_select.where_expression();
      predicate.has_value()) {
    const auto* filter = std::get_if<LogicalFilterNode>(&nodes[index].payload);
    if (filter == nullptr || filter->input != input || filter->predicate != *predicate ||
        !IsValidExpressionId(bound_select, filter->predicate)) {
      return std::unexpected{InvariantError("logical filter does not match the bound predicate")};
    }
    input = LogicalNodeId{static_cast<std::uint32_t>(index)};
    ++index;
  }

  if (ordered) {
    const auto* order = std::get_if<LogicalOrderNode>(&nodes[index].payload);
    if (order == nullptr) {
      return std::unexpected{InvariantError("ordered SELECT has no logical order node")};
    }
    if (std::expected<void, LogicalPlanError> validated =
            ValidateLogicalOrder(bound_select, *order, input);
        !validated.has_value()) {
      return validated;
    }
    input = LogicalNodeId{static_cast<std::uint32_t>(index)};
    ++index;
  }

  if (const BoundLimit* bound_limit = bound_select.limit(); bound_limit != nullptr) {
    const auto* limit = std::get_if<LogicalLimitNode>(&nodes[index].payload);
    if (limit == nullptr || limit->input != input || limit->limit != bound_limit->limit ||
        limit->offset != bound_limit->offset || !IsValidExpressionId(bound_select, limit->limit) ||
        (limit->offset.has_value() && !IsValidExpressionId(bound_select, *limit->offset))) {
      return std::unexpected{InvariantError("logical limit does not match the bound limit")};
    }
    input = LogicalNodeId{static_cast<std::uint32_t>(index)};
    ++index;
  }

  if (ordered) {
    const auto* output = std::get_if<LogicalOutputNode>(&nodes[index].payload);
    if (output == nullptr || output->input != input) {
      return std::unexpected{InvariantError("logical output does not match ordered SELECT input")};
    }
    return {};
  }

  const auto* projection = std::get_if<LogicalProjectionNode>(&nodes[index].payload);
  const std::span<const BoundResultColumn> result_columns = bound_select.result_columns();
  if (projection == nullptr || projection->input != input || projection->core_index != 0U ||
      projection->expressions.size() != result_columns.size()) {
    return std::unexpected{InvariantError("logical projection shape does not match bound results")};
  }
  for (std::size_t result_index = 0; result_index < result_columns.size(); ++result_index) {
    if (projection->expressions[result_index] != result_columns[result_index].expression ||
        !IsValidExpressionId(bound_select, projection->expressions[result_index])) {
      return std::unexpected{
          InvariantError("logical projection expressions do not match bound results")};
    }
  }
  return {};
}

}  // namespace

struct LogicalPlan::Impl {
  explicit Impl(BoundSelect input) noexcept : bound_select(std::move(input)) {}

  BoundSelect bound_select;
  std::vector<LogicalNode> nodes;
  LogicalNodeId root{0};
};

struct LogicalMutationPlan::Impl {
  Impl(BoundStatement statement, LogicalMutationPayload mutation) noexcept
      : bound_statement(std::move(statement)), payload(mutation) {}

  BoundStatement bound_statement;
  LogicalMutationPayload payload;
};

namespace planner_detail {

class LogicalPlanBuilder final {
 public:
  [[nodiscard]] static BuildLogicalPlanResult Build(BoundSelect bound_select) {
    if (!bound_select.valid()) {
      return std::unexpected{
          PlanError(LogicalPlanErrorCode::kInvalidInput, "bound select is invalid")};
    }
    if (IsAdvancedQuery(bound_select)) {
      return BuildAdvanced(std::move(bound_select));
    }
    const bool ordered = !bound_select.order_by().empty();
    const std::size_t node_count =
        2U + static_cast<std::size_t>(bound_select.where_expression().has_value()) +
        static_cast<std::size_t>(bound_select.limit() != nullptr) +
        static_cast<std::size_t>(ordered);
    auto impl = std::make_unique<LogicalPlan::Impl>(std::move(bound_select));
    impl->nodes.reserve(node_count);

    const BoundTableSource* source = impl->bound_select.table_source();
    if (source == nullptr) {
      impl->nodes.push_back(LogicalNode{.payload = LogicalSingleRowNode{}});
    } else {
      impl->nodes.push_back(LogicalNode{
          .payload =
              LogicalScanNode{
                  .source_kind = source->kind,
                  .table = source->table,
              },
      });
    }

    LogicalNodeId input{0};
    if (const std::optional<BoundExpressionId> predicate = impl->bound_select.where_expression();
        predicate.has_value()) {
      impl->nodes.push_back(LogicalNode{
          .payload =
              LogicalFilterNode{
                  .input = input,
                  .predicate = *predicate,
              },
      });
      input = LogicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    }

    if (ordered) {
      std::expected<OrderLayout, LogicalPlanError> layout = BuildOrderLayout(impl->bound_select);
      if (!layout.has_value()) {
        return std::unexpected{std::move(layout.error())};
      }
      impl->nodes.push_back(LogicalNode{
          .payload =
              LogicalOrderNode{
                  .input = input,
                  .terms = std::move(layout->terms),
                  .payload_expressions = std::move(layout->payload_expressions),
                  .output_fields = std::move(layout->output_fields),
                  .schedule = impl->bound_select.limit() == nullptr
                                  ? OrderEvaluationSchedule::kPayloadThenKeys
                                  : OrderEvaluationSchedule::kKeysThenAdmissionThenPayload,
              },
      });
      input = LogicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    }

    if (const BoundLimit* limit = impl->bound_select.limit(); limit != nullptr) {
      impl->nodes.push_back(LogicalNode{
          .payload =
              LogicalLimitNode{
                  .input = input,
                  .limit = limit->limit,
                  .offset = limit->offset,
              },
      });
      input = LogicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    }

    if (ordered) {
      impl->nodes.push_back(LogicalNode{
          .payload =
              LogicalOutputNode{
                  .input = input,
              },
      });
    } else {
      std::vector<BoundExpressionId> projections;
      projections.reserve(impl->bound_select.result_columns().size());
      for (const BoundResultColumn& result : impl->bound_select.result_columns()) {
        projections.push_back(result.expression);
      }
      impl->nodes.push_back(LogicalNode{
          .payload =
              LogicalProjectionNode{
                  .input = input,
                  .expressions = std::move(projections),
              },
      });
    }
    impl->root = LogicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};

    if (std::expected<void, LogicalPlanError> validated =
            ValidateLogicalPlan(impl->bound_select, impl->nodes, impl->root);
        !validated.has_value()) {
      return std::unexpected{std::move(validated.error())};
    }
    return LogicalPlan{std::move(impl)};
  }

 private:
  [[nodiscard]] static BuildLogicalPlanResult BuildAdvanced(BoundSelect bound_select) {
    auto impl = std::make_unique<LogicalPlan::Impl>(std::move(bound_select));
    const std::size_t estimated_nodes = impl->bound_select.query_cores().size() * 4U +
                                        impl->bound_select.compound_operators().size() + 3U;
    if (estimated_nodes > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected{InvariantError("advanced logical plan exceeds node identities")};
    }
    impl->nodes.reserve(estimated_nodes);

    const auto append = [&impl](LogicalNode node) {
      impl->nodes.push_back(std::move(node));
      return LogicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};
    };

    std::vector<LogicalNodeId> core_roots;
    core_roots.reserve(impl->bound_select.query_cores().size());
    for (std::size_t core_index = 0; core_index < impl->bound_select.query_cores().size();
         ++core_index) {
      const BoundQueryCore& core = impl->bound_select.query_cores()[core_index];
      if (std::holds_alternative<BoundValuesCore>(core)) {
        core_roots.push_back(
            append(LogicalNode{.payload = LogicalValuesNode{.core_index = core_index}}));
        continue;
      }

      const auto& select = std::get<BoundSelectCore>(core);
      LogicalNodeId input{0};
      if (select.table_source.has_value()) {
        input = append(LogicalNode{
            .payload =
                LogicalScanNode{
                    .core_index = core_index,
                    .source_kind = select.table_source->kind,
                    .table = select.table_source->table,
                },
        });
      } else {
        input = append(LogicalNode{.payload = LogicalSingleRowNode{.core_index = core_index}});
      }

      if (select.where.has_value()) {
        input = append(LogicalNode{
            .payload =
                LogicalFilterNode{
                    .input = input,
                    .predicate = *select.where,
                },
        });
      }

      std::vector<BoundExpressionId> projections;
      projections.reserve(select.result_columns.size());
      for (const BoundResultColumn& result : select.result_columns) {
        projections.push_back(result.expression);
      }
      input = append(LogicalNode{
          .payload =
              LogicalProjectionNode{
                  .input = input,
                  .core_index = core_index,
                  .expressions = std::move(projections),
              },
      });

      if (select.quantifier == SelectQuantifier::kDistinct) {
        std::expected<std::vector<BoundCollationId>, LogicalPlanError> collations =
            DistinctCollations(impl->bound_select, select);
        if (!collations.has_value()) {
          return std::unexpected{std::move(collations.error())};
        }
        input = append(LogicalNode{
            .payload =
                LogicalDistinctNode{
                    .input = input,
                    .core_index = core_index,
                    .collations = std::move(*collations),
                },
        });
      }
      core_roots.push_back(input);
    }

    LogicalNodeId input = core_roots.front();
    for (std::size_t index = 0; index < impl->bound_select.compound_operators().size(); ++index) {
      std::vector<BoundCollationId> collations{
          impl->bound_select.compound_collations().begin(),
          impl->bound_select.compound_collations().end(),
      };
      input = append(LogicalNode{
          .payload =
              LogicalCompoundNode{
                  .left = input,
                  .right = core_roots[index + 1U],
                  .operation = impl->bound_select.compound_operators()[index],
                  .collations = std::move(collations),
              },
      });
    }

    if (!impl->bound_select.order_by().empty()) {
      std::expected<AdvancedOrderLayout, LogicalPlanError> layout =
          BuildAdvancedOrderLayout(impl->bound_select);
      if (!layout.has_value()) {
        return std::unexpected{std::move(layout.error())};
      }
      input = append(LogicalNode{
          .payload =
              LogicalAdvancedOrderNode{
                  .input = input,
                  .terms = std::move(layout->terms),
                  .core_layouts = std::move(layout->core_layouts),
                  .set_then_order = layout->set_then_order,
              },
      });
    }

    if (const BoundLimit* limit = impl->bound_select.limit(); limit != nullptr) {
      input = append(LogicalNode{
          .payload =
              LogicalLimitNode{
                  .input = input,
                  .limit = limit->limit,
                  .offset = limit->offset,
              },
      });
    }

    if (!impl->bound_select.order_by().empty()) {
      input = append(LogicalNode{.payload = LogicalOutputNode{.input = input}});
    }
    impl->root = input;

    std::expected<void, LogicalPlanError> validated =
        ValidateLogicalPlan(impl->bound_select, impl->nodes, impl->root);
    if (!validated.has_value()) {
      return std::unexpected{std::move(validated.error())};
    }
    return LogicalPlan{std::move(impl)};
  }
};

class LogicalStatementPlanBuilder final {
 public:
  [[nodiscard]] static BuildLogicalStatementPlanResult Build(BoundStatement bound_statement) {
    if (!BoundStatementIsValid(bound_statement)) {
      return std::unexpected{
          PlanError(LogicalPlanErrorCode::kInvalidInput, "bound statement is invalid")};
    }
    if (std::holds_alternative<BoundSelect>(bound_statement)) {
      BuildLogicalPlanResult select =
          BuildLogicalPlan(std::get<BoundSelect>(std::move(bound_statement)));
      if (!select.has_value()) {
        return std::unexpected{std::move(select.error())};
      }
      return LogicalStatementPlan{std::in_place_type<LogicalPlan>, std::move(*select)};
    }
    if (const auto* update = std::get_if<BoundUpdate>(&bound_statement); update != nullptr) {
      const LogicalUpdateMutation mutation{
          .predicate = update->where_expression(),
          .changes_rowid = update->changes_rowid(),
      };
      return Mutation(std::move(bound_statement), mutation);
    }
    if (const auto* delete_statement = std::get_if<BoundDelete>(&bound_statement);
        delete_statement != nullptr) {
      const LogicalDeleteMutation mutation{
          .predicate = delete_statement->where_expression(),
      };
      return Mutation(std::move(bound_statement), mutation);
    }
    if (const auto* create = std::get_if<BoundCreateTable>(&bound_statement); create != nullptr) {
      const LogicalCreateTableMutation mutation{
          .no_op = create->no_op(),
      };
      return Mutation(std::move(bound_statement), mutation);
    }
    if (const auto* create = std::get_if<BoundCreateIndex>(&bound_statement); create != nullptr) {
      const LogicalCreateIndexMutation mutation{
          .no_op = create->no_op(),
      };
      return Mutation(std::move(bound_statement), mutation);
    }
    if (const auto* analyze = std::get_if<BoundAnalyze>(&bound_statement); analyze != nullptr) {
      const LogicalAnalyzeMutation mutation{
          .creates_stat1 = !analyze->stat1_root_page().has_value(),
      };
      return Mutation(std::move(bound_statement), mutation);
    }
    if (std::holds_alternative<BoundInsert>(bound_statement)) {
      return Mutation(std::move(bound_statement), LogicalInsertMutation{});
    }
    if (std::holds_alternative<BoundBeginTransaction>(bound_statement)) {
      return LogicalStatementPlan{
          std::in_place_type<BoundBeginTransaction>,
          std::get<BoundBeginTransaction>(std::move(bound_statement)),
      };
    }
    if (std::holds_alternative<BoundCommitTransaction>(bound_statement)) {
      return LogicalStatementPlan{
          std::in_place_type<BoundCommitTransaction>,
          std::get<BoundCommitTransaction>(std::move(bound_statement)),
      };
    }
    if (std::holds_alternative<BoundRollbackTransaction>(bound_statement)) {
      return LogicalStatementPlan{
          std::in_place_type<BoundRollbackTransaction>,
          std::get<BoundRollbackTransaction>(std::move(bound_statement)),
      };
    }
    if (std::holds_alternative<BoundSavepoint>(bound_statement)) {
      return LogicalStatementPlan{
          std::in_place_type<BoundSavepoint>,
          std::get<BoundSavepoint>(std::move(bound_statement)),
      };
    }
    if (std::holds_alternative<BoundReleaseSavepoint>(bound_statement)) {
      return LogicalStatementPlan{
          std::in_place_type<BoundReleaseSavepoint>,
          std::get<BoundReleaseSavepoint>(std::move(bound_statement)),
      };
    }
    return LogicalStatementPlan{
        std::in_place_type<BoundRollbackToSavepoint>,
        std::get<BoundRollbackToSavepoint>(std::move(bound_statement)),
    };
  }

 private:
  [[nodiscard]] static bool BoundStatementIsValid(const BoundStatement& statement) {
    if (statement.valueless_by_exception()) {
      return false;
    }
    return std::visit(
        [](const auto& value) noexcept {
          if constexpr (requires { value.valid(); }) {
            return value.valid();
          }
          return true;
        },
        statement);
  }

  template <typename Payload>
  [[nodiscard]] static BuildLogicalStatementPlanResult Mutation(BoundStatement statement,
                                                                Payload payload) {
    auto impl = std::make_unique<LogicalMutationPlan::Impl>(
        std::move(statement), LogicalMutationPayload{std::move(payload)});
    return LogicalStatementPlan{
        std::in_place_type<LogicalMutationPlan>,
        LogicalMutationPlan{std::move(impl)},
    };
  }
};

}  // namespace planner_detail

LogicalNodeKind LogicalNodeKindOf(const LogicalNode& node) noexcept {
  return static_cast<LogicalNodeKind>(node.payload.index());
}

LogicalMutationKind LogicalMutationKindOf(const LogicalMutationPayload& payload) noexcept {
  return static_cast<LogicalMutationKind>(payload.index());
}

std::string_view LogicalMutationKindName(LogicalMutationKind kind) noexcept {
  switch (kind) {
    case LogicalMutationKind::kInsert:
      return "insert";
    case LogicalMutationKind::kUpdate:
      return "update";
    case LogicalMutationKind::kDelete:
      return "delete";
    case LogicalMutationKind::kCreateTable:
      return "create_table";
    case LogicalMutationKind::kCreateIndex:
      return "create_index";
    case LogicalMutationKind::kAnalyze:
      return "analyze";
  }
  return "unknown";
}

std::string_view LogicalNodeKindName(LogicalNodeKind kind) noexcept {
  switch (kind) {
    case LogicalNodeKind::kSingleRow:
      return "single_row";
    case LogicalNodeKind::kScan:
      return "scan";
    case LogicalNodeKind::kFilter:
      return "filter";
    case LogicalNodeKind::kLimit:
      return "limit";
    case LogicalNodeKind::kProjection:
      return "projection";
    case LogicalNodeKind::kOrder:
      return "order";
    case LogicalNodeKind::kOutput:
      return "output";
    case LogicalNodeKind::kValues:
      return "values";
    case LogicalNodeKind::kDistinct:
      return "distinct";
    case LogicalNodeKind::kCompound:
      return "compound";
    case LogicalNodeKind::kAdvancedOrder:
      return "advanced_order";
  }
  return "unknown";
}

std::string_view LogicalPlanErrorCodeName(LogicalPlanErrorCode code) noexcept {
  switch (code) {
    case LogicalPlanErrorCode::kInvalidInput:
      return "invalid_input";
    case LogicalPlanErrorCode::kUnsupportedFeature:
      return "unsupported_feature";
    case LogicalPlanErrorCode::kInternalInvariant:
      return "internal_invariant";
  }
  return "unknown";
}

ErrorCode LogicalPlanError::base_error_code() const noexcept {
  switch (code) {
    case LogicalPlanErrorCode::kInvalidInput:
      return ErrorCode::kMisuse;
    case LogicalPlanErrorCode::kUnsupportedFeature:
      return ErrorCode::kGeneric;
    case LogicalPlanErrorCode::kInternalInvariant:
      return ErrorCode::kInternal;
  }
  return ErrorCode::kInternal;
}

LogicalPlan::LogicalPlan(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

LogicalPlan::LogicalPlan(LogicalPlan&&) noexcept = default;

LogicalPlan& LogicalPlan::operator=(LogicalPlan&&) noexcept = default;

LogicalPlan::~LogicalPlan() = default;

bool LogicalPlan::valid() const noexcept { return impl_ != nullptr; }

const BoundSelect& LogicalPlan::bound_select() const noexcept { return impl_->bound_select; }

std::span<const LogicalNode> LogicalPlan::nodes() const noexcept {
  return impl_ != nullptr ? std::span<const LogicalNode>{impl_->nodes}
                          : std::span<const LogicalNode>{};
}

const LogicalNode& LogicalPlan::node(LogicalNodeId id) const noexcept {
  return impl_->nodes[id.value()];
}

LogicalNodeId LogicalPlan::root() const noexcept { return impl_->root; }

BuildLogicalPlanResult BuildLogicalPlan(BoundSelect bound_select) {
  return planner_detail::LogicalPlanBuilder::Build(std::move(bound_select));
}

LogicalMutationPlan::LogicalMutationPlan(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LogicalMutationPlan::LogicalMutationPlan(LogicalMutationPlan&&) noexcept = default;

LogicalMutationPlan& LogicalMutationPlan::operator=(LogicalMutationPlan&&) noexcept = default;

LogicalMutationPlan::~LogicalMutationPlan() = default;

bool LogicalMutationPlan::valid() const noexcept { return impl_ != nullptr; }

const BoundStatement& LogicalMutationPlan::bound_statement() const noexcept {
  return impl_->bound_statement;
}

const LogicalMutationPayload& LogicalMutationPlan::payload() const noexcept {
  return impl_->payload;
}

BuildLogicalStatementPlanResult BuildLogicalStatementPlan(BoundStatement bound_statement) {
  return planner_detail::LogicalStatementPlanBuilder::Build(std::move(bound_statement));
}

}  // namespace modern_sqlite
