#include "global_aggregate_access.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>

namespace modern_sqlite::lowering_internal {
namespace {

class GlobalAggregateAccessEmitter final {
 public:
  GlobalAggregateAccessEmitter(const GlobalAggregateAccessInput& input,
                               GlobalAggregateAccessServices& services) noexcept
      : services_(services),
        builder_(input.builder),
        bound_select_(input.bound_select),
        expressions_(input.expressions),
        aggregate_functions_(input.aggregate_functions),
        aggregate_invocations_(input.aggregate_invocations),
        aggregate_descriptors_(input.aggregate_descriptors),
        aggregate_argument_blocks_(input.aggregate_argument_blocks),
        leaf_(input.leaf),
        table_scan_(input.table_scan),
        rowid_lookup_(input.rowid_lookup),
        index_scan_(input.index_scan),
        filter_(input.filter),
        limit_(input.limit),
        aggregate_(input.aggregate),
        cursor_(input.cursor),
        index_cursor_(input.index_cursor),
        index_key_first_(input.index_key_first),
        index_key_capacity_(input.index_key_capacity),
        index_rowid_register_(input.index_rowid_register),
        indexed_min_max_null_constant_(input.indexed_min_max_null_constant) {}

  [[nodiscard]] LoweringResult<void> Run() { return EmitGlobalAggregatePlan(); }

 private:
  [[nodiscard]] LoweringResult<void> Append(Instruction instruction) {
    auto appended = ConvertProgramResult(builder_->Append(instruction),
                                         "unable to append a bytecode instruction");
    if (!appended.has_value()) {
      return std::unexpected(std::move(appended.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<Label> CreateLabel() {
    return ConvertProgramResult(builder_->CreateLabel(), "unable to create a bytecode label");
  }

  [[nodiscard]] LoweringResult<void> BindLabel(Label label) {
    auto bound =
        ConvertProgramResult(builder_->BindLabel(label), "unable to bind a bytecode label");
    if (!bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJump(Label target) {
    auto emitted = ConvertProgramResult(builder_->EmitJump(target), kEmitJumpFailureDetail);
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJumpIf(RegisterId input, JumpCondition condition,
                                                Label target) {
    auto emitted = ConvertProgramResult(builder_->EmitJumpIf(input, condition, target),
                                        kEmitConditionalJumpFailureDetail);
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return {};
  }

  [[nodiscard]] static RegisterId Home(BoundExpressionId id) noexcept {
    return RegisterId{id.value()};
  }

  [[nodiscard]] LoweringResult<void> EmitExpression(BoundExpressionId id, RegisterId destination) {
    return services_.EmitExpression(id, destination);
  }

  [[nodiscard]] LoweringResult<void> EmitLimitInitialization(std::optional<Label> completion) {
    return services_.EmitLimitInitialization(completion);
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateGuards(Label empty_input) {
    return services_.EmitGlobalAggregateGuards(empty_input);
  }

  [[nodiscard]] LoweringResult<void> EmitFilters(Label rejected) {
    return services_.EmitFilters(rejected);
  }

  [[nodiscard]] LoweringResult<void> OpenGlobalAggregates() {
    return services_.OpenGlobalAggregates();
  }

  [[nodiscard]] LoweringResult<void> StepGlobalAggregates() {
    return services_.StepGlobalAggregates();
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateCompletion() {
    return services_.EmitGlobalAggregateCompletion();
  }

  [[nodiscard]] LoweringResult<void> EmitIndexKeyExpressions(const PhysicalIndexScanNode& index,
                                                             std::optional<Label> null_target) {
    return services_.EmitIndexKeyExpressions(index, null_target);
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexKeyBlock(
      const PhysicalIndexScanNode& index, const PhysicalIndexBound* range_bound) {
    return services_.EmitIndexKeyBlock(index, range_bound);
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateEmpty() {
    std::optional<Label> preopen_completion;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      preopen_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(preopen_completion); !limit.has_value()) {
      return limit;
    }
    if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
      return opened;
    }
    std::optional<Label> guard_empty;
    if (filter_ != nullptr && !filter_->guards.empty()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      guard_empty = *label;
      if (auto guards = EmitGlobalAggregateGuards(*guard_empty); !guards.has_value()) {
        return guards;
      }
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (guard_empty.has_value()) {
      if (auto bound = BindLabel(*guard_empty); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (preopen_completion.has_value()) {
      if (auto bound = BindLabel(*preopen_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateSingleRow() {
    std::optional<Label> preopen_completion;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      preopen_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(preopen_completion); !limit.has_value()) {
      return limit;
    }
    if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
      return opened;
    }
    std::optional<Label> empty_input;
    if (filter_ != nullptr && (!filter_->guards.empty() || !filter_->predicates.empty())) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      empty_input = *label;
      if (auto guards = EmitGlobalAggregateGuards(*empty_input); !guards.has_value()) {
        return guards;
      }
      if (auto filters = EmitFilters(*empty_input); !filters.has_value()) {
        return filters;
      }
    }
    if (auto stepped = StepGlobalAggregates(); !stepped.has_value()) {
      return stepped;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (empty_input.has_value()) {
      if (auto bound = BindLabel(*empty_input); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (preopen_completion.has_value()) {
      if (auto bound = BindLabel(*preopen_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateTableScan() {
    if (!cursor_.has_value()) {
      return std::unexpected(InternalFailure("global aggregate table scan has no cursor"));
    }
    std::optional<Label> preopen_completion;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      preopen_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(preopen_completion); !limit.has_value()) {
      return limit;
    }
    if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
      return opened;
    }
    std::optional<Label> guard_empty;
    if (filter_ != nullptr && !filter_->guards.empty()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      guard_empty = *label;
      if (auto guards = EmitGlobalAggregateGuards(*guard_empty); !guards.has_value()) {
        return guards;
      }
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }
    auto exhausted = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    if (!exhausted.has_value()) {
      return std::unexpected(std::move(exhausted.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    auto rewound = ConvertProgramResult(builder_->EmitRewind(AssumeValue(cursor_), *exhausted),
                                        "unable to emit global aggregate table rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto filters = EmitFilters(*advance); !filters.has_value()) {
      return filters;
    }
    if (auto stepped = StepGlobalAggregates(); !stepped.has_value()) {
      return stepped;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(builder_->EmitNext(AssumeValue(cursor_), *row),
                                     "unable to emit global aggregate table advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
        !closed.has_value()) {
      return closed;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (guard_empty.has_value()) {
      if (auto bound = BindLabel(*guard_empty); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (preopen_completion.has_value()) {
      if (auto bound = BindLabel(*preopen_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateCoveringIndexScan(
      const PhysicalIndexScanNode& index) {
    if (!index.covering || index.direction != PhysicalScanDirection::kForward ||
        !cursor_.has_value()) {
      return std::unexpected(
          InternalFailure("global aggregate covering index metadata is invalid"));
    }
    const PhysicalIndexBound* physical_start = nullptr;
    const PhysicalIndexBound* physical_end = nullptr;
    if (index.range.has_value()) {
      if (index.range->order == SortOrder::kDescending) {
        physical_start = index.range->upper.has_value() ? &*index.range->upper : nullptr;
        physical_end = index.range->lower.has_value() ? &*index.range->lower : nullptr;
      } else {
        physical_start = index.range->lower.has_value() ? &*index.range->lower : nullptr;
        physical_end = index.range->upper.has_value() ? &*index.range->upper : nullptr;
      }
    }
    const bool has_start_key = !index.equalities.empty() || physical_start != nullptr;
    const bool has_end_key = !index.equalities.empty() || physical_end != nullptr;
    const bool rejects_null = std::ranges::any_of(index.equalities,
                                                  [](const PhysicalIndexEquality& equality) {
                                                    return equality.reject_null;
                                                  }) ||
                              index.range.has_value();

    std::optional<Label> preopen_completion;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      preopen_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(preopen_completion); !limit.has_value()) {
      return limit;
    }
    if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
      return opened;
    }
    std::optional<Label> opened_empty;
    if ((filter_ != nullptr && !filter_->guards.empty()) || rejects_null) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      opened_empty = *label;
    }
    if (opened_empty.has_value()) {
      if (auto guards = EmitGlobalAggregateGuards(*opened_empty); !guards.has_value()) {
        return guards;
      }
    }
    if (!index.equalities.empty() || index.range.has_value()) {
      if (auto keys = EmitIndexKeyExpressions(index, opened_empty); !keys.has_value()) {
        return keys;
      }
    }

    std::uint32_t start_key_count = 0;
    if (has_start_key) {
      auto key = EmitIndexKeyBlock(index, physical_start);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      start_key_count = *key;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }
    auto unpositioned = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    if (!unpositioned.has_value()) {
      return std::unexpected(std::move(unpositioned.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (has_start_key) {
      const IndexSeekMode mode = physical_start == nullptr || physical_start->inclusive
                                     ? IndexSeekMode::kGreaterOrEqual
                                     : IndexSeekMode::kGreater;
      auto sought = ConvertProgramResult(
          builder_->EmitSeekIndex(AssumeValue(cursor_), AssumeValue(index_key_first_),
                                  start_key_count, *unpositioned, mode),
          "unable to emit global aggregate index seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      auto rewound = ConvertProgramResult(builder_->EmitRewind(AssumeValue(cursor_), *unpositioned),
                                          "unable to emit global aggregate index rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }

    std::uint32_t end_key_count = 0;
    if (has_end_key) {
      auto key = EmitIndexKeyBlock(index, physical_end);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      end_key_count = *key;
    }
    std::optional<Label> positioned_end;
    if (has_end_key) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_end = *label;
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (has_end_key) {
      const IndexRangeEndMode mode = physical_end == nullptr || physical_end->inclusive
                                         ? IndexRangeEndMode::kInclusive
                                         : IndexRangeEndMode::kExclusive;
      auto checked = ConvertProgramResult(
          builder_->EmitCheckIndexRange(AssumeValue(cursor_), AssumeValue(index_key_first_),
                                        end_key_count, AssumeValue(positioned_end), mode),
          "unable to emit global aggregate index end check");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (auto filters = EmitFilters(*advance); !filters.has_value()) {
      return filters;
    }
    if (auto stepped = StepGlobalAggregates(); !stepped.has_value()) {
      return stepped;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(builder_->EmitNext(AssumeValue(cursor_), *row),
                                     "unable to emit global aggregate index advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }

    if (auto bound = BindLabel(*unpositioned); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
        !closed.has_value()) {
      return closed;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (positioned_end.has_value()) {
      if (auto bound = BindLabel(*positioned_end); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
          !closed.has_value()) {
        return closed;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (opened_empty.has_value()) {
      if (auto bound = BindLabel(*opened_empty); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (preopen_completion.has_value()) {
      if (auto bound = BindLabel(*preopen_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateRowIdLookup(
      const PhysicalRowIdLookupNode& lookup) {
    if (!cursor_.has_value()) {
      return std::unexpected(InternalFailure("global aggregate rowid lookup has no cursor"));
    }
    std::optional<Label> preopen_completion;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      preopen_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(preopen_completion); !limit.has_value()) {
      return limit;
    }
    if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
      return opened;
    }
    std::optional<Label> guard_empty;
    if (filter_ != nullptr && !filter_->guards.empty()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      guard_empty = *label;
      if (auto guards = EmitGlobalAggregateGuards(*guard_empty); !guards.has_value()) {
        return guards;
      }
    }
    const RegisterId key = Home(lookup.key);
    if (auto emitted = EmitExpression(lookup.key, key); !emitted.has_value()) {
      return emitted;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }
    auto missing = CreateLabel();
    if (!missing.has_value()) {
      return std::unexpected(std::move(missing.error()));
    }
    std::optional<Label> rejected;
    if (filter_ != nullptr && !filter_->predicates.empty()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      rejected = *label;
    }
    auto sought = ConvertProgramResult(builder_->EmitSeekRowId(AssumeValue(cursor_), key, *missing),
                                       "unable to emit global aggregate rowid seek");
    if (!sought.has_value()) {
      return std::unexpected(std::move(sought.error()));
    }
    if (rejected.has_value()) {
      if (auto filters = EmitFilters(*rejected); !filters.has_value()) {
        return filters;
      }
    }
    if (auto stepped = StepGlobalAggregates(); !stepped.has_value()) {
      return stepped;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
        !closed.has_value()) {
      return closed;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (rejected.has_value()) {
      if (auto bound = BindLabel(*rejected); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
          !closed.has_value()) {
        return closed;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (auto bound = BindLabel(*missing); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
        !closed.has_value()) {
      return closed;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (guard_empty.has_value()) {
      if (auto bound = BindLabel(*guard_empty); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (preopen_completion.has_value()) {
      if (auto bound = BindLabel(*preopen_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateNoncoveringIndexScan(
      const PhysicalIndexScanNode& index) {
    if (index.covering || index.direction != PhysicalScanDirection::kForward ||
        !cursor_.has_value() || !index_cursor_.has_value() || !index_rowid_register_.has_value() ||
        (index.equalities.empty() && !index.range.has_value())) {
      return std::unexpected(
          InternalFailure("global aggregate noncovering index metadata is invalid"));
    }
    const CursorId index_cursor = AssumeValue(index_cursor_);
    const CursorId table_cursor = AssumeValue(cursor_);
    const PhysicalIndexBound* physical_start = nullptr;
    const PhysicalIndexBound* physical_end = nullptr;
    if (index.range.has_value()) {
      if (index.range->order == SortOrder::kDescending) {
        physical_start = index.range->upper.has_value() ? &*index.range->upper : nullptr;
        physical_end = index.range->lower.has_value() ? &*index.range->lower : nullptr;
      } else {
        physical_start = index.range->lower.has_value() ? &*index.range->lower : nullptr;
        physical_end = index.range->upper.has_value() ? &*index.range->upper : nullptr;
      }
    }
    const bool has_start_key = !index.equalities.empty() || physical_start != nullptr;
    const bool has_end_key = !index.equalities.empty() || physical_end != nullptr;
    const bool rejects_null = std::ranges::any_of(index.equalities,
                                                  [](const PhysicalIndexEquality& equality) {
                                                    return equality.reject_null;
                                                  }) ||
                              index.range.has_value();

    std::optional<Label> preopen_completion;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      preopen_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(preopen_completion); !limit.has_value()) {
      return limit;
    }
    if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
      return opened;
    }
    std::optional<Label> opened_empty;
    if ((filter_ != nullptr && !filter_->guards.empty()) || rejects_null) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      opened_empty = *label;
    }
    if (opened_empty.has_value()) {
      if (auto guards = EmitGlobalAggregateGuards(*opened_empty); !guards.has_value()) {
        return guards;
      }
    }
    if (auto keys = EmitIndexKeyExpressions(index, opened_empty); !keys.has_value()) {
      return keys;
    }

    std::uint32_t start_key_count = 0;
    if (has_start_key) {
      auto key = EmitIndexKeyBlock(index, physical_start);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      start_key_count = *key;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = index_cursor});
        !opened.has_value()) {
      return opened;
    }
    auto index_missing = CreateLabel();
    if (!index_missing.has_value()) {
      return std::unexpected(std::move(index_missing.error()));
    }
    if (has_start_key) {
      const IndexSeekMode mode = physical_start == nullptr || physical_start->inclusive
                                     ? IndexSeekMode::kGreaterOrEqual
                                     : IndexSeekMode::kGreater;
      auto sought =
          ConvertProgramResult(builder_->EmitSeekIndex(index_cursor, AssumeValue(index_key_first_),
                                                       start_key_count, *index_missing, mode),
                               "unable to emit global aggregate noncovering seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      auto rewound = ConvertProgramResult(builder_->EmitRewind(index_cursor, *index_missing),
                                          "unable to emit global aggregate noncovering rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }

    std::uint32_t end_key_count = 0;
    if (has_end_key) {
      auto key = EmitIndexKeyBlock(index, physical_end);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      end_key_count = *key;
    }
    std::optional<Label> first_end;
    std::optional<Label> positioned_end;
    if (has_end_key) {
      auto first = CreateLabel();
      auto positioned = CreateLabel();
      if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
      }
      if (!positioned.has_value()) {
        return std::unexpected(std::move(positioned.error()));
      }
      first_end = *first;
      positioned_end = *positioned;
    }
    auto subsequent = CreateLabel();
    auto process = CreateLabel();
    auto advance = CreateLabel();
    auto exhausted = CreateLabel();
    if (!subsequent.has_value()) {
      return std::unexpected(std::move(subsequent.error()));
    }
    if (!process.has_value()) {
      return std::unexpected(std::move(process.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (!exhausted.has_value()) {
      return std::unexpected(std::move(exhausted.error()));
    }

    if (has_end_key) {
      const IndexRangeEndMode mode = physical_end == nullptr || physical_end->inclusive
                                         ? IndexRangeEndMode::kInclusive
                                         : IndexRangeEndMode::kExclusive;
      auto checked = ConvertProgramResult(
          builder_->EmitCheckIndexRange(index_cursor, AssumeValue(index_key_first_), end_key_count,
                                        AssumeValue(first_end), mode),
          "unable to emit first global aggregate noncovering end check");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (auto rowid = Append(ReadFieldInstruction{
            .cursor = index_cursor,
            .field = CursorFieldId{0},
            .output = AssumeValue(index_rowid_register_),
        });
        !rowid.has_value()) {
      return rowid;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = table_cursor});
        !opened.has_value()) {
      return opened;
    }
    if (auto sought = Append(SeekTableRowIdInstruction{
            .cursor = table_cursor,
            .key = AssumeValue(index_rowid_register_),
        });
        !sought.has_value()) {
      return sought;
    }
    if (auto jumped = EmitJump(*process); !jumped.has_value()) {
      return jumped;
    }

    if (auto bound = BindLabel(*subsequent); !bound.has_value()) {
      return bound;
    }
    if (has_end_key) {
      const IndexRangeEndMode mode = physical_end == nullptr || physical_end->inclusive
                                         ? IndexRangeEndMode::kInclusive
                                         : IndexRangeEndMode::kExclusive;
      auto checked = ConvertProgramResult(
          builder_->EmitCheckIndexRange(index_cursor, AssumeValue(index_key_first_), end_key_count,
                                        AssumeValue(positioned_end), mode),
          "unable to emit global aggregate noncovering end check");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (auto rowid = Append(ReadFieldInstruction{
            .cursor = index_cursor,
            .field = CursorFieldId{0},
            .output = AssumeValue(index_rowid_register_),
        });
        !rowid.has_value()) {
      return rowid;
    }
    if (auto sought = Append(SeekTableRowIdInstruction{
            .cursor = table_cursor,
            .key = AssumeValue(index_rowid_register_),
        });
        !sought.has_value()) {
      return sought;
    }
    if (auto bound = BindLabel(*process); !bound.has_value()) {
      return bound;
    }
    if (auto filters = EmitFilters(*advance); !filters.has_value()) {
      return filters;
    }
    if (auto stepped = StepGlobalAggregates(); !stepped.has_value()) {
      return stepped;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(builder_->EmitNext(index_cursor, *subsequent),
                                     "unable to emit global aggregate noncovering advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }

    if (auto bound = BindLabel(*exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = table_cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = index_cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (first_end.has_value()) {
      if (auto bound = BindLabel(*first_end); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = index_cursor});
          !closed.has_value()) {
        return closed;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (positioned_end.has_value()) {
      if (auto bound = BindLabel(*positioned_end); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = table_cursor});
          !closed.has_value()) {
        return closed;
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = index_cursor});
          !closed.has_value()) {
        return closed;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (auto bound = BindLabel(*index_missing); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = index_cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (opened_empty.has_value()) {
      if (auto bound = BindLabel(*opened_empty); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (preopen_completion.has_value()) {
      if (auto bound = BindLabel(*preopen_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  struct IndexedMinMaxTraversal {
    const PhysicalIndexBound* start = nullptr;
    const PhysicalIndexBound* end = nullptr;
    bool has_start_key = false;
    bool has_end_key = false;
  };

  struct IndexedMinMaxRuntime {
    AggregateId aggregate;
    BoundExpressionId argument;
    RegisterId argument_register;
    BoundAggregateExtremumKind extremum = BoundAggregateExtremumKind::kNone;
    bool nullable = false;
  };

  enum class IndexedMinMaxNullPolicyKind : std::uint8_t {
    kNone,
    kUnconstrained,
    kRuntimeIs,
  };

  struct IndexedMinMaxNullPolicy {
    IndexedMinMaxNullPolicyKind kind = IndexedMinMaxNullPolicyKind::kNone;
    std::optional<BoundExpressionId> runtime_key{};
  };

  enum class IndexedMinMaxPassRegion : std::uint8_t {
    kSelected,
    kNonNull,
    kNull,
  };

  struct IndexedMinMaxPassExits {
    Label missing;
    std::optional<Label> first_end{};
    std::optional<Label> exhausted{};
    std::optional<Label> positioned_end{};
  };

  [[nodiscard]] IndexedMinMaxTraversal ResolveIndexedMinMaxTraversal(
      const PhysicalIndexScanNode& index) const noexcept {
    const PhysicalIndexBound* forward_start = nullptr;
    const PhysicalIndexBound* forward_end = nullptr;
    if (index.range.has_value()) {
      if (index.range->order == SortOrder::kDescending) {
        forward_start = index.range->upper.has_value() ? &*index.range->upper : nullptr;
        forward_end = index.range->lower.has_value() ? &*index.range->lower : nullptr;
      } else {
        forward_start = index.range->lower.has_value() ? &*index.range->lower : nullptr;
        forward_end = index.range->upper.has_value() ? &*index.range->upper : nullptr;
      }
    }
    const bool reverse = index.direction == PhysicalScanDirection::kReverse;
    const PhysicalIndexBound* start = reverse ? forward_end : forward_start;
    const PhysicalIndexBound* end = reverse ? forward_start : forward_end;
    return IndexedMinMaxTraversal{
        .start = start,
        .end = end,
        .has_start_key = !index.equalities.empty() || start != nullptr,
        .has_end_key = !index.equalities.empty() || end != nullptr,
    };
  }

  [[nodiscard]] LoweringResult<IndexedMinMaxRuntime> ResolveIndexedMinMaxRuntime() const {
    if (aggregate_ == nullptr || !aggregate_->min_max.has_value()) {
      return std::unexpected(InternalFailure("indexed MIN/MAX metadata is unavailable"));
    }
    const BoundAggregateInvocationId invocation_id = aggregate_->min_max->invocation;
    if (invocation_id.value() >= aggregate_invocations_.size() ||
        invocation_id.value() >= aggregate_descriptors_.size() ||
        invocation_id.value() >= aggregate_argument_blocks_.size() ||
        !aggregate_descriptors_[invocation_id.value()].has_value() ||
        !aggregate_argument_blocks_[invocation_id.value()].has_value()) {
      return std::unexpected(InternalFailure("indexed MIN/MAX runtime metadata is incomplete"));
    }
    const BoundAggregateInvocation& invocation = aggregate_invocations_[invocation_id.value()];
    if (invocation.function.value() >= aggregate_functions_.size() ||
        invocation.arguments.size() != 1U) {
      return std::unexpected(InternalFailure("indexed MIN/MAX invocation is invalid"));
    }
    const BoundAggregateExtremumKind extremum =
        aggregate_functions_[invocation.function.value()].extremum;
    if (extremum == BoundAggregateExtremumKind::kNone) {
      return std::unexpected(InternalFailure("indexed MIN/MAX extremum is invalid"));
    }
    return IndexedMinMaxRuntime{
        .aggregate = AssumeValue(aggregate_descriptors_[invocation_id.value()]),
        .argument = invocation.arguments.front(),
        .argument_register = AssumeValue(aggregate_argument_blocks_[invocation_id.value()]),
        .extremum = extremum,
        .nullable = aggregate_->min_max->null_strategy ==
                    PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback,
    };
  }

  [[nodiscard]] std::optional<ColumnId> ResolveIndexedMinMaxArgumentColumn(
      BoundExpressionId id) const noexcept {
    while (id.value() < expressions_.size()) {
      const BoundExpression& expression = expressions_[id.value()];
      if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
          alias != nullptr) {
        id = alias->target;
        continue;
      }
      if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
          collate != nullptr) {
        id = collate->operand;
        continue;
      }
      const auto* column = std::get_if<BoundColumnExpression>(&expression.payload);
      if (column == nullptr || column->column.value() >= bound_select_->source_columns().size()) {
        return std::nullopt;
      }
      return bound_select_->source_columns()[column->column.value()].catalog_column;
    }
    return std::nullopt;
  }

  [[nodiscard]] LoweringResult<IndexedMinMaxNullPolicy> ResolveIndexedMinMaxNullPolicy(
      const PhysicalIndexScanNode& index, const IndexedMinMaxRuntime& runtime) const {
    if (!runtime.nullable) {
      return IndexedMinMaxNullPolicy{};
    }
    if (aggregate_ == nullptr || !aggregate_->min_max.has_value()) {
      return std::unexpected(InternalFailure("indexed MIN/MAX NULL metadata is unavailable"));
    }
    if (!aggregate_->min_max->argument_fixed_by_equality) {
      return IndexedMinMaxNullPolicy{
          .kind = index.range.has_value() ? IndexedMinMaxNullPolicyKind::kNone
                                          : IndexedMinMaxNullPolicyKind::kUnconstrained,
      };
    }
    const std::optional<ColumnId> argument_column =
        ResolveIndexedMinMaxArgumentColumn(runtime.argument);
    if (!argument_column.has_value()) {
      return std::unexpected(InternalFailure("indexed MIN/MAX equality has no argument column"));
    }
    const auto equality =
        std::ranges::find(index.equalities, *argument_column, &PhysicalIndexEquality::column);
    if (equality == index.equalities.end()) {
      return std::unexpected(InternalFailure("indexed MIN/MAX equality metadata is inconsistent"));
    }
    if (equality->reject_null) {
      return IndexedMinMaxNullPolicy{};
    }
    return IndexedMinMaxNullPolicy{
        .kind = IndexedMinMaxNullPolicyKind::kRuntimeIs,
        .runtime_key = equality->key,
    };
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxPosition(
      const PhysicalIndexScanNode& index, const IndexedMinMaxTraversal& traversal,
      CursorId index_cursor, Label missing) {
    if (traversal.has_start_key) {
      auto key_count = EmitIndexKeyBlock(index, traversal.start);
      if (!key_count.has_value()) {
        return std::unexpected(std::move(key_count.error()));
      }
      IndexSeekMode mode = IndexSeekMode::kGreaterOrEqual;
      if (index.direction == PhysicalScanDirection::kForward) {
        mode = traversal.start == nullptr || traversal.start->inclusive
                   ? IndexSeekMode::kGreaterOrEqual
                   : IndexSeekMode::kGreater;
      } else {
        mode = traversal.start == nullptr || traversal.start->inclusive
                   ? IndexSeekMode::kLessOrEqual
                   : IndexSeekMode::kLess;
      }
      auto sought =
          ConvertProgramResult(builder_->EmitSeekIndex(index_cursor, AssumeValue(index_key_first_),
                                                       *key_count, missing, mode),
                               "unable to emit indexed MIN/MAX seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
      return {};
    }
    if (index.direction == PhysicalScanDirection::kForward) {
      auto rewound = ConvertProgramResult(builder_->EmitRewind(index_cursor, missing),
                                          "unable to emit indexed MIN/MAX rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
      return {};
    }
    auto last = ConvertProgramResult(builder_->EmitLast(index_cursor, missing),
                                     "unable to emit indexed MIN/MAX last");
    if (!last.has_value()) {
      return std::unexpected(std::move(last.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexedMinMaxEndKey(
      const PhysicalIndexScanNode& index, const IndexedMinMaxTraversal& traversal) {
    if (!traversal.has_end_key) {
      return 0U;
    }
    return EmitIndexKeyBlock(index, traversal.end);
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexedMinMaxNullKey(
      const PhysicalIndexScanNode& index) {
    const std::size_t count = index.equalities.size() + 1U;
    if (!index_key_first_.has_value() || count > index_key_capacity_ ||
        !indexed_min_max_null_constant_.has_value()) {
      return std::unexpected(InternalFailure("indexed MIN/MAX NULL key is unavailable"));
    }
    if (!index.equalities.empty()) {
      auto equality_count = EmitIndexKeyBlock(index, nullptr);
      if (!equality_count.has_value()) {
        return std::unexpected(std::move(equality_count.error()));
      }
    }
    const RegisterId null_register{
        AssumeValue(index_key_first_).value() + static_cast<std::uint32_t>(index.equalities.size()),
    };
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = AssumeValue(indexed_min_max_null_constant_),
            .output = null_register,
        });
        !loaded.has_value()) {
      return std::unexpected(std::move(loaded.error()));
    }
    return static_cast<std::uint32_t>(count);
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexedMinMaxNullPosition(
      const PhysicalIndexScanNode& index, CursorId index_cursor, Label missing) {
    auto key_count = EmitIndexedMinMaxNullKey(index);
    if (!key_count.has_value()) {
      return std::unexpected(std::move(key_count.error()));
    }
    const IndexSeekMode mode = index.direction == PhysicalScanDirection::kForward
                                   ? IndexSeekMode::kGreaterOrEqual
                                   : IndexSeekMode::kLessOrEqual;
    auto sought =
        ConvertProgramResult(builder_->EmitSeekIndex(index_cursor, AssumeValue(index_key_first_),
                                                     *key_count, missing, mode),
                             "unable to emit indexed MIN/MAX NULL seek");
    if (!sought.has_value()) {
      return std::unexpected(std::move(sought.error()));
    }
    return *key_count;
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxNonNullPosition(
      const PhysicalIndexScanNode& index, const IndexedMinMaxTraversal& traversal,
      const IndexedMinMaxRuntime& runtime, CursorId index_cursor, Label missing) {
    if (runtime.extremum == BoundAggregateExtremumKind::kMaximum) {
      return EmitIndexedMinMaxPosition(index, traversal, index_cursor, missing);
    }
    if (runtime.extremum != BoundAggregateExtremumKind::kMinimum) {
      return std::unexpected(InternalFailure("indexed MIN/MAX extremum is invalid"));
    }
    auto key_count = EmitIndexedMinMaxNullKey(index);
    if (!key_count.has_value()) {
      return std::unexpected(std::move(key_count.error()));
    }
    const IndexSeekMode mode = index.direction == PhysicalScanDirection::kForward
                                   ? IndexSeekMode::kGreater
                                   : IndexSeekMode::kLess;
    auto sought =
        ConvertProgramResult(builder_->EmitSeekIndex(index_cursor, AssumeValue(index_key_first_),
                                                     *key_count, missing, mode),
                             "unable to emit indexed MIN/MAX non-NULL seek");
    if (!sought.has_value()) {
      return std::unexpected(std::move(sought.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxEndCheck(
      const PhysicalIndexScanNode& index, const IndexedMinMaxTraversal& traversal,
      CursorId index_cursor, std::uint32_t key_count, Label end) {
    if (!traversal.has_end_key) {
      return {};
    }

    const bool inclusive = traversal.end == nullptr || traversal.end->inclusive;
    const IndexRangeEndMode mode =
        index.direction == PhysicalScanDirection::kForward
            ? (inclusive ? IndexRangeEndMode::kInclusive : IndexRangeEndMode::kExclusive)
            : (inclusive ? IndexRangeEndMode::kReverseInclusive
                         : IndexRangeEndMode::kReverseExclusive);
    auto checked =
        ConvertProgramResult(builder_->EmitCheckIndexRange(
                                 index_cursor, AssumeValue(index_key_first_), key_count, end, mode),
                             "unable to emit indexed MIN/MAX end check");
    if (!checked.has_value()) {
      return std::unexpected(std::move(checked.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxNonNullEndCheck(
      const PhysicalIndexScanNode& index, const IndexedMinMaxTraversal& traversal,
      const IndexedMinMaxRuntime& runtime, CursorId index_cursor, std::uint32_t key_count,
      Label end) {
    if (runtime.extremum == BoundAggregateExtremumKind::kMinimum) {
      return EmitIndexedMinMaxEndCheck(index, traversal, index_cursor, key_count, end);
    }
    if (runtime.extremum != BoundAggregateExtremumKind::kMaximum) {
      return std::unexpected(InternalFailure("indexed MIN/MAX extremum is invalid"));
    }
    const IndexRangeEndMode mode = index.direction == PhysicalScanDirection::kForward
                                       ? IndexRangeEndMode::kExclusive
                                       : IndexRangeEndMode::kReverseExclusive;
    auto checked =
        ConvertProgramResult(builder_->EmitCheckIndexRange(
                                 index_cursor, AssumeValue(index_key_first_), key_count, end, mode),
                             "unable to emit indexed MIN/MAX non-NULL end check");
    if (!checked.has_value()) {
      return std::unexpected(std::move(checked.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxNullEndCheck(
      const PhysicalIndexScanNode& index, CursorId index_cursor, std::uint32_t key_count,
      Label end) {
    const IndexRangeEndMode mode = index.direction == PhysicalScanDirection::kForward
                                       ? IndexRangeEndMode::kInclusive
                                       : IndexRangeEndMode::kReverseInclusive;
    auto checked =
        ConvertProgramResult(builder_->EmitCheckIndexRange(
                                 index_cursor, AssumeValue(index_key_first_), key_count, end, mode),
                             "unable to emit indexed MIN/MAX NULL end check");
    if (!checked.has_value()) {
      return std::unexpected(std::move(checked.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxPassEndCheck(
      const PhysicalIndexScanNode& index, const IndexedMinMaxTraversal& traversal,
      const IndexedMinMaxRuntime& runtime, CursorId index_cursor, std::uint32_t key_count,
      Label end, IndexedMinMaxPassRegion region) {
    if (region == IndexedMinMaxPassRegion::kNull) {
      return EmitIndexedMinMaxNullEndCheck(index, index_cursor, key_count, end);
    }
    if (region == IndexedMinMaxPassRegion::kNonNull) {
      return EmitIndexedMinMaxNonNullEndCheck(index, traversal, runtime, index_cursor, key_count,
                                              end);
    }
    return EmitIndexedMinMaxEndCheck(index, traversal, index_cursor, key_count, end);
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxLookup(CursorId index_cursor,
                                                             std::optional<CursorId> table_cursor) {
    if (!table_cursor.has_value()) {
      return {};
    }
    if (!index_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("indexed MIN/MAX table lookup has no rowid register"));
    }
    if (auto rowid = Append(ReadFieldInstruction{
            .cursor = index_cursor,
            .field = CursorFieldId{0},
            .output = AssumeValue(index_rowid_register_),
        });
        !rowid.has_value()) {
      return rowid;
    }
    return Append(SeekTableRowIdInstruction{
        .cursor = *table_cursor,
        .key = AssumeValue(index_rowid_register_),
    });
  }

  [[nodiscard]] LoweringResult<void> CloseIndexedMinMaxCursors(
      CursorId index_cursor, std::optional<CursorId> table_cursor) {
    if (table_cursor.has_value()) {
      if (auto closed = Append(CloseCursorInstruction{.cursor = *table_cursor});
          !closed.has_value()) {
        return closed;
      }
    }
    return Append(CloseCursorInstruction{.cursor = index_cursor});
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxAdvance(const PhysicalIndexScanNode& index,
                                                              CursorId cursor, Label candidate) {
    if (index.direction == PhysicalScanDirection::kForward) {
      auto next = ConvertProgramResult(builder_->EmitNext(cursor, candidate),
                                       "unable to emit indexed MIN/MAX next");
      if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
      }
      return {};
    }
    auto previous = ConvertProgramResult(builder_->EmitPrevious(cursor, candidate),
                                         "unable to emit indexed MIN/MAX previous");
    if (!previous.has_value()) {
      return std::unexpected(std::move(previous.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxCandidate(
      const IndexedMinMaxRuntime& runtime, std::optional<JumpCondition> skipped_value,
      std::optional<Label> advance) {
    if (auto argument = EmitExpression(runtime.argument, runtime.argument_register);
        !argument.has_value()) {
      return argument;
    }
    if (skipped_value.has_value()) {
      if (!advance.has_value()) {
        return std::unexpected(
            InternalFailure("indexed MIN/MAX skipped value has no advance target"));
      }
      if (auto skipped = EmitJumpIf(runtime.argument_register, *skipped_value, *advance);
          !skipped.has_value()) {
        return skipped;
      }
    }
    if (filter_ != nullptr && !filter_->predicates.empty()) {
      if (!advance.has_value()) {
        return std::unexpected(InternalFailure("indexed MIN/MAX residual has no advance target"));
      }
      if (auto filters = EmitFilters(*advance); !filters.has_value()) {
        return filters;
      }
    }
    return Append(StepAggregateInstruction{
        .aggregate = runtime.aggregate,
        .first_argument = runtime.argument_register,
        .effect_output = std::nullopt,
    });
  }

  [[nodiscard]] LoweringResult<IndexedMinMaxPassExits> EmitIndexedMinMaxIndexPass(
      const PhysicalIndexScanNode& index, const IndexedMinMaxTraversal& traversal,
      const IndexedMinMaxRuntime& runtime, CursorId index_cursor,
      std::optional<CursorId> table_cursor, std::optional<JumpCondition> skipped_value,
      IndexedMinMaxPassRegion region) {
    auto missing = CreateLabel();
    if (!missing.has_value()) {
      return std::unexpected(std::move(missing.error()));
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = index_cursor});
        !opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    if (table_cursor.has_value()) {
      if (auto opened = Append(OpenReadCursorInstruction{.cursor = *table_cursor});
          !opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
    }
    std::uint32_t end_key_count = 0;
    if (region == IndexedMinMaxPassRegion::kNull) {
      auto positioned = EmitIndexedMinMaxNullPosition(index, index_cursor, *missing);
      if (!positioned.has_value()) {
        return std::unexpected(std::move(positioned.error()));
      }
      end_key_count = *positioned;
    } else {
      LoweringResult<void> positioned =
          region == IndexedMinMaxPassRegion::kNonNull
              ? EmitIndexedMinMaxNonNullPosition(index, traversal, runtime, index_cursor, *missing)
              : EmitIndexedMinMaxPosition(index, traversal, index_cursor, *missing);
      if (!positioned.has_value()) {
        return std::unexpected(std::move(positioned.error()));
      }
      LoweringResult<std::uint32_t> count =
          region == IndexedMinMaxPassRegion::kNonNull &&
                  runtime.extremum == BoundAggregateExtremumKind::kMaximum
              ? EmitIndexedMinMaxNullKey(index)
              : EmitIndexedMinMaxEndKey(index, traversal);
      if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
      }
      end_key_count = *count;
    }

    const bool has_end_key = region == IndexedMinMaxPassRegion::kNull ||
                             (region == IndexedMinMaxPassRegion::kNonNull &&
                              runtime.extremum == BoundAggregateExtremumKind::kMaximum) ||
                             traversal.has_end_key;
    std::optional<Label> first_end;
    if (has_end_key) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      first_end = *label;
      if (auto checked = EmitIndexedMinMaxPassEndCheck(index, traversal, runtime, index_cursor,
                                                       end_key_count, *first_end, region);
          !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }

    const bool needs_advance =
        skipped_value.has_value() || (filter_ != nullptr && !filter_->predicates.empty());
    std::optional<Label> exhausted;
    std::optional<Label> positioned_end;
    if (!needs_advance) {
      if (auto lookup = EmitIndexedMinMaxLookup(index_cursor, table_cursor); !lookup.has_value()) {
        return std::unexpected(std::move(lookup.error()));
      }
      if (auto candidate = EmitIndexedMinMaxCandidate(runtime, std::nullopt, std::nullopt);
          !candidate.has_value()) {
        return std::unexpected(std::move(candidate.error()));
      }
      if (auto closed = CloseIndexedMinMaxCursors(index_cursor, table_cursor);
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return std::unexpected(std::move(completed.error()));
      }
      return IndexedMinMaxPassExits{
          .missing = *missing,
          .first_end = first_end,
      };
    }

    auto next_candidate = CreateLabel();
    auto process = CreateLabel();
    auto advance = CreateLabel();
    auto scan_exhausted = CreateLabel();
    if (!next_candidate.has_value()) {
      return std::unexpected(std::move(next_candidate.error()));
    }
    if (!process.has_value()) {
      return std::unexpected(std::move(process.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (!scan_exhausted.has_value()) {
      return std::unexpected(std::move(scan_exhausted.error()));
    }
    exhausted = *scan_exhausted;
    if (has_end_key) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_end = *label;
    }

    if (auto lookup = EmitIndexedMinMaxLookup(index_cursor, table_cursor); !lookup.has_value()) {
      return std::unexpected(std::move(lookup.error()));
    }
    if (auto jumped = EmitJump(*process); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto bound = BindLabel(*next_candidate); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (positioned_end.has_value()) {
      LoweringResult<void> checked = EmitIndexedMinMaxPassEndCheck(
          index, traversal, runtime, index_cursor, end_key_count, *positioned_end, region);
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (auto lookup = EmitIndexedMinMaxLookup(index_cursor, table_cursor); !lookup.has_value()) {
      return std::unexpected(std::move(lookup.error()));
    }
    if (auto bound = BindLabel(*process); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto candidate = EmitIndexedMinMaxCandidate(runtime, skipped_value, *advance);
        !candidate.has_value()) {
      return std::unexpected(std::move(candidate.error()));
    }
    if (auto closed = CloseIndexedMinMaxCursors(index_cursor, table_cursor); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return std::unexpected(std::move(completed.error()));
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto advanced = EmitIndexedMinMaxAdvance(index, index_cursor, *next_candidate);
        !advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    if (auto jumped = EmitJump(*scan_exhausted); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    return IndexedMinMaxPassExits{
        .missing = *missing,
        .first_end = first_end,
        .exhausted = exhausted,
        .positioned_end = positioned_end,
    };
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxPassExit(Label exit, CursorId index_cursor,
                                                               std::optional<CursorId> table_cursor,
                                                               std::optional<Label> continuation) {
    if (auto bound = BindLabel(exit); !bound.has_value()) {
      return bound;
    }
    if (auto closed = CloseIndexedMinMaxCursors(index_cursor, table_cursor); !closed.has_value()) {
      return closed;
    }
    if (continuation.has_value()) {
      return EmitJump(*continuation);
    }
    return EmitGlobalAggregateCompletion();
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxPassExits(
      const IndexedMinMaxPassExits& exits, CursorId index_cursor,
      std::optional<CursorId> table_cursor, std::optional<Label> scan_continuation) {
    if (auto missing =
            EmitIndexedMinMaxPassExit(exits.missing, index_cursor, table_cursor, scan_continuation);
        !missing.has_value()) {
      return missing;
    }
    if (exits.first_end.has_value()) {
      if (auto ended = EmitIndexedMinMaxPassExit(*exits.first_end, index_cursor, table_cursor,
                                                 scan_continuation);
          !ended.has_value()) {
        return ended;
      }
    }
    if (exits.exhausted.has_value()) {
      if (auto exhausted = EmitIndexedMinMaxPassExit(*exits.exhausted, index_cursor, table_cursor,
                                                     scan_continuation);
          !exhausted.has_value()) {
        return exhausted;
      }
    }
    if (exits.positioned_end.has_value()) {
      if (auto ended = EmitIndexedMinMaxPassExit(*exits.positioned_end, index_cursor, table_cursor,
                                                 scan_continuation);
          !ended.has_value()) {
        return ended;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxIndexScan(
      const PhysicalIndexScanNode& index) {
    if (!cursor_.has_value() || aggregate_ == nullptr || !aggregate_->min_max.has_value() ||
        (!index.covering && (!index_cursor_.has_value() || !index_rowid_register_.has_value()))) {
      return std::unexpected(InternalFailure("indexed MIN/MAX index metadata is invalid"));
    }
    const CursorId index_cursor =
        index.covering ? AssumeValue(cursor_) : AssumeValue(index_cursor_);
    const std::optional<CursorId> table_cursor =
        index.covering ? std::nullopt : std::optional<CursorId>{AssumeValue(cursor_)};
    const IndexedMinMaxTraversal traversal = ResolveIndexedMinMaxTraversal(index);
    auto runtime = ResolveIndexedMinMaxRuntime();
    if (!runtime.has_value()) {
      return std::unexpected(std::move(runtime.error()));
    }
    const bool rejects_null = std::ranges::any_of(index.equalities,
                                                  [](const PhysicalIndexEquality& equality) {
                                                    return equality.reject_null;
                                                  }) ||
                              index.range.has_value();

    std::optional<Label> preopen_completion;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      preopen_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(preopen_completion); !limit.has_value()) {
      return limit;
    }
    if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
      return opened;
    }
    std::optional<Label> opened_empty;
    if ((filter_ != nullptr && !filter_->guards.empty()) || rejects_null) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      opened_empty = *label;
    }
    if (opened_empty.has_value()) {
      if (auto guards = EmitGlobalAggregateGuards(*opened_empty); !guards.has_value()) {
        return guards;
      }
    }
    if (auto keys = EmitIndexKeyExpressions(index, opened_empty); !keys.has_value()) {
      return keys;
    }

    auto null_policy = ResolveIndexedMinMaxNullPolicy(index, *runtime);
    if (!null_policy.has_value()) {
      return std::unexpected(std::move(null_policy.error()));
    }
    std::optional<Label> runtime_null_pass;
    if (null_policy->kind == IndexedMinMaxNullPolicyKind::kRuntimeIs) {
      if (!null_policy->runtime_key.has_value()) {
        return std::unexpected(InternalFailure("indexed MIN/MAX runtime IS key is unavailable"));
      }
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      runtime_null_pass = *label;
      if (auto jumped = EmitJumpIf(Home(*null_policy->runtime_key), JumpCondition::kIfNull, *label);
          !jumped.has_value()) {
        return jumped;
      }
    }
    const IndexedMinMaxPassRegion first_region =
        null_policy->kind == IndexedMinMaxNullPolicyKind::kUnconstrained
            ? IndexedMinMaxPassRegion::kNonNull
            : IndexedMinMaxPassRegion::kSelected;
    auto first = EmitIndexedMinMaxIndexPass(index, traversal, *runtime, index_cursor, table_cursor,
                                            std::nullopt, first_region);
    if (!first.has_value()) {
      return std::unexpected(std::move(first.error()));
    }
    if (null_policy->kind == IndexedMinMaxNullPolicyKind::kUnconstrained) {
      auto fallback = CreateLabel();
      if (!fallback.has_value()) {
        return std::unexpected(std::move(fallback.error()));
      }
      if (auto exits = EmitIndexedMinMaxPassExits(*first, index_cursor, table_cursor, *fallback);
          !exits.has_value()) {
        return exits;
      }
      if (auto bound = BindLabel(*fallback); !bound.has_value()) {
        return bound;
      }
      auto null_pass =
          EmitIndexedMinMaxIndexPass(index, traversal, *runtime, index_cursor, table_cursor,
                                     JumpCondition::kIfNotNull, IndexedMinMaxPassRegion::kNull);
      if (!null_pass.has_value()) {
        return std::unexpected(std::move(null_pass.error()));
      }
      if (auto exits =
              EmitIndexedMinMaxPassExits(*null_pass, index_cursor, table_cursor, std::nullopt);
          !exits.has_value()) {
        return exits;
      }
    } else {
      if (auto exits = EmitIndexedMinMaxPassExits(*first, index_cursor, table_cursor, std::nullopt);
          !exits.has_value()) {
        return exits;
      }
    }
    if (runtime_null_pass.has_value()) {
      if (auto bound = BindLabel(*runtime_null_pass); !bound.has_value()) {
        return bound;
      }
      auto null_pass =
          EmitIndexedMinMaxIndexPass(index, traversal, *runtime, index_cursor, table_cursor,
                                     JumpCondition::kIfNotNull, IndexedMinMaxPassRegion::kSelected);
      if (!null_pass.has_value()) {
        return std::unexpected(std::move(null_pass.error()));
      }
      if (auto exits =
              EmitIndexedMinMaxPassExits(*null_pass, index_cursor, table_cursor, std::nullopt);
          !exits.has_value()) {
        return exits;
      }
    }
    if (opened_empty.has_value()) {
      if (auto bound = BindLabel(*opened_empty); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (preopen_completion.has_value()) {
      if (auto bound = BindLabel(*preopen_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexedMinMaxTableScan() {
    if (table_scan_ == nullptr || !cursor_.has_value() || aggregate_ == nullptr ||
        !aggregate_->min_max.has_value() ||
        aggregate_->min_max->null_strategy != PhysicalMinMaxNullStrategy::kNone) {
      return std::unexpected(InternalFailure("indexed MIN/MAX table metadata is invalid"));
    }
    const CursorId cursor = AssumeValue(cursor_);
    std::optional<Label> preopen_completion;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      preopen_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(preopen_completion); !limit.has_value()) {
      return limit;
    }
    if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
      return opened;
    }
    std::optional<Label> opened_empty;
    if (filter_ != nullptr && !filter_->guards.empty()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      opened_empty = *label;
      if (auto guards = EmitGlobalAggregateGuards(*opened_empty); !guards.has_value()) {
        return guards;
      }
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
      return opened;
    }
    auto exhausted = CreateLabel();
    auto row = CreateLabel();
    if (!exhausted.has_value()) {
      return std::unexpected(std::move(exhausted.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    std::optional<Label> advance;
    if (filter_ != nullptr && !filter_->predicates.empty()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      advance = *label;
    }
    LoweringResult<InstructionAddress> positioned =
        table_scan_->direction == PhysicalScanDirection::kForward
            ? ConvertProgramResult(builder_->EmitRewind(cursor, *exhausted),
                                   "unable to emit indexed MIN/MAX table rewind")
            : ConvertProgramResult(builder_->EmitLast(cursor, *exhausted),
                                   "unable to emit indexed MIN/MAX table last");
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (advance.has_value()) {
      if (auto filters = EmitFilters(*advance); !filters.has_value()) {
        return filters;
      }
    }
    if (auto stepped = StepGlobalAggregates(); !stepped.has_value()) {
      return stepped;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (advance.has_value()) {
      if (auto bound = BindLabel(*advance); !bound.has_value()) {
        return bound;
      }
      LoweringResult<InstructionAddress> advanced =
          table_scan_->direction == PhysicalScanDirection::kForward
              ? ConvertProgramResult(builder_->EmitNext(cursor, *row),
                                     "unable to emit indexed MIN/MAX table next")
              : ConvertProgramResult(builder_->EmitPrevious(cursor, *row),
                                     "unable to emit indexed MIN/MAX table previous");
      if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
      }
    }
    if (auto bound = BindLabel(*exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
      return completed;
    }
    if (opened_empty.has_value()) {
      if (auto bound = BindLabel(*opened_empty); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitGlobalAggregateCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (preopen_completion.has_value()) {
      if (auto bound = BindLabel(*preopen_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregatePlan() {
    if (aggregate_ == nullptr) {
      return std::unexpected(InternalFailure("global aggregate metadata is unavailable"));
    }
    if (aggregate_->strategy == PhysicalAggregateStrategy::kIndexedMinMax) {
      if (table_scan_ != nullptr) {
        return EmitIndexedMinMaxTableScan();
      }
      if (rowid_lookup_ != nullptr) {
        return EmitGlobalAggregateRowIdLookup(*rowid_lookup_);
      }
      if (index_scan_ != nullptr) {
        return EmitIndexedMinMaxIndexScan(*index_scan_);
      }
      return std::unexpected(
          UnsupportedFailure("indexed global aggregate lowering is not implemented"));
    }
    if (aggregate_->strategy != PhysicalAggregateStrategy::kGlobal) {
      return std::unexpected(UnsupportedFailure("grouped aggregate lowering is not implemented"));
    }
    if (std::holds_alternative<PhysicalEmptyNode>(leaf_->payload)) {
      return EmitGlobalAggregateEmpty();
    }
    if (std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload)) {
      return EmitGlobalAggregateSingleRow();
    }
    if (table_scan_ != nullptr) {
      return EmitGlobalAggregateTableScan();
    }
    if (index_scan_ != nullptr && index_scan_->covering) {
      return EmitGlobalAggregateCoveringIndexScan(*index_scan_);
    }
    if (rowid_lookup_ != nullptr) {
      return EmitGlobalAggregateRowIdLookup(*rowid_lookup_);
    }
    if (index_scan_ != nullptr && !index_scan_->covering) {
      return EmitGlobalAggregateNoncoveringIndexScan(*index_scan_);
    }
    return std::unexpected(UnsupportedFailure("global aggregate access kind is not implemented"));
  }

  GlobalAggregateAccessServices& services_;
  ProgramBuilder* builder_;
  const BoundSelect* bound_select_;
  std::span<const BoundExpression> expressions_;
  std::span<const BoundAggregateFunction> aggregate_functions_;
  std::span<const BoundAggregateInvocation> aggregate_invocations_;
  std::span<const std::optional<AggregateId>> aggregate_descriptors_;
  std::span<const std::optional<RegisterId>> aggregate_argument_blocks_;
  const PhysicalNode* leaf_;
  const PhysicalTableScanNode* table_scan_;
  const PhysicalRowIdLookupNode* rowid_lookup_;
  const PhysicalIndexScanNode* index_scan_;
  const PhysicalFilterNode* filter_;
  const PhysicalLimitNode* limit_;
  const PhysicalAggregateNode* aggregate_;
  std::optional<CursorId> cursor_;
  std::optional<CursorId> index_cursor_;
  std::optional<RegisterId> index_key_first_;
  std::uint32_t index_key_capacity_;
  std::optional<RegisterId> index_rowid_register_;
  std::optional<ConstantId> indexed_min_max_null_constant_;
};

}  // namespace

LoweringResult<void> EmitGlobalAggregateAccess(const GlobalAggregateAccessInput& input,
                                               GlobalAggregateAccessServices& services) {
  if (input.builder == nullptr || input.bound_select == nullptr || input.leaf == nullptr ||
      input.aggregate == nullptr) {
    return std::unexpected(InternalFailure("global aggregate access input is incomplete"));
  }
  return GlobalAggregateAccessEmitter(input, services).Run();
}

}  // namespace modern_sqlite::lowering_internal
