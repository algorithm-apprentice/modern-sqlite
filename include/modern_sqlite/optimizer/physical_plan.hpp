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
  kIndexScan,
};

[[nodiscard]] std::string_view PhysicalAccessKindName(PhysicalAccessKind kind) noexcept;

struct AccessPathCost {
  std::uint64_t estimated_input_rows = 0;
  std::uint64_t estimated_output_rows = 0;
  std::uint64_t work_units = 0;

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

struct PhysicalSingleRowNode {};

struct PhysicalEmptyNode {};

struct PhysicalTableScanNode {
  BoundSourceKind source_kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
  RootPageId root_page{};
};

struct PhysicalRowIdLookupNode {
  BoundSourceKind source_kind = BoundSourceKind::kCatalogTable;
  std::optional<TableId> table{};
  RootPageId root_page{};
  BoundExpressionId key;
};

struct PhysicalIndexEquality {
  ColumnId column{};
  BoundExpressionId key{0};
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
  TypeAffinity affinity = TypeAffinity::kNone;
  std::string_view collation_name = "BINARY";
  SortOrder order = SortOrder::kAscending;
  bool reject_null = true;
};

struct PhysicalIndexBound {
  BoundExpressionId key{0};
  BoundExpressionId predicate{0};
  std::size_t conjunct_position = 0;
  bool inclusive = false;
};

struct PhysicalIndexRange {
  ColumnId column{};
  TypeAffinity affinity = TypeAffinity::kNone;
  std::string_view collation_name = "BINARY";
  SortOrder order = SortOrder::kAscending;
  std::optional<PhysicalIndexBound> lower{};
  std::optional<PhysicalIndexBound> upper{};
};

struct PhysicalIndexScanNode {
  TableId table{};
  RootPageId table_root_page{};
  IndexId index{};
  RootPageId index_root_page{};
  std::vector<PhysicalIndexEquality> equalities{};
  std::optional<PhysicalIndexRange> range{};
  bool covering = false;
};

struct PhysicalGuardNode {
  PhysicalNodeId input;
  std::vector<BoundExpressionId> predicates{};
};

struct PhysicalFilterNode {
  PhysicalNodeId input;
  std::vector<BoundExpressionId> predicates{};
};

struct PhysicalLimitNode {
  PhysicalNodeId input;
  BoundExpressionId limit;
  std::optional<BoundExpressionId> offset{};
};

struct PhysicalProjectionNode {
  PhysicalNodeId input;
  LogicalNodeId logical_projection;
};

using PhysicalNodePayload =
    std::variant<PhysicalSingleRowNode, PhysicalEmptyNode, PhysicalTableScanNode,
                 PhysicalRowIdLookupNode, PhysicalIndexScanNode, PhysicalGuardNode,
                 PhysicalFilterNode, PhysicalLimitNode, PhysicalProjectionNode>;

struct PhysicalNode {
  PhysicalNodePayload payload;
};

enum class PhysicalNodeKind : std::uint8_t {
  kSingleRow,
  kEmpty,
  kTableScan,
  kRowIdLookup,
  kIndexScan,
  kGuard,
  kFilter,
  kLimit,
  kProjection,
};

[[nodiscard]] PhysicalNodeKind PhysicalNodeKindOf(const PhysicalNode& node) noexcept;
[[nodiscard]] std::string_view PhysicalNodeKindName(PhysicalNodeKind kind) noexcept;

enum class OptimizerErrorCode : std::uint8_t {
  kInvalidInput,
  kInternalInvariant,
};

struct OptimizerError {
  OptimizerErrorCode code = OptimizerErrorCode::kInternalInvariant;
  std::string detail{};

  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view OptimizerErrorCodeName(OptimizerErrorCode code) noexcept;

class PhysicalPlan final {
 public:
  PhysicalPlan(const PhysicalPlan&) = delete;
  PhysicalPlan& operator=(const PhysicalPlan&) = delete;
  PhysicalPlan(PhysicalPlan&&) noexcept;
  PhysicalPlan& operator=(PhysicalPlan&&) noexcept;
  ~PhysicalPlan();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const LogicalPlan& logical_plan() const noexcept;
  [[nodiscard]] std::span<const PhysicalNode> nodes() const noexcept;
  [[nodiscard]] const PhysicalNode& node(PhysicalNodeId id) const noexcept;
  [[nodiscard]] PhysicalNodeId root() const noexcept;
  [[nodiscard]] std::span<const AccessPathCandidate> candidates() const noexcept;
  [[nodiscard]] std::size_t selected_candidate_index() const noexcept;
  [[nodiscard]] const AccessPathCandidate& selected_candidate() const noexcept;

 private:
  friend class optimizer_detail::PhysicalPlanBuilder;

  struct Impl;

  explicit PhysicalPlan(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

using OptimizeLogicalPlanResult = std::expected<PhysicalPlan, OptimizerError>;

[[nodiscard]] OptimizeLogicalPlanResult OptimizeLogicalPlan(LogicalPlan logical_plan);
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

using PhysicalMutationPayload =
    std::variant<PhysicalInsertMutation, PhysicalUpdateMutation, PhysicalDeleteMutation,
                 PhysicalCreateTableMutation, PhysicalCreateIndexMutation>;

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
    LogicalStatementPlan logical_plan);

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_OPTIMIZER_PHYSICAL_PLAN_HPP_
