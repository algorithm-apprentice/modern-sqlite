#ifndef MODERN_SQLITE_SRC_LOWERING_GLOBAL_AGGREGATE_ACCESS_HPP_
#define MODERN_SQLITE_SRC_LOWERING_GLOBAL_AGGREGATE_ACCESS_HPP_

#include <cstdint>
#include <optional>
#include <span>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"
#include "plan_lowering_internal.hpp"

namespace modern_sqlite::lowering_internal {

class GlobalAggregateAccessServices {
 public:
  virtual ~GlobalAggregateAccessServices() = default;

  [[nodiscard]] virtual LoweringResult<void> EmitExpression(BoundExpressionId id,
                                                            RegisterId destination) = 0;
  [[nodiscard]] virtual LoweringResult<void> EmitLimitInitialization(
      std::optional<Label> completion) = 0;
  [[nodiscard]] virtual LoweringResult<void> EmitGlobalAggregateGuards(Label empty_input) = 0;
  [[nodiscard]] virtual LoweringResult<void> EmitFilters(Label rejected) = 0;
  [[nodiscard]] virtual LoweringResult<void> OpenGlobalAggregates() = 0;
  [[nodiscard]] virtual LoweringResult<void> StepGlobalAggregates() = 0;
  [[nodiscard]] virtual LoweringResult<void> StepIndexedMinMaxAggregate(
      BoundAggregateInvocationId invocation, RegisterId argument) = 0;
  [[nodiscard]] virtual LoweringResult<void> EmitGlobalAggregateCompletion() = 0;
  [[nodiscard]] virtual LoweringResult<void> EmitIndexKeyExpressions(
      const PhysicalIndexScanNode& index, std::optional<Label> null_target) = 0;
  [[nodiscard]] virtual LoweringResult<std::uint32_t> EmitIndexKeyBlock(
      const PhysicalIndexScanNode& index, const PhysicalIndexBound* range_bound) = 0;
};

struct GlobalAggregateAccessInput {
  ProgramBuilder* builder = nullptr;
  const BoundSelect* bound_select = nullptr;
  std::span<const BoundExpression> expressions{};
  std::span<const BoundAggregateFunction> aggregate_functions{};
  std::span<const BoundAggregateInvocation> aggregate_invocations{};
  std::span<const std::optional<AggregateId>> aggregate_descriptors{};
  std::span<const std::optional<RegisterId>> aggregate_argument_blocks{};
  const PhysicalNode* leaf = nullptr;
  const PhysicalTableScanNode* table_scan = nullptr;
  const PhysicalRowIdLookupNode* rowid_lookup = nullptr;
  const PhysicalIndexScanNode* index_scan = nullptr;
  const PhysicalFilterNode* filter = nullptr;
  const PhysicalLimitNode* limit = nullptr;
  const PhysicalAggregateNode* aggregate = nullptr;
  std::optional<CursorId> cursor{};
  std::optional<CursorId> index_cursor{};
  std::optional<RegisterId> index_key_first{};
  std::uint32_t index_key_capacity = 0;
  std::optional<RegisterId> index_rowid_register{};
  std::optional<ConstantId> indexed_min_max_null_constant{};
};

[[nodiscard]] LoweringResult<void> EmitGlobalAggregateAccess(
    const GlobalAggregateAccessInput& input, GlobalAggregateAccessServices& services);

}  // namespace modern_sqlite::lowering_internal

#endif  // MODERN_SQLITE_SRC_LOWERING_GLOBAL_AGGREGATE_ACCESS_HPP_
