#include "modern_sqlite/planner/logical_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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

[[nodiscard]] bool IsValidSource(const BoundTableSource& source) noexcept {
  switch (source.kind) {
    case BoundSourceKind::kCatalogTable:
      return source.table.has_value();
    case BoundSourceKind::kSchemaTable:
      return !source.table.has_value();
  }
  return false;
}

[[nodiscard]] std::expected<void, LogicalPlanError> ValidateLogicalPlan(
    const BoundSelect& bound_select, std::span<const LogicalNode> nodes, LogicalNodeId root) {
  if (!bound_select.valid()) {
    return std::unexpected{InvariantError("logical plan retained an invalid bound select")};
  }

  const std::size_t expected_node_count =
      2U + static_cast<std::size_t>(bound_select.where_expression().has_value()) +
      static_cast<std::size_t>(bound_select.limit() != nullptr);
  if (nodes.size() != expected_node_count) {
    return std::unexpected{
        InvariantError("logical plan node count does not match bound statement")};
  }
  if (nodes.empty() || root.value() != nodes.size() - 1U) {
    return std::unexpected{InvariantError("logical plan root is not the final node")};
  }

  const BoundTableSource* source = bound_select.table_source();
  if (source == nullptr) {
    if (!std::holds_alternative<LogicalSingleRowNode>(nodes[0].payload)) {
      return std::unexpected{
          InvariantError("source-free SELECT does not begin with single-row input")};
    }
  } else {
    if (!IsValidSource(*source)) {
      return std::unexpected{InvariantError("bound table source violates source-kind invariants")};
    }
    const auto* scan = std::get_if<LogicalScanNode>(&nodes[0].payload);
    if (scan == nullptr || scan->source_kind != source->kind || scan->table != source->table) {
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

  const auto* projection = std::get_if<LogicalProjectionNode>(&nodes[index].payload);
  const std::span<const BoundResultColumn> result_columns = bound_select.result_columns();
  if (projection == nullptr || projection->input != input ||
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

    const std::size_t node_count =
        2U + static_cast<std::size_t>(bound_select.where_expression().has_value()) +
        static_cast<std::size_t>(bound_select.limit() != nullptr);
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
    impl->root = LogicalNodeId{static_cast<std::uint32_t>(impl->nodes.size() - 1U)};

    if (std::expected<void, LogicalPlanError> validated =
            ValidateLogicalPlan(impl->bound_select, impl->nodes, impl->root);
        !validated.has_value()) {
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
  }
  return "unknown";
}

std::string_view LogicalPlanErrorCodeName(LogicalPlanErrorCode code) noexcept {
  switch (code) {
    case LogicalPlanErrorCode::kInvalidInput:
      return "invalid_input";
    case LogicalPlanErrorCode::kInternalInvariant:
      return "internal_invariant";
  }
  return "unknown";
}

ErrorCode LogicalPlanError::base_error_code() const noexcept {
  switch (code) {
    case LogicalPlanErrorCode::kInvalidInput:
      return ErrorCode::kMisuse;
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
