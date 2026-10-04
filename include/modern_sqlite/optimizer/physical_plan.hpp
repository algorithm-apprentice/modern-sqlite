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
                 PhysicalRowIdLookupNode, PhysicalGuardNode, PhysicalFilterNode, PhysicalLimitNode,
                 PhysicalProjectionNode>;

struct PhysicalNode {
  PhysicalNodePayload payload;
};

enum class PhysicalNodeKind : std::uint8_t {
  kSingleRow,
  kEmpty,
  kTableScan,
  kRowIdLookup,
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

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_OPTIMIZER_PHYSICAL_PLAN_HPP_
