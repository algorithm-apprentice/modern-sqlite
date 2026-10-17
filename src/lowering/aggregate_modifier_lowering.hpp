#ifndef MODERN_SQLITE_SRC_LOWERING_AGGREGATE_MODIFIER_LOWERING_HPP_
#define MODERN_SQLITE_SRC_LOWERING_AGGREGATE_MODIFIER_LOWERING_HPP_

#include <cstdint>
#include <optional>
#include <span>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/bytecode/program.hpp"
#include "plan_lowering_internal.hpp"

namespace modern_sqlite::lowering_internal {

enum class AggregateModifierStrategy : std::uint8_t {
  Direct,
  Distinct,
  Ordered,
  DistinctOrdered,
  UniqueOrdered,
};

enum class AggregateRowKind : std::uint8_t {
  Global,
  FirstGroupRow,
  SubsequentGroupRow,
};

struct AggregateModifierRuntime {
  AggregateModifierStrategy strategy = AggregateModifierStrategy::Direct;
  std::optional<RelationId> relation{};
  std::optional<SorterId> sorter{};
  std::optional<RegisterId> record_first{};
  std::uint32_t order_key_count = 0;
  std::uint32_t record_field_count = 0;
  bool argument_reuses_single_order_key = false;
  bool representative_controller = false;
};

class AggregateModifierServices {
 public:
  virtual ~AggregateModifierServices() = default;

  [[nodiscard]] virtual LoweringResult<void> EmitAggregateExpression(
      BoundExpressionId id, RegisterId destination, AggregateExpressionSource source) = 0;
  [[nodiscard]] virtual LoweringResult<void> EmitAggregatePredicate(
      BoundExpressionId id, Label rejected, AggregateExpressionSource source) = 0;
};

struct AggregateModifierInput {
  ProgramBuilder* builder = nullptr;
  const BoundAggregateProgram* program = nullptr;
  std::span<const BoundAggregateInvocation> invocations{};
  std::span<const std::optional<AggregateId>> aggregates{};
  std::span<const std::optional<RegisterId>> argument_blocks{};
  std::span<const std::optional<RegisterId>> result_registers{};
  std::span<const std::optional<AggregateModifierRuntime>> runtimes{};
  std::optional<RegisterId> representative_effect_register{};
  std::optional<ConstantId> representative_replace_constant{};
  std::optional<ConstantId> representative_keep_constant{};
  std::optional<RegisterId> global_seen_row_register{};
  std::optional<ConstantId> zero_constant{};
  bool global_filtered_controller_defaults = false;
};

[[nodiscard]] LoweringResult<void> EmitAggregateModifierOpen(const AggregateModifierInput& input);
[[nodiscard]] LoweringResult<void> EmitAggregateModifierSteps(const AggregateModifierInput& input,
                                                              AggregateModifierServices& services,
                                                              AggregateExpressionSource source,
                                                              AggregateRowKind row_kind);
[[nodiscard]] LoweringResult<void> EmitAggregateModifierFinalization(
    const AggregateModifierInput& input);
[[nodiscard]] LoweringResult<void> EmitAggregateModifierReset(const AggregateModifierInput& input);
[[nodiscard]] LoweringResult<void> EmitAggregateModifierClose(const AggregateModifierInput& input);

}  // namespace modern_sqlite::lowering_internal

#endif  // MODERN_SQLITE_SRC_LOWERING_AGGREGATE_MODIFIER_LOWERING_HPP_
