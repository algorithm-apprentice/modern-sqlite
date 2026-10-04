#ifndef MODERN_SQLITE_PLANNER_LOGICAL_PLAN_HPP_
#define MODERN_SQLITE_PLANNER_LOGICAL_PLAN_HPP_

#include <compare>
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
#include "modern_sqlite/binder/bound_select.hpp"

namespace modern_sqlite {

namespace planner_detail {
class LogicalPlanBuilder;
}  // namespace planner_detail

class LogicalNodeId final {
 public:
  constexpr explicit LogicalNodeId(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const LogicalNodeId&) const noexcept = default;

 private:
  std::uint32_t value_;
};

struct LogicalSingleRowNode {};

struct LogicalScanNode {
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
  std::vector<BoundExpressionId> expressions{};
};

using LogicalNodePayload = std::variant<LogicalSingleRowNode, LogicalScanNode, LogicalFilterNode,
                                        LogicalLimitNode, LogicalProjectionNode>;

struct LogicalNode {
  LogicalNodePayload payload;
};

enum class LogicalNodeKind : std::uint8_t {
  kSingleRow,
  kScan,
  kFilter,
  kLimit,
  kProjection,
};

[[nodiscard]] LogicalNodeKind LogicalNodeKindOf(const LogicalNode& node) noexcept;
[[nodiscard]] std::string_view LogicalNodeKindName(LogicalNodeKind kind) noexcept;

enum class LogicalPlanErrorCode : std::uint8_t {
  kInvalidInput,
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

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_PLANNER_LOGICAL_PLAN_HPP_
