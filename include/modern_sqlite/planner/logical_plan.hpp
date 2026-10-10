#ifndef MODERN_SQLITE_PLANNER_LOGICAL_PLAN_HPP_
#define MODERN_SQLITE_PLANNER_LOGICAL_PLAN_HPP_

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
#include "modern_sqlite/binder/bound_statement.hpp"

namespace modern_sqlite {

namespace planner_detail {
class LogicalPlanBuilder;
class LogicalStatementPlanBuilder;
}  // namespace planner_detail

class LogicalNodeId final {
 public:
  constexpr explicit LogicalNodeId(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const LogicalNodeId&) const noexcept = default;

 private:
  std::uint32_t value_;
};

struct LogicalSingleRowNode {
  std::size_t core_index = 0;
};

struct LogicalScanNode {
  std::size_t core_index = 0;
  BoundSourceKind source_kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
};

struct LogicalFilterNode {
  LogicalNodeId input;
  BoundExpressionId predicate;
};

struct LogicalLimitNode {
  LogicalNodeId input;
  BoundExpressionId limit;
  std::optional<BoundExpressionId> offset{};
};

struct LogicalProjectionNode {
  LogicalNodeId input;
  std::size_t core_index = 0;
  std::vector<BoundExpressionId> expressions{};
};

enum class OrderEvaluationSchedule : std::uint8_t {
  kPayloadThenKeys,
  kKeysThenAdmissionThenPayload,
};

enum class SortOutputFieldKind : std::uint8_t {
  kKey,
  kPayload,
};

struct SortOutputField {
  SortOutputFieldKind kind = SortOutputFieldKind::kPayload;
  std::uint32_t field_index = 0;

  constexpr auto operator<=>(const SortOutputField&) const noexcept = default;
};

struct LogicalOrderNode {
  LogicalNodeId input;
  std::vector<BoundOrderingTerm> terms{};
  std::vector<BoundExpressionId> payload_expressions{};
  std::vector<SortOutputField> output_fields{};
  OrderEvaluationSchedule schedule = OrderEvaluationSchedule::kPayloadThenKeys;
};

struct LogicalOutputNode {
  LogicalNodeId input;
};

enum class LogicalOrderValueKind : std::uint8_t {
  kExpression,
  kInputField,
};

struct LogicalOrderValue {
  LogicalOrderValueKind kind = LogicalOrderValueKind::kExpression;
  std::optional<BoundExpressionId> expression{};
  std::uint32_t field_index = 0;

  constexpr auto operator<=>(const LogicalOrderValue&) const noexcept = default;
};

struct LogicalCoreOrderLayout {
  std::size_t core_index = 0;
  std::vector<LogicalOrderValue> key_values{};
  std::vector<LogicalOrderValue> payload_values{};
  std::vector<SortOutputField> output_fields{};
  OrderEvaluationSchedule schedule = OrderEvaluationSchedule::kPayloadThenKeys;

  constexpr auto operator<=>(const LogicalCoreOrderLayout&) const noexcept = default;
};

struct LogicalAdvancedOrderNode {
  LogicalNodeId input;
  std::vector<BoundOrderingTerm> terms{};
  std::vector<LogicalCoreOrderLayout> core_layouts{};
  bool set_then_order = false;
};

struct LogicalValuesNode {
  std::size_t core_index = 0;
};

struct LogicalDistinctNode {
  LogicalNodeId input;
  std::size_t core_index = 0;
  std::vector<BoundCollationId> collations{};
};

struct LogicalCompoundNode {
  LogicalNodeId left;
  LogicalNodeId right;
  CompoundOperator operation = CompoundOperator::kUnion;
  std::vector<BoundCollationId> collations{};
};

using LogicalNodePayload =
    std::variant<LogicalSingleRowNode, LogicalScanNode, LogicalFilterNode, LogicalLimitNode,
                 LogicalProjectionNode, LogicalOrderNode, LogicalOutputNode, LogicalValuesNode,
                 LogicalDistinctNode, LogicalCompoundNode, LogicalAdvancedOrderNode>;

struct LogicalNode {
  LogicalNodePayload payload;
};

enum class LogicalNodeKind : std::uint8_t {
  kSingleRow,
  kScan,
  kFilter,
  kLimit,
  kProjection,
  kOrder,
  kOutput,
  kValues,
  kDistinct,
  kCompound,
  kAdvancedOrder,
};

[[nodiscard]] LogicalNodeKind LogicalNodeKindOf(const LogicalNode& node) noexcept;
[[nodiscard]] std::string_view LogicalNodeKindName(LogicalNodeKind kind) noexcept;

enum class LogicalPlanErrorCode : std::uint8_t {
  kInvalidInput,
  kUnsupportedFeature,
  kInternalInvariant,
};

struct LogicalPlanError {
  LogicalPlanErrorCode code = LogicalPlanErrorCode::kInternalInvariant;
  std::string detail{};

  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view LogicalPlanErrorCodeName(LogicalPlanErrorCode code) noexcept;

class LogicalPlan final {
 public:
  LogicalPlan(const LogicalPlan&) = delete;
  LogicalPlan& operator=(const LogicalPlan&) = delete;
  LogicalPlan(LogicalPlan&&) noexcept;
  LogicalPlan& operator=(LogicalPlan&&) noexcept;
  ~LogicalPlan();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const BoundSelect& bound_select() const noexcept;
  [[nodiscard]] std::span<const LogicalNode> nodes() const noexcept;
  [[nodiscard]] const LogicalNode& node(LogicalNodeId id) const noexcept;
  [[nodiscard]] LogicalNodeId root() const noexcept;

 private:
  friend class planner_detail::LogicalPlanBuilder;

  struct Impl;

  explicit LogicalPlan(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

using BuildLogicalPlanResult = std::expected<LogicalPlan, LogicalPlanError>;

[[nodiscard]] BuildLogicalPlanResult BuildLogicalPlan(BoundSelect bound_select);

struct LogicalInsertMutation {};

struct LogicalUpdateMutation {
  std::optional<BoundExpressionId> predicate{};
  bool changes_rowid = false;
};

struct LogicalDeleteMutation {
  std::optional<BoundExpressionId> predicate{};
};

struct LogicalCreateTableMutation {
  bool no_op = false;
};

struct LogicalCreateIndexMutation {
  bool no_op = false;
};

struct LogicalAnalyzeMutation {
  bool creates_stat1 = false;
};

using LogicalMutationPayload =
    std::variant<LogicalInsertMutation, LogicalUpdateMutation, LogicalDeleteMutation,
                 LogicalCreateTableMutation, LogicalCreateIndexMutation, LogicalAnalyzeMutation>;

enum class LogicalMutationKind : std::uint8_t {
  kInsert,
  kUpdate,
  kDelete,
  kCreateTable,
  kCreateIndex,
  kAnalyze,
};

[[nodiscard]] LogicalMutationKind LogicalMutationKindOf(
    const LogicalMutationPayload& payload) noexcept;
[[nodiscard]] std::string_view LogicalMutationKindName(LogicalMutationKind kind) noexcept;

class LogicalMutationPlan final {
 public:
  LogicalMutationPlan(const LogicalMutationPlan&) = delete;
  LogicalMutationPlan& operator=(const LogicalMutationPlan&) = delete;
  LogicalMutationPlan(LogicalMutationPlan&&) noexcept;
  LogicalMutationPlan& operator=(LogicalMutationPlan&&) noexcept;
  ~LogicalMutationPlan();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const BoundStatement& bound_statement() const noexcept;
  [[nodiscard]] const LogicalMutationPayload& payload() const noexcept;

 private:
  friend class planner_detail::LogicalStatementPlanBuilder;

  struct Impl;

  explicit LogicalMutationPlan(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

using LogicalStatementPlan =
    std::variant<LogicalPlan, LogicalMutationPlan, BoundBeginTransaction, BoundCommitTransaction,
                 BoundRollbackTransaction, BoundSavepoint, BoundReleaseSavepoint,
                 BoundRollbackToSavepoint>;

using BuildLogicalStatementPlanResult = std::expected<LogicalStatementPlan, LogicalPlanError>;

[[nodiscard]] BuildLogicalStatementPlanResult BuildLogicalStatementPlan(
    BoundStatement bound_statement);

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_PLANNER_LOGICAL_PLAN_HPP_
