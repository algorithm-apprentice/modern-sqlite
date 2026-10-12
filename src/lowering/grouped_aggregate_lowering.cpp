#include "grouped_aggregate_lowering.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace modern_sqlite::lowering_internal {
namespace {

class GroupedAggregateEmitter final {
 public:
  GroupedAggregateEmitter(const GroupedAggregateInput& input,
                          GroupedAggregateServices* services) noexcept
      : input_(input), services_(services) {}

  [[nodiscard]] LoweringResult<void> Initialize() {
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = input_.zero_constant,
            .output = input_.has_group_register,
        });
        !loaded.has_value()) {
      return loaded;
    }
    for (std::size_t index = 0; index < input_.program->group_by.size(); ++index) {
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = input_.zero_constant,
              .output = PreviousGroup(index),
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    for (const RegisterId representative : input_.representative_registers) {
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = input_.zero_constant,
              .output = representative,
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (input_.representative_replace_register.has_value()) {
      if (!input_.representative_replace_constant.has_value()) {
        return std::unexpected(
            InternalFailure("grouped representative replacement constant is unavailable"));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = AssumeValue(input_.representative_replace_constant),
              .output = AssumeValue(input_.representative_replace_register),
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (input_.aggregate->strategy == PhysicalAggregateStrategy::kSortedGroup) {
      if (!input_.group_sorter.has_value()) {
        return std::unexpected(InternalFailure("sorted grouping has no sorter"));
      }
      if (auto opened = Append(OpenSorterInstruction{.sorter = AssumeValue(input_.group_sorter)});
          !opened.has_value()) {
        return opened;
      }
    }
    return OpenAggregates();
  }

  [[nodiscard]] LoweringResult<void> EmitRow() {
    if (services_ == nullptr) {
      return std::unexpected(InternalFailure("grouped aggregate services are unavailable"));
    }
    if (input_.aggregate->strategy == PhysicalAggregateStrategy::kOrderedGroup) {
      if (auto keys = EmitGroupKeys(GroupedExpressionSource::AccessRow, input_.current_group_first);
          !keys.has_value()) {
        return keys;
      }
      return ConsumeCurrentRow(GroupedExpressionSource::AccessRow);
    }
    if (input_.aggregate->strategy != PhysicalAggregateStrategy::kSortedGroup) {
      return std::unexpected(InternalFailure("grouped aggregate strategy is invalid"));
    }
    return InsertSortedRow();
  }

  [[nodiscard]] LoweringResult<void> Complete() {
    if (services_ == nullptr) {
      return std::unexpected(InternalFailure("grouped aggregate services are unavailable"));
    }
    if (input_.aggregate->strategy == PhysicalAggregateStrategy::kOrderedGroup) {
      return FinishActiveGroup(false);
    }
    if (input_.aggregate->strategy != PhysicalAggregateStrategy::kSortedGroup ||
        !input_.group_sorter.has_value()) {
      return std::unexpected(InternalFailure("sorted grouping completion is invalid"));
    }
    return DrainSortedRows();
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

  [[nodiscard]] RegisterId CurrentGroup(std::size_t index) const noexcept {
    return RegisterId{input_.current_group_first.value() + static_cast<std::uint32_t>(index)};
  }

  [[nodiscard]] RegisterId PreviousGroup(std::size_t index) const noexcept {
    return RegisterId{input_.previous_group_first.value() + static_cast<std::uint32_t>(index)};
  }

  [[nodiscard]] LoweringResult<void> EmitGroupKeys(GroupedExpressionSource source,
                                                   RegisterId first) {
    for (std::size_t index = 0; index < input_.program->group_by.size(); ++index) {
      if (auto emitted = services_->EmitGroupedExpression(
              input_.program->group_by[index].expression,
              RegisterId{first.value() + static_cast<std::uint32_t>(index)}, source);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> CopyCurrentGroup() {
    for (std::size_t index = 0; index < input_.program->group_by.size(); ++index) {
      if (auto copied = Append(CopyInstruction{
              .input = CurrentGroup(index),
              .output = PreviousGroup(index),
          });
          !copied.has_value()) {
        return copied;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> CompareCurrentGroup(Label changed) {
    for (std::size_t index = 0; index < input_.program->group_by.size(); ++index) {
      const BoundCollationId collation = input_.program->group_by[index].collation;
      if (collation.value() >= input_.collation_symbols.size()) {
        return std::unexpected(InternalFailure("grouping collation is unavailable"));
      }
      if (auto compared = Append(CompareInstruction{
              .comparison = SqlComparison::kIs,
              .affinity = TypeAffinity::kNone,
              .collation = input_.collation_symbols[collation.value()],
              .left = PreviousGroup(index),
              .right = CurrentGroup(index),
              .output = input_.comparison_register,
          });
          !compared.has_value()) {
        return compared;
      }
      if (auto jumped = EmitJumpIf(input_.comparison_register, JumpCondition::kIfFalse, changed);
          !jumped.has_value()) {
        return jumped;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> OpenAggregates() {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      if (invocation_id.value() >= input_.aggregate_descriptors.size() ||
          !input_.aggregate_descriptors[invocation_id.value()].has_value()) {
        return std::unexpected(InternalFailure("grouped aggregate descriptor is unavailable"));
      }
      if (auto opened = Append(OpenAggregateInstruction{
              .aggregate = AssumeValue(input_.aggregate_descriptors[invocation_id.value()]),
          });
          !opened.has_value()) {
        return opened;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> ResetAggregates() {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      if (auto reset = Append(ResetAggregateInstruction{
              .aggregate = AssumeValue(input_.aggregate_descriptors[invocation_id.value()]),
          });
          !reset.has_value()) {
        return reset;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> FinalizeAggregates() {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      if (invocation_id.value() >= input_.aggregate_result_registers.size() ||
          !input_.aggregate_result_registers[invocation_id.value()].has_value()) {
        return std::unexpected(
            InternalFailure("grouped aggregate final result register is unavailable"));
      }
      if (auto finalized = Append(FinalizeAggregateInstruction{
              .aggregate = AssumeValue(input_.aggregate_descriptors[invocation_id.value()]),
              .output = AssumeValue(input_.aggregate_result_registers[invocation_id.value()]),
          });
          !finalized.has_value()) {
        return finalized;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> CloseAggregates() {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      if (auto closed = Append(CloseAggregateInstruction{
              .aggregate = AssumeValue(input_.aggregate_descriptors[invocation_id.value()]),
          });
          !closed.has_value()) {
        return closed;
      }
    }
    return {};
  }

  [[nodiscard]] bool HasRepresentativeController() const {
    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      const BoundAggregateInvocation& invocation =
          input_.aggregate_invocations[invocation_id.value()];
      if (input_.aggregate_functions[invocation.function.value()].extremum !=
          BoundAggregateExtremumKind::kNone) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] LoweringResult<void> CaptureRepresentative(GroupedExpressionSource source) {
    for (std::size_t index = 0; index < input_.program->representative_leaves.size(); ++index) {
      if (auto emitted =
              services_->EmitGroupedExpression(input_.program->representative_leaves[index],
                                               input_.representative_registers[index], source);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> StepAggregates(GroupedExpressionSource source,
                                                    bool first_row) {
    const bool has_representative = !input_.program->representative_leaves.empty();
    const bool has_controller = has_representative && HasRepresentativeController();
    if (has_controller && (!input_.representative_effect_register.has_value() ||
                           !input_.representative_replace_register.has_value())) {
      return std::unexpected(
          InternalFailure("grouped representative effect registers are unavailable"));
    }

    for (const BoundAggregateInvocationId invocation_id : input_.program->invocations) {
      const BoundAggregateInvocation& invocation =
          input_.aggregate_invocations[invocation_id.value()];
      if (invocation_id.value() >= input_.aggregate_argument_blocks.size() ||
          !input_.aggregate_argument_blocks[invocation_id.value()].has_value()) {
        return std::unexpected(InternalFailure("grouped aggregate argument block is unavailable"));
      }
      const RegisterId first = AssumeValue(input_.aggregate_argument_blocks[invocation_id.value()]);
      for (std::size_t index = 0; index < invocation.arguments.size(); ++index) {
        if (auto emitted = services_->EmitGroupedExpression(
                invocation.arguments[index],
                RegisterId{first.value() + static_cast<std::uint32_t>(index)}, source);
            !emitted.has_value()) {
          return emitted;
        }
      }
      const bool controls_representative =
          has_controller && input_.aggregate_functions[invocation.function.value()].extremum !=
                                BoundAggregateExtremumKind::kNone;
      if (auto stepped = Append(StepAggregateInstruction{
              .aggregate = AssumeValue(input_.aggregate_descriptors[invocation_id.value()]),
              .first_argument = first,
              .effect_output =
                  controls_representative ? input_.representative_effect_register : std::nullopt,
          });
          !stepped.has_value()) {
        return stepped;
      }
    }

    if (!has_representative) {
      return {};
    }
    if (!has_controller) {
      return first_row ? CaptureRepresentative(source) : LoweringResult<void>{};
    }

    auto keep = CreateLabel();
    if (!keep.has_value()) {
      return std::unexpected(std::move(keep.error()));
    }
    if (auto compared = Append(CompareInstruction{
            .comparison = SqlComparison::kEqual,
            .affinity = TypeAffinity::kNone,
            .collation = input_.binary_collation,
            .left = AssumeValue(input_.representative_effect_register),
            .right = AssumeValue(input_.representative_replace_register),
            .output = input_.comparison_register,
        });
        !compared.has_value()) {
      return compared;
    }
    if (auto jumped = EmitJumpIf(input_.comparison_register, JumpCondition::kIfFalse, *keep);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto captured = CaptureRepresentative(source); !captured.has_value()) {
      return captured;
    }
    return BindLabel(*keep);
  }

  [[nodiscard]] LoweringResult<void> StartFirstGroup(GroupedExpressionSource source) {
    if (auto copied = CopyCurrentGroup(); !copied.has_value()) {
      return copied;
    }
    if (auto stepped = StepAggregates(source, true); !stepped.has_value()) {
      return stepped;
    }
    return Append(LoadConstantInstruction{
        .constant = input_.one_constant,
        .output = input_.has_group_register,
    });
  }

  [[nodiscard]] LoweringResult<void> StopAtCompletedGroup() {
    if (auto closed = CloseAggregates(); !closed.has_value()) {
      return closed;
    }
    if (input_.group_sorter.has_value()) {
      if (auto closed = Append(CloseSorterInstruction{.sorter = AssumeValue(input_.group_sorter)});
          !closed.has_value()) {
        return closed;
      }
    }
    return services_->EmitGroupedOutputCompletion();
  }

  [[nodiscard]] LoweringResult<void> ConsumeCurrentRow(GroupedExpressionSource source) {
    auto first = CreateLabel();
    auto changed = CreateLabel();
    auto continue_group = CreateLabel();
    auto done = CreateLabel();
    std::optional<Label> stopped;
    if (!first.has_value()) {
      return std::unexpected(std::move(first.error()));
    }
    if (!changed.has_value()) {
      return std::unexpected(std::move(changed.error()));
    }
    if (!continue_group.has_value()) {
      return std::unexpected(std::move(continue_group.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    if (input_.may_stop_early) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      stopped = *label;
    }

    if (auto jumped = EmitJumpIf(input_.has_group_register, JumpCondition::kIfFalse, *first);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto compared = CompareCurrentGroup(*changed); !compared.has_value()) {
      return compared;
    }
    if (auto stepped = StepAggregates(source, false); !stepped.has_value()) {
      return stepped;
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return jumped;
    }

    if (auto bound = BindLabel(*changed); !bound.has_value()) {
      return bound;
    }
    if (auto finalized = FinalizeAggregates(); !finalized.has_value()) {
      return finalized;
    }
    if (auto output = services_->EmitCompletedGroupedAggregate(GroupedCompletionTargets{
            .skipped = *continue_group,
            .stopped = stopped.value_or(*continue_group),
        });
        !output.has_value()) {
      return output;
    }
    if (auto bound = BindLabel(*continue_group); !bound.has_value()) {
      return bound;
    }
    if (auto reset = ResetAggregates(); !reset.has_value()) {
      return reset;
    }
    if (auto copied = CopyCurrentGroup(); !copied.has_value()) {
      return copied;
    }
    if (auto stepped = StepAggregates(source, true); !stepped.has_value()) {
      return stepped;
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return jumped;
    }

    if (stopped.has_value()) {
      if (auto bound = BindLabel(*stopped); !bound.has_value()) {
        return bound;
      }
      if (auto stopped_result = StopAtCompletedGroup(); !stopped_result.has_value()) {
        return stopped_result;
      }
    }

    if (auto bound = BindLabel(*first); !bound.has_value()) {
      return bound;
    }
    if (auto started = StartFirstGroup(source); !started.has_value()) {
      return started;
    }
    return BindLabel(*done);
  }

  [[nodiscard]] LoweringResult<void> InsertSortedRow() {
    if (!input_.group_sorter.has_value() || !input_.group_sorter_record_first.has_value()) {
      return std::unexpected(InternalFailure("sorted grouping record metadata is unavailable"));
    }
    const RegisterId record_first = AssumeValue(input_.group_sorter_record_first);
    if (auto keys = EmitGroupKeys(GroupedExpressionSource::AccessRow, record_first);
        !keys.has_value()) {
      return keys;
    }
    const auto key_count = static_cast<std::uint32_t>(input_.program->group_by.size());
    for (std::size_t index = 0; index < input_.retained_leaves.size(); ++index) {
      if (auto emitted = services_->EmitGroupedExpression(
              input_.retained_leaves[index],
              RegisterId{record_first.value() + key_count + static_cast<std::uint32_t>(index)},
              GroupedExpressionSource::AccessRow);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return Append(InsertSorterInstruction{
        .sorter = AssumeValue(input_.group_sorter),
        .first_value = record_first,
        .value_count = static_cast<std::uint32_t>(input_.program->group_by.size() +
                                                  input_.retained_leaves.size()),
    });
  }

  [[nodiscard]] LoweringResult<void> ReadSortedRow() {
    for (std::size_t index = 0; index < input_.program->group_by.size(); ++index) {
      if (auto read = Append(ReadSorterFieldInstruction{
              .sorter = AssumeValue(input_.group_sorter),
              .field = static_cast<std::uint32_t>(index),
              .output = CurrentGroup(index),
          });
          !read.has_value()) {
        return read;
      }
    }
    const auto key_count = static_cast<std::uint32_t>(input_.program->group_by.size());
    for (std::size_t index = 0; index < input_.retained_leaf_registers.size(); ++index) {
      if (auto read = Append(ReadSorterFieldInstruction{
              .sorter = AssumeValue(input_.group_sorter),
              .field = key_count + static_cast<std::uint32_t>(index),
              .output = input_.retained_leaf_registers[index],
          });
          !read.has_value()) {
        return read;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> FinishActiveGroup(bool close_group_sorter) {
    auto no_group = CreateLabel();
    auto close_active = CreateLabel();
    auto finish = CreateLabel();
    if (!no_group.has_value()) {
      return std::unexpected(std::move(no_group.error()));
    }
    if (!close_active.has_value()) {
      return std::unexpected(std::move(close_active.error()));
    }
    if (!finish.has_value()) {
      return std::unexpected(std::move(finish.error()));
    }

    if (auto jumped = EmitJumpIf(input_.has_group_register, JumpCondition::kIfFalse, *no_group);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto finalized = FinalizeAggregates(); !finalized.has_value()) {
      return finalized;
    }
    if (auto output = services_->EmitCompletedGroupedAggregate(GroupedCompletionTargets{
            .skipped = *close_active,
            .stopped = *close_active,
        });
        !output.has_value()) {
      return output;
    }
    if (auto bound = BindLabel(*close_active); !bound.has_value()) {
      return bound;
    }
    if (auto closed = CloseAggregates(); !closed.has_value()) {
      return closed;
    }
    if (auto jumped = EmitJump(*finish); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*no_group); !bound.has_value()) {
      return bound;
    }
    if (auto finalized = FinalizeAggregates(); !finalized.has_value()) {
      return finalized;
    }
    if (auto closed = CloseAggregates(); !closed.has_value()) {
      return closed;
    }
    if (auto bound = BindLabel(*finish); !bound.has_value()) {
      return bound;
    }
    if (close_group_sorter) {
      if (!input_.group_sorter.has_value()) {
        return std::unexpected(InternalFailure("group sorter close is unavailable"));
      }
      if (auto closed = Append(CloseSorterInstruction{.sorter = AssumeValue(input_.group_sorter)});
          !closed.has_value()) {
        return closed;
      }
    }
    return services_->EmitGroupedOutputCompletion();
  }

  [[nodiscard]] LoweringResult<void> DrainSortedRows() {
    auto empty = CreateLabel();
    auto row = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    auto rewound = ConvertProgramResult(
        input_.builder->EmitRewindSorter(AssumeValue(input_.group_sorter), *empty),
        "unable to emit grouped sorter rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto read = ReadSortedRow(); !read.has_value()) {
      return read;
    }
    if (auto consumed = ConsumeCurrentRow(GroupedExpressionSource::SortedRow);
        !consumed.has_value()) {
      return consumed;
    }
    auto advanced =
        ConvertProgramResult(input_.builder->EmitNextSorter(AssumeValue(input_.group_sorter), *row),
                             "unable to emit grouped sorter advance");
    if (!advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    if (auto finished = FinishActiveGroup(true); !finished.has_value()) {
      return finished;
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    return FinishActiveGroup(true);
  }

  const GroupedAggregateInput& input_;
  GroupedAggregateServices* services_;
};

[[nodiscard]] LoweringResult<void> ValidateInput(const GroupedAggregateInput& input) {
  if (input.builder == nullptr || input.program == nullptr || input.aggregate == nullptr ||
      input.program->group_by.empty() ||
      input.representative_registers.size() != input.program->representative_leaves.size() ||
      input.retained_leaves.size() != input.retained_leaf_registers.size()) {
    return std::unexpected(InternalFailure("grouped aggregate input is incomplete"));
  }
  for (const BoundAggregateInvocationId invocation_id : input.program->invocations) {
    if (invocation_id.value() >= input.aggregate_invocations.size() ||
        invocation_id.value() >= input.aggregate_descriptors.size() ||
        invocation_id.value() >= input.aggregate_argument_blocks.size() ||
        invocation_id.value() >= input.aggregate_result_registers.size() ||
        !input.aggregate_descriptors[invocation_id.value()].has_value() ||
        !input.aggregate_argument_blocks[invocation_id.value()].has_value() ||
        !input.aggregate_result_registers[invocation_id.value()].has_value()) {
      return std::unexpected(
          InternalFailure("grouped aggregate invocation metadata is incomplete"));
    }
    const BoundAggregateInvocation& invocation = input.aggregate_invocations[invocation_id.value()];
    if (invocation.function.value() >= input.aggregate_functions.size()) {
      return std::unexpected(InternalFailure("grouped aggregate function is unavailable"));
    }
  }
  if (input.aggregate->strategy == PhysicalAggregateStrategy::kSortedGroup &&
      (!input.group_sorter.has_value() || !input.group_sorter_record_first.has_value())) {
    return std::unexpected(InternalFailure("sorted grouped aggregate input is incomplete"));
  }
  if (input.aggregate->strategy != PhysicalAggregateStrategy::kOrderedGroup &&
      input.aggregate->strategy != PhysicalAggregateStrategy::kSortedGroup) {
    return std::unexpected(InternalFailure("grouped aggregate strategy is invalid"));
  }
  return {};
}

}  // namespace

LoweringResult<void> EmitGroupedAggregateInitialization(const GroupedAggregateInput& input) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return GroupedAggregateEmitter(input, nullptr).Initialize();
}

LoweringResult<void> EmitGroupedAggregateRow(const GroupedAggregateInput& input,
                                             GroupedAggregateServices& services) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return GroupedAggregateEmitter(input, &services).EmitRow();
}

LoweringResult<void> EmitGroupedAggregateCompletion(const GroupedAggregateInput& input,
                                                    GroupedAggregateServices& services) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return GroupedAggregateEmitter(input, &services).Complete();
}

}  // namespace modern_sqlite::lowering_internal
