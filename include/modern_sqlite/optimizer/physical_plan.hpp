#ifndef MODERN_SQLITE_OPTIMIZER_PHYSICAL_PLAN_HPP_
#define MODERN_SQLITE_OPTIMIZER_PHYSICAL_PLAN_HPP_

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
#include "modern_sqlite/planner/logical_plan.hpp"

namespace modern_sqlite {

namespace optimizer_detail {
class PhysicalPlanBuilder;
class PhysicalStatementPlanBuilder;
}  // namespace optimizer_detail

class PhysicalNodeId final {
 public:
  constexpr explicit PhysicalNodeId(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const PhysicalNodeId&) const noexcept = default;

 private:
  std::uint32_t value_;
};

enum class PhysicalAccessKind : std::uint8_t {
  kSingleRow,
  kEmpty,
  kTableScan,
  kRowIdLookup,
  kRowIdRange,
  kIndexScan,
  kAutomaticIndex,
};

struct OptimizerOptions {
  bool allow_file_backed_automatic_indexes = false;

  constexpr auto operator<=>(const OptimizerOptions&) const noexcept = default;
};

[[nodiscard]] std::string_view PhysicalAccessKindName(PhysicalAccessKind kind) noexcept;

struct AccessPathCost {
  std::int32_t setup_log_est = 0;
  std::int32_t run_log_est = 0;
  std::int32_t output_log_est = 0;

  constexpr auto operator<=>(const AccessPathCost&) const noexcept = default;
};

struct AccessPathCandidate {
  PhysicalAccessKind kind = PhysicalAccessKind::kSingleRow;
  AccessPathCost cost{};
  std::optional<IndexId> index{};
  bool covering = false;
  std::uint32_t equality_term_count = 0;
  std::uint32_t range_bound_count = 0;

  constexpr auto operator<=>(const AccessPathCandidate&) const noexcept = default;
};

enum class PhysicalScanDirection : std::uint8_t {
  kForward,
  kReverse,
};

enum class PhysicalPredicateOrigin : std::uint8_t {
  kOn,
  kWhere,
};

struct PhysicalAccessPredicate {
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
  PhysicalPredicateOrigin origin = PhysicalPredicateOrigin::kWhere;

  constexpr auto operator<=>(const PhysicalAccessPredicate&) const noexcept = default;
};

struct PhysicalSingleRowNode {
  std::size_t core_index = 0;

  constexpr bool operator==(const PhysicalSingleRowNode&) const noexcept = default;
};

struct PhysicalEmptyNode {
  std::size_t core_index = 0;

  constexpr bool operator==(const PhysicalEmptyNode&) const noexcept = default;
};

struct PhysicalTableScanNode {
  std::size_t core_index = 0;
  BoundSourceId source{0};
  BoundSourceKind source_kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
  RootPageId root_page{};
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;

  constexpr bool operator==(const PhysicalTableScanNode&) const noexcept = default;
};

struct PhysicalRowIdLookupNode {
  std::size_t core_index = 0;
  BoundSourceId source{0};
  BoundSourceKind source_kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
  RootPageId root_page{};
  BoundExpressionId key;
  std::optional<PhysicalAccessPredicate> constraint{};

  constexpr bool operator==(const PhysicalRowIdLookupNode&) const noexcept = default;
};

struct PhysicalRangeBound {
  BoundExpressionId key{0};
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
  PhysicalPredicateOrigin origin = PhysicalPredicateOrigin::kWhere;
  bool inclusive = false;

  constexpr auto operator<=>(const PhysicalRangeBound&) const noexcept = default;
};

struct PhysicalRowIdRangeNode {
  std::size_t core_index = 0;
  BoundSourceId source{0};
  BoundSourceKind source_kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
  RootPageId root_page{};
  std::optional<PhysicalRangeBound> lower{};
  std::optional<PhysicalRangeBound> upper{};
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;

  constexpr bool operator==(const PhysicalRowIdRangeNode&) const noexcept = default;
};

struct PhysicalIndexEquality {
  ColumnId column{};
  BoundExpressionId key{0};
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
  PhysicalPredicateOrigin origin = PhysicalPredicateOrigin::kWhere;
  TypeAffinity affinity = TypeAffinity::kNone;
  std::string_view collation_name = "BINARY";
  SortOrder order = SortOrder::kAscending;
  bool reject_null = true;

  constexpr auto operator<=>(const PhysicalIndexEquality&) const noexcept = default;
};

struct PhysicalIndexRange {
  ColumnId column{};
  TypeAffinity affinity = TypeAffinity::kNone;
  std::string_view collation_name = "BINARY";
  SortOrder order = SortOrder::kAscending;
  std::optional<PhysicalRangeBound> lower{};
  std::optional<PhysicalRangeBound> upper{};

  constexpr auto operator<=>(const PhysicalIndexRange&) const noexcept = default;
};

struct PhysicalIndexScanNode {
  std::size_t core_index = 0;
  BoundSourceId source{0};
  TableId table{};
  RootPageId table_root_page{};
  IndexId index{};
  RootPageId index_root_page{};
  std::vector<PhysicalIndexEquality> equalities{};
  std::optional<PhysicalIndexRange> range{};
  bool covering = false;
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;

  constexpr bool operator==(const PhysicalIndexScanNode&) const noexcept = default;
};

struct PhysicalAutomaticIndexNode {
  std::size_t core_index = 0;
  BoundSourceId source{0};
  TableId table{};
  RootPageId table_root_page{};
  std::vector<PhysicalIndexEquality> equalities{};
  std::vector<ColumnId> covering_columns{};
  PhysicalScanDirection direction = PhysicalScanDirection::kForward;

  constexpr bool operator==(const PhysicalAutomaticIndexNode&) const noexcept = default;
};

struct PhysicalGuardNode {
  PhysicalNodeId input;
  std::vector<BoundExpressionId> predicates{};
};

struct PhysicalFilterNode {
  PhysicalNodeId input;
  std::vector<BoundExpressionId> predicates{};
  std::vector<BoundExpressionId> guards{};
  std::vector<BoundSourceId> predicate_sources{};
};

struct PhysicalLimitNode {
  PhysicalNodeId input;
  BoundExpressionId limit;
  std::optional<BoundExpressionId> offset{};
};

struct PhysicalProjectionNode {
  PhysicalNodeId input;
  LogicalNodeId logical_projection;
  std::size_t core_index = 0;
};

enum class PhysicalSortStrategy : std::uint8_t {
  kExternal,
  kRuntimeLimit,
};

struct PhysicalSortNode {
  PhysicalNodeId input;
  std::vector<BoundOrderingTerm> terms{};
  std::vector<BoundExpressionId> payload_expressions{};
  std::vector<SortOutputField> output_fields{};
  OrderEvaluationSchedule schedule = OrderEvaluationSchedule::kPayloadThenKeys;
  PhysicalSortStrategy strategy = PhysicalSortStrategy::kExternal;
  bool input_order_satisfied = false;
};

struct PhysicalOutputNode {
  PhysicalNodeId input;
  LogicalNodeId logical_output;
};

struct PhysicalValuesNode {
  std::size_t core_index = 0;
};

enum class PhysicalDistinctStrategy : std::uint8_t {
  kEphemeralMembership,
};

struct PhysicalDistinctNode {
  PhysicalNodeId input;
  std::size_t core_index = 0;
  std::vector<BoundCollationId> collations{};
  PhysicalDistinctStrategy strategy = PhysicalDistinctStrategy::kEphemeralMembership;
};

enum class PhysicalCompoundStrategy : std::uint8_t {
  kConcatenate,
  kUnionLimitOne,
  kEphemeralSet,
  kOrderedMerge,
  kSetThenOrder,
};

struct PhysicalCompoundNode {
  PhysicalNodeId left;
  PhysicalNodeId right;
  CompoundOperator operation = CompoundOperator::kUnion;
  std::vector<BoundCollationId> collations{};
  PhysicalCompoundStrategy strategy = PhysicalCompoundStrategy::kConcatenate;
};

struct PhysicalCoreOrderPlan {
  LogicalCoreOrderLayout layout{};
  PhysicalSortStrategy strategy = PhysicalSortStrategy::kExternal;

  constexpr auto operator<=>(const PhysicalCoreOrderPlan&) const noexcept = default;
};

struct PhysicalAdvancedOrderNode {
  PhysicalNodeId input;
  std::vector<BoundOrderingTerm> terms{};
  std::vector<PhysicalCoreOrderPlan> core_plans{};
  bool set_then_order = false;
  PhysicalSortStrategy final_strategy = PhysicalSortStrategy::kExternal;
  bool input_order_satisfied = false;
};

enum class PhysicalAggregateStrategy : std::uint8_t {
  kGlobal,
  kIndexedMinMax,
  kOrderedGroup,
  kSortedGroup,
};

enum class PhysicalMinMaxNullStrategy : std::uint8_t {
  kNone,
  kNonNullFirstWithSingleNullFallback,
};

struct PhysicalMinMaxPlan {
  BoundAggregateInvocationId invocation;
  PhysicalMinMaxNullStrategy null_strategy = PhysicalMinMaxNullStrategy::kNone;
  bool argument_fixed_by_equality = false;

  constexpr auto operator<=>(const PhysicalMinMaxPlan&) const noexcept = default;
};

struct PhysicalGroupingOrderTerm {
  std::uint32_t group_term_index = 0;
  SortOrder order = SortOrder::kAscending;
  BoundNullPlacement null_placement = BoundNullPlacement::kFirst;

  constexpr auto operator<=>(const PhysicalGroupingOrderTerm&) const noexcept = default;
};

struct PhysicalAggregateNode {
  PhysicalNodeId input;
  std::size_t core_index = 0;
  PhysicalAggregateStrategy strategy = PhysicalAggregateStrategy::kGlobal;
  std::optional<PhysicalMinMaxPlan> min_max{};
  std::vector<PhysicalGroupingOrderTerm> grouping_order{};
  bool at_most_one_row = false;
};

struct PhysicalHavingNode {
  PhysicalNodeId input;
  std::size_t core_index = 0;
  BoundExpressionId predicate;
};

struct PhysicalJoinPredicatePlacement {
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
  std::optional<BoundSourceId> source{};

  constexpr auto operator<=>(const PhysicalJoinPredicatePlacement&) const noexcept = default;
};

struct PhysicalNestedLoopJoinNode {
  PhysicalNodeId left;
  PhysicalNodeId right;
  std::size_t core_index = 0;
  SourceJoinKind join = SourceJoinKind::kCross;
  std::optional<BoundExpressionId> on{};
  std::vector<PhysicalJoinPredicatePlacement> predicates{};
};

using PhysicalNodePayload =
    std::variant<PhysicalSingleRowNode, PhysicalEmptyNode, PhysicalTableScanNode,
                 PhysicalRowIdLookupNode, PhysicalRowIdRangeNode, PhysicalIndexScanNode,
                 PhysicalGuardNode, PhysicalFilterNode, PhysicalLimitNode, PhysicalProjectionNode,
                 PhysicalSortNode, PhysicalOutputNode, PhysicalValuesNode, PhysicalDistinctNode,
                 PhysicalCompoundNode, PhysicalAdvancedOrderNode, PhysicalAggregateNode,
                 PhysicalHavingNode, PhysicalNestedLoopJoinNode, PhysicalAutomaticIndexNode>;

struct PhysicalNode {
  PhysicalNodePayload payload;
};

enum class PhysicalNodeKind : std::uint8_t {
  kSingleRow,
  kEmpty,
  kTableScan,
  kRowIdLookup,
  kRowIdRange,
  kIndexScan,
  kGuard,
  kFilter,
  kLimit,
  kProjection,
  kSort,
  kOutput,
  kValues,
  kDistinct,
  kCompound,
  kAdvancedOrder,
  kAggregate,
  kHaving,
  kNestedLoopJoin,
  kAutomaticIndex,
};

[[nodiscard]] PhysicalNodeKind PhysicalNodeKindOf(const PhysicalNode& node) noexcept;
[[nodiscard]] std::string_view PhysicalNodeKindName(PhysicalNodeKind kind) noexcept;

enum class OptimizerErrorCode : std::uint8_t {
  kInvalidInput,
  kUnsupportedFeature,
  kInternalInvariant,
};

struct OptimizerError {
  OptimizerErrorCode code = OptimizerErrorCode::kInternalInvariant;
  std::string detail{};

  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view OptimizerErrorCodeName(OptimizerErrorCode code) noexcept;

struct PhysicalCoreAccessPlan {
  std::size_t core_index = 0;
  std::vector<AccessPathCandidate> candidates;
  std::size_t selected_candidate = 0;

  constexpr bool operator==(const PhysicalCoreAccessPlan&) const noexcept = default;
};

struct PhysicalQueryPlan {
  BoundQueryId query{0};
  std::vector<PhysicalNode> nodes;
  std::optional<PhysicalCoreAccessPlan> root_core_access{};
  std::vector<PhysicalCoreAccessPlan> core_accesses;
  PhysicalNodeId root{0};
};

class PhysicalPlan final {
 public:
  PhysicalPlan(const PhysicalPlan&) = delete;
  PhysicalPlan& operator=(const PhysicalPlan&) = delete;
  PhysicalPlan(PhysicalPlan&&) noexcept;
  PhysicalPlan& operator=(PhysicalPlan&&) noexcept;
  ~PhysicalPlan();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const LogicalPlan& logical_plan() const noexcept;
  [[nodiscard]] OptimizerOptions options() const noexcept;
  [[nodiscard]] std::span<const PhysicalNode> nodes() const noexcept;
  [[nodiscard]] const PhysicalNode& node(PhysicalNodeId id) const noexcept;
  [[nodiscard]] PhysicalNodeId root() const noexcept;
  [[nodiscard]] std::span<const AccessPathCandidate> candidates() const noexcept;
  [[nodiscard]] std::optional<std::size_t> selected_candidate_index() const noexcept;
  [[nodiscard]] const AccessPathCandidate* selected_candidate() const noexcept;
  [[nodiscard]] std::span<const PhysicalCoreAccessPlan> core_access_plans() const noexcept;
  [[nodiscard]] const PhysicalQueryPlan& query_plan(BoundQueryId id) const noexcept;

 private:
  friend class optimizer_detail::PhysicalPlanBuilder;

  struct Impl;

  explicit PhysicalPlan(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

class PhysicalQueryView final {
 public:
  explicit PhysicalQueryView(const PhysicalPlan& owner,
                             BoundQueryId query = BoundQueryId{0}) noexcept
      : owner_(&owner), query_(query) {}

  [[nodiscard]] bool valid() const noexcept {
    return owner_->valid() && query_.value() < owner_->logical_plan().bound_select().query_count();
  }
  [[nodiscard]] LogicalQueryView logical_plan() const noexcept {
    return LogicalQueryView{owner_->logical_plan(), query_};
  }
  [[nodiscard]] OptimizerOptions options() const noexcept { return owner_->options(); }
  [[nodiscard]] std::span<const PhysicalNode> nodes() const noexcept {
    return owner_->query_plan(query_).nodes;
  }
  [[nodiscard]] const PhysicalNode& node(PhysicalNodeId id) const noexcept {
    return nodes()[id.value()];
  }
  [[nodiscard]] PhysicalNodeId root() const noexcept { return owner_->query_plan(query_).root; }
  [[nodiscard]] std::span<const AccessPathCandidate> candidates() const noexcept {
    const PhysicalQueryPlan& plan = owner_->query_plan(query_);
    const PhysicalCoreAccessPlan* root_access =
        !plan.core_accesses.empty()
            ? &plan.core_accesses.front()
            : (plan.root_core_access.has_value() ? &*plan.root_core_access : nullptr);
    return root_access != nullptr && root_access->core_index == 0U
               ? std::span<const AccessPathCandidate>{root_access->candidates}
               : std::span<const AccessPathCandidate>{};
  }
  [[nodiscard]] std::optional<std::size_t> selected_candidate_index() const noexcept {
    const auto plans = core_access_plans();
    return !plans.empty() && plans.front().core_index == 0U && !plans.front().candidates.empty()
               ? std::optional<std::size_t>{plans.front().selected_candidate}
               : std::nullopt;
  }
  [[nodiscard]] const AccessPathCandidate* selected_candidate() const noexcept {
    const auto selected = selected_candidate_index();
    return selected.has_value() ? &candidates()[*selected] : nullptr;
  }
  [[nodiscard]] std::span<const PhysicalCoreAccessPlan> core_access_plans() const noexcept {
    const PhysicalQueryPlan& plan = owner_->query_plan(query_);
    if (!plan.core_accesses.empty()) {
      return plan.core_accesses;
    }
    return plan.root_core_access.has_value()
               ? std::span<const PhysicalCoreAccessPlan>{&*plan.root_core_access, 1U}
               : std::span<const PhysicalCoreAccessPlan>{};
  }

 private:
  const PhysicalPlan* owner_;
  BoundQueryId query_;
};

using OptimizeLogicalPlanResult = std::expected<PhysicalPlan, OptimizerError>;

[[nodiscard]] OptimizeLogicalPlanResult OptimizeLogicalPlan(LogicalPlan logical_plan,
                                                            OptimizerOptions options = {});
[[nodiscard]] std::string ExplainPhysicalPlan(const PhysicalPlan& plan);

enum class MutationAccessKind : std::uint8_t {
  kEmpty,
  kTableScan,
  kRowIdLookup,
};

[[nodiscard]] std::string_view MutationAccessKindName(MutationAccessKind kind) noexcept;

enum class MutationAtomicity : std::uint8_t {
  kTransaction,
  kStatement,
};

[[nodiscard]] std::string_view MutationAtomicityName(MutationAtomicity atomicity) noexcept;

struct PhysicalMutationAccess {
  MutationAccessKind kind = MutationAccessKind::kTableScan;
  RootPageId root_page{};
  std::optional<BoundExpressionId> key{};
  std::vector<BoundExpressionId> guards{};
  std::vector<BoundExpressionId> residuals{};
};

struct PhysicalInsertMutation {
  MutationAtomicity atomicity = MutationAtomicity::kStatement;
};

struct PhysicalUpdateMutation {
  PhysicalMutationAccess access{};
  MutationAtomicity atomicity = MutationAtomicity::kTransaction;
  bool collect_original_rowids = false;
};

struct PhysicalDeleteMutation {
  PhysicalMutationAccess access{};
  MutationAtomicity atomicity = MutationAtomicity::kTransaction;
  bool collect_original_rowids = false;
};

struct PhysicalCreateTableMutation {
  bool no_op = false;
  MutationAtomicity atomicity = MutationAtomicity::kStatement;
};

struct PhysicalCreateIndexMutation {
  bool no_op = false;
  MutationAtomicity atomicity = MutationAtomicity::kStatement;
};

struct PhysicalAnalyzeMutation {
  bool creates_stat1 = false;
  MutationAtomicity atomicity = MutationAtomicity::kStatement;
};

using PhysicalMutationPayload =
    std::variant<PhysicalInsertMutation, PhysicalUpdateMutation, PhysicalDeleteMutation,
                 PhysicalCreateTableMutation, PhysicalCreateIndexMutation, PhysicalAnalyzeMutation>;

class PhysicalMutationPlan final {
 public:
  PhysicalMutationPlan(const PhysicalMutationPlan&) = delete;
  PhysicalMutationPlan& operator=(const PhysicalMutationPlan&) = delete;
  PhysicalMutationPlan(PhysicalMutationPlan&&) noexcept;
  PhysicalMutationPlan& operator=(PhysicalMutationPlan&&) noexcept;
  ~PhysicalMutationPlan();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const LogicalMutationPlan& logical_plan() const noexcept;
  [[nodiscard]] const PhysicalMutationPayload& payload() const noexcept;

 private:
  friend class optimizer_detail::PhysicalStatementPlanBuilder;

  struct Impl;

  explicit PhysicalMutationPlan(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

using PhysicalStatementPlan =
    std::variant<PhysicalPlan, PhysicalMutationPlan, BoundBeginTransaction, BoundCommitTransaction,
                 BoundRollbackTransaction, BoundSavepoint, BoundReleaseSavepoint,
                 BoundRollbackToSavepoint>;

using OptimizeLogicalStatementPlanResult = std::expected<PhysicalStatementPlan, OptimizerError>;

[[nodiscard]] OptimizeLogicalStatementPlanResult OptimizeLogicalStatementPlan(
    LogicalStatementPlan logical_plan, OptimizerOptions options = {});

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_OPTIMIZER_PHYSICAL_PLAN_HPP_
