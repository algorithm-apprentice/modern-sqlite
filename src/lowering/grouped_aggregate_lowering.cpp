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
    if (services_ == nullptr) {
      return std::unexpected(InternalFailure("grouped aggregate services are unavailable"));
    }
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
    return services_->OpenGroupedAggregateInvocations();
  }

  [[nodiscard]] LoweringResult<void> EmitRow() {
    if (services_ == nullptr) {
      return std::unexpected(InternalFailure("grouped aggregate services are unavailable"));
    }
    if (input_.aggregate->strategy == PhysicalAggregateStrategy::kOrderedGroup) {
      if (auto keys =
              EmitGroupKeys(AggregateExpressionSource::AccessRow, input_.current_group_first);
          !keys.has_value()) {
        return keys;
      }
      return ConsumeCurrentRow(AggregateExpressionSource::AccessRow);
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

  [[nodiscard]] LoweringResult<void> EmitGroupKeys(AggregateExpressionSource source,
                                                   RegisterId first) {
    for (std::size_t index = 0; index < input_.program->group_by.size(); ++index) {
      if (auto emitted = services_->EmitAggregateExpression(
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

  [[nodiscard]] LoweringResult<void> CaptureRepresentative(AggregateExpressionSource source) {
    for (std::size_t index = 0; index < input_.program->representative_leaves.size(); ++index) {
      if (auto emitted =
              services_->EmitAggregateExpression(input_.program->representative_leaves[index],
                                                 input_.representative_registers[index], source);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> StepAggregates(AggregateExpressionSource source,
                                                    bool first_row) {
    const bool has_representative = !input_.program->representative_leaves.empty();
    const bool has_controller = has_representative && input_.has_representative_controller;
    if (has_controller && (!input_.representative_effect_register.has_value() ||
                           !input_.representative_replace_register.has_value())) {
      return std::unexpected(
          InternalFailure("grouped representative effect registers are unavailable"));
    }

    if (auto stepped = services_->StepGroupedAggregateInvocations(source, first_row);
        !stepped.has_value()) {
      return stepped;
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

  [[nodiscard]] LoweringResult<void> StartFirstGroup(AggregateExpressionSource source) {
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

  [[nodiscard]] bool OutputMayContinue() const noexcept {
    return !input_.stops_after_output || input_.program->having.has_value();
  }

  [[nodiscard]] LoweringResult<void> StopAtCompletedGroup(bool source_row_active) {
    if (auto closed = services_->CloseGroupedAggregateInvocations(); !closed.has_value()) {
      return closed;
    }
    if (input_.group_sorter.has_value()) {
      if (auto closed = Append(CloseSorterInstruction{.sorter = AssumeValue(input_.group_sorter)});
          !closed.has_value()) {
        return closed;
      }
    }
    return services_->EmitGroupedEarlyStopCompletion(source_row_active);
  }

  [[nodiscard]] LoweringResult<void> ConsumeCurrentRow(AggregateExpressionSource source) {
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
    if (auto finalized = services_->FinalizeGroupedAggregateInvocations(); !finalized.has_value()) {
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
    if (OutputMayContinue()) {
      if (auto reset = services_->ResetGroupedAggregateInvocations(); !reset.has_value()) {
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
    }

    if (stopped.has_value()) {
      if (auto bound = BindLabel(*stopped); !bound.has_value()) {
        return bound;
      }
      if (auto stopped_result =
              StopAtCompletedGroup(source == AggregateExpressionSource::AccessRow);
          !stopped_result.has_value()) {
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
    if (auto keys = EmitGroupKeys(AggregateExpressionSource::AccessRow, record_first);
        !keys.has_value()) {
      return keys;
    }
    const auto key_count = static_cast<std::uint32_t>(input_.program->group_by.size());
    for (std::size_t index = 0; index < input_.retained_leaves.size(); ++index) {
      if (auto emitted = services_->EmitAggregateExpression(
              input_.retained_leaves[index],
              RegisterId{record_first.value() + key_count + static_cast<std::uint32_t>(index)},
              AggregateExpressionSource::AccessRow);
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
    std::optional<Label> stopped;
    if (input_.may_stop_early) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      stopped = *label;
    }

    if (auto jumped = EmitJumpIf(input_.has_group_register, JumpCondition::kIfFalse, *no_group);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto finalized = services_->FinalizeGroupedAggregateInvocations(); !finalized.has_value()) {
      return finalized;
    }
    if (auto output = services_->EmitCompletedGroupedAggregate(GroupedCompletionTargets{
            .skipped = *close_active,
            .stopped = stopped.value_or(*close_active),
        });
        !output.has_value()) {
      return output;
    }
    if (OutputMayContinue()) {
      if (auto bound = BindLabel(*close_active); !bound.has_value()) {
        return bound;
      }
      if (auto closed = services_->CloseGroupedAggregateInvocations(); !closed.has_value()) {
        return closed;
      }
      if (auto jumped = EmitJump(*finish); !jumped.has_value()) {
        return jumped;
      }
    }
    if (auto bound = BindLabel(*no_group); !bound.has_value()) {
      return bound;
    }
    if (auto finalized = services_->FinalizeGroupedAggregateInvocations(); !finalized.has_value()) {
      return finalized;
    }
    if (auto closed = services_->CloseGroupedAggregateInvocations(); !closed.has_value()) {
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
    if (auto completed = services_->EmitGroupedOutputCompletion(); !completed.has_value()) {
      return completed;
    }
    if (stopped.has_value()) {
      if (!OutputMayContinue()) {
        if (auto bound = BindLabel(*close_active); !bound.has_value()) {
          return bound;
        }
      }
      if (auto bound = BindLabel(*stopped); !bound.has_value()) {
        return bound;
      }
      return StopAtCompletedGroup(false);
    }
    return {};
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
    if (auto consumed = ConsumeCurrentRow(AggregateExpressionSource::SortedRow);
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
  if (input.builder == nullptr || input.program == nullptr || input.aggregate == nullptr) {
    return std::unexpected(InternalFailure("grouped aggregate services are incomplete"));
  }
  if (input.program->group_by.empty()) {
    return std::unexpected(InternalFailure("grouped aggregate program has no grouping terms"));
  }
  if (input.stops_after_output && !input.may_stop_early) {
    return std::unexpected(InternalFailure("grouped output stop has no early-stop lifecycle"));
  }
  if (input.representative_registers.size() != input.program->representative_leaves.size()) {
    return std::unexpected(
        InternalFailure("grouped aggregate representative register count is invalid"));
  }
  if (input.retained_leaves.size() != input.retained_leaf_registers.size()) {
    return std::unexpected(InternalFailure("grouped aggregate retained register count is invalid"));
  }
  if (input.has_representative_controller && (!input.representative_effect_register.has_value() ||
                                              !input.representative_replace_register.has_value())) {
    return std::unexpected(
        InternalFailure("grouped aggregate representative metadata is incomplete"));
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

LoweringResult<void> EmitGroupedAggregateInitialization(const GroupedAggregateInput& input,
                                                        GroupedAggregateServices& services) {
  if (auto valid = ValidateInput(input); !valid.has_value()) {
    return valid;
  }
  return GroupedAggregateEmitter(input, &services).Initialize();
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
