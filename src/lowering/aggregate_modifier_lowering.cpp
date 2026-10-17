#include "aggregate_modifier_lowering.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace modern_sqlite::lowering_internal {
namespace {

class AggregateModifierEmitter final {
 public:
  AggregateModifierEmitter(const AggregateModifierInput& input,
                           AggregateModifierServices* services) noexcept
      : input_(input), services_(services) {}

  [[nodiscard]] LoweringResult<void> Open() {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      if (auto opened = Append(OpenAggregateInstruction{
              .aggregate = Aggregate(invocation_id),
          });
          !opened.has_value()) {
        return opened;
      }
    }
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      const AggregateModifierRuntime& runtime = Runtime(invocation_id);
      if (runtime.relation.has_value()) {
        if (auto opened =
                Append(OpenRelationInstruction{.relation = AssumeValue(runtime.relation)});
            !opened.has_value()) {
          return opened;
        }
      }
      if (runtime.sorter.has_value()) {
        if (auto opened = Append(OpenSorterInstruction{.sorter = AssumeValue(runtime.sorter)});
            !opened.has_value()) {
          return opened;
        }
      }
    }
    if (input_.representative_effect_register.has_value()) {
      if (!input_.representative_replace_constant.has_value()) {
        return std::unexpected(
            InternalFailure("aggregate representative replacement constant is unavailable"));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = AssumeValue(input_.representative_replace_constant),
              .output = AssumeValue(input_.representative_effect_register),
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (input_.global_seen_row_register.has_value()) {
      if (!input_.zero_constant.has_value()) {
        return std::unexpected(
            InternalFailure("global aggregate seen-row constant is unavailable"));
      }
      return Append(LoadConstantInstruction{
          .constant = AssumeValue(input_.zero_constant),
          .output = AssumeValue(input_.global_seen_row_register),
      });
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> Steps(AggregateExpressionSource source,
                                           AggregateRowKind row_kind) {
    if (services_ == nullptr) {
      return std::unexpected(InternalFailure("aggregate modifier services are unavailable"));
    }
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      if (auto stepped = EmitInvocation(invocation_id, source, row_kind); !stepped.has_value()) {
        return stepped;
      }
    }
    if (row_kind == AggregateRowKind::Global && input_.global_seen_row_register.has_value()) {
      if (!input_.representative_keep_constant.has_value()) {
        return std::unexpected(InternalFailure("global aggregate seen-row value is unavailable"));
      }
      return Append(LoadConstantInstruction{
          .constant = AssumeValue(input_.representative_keep_constant),
          .output = AssumeValue(input_.global_seen_row_register),
      });
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> Finalize() {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      const AggregateModifierRuntime& runtime = Runtime(invocation_id);
      if (runtime.strategy == AggregateModifierStrategy::Ordered ||
          runtime.strategy == AggregateModifierStrategy::DistinctOrdered) {
        if (auto drained = DrainSorter(invocation_id); !drained.has_value()) {
          return drained;
        }
      } else if (runtime.strategy == AggregateModifierStrategy::UniqueOrdered) {
        if (auto drained = DrainRelation(invocation_id); !drained.has_value()) {
          return drained;
        }
      }
      if (auto finalized = Append(FinalizeAggregateInstruction{
              .aggregate = Aggregate(invocation_id),
              .output = ResultRegister(invocation_id),
          });
          !finalized.has_value()) {
        return finalized;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> Reset() {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      if (auto reset = Append(ResetAggregateInstruction{
              .aggregate = Aggregate(invocation_id),
          });
          !reset.has_value()) {
        return reset;
      }
    }
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      const AggregateModifierRuntime& runtime = Runtime(invocation_id);
      if (runtime.relation.has_value()) {
        if (auto reset =
                Append(ResetRelationInstruction{.relation = AssumeValue(runtime.relation)});
            !reset.has_value()) {
          return reset;
        }
      }
      if (runtime.sorter.has_value()) {
        if (auto reset = Append(ResetSorterInstruction{.sorter = AssumeValue(runtime.sorter)});
            !reset.has_value()) {
          return reset;
        }
      }
    }
    if (input_.representative_effect_register.has_value()) {
      if (!input_.representative_replace_constant.has_value()) {
        return std::unexpected(
            InternalFailure("aggregate reset replacement constant is unavailable"));
      }
      return Append(LoadConstantInstruction{
          .constant = AssumeValue(input_.representative_replace_constant),
          .output = AssumeValue(input_.representative_effect_register),
      });
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> Close() {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      const AggregateModifierRuntime& runtime = Runtime(invocation_id);
      if (runtime.relation.has_value()) {
        if (auto closed =
                Append(CloseRelationInstruction{.relation = AssumeValue(runtime.relation)});
            !closed.has_value()) {
          return closed;
        }
      }
      if (runtime.sorter.has_value()) {
        if (auto closed = Append(CloseSorterInstruction{.sorter = AssumeValue(runtime.sorter)});
            !closed.has_value()) {
          return closed;
        }
      }
      if (auto closed = Append(CloseAggregateInstruction{
              .aggregate = Aggregate(invocation_id),
          });
          !closed.has_value()) {
        return closed;
      }
    }
    return {};
  }

 private:
  [[nodiscard]] LoweringResult<void> Append(Instruction instruction) {
    auto appended = ConvertProgramResult(input_.builder->Append(instruction),
                                         "unable to append a bytecode instruction");
    if (!appended.has_value()) {
      return std::unexpected(std::move(appended.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<Label> CreateLabel() {
    return ConvertProgramResult(input_.builder->CreateLabel(), "unable to create a bytecode label");
  }

  [[nodiscard]] LoweringResult<void> BindLabel(Label label) {
    auto bound =
        ConvertProgramResult(input_.builder->BindLabel(label), "unable to bind a bytecode label");
    if (!bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJump(Label target) {
    auto emitted = ConvertProgramResult(input_.builder->EmitJump(target), kEmitJumpFailureDetail);
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJumpIf(RegisterId input, JumpCondition condition,
                                                Label target) {
    auto emitted = ConvertProgramResult(input_.builder->EmitJumpIf(input, condition, target),
                                        kEmitConditionalJumpFailureDetail);
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return {};
  }

  [[nodiscard]] const AggregateModifierRuntime& Runtime(
      BoundAggregateInvocationId invocation_id) const noexcept {
    return AssumeValue(input_.runtimes[invocation_id.value()]);
  }

  [[nodiscard]] AggregateId Aggregate(BoundAggregateInvocationId invocation_id) const noexcept {
    return AssumeValue(input_.aggregates[invocation_id.value()]);
  }

  [[nodiscard]] RegisterId ArgumentBlock(BoundAggregateInvocationId invocation_id) const noexcept {
    return AssumeValue(input_.argument_blocks[invocation_id.value()]);
  }

  [[nodiscard]] RegisterId ResultRegister(BoundAggregateInvocationId invocation_id) const noexcept {
    return AssumeValue(input_.result_registers[invocation_id.value()]);
  }

  [[nodiscard]] LoweringResult<void> EmitExpression(BoundExpressionId id, RegisterId destination,
                                                    AggregateExpressionSource source) {
    return services_->EmitAggregateExpression(id, destination, source);
  }

  [[nodiscard]] LoweringResult<void> PrepareFilteredRepresentativeGate(
      const BoundAggregateInvocation& invocation, const AggregateModifierRuntime& runtime,
      AggregateRowKind row_kind) {
    if (!runtime.representative_controller || !invocation.filter.has_value() ||
        !input_.representative_effect_register.has_value()) {
      return {};
    }
    if (row_kind == AggregateRowKind::Global && !input_.global_filtered_controller_defaults) {
      return {};
    }
    if (!input_.representative_replace_constant.has_value() ||
        !input_.representative_keep_constant.has_value()) {
      return std::unexpected(
          InternalFailure("filtered aggregate representative constants are unavailable"));
    }
    if (row_kind == AggregateRowKind::FirstGroupRow) {
      return Append(LoadConstantInstruction{
          .constant = AssumeValue(input_.representative_replace_constant),
          .output = AssumeValue(input_.representative_effect_register),
      });
    }
    if (row_kind == AggregateRowKind::SubsequentGroupRow) {
      return Append(LoadConstantInstruction{
          .constant = AssumeValue(input_.representative_keep_constant),
          .output = AssumeValue(input_.representative_effect_register),
      });
    }
    if (!input_.global_seen_row_register.has_value()) {
      return std::unexpected(InternalFailure("global filtered aggregate has no seen-row register"));
    }
    auto first = CreateLabel();
    auto ready = CreateLabel();
    if (!first.has_value()) {
      return std::unexpected(std::move(first.error()));
    }
    if (!ready.has_value()) {
      return std::unexpected(std::move(ready.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(input_.global_seen_row_register),
                                 JumpCondition::kIfFalse, *first);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = AssumeValue(input_.representative_keep_constant),
            .output = AssumeValue(input_.representative_effect_register),
        });
        !loaded.has_value()) {
      return loaded;
    }
    if (auto jumped = EmitJump(*ready); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*first); !bound.has_value()) {
      return bound;
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = AssumeValue(input_.representative_replace_constant),
            .output = AssumeValue(input_.representative_effect_register),
        });
        !loaded.has_value()) {
      return loaded;
    }
    return BindLabel(*ready);
  }

  [[nodiscard]] LoweringResult<void> EmitFilter(BoundExpressionId filter, Label rejected,
                                                AggregateExpressionSource source) {
    return services_->EmitAggregatePredicate(filter, rejected, source);
  }

  [[nodiscard]] LoweringResult<void> EmitArguments(const BoundAggregateInvocation& invocation,
                                                   RegisterId first,
                                                   AggregateExpressionSource source) {
    for (std::size_t index = 0; index < invocation.arguments.size(); ++index) {
      if (auto emitted =
              EmitExpression(invocation.arguments[index],
                             RegisterId{first.value() + static_cast<std::uint32_t>(index)}, source);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedRecord(const BoundAggregateInvocation& invocation,
                                                       const AggregateModifierRuntime& runtime,
                                                       AggregateExpressionSource source) {
    if (!runtime.record_first.has_value()) {
      return std::unexpected(InternalFailure("ordered aggregate has no record registers"));
    }
    const RegisterId record_first = AssumeValue(runtime.record_first);
    if (runtime.argument_reuses_single_order_key) {
      return EmitExpression(invocation.arguments.front(), record_first, source);
    }
    for (std::size_t index = 0; index < invocation.order_by.size(); ++index) {
      if (auto emitted = EmitExpression(
              invocation.order_by[index].expression,
              RegisterId{record_first.value() + static_cast<std::uint32_t>(index)}, source);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return EmitArguments(
        invocation,
        RegisterId{record_first.value() + static_cast<std::uint32_t>(invocation.order_by.size())},
        source);
  }

  [[nodiscard]] LoweringResult<void> StepDirect(BoundAggregateInvocationId invocation_id,
                                                const AggregateModifierRuntime& runtime,
                                                RegisterId first_argument) {
    return Append(StepAggregateInstruction{
        .aggregate = Aggregate(invocation_id),
        .first_argument = first_argument,
        .effect_output = runtime.representative_controller ? input_.representative_effect_register
                                                           : std::nullopt,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitInvocation(BoundAggregateInvocationId invocation_id,
                                                    AggregateExpressionSource source,
                                                    AggregateRowKind row_kind) {
    const BoundAggregateInvocation& invocation = input_.invocations[invocation_id.value()];
    const AggregateModifierRuntime& runtime = Runtime(invocation_id);
    if (auto prepared = PrepareFilteredRepresentativeGate(invocation, runtime, row_kind);
        !prepared.has_value()) {
      return prepared;
    }

    const bool needs_next = invocation.filter.has_value() ||
                            runtime.strategy == AggregateModifierStrategy::Distinct ||
                            runtime.strategy == AggregateModifierStrategy::DistinctOrdered ||
                            runtime.strategy == AggregateModifierStrategy::UniqueOrdered;
    std::optional<Label> next;
    if (needs_next) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      next = *label;
    }
    if (invocation.filter.has_value()) {
      if (auto filtered = EmitFilter(*invocation.filter, AssumeValue(next), source);
          !filtered.has_value()) {
        return filtered;
      }
    }

    switch (runtime.strategy) {
      case AggregateModifierStrategy::Direct: {
        const RegisterId arguments = ArgumentBlock(invocation_id);
        if (auto emitted = EmitArguments(invocation, arguments, source); !emitted.has_value()) {
          return emitted;
        }
        if (auto stepped = StepDirect(invocation_id, runtime, arguments); !stepped.has_value()) {
          return stepped;
        }
        break;
      }
      case AggregateModifierStrategy::Distinct: {
        const RegisterId arguments = ArgumentBlock(invocation_id);
        if (auto emitted = EmitArguments(invocation, arguments, source); !emitted.has_value()) {
          return emitted;
        }
        auto inserted =
            ConvertProgramResult(input_.builder->EmitInsertRelation(
                                     AssumeValue(runtime.relation), arguments, 1,
                                     RelationInsertMode::kKeepExisting, AssumeValue(next)),
                                 "unable to emit aggregate DISTINCT membership");
        if (!inserted.has_value()) {
          return std::unexpected(std::move(inserted.error()));
        }
        if (auto stepped = StepDirect(invocation_id, runtime, arguments); !stepped.has_value()) {
          return stepped;
        }
        break;
      }
      case AggregateModifierStrategy::Ordered: {
        if (auto emitted = EmitOrderedRecord(invocation, runtime, source); !emitted.has_value()) {
          return emitted;
        }
        if (auto inserted = Append(InsertSorterInstruction{
                .sorter = AssumeValue(runtime.sorter),
                .first_value = AssumeValue(runtime.record_first),
                .value_count = runtime.record_field_count,
            });
            !inserted.has_value()) {
          return inserted;
        }
        break;
      }
      case AggregateModifierStrategy::DistinctOrdered: {
        if (auto emitted = EmitOrderedRecord(invocation, runtime, source); !emitted.has_value()) {
          return emitted;
        }
        const RegisterId argument{AssumeValue(runtime.record_first).value() +
                                  runtime.order_key_count};
        auto inserted =
            ConvertProgramResult(input_.builder->EmitInsertRelation(
                                     AssumeValue(runtime.relation), argument, 1,
                                     RelationInsertMode::kKeepExisting, AssumeValue(next)),
                                 "unable to emit ordered aggregate DISTINCT membership");
        if (!inserted.has_value()) {
          return std::unexpected(std::move(inserted.error()));
        }
        if (auto sorted = Append(InsertSorterInstruction{
                .sorter = AssumeValue(runtime.sorter),
                .first_value = AssumeValue(runtime.record_first),
                .value_count = runtime.record_field_count,
            });
            !sorted.has_value()) {
          return sorted;
        }
        break;
      }
      case AggregateModifierStrategy::UniqueOrdered: {
        if (auto emitted = EmitOrderedRecord(invocation, runtime, source); !emitted.has_value()) {
          return emitted;
        }
        auto inserted = ConvertProgramResult(
            input_.builder->EmitInsertRelation(
                AssumeValue(runtime.relation), AssumeValue(runtime.record_first), 1,
                RelationInsertMode::kReplaceExisting, AssumeValue(next)),
            "unable to emit unique ordered aggregate insertion");
        if (!inserted.has_value()) {
          return std::unexpected(std::move(inserted.error()));
        }
        break;
      }
    }

    if (next.has_value()) {
      return BindLabel(*next);
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> ReadSorterArguments(BoundAggregateInvocationId invocation_id) {
    const BoundAggregateInvocation& invocation = input_.invocations[invocation_id.value()];
    const AggregateModifierRuntime& runtime = Runtime(invocation_id);
    const RegisterId arguments = ArgumentBlock(invocation_id);
    for (std::size_t index = 0; index < invocation.arguments.size(); ++index) {
      const std::uint32_t field = runtime.argument_reuses_single_order_key
                                      ? 0U
                                      : runtime.order_key_count + static_cast<std::uint32_t>(index);
      if (auto read = Append(ReadSorterFieldInstruction{
              .sorter = AssumeValue(runtime.sorter),
              .field = field,
              .output = RegisterId{arguments.value() + static_cast<std::uint32_t>(index)},
          });
          !read.has_value()) {
        return read;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> DrainSorter(BoundAggregateInvocationId invocation_id) {
    const AggregateModifierRuntime& runtime = Runtime(invocation_id);
    auto empty = CreateLabel();
    auto row = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    auto rewound =
        ConvertProgramResult(input_.builder->EmitRewindSorter(AssumeValue(runtime.sorter), *empty),
                             "unable to emit aggregate ordering rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto read = ReadSorterArguments(invocation_id); !read.has_value()) {
      return read;
    }
    if (auto stepped = Append(StepAggregateInstruction{
            .aggregate = Aggregate(invocation_id),
            .first_argument = ArgumentBlock(invocation_id),
            .effect_output = std::nullopt,
        });
        !stepped.has_value()) {
      return stepped;
    }
    auto advanced =
        ConvertProgramResult(input_.builder->EmitNextSorter(AssumeValue(runtime.sorter), *row),
                             "unable to emit aggregate ordering advance");
    if (!advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    return BindLabel(*empty);
  }

  [[nodiscard]] LoweringResult<void> DrainRelation(BoundAggregateInvocationId invocation_id) {
    const AggregateModifierRuntime& runtime = Runtime(invocation_id);
    auto empty = CreateLabel();
    auto row = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    auto rewound = ConvertProgramResult(
        input_.builder->EmitRewindRelation(AssumeValue(runtime.relation), *empty),
        "unable to emit unique aggregate ordering rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto read = Append(ReadRelationFieldInstruction{
            .relation = AssumeValue(runtime.relation),
            .field = 0,
            .output = ArgumentBlock(invocation_id),
        });
        !read.has_value()) {
      return read;
    }
    if (auto stepped = Append(StepAggregateInstruction{
            .aggregate = Aggregate(invocation_id),
            .first_argument = ArgumentBlock(invocation_id),
            .effect_output = std::nullopt,
        });
        !stepped.has_value()) {
      return stepped;
    }
    auto advanced =
        ConvertProgramResult(input_.builder->EmitNextRelation(AssumeValue(runtime.relation), *row),
                             "unable to emit unique aggregate ordering advance");
    if (!advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    return BindLabel(*empty);
  }

  const AggregateModifierInput& input_;
  AggregateModifierServices* services_;
};

[[nodiscard]] LoweringResult<void> ValidateInput(const AggregateModifierInput& input) {
  if (input.builder == nullptr || input.program == nullptr ||
      input.runtimes.size() != input.invocations.size()) {
    return std::unexpected(InternalFailure("aggregate modifier input is incomplete"));
  }
  if (input.representative_effect_register.has_value() !=
      input.representative_replace_constant.has_value()) {
    return std::unexpected(
        InternalFailure("aggregate representative initialization is incomplete"));
  }
  if (input.global_filtered_controller_defaults &&
      (!input.representative_effect_register.has_value() ||
       !input.representative_keep_constant.has_value() ||
       !input.global_seen_row_register.has_value() || !input.zero_constant.has_value())) {
    return std::unexpected(
        InternalFailure("global filtered representative metadata is incomplete"));
  }
  for (const BoundAggregateInvocationId invocation_id : input.program->invocations) {
    if (invocation_id.value() >= input.invocations.size() ||
        invocation_id.value() >= input.aggregates.size() ||
        invocation_id.value() >= input.argument_blocks.size() ||
        invocation_id.value() >= input.result_registers.size() ||
        !input.aggregates[invocation_id.value()].has_value() ||
        !input.argument_blocks[invocation_id.value()].has_value() ||
        !input.result_registers[invocation_id.value()].has_value() ||
        !input.runtimes[invocation_id.value()].has_value()) {
      return std::unexpected(InternalFailure("aggregate modifier metadata is incomplete"));
    }
    const BoundAggregateInvocation& invocation = input.invocations[invocation_id.value()];
    const AggregateModifierRuntime& runtime = AssumeValue(input.runtimes[invocation_id.value()]);
    const bool ordered_strategy = runtime.strategy == AggregateModifierStrategy::Ordered ||
                                  runtime.strategy == AggregateModifierStrategy::DistinctOrdered ||
                                  runtime.strategy == AggregateModifierStrategy::UniqueOrdered;
    if (ordered_strategy) {
      const std::size_t expected_fields =
          runtime.argument_reuses_single_order_key
              ? 1U
              : invocation.order_by.size() + invocation.arguments.size();
      if (runtime.order_key_count != invocation.order_by.size() ||
          runtime.record_field_count != expected_fields ||
          runtime.argument_reuses_single_order_key != invocation.argument_reuses_single_order_key) {
        return std::unexpected(InternalFailure("aggregate ordering layout is inconsistent"));
      }
    }
    switch (runtime.strategy) {
      case AggregateModifierStrategy::Direct:
        if (runtime.relation.has_value() || runtime.sorter.has_value() ||
            runtime.record_first.has_value()) {
          return std::unexpected(InternalFailure("direct aggregate runtime is invalid"));
        }
        break;
      case AggregateModifierStrategy::Distinct:
        if (!runtime.relation.has_value() || runtime.sorter.has_value() ||
            runtime.record_first.has_value() || invocation.arguments.size() != 1U) {
          return std::unexpected(InternalFailure("aggregate DISTINCT runtime is invalid"));
        }
        break;
      case AggregateModifierStrategy::Ordered:
        if (runtime.relation.has_value() || !runtime.sorter.has_value() ||
            !runtime.record_first.has_value()) {
          return std::unexpected(InternalFailure("ordered aggregate runtime is invalid"));
        }
        break;
      case AggregateModifierStrategy::DistinctOrdered:
        if (!runtime.relation.has_value() || !runtime.sorter.has_value() ||
            !runtime.record_first.has_value() || invocation.arguments.size() != 1U ||
            runtime.argument_reuses_single_order_key) {
          return std::unexpected(InternalFailure("ordered aggregate DISTINCT runtime is invalid"));
        }
        break;
      case AggregateModifierStrategy::UniqueOrdered:
        if (!runtime.relation.has_value() || runtime.sorter.has_value() ||
            !runtime.record_first.has_value() || invocation.arguments.size() != 1U ||
            !runtime.argument_reuses_single_order_key) {
          return std::unexpected(InternalFailure("unique ordered aggregate runtime is invalid"));
        }
        break;
    }
    if (runtime.representative_controller && !input.representative_effect_register.has_value()) {
      return std::unexpected(
          InternalFailure("aggregate representative effect register is unavailable"));
    }
  }
  return {};
}

}  // namespace

LoweringResult<void> EmitAggregateModifierOpen(const AggregateModifierInput& input) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return AggregateModifierEmitter(input, nullptr).Open();
}

LoweringResult<void> EmitAggregateModifierSteps(const AggregateModifierInput& input,
                                                AggregateModifierServices& services,
                                                AggregateExpressionSource source,
                                                AggregateRowKind row_kind) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return AggregateModifierEmitter(input, &services).Steps(source, row_kind);
}

LoweringResult<void> EmitAggregateModifierFinalization(const AggregateModifierInput& input) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return AggregateModifierEmitter(input, nullptr).Finalize();
}

LoweringResult<void> EmitAggregateModifierReset(const AggregateModifierInput& input) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return AggregateModifierEmitter(input, nullptr).Reset();
}

LoweringResult<void> EmitAggregateModifierClose(const AggregateModifierInput& input) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return AggregateModifierEmitter(input, nullptr).Close();
}

}  // namespace modern_sqlite::lowering_internal
