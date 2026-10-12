#ifndef MODERN_SQLITE_SRC_LOWERING_GROUPED_AGGREGATE_LOWERING_HPP_
#define MODERN_SQLITE_SRC_LOWERING_GROUPED_AGGREGATE_LOWERING_HPP_

#include <cstdint>
#include <optional>
#include <span>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"
#include "plan_lowering_internal.hpp"

namespace modern_sqlite::lowering_internal {

enum class GroupedExpressionSource : std::uint8_t {
  AccessRow,
  SortedRow,
  Representative,
};

struct GroupedCompletionTargets {
  Label skipped;
  Label stopped;
};

class GroupedAggregateServices {
 public:
  virtual ~GroupedAggregateServices() = default;

  [[nodiscard]] virtual LoweringResult<void> EmitGroupedExpression(
      BoundExpressionId id, RegisterId destination, GroupedExpressionSource source) = 0;
  [[nodiscard]] virtual LoweringResult<void> EmitCompletedGroupedAggregate(
      GroupedCompletionTargets targets) = 0;
  [[nodiscard]] virtual LoweringResult<void> EmitGroupedOutputCompletion() = 0;
};

struct GroupedAggregateInput {
  ProgramBuilder* builder = nullptr;
  const BoundAggregateProgram* program = nullptr;
  const PhysicalAggregateNode* aggregate = nullptr;
  std::span<const BoundAggregateFunction> aggregate_functions{};
  std::span<const BoundAggregateInvocation> aggregate_invocations{};
  std::span<const std::optional<AggregateId>> aggregate_descriptors{};
  std::span<const std::optional<RegisterId>> aggregate_argument_blocks{};
  std::span<const std::optional<RegisterId>> aggregate_result_registers{};
  std::span<const SymbolId> collation_symbols{};
  std::span<const BoundExpressionId> retained_leaves{};
  std::span<const RegisterId> retained_leaf_registers{};
  std::span<const RegisterId> representative_registers{};
  std::optional<SorterId> group_sorter{};
  std::optional<RegisterId> group_sorter_record_first{};
  RegisterId current_group_first{0};
  RegisterId previous_group_first{0};
  RegisterId has_group_register{0};
  RegisterId comparison_register{0};
  std::optional<RegisterId> representative_effect_register{};
  std::optional<RegisterId> representative_replace_register{};
  std::optional<ConstantId> representative_replace_constant{};
  SymbolId binary_collation{0};
  ConstantId zero_constant{0};
  ConstantId one_constant{0};
  bool may_stop_early = false;
};

[[nodiscard]] LoweringResult<void> EmitGroupedAggregateInitialization(
    const GroupedAggregateInput& input);
[[nodiscard]] LoweringResult<void> EmitGroupedAggregateRow(const GroupedAggregateInput& input,
                                                           GroupedAggregateServices& services);
[[nodiscard]] LoweringResult<void> EmitGroupedAggregateCompletion(
    const GroupedAggregateInput& input, GroupedAggregateServices& services);

}  // namespace modern_sqlite::lowering_internal

#endif  // MODERN_SQLITE_SRC_LOWERING_GROUPED_AGGREGATE_LOWERING_HPP_
