#include "modern_sqlite/lowering/plan_lowering.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "aggregate_modifier_lowering.hpp"
#include "global_aggregate_access.hpp"
#include "grouped_aggregate_lowering.hpp"
#include "plan_lowering_internal.hpp"

namespace modern_sqlite {
namespace {

template <typename T>
using LoweringResult = lowering_internal::LoweringResult<T>;

using lowering_internal::AssumeValue;
using lowering_internal::ConvertProgramResult;
using lowering_internal::EmitIndexTableLookup;
using lowering_internal::InternalFailure;
using lowering_internal::ProgramFailure;
using lowering_internal::UnsupportedFailure;

[[nodiscard]] bool NamesEqual(std::string_view left, std::string_view right) noexcept {
  return CatalogNamesEqual(left, right);
}

struct NamedSymbol {
  std::string name;
  SymbolId id;
};

struct SubqueryCallBinding {
  RegisterId result{0};
  RegisterId ready{0};
  std::optional<Label> entry;
  std::optional<Label> last;
  std::optional<SubroutineId> subroutine;
};

struct StatementLoweringState {
  std::size_t next_register = 0;
  bool expression_layout_initialized = false;
  std::vector<std::optional<RegisterId>> call_argument_blocks;
  std::optional<ProgramBuilder> builder;
  std::vector<std::optional<ConstantId>> literal_constants;
  std::optional<ConstantId> null_constant;
  std::optional<ConstantId> zero_constant;
  std::optional<ConstantId> one_constant;
  std::optional<ConstantId> maximum_integer_constant;
  std::vector<SymbolId> collation_symbols;
  std::vector<SymbolId> function_symbols;
  std::vector<SymbolId> aggregate_function_symbols;
  std::vector<NamedSymbol> symbol_names;
  std::optional<SymbolId> binary_symbol;
  std::vector<SubqueryCallBinding> subqueries;
};

class PlanLowerer final : private lowering_internal::GlobalAggregateAccessServices,
                          private lowering_internal::GroupedAggregateServices {
 public:
  PlanLowerer(const PlanLowerer&) = delete;
  PlanLowerer& operator=(const PlanLowerer&) = delete;
  PlanLowerer(PlanLowerer&&) = delete;
  PlanLowerer& operator=(PlanLowerer&&) = delete;

  PlanLowerer(const PhysicalPlan& plan, ProgramLimits limits, StatementLoweringState& state,
              BoundQueryId query = BoundQueryId{0}) noexcept
      : read_query_(std::in_place, plan, query),
        logical_query_(std::in_place, plan.logical_plan(), query),
        bound_query_(std::in_place, plan.logical_plan().bound_select(), query),
        read_owner_(&plan),
        read_plan_(&*read_query_),
        logical_plan_(&*logical_query_),
        bound_select_(&*bound_query_),
        expressions_(bound_select_->expressions()),
        parameters_(bound_select_->parameters()),
        collations_(bound_select_->collations()),
        functions_(bound_select_->functions()),
        aggregate_functions_(bound_select_->aggregate_functions()),
        aggregate_invocations_(bound_select_->aggregate_invocations()),
        limits_(limits),
        statement_(state),
        next_register_(state.next_register),
        call_argument_blocks_(state.call_argument_blocks),
        builder_(state.builder),
        literal_constants_(state.literal_constants),
        null_constant_(state.null_constant),
        zero_constant_(state.zero_constant),
        one_constant_(state.one_constant),
        maximum_integer_constant_(state.maximum_integer_constant),
        collation_symbols_(state.collation_symbols),
        function_symbols_(state.function_symbols),
        aggregate_function_symbols_(state.aggregate_function_symbols),
        symbol_names_(state.symbol_names),
        binary_symbol_(state.binary_symbol) {
    if (const auto* core = std::get_if<BoundSelectCore>(&bound_select_->query_cores().front());
        core != nullptr) {
      active_source_column_begin_ = core->source_column_begin;
      active_source_column_count_ = core->source_column_count;
    }
  }

  PlanLowerer(const PhysicalMutationPlan& plan, ProgramLimits limits,
              StatementLoweringState& state) noexcept
      : mutation_plan_(&plan),
        limits_(limits),
        statement_(state),
        next_register_(state.next_register),
        call_argument_blocks_(state.call_argument_blocks),
        builder_(state.builder),
        literal_constants_(state.literal_constants),
        null_constant_(state.null_constant),
        zero_constant_(state.zero_constant),
        one_constant_(state.one_constant),
        maximum_integer_constant_(state.maximum_integer_constant),
        collation_symbols_(state.collation_symbols),
        function_symbols_(state.function_symbols),
        aggregate_function_symbols_(state.aggregate_function_symbols),
        symbol_names_(state.symbol_names),
        binary_symbol_(state.binary_symbol) {
    bound_insert_ = std::get_if<BoundInsert>(&plan.logical_plan().bound_statement());
    if (bound_insert_ != nullptr) {
      expressions_ = bound_insert_->expressions();
      parameters_ = bound_insert_->parameters();
      collations_ = bound_insert_->collations();
      functions_ = bound_insert_->functions();
      return;
    }
    bound_delete_ = std::get_if<BoundDelete>(&plan.logical_plan().bound_statement());
    if (bound_delete_ != nullptr) {
      expressions_ = bound_delete_->expressions();
      parameters_ = bound_delete_->parameters();
      collations_ = bound_delete_->collations();
      functions_ = bound_delete_->functions();
      source_columns_ = bound_delete_->source_columns();
      return;
    }
    bound_update_ = std::get_if<BoundUpdate>(&plan.logical_plan().bound_statement());
    if (bound_update_ != nullptr) {
      expressions_ = bound_update_->expressions();
      parameters_ = bound_update_->parameters();
      collations_ = bound_update_->collations();
      functions_ = bound_update_->functions();
      source_columns_ = bound_update_->source_columns();
      return;
    }
    bound_create_ = std::get_if<BoundCreateTable>(&plan.logical_plan().bound_statement());
    bound_create_index_ = std::get_if<BoundCreateIndex>(&plan.logical_plan().bound_statement());
    bound_analyze_ = std::get_if<BoundAnalyze>(&plan.logical_plan().bound_statement());
  }

  [[nodiscard]] LowerPlanResult Run() {
    if (read_plan_ != nullptr) {
      return RunRead();
    }
    return RunMutation();
  }

 private:
  [[nodiscard]] LoweringResult<void> PrepareReadLayout() {
    if (bound_select_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound SELECT does not retain a catalog"));
    }
    const bool has_aggregate =
        std::ranges::any_of(read_plan_->nodes(), [](const PhysicalNode& node) {
          return std::holds_alternative<PhysicalAggregateNode>(node.payload);
        });
    const bool advanced_aggregate =
        has_aggregate &&
        (bound_select_->query_cores().size() != 1U ||
         !bound_select_->compound_operators().empty() ||
         (std::holds_alternative<BoundSelectCore>(bound_select_->query_cores().front()) &&
          (std::get<BoundSelectCore>(bound_select_->query_cores().front()).quantifier ==
               SelectQuantifier::kDistinct ||
           std::get<BoundSelectCore>(bound_select_->query_cores().front()).table_sources.size() >
               1U)));
    LoweringResult<void> inspected =
        has_aggregate && !advanced_aggregate ? InspectAggregatePlan() : InspectPlan();
    if (!inspected.has_value()) {
      return std::unexpected(std::move(inspected.error()));
    }
    LoweringResult<void> layout =
        streaming_aggregate_query_ ? BuildStreamingAggregateRegisterLayout()
        : grouped_aggregate_query_ ? BuildGroupedAggregateRegisterLayout()
        : global_aggregate_query_  ? BuildGlobalAggregateRegisterLayout()
        : ordered_compound_
            ? BuildOrderedCompoundRegisterLayout()
            : (set_query_
                   ? BuildSetRegisterLayout()
                   : (streaming_query_ ? BuildStreamingRegisterLayout()
                                       : (distinct_ != nullptr ? BuildDistinctRegisterLayout()
                                                               : BuildReadRegisterLayout())));
    if (!layout.has_value()) {
      return std::unexpected(std::move(layout.error()));
    }
    return {};
  }

  [[nodiscard]] bool RequiresReadSnapshot() const noexcept {
    return streaming_requires_snapshot_ ||
           (!streaming_query_ && !set_query_ && !bound_select_->table_sources().empty());
  }

  [[nodiscard]] LowerPlanResult RunRead() {
    if (auto prepared = PrepareReadLayout(); !prepared.has_value()) {
      return std::unexpected(std::move(prepared.error()));
    }
    const std::size_t nested_count = bound_select_->owner().query_count() - 1U;
    std::vector<std::unique_ptr<PlanLowerer>> children;
    children.reserve(nested_count);
    statement_.subqueries.resize(nested_count);
    bool snapshot = RequiresReadSnapshot();
    for (std::size_t index = 0; index < nested_count; ++index) {
      auto registers = AllocateRegisters(2);
      if (!registers.has_value()) {
        return std::unexpected(std::move(registers.error()));
      }
      statement_.subqueries[index].result = *registers;
      statement_.subqueries[index].ready = RegisterId{registers->value() + 1U};
      auto child = std::make_unique<PlanLowerer>(
          *read_owner_, limits_, statement_, BoundQueryId{static_cast<std::uint32_t>(index + 1U)});
      if (auto prepared = child->PrepareReadLayout(); !prepared.has_value()) {
        return std::unexpected(std::move(prepared.error()));
      }
      snapshot = snapshot || child->RequiresReadSnapshot();
      children.push_back(std::move(child));
    }
    if (auto layout = FinishRegisterLayout(); !layout.has_value()) {
      return std::unexpected(std::move(layout.error()));
    }

    const CatalogVersion version = bound_select_->catalog()->version();
    auto created = ConvertProgramResult(ProgramBuilder::Create(
                                            SchemaVersionRequirement{
                                                .schema_cookie = version.schema_cookie,
                                                .generation = version.generation,
                                            },
                                            ProgramResourceCounts{
                                                .registers = register_count_,
                                                .parameters = parameter_count_,
                                            },
                                            limits_),
                                        "unable to create bytecode builder");
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    builder_.emplace(std::move(*created));

    if (snapshot) {
      auto required = ConvertProgramResult(AssumeValue(builder_).RequireDatabaseSnapshot(),
                                           "unable to require a database snapshot");
      if (!required.has_value()) {
        return std::unexpected(std::move(required.error()));
      }
    }
    if (auto constants = AddBoundConstants(); !constants.has_value()) {
      return std::unexpected(std::move(constants.error()));
    }
    if (auto symbols = AddBoundSymbols(); !symbols.has_value()) {
      return std::unexpected(std::move(symbols.error()));
    }
    if (auto descriptors = AddReadDescriptors(); !descriptors.has_value()) {
      return std::unexpected(std::move(descriptors.error()));
    }
    if (auto caches = EmitSubqueryCacheInitialization(); !caches.has_value()) {
      return std::unexpected(std::move(caches.error()));
    }
    if (auto emitted = EmitReadPlan(); !emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    for (const auto& child : children) {
      if (auto emitted = child->EmitReadSubroutine(); !emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
    }

    std::vector<ResultColumnMetadata> result_columns;
    result_columns.reserve(bound_select_->result_columns().size());
    for (const BoundResultColumn& column : bound_select_->result_columns()) {
      result_columns.push_back(ResultColumnMetadata{
          .name = column.name,
          .declared_type = column.declared_type,
          .affinity = column.affinity,
      });
    }

    auto built =
        ConvertProgramResult(std::move(AssumeValue(builder_)).Build(std::move(result_columns)),
                             "lowered bytecode failed verification");
    if (!built.has_value()) {
      return std::unexpected(std::move(built.error()));
    }
    return std::move(*built);
  }

  [[nodiscard]] LoweringResult<void> AddReadDescriptors() {
    if (global_aggregate_query_ || grouped_aggregate_query_) {
      if (auto descriptors = AddGlobalAggregateDescriptors(); !descriptors.has_value()) {
        return std::unexpected(std::move(descriptors.error()));
      }
      if (auto modifiers = AddAggregateModifierDescriptors(); !modifiers.has_value()) {
        return std::unexpected(std::move(modifiers.error()));
      }
      if (grouped_aggregate_query_) {
        if (auto grouped = AddGroupedAggregateDescriptors(); !grouped.has_value()) {
          return std::unexpected(std::move(grouped.error()));
        }
      }
    } else {
      if (streaming_aggregate_query_) {
        if (auto descriptors = AddStreamingAggregateDescriptors(); !descriptors.has_value()) {
          return std::unexpected(std::move(descriptors.error()));
        }
      }
      if (ordered_compound_) {
        if (auto descriptors = AddOrderedCompoundDescriptors(); !descriptors.has_value()) {
          return std::unexpected(std::move(descriptors.error()));
        }
      } else if (set_query_) {
        if (auto descriptors = AddSetDescriptors(); !descriptors.has_value()) {
          return std::unexpected(std::move(descriptors.error()));
        }
      } else if (streaming_query_) {
        if (auto descriptors = AddStreamingDescriptors(); !descriptors.has_value()) {
          return std::unexpected(std::move(descriptors.error()));
        }
      } else if (distinct_ != nullptr) {
        if (auto descriptors = AddDistinctDescriptors(); !descriptors.has_value()) {
          return std::unexpected(std::move(descriptors.error()));
        }
      } else if (auto ordering = AddOrderingDescriptors(); !ordering.has_value()) {
        return std::unexpected(std::move(ordering.error()));
      }
    }
    if (!streaming_query_ && !set_query_ && !ordered_compound_) {
      if (auto cursor = AddCursorDescriptor(); !cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitReadPlan() {
    return grouped_aggregate_query_  ? EmitGroupedAggregatePlan()
           : global_aggregate_query_ ? EmitGlobalAggregatePlan()
           : ordered_compound_
               ? EmitOrderedCompoundPlan()
               : (set_query_ ? EmitSetPlan()
                             : (streaming_query_
                                    ? EmitStreamingPlan()
                                    : (distinct_ != nullptr ? EmitDistinctPlan() : EmitPlan())));
  }

  [[nodiscard]] LoweringResult<void> EmitSubqueryCacheInitialization() {
    if (statement_.subqueries.empty()) {
      return {};
    }
    auto null = EnsureNullConstant();
    auto zero = EnsureZeroConstant();
    if (!null.has_value()) {
      return std::unexpected(std::move(null.error()));
    }
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    for (const SubqueryCallBinding& call : statement_.subqueries) {
      if (auto initialized =
              Append(LoadConstantInstruction{.constant = *null, .output = call.result});
          !initialized.has_value()) {
        return initialized;
      }
      if (auto initialized =
              Append(LoadConstantInstruction{.constant = *zero, .output = call.ready});
          !initialized.has_value()) {
        return initialized;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitReadSubroutine() {
    const BoundQueryId query = bound_select_->query().id;
    SubqueryCallBinding& call = statement_.subqueries[query.value() - 1U];
    if (!call.subroutine.has_value()) {
      return {};
    }
    if (auto descriptors = AddReadDescriptors(); !descriptors.has_value()) {
      return descriptors;
    }
    auto completed = CreateLabel();
    if (!completed.has_value()) {
      return std::unexpected(std::move(completed.error()));
    }
    subquery_completion_ = *completed;
    if (auto bound = BindLabel(AssumeValue(call.entry)); !bound.has_value()) {
      return bound;
    }
    auto initial = bound_select_->query().destination == BoundQueryDestination::kExists
                       ? EnsureZeroConstant()
                       : EnsureNullConstant();
    if (!initial.has_value()) {
      return std::unexpected(std::move(initial.error()));
    }
    if (auto initialized =
            Append(LoadConstantInstruction{.constant = *initial, .output = call.result});
        !initialized.has_value()) {
      return initialized;
    }
    if (auto emitted = EmitReadPlan(); !emitted.has_value()) {
      return emitted;
    }
    if (auto bound = BindLabel(*completed); !bound.has_value()) {
      return bound;
    }
    auto one = EnsureOneConstant();
    if (!one.has_value()) {
      return std::unexpected(std::move(one.error()));
    }
    if (auto ready = Append(LoadConstantInstruction{.constant = *one, .output = call.ready});
        !ready.has_value()) {
      return ready;
    }
    if (auto bound = BindLabel(AssumeValue(call.last)); !bound.has_value()) {
      return bound;
    }
    return Append(ReturnSubroutineInstruction{.subroutine = *call.subroutine});
  }

  [[nodiscard]] LowerPlanResult RunMutation() {
    const auto* insert = std::get_if<PhysicalInsertMutation>(&mutation_plan_->payload());
    if (insert != nullptr) {
      return RunInsert(*insert);
    }
    const auto* delete_plan = std::get_if<PhysicalDeleteMutation>(&mutation_plan_->payload());
    if (delete_plan != nullptr) {
      return RunDelete(*delete_plan);
    }
    const auto* update = std::get_if<PhysicalUpdateMutation>(&mutation_plan_->payload());
    if (update != nullptr) {
      return RunUpdate(*update);
    }
    const auto* create = std::get_if<PhysicalCreateTableMutation>(&mutation_plan_->payload());
    if (create != nullptr) {
      return RunCreateTable(*create);
    }
    const auto* create_index = std::get_if<PhysicalCreateIndexMutation>(&mutation_plan_->payload());
    if (create_index != nullptr) {
      return RunCreateIndex(*create_index);
    }
    const auto* analyze = std::get_if<PhysicalAnalyzeMutation>(&mutation_plan_->payload());
    if (analyze != nullptr) {
      return RunAnalyze(*analyze);
    }
    return std::unexpected(UnsupportedFailure("physical mutation plan is not implemented"));
  }

  [[nodiscard]] LowerPlanResult RunInsert(const PhysicalInsertMutation& insert) {
    if (bound_insert_ == nullptr ||
        !std::holds_alternative<LogicalInsertMutation>(mutation_plan_->logical_plan().payload())) {
      return std::unexpected(InternalFailure("physical INSERT plan has inconsistent ownership"));
    }
    if (bound_insert_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound INSERT does not retain a catalog"));
    }
    if (auto layout = BuildInsertRegisterLayout(); !layout.has_value()) {
      return std::unexpected(std::move(layout.error()));
    }

    const CatalogVersion version = bound_insert_->required_catalog_version();
    auto created = ConvertProgramResult(ProgramBuilder::Create(
                                            SchemaVersionRequirement{
                                                .schema_cookie = version.schema_cookie,
                                                .generation = version.generation,
                                            },
                                            ProgramResourceCounts{
                                                .registers = register_count_,
                                                .parameters = parameter_count_,
                                            },
                                            limits_),
                                        "unable to create bytecode builder");
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    builder_.emplace(std::move(*created));

    const ProgramRollbackMode rollback_mode = insert.atomicity == MutationAtomicity::kStatement
                                                  ? ProgramRollbackMode::kStatement
                                                  : ProgramRollbackMode::kTransaction;
    auto metadata = ConvertProgramResult(
        AssumeValue(builder_).SetExecutionMetadata(ProgramStatementKind::kInsert,
                                                   ProgramTransactionAccess::kWrite, rollback_mode,
                                                   MutationResultMetadata{
                                                       .publishes_changes = true,
                                                       .publishes_last_insert_rowid = true,
                                                   }),
        "unable to set INSERT execution metadata");
    if (!metadata.has_value()) {
      return std::unexpected(std::move(metadata.error()));
    }
    if (auto constants = AddBoundConstants(); !constants.has_value()) {
      return std::unexpected(std::move(constants.error()));
    }
    if (auto symbols = AddBoundSymbols(); !symbols.has_value()) {
      return std::unexpected(std::move(symbols.error()));
    }
    if (auto cursor =
            AddMutationWriteCursorDescriptor(bound_insert_->target(), &insert_default_constants_);
        !cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    if (auto indexes = AddMutationIndexWriteCursorDescriptors(bound_insert_->target());
        !indexes.has_value()) {
      return std::unexpected(std::move(indexes.error()));
    }
    if (auto emitted = EmitInsert(); !emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }

    auto built = ConvertProgramResult(std::move(AssumeValue(builder_)).Build({}),
                                      "lowered INSERT bytecode failed verification");
    if (!built.has_value()) {
      return std::unexpected(std::move(built.error()));
    }
    return std::move(*built);
  }

  [[nodiscard]] LowerPlanResult RunDelete(const PhysicalDeleteMutation& delete_plan) {
    if (bound_delete_ == nullptr ||
        !std::holds_alternative<LogicalDeleteMutation>(mutation_plan_->logical_plan().payload())) {
      return std::unexpected(InternalFailure("physical DELETE plan has inconsistent ownership"));
    }
    if (bound_delete_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound DELETE does not retain a catalog"));
    }
    if (auto layout = BuildDeleteRegisterLayout(delete_plan); !layout.has_value()) {
      return std::unexpected(std::move(layout.error()));
    }

    const CatalogVersion version = bound_delete_->required_catalog_version();
    auto created = ConvertProgramResult(ProgramBuilder::Create(
                                            SchemaVersionRequirement{
                                                .schema_cookie = version.schema_cookie,
                                                .generation = version.generation,
                                            },
                                            ProgramResourceCounts{
                                                .registers = register_count_,
                                                .parameters = parameter_count_,
                                            },
                                            limits_),
                                        "unable to create bytecode builder");
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    builder_.emplace(std::move(*created));

    const ProgramRollbackMode rollback_mode = delete_plan.atomicity == MutationAtomicity::kStatement
                                                  ? ProgramRollbackMode::kStatement
                                                  : ProgramRollbackMode::kTransaction;
    auto metadata = ConvertProgramResult(
        AssumeValue(builder_).SetExecutionMetadata(ProgramStatementKind::kDelete,
                                                   ProgramTransactionAccess::kWrite, rollback_mode,
                                                   MutationResultMetadata{
                                                       .publishes_changes = true,
                                                       .publishes_last_insert_rowid = false,
                                                   }),
        "unable to set DELETE execution metadata");
    if (!metadata.has_value()) {
      return std::unexpected(std::move(metadata.error()));
    }
    if (auto constants = AddBoundConstants(); !constants.has_value()) {
      return std::unexpected(std::move(constants.error()));
    }
    if (auto symbols = AddBoundSymbols(); !symbols.has_value()) {
      return std::unexpected(std::move(symbols.error()));
    }
    if (delete_plan.access.kind != MutationAccessKind::kEmpty) {
      if (auto read_cursor =
              AddMutationReadCursorDescriptor(bound_delete_->target(), *bound_delete_->catalog());
          !read_cursor.has_value()) {
        return std::unexpected(std::move(read_cursor.error()));
      }
      if (delete_plan.access.kind == MutationAccessKind::kRowIdLookup ||
          delete_plan.collect_original_rowids) {
        if (auto write_cursor = AddMutationWriteCursorDescriptor(bound_delete_->target(), nullptr);
            !write_cursor.has_value()) {
          return std::unexpected(std::move(write_cursor.error()));
        }
      }
      if (auto indexes = AddMutationIndexWriteCursorDescriptors(bound_delete_->target());
          !indexes.has_value()) {
        return std::unexpected(std::move(indexes.error()));
      }
    }
    if (auto emitted = EmitDelete(delete_plan); !emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }

    auto built = ConvertProgramResult(std::move(AssumeValue(builder_)).Build({}),
                                      "lowered DELETE bytecode failed verification");
    if (!built.has_value()) {
      return std::unexpected(std::move(built.error()));
    }
    return std::move(*built);
  }

  [[nodiscard]] LowerPlanResult RunUpdate(const PhysicalUpdateMutation& update) {
    if (bound_update_ == nullptr ||
        !std::holds_alternative<LogicalUpdateMutation>(mutation_plan_->logical_plan().payload())) {
      return std::unexpected(InternalFailure("physical UPDATE plan has inconsistent ownership"));
    }
    if (bound_update_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound UPDATE does not retain a catalog"));
    }
    if (auto layout = BuildUpdateRegisterLayout(update); !layout.has_value()) {
      return std::unexpected(std::move(layout.error()));
    }

    const CatalogVersion version = bound_update_->required_catalog_version();
    auto created = ConvertProgramResult(ProgramBuilder::Create(
                                            SchemaVersionRequirement{
                                                .schema_cookie = version.schema_cookie,
                                                .generation = version.generation,
                                            },
                                            ProgramResourceCounts{
                                                .registers = register_count_,
                                                .parameters = parameter_count_,
                                            },
                                            limits_),
                                        "unable to create bytecode builder");
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    builder_.emplace(std::move(*created));

    const ProgramRollbackMode rollback_mode = update.atomicity == MutationAtomicity::kStatement
                                                  ? ProgramRollbackMode::kStatement
                                                  : ProgramRollbackMode::kTransaction;
    auto metadata = ConvertProgramResult(
        AssumeValue(builder_).SetExecutionMetadata(ProgramStatementKind::kUpdate,
                                                   ProgramTransactionAccess::kWrite, rollback_mode,
                                                   MutationResultMetadata{
                                                       .publishes_changes = true,
                                                       .publishes_last_insert_rowid = false,
                                                   }),
        "unable to set UPDATE execution metadata");
    if (!metadata.has_value()) {
      return std::unexpected(std::move(metadata.error()));
    }
    if (auto constants = AddBoundConstants(); !constants.has_value()) {
      return std::unexpected(std::move(constants.error()));
    }
    if (auto symbols = AddBoundSymbols(); !symbols.has_value()) {
      return std::unexpected(std::move(symbols.error()));
    }
    if (update.access.kind != MutationAccessKind::kEmpty) {
      if (auto read_cursor =
              AddMutationReadCursorDescriptor(bound_update_->target(), *bound_update_->catalog());
          !read_cursor.has_value()) {
        return std::unexpected(std::move(read_cursor.error()));
      }
      if (auto write_cursor = AddMutationWriteCursorDescriptor(bound_update_->target(), nullptr);
          !write_cursor.has_value()) {
        return std::unexpected(std::move(write_cursor.error()));
      }
      if (auto indexes = AddMutationIndexWriteCursorDescriptors(bound_update_->target());
          !indexes.has_value()) {
        return std::unexpected(std::move(indexes.error()));
      }
    }
    if (auto emitted = EmitUpdate(update); !emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }

    auto built = ConvertProgramResult(std::move(AssumeValue(builder_)).Build({}),
                                      "lowered UPDATE bytecode failed verification");
    if (!built.has_value()) {
      return std::unexpected(std::move(built.error()));
    }
    return std::move(*built);
  }

  [[nodiscard]] LowerPlanResult RunCreateTable(const PhysicalCreateTableMutation& create) {
    if (bound_create_ == nullptr || !std::holds_alternative<LogicalCreateTableMutation>(
                                        mutation_plan_->logical_plan().payload())) {
      return std::unexpected(
          InternalFailure("physical CREATE TABLE plan has inconsistent ownership"));
    }
    if (bound_create_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound CREATE TABLE does not retain a catalog"));
    }

    constexpr std::uint32_t kCreateRegisterCount = 8;
    const std::uint32_t register_count = create.no_op ? 0U : kCreateRegisterCount;
    const CatalogVersion version = bound_create_->required_catalog_version();
    auto created = ConvertProgramResult(ProgramBuilder::Create(
                                            SchemaVersionRequirement{
                                                .schema_cookie = version.schema_cookie,
                                                .generation = version.generation,
                                            },
                                            ProgramResourceCounts{
                                                .registers = register_count,
                                                .parameters = 0,
                                            },
                                            limits_),
                                        "unable to create bytecode builder");
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    builder_.emplace(std::move(*created));

    const ProgramRollbackMode rollback_mode = create.atomicity == MutationAtomicity::kStatement
                                                  ? ProgramRollbackMode::kStatement
                                                  : ProgramRollbackMode::kTransaction;
    auto metadata = ConvertProgramResult(
        AssumeValue(builder_).SetExecutionMetadata(ProgramStatementKind::kCreateTable,
                                                   ProgramTransactionAccess::kWrite, rollback_mode),
        "unable to set CREATE TABLE execution metadata");
    if (!metadata.has_value()) {
      return std::unexpected(std::move(metadata.error()));
    }
    if (create.no_op) {
      if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
        return std::unexpected(std::move(halted.error()));
      }
    } else {
      auto schema_cursor = AddSchemaWriteCursorDescriptor();
      if (!schema_cursor.has_value()) {
        return std::unexpected(std::move(schema_cursor.error()));
      }
      schema_write_cursor_ = *schema_cursor;
      if (auto emitted = EmitCreateTable(); !emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
    }

    auto built = ConvertProgramResult(std::move(AssumeValue(builder_)).Build({}),
                                      "lowered CREATE TABLE bytecode failed verification");
    if (!built.has_value()) {
      return std::unexpected(std::move(built.error()));
    }
    return std::move(*built);
  }

  [[nodiscard]] LowerPlanResult RunCreateIndex(const PhysicalCreateIndexMutation& create) {
    if (bound_create_index_ == nullptr || !std::holds_alternative<LogicalCreateIndexMutation>(
                                              mutation_plan_->logical_plan().payload())) {
      return std::unexpected(
          InternalFailure("physical CREATE INDEX plan has inconsistent ownership"));
    }
    if (bound_create_index_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound CREATE INDEX does not retain a catalog"));
    }
    const std::size_t key_register_count = bound_create_index_->terms().size() + 1U;
    if (key_register_count > std::numeric_limits<std::uint32_t>::max() - 8U) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kRegisterLimitExceeded},
                         "CREATE INDEX register count exceeds the bytecode identity range"));
    }
    const std::uint32_t register_count =
        create.no_op ? 0U : static_cast<std::uint32_t>(8U + key_register_count);
    const CatalogVersion version = bound_create_index_->required_catalog_version();
    auto created = ConvertProgramResult(ProgramBuilder::Create(
                                            SchemaVersionRequirement{
                                                .schema_cookie = version.schema_cookie,
                                                .generation = version.generation,
                                            },
                                            ProgramResourceCounts{
                                                .registers = register_count,
                                                .parameters = 0,
                                            },
                                            limits_),
                                        "unable to create bytecode builder");
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    builder_.emplace(std::move(*created));

    const ProgramRollbackMode rollback_mode = create.atomicity == MutationAtomicity::kStatement
                                                  ? ProgramRollbackMode::kStatement
                                                  : ProgramRollbackMode::kTransaction;
    auto metadata = ConvertProgramResult(
        AssumeValue(builder_).SetExecutionMetadata(ProgramStatementKind::kCreateIndex,
                                                   ProgramTransactionAccess::kWrite, rollback_mode),
        "unable to set CREATE INDEX execution metadata");
    if (!metadata.has_value()) {
      return std::unexpected(std::move(metadata.error()));
    }
    if (create.no_op) {
      if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
        return std::unexpected(std::move(halted.error()));
      }
    } else {
      auto snapshot = ConvertProgramResult(AssumeValue(builder_).RequireDatabaseSnapshot(),
                                           "unable to require a database snapshot");
      if (!snapshot.has_value()) {
        return std::unexpected(std::move(snapshot.error()));
      }
      if (auto descriptors = AddCreateIndexDescriptors(); !descriptors.has_value()) {
        return std::unexpected(std::move(descriptors.error()));
      }
      if (auto emitted = EmitCreateIndex(); !emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
    }

    auto built = ConvertProgramResult(std::move(AssumeValue(builder_)).Build({}),
                                      "lowered CREATE INDEX bytecode failed verification");
    if (!built.has_value()) {
      return std::unexpected(std::move(built.error()));
    }
    return std::move(*built);
  }

  [[nodiscard]] LowerPlanResult RunAnalyze(const PhysicalAnalyzeMutation& analyze) {
    if (bound_analyze_ == nullptr ||
        !std::holds_alternative<LogicalAnalyzeMutation>(mutation_plan_->logical_plan().payload())) {
      return std::unexpected(InternalFailure("physical ANALYZE plan has inconsistent ownership"));
    }
    if (bound_analyze_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound ANALYZE does not retain a catalog"));
    }
    constexpr std::uint32_t kAnalyzeRegisterCount = 8;
    const CatalogVersion version = bound_analyze_->required_catalog_version();
    auto created = ConvertProgramResult(ProgramBuilder::Create(
                                            SchemaVersionRequirement{
                                                .schema_cookie = version.schema_cookie,
                                                .generation = version.generation,
                                            },
                                            ProgramResourceCounts{
                                                .registers = kAnalyzeRegisterCount,
                                                .parameters = 0,
                                            },
                                            limits_),
                                        "unable to create bytecode builder");
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    builder_.emplace(std::move(*created));

    const ProgramRollbackMode rollback_mode = analyze.atomicity == MutationAtomicity::kStatement
                                                  ? ProgramRollbackMode::kStatement
                                                  : ProgramRollbackMode::kTransaction;
    auto metadata = ConvertProgramResult(
        AssumeValue(builder_).SetExecutionMetadata(ProgramStatementKind::kAnalyze,
                                                   ProgramTransactionAccess::kWrite, rollback_mode),
        "unable to set ANALYZE execution metadata");
    if (!metadata.has_value()) {
      return std::unexpected(std::move(metadata.error()));
    }
    auto snapshot = ConvertProgramResult(AssumeValue(builder_).RequireDatabaseSnapshot(),
                                         "unable to require a database snapshot");
    if (!snapshot.has_value()) {
      return std::unexpected(std::move(snapshot.error()));
    }
    if (auto descriptors = AddAnalyzeDescriptors(analyze); !descriptors.has_value()) {
      return std::unexpected(std::move(descriptors.error()));
    }
    if (auto emitted = EmitAnalyze(analyze); !emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }

    auto built = ConvertProgramResult(std::move(AssumeValue(builder_)).Build({}),
                                      "lowered ANALYZE bytecode failed verification");
    if (!built.has_value()) {
      return std::unexpected(std::move(built.error()));
    }
    return std::move(*built);
  }

  struct JoinedSourcePlan {
    const PhysicalNode* access = nullptr;
    BoundSourceId source{0};
    std::optional<CursorId> cursor{};
    std::optional<CursorId> table_cursor{};
    std::optional<RegisterId> index_key_first{};
    const PhysicalNestedLoopJoinNode* join = nullptr;
    std::optional<RegisterId> matched{};
    std::optional<RegisterId> replay_mode{};
    std::optional<RegisterId> right_key_first{};
    std::uint32_t right_key_count = 0;
    std::optional<RelationId> right_matches{};
  };

  struct RightJoinContinuation {
    std::size_t depth;
    Label published;
    Label replay_candidate;
    Label replay_done;
    std::optional<Label> replay_exhausted{};
  };

  struct SourceCursorBinding {
    CursorId cursor;
    std::optional<CursorFieldId> rowid_field{};
  };

  struct AggregateLeafRegister {
    BoundSourceLeafIdentity leaf;
    RegisterId reg;
  };

  struct StreamingCorePlan {
    std::size_t core_index = 0;
    const BoundQueryCore* bound = nullptr;
    const PhysicalNode* leaf = nullptr;
    const PhysicalEmptyNode* empty = nullptr;
    const PhysicalSingleRowNode* single_row = nullptr;
    const PhysicalTableScanNode* table_scan = nullptr;
    const PhysicalRowIdLookupNode* rowid_lookup = nullptr;
    const PhysicalIndexScanNode* index_scan = nullptr;
    const PhysicalNestedLoopJoinNode* join = nullptr;
    std::vector<JoinedSourcePlan> joined_sources{};
    const PhysicalValuesNode* values = nullptr;
    const PhysicalFilterNode* filter = nullptr;
    const PhysicalAggregateNode* aggregate = nullptr;
    const PhysicalHavingNode* having = nullptr;
    const PhysicalProjectionNode* projection = nullptr;
    const PhysicalDistinctNode* distinct = nullptr;
    std::optional<CursorId> cursor{};
    std::optional<CursorId> index_cursor{};
    std::optional<RelationId> relation{};
    std::optional<SorterId> group_sorter{};
    std::vector<BoundExpressionId> global_representative_leaves{};
    std::vector<BoundExpressionId> grouped_retained_leaves{};
    bool global_filtered_controller_defaults = false;
  };

  [[nodiscard]] LoweringResult<bool> InspectJoinedAccess(const PhysicalNode& access,
                                                         std::size_t core_index,
                                                         BoundSourceId source) {
    if (core_index >= streaming_cores_.size()) {
      return std::unexpected(InternalFailure("streaming source core is invalid"));
    }
    StreamingCorePlan& core = streaming_cores_[core_index];
    const auto* select = std::get_if<BoundSelectCore>(core.bound);
    if (select == nullptr || select->table_sources.size() < 2U) {
      return false;
    }
    const std::size_t source_index = core.joined_sources.size();
    if (source_index >= select->table_sources.size() ||
        source != select->table_sources[source_index].id) {
      return std::unexpected(InternalFailure("join access does not match declared sources"));
    }
    core.joined_sources.push_back(JoinedSourcePlan{.access = &access, .source = source});
    return true;
  }

  enum class SelectOutputMode : std::uint8_t {
    Streaming,
    Ordered,
    Materialized,
    DemandOne,
  };

  [[nodiscard]] LoweringResult<void> EmitProjectionValue(
      BoundExpressionId expression, RegisterId destination,
      SelectOutputMode mode = SelectOutputMode::Streaming) {
    if (bound_select_->query().destination == BoundQueryDestination::kExists &&
        (mode == SelectOutputMode::Streaming || mode == SelectOutputMode::DemandOne)) {
      return {};
    }
    return EmitExpression(expression, destination);
  }

  struct SelectOutputContext {
    SelectOutputMode mode = SelectOutputMode::Streaming;
    const StreamingCorePlan* core = nullptr;
    std::optional<Label> final_completion{};
    const LogicalCoreOrderLayout* ordered_layout = nullptr;
    std::optional<SorterId> ordered_sorter{};
    std::optional<TopNId> ordered_top_n{};
    bool ordered_full_key = false;
    std::optional<RegisterId> ordered_record_first{};
    std::optional<RelationId> materialized_target{};
    std::optional<RegisterId> materialized_count{};
  };

  struct OrderedRun {
    SorterId sorter;
    bool full_key = false;
    bool unique = false;
  };

  struct OrderedCoreRange {
    std::size_t first = 0;
    std::size_t last = 0;
  };

  struct SetAccumulator {
    std::size_t accumulator = 0;
    std::size_t right = 1;
    std::size_t output = 2;
  };

  [[nodiscard]] LoweringResult<void> InspectStreamingPlan() {
    const std::span<const PhysicalNode> nodes = read_plan_->nodes();
    const std::span<const BoundQueryCore> cores = bound_select_->query_cores();
    if (nodes.empty() || cores.empty()) {
      return std::unexpected(InternalFailure("streaming query plan is empty"));
    }
    ordered_compound_ = !bound_select_->order_by().empty();
    streaming_query_ = std::ranges::all_of(
        bound_select_->compound_operators(),
        [](CompoundOperator operation) { return operation == CompoundOperator::kUnionAll; });
    set_query_ = !streaming_query_;
    streaming_cores_.resize(cores.size());
    for (std::size_t index = 0; index < cores.size(); ++index) {
      streaming_cores_[index].core_index = index;
      streaming_cores_[index].bound = &cores[index];
      if (const auto* select = std::get_if<BoundSelectCore>(&cores[index]);
          select != nullptr && select->table_sources.size() > 1U) {
        streaming_cores_[index].joined_sources.reserve(select->table_sources.size());
      }
    }
    const auto has_access = [](const StreamingCorePlan& core) {
      return core.empty != nullptr || core.single_row != nullptr || core.table_scan != nullptr ||
             core.rowid_lookup != nullptr || core.index_scan != nullptr || core.values != nullptr ||
             core.join != nullptr || !core.joined_sources.empty();
    };
    std::vector<std::optional<std::size_t>> node_cores(nodes.size());
    std::vector<PhysicalCompoundStrategy> compound_strategies;
    compound_strategies.reserve(bound_select_->compound_operators().size());
    std::size_t compound_count = 0;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      const PhysicalNode& physical = nodes[index];
      if (const auto* empty = std::get_if<PhysicalEmptyNode>(&physical.payload); empty != nullptr) {
        if (empty->core_index >= streaming_cores_.size() ||
            has_access(streaming_cores_[empty->core_index])) {
          return std::unexpected(InternalFailure("streaming empty core is invalid"));
        }
        streaming_cores_[empty->core_index].leaf = &physical;
        streaming_cores_[empty->core_index].empty = empty;
        node_cores[index] = empty->core_index;
      } else if (const auto* single = std::get_if<PhysicalSingleRowNode>(&physical.payload);
                 single != nullptr) {
        if (single->core_index >= streaming_cores_.size() ||
            has_access(streaming_cores_[single->core_index])) {
          return std::unexpected(InternalFailure("streaming single-row core is invalid"));
        }
        streaming_cores_[single->core_index].leaf = &physical;
        streaming_cores_[single->core_index].single_row = single;
        node_cores[index] = single->core_index;
      } else if (const auto* scan = std::get_if<PhysicalTableScanNode>(&physical.payload);
                 scan != nullptr) {
        auto joined = InspectJoinedAccess(physical, scan->core_index, scan->source);
        if (!joined.has_value()) {
          return std::unexpected(std::move(joined.error()));
        }
        StreamingCorePlan& core = streaming_cores_[scan->core_index];
        if (!*joined) {
          if (has_access(core)) {
            return std::unexpected(InternalFailure("streaming core has duplicate access"));
          }
          core.leaf = &physical;
          core.table_scan = scan;
        }
        node_cores[index] = scan->core_index;
        streaming_requires_snapshot_ = true;
      } else if (const auto* join = std::get_if<PhysicalNestedLoopJoinNode>(&physical.payload);
                 join != nullptr) {
        if (join->core_index >= streaming_cores_.size() || join->left.value() >= index ||
            join->right.value() >= index || !node_cores[join->left.value()].has_value() ||
            !node_cores[join->right.value()].has_value() ||
            *node_cores[join->left.value()] != join->core_index ||
            *node_cores[join->right.value()] != join->core_index) {
          return std::unexpected(InternalFailure("join inputs do not share their core"));
        }
        StreamingCorePlan& core = streaming_cores_[join->core_index];
        if (core.joined_sources.size() < 2U) {
          return std::unexpected(InternalFailure("joined source has no preceding prefix"));
        }
        core.joined_sources.back().join = join;
        core.join = join;
        core.leaf = &physical;
        node_cores[index] = join->core_index;
      } else if (const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&physical.payload);
                 lookup != nullptr) {
        auto joined = InspectJoinedAccess(physical, lookup->core_index, lookup->source);
        if (!joined.has_value()) {
          return std::unexpected(std::move(joined.error()));
        }
        StreamingCorePlan& core = streaming_cores_[lookup->core_index];
        if (!*joined) {
          if (has_access(core)) {
            return std::unexpected(InternalFailure("streaming rowid core is invalid"));
          }
          core.leaf = &physical;
          core.rowid_lookup = lookup;
        }
        node_cores[index] = lookup->core_index;
        streaming_requires_snapshot_ = true;
      } else if (const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&physical.payload);
                 index_scan != nullptr) {
        auto joined = InspectJoinedAccess(physical, index_scan->core_index, index_scan->source);
        if (!joined.has_value()) {
          return std::unexpected(std::move(joined.error()));
        }
        StreamingCorePlan& core = streaming_cores_[index_scan->core_index];
        if (!*joined) {
          if (has_access(core)) {
            return std::unexpected(InternalFailure("streaming index core is invalid"));
          }
          core.leaf = &physical;
          core.index_scan = index_scan;
        }
        node_cores[index] = index_scan->core_index;
        streaming_requires_snapshot_ = true;
      } else if (const auto* values = std::get_if<PhysicalValuesNode>(&physical.payload);
                 values != nullptr) {
        if (values->core_index >= streaming_cores_.size() ||
            has_access(streaming_cores_[values->core_index])) {
          return std::unexpected(InternalFailure("streaming VALUES core is invalid"));
        }
        streaming_cores_[values->core_index].leaf = &physical;
        streaming_cores_[values->core_index].values = values;
        node_cores[index] = values->core_index;
      } else if (const auto* filter = std::get_if<PhysicalFilterNode>(&physical.payload);
                 filter != nullptr) {
        if (filter->input.value() >= node_cores.size() ||
            !node_cores[filter->input.value()].has_value()) {
          return std::unexpected(InternalFailure("streaming filter input is invalid"));
        }
        const std::size_t core_index = AssumeValue(node_cores[filter->input.value()]);
        if (streaming_cores_[core_index].filter != nullptr) {
          return std::unexpected(InternalFailure("streaming core has duplicate filters"));
        }
        streaming_cores_[core_index].filter = filter;
        node_cores[index] = core_index;
      } else if (const auto* aggregate = std::get_if<PhysicalAggregateNode>(&physical.payload);
                 aggregate != nullptr) {
        if (aggregate->core_index >= streaming_cores_.size() ||
            aggregate->input.value() >= node_cores.size() ||
            !node_cores[aggregate->input.value()].has_value() ||
            AssumeValue(node_cores[aggregate->input.value()]) != aggregate->core_index ||
            streaming_cores_[aggregate->core_index].aggregate != nullptr) {
          return std::unexpected(InternalFailure("streaming aggregate core is invalid"));
        }
        streaming_cores_[aggregate->core_index].aggregate = aggregate;
        node_cores[index] = aggregate->core_index;
        streaming_aggregate_query_ = true;
      } else if (const auto* having = std::get_if<PhysicalHavingNode>(&physical.payload);
                 having != nullptr) {
        if (having->core_index >= streaming_cores_.size() ||
            having->input.value() >= node_cores.size() ||
            !node_cores[having->input.value()].has_value() ||
            AssumeValue(node_cores[having->input.value()]) != having->core_index ||
            streaming_cores_[having->core_index].having != nullptr) {
          return std::unexpected(InternalFailure("streaming HAVING core is invalid"));
        }
        streaming_cores_[having->core_index].having = having;
        node_cores[index] = having->core_index;
      } else if (const auto* projection = std::get_if<PhysicalProjectionNode>(&physical.payload);
                 projection != nullptr) {
        if (projection->core_index >= streaming_cores_.size() ||
            projection->input.value() >= node_cores.size() ||
            !node_cores[projection->input.value()].has_value() ||
            AssumeValue(node_cores[projection->input.value()]) != projection->core_index ||
            streaming_cores_[projection->core_index].projection != nullptr) {
          return std::unexpected(InternalFailure("streaming projection is invalid"));
        }
        streaming_cores_[projection->core_index].projection = projection;
        node_cores[index] = projection->core_index;
      } else if (const auto* distinct = std::get_if<PhysicalDistinctNode>(&physical.payload);
                 distinct != nullptr) {
        if (distinct->core_index >= streaming_cores_.size() ||
            distinct->input.value() >= node_cores.size() ||
            !node_cores[distinct->input.value()].has_value() ||
            AssumeValue(node_cores[distinct->input.value()]) != distinct->core_index ||
            streaming_cores_[distinct->core_index].distinct != nullptr) {
          return std::unexpected(InternalFailure("streaming DISTINCT core is invalid"));
        }
        streaming_cores_[distinct->core_index].distinct = distinct;
        node_cores[index] = distinct->core_index;
      } else if (const auto* compound = std::get_if<PhysicalCompoundNode>(&physical.payload);
                 compound != nullptr) {
        if (!ordered_compound_) {
          const bool concatenate = compound->operation == CompoundOperator::kUnionAll &&
                                   compound->strategy == PhysicalCompoundStrategy::kConcatenate;
          const bool set = compound->operation != CompoundOperator::kUnionAll &&
                           (compound->strategy == PhysicalCompoundStrategy::kEphemeralSet ||
                            compound->strategy == PhysicalCompoundStrategy::kUnionLimitOne);
          if (!concatenate && !set) {
            return std::unexpected(
                UnsupportedFailure("ordered compound SELECT lowering is not supported"));
          }
        }
        set_limit_one_ =
            set_limit_one_ || compound->strategy == PhysicalCompoundStrategy::kUnionLimitOne;
        compound_strategies.push_back(compound->strategy);
        ++compound_count;
      } else if (const auto* order = std::get_if<PhysicalAdvancedOrderNode>(&physical.payload);
                 order != nullptr) {
        if (!ordered_compound_ || advanced_order_ != nullptr) {
          return std::unexpected(InternalFailure("streaming advanced order is invalid"));
        }
        advanced_order_ = order;
      } else if (const auto* limit = std::get_if<PhysicalLimitNode>(&physical.payload);
                 limit != nullptr) {
        if (limit_ != nullptr) {
          return std::unexpected(InternalFailure("streaming query has duplicate LIMIT nodes"));
        }
        limit_ = limit;
      } else if (const auto* output = std::get_if<PhysicalOutputNode>(&physical.payload);
                 output != nullptr) {
        if (!ordered_compound_ || output_ != nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("streaming output metadata is invalid"));
        }
        output_ = output;
      } else {
        return std::unexpected(
            UnsupportedFailure("streaming query contains an unsupported physical node"));
      }
    }

    if (compound_count != bound_select_->compound_operators().size()) {
      return std::unexpected(InternalFailure("streaming compound count is invalid"));
    }
    if (ordered_compound_) {
      if (advanced_order_ == nullptr || output_ == nullptr ||
          advanced_order_->core_plans.size() != cores.size() ||
          advanced_order_->terms.size() != bound_select_->order_by().size()) {
        return std::unexpected(InternalFailure("ordered compound metadata is incomplete"));
      }
      for (std::size_t index = 0; index < compound_strategies.size(); ++index) {
        const CompoundOperator operation = bound_select_->compound_operators()[index];
        const PhysicalCompoundStrategy expected =
            !advanced_order_->set_then_order ? PhysicalCompoundStrategy::kOrderedMerge
                                             : (operation == CompoundOperator::kUnionAll
                                                    ? PhysicalCompoundStrategy::kConcatenate
                                                    : PhysicalCompoundStrategy::kSetThenOrder);
        if (compound_strategies[index] != expected) {
          return std::unexpected(InternalFailure("ordered compound strategy is invalid"));
        }
      }
      if (advanced_order_->input_order_satisfied) {
        if (cores.size() != 1U || !bound_select_->compound_operators().empty() ||
            !streaming_aggregate_query_ || advanced_order_->set_then_order) {
          return std::unexpected(InternalFailure("satisfied advanced aggregate order is invalid"));
        }
        ordered_compound_ = false;
      }
    } else if (advanced_order_ != nullptr || output_ != nullptr) {
      return std::unexpected(InternalFailure("unordered compound has ordering metadata"));
    }
    for (const StreamingCorePlan& core : streaming_cores_) {
      if (core.bound == nullptr) {
        return std::unexpected(InternalFailure("streaming core has no bound metadata"));
      }
      if (std::holds_alternative<BoundValuesCore>(*core.bound)) {
        if (core.values == nullptr || core.empty != nullptr || core.single_row != nullptr ||
            core.table_scan != nullptr || core.rowid_lookup != nullptr ||
            core.index_scan != nullptr || core.filter != nullptr || core.aggregate != nullptr ||
            core.having != nullptr || core.projection != nullptr || core.distinct != nullptr) {
          return std::unexpected(InternalFailure("streaming VALUES core shape is invalid"));
        }
        continue;
      }
      const auto& select = std::get<BoundSelectCore>(*core.bound);
      const bool distinct_matches =
          (select.quantifier == SelectQuantifier::kDistinct) == (core.distinct != nullptr);
      if (core.values != nullptr || core.projection == nullptr || !distinct_matches) {
        return std::unexpected(InternalFailure("streaming SELECT core shape is invalid"));
      }
      const bool has_cursor_access = core.table_scan != nullptr || core.rowid_lookup != nullptr ||
                                     core.index_scan != nullptr || core.join != nullptr;
      const bool source_matches = !select.table_sources.empty()
                                      ? has_cursor_access || core.empty != nullptr
                                      : core.single_row != nullptr || core.empty != nullptr;
      if (!source_matches) {
        return std::unexpected(InternalFailure("streaming SELECT access shape is invalid"));
      }
      if (select.table_sources.size() > 1U) {
        if (core.join == nullptr || core.joined_sources.size() != select.table_sources.size() ||
            (core.filter != nullptr &&
             core.filter->predicate_sources.size() != core.filter->predicates.size())) {
          return std::unexpected(
              InternalFailure("joined core shape or predicate schedule is invalid"));
        }
      }
      if (!select.aggregate.has_value()) {
        if (core.aggregate != nullptr || core.having != nullptr || core.empty != nullptr ||
            core.rowid_lookup != nullptr || core.index_scan != nullptr) {
          return std::unexpected(
              InternalFailure("ordinary streaming SELECT retained aggregate access metadata"));
        }
        continue;
      }
      if (core.aggregate == nullptr ||
          (select.aggregate->having.has_value() != (core.having != nullptr)) ||
          (core.having != nullptr && core.having->predicate != *select.aggregate->having)) {
        return std::unexpected(InternalFailure("streaming aggregate shape is incomplete"));
      }
      const bool grouped = !select.aggregate->group_by.empty();
      if (grouped != (core.aggregate->strategy == PhysicalAggregateStrategy::kOrderedGroup ||
                      core.aggregate->strategy == PhysicalAggregateStrategy::kSortedGroup) ||
          (!grouped && core.aggregate->strategy != PhysicalAggregateStrategy::kGlobal &&
           core.aggregate->strategy != PhysicalAggregateStrategy::kIndexedMinMax) ||
          (!grouped && !core.aggregate->at_most_one_row)) {
        return std::unexpected(InternalFailure("streaming aggregate strategy is invalid"));
      }
      for (const BoundAggregateInvocationId invocation_id : select.aggregate->invocations) {
        if (invocation_id.value() >= aggregate_invocations_.size()) {
          return std::unexpected(InternalFailure("streaming aggregate invocation ID is invalid"));
        }
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> InspectDistinctPlan() {
    const std::span<const PhysicalNode> nodes = read_plan_->nodes();
    if (nodes.size() < 3U || bound_select_->query_cores().size() != 1U ||
        !bound_select_->compound_operators().empty()) {
      return std::unexpected(InternalFailure("simple DISTINCT plan shape is invalid"));
    }
    const auto* core = std::get_if<BoundSelectCore>(&bound_select_->query_cores().front());
    if (core == nullptr || core->quantifier != SelectQuantifier::kDistinct) {
      return std::unexpected(InternalFailure("simple DISTINCT bound core is invalid"));
    }

    leaf_ = &nodes.front();
    table_scan_ = std::get_if<PhysicalTableScanNode>(&leaf_->payload);
    if (!std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload) && table_scan_ == nullptr) {
      return std::unexpected(UnsupportedFailure("simple DISTINCT access kind is not implemented"));
    }

    for (std::size_t index = 1; index < nodes.size(); ++index) {
      const PhysicalNode& node = nodes[index];
      if (const auto* filter = std::get_if<PhysicalFilterNode>(&node.payload); filter != nullptr) {
        if (filter_ != nullptr) {
          return std::unexpected(InternalFailure("DISTINCT plan has duplicate filters"));
        }
        filter_ = filter;
      } else if (const auto* projection = std::get_if<PhysicalProjectionNode>(&node.payload);
                 projection != nullptr) {
        if (projection_ != nullptr || projection->core_index != 0U) {
          return std::unexpected(InternalFailure("DISTINCT projection metadata is invalid"));
        }
        projection_ = projection;
      } else if (const auto* distinct = std::get_if<PhysicalDistinctNode>(&node.payload);
                 distinct != nullptr) {
        if (distinct_ != nullptr || projection_ == nullptr || distinct->core_index != 0U ||
            distinct->input.value() != projection_->logical_projection.value()) {
          return std::unexpected(InternalFailure("DISTINCT membership metadata is invalid"));
        }
        distinct_ = distinct;
      } else if (const auto* order = std::get_if<PhysicalAdvancedOrderNode>(&node.payload);
                 order != nullptr) {
        if (advanced_order_ != nullptr || distinct_ == nullptr || order->set_then_order ||
            order->core_plans.size() != 1U || order->core_plans[0].layout.core_index != 0U ||
            order->core_plans[0].strategy != PhysicalSortStrategy::kExternal ||
            order->final_strategy != PhysicalSortStrategy::kExternal) {
          return std::unexpected(InternalFailure("DISTINCT ordering metadata is invalid"));
        }
        advanced_order_ = order;
      } else if (const auto* limit = std::get_if<PhysicalLimitNode>(&node.payload);
                 limit != nullptr) {
        if (limit_ != nullptr || distinct_ == nullptr) {
          return std::unexpected(InternalFailure("DISTINCT LIMIT metadata is invalid"));
        }
        limit_ = limit;
      } else if (const auto* output = std::get_if<PhysicalOutputNode>(&node.payload);
                 output != nullptr) {
        if (output_ != nullptr || advanced_order_ == nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("DISTINCT output metadata is invalid"));
        }
        output_ = output;
      } else {
        return std::unexpected(InternalFailure("DISTINCT plan contains an unexpected node"));
      }
    }
    if (projection_ == nullptr || distinct_ == nullptr ||
        distinct_->collations.size() != bound_select_->result_columns().size() ||
        (advanced_order_ == nullptr) != (output_ == nullptr)) {
      return std::unexpected(InternalFailure("DISTINCT plan is incomplete"));
    }
    return {};
  }

  void CollectAggregateRepresentativeLeaves(
      BoundExpressionId id, std::vector<BoundExpressionId>* leaves,
      std::vector<BoundSourceLeafIdentity>* identities) const {
    const BoundExpression& expression = expressions_[id.value()];
    if (const std::optional<BoundSourceLeafIdentity> identity = AggregateIdentity(id);
        identity.has_value()) {
      if (!std::ranges::contains(*identities, *identity)) {
        identities->push_back(*identity);
        leaves->push_back(id);
      }
      return;
    }
    if (std::holds_alternative<BoundLiteralExpression>(expression.payload) ||
        std::holds_alternative<BoundParameterExpression>(expression.payload) ||
        std::holds_alternative<BoundAggregateCallExpression>(expression.payload)) {
      return;
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      CollectAggregateRepresentativeLeaves(alias->target, leaves, identities);
    } else if (const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
               unary != nullptr) {
      CollectAggregateRepresentativeLeaves(unary->operand, leaves, identities);
    } else if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
               binary != nullptr) {
      CollectAggregateRepresentativeLeaves(binary->left, leaves, identities);
      CollectAggregateRepresentativeLeaves(binary->right, leaves, identities);
    } else if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.payload);
               comparison != nullptr) {
      CollectAggregateRepresentativeLeaves(comparison->left, leaves, identities);
      CollectAggregateRepresentativeLeaves(comparison->right, leaves, identities);
    } else if (const auto* truth = std::get_if<BoundTruthTestExpression>(&expression.payload);
               truth != nullptr) {
      CollectAggregateRepresentativeLeaves(truth->operand, leaves, identities);
    } else if (const auto* call = std::get_if<BoundScalarCallExpression>(&expression.payload);
               call != nullptr) {
      for (const BoundExpressionId argument : call->arguments) {
        CollectAggregateRepresentativeLeaves(argument, leaves, identities);
      }
    } else if (const auto* coalesce = std::get_if<BoundCoalesceExpression>(&expression.payload);
               coalesce != nullptr) {
      for (const BoundExpressionId argument : coalesce->arguments) {
        CollectAggregateRepresentativeLeaves(argument, leaves, identities);
      }
    } else if (const auto* conditional =
                   std::get_if<BoundConditionalExpression>(&expression.payload);
               conditional != nullptr) {
      for (const BoundExpressionId argument : conditional->arguments) {
        CollectAggregateRepresentativeLeaves(argument, leaves, identities);
      }
    } else if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&expression.payload);
               likelihood != nullptr) {
      CollectAggregateRepresentativeLeaves(likelihood->operand, leaves, identities);
    } else if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
               collate != nullptr) {
      CollectAggregateRepresentativeLeaves(collate->operand, leaves, identities);
    }
  }

  [[nodiscard]] bool ExpressionIsDirectGroupedRepresentative(BoundExpressionId id) const {
    const BoundExpression& expression = expressions_[id.value()];
    if (std::holds_alternative<BoundColumnExpression>(expression.payload) ||
        std::holds_alternative<BoundRowIdExpression>(expression.payload)) {
      return true;
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      return ExpressionIsDirectGroupedRepresentative(alias->target);
    }
    if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&expression.payload);
        likelihood != nullptr) {
      return ExpressionIsDirectGroupedRepresentative(likelihood->operand);
    }
    if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
        collate != nullptr) {
      return ExpressionIsDirectGroupedRepresentative(collate->operand);
    }
    return false;
  }

  [[nodiscard]] LoweringResult<void> InspectGlobalAggregatePlan() {
    const std::span<const PhysicalNode> nodes = read_plan_->nodes();
    if (bound_select_->query_cores().size() != 1U || !bound_select_->compound_operators().empty() ||
        nodes.size() < 3U) {
      return std::unexpected(
          UnsupportedFailure("global aggregate statement shape is not implemented"));
    }
    aggregate_core_ = std::get_if<BoundSelectCore>(&bound_select_->query_cores().front());
    if (aggregate_core_ == nullptr || aggregate_core_->quantifier == SelectQuantifier::kDistinct ||
        !aggregate_core_->aggregate.has_value() || !aggregate_core_->aggregate->group_by.empty()) {
      return std::unexpected(
          UnsupportedFailure("grouped or DISTINCT aggregate lowering is not implemented"));
    }
    aggregate_program_ = &*aggregate_core_->aggregate;
    std::vector<BoundSourceLeafIdentity> representative_identities;
    for (const BoundResultColumn& result : aggregate_core_->result_columns) {
      CollectAggregateRepresentativeLeaves(result.expression, &global_representative_leaves_,
                                           &representative_identities);
    }
    if (aggregate_program_->having.has_value()) {
      CollectAggregateRepresentativeLeaves(
          *aggregate_program_->having, &global_representative_leaves_, &representative_identities);
    }
    global_representative_query_ = !global_representative_leaves_.empty();
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      if (invocation_id.value() >= aggregate_invocations_.size()) {
        return std::unexpected(InternalFailure("aggregate invocation ID is out of range"));
      }
    }

    leaf_ = &nodes.front();
    table_scan_ = std::get_if<PhysicalTableScanNode>(&leaf_->payload);
    rowid_lookup_ = std::get_if<PhysicalRowIdLookupNode>(&leaf_->payload);
    index_scan_ = std::get_if<PhysicalIndexScanNode>(&leaf_->payload);
    for (std::size_t index = 1; index < nodes.size(); ++index) {
      const PhysicalNode& node = nodes[index];
      if (const auto* filter = std::get_if<PhysicalFilterNode>(&node.payload); filter != nullptr) {
        if (filter_ != nullptr) {
          return std::unexpected(InternalFailure("global aggregate has duplicate filters"));
        }
        filter_ = filter;
      } else if (const auto* aggregate = std::get_if<PhysicalAggregateNode>(&node.payload);
                 aggregate != nullptr) {
        if (aggregate_ != nullptr || aggregate->core_index != 0U || !aggregate->at_most_one_row) {
          return std::unexpected(InternalFailure("global aggregate node is invalid"));
        }
        aggregate_ = aggregate;
      } else if (const auto* having = std::get_if<PhysicalHavingNode>(&node.payload);
                 having != nullptr) {
        if (having_ != nullptr || aggregate_ == nullptr || having->core_index != 0U) {
          return std::unexpected(InternalFailure("global HAVING node is invalid"));
        }
        having_ = having;
      } else if (const auto* sort = std::get_if<PhysicalSortNode>(&node.payload); sort != nullptr) {
        if (sort_ != nullptr || aggregate_ == nullptr || !sort->input_order_satisfied) {
          return std::unexpected(InternalFailure("global aggregate order is invalid"));
        }
        sort_ = sort;
      } else if (const auto* limit = std::get_if<PhysicalLimitNode>(&node.payload);
                 limit != nullptr) {
        if (limit_ != nullptr || aggregate_ == nullptr) {
          return std::unexpected(InternalFailure("global aggregate LIMIT is invalid"));
        }
        limit_ = limit;
      } else if (const auto* projection = std::get_if<PhysicalProjectionNode>(&node.payload);
                 projection != nullptr) {
        if (projection_ != nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("global aggregate projection is invalid"));
        }
        projection_ = projection;
      } else if (const auto* output = std::get_if<PhysicalOutputNode>(&node.payload);
                 output != nullptr) {
        if (output_ != nullptr || sort_ == nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("global aggregate output is invalid"));
        }
        output_ = output;
      } else {
        return std::unexpected(
            InternalFailure("global aggregate plan contains an unexpected node"));
      }
    }
    if (aggregate_ == nullptr || (sort_ == nullptr) != (output_ == nullptr) ||
        (sort_ == nullptr) != (projection_ != nullptr) ||
        (aggregate_->strategy != PhysicalAggregateStrategy::kGlobal &&
         aggregate_->strategy != PhysicalAggregateStrategy::kIndexedMinMax)) {
      return std::unexpected(
          UnsupportedFailure("grouped or malformed aggregate lowering is not implemented"));
    }
    if (aggregate_->strategy == PhysicalAggregateStrategy::kIndexedMinMax) {
      if (!aggregate_->min_max.has_value() || having_ != nullptr ||
          aggregate_program_->invocations.size() != 1U ||
          aggregate_->min_max->invocation != aggregate_program_->invocations.front()) {
        return std::unexpected(InternalFailure("indexed MIN/MAX metadata is invalid"));
      }
      const BoundAggregateInvocation& invocation =
          aggregate_invocations_[aggregate_->min_max->invocation.value()];
      if (invocation.arguments.size() != 1U || invocation.distinct ||
          invocation.filter.has_value() || !invocation.order_by.empty()) {
        return std::unexpected(InternalFailure("indexed MIN/MAX invocation is invalid"));
      }
      const BoundAggregateFunction& function = aggregate_functions_[invocation.function.value()];
      if (function.extremum == BoundAggregateExtremumKind::kNone) {
        return std::unexpected(InternalFailure("indexed MIN/MAX function is invalid"));
      }
    }
    if (!std::holds_alternative<PhysicalEmptyNode>(leaf_->payload) &&
        !std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload) && table_scan_ == nullptr &&
        rowid_lookup_ == nullptr && index_scan_ == nullptr) {
      return std::unexpected(InternalFailure("global aggregate access node is invalid"));
    }
    global_aggregate_query_ = true;
    return {};
  }

  [[nodiscard]] LoweringResult<void> InspectGroupedAggregatePlan() {
    const std::span<const PhysicalNode> nodes = read_plan_->nodes();
    if (bound_select_->query_cores().size() != 1U || !bound_select_->compound_operators().empty() ||
        nodes.size() < 3U) {
      return std::unexpected(
          UnsupportedFailure("grouped aggregate statement shape is not implemented"));
    }
    aggregate_core_ = std::get_if<BoundSelectCore>(&bound_select_->query_cores().front());
    if (aggregate_core_ == nullptr || aggregate_core_->quantifier == SelectQuantifier::kDistinct ||
        !aggregate_core_->aggregate.has_value() || aggregate_core_->aggregate->group_by.empty()) {
      return std::unexpected(
          UnsupportedFailure("grouped or DISTINCT aggregate lowering is not implemented"));
    }
    aggregate_program_ = &*aggregate_core_->aggregate;
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      if (invocation_id.value() >= aggregate_invocations_.size()) {
        return std::unexpected(InternalFailure("aggregate invocation ID is out of range"));
      }
    }

    leaf_ = &nodes.front();
    table_scan_ = std::get_if<PhysicalTableScanNode>(&leaf_->payload);
    rowid_lookup_ = std::get_if<PhysicalRowIdLookupNode>(&leaf_->payload);
    index_scan_ = std::get_if<PhysicalIndexScanNode>(&leaf_->payload);
    for (std::size_t index = 1; index < nodes.size(); ++index) {
      const PhysicalNode& node = nodes[index];
      if (const auto* filter = std::get_if<PhysicalFilterNode>(&node.payload); filter != nullptr) {
        if (filter_ != nullptr) {
          return std::unexpected(InternalFailure("grouped aggregate has duplicate filters"));
        }
        filter_ = filter;
      } else if (const auto* aggregate = std::get_if<PhysicalAggregateNode>(&node.payload);
                 aggregate != nullptr) {
        if (aggregate_ != nullptr || aggregate->core_index != 0U ||
            (aggregate->strategy != PhysicalAggregateStrategy::kOrderedGroup &&
             aggregate->strategy != PhysicalAggregateStrategy::kSortedGroup) ||
            aggregate->min_max.has_value()) {
          return std::unexpected(InternalFailure("grouped aggregate node is invalid"));
        }
        aggregate_ = aggregate;
      } else if (const auto* having = std::get_if<PhysicalHavingNode>(&node.payload);
                 having != nullptr) {
        if (having_ != nullptr || aggregate_ == nullptr || having->core_index != 0U) {
          return std::unexpected(InternalFailure("grouped HAVING node is invalid"));
        }
        having_ = having;
      } else if (const auto* sort = std::get_if<PhysicalSortNode>(&node.payload); sort != nullptr) {
        if (sort_ != nullptr || aggregate_ == nullptr) {
          return std::unexpected(InternalFailure("grouped aggregate order is invalid"));
        }
        sort_ = sort;
      } else if (const auto* limit = std::get_if<PhysicalLimitNode>(&node.payload);
                 limit != nullptr) {
        if (limit_ != nullptr || aggregate_ == nullptr) {
          return std::unexpected(InternalFailure("grouped aggregate LIMIT is invalid"));
        }
        limit_ = limit;
      } else if (const auto* projection = std::get_if<PhysicalProjectionNode>(&node.payload);
                 projection != nullptr) {
        if (projection_ != nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("grouped aggregate projection is invalid"));
        }
        projection_ = projection;
      } else if (const auto* output = std::get_if<PhysicalOutputNode>(&node.payload);
                 output != nullptr) {
        if (output_ != nullptr || sort_ == nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("grouped aggregate output is invalid"));
        }
        output_ = output;
      } else {
        return std::unexpected(
            InternalFailure("grouped aggregate plan contains an unexpected node"));
      }
    }

    if (aggregate_ == nullptr || (sort_ == nullptr) != (output_ == nullptr) ||
        (sort_ == nullptr) != (projection_ != nullptr)) {
      return std::unexpected(InternalFailure("grouped aggregate plan is incomplete"));
    }
    if (sort_ != nullptr && !sort_->input_order_satisfied &&
        sort_->strategy == PhysicalSortStrategy::kExternal &&
        (sort_->schedule != OrderEvaluationSchedule::kPayloadThenKeys || limit_ != nullptr)) {
      return std::unexpected(InternalFailure("grouped external ORDER BY plan shape is invalid"));
    }
    if (sort_ != nullptr && !sort_->input_order_satisfied &&
        sort_->strategy == PhysicalSortStrategy::kRuntimeLimit &&
        (sort_->schedule != OrderEvaluationSchedule::kKeysThenAdmissionThenPayload ||
         limit_ == nullptr)) {
      return std::unexpected(
          InternalFailure("grouped runtime LIMIT ORDER BY plan shape is invalid"));
    }
    if (!std::holds_alternative<PhysicalEmptyNode>(leaf_->payload) &&
        !std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload) && table_scan_ == nullptr &&
        rowid_lookup_ == nullptr && index_scan_ == nullptr) {
      return std::unexpected(InternalFailure("grouped aggregate access node is invalid"));
    }
    grouped_aggregate_query_ = true;
    return {};
  }

  [[nodiscard]] LoweringResult<void> InspectAggregatePlan() {
    if (bound_select_->query_cores().size() == 1U) {
      const auto* core = std::get_if<BoundSelectCore>(&bound_select_->query_cores().front());
      if (core != nullptr && core->aggregate.has_value() && !core->aggregate->group_by.empty()) {
        return InspectGroupedAggregatePlan();
      }
    }
    return InspectGlobalAggregatePlan();
  }

  [[nodiscard]] LoweringResult<void> InspectPlan() {
    if (std::ranges::any_of(read_plan_->nodes(), [](const PhysicalNode& node) {
          return std::holds_alternative<PhysicalValuesNode>(node.payload) ||
                 std::holds_alternative<PhysicalCompoundNode>(node.payload) ||
                 std::holds_alternative<PhysicalAggregateNode>(node.payload) ||
                 std::holds_alternative<PhysicalNestedLoopJoinNode>(node.payload);
        })) {
      return InspectStreamingPlan();
    }
    if (std::ranges::any_of(read_plan_->nodes(), [](const PhysicalNode& node) {
          return std::holds_alternative<PhysicalDistinctNode>(node.payload);
        })) {
      return InspectDistinctPlan();
    }
    const std::span<const PhysicalNode> nodes = read_plan_->nodes();
    if (nodes.size() < 2U) {
      return std::unexpected(InternalFailure("physical plan has no lowering chain"));
    }
    leaf_ = &nodes.front();
    table_scan_ = std::get_if<PhysicalTableScanNode>(&leaf_->payload);
    rowid_lookup_ = std::get_if<PhysicalRowIdLookupNode>(&leaf_->payload);
    index_scan_ = std::get_if<PhysicalIndexScanNode>(&leaf_->payload);
    for (std::size_t index = 1; index < nodes.size(); ++index) {
      const PhysicalNode& node = nodes[index];
      if (const auto* guard = std::get_if<PhysicalGuardNode>(&node.payload); guard != nullptr) {
        if (guard_ != nullptr) {
          return std::unexpected(InternalFailure("physical plan has duplicate guards"));
        }
        guard_ = guard;
      } else if (const auto* filter = std::get_if<PhysicalFilterNode>(&node.payload);
                 filter != nullptr) {
        if (filter_ != nullptr) {
          return std::unexpected(InternalFailure("physical plan has duplicate filters"));
        }
        filter_ = filter;
      } else if (const auto* limit = std::get_if<PhysicalLimitNode>(&node.payload);
                 limit != nullptr) {
        if (limit_ != nullptr) {
          return std::unexpected(InternalFailure("physical plan has duplicate limits"));
        }
        limit_ = limit;
      } else if (const auto* projection = std::get_if<PhysicalProjectionNode>(&node.payload);
                 projection != nullptr) {
        if (projection_ != nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("physical projection is not the final node"));
        }
        projection_ = projection;
      } else if (const auto* sort = std::get_if<PhysicalSortNode>(&node.payload); sort != nullptr) {
        if (sort_ != nullptr) {
          return std::unexpected(InternalFailure("physical plan has duplicate sort nodes"));
        }
        sort_ = sort;
      } else if (const auto* output = std::get_if<PhysicalOutputNode>(&node.payload);
                 output != nullptr) {
        if (output_ != nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("physical output is not the final node"));
        }
        output_ = output;
      } else {
        return std::unexpected(InternalFailure("physical plan contains an unexpected node"));
      }
    }
    if ((sort_ == nullptr) != (output_ == nullptr) ||
        (sort_ == nullptr) != (projection_ != nullptr)) {
      return std::unexpected(InternalFailure("physical output shape is inconsistent"));
    }
    if (sort_ != nullptr && sort_->input_order_satisfied) {
      return std::unexpected(UnsupportedFailure("ORDER BY input-order elimination is unsupported"));
    }
    if (sort_ != nullptr && sort_->strategy == PhysicalSortStrategy::kExternal &&
        (sort_->schedule != OrderEvaluationSchedule::kPayloadThenKeys || limit_ != nullptr)) {
      return std::unexpected(InternalFailure("external ORDER BY plan shape is invalid"));
    }
    if (sort_ != nullptr && sort_->strategy == PhysicalSortStrategy::kRuntimeLimit &&
        (sort_->schedule != OrderEvaluationSchedule::kKeysThenAdmissionThenPayload ||
         limit_ == nullptr)) {
      return std::unexpected(InternalFailure("runtime LIMIT ORDER BY plan shape is invalid"));
    }
    if (!std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload) &&
        !std::holds_alternative<PhysicalEmptyNode>(leaf_->payload) &&
        !std::holds_alternative<PhysicalTableScanNode>(leaf_->payload) &&
        !std::holds_alternative<PhysicalRowIdLookupNode>(leaf_->payload) &&
        !std::holds_alternative<PhysicalIndexScanNode>(leaf_->payload)) {
      return std::unexpected(InternalFailure("physical plan has an invalid access node"));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<RegisterId> AllocateRegisters(std::size_t count) {
    if (count >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - next_register_) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kRegisterLimitExceeded},
                         "lowering register count exceeds the bytecode identity range"));
    }
    const RegisterId first{static_cast<std::uint32_t>(next_register_)};
    next_register_ += count;
    return first;
  }

  [[nodiscard]] LoweringResult<void> BeginExpressionRegisterLayout() {
    if (statement_.expression_layout_initialized) {
      return {};
    }
    const std::size_t expression_count = expressions_.size();
    if (expression_count > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kRegisterLimitExceeded},
                         "bound expression count exceeds the bytecode identity range"));
    }
    next_register_ = expression_count;
    call_argument_blocks_.resize(expression_count);
    literal_constants_.resize(expression_count);

    for (std::size_t index = 0; index < expression_count; ++index) {
      const auto* call = std::get_if<BoundScalarCallExpression>(&expressions_[index].payload);
      if (call == nullptr) {
        continue;
      }
      auto block = AllocateRegisters(call->arguments.size());
      if (!block.has_value()) {
        return std::unexpected(std::move(block.error()));
      }
      call_argument_blocks_[index] = *block;
    }
    statement_.expression_layout_initialized = true;
    return {};
  }

  [[nodiscard]] LoweringResult<void> FinishRegisterLayout() {
    if (next_register_ > limits_.maximum_registers) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kRegisterLimitExceeded},
                         "lowering register count exceeds the configured limit"));
    }
    if (parameters_.size() > std::numeric_limits<std::uint32_t>::max() ||
        parameters_.size() > limits_.maximum_parameters) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kParameterLimitExceeded},
                         "lowering parameter count exceeds the configured limit"));
    }

    register_count_ = static_cast<std::uint32_t>(next_register_);
    parameter_count_ = static_cast<std::uint32_t>(parameters_.size());
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateAggregateInvocationRegisters() {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("aggregate program is unavailable"));
    }
    aggregate_argument_blocks_.resize(aggregate_invocations_.size());
    aggregate_result_registers_.resize(aggregate_invocations_.size());
    aggregate_descriptors_.resize(aggregate_invocations_.size());
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      const BoundAggregateInvocation& invocation = aggregate_invocations_[invocation_id.value()];
      auto arguments = AllocateRegisters(invocation.arguments.size());
      auto result = AllocateRegisters(1);
      if (!arguments.has_value()) {
        return std::unexpected(std::move(arguments.error()));
      }
      if (!result.has_value()) {
        return std::unexpected(std::move(result.error()));
      }
      aggregate_argument_blocks_[invocation_id.value()] = *arguments;
      aggregate_result_registers_[invocation_id.value()] = *result;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateQueryAggregateInvocationRegisters() {
    aggregate_argument_blocks_.resize(aggregate_invocations_.size());
    aggregate_result_registers_.resize(aggregate_invocations_.size());
    aggregate_descriptors_.resize(aggregate_invocations_.size());
    for (const BoundQueryCore& query_core : bound_select_->query_cores()) {
      const auto* core = std::get_if<BoundSelectCore>(&query_core);
      if (core == nullptr || !core->aggregate.has_value()) {
        continue;
      }
      for (const BoundAggregateInvocationId id : core->aggregate->invocations) {
        const BoundAggregateInvocation& invocation = aggregate_invocations_[id.value()];
        auto arguments = AllocateRegisters(invocation.arguments.size());
        auto result = AllocateRegisters(1);
        if (!arguments.has_value()) {
          return std::unexpected(std::move(arguments.error()));
        }
        if (!result.has_value()) {
          return std::unexpected(std::move(result.error()));
        }
        aggregate_argument_blocks_[id.value()] = *arguments;
        aggregate_result_registers_[id.value()] = *result;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateAggregateResultRegisters() {
    auto result_block = AllocateRegisters(bound_select_->result_columns().size());
    if (!result_block.has_value()) {
      return std::unexpected(std::move(result_block.error()));
    }
    result_block_first_ = *result_block;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateGlobalRepresentativeRegisters() {
    if (!global_representative_query_) {
      return {};
    }
    auto representative = AllocateRegisters(global_representative_leaves_.size());
    if (!representative.has_value()) {
      return std::unexpected(std::move(representative.error()));
    }
    global_representative_registers_.reserve(global_representative_leaves_.size());
    for (std::size_t index = 0; index < global_representative_leaves_.size(); ++index) {
      global_representative_registers_.emplace_back(representative->value() +
                                                    static_cast<std::uint32_t>(index));
    }
    if (auto mapped = MapAggregateLeafRegisters(global_representative_leaves_,
                                                global_representative_registers_,
                                                global_representative_register_map_);
        !mapped.has_value()) {
      return mapped;
    }
    if (HasAggregateRepresentativeController()) {
      auto comparison = AllocateRegisters(1);
      auto effect = AllocateRegisters(1);
      auto replacement = AllocateRegisters(1);
      if (!comparison.has_value()) {
        return std::unexpected(std::move(comparison.error()));
      }
      if (!effect.has_value()) {
        return std::unexpected(std::move(effect.error()));
      }
      if (!replacement.has_value()) {
        return std::unexpected(std::move(replacement.error()));
      }
      global_representative_comparison_register_ = *comparison;
      global_representative_effect_register_ = *effect;
      global_representative_replace_register_ = *replacement;
      return {};
    }
    auto has_row = AllocateRegisters(1);
    if (!has_row.has_value()) {
      return std::unexpected(std::move(has_row.error()));
    }
    global_has_representative_row_register_ = *has_row;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateGlobalLimitRegisters() {
    if (limit_ == nullptr) {
      return {};
    }
    auto limit_register = AllocateRegisters(1);
    if (!limit_register.has_value()) {
      return std::unexpected(std::move(limit_register.error()));
    }
    limit_register_ = *limit_register;
    if (!limit_->offset.has_value()) {
      return {};
    }
    auto offset_register = AllocateRegisters(1);
    auto zero_register = AllocateRegisters(1);
    auto one_register = AllocateRegisters(1);
    auto comparison_register = AllocateRegisters(1);
    if (!offset_register.has_value()) {
      return std::unexpected(std::move(offset_register.error()));
    }
    if (!zero_register.has_value()) {
      return std::unexpected(std::move(zero_register.error()));
    }
    if (!one_register.has_value()) {
      return std::unexpected(std::move(one_register.error()));
    }
    if (!comparison_register.has_value()) {
      return std::unexpected(std::move(comparison_register.error()));
    }
    offset_register_ = *offset_register;
    zero_register_ = *zero_register;
    one_register_ = *one_register;
    comparison_register_ = *comparison_register;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateAggregateAccessRegisters(
      bool nullable_indexed_min_max) {
    if (index_scan_ != nullptr) {
      std::size_t key_count =
          index_scan_->equalities.size() + static_cast<std::size_t>(index_scan_->range.has_value());
      if (nullable_indexed_min_max) {
        key_count = std::max(key_count, index_scan_->equalities.size() + 1U);
      }
      if (key_count != 0U) {
        auto key = AllocateRegisters(key_count);
        if (!key.has_value()) {
          return std::unexpected(std::move(key.error()));
        }
        index_key_first_ = *key;
        index_key_capacity_ = static_cast<std::uint32_t>(key_count);
      }
    }
    if (index_scan_ != nullptr && !index_scan_->covering) {
      auto rowid = AllocateRegisters(1);
      if (!rowid.has_value()) {
        return std::unexpected(std::move(rowid.error()));
      }
      index_rowid_register_ = *rowid;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> BuildGlobalAggregateRegisterLayout() {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("global aggregate metadata is unavailable"));
    }
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }
    if (auto invocations = AllocateAggregateInvocationRegisters(); !invocations.has_value()) {
      return invocations;
    }
    if (auto representative = AllocateGlobalRepresentativeRegisters();
        !representative.has_value()) {
      return representative;
    }
    if (auto modifiers = BuildAggregateModifierRegisterLayout(); !modifiers.has_value()) {
      return modifiers;
    }
    if (auto result = AllocateAggregateResultRegisters(); !result.has_value()) {
      return result;
    }
    if (auto limit = AllocateGlobalLimitRegisters(); !limit.has_value()) {
      return limit;
    }
    const bool nullable_indexed_min_max =
        aggregate_ != nullptr &&
        aggregate_->strategy == PhysicalAggregateStrategy::kIndexedMinMax &&
        aggregate_->min_max.has_value() &&
        aggregate_->min_max->null_strategy ==
            PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback;
    if (auto access = AllocateAggregateAccessRegisters(nullable_indexed_min_max);
        !access.has_value()) {
      return access;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] std::optional<BoundSourceLeafIdentity> AggregateIdentity(
      BoundExpressionId id) const noexcept {
    return BoundSourceLeafIdentityOf(expressions_[id.value()], bound_select_->source_columns());
  }

  [[nodiscard]] LoweringResult<void> BuildGroupedRetainedLeaves() {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("grouped aggregate program is unavailable"));
    }
    grouped_retained_leaves_ = aggregate_program_->source_leaves;
    std::vector<BoundSourceLeafIdentity> identities;
    identities.reserve(grouped_retained_leaves_.size() +
                       aggregate_program_->representative_leaves.size());
    for (const BoundExpressionId leaf : grouped_retained_leaves_) {
      const std::optional<BoundSourceLeafIdentity> identity = AggregateIdentity(leaf);
      if (!identity.has_value()) {
        return std::unexpected(InternalFailure("grouped source leaf is not a source identity"));
      }
      identities.push_back(*identity);
    }
    for (const BoundExpressionId leaf : aggregate_program_->representative_leaves) {
      const std::optional<BoundSourceLeafIdentity> identity = AggregateIdentity(leaf);
      if (!identity.has_value()) {
        return std::unexpected(
            InternalFailure("grouped representative leaf is not a source identity"));
      }
      if (!std::ranges::contains(identities, *identity)) {
        identities.push_back(*identity);
        grouped_retained_leaves_.push_back(leaf);
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> MapAggregateLeafRegisters(
      std::span<const BoundExpressionId> leaves, std::span<const RegisterId> registers,
      std::vector<AggregateLeafRegister>& mapping) {
    if (leaves.size() != registers.size()) {
      return std::unexpected(InternalFailure("grouped leaf register map is invalid"));
    }
    mapping.clear();
    mapping.reserve(leaves.size());
    for (std::size_t index = 0; index < leaves.size(); ++index) {
      const std::optional<BoundSourceLeafIdentity> identity = AggregateIdentity(leaves[index]);
      if (!identity.has_value()) {
        return std::unexpected(InternalFailure("grouped retained leaf identity is invalid"));
      }
      if (std::ranges::find(mapping, *identity, &AggregateLeafRegister::leaf) != mapping.end()) {
        return std::unexpected(InternalFailure("grouped retained leaf has duplicate bindings"));
      }
      mapping.push_back(AggregateLeafRegister{.leaf = *identity, .reg = registers[index]});
    }
    return {};
  }

  [[nodiscard]] bool HasAggregateRepresentativeController() const {
    if (aggregate_program_ == nullptr) {
      return false;
    }
    return std::ranges::any_of(
        aggregate_program_->invocations, [this](BoundAggregateInvocationId invocation_id) {
          const BoundAggregateInvocation& invocation =
              aggregate_invocations_[invocation_id.value()];
          return aggregate_functions_[invocation.function.value()].extremum !=
                 BoundAggregateExtremumKind::kNone;
        });
  }

  [[nodiscard]] LoweringResult<void> BuildAggregateModifierRegisterLayout() {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("aggregate modifier program is unavailable"));
    }
    aggregate_modifier_runtimes_.resize(aggregate_invocations_.size());
    bool has_filtered_controller = false;
    bool has_unfiltered_controller = false;
    const bool uses_representatives =
        global_representative_query_ ||
        (grouped_aggregate_query_ && !aggregate_program_->representative_leaves.empty());

    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      const BoundAggregateInvocation& invocation = aggregate_invocations_[invocation_id.value()];
      if (invocation.function.value() >= aggregate_functions_.size()) {
        return std::unexpected(InternalFailure("aggregate modifier function is unavailable"));
      }
      const BoundAggregateFunction& function = aggregate_functions_[invocation.function.value()];
      const bool controller =
          uses_representatives && function.extremum != BoundAggregateExtremumKind::kNone;
      const bool has_effective_order =
          function.extremum == BoundAggregateExtremumKind::kNone && !invocation.order_by.empty();
      if (invocation.argument_reuses_single_order_key &&
          (invocation.arguments.size() != 1U || invocation.order_by.size() != 1U)) {
        return std::unexpected(
            InternalFailure("aggregate argument/order reuse metadata is invalid"));
      }

      lowering_internal::AggregateModifierStrategy strategy =
          lowering_internal::AggregateModifierStrategy::Direct;
      if (invocation.distinct && has_effective_order &&
          invocation.argument_reuses_single_order_key) {
        strategy = lowering_internal::AggregateModifierStrategy::UniqueOrdered;
      } else if (invocation.distinct && has_effective_order) {
        strategy = lowering_internal::AggregateModifierStrategy::DistinctOrdered;
      } else if (has_effective_order) {
        strategy = lowering_internal::AggregateModifierStrategy::Ordered;
      } else if (invocation.distinct) {
        strategy = lowering_internal::AggregateModifierStrategy::Distinct;
      }

      std::optional<RegisterId> record_first;
      std::uint32_t order_key_count = 0;
      std::uint32_t record_field_count = 0;
      if (strategy == lowering_internal::AggregateModifierStrategy::Ordered ||
          strategy == lowering_internal::AggregateModifierStrategy::DistinctOrdered ||
          strategy == lowering_internal::AggregateModifierStrategy::UniqueOrdered) {
        const std::size_t field_count =
            invocation.argument_reuses_single_order_key
                ? 1U
                : invocation.order_by.size() + invocation.arguments.size();
        if (invocation.order_by.size() > std::numeric_limits<std::uint32_t>::max() ||
            field_count > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(
              ProgramFailure(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded},
                             "aggregate modifier record exceeds bytecode field identities"));
        }
        auto record = AllocateRegisters(field_count);
        if (!record.has_value()) {
          return std::unexpected(std::move(record.error()));
        }
        record_first = *record;
        order_key_count = static_cast<std::uint32_t>(invocation.order_by.size());
        record_field_count = static_cast<std::uint32_t>(field_count);
      }

      aggregate_modifier_runtimes_[invocation_id.value()] =
          lowering_internal::AggregateModifierRuntime{
              .strategy = strategy,
              .relation = std::nullopt,
              .sorter = std::nullopt,
              .record_first = record_first,
              .order_key_count = order_key_count,
              .record_field_count = record_field_count,
              .argument_reuses_single_order_key =
                  has_effective_order && invocation.argument_reuses_single_order_key,
              .representative_controller = controller,
      };
      if (controller && invocation.filter.has_value()) {
        has_filtered_controller = true;
      } else if (controller) {
        has_unfiltered_controller = true;
      }
    }

    global_filtered_controller_defaults_ =
        global_aggregate_query_ && has_filtered_controller && !has_unfiltered_controller;
    if (global_filtered_controller_defaults_ && !global_modifier_seen_row_register_.has_value()) {
      auto seen = AllocateRegisters(1);
      if (!seen.has_value()) {
        return std::unexpected(std::move(seen.error()));
      }
      global_modifier_seen_row_register_ = *seen;
    }
    return {};
  }

  [[nodiscard]] bool HasGroupedOuterOrder() const noexcept {
    return sort_ != nullptr && !sort_->input_order_satisfied;
  }

  [[nodiscard]] LoweringResult<void> AllocateGroupedOutputRegisters() {
    if (auto result = AllocateAggregateResultRegisters(); !result.has_value()) {
      return result;
    }
    if (!HasGroupedOuterOrder()) {
      return {};
    }
    const std::size_t field_count = sort_->terms.size() + sort_->payload_expressions.size();
    auto sorter_record = AllocateRegisters(field_count);
    if (!sorter_record.has_value()) {
      return std::unexpected(std::move(sorter_record.error()));
    }
    sorter_record_first_ = *sorter_record;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateGroupedLimitRegisters() {
    if (limit_ == nullptr) {
      return {};
    }
    auto limit_register = AllocateRegisters(1);
    auto zero_register = AllocateRegisters(1);
    auto one_register = AllocateRegisters(1);
    auto negative_register = AllocateRegisters(1);
    if (!limit_register.has_value()) {
      return std::unexpected(std::move(limit_register.error()));
    }
    if (!zero_register.has_value()) {
      return std::unexpected(std::move(zero_register.error()));
    }
    if (!one_register.has_value()) {
      return std::unexpected(std::move(one_register.error()));
    }
    if (!negative_register.has_value()) {
      return std::unexpected(std::move(negative_register.error()));
    }
    limit_register_ = *limit_register;
    zero_register_ = *zero_register;
    one_register_ = *one_register;
    negative_limit_register_ = *negative_register;

    if (limit_->offset.has_value()) {
      auto offset_register = AllocateRegisters(1);
      auto comparison_register = AllocateRegisters(1);
      if (!offset_register.has_value()) {
        return std::unexpected(std::move(offset_register.error()));
      }
      if (!comparison_register.has_value()) {
        return std::unexpected(std::move(comparison_register.error()));
      }
      offset_register_ = *offset_register;
      comparison_register_ = *comparison_register;
    }

    if (!HasGroupedOuterOrder() || sort_->strategy != PhysicalSortStrategy::kRuntimeLimit) {
      return {};
    }
    auto top_n_bound = AllocateRegisters(1);
    if (!top_n_bound.has_value()) {
      return std::unexpected(std::move(top_n_bound.error()));
    }
    top_n_bound_register_ = *top_n_bound;
    if (!limit_->offset.has_value()) {
      return {};
    }
    auto maximum_integer = AllocateRegisters(1);
    auto bound_room = AllocateRegisters(1);
    if (!maximum_integer.has_value()) {
      return std::unexpected(std::move(maximum_integer.error()));
    }
    if (!bound_room.has_value()) {
      return std::unexpected(std::move(bound_room.error()));
    }
    maximum_integer_register_ = *maximum_integer;
    bound_room_register_ = *bound_room;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateGroupedStateRegisters() {
    auto current_group = AllocateRegisters(aggregate_program_->group_by.size());
    auto previous_group = AllocateRegisters(aggregate_program_->group_by.size());
    auto has_group = AllocateRegisters(1);
    auto group_comparison = AllocateRegisters(1);
    if (!current_group.has_value()) {
      return std::unexpected(std::move(current_group.error()));
    }
    if (!previous_group.has_value()) {
      return std::unexpected(std::move(previous_group.error()));
    }
    if (!has_group.has_value()) {
      return std::unexpected(std::move(has_group.error()));
    }
    if (!group_comparison.has_value()) {
      return std::unexpected(std::move(group_comparison.error()));
    }
    grouped_current_group_first_ = *current_group;
    grouped_previous_group_first_ = *previous_group;
    grouped_has_group_register_ = *has_group;
    grouped_comparison_register_ = *group_comparison;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateGroupedRepresentativeRegisters() {
    if (aggregate_program_->representative_leaves.empty()) {
      return {};
    }
    auto representative = AllocateRegisters(aggregate_program_->representative_leaves.size());
    if (!representative.has_value()) {
      return std::unexpected(std::move(representative.error()));
    }
    grouped_representative_registers_.reserve(aggregate_program_->representative_leaves.size());
    for (std::size_t index = 0; index < aggregate_program_->representative_leaves.size(); ++index) {
      grouped_representative_registers_.emplace_back(representative->value() +
                                                     static_cast<std::uint32_t>(index));
    }
    if (auto mapped = MapAggregateLeafRegisters(aggregate_program_->representative_leaves,
                                                grouped_representative_registers_,
                                                grouped_representative_register_map_);
        !mapped.has_value()) {
      return mapped;
    }
    if (!HasAggregateRepresentativeController()) {
      return {};
    }
    auto effect = AllocateRegisters(1);
    auto replacement = AllocateRegisters(1);
    if (!effect.has_value()) {
      return std::unexpected(std::move(effect.error()));
    }
    if (!replacement.has_value()) {
      return std::unexpected(std::move(replacement.error()));
    }
    grouped_representative_effect_register_ = *effect;
    grouped_representative_replace_register_ = *replacement;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateGroupedSortedRegisters() {
    if (aggregate_->strategy != PhysicalAggregateStrategy::kSortedGroup) {
      return {};
    }
    if (auto retained = BuildGroupedRetainedLeaves(); !retained.has_value()) {
      return retained;
    }
    auto retained_registers = AllocateRegisters(grouped_retained_leaves_.size());
    auto sorter_record =
        AllocateRegisters(aggregate_program_->group_by.size() + grouped_retained_leaves_.size());
    if (!retained_registers.has_value()) {
      return std::unexpected(std::move(retained_registers.error()));
    }
    if (!sorter_record.has_value()) {
      return std::unexpected(std::move(sorter_record.error()));
    }
    grouped_retained_leaf_registers_.reserve(grouped_retained_leaves_.size());
    for (std::size_t index = 0; index < grouped_retained_leaves_.size(); ++index) {
      grouped_retained_leaf_registers_.emplace_back(retained_registers->value() +
                                                    static_cast<std::uint32_t>(index));
    }
    grouped_sorter_record_first_ = *sorter_record;
    return MapAggregateLeafRegisters(grouped_retained_leaves_, grouped_retained_leaf_registers_,
                                     grouped_sorted_register_map_);
  }

  void BuildGroupedKeyRegisterMap() {
    grouped_key_expression_registers_.assign(expressions_.size(), std::nullopt);
    for (std::size_t term_index = 0; term_index < aggregate_program_->group_by.size();
         ++term_index) {
      const BoundGroupingTerm& term = aggregate_program_->group_by[term_index];
      if (!term.result_column.has_value() ||
          *term.result_column >= aggregate_core_->result_columns.size()) {
        continue;
      }
      const BoundExpressionId result =
          aggregate_core_->result_columns[*term.result_column].expression;
      if (grouped_key_expression_registers_[result.value()].has_value() ||
          ExpressionIsDirectGroupedRepresentative(result)) {
        continue;
      }
      grouped_key_expression_registers_[result.value()] =
          RegisterId{AssumeValue(grouped_previous_group_first_).value() +
                     static_cast<std::uint32_t>(term_index)};
    }
  }

  [[nodiscard]] LoweringResult<void> BuildGroupedAggregateRegisterLayout() {
    if (aggregate_program_ == nullptr || aggregate_ == nullptr) {
      return std::unexpected(InternalFailure("grouped aggregate metadata is unavailable"));
    }
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }

    if (auto invocations = AllocateAggregateInvocationRegisters(); !invocations.has_value()) {
      return invocations;
    }
    if (auto output = AllocateGroupedOutputRegisters(); !output.has_value()) {
      return output;
    }
    if (auto limit = AllocateGroupedLimitRegisters(); !limit.has_value()) {
      return limit;
    }
    if (auto state = AllocateGroupedStateRegisters(); !state.has_value()) {
      return state;
    }
    if (auto representative = AllocateGroupedRepresentativeRegisters();
        !representative.has_value()) {
      return representative;
    }
    if (auto modifiers = BuildAggregateModifierRegisterLayout(); !modifiers.has_value()) {
      return modifiers;
    }
    if (auto sorted = AllocateGroupedSortedRegisters(); !sorted.has_value()) {
      return sorted;
    }
    BuildGroupedKeyRegisterMap();
    if (auto access = AllocateAggregateAccessRegisters(false); !access.has_value()) {
      return access;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> BuildReadRegisterLayout() {
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }

    auto result_block = AllocateRegisters(bound_select_->result_columns().size());
    if (!result_block.has_value()) {
      return std::unexpected(std::move(result_block.error()));
    }
    result_block_first_ = *result_block;

    if (sort_ != nullptr) {
      const std::size_t field_count = sort_->terms.size() + sort_->payload_expressions.size();
      auto sorter_record = AllocateRegisters(field_count);
      if (!sorter_record.has_value()) {
        return std::unexpected(std::move(sorter_record.error()));
      }
      sorter_record_first_ = *sorter_record;
    }

    if (limit_ != nullptr) {
      auto limit_register = AllocateRegisters(1);
      if (!limit_register.has_value()) {
        return std::unexpected(std::move(limit_register.error()));
      }
      limit_register_ = *limit_register;

      if (limit_->offset.has_value()) {
        auto offset_register = AllocateRegisters(1);
        auto zero_register = AllocateRegisters(1);
        auto one_register = AllocateRegisters(1);
        auto comparison_register = AllocateRegisters(1);
        if (!offset_register.has_value()) {
          return std::unexpected(std::move(offset_register.error()));
        }
        if (!zero_register.has_value()) {
          return std::unexpected(std::move(zero_register.error()));
        }
        if (!one_register.has_value()) {
          return std::unexpected(std::move(one_register.error()));
        }
        if (!comparison_register.has_value()) {
          return std::unexpected(std::move(comparison_register.error()));
        }
        offset_register_ = *offset_register;
        zero_register_ = *zero_register;
        one_register_ = *one_register;
        comparison_register_ = *comparison_register;
      }

      if (IsRowLoopAccess() || sort_ != nullptr) {
        if (!zero_register_.has_value()) {
          auto zero_register = AllocateRegisters(1);
          if (!zero_register.has_value()) {
            return std::unexpected(std::move(zero_register.error()));
          }
          zero_register_ = *zero_register;
        }
        if (!one_register_.has_value()) {
          auto one_register = AllocateRegisters(1);
          if (!one_register.has_value()) {
            return std::unexpected(std::move(one_register.error()));
          }
          one_register_ = *one_register;
        }
        auto negative_register = AllocateRegisters(1);
        if (!negative_register.has_value()) {
          return std::unexpected(std::move(negative_register.error()));
        }
        negative_limit_register_ = *negative_register;
      }

      if (sort_ != nullptr && sort_->strategy == PhysicalSortStrategy::kRuntimeLimit) {
        auto top_n_bound = AllocateRegisters(1);
        if (!top_n_bound.has_value()) {
          return std::unexpected(std::move(top_n_bound.error()));
        }
        top_n_bound_register_ = *top_n_bound;
        if (limit_->offset.has_value()) {
          auto maximum_integer = AllocateRegisters(1);
          auto bound_room = AllocateRegisters(1);
          if (!maximum_integer.has_value()) {
            return std::unexpected(std::move(maximum_integer.error()));
          }
          if (!bound_room.has_value()) {
            return std::unexpected(std::move(bound_room.error()));
          }
          maximum_integer_register_ = *maximum_integer;
          bound_room_register_ = *bound_room;
        }
      }
    }

    if (index_scan_ != nullptr &&
        (!index_scan_->equalities.empty() || index_scan_->range.has_value())) {
      const std::size_t key_count =
          index_scan_->equalities.size() + static_cast<std::size_t>(index_scan_->range.has_value());
      auto key = AllocateRegisters(key_count);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      index_key_first_ = *key;
      index_key_capacity_ = static_cast<std::uint32_t>(key_count);
    }
    if (index_scan_ != nullptr && !index_scan_->covering) {
      auto rowid = AllocateRegisters(1);
      if (!rowid.has_value()) {
        return std::unexpected(std::move(rowid.error()));
      }
      index_rowid_register_ = *rowid;
    }

    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> BuildDistinctRegisterLayout() {
    if (distinct_ == nullptr || projection_ == nullptr) {
      return std::unexpected(InternalFailure("DISTINCT register layout has no plan metadata"));
    }
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }

    auto result_block = AllocateRegisters(bound_select_->result_columns().size());
    if (!result_block.has_value()) {
      return std::unexpected(std::move(result_block.error()));
    }
    result_block_first_ = *result_block;

    if (advanced_order_ != nullptr) {
      const LogicalCoreOrderLayout& layout = advanced_order_->core_plans[0].layout;
      const std::size_t field_count = layout.key_values.size() + layout.payload_values.size();
      auto sorter_record = AllocateRegisters(field_count);
      if (!sorter_record.has_value()) {
        return std::unexpected(std::move(sorter_record.error()));
      }
      sorter_record_first_ = *sorter_record;
    }

    if (limit_ != nullptr) {
      auto limit_register = AllocateRegisters(1);
      if (!limit_register.has_value()) {
        return std::unexpected(std::move(limit_register.error()));
      }
      limit_register_ = *limit_register;

      if (limit_->offset.has_value()) {
        auto offset_register = AllocateRegisters(1);
        auto zero_register = AllocateRegisters(1);
        auto one_register = AllocateRegisters(1);
        auto comparison_register = AllocateRegisters(1);
        if (!offset_register.has_value()) {
          return std::unexpected(std::move(offset_register.error()));
        }
        if (!zero_register.has_value()) {
          return std::unexpected(std::move(zero_register.error()));
        }
        if (!one_register.has_value()) {
          return std::unexpected(std::move(one_register.error()));
        }
        if (!comparison_register.has_value()) {
          return std::unexpected(std::move(comparison_register.error()));
        }
        offset_register_ = *offset_register;
        zero_register_ = *zero_register;
        one_register_ = *one_register;
        comparison_register_ = *comparison_register;
      }

      if (table_scan_ != nullptr || advanced_order_ != nullptr) {
        if (!zero_register_.has_value()) {
          auto zero_register = AllocateRegisters(1);
          if (!zero_register.has_value()) {
            return std::unexpected(std::move(zero_register.error()));
          }
          zero_register_ = *zero_register;
        }
        if (!one_register_.has_value()) {
          auto one_register = AllocateRegisters(1);
          if (!one_register.has_value()) {
            return std::unexpected(std::move(one_register.error()));
          }
          one_register_ = *one_register;
        }
        auto negative_register = AllocateRegisters(1);
        if (!negative_register.has_value()) {
          return std::unexpected(std::move(negative_register.error()));
        }
        negative_limit_register_ = *negative_register;
      }
    }

    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> BuildStreamingRegisterLayout() {
    if ((!streaming_query_ && !set_query_) || streaming_cores_.empty()) {
      return std::unexpected(InternalFailure("streaming register layout has no cores"));
    }
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }
    auto result_block = AllocateRegisters(bound_select_->result_columns().size());
    if (!result_block.has_value()) {
      return std::unexpected(std::move(result_block.error()));
    }
    result_block_first_ = *result_block;

    for (StreamingCorePlan& core : streaming_cores_) {
      for (std::size_t index = 0; index < core.joined_sources.size(); ++index) {
        JoinedSourcePlan& source = core.joined_sources[index];
        if (source.access == nullptr) {
          return std::unexpected(InternalFailure("joined source has no register-layout access"));
        }
        if (const auto* scan = std::get_if<PhysicalIndexScanNode>(&source.access->payload);
            scan != nullptr) {
          if (scan->equalities.empty() || scan->range.has_value() ||
              scan->direction != PhysicalScanDirection::kForward) {
            return std::unexpected(InternalFailure("joined index prefix metadata is invalid"));
          }
          auto keys = AllocateRegisters(scan->equalities.size());
          if (!keys.has_value()) {
            return std::unexpected(std::move(keys.error()));
          }
          source.index_key_first = *keys;
          if (!scan->covering && !index_rowid_register_.has_value()) {
            auto rowid = AllocateRegisters(1);
            if (!rowid.has_value()) {
              return std::unexpected(std::move(rowid.error()));
            }
            index_rowid_register_ = *rowid;
          }
        }
        if (source.join != nullptr && SourceJoinPreservesLeft(source.join->join)) {
          auto matched = AllocateRegisters(1);
          if (!matched.has_value()) {
            return std::unexpected(std::move(matched.error()));
          }
          source.matched = *matched;
        }
        if (source.join != nullptr && SourceJoinPreservesRight(source.join->join)) {
          const auto& select = std::get<BoundSelectCore>(*core.bound);
          const BoundTableSource& bound_source = select.table_sources[index];
          std::size_t key_count = 1;
          if (bound_source.table.has_value()) {
            const CatalogSnapshot& catalog = *bound_select_->catalog();
            if (catalog.table(*bound_source.table).without_rowid) {
              const auto primary = catalog.primary_key_index(*bound_source.table);
              if (!primary.has_value()) {
                return std::unexpected(InternalFailure("RIGHT source has no primary key"));
              }
              key_count = catalog.index(*primary).key_term_count;
            }
          }
          if (key_count == 0U || key_count > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(InternalFailure("RIGHT source identity width is invalid"));
          }
          auto mode = AllocateRegisters(1);
          auto keys = AllocateRegisters(key_count);
          if (!mode.has_value()) {
            return std::unexpected(std::move(mode.error()));
          }
          if (!keys.has_value()) {
            return std::unexpected(std::move(keys.error()));
          }
          source.replay_mode = *mode;
          source.right_key_first = *keys;
          source.right_key_count = static_cast<std::uint32_t>(key_count);
        }
      }
    }

    if (limit_ != nullptr) {
      auto limit_register = AllocateRegisters(1);
      auto zero_register = AllocateRegisters(1);
      auto one_register = AllocateRegisters(1);
      auto negative_register = AllocateRegisters(1);
      if (!limit_register.has_value()) {
        return std::unexpected(std::move(limit_register.error()));
      }
      if (!zero_register.has_value()) {
        return std::unexpected(std::move(zero_register.error()));
      }
      if (!one_register.has_value()) {
        return std::unexpected(std::move(one_register.error()));
      }
      if (!negative_register.has_value()) {
        return std::unexpected(std::move(negative_register.error()));
      }
      limit_register_ = *limit_register;
      zero_register_ = *zero_register;
      one_register_ = *one_register;
      negative_limit_register_ = *negative_register;
      if (limit_->offset.has_value()) {
        auto offset_register = AllocateRegisters(1);
        auto comparison_register = AllocateRegisters(1);
        if (!offset_register.has_value()) {
          return std::unexpected(std::move(offset_register.error()));
        }
        if (!comparison_register.has_value()) {
          return std::unexpected(std::move(comparison_register.error()));
        }
        offset_register_ = *offset_register;
        comparison_register_ = *comparison_register;
      }
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> BuildSetRegisterLayout() {
    if (!set_query_) {
      return std::unexpected(InternalFailure("set register layout has no set query"));
    }
    if (auto streaming = BuildStreamingRegisterLayout(); !streaming.has_value()) {
      return streaming;
    }
    if (!zero_register_.has_value()) {
      auto zero = AllocateRegisters(1);
      if (!zero.has_value()) {
        return std::unexpected(std::move(zero.error()));
      }
      zero_register_ = *zero;
    }
    if (!one_register_.has_value()) {
      auto one = AllocateRegisters(1);
      if (!one.has_value()) {
        return std::unexpected(std::move(one.error()));
      }
      one_register_ = *one;
    }
    for (std::optional<RegisterId>& count : set_count_registers_) {
      auto allocated = AllocateRegisters(1);
      if (!allocated.has_value()) {
        return std::unexpected(std::move(allocated.error()));
      }
      count = *allocated;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] std::size_t OrderedRecordFieldCount() const noexcept {
    if (advanced_order_ == nullptr || advanced_order_->core_plans.empty()) {
      return 0;
    }
    const LogicalCoreOrderLayout& layout = advanced_order_->core_plans.front().layout;
    return layout.key_values.size() + layout.payload_values.size();
  }

  [[nodiscard]] bool OrderedCoreNeedsFullKey(std::size_t core_index) const noexcept {
    for (std::size_t operation = 0; operation < bound_select_->compound_operators().size();
         ++operation) {
      if (bound_select_->compound_operators()[operation] != CompoundOperator::kUnionAll &&
          core_index <= operation + 1U) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool OrderedUsesRuntimeLimit() const noexcept {
    if (advanced_order_ == nullptr) {
      return false;
    }
    return advanced_order_->final_strategy == PhysicalSortStrategy::kRuntimeLimit ||
           std::ranges::any_of(advanced_order_->core_plans, [](const PhysicalCoreOrderPlan& core) {
             return core.strategy == PhysicalSortStrategy::kRuntimeLimit;
           });
  }

  [[nodiscard]] LoweringResult<void> BuildOrderedCompoundRegisterLayout() {
    if (!ordered_compound_ || advanced_order_ == nullptr) {
      return std::unexpected(
          InternalFailure("ordered compound register layout has no ordering metadata"));
    }
    LoweringResult<void> base =
        advanced_order_->set_then_order ? BuildSetRegisterLayout() : BuildStreamingRegisterLayout();
    if (!base.has_value()) {
      return base;
    }
    const std::size_t field_count = OrderedRecordFieldCount();
    if (field_count == 0U) {
      return std::unexpected(InternalFailure("ordered compound record layout is empty"));
    }
    auto left = AllocateRegisters(field_count);
    auto right = AllocateRegisters(field_count);
    auto comparison = AllocateRegisters(1);
    auto predicate = AllocateRegisters(1);
    if (!left.has_value()) {
      return std::unexpected(std::move(left.error()));
    }
    if (!right.has_value()) {
      return std::unexpected(std::move(right.error()));
    }
    if (!comparison.has_value()) {
      return std::unexpected(std::move(comparison.error()));
    }
    if (!predicate.has_value()) {
      return std::unexpected(std::move(predicate.error()));
    }
    ordered_left_first_ = *left;
    ordered_right_first_ = *right;
    ordered_comparison_register_ = *comparison;
    ordered_predicate_register_ = *predicate;

    if (!zero_register_.has_value()) {
      auto zero = AllocateRegisters(1);
      if (!zero.has_value()) {
        return std::unexpected(std::move(zero.error()));
      }
      zero_register_ = *zero;
    }
    if (!one_register_.has_value()) {
      auto one = AllocateRegisters(1);
      if (!one.has_value()) {
        return std::unexpected(std::move(one.error()));
      }
      one_register_ = *one;
    }

    if (OrderedUsesRuntimeLimit()) {
      if (limit_ == nullptr) {
        return std::unexpected(InternalFailure("ordered runtime limit has no LIMIT node"));
      }
      if (!top_n_bound_register_.has_value()) {
        auto bound = AllocateRegisters(1);
        if (!bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        top_n_bound_register_ = *bound;
      }
      if (limit_->offset.has_value()) {
        if (!maximum_integer_register_.has_value()) {
          auto maximum = AllocateRegisters(1);
          if (!maximum.has_value()) {
            return std::unexpected(std::move(maximum.error()));
          }
          maximum_integer_register_ = *maximum;
        }
        if (!bound_room_register_.has_value()) {
          auto room = AllocateRegisters(1);
          if (!room.has_value()) {
            return std::unexpected(std::move(room.error()));
          }
          bound_room_register_ = *room;
        }
        auto overflow = AllocateRegisters(1);
        if (!overflow.has_value()) {
          return std::unexpected(std::move(overflow.error()));
        }
        ordered_overflow_register_ = *overflow;
      }
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> AllocateRegisterPool(std::size_t count,
                                                          std::vector<RegisterId>* output) {
    output->clear();
    if (count == 0U) {
      return {};
    }
    auto first = AllocateRegisters(count);
    if (!first.has_value()) {
      return std::unexpected(std::move(first.error()));
    }
    output->reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      output->emplace_back(first->value() + static_cast<std::uint32_t>(index));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> PrepareStreamingAggregateCoreMetadata(
      StreamingCorePlan* core) {
    if (core == nullptr || core->aggregate == nullptr) {
      return std::unexpected(InternalFailure("streaming aggregate core metadata is unavailable"));
    }
    const auto* select = std::get_if<BoundSelectCore>(core->bound);
    if (select == nullptr || !select->aggregate.has_value()) {
      return std::unexpected(InternalFailure("streaming bound aggregate core is unavailable"));
    }
    SetStreamingAggregateCoreBasics(*core);
    if (select->aggregate->group_by.empty()) {
      core->global_representative_leaves.clear();
      std::vector<BoundSourceLeafIdentity> identities;
      for (const BoundResultColumn& result : select->result_columns) {
        CollectAggregateRepresentativeLeaves(result.expression, &core->global_representative_leaves,
                                             &identities);
      }
      if (select->aggregate->having.has_value()) {
        CollectAggregateRepresentativeLeaves(*select->aggregate->having,
                                             &core->global_representative_leaves, &identities);
      }
      global_representative_query_ = !core->global_representative_leaves.empty();
    } else {
      if (auto retained = BuildGroupedRetainedLeaves(); !retained.has_value()) {
        return retained;
      }
      core->grouped_retained_leaves = grouped_retained_leaves_;
    }
    if (auto modifiers = BuildAggregateModifierRegisterLayout(); !modifiers.has_value()) {
      return modifiers;
    }
    core->global_filtered_controller_defaults = global_filtered_controller_defaults_;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateStreamingAggregateWorkspace() {
    if (!streaming_aggregate_query_) {
      return {};
    }
    if (auto invocations = AllocateQueryAggregateInvocationRegisters(); !invocations.has_value()) {
      return invocations;
    }

    std::size_t maximum_global_representatives = 0;
    std::size_t maximum_group_terms = 0;
    std::size_t maximum_grouped_representatives = 0;
    std::size_t maximum_grouped_retained = 0;
    std::size_t maximum_grouped_sorter_fields = 0;
    std::size_t maximum_index_keys = 0;
    bool has_global_representatives = false;
    bool has_global_controller = false;
    bool has_grouped_controller = false;
    bool has_noncovering_index = false;

    for (StreamingCorePlan& core : streaming_cores_) {
      if (core.aggregate == nullptr) {
        continue;
      }
      if (auto prepared = PrepareStreamingAggregateCoreMetadata(&core); !prepared.has_value()) {
        return prepared;
      }
      const auto& select = std::get<BoundSelectCore>(*core.bound);
      const BoundAggregateProgram& program = AssumeValue(select.aggregate);
      if (program.group_by.empty()) {
        maximum_global_representatives =
            std::max(maximum_global_representatives, core.global_representative_leaves.size());
        has_global_representatives =
            has_global_representatives || !core.global_representative_leaves.empty();
        has_global_controller =
            has_global_controller ||
            (!core.global_representative_leaves.empty() && HasAggregateRepresentativeController());
      } else {
        maximum_group_terms = std::max(maximum_group_terms, program.group_by.size());
        maximum_grouped_representatives =
            std::max(maximum_grouped_representatives, program.representative_leaves.size());
        maximum_grouped_retained =
            std::max(maximum_grouped_retained, core.grouped_retained_leaves.size());
        if (core.aggregate->strategy == PhysicalAggregateStrategy::kSortedGroup) {
          maximum_grouped_sorter_fields =
              std::max(maximum_grouped_sorter_fields,
                       program.group_by.size() + core.grouped_retained_leaves.size());
        }
        has_grouped_controller =
            has_grouped_controller ||
            (!program.representative_leaves.empty() && HasAggregateRepresentativeController());
      }
      if (core.index_scan != nullptr) {
        std::size_t key_count = core.index_scan->equalities.size() +
                                static_cast<std::size_t>(core.index_scan->range.has_value());
        if (core.aggregate->strategy == PhysicalAggregateStrategy::kIndexedMinMax &&
            core.aggregate->min_max.has_value() &&
            AssumeValue(core.aggregate->min_max).null_strategy ==
                PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback) {
          key_count = std::max(key_count, core.index_scan->equalities.size() + 1U);
        }
        maximum_index_keys = std::max(maximum_index_keys, key_count);
        has_noncovering_index = has_noncovering_index || !core.index_scan->covering;
      }
    }

    if (auto allocated = AllocateRegisterPool(maximum_global_representatives,
                                              &streaming_global_representative_registers_);
        !allocated.has_value()) {
      return allocated;
    }
    if (has_global_representatives) {
      auto has_row = AllocateRegisters(1);
      if (!has_row.has_value()) {
        return std::unexpected(std::move(has_row.error()));
      }
      global_has_representative_row_register_ = *has_row;
    }
    if (has_global_controller) {
      auto comparison = AllocateRegisters(1);
      auto effect = AllocateRegisters(1);
      auto replacement = AllocateRegisters(1);
      if (!comparison.has_value()) {
        return std::unexpected(std::move(comparison.error()));
      }
      if (!effect.has_value()) {
        return std::unexpected(std::move(effect.error()));
      }
      if (!replacement.has_value()) {
        return std::unexpected(std::move(replacement.error()));
      }
      global_representative_comparison_register_ = *comparison;
      global_representative_effect_register_ = *effect;
      global_representative_replace_register_ = *replacement;
    }

    if (maximum_group_terms != 0U) {
      auto current = AllocateRegisters(maximum_group_terms);
      auto previous = AllocateRegisters(maximum_group_terms);
      auto has_group = AllocateRegisters(1);
      auto comparison = AllocateRegisters(1);
      if (!current.has_value()) {
        return std::unexpected(std::move(current.error()));
      }
      if (!previous.has_value()) {
        return std::unexpected(std::move(previous.error()));
      }
      if (!has_group.has_value()) {
        return std::unexpected(std::move(has_group.error()));
      }
      if (!comparison.has_value()) {
        return std::unexpected(std::move(comparison.error()));
      }
      grouped_current_group_first_ = *current;
      grouped_previous_group_first_ = *previous;
      grouped_has_group_register_ = *has_group;
      grouped_comparison_register_ = *comparison;
    }
    if (auto allocated = AllocateRegisterPool(maximum_grouped_representatives,
                                              &streaming_grouped_representative_registers_);
        !allocated.has_value()) {
      return allocated;
    }
    if (has_grouped_controller) {
      auto effect = AllocateRegisters(1);
      auto replacement = AllocateRegisters(1);
      if (!effect.has_value()) {
        return std::unexpected(std::move(effect.error()));
      }
      if (!replacement.has_value()) {
        return std::unexpected(std::move(replacement.error()));
      }
      grouped_representative_effect_register_ = *effect;
      grouped_representative_replace_register_ = *replacement;
    }
    if (auto allocated =
            AllocateRegisterPool(maximum_grouped_retained, &streaming_grouped_retained_registers_);
        !allocated.has_value()) {
      return allocated;
    }
    if (maximum_grouped_sorter_fields != 0U) {
      auto record = AllocateRegisters(maximum_grouped_sorter_fields);
      if (!record.has_value()) {
        return std::unexpected(std::move(record.error()));
      }
      grouped_sorter_record_first_ = *record;
    }
    if (maximum_index_keys != 0U) {
      auto keys = AllocateRegisters(maximum_index_keys);
      if (!keys.has_value()) {
        return std::unexpected(std::move(keys.error()));
      }
      index_key_first_ = *keys;
      index_key_capacity_ = static_cast<std::uint32_t>(maximum_index_keys);
    }
    if (has_noncovering_index && !index_rowid_register_.has_value()) {
      auto rowid = AllocateRegisters(1);
      if (!rowid.has_value()) {
        return std::unexpected(std::move(rowid.error()));
      }
      index_rowid_register_ = *rowid;
    }
    ClearStreamingAggregateContext();
    return {};
  }

  [[nodiscard]] LoweringResult<void> BuildStreamingAggregateRegisterLayout() {
    LoweringResult<void> base = ordered_compound_ ? BuildOrderedCompoundRegisterLayout()
                                : set_query_      ? BuildSetRegisterLayout()
                                                  : BuildStreamingRegisterLayout();
    if (!base.has_value()) {
      return base;
    }
    if (auto aggregate = AllocateStreamingAggregateWorkspace(); !aggregate.has_value()) {
      return aggregate;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> BuildInsertRegisterLayout() {
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }
    const BoundMutationTarget& target = bound_insert_->target();
    auto values = AllocateRegisters(target.columns.size());
    if (!values.has_value()) {
      return std::unexpected(std::move(values.error()));
    }
    insert_values_first_ = *values;
    auto rowid = AllocateRegisters(1);
    if (!rowid.has_value()) {
      return std::unexpected(std::move(rowid.error()));
    }
    insert_rowid_register_ = *rowid;
    auto record = AllocateRegisters(1);
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    insert_record_register_ = *record;
    if (auto index_key = AllocateMutationIndexKeyRegisters(target); !index_key.has_value()) {
      return index_key;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> AllocateMutationIndexKeyRegisters(
      const BoundMutationTarget& target) {
    std::size_t maximum_index_terms = 0;
    for (const BoundIndexMaintenance& index : target.indexes) {
      maximum_index_terms = std::max(maximum_index_terms, index.terms.size());
    }
    if (maximum_index_terms > 0U) {
      auto index_key = AllocateRegisters(maximum_index_terms);
      if (!index_key.has_value()) {
        return std::unexpected(std::move(index_key.error()));
      }
      mutation_index_key_first_ = *index_key;
      mutation_index_key_capacity_ = static_cast<std::uint32_t>(maximum_index_terms);
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateSourceSnapshotRegisters() {
    auto source = AllocateRegisters(source_columns_.size());
    if (!source.has_value()) {
      return std::unexpected(std::move(source.error()));
    }
    source_snapshot_registers_.reserve(source_columns_.size());
    for (std::size_t index = 0; index < source_columns_.size(); ++index) {
      source_snapshot_registers_.emplace_back(source->value() + static_cast<std::uint32_t>(index));
    }
    auto rowid = AllocateRegisters(1);
    if (!rowid.has_value()) {
      return std::unexpected(std::move(rowid.error()));
    }
    source_rowid_register_ = *rowid;
    return {};
  }

  [[nodiscard]] LoweringResult<void> BuildDeleteRegisterLayout(
      const PhysicalDeleteMutation& delete_plan) {
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }
    if (delete_plan.access.kind == MutationAccessKind::kEmpty) {
      return FinishRegisterLayout();
    }
    if (auto snapshot = AllocateSourceSnapshotRegisters(); !snapshot.has_value()) {
      return snapshot;
    }
    if (auto index_key = AllocateMutationIndexKeyRegisters(bound_delete_->target());
        !index_key.has_value()) {
      return index_key;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> BuildUpdateRegisterLayout(
      const PhysicalUpdateMutation& update) {
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }
    if (update.access.kind == MutationAccessKind::kEmpty) {
      return FinishRegisterLayout();
    }
    if (auto snapshot = AllocateSourceSnapshotRegisters(); !snapshot.has_value()) {
      return snapshot;
    }
    auto values = AllocateRegisters(bound_update_->target().columns.size());
    if (!values.has_value()) {
      return std::unexpected(std::move(values.error()));
    }
    update_values_first_ = *values;
    auto new_rowid = AllocateRegisters(1);
    if (!new_rowid.has_value()) {
      return std::unexpected(std::move(new_rowid.error()));
    }
    update_new_rowid_register_ = *new_rowid;
    auto record = AllocateRegisters(1);
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    update_record_register_ = *record;
    if (auto index_key = AllocateMutationIndexKeyRegisters(bound_update_->target());
        !index_key.has_value()) {
      return index_key;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> AddBoundConstants() {
    for (std::size_t index = 0; index < expressions_.size(); ++index) {
      const auto* literal = std::get_if<BoundLiteralExpression>(&expressions_[index].payload);
      if (literal == nullptr) {
        continue;
      }
      auto constant = AddConstant(literal->value.Clone());
      if (!constant.has_value()) {
        return std::unexpected(std::move(constant.error()));
      }
      literal_constants_[index] = *constant;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<SymbolId> AddNamedSymbol(std::string_view name) {
    auto symbol = ConvertProgramResult(AssumeValue(builder_).AddSymbol(std::string{name}),
                                       "unable to add a bytecode symbol");
    if (!symbol.has_value()) {
      return std::unexpected(std::move(symbol.error()));
    }
    symbol_names_.push_back(NamedSymbol{.name = std::string{name}, .id = *symbol});
    return *symbol;
  }

  [[nodiscard]] LoweringResult<void> AddBoundSymbols() {
    collation_symbols_.reserve(collations_.size());
    for (const BoundCollation& collation : collations_) {
      auto symbol = AddNamedSymbol(collation.name);
      if (!symbol.has_value()) {
        return std::unexpected(std::move(symbol.error()));
      }
      collation_symbols_.push_back(*symbol);
    }

    function_symbols_.reserve(functions_.size());
    for (const BoundScalarFunction& function : functions_) {
      auto symbol = AddNamedSymbol(function.name);
      if (!symbol.has_value()) {
        return std::unexpected(std::move(symbol.error()));
      }
      function_symbols_.push_back(*symbol);
    }
    aggregate_function_symbols_.reserve(aggregate_functions_.size());
    for (const BoundAggregateFunction& function : aggregate_functions_) {
      auto symbol = AddNamedSymbol(function.name);
      if (!symbol.has_value()) {
        return std::unexpected(std::move(symbol.error()));
      }
      aggregate_function_symbols_.push_back(*symbol);
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddGlobalAggregateDescriptors() {
    if (aggregate_program_ == nullptr ||
        aggregate_descriptors_.size() != aggregate_invocations_.size()) {
      return std::unexpected(InternalFailure("global aggregate descriptors are unavailable"));
    }
    if (aggregate_ != nullptr &&
        aggregate_->strategy == PhysicalAggregateStrategy::kIndexedMinMax &&
        aggregate_->min_max.has_value() &&
        aggregate_->min_max->null_strategy ==
            PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback &&
        index_scan_ != nullptr) {
      auto constant = AddConstant(SqlValue{});
      if (!constant.has_value()) {
        return std::unexpected(std::move(constant.error()));
      }
      indexed_min_max_null_constant_ = *constant;
    }
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      const BoundAggregateInvocation& invocation = aggregate_invocations_[invocation_id.value()];
      if (invocation.function.value() >= aggregate_function_symbols_.size() ||
          invocation.collation.value() >= collation_symbols_.size() ||
          invocation.arguments.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(InternalFailure("bound aggregate descriptor is invalid"));
      }
      auto descriptor = ConvertProgramResult(
          AssumeValue(builder_).AddAggregate(AggregateDescriptor{
              .function = aggregate_function_symbols_[invocation.function.value()],
              .collation = collation_symbols_[invocation.collation.value()],
              .argument_count = static_cast<std::uint32_t>(invocation.arguments.size()),
          }),
          "unable to add aggregate descriptor");
      if (!descriptor.has_value()) {
        return std::unexpected(std::move(descriptor.error()));
      }
      aggregate_descriptors_[invocation_id.value()] = *descriptor;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<OrderingRecordDescriptor> BuildAggregateDistinctDescriptor(
      const BoundAggregateInvocation& invocation) {
    if (invocation.arguments.size() != 1U ||
        invocation.collation.value() >= collation_symbols_.size()) {
      return std::unexpected(InternalFailure("aggregate DISTINCT descriptor is invalid"));
    }
    OrderingRecordDescriptor descriptor{
        .field_count = 1,
        .key_field_count = 1,
        .key_columns = {},
    };
    descriptor.key_columns.push_back(OrderingColumnMetadata{
        .collation = collation_symbols_[invocation.collation.value()],
        .order = BytecodeSortOrder::kAscending,
        .null_placement = BytecodeNullPlacement::kFirst,
    });
    return descriptor;
  }

  [[nodiscard]] LoweringResult<OrderingRecordDescriptor> BuildAggregateOrderDescriptor(
      const BoundAggregateInvocation& invocation,
      const lowering_internal::AggregateModifierRuntime& runtime) {
    if (runtime.record_field_count == 0U || runtime.order_key_count != invocation.order_by.size()) {
      return std::unexpected(InternalFailure("aggregate ordering descriptor is invalid"));
    }
    OrderingRecordDescriptor descriptor{
        .field_count = runtime.record_field_count,
        .key_field_count = runtime.order_key_count,
        .key_columns = {},
    };
    descriptor.key_columns.reserve(invocation.order_by.size());
    for (const BoundOrderingTerm& term : invocation.order_by) {
      if (term.collation.value() >= collation_symbols_.size()) {
        return std::unexpected(InternalFailure("aggregate order collation is unavailable"));
      }
      descriptor.key_columns.push_back(OrderingColumnMetadata{
          .collation = collation_symbols_[term.collation.value()],
          .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                        : BytecodeSortOrder::kAscending,
          .null_placement = term.null_placement == BoundNullPlacement::kLast
                                ? BytecodeNullPlacement::kLast
                                : BytecodeNullPlacement::kFirst,
      });
    }
    return descriptor;
  }

  [[nodiscard]] LoweringResult<void> AddAggregateModifierDescriptors() {
    if (aggregate_program_ == nullptr ||
        aggregate_modifier_runtimes_.size() != aggregate_invocations_.size()) {
      return std::unexpected(InternalFailure("aggregate modifier descriptors are unavailable"));
    }
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      const BoundAggregateInvocation& invocation = aggregate_invocations_[invocation_id.value()];
      lowering_internal::AggregateModifierRuntime& runtime =
          AssumeValue(aggregate_modifier_runtimes_[invocation_id.value()]);
      if (runtime.strategy == lowering_internal::AggregateModifierStrategy::Distinct ||
          runtime.strategy == lowering_internal::AggregateModifierStrategy::DistinctOrdered) {
        auto descriptor = BuildAggregateDistinctDescriptor(invocation);
        if (!descriptor.has_value()) {
          return std::unexpected(std::move(descriptor.error()));
        }
        auto relation =
            ConvertProgramResult(AssumeValue(builder_).AddRelation(std::move(*descriptor)),
                                 "unable to add aggregate DISTINCT relation descriptor");
        if (!relation.has_value()) {
          return std::unexpected(std::move(relation.error()));
        }
        runtime.relation = *relation;
      } else if (runtime.strategy == lowering_internal::AggregateModifierStrategy::UniqueOrdered) {
        if (invocation.order_by.size() != 1U ||
            invocation.order_by.front().collation != invocation.collation) {
          return std::unexpected(
              InternalFailure("unique ordered aggregate collation is inconsistent"));
        }
        auto descriptor = BuildAggregateOrderDescriptor(invocation, runtime);
        if (!descriptor.has_value()) {
          return std::unexpected(std::move(descriptor.error()));
        }
        auto relation =
            ConvertProgramResult(AssumeValue(builder_).AddRelation(std::move(*descriptor)),
                                 "unable to add unique ordered aggregate relation descriptor");
        if (!relation.has_value()) {
          return std::unexpected(std::move(relation.error()));
        }
        runtime.relation = *relation;
      }

      if (runtime.strategy == lowering_internal::AggregateModifierStrategy::Ordered ||
          runtime.strategy == lowering_internal::AggregateModifierStrategy::DistinctOrdered) {
        auto descriptor = BuildAggregateOrderDescriptor(invocation, runtime);
        if (!descriptor.has_value()) {
          return std::unexpected(std::move(descriptor.error()));
        }
        auto sorter = ConvertProgramResult(AssumeValue(builder_).AddSorter(std::move(*descriptor)),
                                           "unable to add aggregate ordering sorter descriptor");
        if (!sorter.has_value()) {
          return std::unexpected(std::move(sorter.error()));
        }
        runtime.sorter = *sorter;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddGroupedAggregateDescriptors() {
    if (aggregate_program_ == nullptr || aggregate_ == nullptr) {
      return std::unexpected(InternalFailure("grouped aggregate descriptors are unavailable"));
    }
    if (aggregate_->strategy == PhysicalAggregateStrategy::kSortedGroup) {
      const std::size_t field_count =
          aggregate_program_->group_by.size() + grouped_retained_leaves_.size();
      if (field_count > std::numeric_limits<std::uint32_t>::max() ||
          aggregate_program_->group_by.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(
            ProgramFailure(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded},
                           "group sorter descriptor exceeds bytecode field identities"));
      }
      OrderingRecordDescriptor descriptor{
          .field_count = static_cast<std::uint32_t>(field_count),
          .key_field_count = static_cast<std::uint32_t>(aggregate_program_->group_by.size()),
          .key_columns = {},
      };
      descriptor.key_columns.reserve(aggregate_program_->group_by.size());
      for (std::size_t index = 0; index < aggregate_program_->group_by.size(); ++index) {
        const BoundGroupingTerm& term = aggregate_program_->group_by[index];
        if (term.collation.value() >= collation_symbols_.size()) {
          return std::unexpected(InternalFailure("group sorter collation is unavailable"));
        }
        const auto delivered = std::ranges::find_if(
            aggregate_->grouping_order, [index](const PhysicalGroupingOrderTerm& candidate) {
              return candidate.group_term_index == index;
            });
        if (delivered == aggregate_->grouping_order.end()) {
          return std::unexpected(InternalFailure("group sorter delivery metadata is incomplete"));
        }
        descriptor.key_columns.push_back(OrderingColumnMetadata{
            .collation = collation_symbols_[term.collation.value()],
            .order = delivered->order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                                : BytecodeSortOrder::kAscending,
            .null_placement = delivered->null_placement == BoundNullPlacement::kLast
                                  ? BytecodeNullPlacement::kLast
                                  : BytecodeNullPlacement::kFirst,
        });
      }
      auto sorter = ConvertProgramResult(AssumeValue(builder_).AddSorter(std::move(descriptor)),
                                         "unable to add grouped aggregate sorter descriptor");
      if (!sorter.has_value()) {
        return std::unexpected(std::move(sorter.error()));
      }
      grouped_sorter_ = *sorter;
    }
    if (HasGroupedOuterOrder()) {
      return AddOrderingDescriptors();
    }
    return {};
  }

  [[nodiscard]] LoweringResult<OrderingRecordDescriptor> BuildOrderingDescriptor() {
    if (sort_ == nullptr) {
      return std::unexpected(InternalFailure("ordering descriptor has no physical sort"));
    }
    const std::size_t field_count = sort_->terms.size() + sort_->payload_expressions.size();
    if (field_count > std::numeric_limits<std::uint32_t>::max() ||
        sort_->terms.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded},
                         "sorter descriptor exceeds bytecode field identities"));
    }

    OrderingRecordDescriptor descriptor{
        .field_count = static_cast<std::uint32_t>(field_count),
        .key_field_count = static_cast<std::uint32_t>(sort_->terms.size()),
        .key_columns = {},
    };
    descriptor.key_columns.reserve(sort_->terms.size());
    for (const BoundOrderingTerm& term : sort_->terms) {
      if (term.collation.value() >= collation_symbols_.size()) {
        return std::unexpected(InternalFailure("sorter term collation is not published"));
      }
      descriptor.key_columns.push_back(OrderingColumnMetadata{
          .collation = collation_symbols_[term.collation.value()],
          .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                        : BytecodeSortOrder::kAscending,
          .null_placement = term.null_placement == BoundNullPlacement::kLast
                                ? BytecodeNullPlacement::kLast
                                : BytecodeNullPlacement::kFirst,
      });
    }
    return descriptor;
  }

  [[nodiscard]] LoweringResult<void> AddOrderingDescriptors() {
    if (sort_ == nullptr) {
      return {};
    }
    auto sorter_descriptor = BuildOrderingDescriptor();
    if (!sorter_descriptor.has_value()) {
      return std::unexpected(std::move(sorter_descriptor.error()));
    }
    auto added_sorter =
        ConvertProgramResult(AssumeValue(builder_).AddSorter(std::move(*sorter_descriptor)),
                             "unable to add sorter descriptor");
    if (!added_sorter.has_value()) {
      return std::unexpected(std::move(added_sorter.error()));
    }
    sorter_ = *added_sorter;
    if (sort_->strategy == PhysicalSortStrategy::kRuntimeLimit) {
      auto top_n_descriptor = BuildOrderingDescriptor();
      if (!top_n_descriptor.has_value()) {
        return std::unexpected(std::move(top_n_descriptor.error()));
      }
      auto added_top_n =
          ConvertProgramResult(AssumeValue(builder_).AddTopN(std::move(*top_n_descriptor)),
                               "unable to add top-N descriptor");
      if (!added_top_n.has_value()) {
        return std::unexpected(std::move(added_top_n.error()));
      }
      top_n_ = *added_top_n;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddDistinctDescriptors() {
    if (distinct_ == nullptr ||
        distinct_->collations.size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("DISTINCT descriptor metadata is invalid"));
    }
    if (bound_select_->result_columns().size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded},
                         "DISTINCT descriptor exceeds bytecode field identities"));
    }

    OrderingRecordDescriptor relation_descriptor{
        .field_count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
        .key_field_count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
        .key_columns = {},
    };
    relation_descriptor.key_columns.reserve(distinct_->collations.size());
    for (const BoundCollationId collation : distinct_->collations) {
      if (collation.value() >= collation_symbols_.size()) {
        return std::unexpected(InternalFailure("DISTINCT collation is not published"));
      }
      relation_descriptor.key_columns.push_back(OrderingColumnMetadata{
          .collation = collation_symbols_[collation.value()],
          .order = BytecodeSortOrder::kAscending,
          .null_placement = BytecodeNullPlacement::kFirst,
      });
    }
    auto relation =
        ConvertProgramResult(AssumeValue(builder_).AddRelation(std::move(relation_descriptor)),
                             "unable to add DISTINCT relation descriptor");
    if (!relation.has_value()) {
      return std::unexpected(std::move(relation.error()));
    }
    distinct_relation_ = *relation;

    if (advanced_order_ == nullptr) {
      return {};
    }
    const LogicalCoreOrderLayout& layout = advanced_order_->core_plans[0].layout;
    const std::size_t field_count = layout.key_values.size() + layout.payload_values.size();
    if (field_count > std::numeric_limits<std::uint32_t>::max() ||
        advanced_order_->terms.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded},
                         "DISTINCT ordering descriptor exceeds bytecode field identities"));
    }
    OrderingRecordDescriptor sorter_descriptor{
        .field_count = static_cast<std::uint32_t>(field_count),
        .key_field_count = static_cast<std::uint32_t>(advanced_order_->terms.size()),
        .key_columns = {},
    };
    sorter_descriptor.key_columns.reserve(advanced_order_->terms.size());
    for (const BoundOrderingTerm& term : advanced_order_->terms) {
      if (term.collation.value() >= collation_symbols_.size()) {
        return std::unexpected(InternalFailure("DISTINCT order collation is not published"));
      }
      sorter_descriptor.key_columns.push_back(OrderingColumnMetadata{
          .collation = collation_symbols_[term.collation.value()],
          .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                        : BytecodeSortOrder::kAscending,
          .null_placement = term.null_placement == BoundNullPlacement::kLast
                                ? BytecodeNullPlacement::kLast
                                : BytecodeNullPlacement::kFirst,
      });
    }
    auto sorter =
        ConvertProgramResult(AssumeValue(builder_).AddSorter(std::move(sorter_descriptor)),
                             "unable to add DISTINCT ordering descriptor");
    if (!sorter.has_value()) {
      return std::unexpected(std::move(sorter.error()));
    }
    sorter_ = *sorter;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddStreamingCursorDescriptor(StreamingCorePlan& core,
                                                                  const BoundSelectCore& select) {
    if (core.table_scan == nullptr || select.table_sources.size() != 1U ||
        core.table_scan->source != select.table_sources.front().id) {
      return std::unexpected(InternalFailure("streaming cursor metadata is invalid"));
    }
    SetStreamingCoreContext(core);
    if (auto added = AddCursorDescriptor(); !added.has_value()) {
      return added;
    }
    core.cursor = cursor_;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddRightJoinDescriptor(JoinedSourcePlan& source,
                                                            const BoundTableSource& bound_source) {
    if (!source.replay_mode.has_value()) {
      return {};
    }
    OrderingRecordDescriptor descriptor{
        .field_count = source.right_key_count,
        .key_field_count = source.right_key_count,
        .key_columns = {},
    };
    descriptor.key_columns.reserve(source.right_key_count);
    const CatalogSnapshot& catalog = *bound_select_->catalog();
    const CatalogIndex* primary = nullptr;
    if (bound_source.table.has_value() && catalog.table(*bound_source.table).without_rowid) {
      const auto id = catalog.primary_key_index(*bound_source.table);
      if (!id.has_value()) {
        return std::unexpected(InternalFailure("RIGHT source has no primary key descriptor"));
      }
      primary = &catalog.index(*id);
    }
    for (std::uint32_t index = 0; index < source.right_key_count; ++index) {
      const std::string_view name =
          primary == nullptr ? std::string_view{"BINARY"} : primary->terms[index].collation_name;
      auto collation = SymbolForName(name);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      descriptor.key_columns.push_back(OrderingColumnMetadata{
          .collation = *collation,
          .order = primary != nullptr && primary->terms[index].order == SortOrder::kDescending
                       ? BytecodeSortOrder::kDescending
                       : BytecodeSortOrder::kAscending,
          .null_placement = BytecodeNullPlacement::kFirst,
      });
    }
    auto relation = ConvertProgramResult(AssumeValue(builder_).AddRelation(std::move(descriptor)),
                                         "unable to add RIGHT matched-key relation");
    if (!relation.has_value()) {
      return std::unexpected(std::move(relation.error()));
    }
    source.right_matches = *relation;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddStreamingDescriptors() {
    if (source_cursor_fields_.size() != bound_select_->source_columns().size()) {
      source_cursor_fields_.assign(bound_select_->source_columns().size(), std::nullopt);
    }
    if (source_field_real_affinity_.size() != bound_select_->source_columns().size()) {
      source_field_real_affinity_.assign(bound_select_->source_columns().size(), false);
    }
    for (StreamingCorePlan& core : streaming_cores_) {
      if (const auto* select = std::get_if<BoundSelectCore>(core.bound); select != nullptr) {
        if (core.join != nullptr) {
          for (std::size_t index = 0; index < core.joined_sources.size(); ++index) {
            JoinedSourcePlan& joined = core.joined_sources[index];
            const BoundTableSource& source = select->table_sources[index];
            if (joined.access == nullptr) {
              return std::unexpected(InternalFailure("joined source has no access leaf"));
            }
            table_scan_ = std::get_if<PhysicalTableScanNode>(&joined.access->payload);
            rowid_lookup_ = std::get_if<PhysicalRowIdLookupNode>(&joined.access->payload);
            index_scan_ = std::get_if<PhysicalIndexScanNode>(&joined.access->payload);
            leaf_ = joined.access;
            active_source_column_begin_ = source.source_column_begin;
            active_source_column_count_ = source.source_column_count;
            if (auto added = AddCursorDescriptor(); !added.has_value()) {
              return added;
            }
            joined.cursor = index_scan_ != nullptr ? index_cursor_ : cursor_;
            if (index_scan_ != nullptr && !index_scan_->covering) {
              joined.table_cursor = cursor_;
            }
            if (auto added = AddRightJoinDescriptor(joined, source); !added.has_value()) {
              return added;
            }
          }
        }
        if (core.table_scan != nullptr && core.aggregate == nullptr) {
          if (auto cursor = AddStreamingCursorDescriptor(core, *select); !cursor.has_value()) {
            return cursor;
          }
        }
        if (core.distinct != nullptr) {
          if (core.distinct->collations.size() != select->result_columns.size() ||
              select->result_columns.size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(
                InternalFailure("streaming DISTINCT descriptor shape is invalid"));
          }
          OrderingRecordDescriptor descriptor{
              .field_count = static_cast<std::uint32_t>(select->result_columns.size()),
              .key_field_count = static_cast<std::uint32_t>(select->result_columns.size()),
              .key_columns = {},
          };
          descriptor.key_columns.reserve(core.distinct->collations.size());
          for (const BoundCollationId collation : core.distinct->collations) {
            if (collation.value() >= collation_symbols_.size()) {
              return std::unexpected(
                  InternalFailure("streaming DISTINCT collation is not published"));
            }
            descriptor.key_columns.push_back(OrderingColumnMetadata{
                .collation = collation_symbols_[collation.value()],
                .order = BytecodeSortOrder::kAscending,
                .null_placement = BytecodeNullPlacement::kFirst,
            });
          }
          auto relation =
              ConvertProgramResult(AssumeValue(builder_).AddRelation(std::move(descriptor)),
                                   "unable to add streaming DISTINCT relation");
          if (!relation.has_value()) {
            return std::unexpected(std::move(relation.error()));
          }
          core.relation = *relation;
        }
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddStreamingAggregateDescriptors() {
    if (!streaming_aggregate_query_) {
      return {};
    }
    source_cursor_fields_.assign(bound_select_->source_columns().size(), std::nullopt);
    source_field_real_affinity_.assign(bound_select_->source_columns().size(), false);
    for (StreamingCorePlan& core : streaming_cores_) {
      const auto* select = std::get_if<BoundSelectCore>(core.bound);
      if (select == nullptr || !select->aggregate.has_value()) {
        continue;
      }
      if (auto context = SetStreamingAggregateCoreContext(core); !context.has_value()) {
        return context;
      }
      if (IsCursorAccess()) {
        if (auto cursor = AddCursorDescriptor(); !cursor.has_value()) {
          return cursor;
        }
        core.cursor = cursor_;
        core.index_cursor = index_cursor_;
      }
      if (auto aggregates = AddGlobalAggregateDescriptors(); !aggregates.has_value()) {
        return aggregates;
      }
      if (auto modifiers = AddAggregateModifierDescriptors(); !modifiers.has_value()) {
        return modifiers;
      }
      if (!select->aggregate->group_by.empty()) {
        if (auto grouped = AddGroupedAggregateDescriptors(); !grouped.has_value()) {
          return grouped;
        }
        core.group_sorter = grouped_sorter_;
      }
    }
    ClearStreamingAggregateContext();
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddSetDescriptors() {
    if (auto streaming = AddStreamingDescriptors(); !streaming.has_value()) {
      return streaming;
    }
    if (bound_select_->compound_collations().size() != bound_select_->result_columns().size() ||
        bound_select_->result_columns().size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InternalFailure("compound relation descriptor shape is invalid"));
    }
    const bool materializes =
        std::ranges::any_of(read_plan_->nodes(), [](const PhysicalNode& node) {
          const auto* compound = std::get_if<PhysicalCompoundNode>(&node.payload);
          return compound != nullptr &&
                 (compound->strategy == PhysicalCompoundStrategy::kEphemeralSet ||
                  compound->strategy == PhysicalCompoundStrategy::kSetThenOrder);
        });
    if (!materializes) {
      return {};
    }
    for (std::optional<RelationId>& relation : set_relations_) {
      OrderingRecordDescriptor descriptor{
          .field_count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
          .key_field_count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
          .key_columns = {},
      };
      descriptor.key_columns.reserve(bound_select_->compound_collations().size());
      for (const BoundCollationId collation : bound_select_->compound_collations()) {
        if (collation.value() >= collation_symbols_.size()) {
          return std::unexpected(InternalFailure("compound collation is not published"));
        }
        descriptor.key_columns.push_back(OrderingColumnMetadata{
            .collation = collation_symbols_[collation.value()],
            .order = BytecodeSortOrder::kAscending,
            .null_placement = BytecodeNullPlacement::kFirst,
        });
      }
      auto added = ConvertProgramResult(AssumeValue(builder_).AddRelation(std::move(descriptor)),
                                        "unable to add compound relation descriptor");
      if (!added.has_value()) {
        return std::unexpected(std::move(added.error()));
      }
      relation = *added;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<OrderingRecordDescriptor> BuildOrderedCompoundDescriptor(
      bool full_key) {
    if (advanced_order_ == nullptr || advanced_order_->core_plans.empty() ||
        advanced_order_->terms.empty()) {
      return std::unexpected(InternalFailure("ordered compound descriptor has no layout"));
    }
    const LogicalCoreOrderLayout& layout = advanced_order_->core_plans.front().layout;
    const std::size_t field_count = layout.key_values.size() + layout.payload_values.size();
    const std::size_t key_count = full_key ? field_count : layout.key_values.size();
    if (field_count > std::numeric_limits<std::uint32_t>::max() ||
        key_count > std::numeric_limits<std::uint32_t>::max() ||
        layout.output_fields.size() != bound_select_->result_columns().size()) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded},
                         "ordered compound descriptor exceeds bytecode field identities"));
    }
    OrderingRecordDescriptor descriptor{
        .field_count = static_cast<std::uint32_t>(field_count),
        .key_field_count = static_cast<std::uint32_t>(key_count),
        .key_columns = {},
    };
    descriptor.key_columns.reserve(key_count);
    for (const BoundOrderingTerm& term : advanced_order_->terms) {
      if (term.collation.value() >= collation_symbols_.size()) {
        return std::unexpected(InternalFailure("ordered compound collation is not published"));
      }
      descriptor.key_columns.push_back(OrderingColumnMetadata{
          .collation = collation_symbols_[term.collation.value()],
          .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                        : BytecodeSortOrder::kAscending,
          .null_placement = term.null_placement == BoundNullPlacement::kLast
                                ? BytecodeNullPlacement::kLast
                                : BytecodeNullPlacement::kFirst,
      });
    }
    if (!full_key) {
      return descriptor;
    }
    if (bound_select_->compound_collations().size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("full-key compound collations are unavailable"));
    }
    for (std::size_t payload = 0; payload < layout.payload_values.size(); ++payload) {
      const auto output =
          std::ranges::find_if(layout.output_fields, [&](const SortOutputField& field) {
            return field.kind == SortOutputFieldKind::kPayload && field.field_index == payload;
          });
      if (output == layout.output_fields.end()) {
        return std::unexpected(InternalFailure("ordered compound payload mapping is invalid"));
      }
      const auto column =
          static_cast<std::size_t>(std::distance(layout.output_fields.begin(), output));
      const BoundCollationId collation = bound_select_->compound_collations()[column];
      if (collation.value() >= collation_symbols_.size()) {
        return std::unexpected(InternalFailure("compound comparison collation is not published"));
      }
      descriptor.key_columns.push_back(OrderingColumnMetadata{
          .collation = collation_symbols_[collation.value()],
          .order = BytecodeSortOrder::kAscending,
          .null_placement = BytecodeNullPlacement::kFirst,
      });
    }
    return descriptor;
  }

  [[nodiscard]] LoweringResult<void> AddOrderedCompoundDescriptors() {
    if (!ordered_compound_ || advanced_order_ == nullptr) {
      return std::unexpected(
          InternalFailure("ordered compound descriptors have no ordering metadata"));
    }
    if (advanced_order_->set_then_order) {
      if (auto set = AddSetDescriptors(); !set.has_value()) {
        return set;
      }
    } else if (auto streaming = AddStreamingDescriptors(); !streaming.has_value()) {
      return streaming;
    }
    const bool has_compound = !bound_select_->compound_operators().empty();

    for (std::size_t slot = 0; slot < ordered_sorters_.size(); ++slot) {
      auto order_descriptor = BuildOrderedCompoundDescriptor(false);
      if (!order_descriptor.has_value()) {
        return std::unexpected(std::move(order_descriptor.error()));
      }
      auto order_sorter =
          ConvertProgramResult(AssumeValue(builder_).AddSorter(std::move(*order_descriptor)),
                               "unable to add ordered compound sorter");
      if (!order_sorter.has_value()) {
        return std::unexpected(std::move(order_sorter.error()));
      }
      ordered_sorters_[slot] = *order_sorter;

      if (!has_compound) {
        continue;
      }
      auto full_descriptor = BuildOrderedCompoundDescriptor(true);
      if (!full_descriptor.has_value()) {
        return std::unexpected(std::move(full_descriptor.error()));
      }
      auto full_sorter =
          ConvertProgramResult(AssumeValue(builder_).AddSorter(std::move(*full_descriptor)),
                               "unable to add full-key compound sorter");
      if (!full_sorter.has_value()) {
        return std::unexpected(std::move(full_sorter.error()));
      }
      ordered_full_sorters_[slot] = *full_sorter;
    }

    auto order_comparison = BuildOrderedCompoundDescriptor(false);
    if (!order_comparison.has_value()) {
      return std::unexpected(std::move(order_comparison.error()));
    }
    order_comparison->field_count = order_comparison->key_field_count;
    auto added_order_comparison = ConvertProgramResult(
        AssumeValue(builder_).AddRecordComparison(std::move(*order_comparison)),
        "unable to add ordered compound comparison");
    if (!added_order_comparison.has_value()) {
      return std::unexpected(std::move(added_order_comparison.error()));
    }
    ordered_comparison_ = *added_order_comparison;

    if (has_compound) {
      auto full_comparison = BuildOrderedCompoundDescriptor(true);
      if (!full_comparison.has_value()) {
        return std::unexpected(std::move(full_comparison.error()));
      }
      auto added_full_comparison = ConvertProgramResult(
          AssumeValue(builder_).AddRecordComparison(std::move(*full_comparison)),
          "unable to add full-key compound comparison");
      if (!added_full_comparison.has_value()) {
        return std::unexpected(std::move(added_full_comparison.error()));
      }
      ordered_full_comparison_ = *added_full_comparison;
    }

    if (OrderedUsesRuntimeLimit()) {
      auto order_top_n_descriptor = BuildOrderedCompoundDescriptor(false);
      if (!order_top_n_descriptor.has_value()) {
        return std::unexpected(std::move(order_top_n_descriptor.error()));
      }
      auto order_top_n =
          ConvertProgramResult(AssumeValue(builder_).AddTopN(std::move(*order_top_n_descriptor)),
                               "unable to add ordered compound top-N");
      if (!order_top_n.has_value()) {
        return std::unexpected(std::move(order_top_n.error()));
      }
      ordered_top_n_ = *order_top_n;

      if (!has_compound) {
        return {};
      }
      auto full_top_n_descriptor = BuildOrderedCompoundDescriptor(true);
      if (!full_top_n_descriptor.has_value()) {
        return std::unexpected(std::move(full_top_n_descriptor.error()));
      }
      auto full_top_n =
          ConvertProgramResult(AssumeValue(builder_).AddTopN(std::move(*full_top_n_descriptor)),
                               "unable to add full-key compound top-N");
      if (!full_top_n.has_value()) {
        return std::unexpected(std::move(full_top_n.error()));
      }
      ordered_full_top_n_ = *full_top_n;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<SymbolId> SymbolForName(std::string_view name) {
    const auto found = std::ranges::find_if(
        symbol_names_, [&](const NamedSymbol& symbol) { return NamesEqual(symbol.name, name); });
    if (found != symbol_names_.end()) {
      return found->id;
    }
    return AddNamedSymbol(name);
  }

  [[nodiscard]] LoweringResult<CursorFieldSource> CatalogFieldSource(const CatalogColumn& column,
                                                                     std::uint32_t record_field) {
    CursorFieldSource source{
        .kind = CursorFieldSourceKind::kRecordField,
        .record_field = record_field,
    };
    if (!column.default_expression.has_value()) {
      return source;
    }
    if (column.missing_record_value == nullptr) {
      source.missing_value_kind = MissingFieldValueKind::kUnsupported;
      return source;
    }
    auto constant = AddConstant(column.missing_record_value->Clone());
    if (!constant.has_value()) {
      return std::unexpected(std::move(constant.error()));
    }
    source.missing_value_kind = MissingFieldValueKind::kConstant;
    source.missing_value = *constant;
    return source;
  }

  [[nodiscard]] LoweringResult<void> AddCursorDescriptor() {
    if (!IsCursorAccess()) {
      return {};
    }
    if (active_source_column_begin_ > bound_select_->source_columns().size() ||
        active_source_column_count_ >
            bound_select_->source_columns().size() - active_source_column_begin_) {
      return std::unexpected(InternalFailure("active source column range is invalid"));
    }
    if (source_cursor_fields_.size() != bound_select_->source_columns().size()) {
      source_cursor_fields_.assign(bound_select_->source_columns().size(), std::nullopt);
    }
    if (source_field_real_affinity_.size() != bound_select_->source_columns().size()) {
      source_field_real_affinity_.assign(bound_select_->source_columns().size(), false);
    }
    std::optional<CursorFieldId> rowid_field;
    const std::span<const BoundSourceColumn> active_columns =
        bound_select_->source_columns().subspan(active_source_column_begin_,
                                                active_source_column_count_);
    const std::optional<BoundSourceId> source_id =
        table_scan_ != nullptr     ? std::optional{table_scan_->source}
        : rowid_lookup_ != nullptr ? std::optional{rowid_lookup_->source}
        : index_scan_ != nullptr   ? std::optional{index_scan_->source}
                                   : std::nullopt;
    if (!source_id.has_value()) {
      return std::unexpected(InternalFailure("cursor access has no source identity"));
    }
    const BoundSourceId owner = *source_id;
    if (std::ranges::any_of(active_columns, [owner](const BoundSourceColumn& column) {
          return column.source != owner;
        })) {
      return std::unexpected(
          InternalFailure("cursor source ownership does not match bound columns"));
    }

    if (index_scan_ != nullptr) {
      const CatalogSnapshot& catalog = *bound_select_->catalog();
      if (index_scan_->table.value >= catalog.tables().size() ||
          index_scan_->index.value >= catalog.indexes().size()) {
        return std::unexpected(InternalFailure("covering index access ID is out of range"));
      }
      const CatalogTable& table = catalog.table(index_scan_->table);
      const CatalogIndex& index = catalog.index(index_scan_->index);
      if (table.without_rowid || index.table != index_scan_->table ||
          table.root_page != index_scan_->table_root_page ||
          index.root_page != index_scan_->index_root_page || index.terms.empty() ||
          index.terms.size() > std::numeric_limits<std::uint32_t>::max() ||
          !std::holds_alternative<RowIdIndexTerm>(index.terms.back().target)) {
        return std::unexpected(InternalFailure("covering index access metadata is invalid"));
      }

      ReadCursorDescriptor descriptor{
          .root_page = RootPageNumber(index.root_page.value),
          .storage = CursorStorageKind::kIndex,
          .record_field_count = static_cast<std::uint32_t>(index.terms.size()),
          .fields = {},
          .index_columns = {},
      };
      if (index_scan_->covering) {
        descriptor.fields.reserve(index.terms.size());
      }
      descriptor.index_columns.reserve(index.terms.size());
      std::vector<std::optional<std::uint32_t>> field_by_column(table.columns.size());
      for (std::size_t term_index = 0; term_index < index.terms.size(); ++term_index) {
        const CatalogIndexTerm& term = index.terms[term_index];
        const auto field = static_cast<std::uint32_t>(term_index);
        if (index_scan_->covering) {
          descriptor.fields.push_back(CursorFieldSource{
              .kind = CursorFieldSourceKind::kRecordField,
              .record_field = field,
          });
        }
        auto collation = SymbolForName(term.collation_name);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
        descriptor.index_columns.push_back(IndexColumnMetadata{
            .collation = *collation,
            .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                          : BytecodeSortOrder::kAscending,
        });
        if (index_scan_->covering) {
          if (const auto* column = std::get_if<ColumnId>(&term.target);
              column != nullptr && column->value < field_by_column.size() &&
              !field_by_column[column->value].has_value()) {
            field_by_column[column->value] = field;
          } else if (std::holds_alternative<RowIdIndexTerm>(term.target)) {
            rowid_field = CursorFieldId{field};
          }
        }
        if (!index_scan_->covering) {
          descriptor.fields.push_back(CursorFieldSource{
              .kind = CursorFieldSourceKind::kRecordField,
              .record_field = static_cast<std::uint32_t>(index.terms.size() - 1U),
          });
        }
      }
      if (index_scan_->covering) {
        if (table.rowid_alias.has_value() && rowid_field.has_value() &&
            !field_by_column[table.rowid_alias->value].has_value()) {
          field_by_column[table.rowid_alias->value] = rowid_field->value();
        }
        for (std::size_t local_index = 0; local_index < active_columns.size(); ++local_index) {
          const std::size_t source_index = active_source_column_begin_ + local_index;
          const BoundSourceColumn& source_column = active_columns[local_index];
          if (!source_column.catalog_column.has_value() ||
              source_column.catalog_column->value >= field_by_column.size()) {
            return std::unexpected(InternalFailure("covering index source column is invalid"));
          }
          const std::optional<std::uint32_t> field =
              field_by_column[source_column.catalog_column->value];
          if (field.has_value()) {
            source_cursor_fields_[source_index] = CursorFieldId{*field};
            source_field_real_affinity_[source_index] =
                source_column.affinity == TypeAffinity::kReal;
          }
        }
      }

      auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                         "unable to add the index cursor descriptor");
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      index_cursor_ = *cursor;
      if (index_scan_->covering) {
        cursor_ = *cursor;
        return PublishSourceCursor(owner, *cursor, rowid_field);
      }
    }

    BoundSourceKind source_kind = BoundSourceKind::kCatalogTable;
    std::optional<TableId> access_table;
    std::uint32_t root_page = 0;
    if (table_scan_ != nullptr) {
      source_kind = table_scan_->source_kind;
      access_table = table_scan_->table;
      root_page = table_scan_->root_page.value;
    } else if (rowid_lookup_ != nullptr) {
      source_kind = rowid_lookup_->source_kind;
      access_table = rowid_lookup_->table;
      root_page = rowid_lookup_->root_page.value;
    } else if (index_scan_ != nullptr && !index_scan_->covering) {
      source_kind = BoundSourceKind::kCatalogTable;
      access_table = index_scan_->table;
      root_page = index_scan_->table_root_page.value;
    } else {
      return std::unexpected(InternalFailure("cursor access metadata is unavailable"));
    }

    ReadCursorDescriptor descriptor{
        .root_page = RootPageNumber(root_page),
        .storage = CursorStorageKind::kRowIdTable,
        .record_field_count = 0,
        .fields = {},
        .index_columns = {},
    };
    if (source_kind == BoundSourceKind::kSchemaTable) {
      constexpr std::uint32_t kSchemaFieldCount = 5;
      if (active_columns.size() != kSchemaFieldCount) {
        return std::unexpected(InternalFailure("sqlite_schema source column count is invalid"));
      }
      descriptor.record_field_count = kSchemaFieldCount;
      descriptor.fields.reserve(kSchemaFieldCount);
      for (std::uint32_t index = 0; index < kSchemaFieldCount; ++index) {
        descriptor.fields.push_back(CursorFieldSource{
            .kind = CursorFieldSourceKind::kRecordField,
            .record_field = index,
        });
        source_cursor_fields_[active_source_column_begin_ + index] = CursorFieldId{index};
      }
    } else {
      if (!access_table.has_value()) {
        return std::unexpected(InternalFailure("catalog-table access has no table ID"));
      }
      const CatalogSnapshot& catalog = *bound_select_->catalog();
      const TableId table_id = AssumeValue(access_table);
      const CatalogTable& table = catalog.table(table_id);
      if (!table.without_rowid) {
        if (table.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(InternalFailure("table record field count is too large"));
        }
        descriptor.record_field_count = static_cast<std::uint32_t>(table.columns.size());
        descriptor.fields.reserve(active_columns.size());
        for (std::size_t index = 0; index < active_columns.size(); ++index) {
          const std::size_t source_index = active_source_column_begin_ + index;
          const BoundSourceColumn& source_column = active_columns[index];
          if (!source_column.catalog_column.has_value() ||
              source_column.catalog_column->value >= table.columns.size()) {
            return std::unexpected(InternalFailure("bound source column is not a table column"));
          }
          const ColumnId column_id = *source_column.catalog_column;
          if (table.rowid_alias.has_value() && *table.rowid_alias == column_id) {
            descriptor.fields.push_back(CursorFieldSource{
                .kind = CursorFieldSourceKind::kRowId,
                .record_field = 0,
            });
          } else {
            auto field = CatalogFieldSource(table.columns[column_id.value],
                                            static_cast<std::uint32_t>(column_id.value));
            if (!field.has_value()) {
              return std::unexpected(std::move(field.error()));
            }
            descriptor.fields.push_back(*field);
            source_field_real_affinity_[source_index] =
                source_column.affinity == TypeAffinity::kReal;
          }
          source_cursor_fields_[source_index] =
              CursorFieldId{static_cast<std::uint32_t>(descriptor.fields.size() - 1U)};
        }
      } else {
        descriptor.storage = CursorStorageKind::kIndex;
        const std::optional<IndexId> primary_id = catalog.primary_key_index(table_id);
        if (!primary_id.has_value()) {
          return std::unexpected(InternalFailure("WITHOUT ROWID table has no primary index"));
        }
        const CatalogIndex& primary = catalog.index(*primary_id);
        if (primary.terms.empty() ||
            primary.terms.size() > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(InternalFailure("WITHOUT ROWID primary index shape is invalid"));
        }
        descriptor.record_field_count = static_cast<std::uint32_t>(primary.terms.size());
        descriptor.index_columns.reserve(primary.terms.size());
        std::vector<std::optional<std::uint32_t>> field_by_column(table.columns.size());
        for (std::size_t term_index = 0; term_index < primary.terms.size(); ++term_index) {
          const CatalogIndexTerm& term = primary.terms[term_index];
          const auto* column = std::get_if<ColumnId>(&term.target);
          if (column == nullptr || column->value >= table.columns.size() ||
              field_by_column[column->value].has_value()) {
            return std::unexpected(InternalFailure(
                "WITHOUT ROWID primary record contains a non-column or duplicate term"));
          }
          field_by_column[column->value] = static_cast<std::uint32_t>(term_index);
          auto collation = SymbolForName(term.collation_name);
          if (!collation.has_value()) {
            return std::unexpected(std::move(collation.error()));
          }
          descriptor.index_columns.push_back(IndexColumnMetadata{
              .collation = *collation,
              .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                            : BytecodeSortOrder::kAscending,
          });
        }
        descriptor.fields.reserve(active_columns.size());
        for (std::size_t index = 0; index < active_columns.size(); ++index) {
          const std::size_t source_index = active_source_column_begin_ + index;
          const BoundSourceColumn& source_column = active_columns[index];
          if (!source_column.catalog_column.has_value() ||
              source_column.catalog_column->value >= table.columns.size()) {
            return std::unexpected(
                InternalFailure("bound source column is not a WITHOUT ROWID column"));
          }
          const ColumnId column_id = *source_column.catalog_column;
          if (!field_by_column[column_id.value].has_value()) {
            return std::unexpected(
                InternalFailure("WITHOUT ROWID primary record omits a table column"));
          }
          const std::optional<std::uint32_t>& record_field = field_by_column[column_id.value];
          auto field =
              CatalogFieldSource(table.columns[column_id.value], AssumeValue(record_field));
          if (!field.has_value()) {
            return std::unexpected(std::move(field.error()));
          }
          descriptor.fields.push_back(*field);
          source_cursor_fields_[source_index] =
              CursorFieldId{static_cast<std::uint32_t>(descriptor.fields.size() - 1U)};
          source_field_real_affinity_[source_index] = source_column.affinity == TypeAffinity::kReal;
        }
      }
    }

    auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                       "unable to add the read cursor descriptor");
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    cursor_ = *cursor;
    return PublishSourceCursor(owner, *cursor, std::nullopt);
  }

  [[nodiscard]] LoweringResult<void> PublishSourceCursor(BoundSourceId source, CursorId cursor,
                                                         std::optional<CursorFieldId> rowid_field) {
    if (source_cursor_bindings_.empty()) {
      std::size_t source_count = 0;
      for (const BoundQueryCore& core : bound_select_->owner().query_cores()) {
        if (const auto* select = std::get_if<BoundSelectCore>(&core); select != nullptr) {
          source_count += select->table_sources.size();
        }
      }
      source_cursor_bindings_.resize(source_count);
    }
    if (source.value() >= source_cursor_bindings_.size() ||
        source_cursor_bindings_[source.value()].has_value()) {
      return std::unexpected(InternalFailure("source cursor ownership is duplicated or invalid"));
    }
    source_cursor_bindings_[source.value()] =
        SourceCursorBinding{.cursor = cursor, .rowid_field = rowid_field};
    return {};
  }

  [[nodiscard]] LoweringResult<SourceCursorBinding> SourceCursor(BoundSourceId source) const {
    if (source.value() >= source_cursor_bindings_.size()) {
      return std::unexpected(InternalFailure("source expression has invalid ownership"));
    }
    const std::optional<SourceCursorBinding>& binding = source_cursor_bindings_[source.value()];
    if (!binding.has_value()) {
      return std::unexpected(InternalFailure("source expression has no owned cursor"));
    }
    return *binding;
  }

  [[nodiscard]] LoweringResult<void> AddMutationReadCursorDescriptor(
      const BoundMutationTarget& target, const CatalogSnapshot& catalog) {
    const CatalogTable& table = catalog.table(target.table);
    if (table.without_rowid || table.root_page != target.root_page ||
        table.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InternalFailure("bound mutation target table shape is invalid"));
    }

    ReadCursorDescriptor descriptor{
        .root_page = RootPageNumber(target.root_page.value),
        .storage = CursorStorageKind::kRowIdTable,
        .record_field_count = static_cast<std::uint32_t>(table.columns.size()),
        .fields = {},
        .index_columns = {},
    };
    descriptor.fields.reserve(source_columns_.size());
    source_field_real_affinity_.assign(source_columns_.size(), false);
    for (std::size_t index = 0; index < source_columns_.size(); ++index) {
      const BoundSourceColumn& source_column = source_columns_[index];
      if (!source_column.catalog_column.has_value() ||
          source_column.catalog_column->value >= table.columns.size()) {
        return std::unexpected(InternalFailure("bound mutation source column is invalid"));
      }
      const ColumnId column_id = *source_column.catalog_column;
      if (table.rowid_alias.has_value() && *table.rowid_alias == column_id) {
        descriptor.fields.push_back(CursorFieldSource{
            .kind = CursorFieldSourceKind::kRowId,
            .record_field = 0,
        });
      } else {
        auto field = CatalogFieldSource(table.columns[column_id.value],
                                        static_cast<std::uint32_t>(column_id.value));
        if (!field.has_value()) {
          return std::unexpected(std::move(field.error()));
        }
        descriptor.fields.push_back(*field);
        source_field_real_affinity_[index] = source_column.affinity == TypeAffinity::kReal;
      }
    }

    auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                       "unable to add the mutation read cursor descriptor");
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    cursor_ = *cursor;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddMutationWriteCursorDescriptor(
      const BoundMutationTarget& target,
      std::vector<std::optional<ConstantId>>* default_constants) {
    if (target.columns.empty() ||
        target.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InternalFailure("bound mutation target column count is invalid"));
    }

    WriteCursorDescriptor descriptor{
        .root_page = RootPageNumber(target.root_page.value),
        .columns = {},
        .rowid_alias = std::nullopt,
        .index_columns = {},
        .key_term_count = 0,
        .unique = false,
        .unique_not_null = false,
        .storage = WriteCursorStorageKind::kRowIdTable,
    };
    descriptor.columns.reserve(target.columns.size());
    if (default_constants != nullptr) {
      default_constants->assign(target.columns.size(), std::nullopt);
    }

    for (std::size_t index = 0; index < target.columns.size(); ++index) {
      const BoundMutationColumn& column = target.columns[index];
      std::optional<ConstantId> default_value;
      if (default_constants != nullptr && !column.rowid_alias && column.default_value != nullptr) {
        auto constant = AddConstant(column.default_value->Clone());
        if (!constant.has_value()) {
          return std::unexpected(std::move(constant.error()));
        }
        default_value = *constant;
        (*default_constants)[index] = *constant;
      }
      if (column.rowid_alias) {
        if (descriptor.rowid_alias.has_value()) {
          return std::unexpected(
              InternalFailure("bound mutation target has duplicate rowid aliases"));
        }
        descriptor.rowid_alias = static_cast<std::uint32_t>(index);
      }
      descriptor.columns.push_back(WriteColumnDescriptor{
          .affinity = column.affinity,
          .not_null = column.not_null,
          .rowid_alias = column.rowid_alias,
          .default_value = default_value,
      });
    }

    if (target.rowid_alias.has_value()) {
      const std::optional<std::uint32_t> alias = TargetColumnIndex(target, *target.rowid_alias);
      if (!alias.has_value() || descriptor.rowid_alias != alias) {
        return std::unexpected(
            InternalFailure("bound mutation rowid alias metadata is inconsistent"));
      }
    } else if (descriptor.rowid_alias.has_value()) {
      return std::unexpected(
          InternalFailure("bound mutation target has an unexpected rowid alias"));
    }

    auto cursor = ConvertProgramResult(AssumeValue(builder_).AddWriteCursor(std::move(descriptor)),
                                       "unable to add the mutation write cursor descriptor");
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    write_cursor_ = *cursor;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddMutationIndexWriteCursorDescriptors(
      const BoundMutationTarget& target) {
    index_write_cursors_.reserve(target.indexes.size());
    for (const BoundIndexMaintenance& index : target.indexes) {
      if (index.terms.empty() || index.terms.size() > std::numeric_limits<std::uint32_t>::max() ||
          index.key_term_count == 0U || index.key_term_count >= index.terms.size()) {
        return std::unexpected(InternalFailure("bound index maintenance shape is invalid"));
      }
      WriteCursorDescriptor descriptor{
          .root_page = RootPageNumber(index.root_page.value),
          .columns = {},
          .rowid_alias = std::nullopt,
          .index_columns = {},
          .key_term_count = index.key_term_count,
          .unique = index.unique,
          .unique_not_null = index.unique_not_null,
          .storage = WriteCursorStorageKind::kIndex,
      };
      descriptor.index_columns.reserve(index.terms.size());
      for (const BoundIndexTerm& term : index.terms) {
        auto collation = SymbolForName(term.collation_name);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
        descriptor.index_columns.push_back(IndexColumnMetadata{
            .collation = *collation,
            .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                          : BytecodeSortOrder::kAscending,
        });
      }
      auto cursor =
          ConvertProgramResult(AssumeValue(builder_).AddWriteCursor(std::move(descriptor)),
                               "unable to add an index write cursor descriptor");
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      index_write_cursors_.push_back(*cursor);
    }
    return {};
  }

  [[nodiscard]] LoweringResult<WriteCursorId> AddSchemaWriteCursorDescriptor() {
    WriteCursorDescriptor descriptor{
        .root_page = RootPageNumber(1),
        .columns =
            {
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kText,
                    .not_null = true,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kText,
                    .not_null = true,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kText,
                    .not_null = true,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kInteger,
                    .not_null = true,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kText,
                    .not_null = true,
                },
            },
        .rowid_alias = std::nullopt,
        .index_columns = {},
        .key_term_count = 0,
        .unique = false,
        .unique_not_null = false,
        .pending_root = false,
        .storage = WriteCursorStorageKind::kRowIdTable,
    };
    return ConvertProgramResult(AssumeValue(builder_).AddWriteCursor(std::move(descriptor)),
                                "unable to add the schema write cursor");
  }

  [[nodiscard]] LoweringResult<void> AddCreateIndexDescriptors() {
    const CatalogSnapshot* catalog = bound_create_index_->catalog();
    const std::span<const BoundCreateIndexTerm> terms = bound_create_index_->terms();
    if (catalog == nullptr || bound_create_index_->table().value >= catalog->tables().size() ||
        terms.empty() || terms.size() >= std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InternalFailure("bound CREATE INDEX metadata is invalid"));
    }
    const CatalogTable& table = catalog->table(bound_create_index_->table());
    if (table.without_rowid || table.root_page != bound_create_index_->table_root_page() ||
        table.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InternalFailure("bound CREATE INDEX table metadata is invalid"));
    }

    ReadCursorDescriptor read_descriptor{
        .root_page = RootPageNumber(table.root_page.value),
        .storage = CursorStorageKind::kRowIdTable,
        .record_field_count = static_cast<std::uint32_t>(table.columns.size()),
        .fields = {},
        .index_columns = {},
    };
    read_descriptor.fields.reserve(terms.size());
    for (const BoundCreateIndexTerm& term : terms) {
      if (term.column.value >= table.columns.size()) {
        return std::unexpected(InternalFailure("CREATE INDEX term column is out of range"));
      }
      if (term.rowid) {
        if (table.rowid_alias != term.column) {
          return std::unexpected(InternalFailure("CREATE INDEX rowid term metadata is invalid"));
        }
        read_descriptor.fields.push_back(CursorFieldSource{
            .kind = CursorFieldSourceKind::kRowId,
            .record_field = 0,
        });
      } else {
        auto field = CatalogFieldSource(table.columns[term.column.value],
                                        static_cast<std::uint32_t>(term.column.value));
        if (!field.has_value()) {
          return std::unexpected(std::move(field.error()));
        }
        read_descriptor.fields.push_back(*field);
      }
    }
    auto read_cursor =
        ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(read_descriptor)),
                             "unable to add the CREATE INDEX table cursor");
    if (!read_cursor.has_value()) {
      return std::unexpected(std::move(read_cursor.error()));
    }
    create_index_table_cursor_ = *read_cursor;

    auto schema_cursor = AddSchemaWriteCursorDescriptor();
    if (!schema_cursor.has_value()) {
      return std::unexpected(std::move(schema_cursor.error()));
    }
    schema_write_cursor_ = *schema_cursor;

    WriteCursorDescriptor index_descriptor{
        .root_page = RootPageNumber(0),
        .columns = {},
        .rowid_alias = std::nullopt,
        .index_columns = {},
        .key_term_count = static_cast<std::uint32_t>(terms.size()),
        .unique = bound_create_index_->unique(),
        .unique_not_null = bound_create_index_->unique_not_null(),
        .pending_root = true,
        .storage = WriteCursorStorageKind::kIndex,
    };
    index_descriptor.index_columns.reserve(terms.size() + 1U);
    for (const BoundCreateIndexTerm& term : terms) {
      auto collation = SymbolForName(term.collation_name);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      index_descriptor.index_columns.push_back(IndexColumnMetadata{
          .collation = *collation,
          .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                        : BytecodeSortOrder::kAscending,
      });
    }
    auto binary = SymbolForName("BINARY");
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    index_descriptor.index_columns.push_back(IndexColumnMetadata{
        .collation = *binary,
        .order = BytecodeSortOrder::kAscending,
    });
    auto index_cursor =
        ConvertProgramResult(AssumeValue(builder_).AddWriteCursor(std::move(index_descriptor)),
                             "unable to add the created index cursor");
    if (!index_cursor.has_value()) {
      return std::unexpected(std::move(index_cursor.error()));
    }
    create_index_write_cursor_ = *index_cursor;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddAnalyzeDescriptors(const PhysicalAnalyzeMutation& analyze) {
    RootPageNumber stat1_root{0};
    if (!analyze.creates_stat1) {
      const std::optional<RootPageId> bound_root = bound_analyze_->stat1_root_page();
      if (!bound_root.has_value()) {
        return std::unexpected(InternalFailure("ANALYZE sqlite_stat1 root is missing"));
      }
      stat1_root = RootPageNumber(AssumeValue(bound_root).value);
    }
    WriteCursorDescriptor stat1_descriptor{
        .root_page = stat1_root,
        .columns =
            {
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kBlob,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kBlob,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kBlob,
                },
            },
        .rowid_alias = std::nullopt,
        .index_columns = {},
        .key_term_count = 0,
        .unique = false,
        .unique_not_null = false,
        .pending_root = analyze.creates_stat1,
        .storage = WriteCursorStorageKind::kRowIdTable,
    };
    auto stat1_cursor =
        ConvertProgramResult(AssumeValue(builder_).AddWriteCursor(std::move(stat1_descriptor)),
                             "unable to add the sqlite_stat1 write cursor");
    if (!stat1_cursor.has_value()) {
      return std::unexpected(std::move(stat1_cursor.error()));
    }
    stat1_write_cursor_ = *stat1_cursor;

    if (analyze.creates_stat1) {
      auto schema_cursor = AddSchemaWriteCursorDescriptor();
      if (!schema_cursor.has_value()) {
        return std::unexpected(std::move(schema_cursor.error()));
      }
      schema_write_cursor_ = *schema_cursor;
    }

    analyze_index_cursors_.reserve(bound_analyze_->indexes().size());
    for (const BoundAnalyzeIndex& index : bound_analyze_->indexes()) {
      if (index.columns.size() > std::numeric_limits<std::uint32_t>::max() ||
          index.key_term_count == 0U || index.key_term_count > index.columns.size()) {
        return std::unexpected(InternalFailure("bound ANALYZE index metadata is invalid"));
      }
      ReadCursorDescriptor descriptor{
          .root_page = RootPageNumber(index.root_page.value),
          .storage = CursorStorageKind::kIndex,
          .record_field_count = static_cast<std::uint32_t>(index.columns.size()),
          .fields = {},
          .index_columns = {},
      };
      descriptor.index_columns.reserve(index.columns.size());
      for (const BoundAnalyzeIndexColumn& column : index.columns) {
        auto collation = SymbolForName(column.collation_name);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
        descriptor.index_columns.push_back(IndexColumnMetadata{
            .collation = *collation,
            .order = column.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                            : BytecodeSortOrder::kAscending,
        });
      }
      auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                         "unable to add an ANALYZE index cursor");
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      analyze_index_cursors_.push_back(*cursor);
    }

    analyze_table_cursors_.reserve(bound_analyze_->tables().size());
    for (const BoundAnalyzeTable& table : bound_analyze_->tables()) {
      if (table.record_field_count == 0U) {
        return std::unexpected(InternalFailure("bound ANALYZE table metadata is invalid"));
      }
      ReadCursorDescriptor descriptor{
          .root_page = RootPageNumber(table.root_page.value),
          .storage = CursorStorageKind::kRowIdTable,
          .record_field_count = table.record_field_count,
          .fields = {},
          .index_columns = {},
      };
      auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                         "unable to add an ANALYZE table cursor");
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      analyze_table_cursors_.push_back(*cursor);
    }
    return {};
  }

  [[nodiscard]] static std::optional<std::uint32_t> TargetColumnIndex(
      const BoundMutationTarget& target, ColumnId column) noexcept {
    const std::span<const BoundMutationColumn> columns = target.columns;
    const auto found = std::ranges::find_if(
        columns,
        [column](const BoundMutationColumn& candidate) { return candidate.column == column; });
    if (found == columns.end()) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::distance(columns.begin(), found));
  }

  [[nodiscard]] LoweringResult<ConstantId> AddConstant(SqlValue value) {
    return ConvertProgramResult(AssumeValue(builder_).AddConstant(std::move(value)),
                                "unable to add a bytecode constant");
  }

  [[nodiscard]] LoweringResult<ConstantId> EnsureNullConstant() {
    if (!null_constant_.has_value()) {
      auto constant = AddConstant(SqlValue{});
      if (!constant.has_value()) {
        return std::unexpected(std::move(constant.error()));
      }
      null_constant_ = *constant;
    }
    return *null_constant_;
  }

  [[nodiscard]] LoweringResult<ConstantId> EnsureZeroConstant() {
    if (!zero_constant_.has_value()) {
      auto constant = AddConstant(SqlValue::Integer(0));
      if (!constant.has_value()) {
        return std::unexpected(std::move(constant.error()));
      }
      zero_constant_ = *constant;
    }
    return *zero_constant_;
  }

  [[nodiscard]] LoweringResult<ConstantId> EnsureOneConstant() {
    if (!one_constant_.has_value()) {
      auto constant = AddConstant(SqlValue::Integer(1));
      if (!constant.has_value()) {
        return std::unexpected(std::move(constant.error()));
      }
      one_constant_ = *constant;
    }
    return *one_constant_;
  }

  [[nodiscard]] LoweringResult<ConstantId> EnsureMaximumIntegerConstant() {
    if (!maximum_integer_constant_.has_value()) {
      auto constant = AddConstant(SqlValue::Integer(std::numeric_limits<std::int64_t>::max()));
      if (!constant.has_value()) {
        return std::unexpected(std::move(constant.error()));
      }
      maximum_integer_constant_ = *constant;
    }
    return *maximum_integer_constant_;
  }

  [[nodiscard]] LoweringResult<SymbolId> EnsureBinarySymbol() {
    if (!binary_symbol_.has_value()) {
      auto symbol = SymbolForName("BINARY");
      if (!symbol.has_value()) {
        return std::unexpected(std::move(symbol.error()));
      }
      binary_symbol_ = *symbol;
    }
    return *binary_symbol_;
  }

  [[nodiscard]] LoweringResult<void> Append(Instruction instruction) {
    if (bound_select_ != nullptr &&
        bound_select_->query().destination != BoundQueryDestination::kClient) {
      if (const auto* row = std::get_if<ResultRowInstruction>(&instruction); row != nullptr) {
        const SubqueryCallBinding& call =
            statement_.subqueries[bound_select_->query().id.value() - 1U];
        if (bound_select_->query().destination == BoundQueryDestination::kScalar) {
          if (row->count != 1U) {
            return std::unexpected(InternalFailure("scalar result destination has invalid width"));
          }
          instruction = CopyInstruction{.input = row->first, .output = call.result};
        } else {
          auto one = EnsureOneConstant();
          if (!one.has_value()) {
            return std::unexpected(std::move(one.error()));
          }
          instruction = LoadConstantInstruction{.constant = *one, .output = call.result};
        }
      } else if (std::holds_alternative<HaltInstruction>(instruction)) {
        if (!subquery_completion_.has_value()) {
          return std::unexpected(InternalFailure("subquery has no private completion target"));
        }
        return EmitJump(*subquery_completion_);
      }
    }
    auto appended = ConvertProgramResult(AssumeValue(builder_).Append(instruction),
                                         "unable to append a bytecode instruction");
    if (!appended.has_value()) {
      return std::unexpected(std::move(appended.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<Label> CreateLabel() {
    return ConvertProgramResult(AssumeValue(builder_).CreateLabel(),
                                "unable to create a bytecode label");
  }

  [[nodiscard]] LoweringResult<void> BindLabel(Label label) {
    auto bound = ConvertProgramResult(AssumeValue(builder_).BindLabel(label),
                                      "unable to bind a bytecode label");
    if (!bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJump(Label target) {
    auto emitted = ConvertProgramResult(AssumeValue(builder_).EmitJump(target),
                                        lowering_internal::kEmitJumpFailureDetail);
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJumpIf(RegisterId input, JumpCondition condition,
                                                Label target) {
    auto emitted = ConvertProgramResult(AssumeValue(builder_).EmitJumpIf(input, condition, target),
                                        lowering_internal::kEmitConditionalJumpFailureDetail);
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return {};
  }

  [[nodiscard]] RegisterId Home(BoundExpressionId id) const noexcept {
    return RegisterId{id.value()};
  }

  [[nodiscard]] LoweringResult<void> EmitSubqueryValue(BoundQueryId query, RegisterId destination) {
    if (query.value() == 0U || query.value() > statement_.subqueries.size()) {
      return std::unexpected(InternalFailure("subquery expression has invalid query ownership"));
    }
    const BoundQuery& target = bound_select_->owner().query(query);
    if (target.destination == BoundQueryDestination::kScalar &&
        target.output_columns.size() != 1U) {
      return std::unexpected(UnsupportedFailure("sub-select returns " +
                                                std::to_string(target.output_columns.size()) +
                                                " columns - expected 1"));
    }
    SubqueryCallBinding& call = statement_.subqueries[query.value() - 1U];
    if (!call.subroutine.has_value()) {
      auto entry = CreateLabel();
      auto last = CreateLabel();
      if (!entry.has_value()) {
        return std::unexpected(std::move(entry.error()));
      }
      if (!last.has_value()) {
        return std::unexpected(std::move(last.error()));
      }
      auto subroutine = ConvertProgramResult(
          AssumeValue(builder_).AddSubroutine(*entry, *last, SubroutineKind::kRead),
          "unable to register a private query subroutine");
      if (!subroutine.has_value()) {
        return std::unexpected(std::move(subroutine.error()));
      }
      call.entry = *entry;
      call.last = *last;
      call.subroutine = *subroutine;
    }
    auto cached = CreateLabel();
    if (!cached.has_value()) {
      return std::unexpected(std::move(cached.error()));
    }
    if (auto ready = EmitJumpIf(call.ready, JumpCondition::kIfTrue, *cached); !ready.has_value()) {
      return ready;
    }
    if (auto called = Append(CallSubroutineInstruction{.subroutine = *call.subroutine});
        !called.has_value()) {
      return called;
    }
    if (auto bound = BindLabel(*cached); !bound.has_value()) {
      return bound;
    }
    return Append(CopyInstruction{.input = call.result, .output = destination});
  }

  [[nodiscard]] const BoundExpression& Expression(BoundExpressionId id) const noexcept {
    assert(id.value() < expressions_.size());
    return expressions_[id.value()];
  }

  [[nodiscard]] LoweringResult<SymbolId> CollationSymbol(BoundCollationId id) const {
    if (id.value() >= collation_symbols_.size()) {
      return std::unexpected(InternalFailure("bound collation ID is out of range"));
    }
    return collation_symbols_[id.value()];
  }

  [[nodiscard]] LoweringResult<SymbolId> FunctionSymbol(BoundFunctionId id) const {
    if (id.value() >= function_symbols_.size()) {
      return std::unexpected(InternalFailure("bound function ID is out of range"));
    }
    return function_symbols_[id.value()];
  }

  [[nodiscard]] static UnaryOperation ToUnaryOperation(BoundUnaryOperation operation) {
    switch (operation) {
      case BoundUnaryOperation::kNegative:
        return UnaryOperation::kNegate;
      case BoundUnaryOperation::kBitwiseNot:
        return UnaryOperation::kBitwiseNot;
      case BoundUnaryOperation::kLogicalNot:
        return UnaryOperation::kLogicalNot;
      case BoundUnaryOperation::kPositive:
        break;
    }
    return UnaryOperation::kNegate;
  }

  [[nodiscard]] static BinaryOperation ToBinaryOperation(BoundBinaryOperation operation) {
    switch (operation) {
      case BoundBinaryOperation::kAdd:
        return BinaryOperation::kAdd;
      case BoundBinaryOperation::kSubtract:
        return BinaryOperation::kSubtract;
      case BoundBinaryOperation::kMultiply:
        return BinaryOperation::kMultiply;
      case BoundBinaryOperation::kDivide:
        return BinaryOperation::kDivide;
      case BoundBinaryOperation::kRemainder:
        return BinaryOperation::kRemainder;
      case BoundBinaryOperation::kShiftLeft:
        return BinaryOperation::kShiftLeft;
      case BoundBinaryOperation::kShiftRight:
        return BinaryOperation::kShiftRight;
      case BoundBinaryOperation::kBitwiseAnd:
        return BinaryOperation::kBitwiseAnd;
      case BoundBinaryOperation::kBitwiseOr:
        return BinaryOperation::kBitwiseOr;
      case BoundBinaryOperation::kConcatenate:
        return BinaryOperation::kConcatenate;
      case BoundBinaryOperation::kLogicalAnd:
        return BinaryOperation::kLogicalAnd;
      case BoundBinaryOperation::kLogicalOr:
        return BinaryOperation::kLogicalOr;
    }
    return BinaryOperation::kAdd;
  }

  [[nodiscard]] std::optional<BoundExpressionId> SimplifiedLogicalOperand(
      const BoundBinaryExpression& binary) const noexcept {
    if (binary.operation != BoundBinaryOperation::kLogicalAnd &&
        binary.operation != BoundBinaryOperation::kLogicalOr) {
      return std::nullopt;
    }
    const BoundTruthHint left = Expression(binary.left).properties.truth_hint;
    const BoundTruthHint right = Expression(binary.right).properties.truth_hint;
    const bool is_and = binary.operation == BoundBinaryOperation::kLogicalAnd;
    if (left == BoundTruthHint::kAlwaysTrue || right == BoundTruthHint::kAlwaysFalse) {
      return is_and ? binary.right : binary.left;
    }
    if (right == BoundTruthHint::kAlwaysTrue || left == BoundTruthHint::kAlwaysFalse) {
      return is_and ? binary.left : binary.right;
    }
    return std::nullopt;
  }

  [[nodiscard]] LoweringResult<void> EmitTruthTest(const BoundTruthTestExpression& truth_test,
                                                   RegisterId destination) {
    const RegisterId operand = Home(truth_test.operand);
    if (auto emitted = EmitExpression(truth_test.operand, operand); !emitted.has_value()) {
      return emitted;
    }
    const std::int64_t matching = truth_test.negated ? 0 : 1;
    const std::int64_t nonmatching = truth_test.negated ? 1 : 0;
    auto matching_constant = matching == 0 ? EnsureZeroConstant() : EnsureOneConstant();
    auto nonmatching_constant = nonmatching == 0 ? EnsureZeroConstant() : EnsureOneConstant();
    if (!matching_constant.has_value()) {
      return std::unexpected(std::move(matching_constant.error()));
    }
    if (!nonmatching_constant.has_value()) {
      return std::unexpected(std::move(nonmatching_constant.error()));
    }
    auto match = CreateLabel();
    auto end = CreateLabel();
    if (!match.has_value()) {
      return std::unexpected(std::move(match.error()));
    }
    if (!end.has_value()) {
      return std::unexpected(std::move(end.error()));
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = *nonmatching_constant,
            .output = destination,
        });
        !loaded.has_value()) {
      return loaded;
    }
    const JumpCondition condition = truth_test.expected == BoundTruthValue::kTrue
                                        ? JumpCondition::kIfTrue
                                        : JumpCondition::kIfFalse;
    if (auto jumped = EmitJumpIf(operand, condition, *match); !jumped.has_value()) {
      return jumped;
    }
    if (auto jumped = EmitJump(*end); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*match); !bound.has_value()) {
      return bound;
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = *matching_constant,
            .output = destination,
        });
        !loaded.has_value()) {
      return loaded;
    }
    return BindLabel(*end);
  }

  [[nodiscard]] LoweringResult<void> EmitCoalesce(const BoundCoalesceExpression& coalesce,
                                                  RegisterId destination) {
    if (coalesce.arguments.empty()) {
      return std::unexpected(InternalFailure("coalesce expression has no arguments"));
    }
    if (coalesce.arguments.size() == 1U) {
      return EmitExpression(coalesce.arguments.front(), destination);
    }
    auto end = CreateLabel();
    if (!end.has_value()) {
      return std::unexpected(std::move(end.error()));
    }
    for (std::size_t index = 0; index + 1U < coalesce.arguments.size(); ++index) {
      if (auto emitted = EmitExpression(coalesce.arguments[index], destination);
          !emitted.has_value()) {
        return emitted;
      }
      if (auto jumped = EmitJumpIf(destination, JumpCondition::kIfNotNull, *end);
          !jumped.has_value()) {
        return jumped;
      }
    }
    if (auto emitted = EmitExpression(coalesce.arguments.back(), destination);
        !emitted.has_value()) {
      return emitted;
    }
    return BindLabel(*end);
  }

  [[nodiscard]] LoweringResult<void> EmitConditional(const BoundConditionalExpression& conditional,
                                                     RegisterId destination) {
    if (conditional.arguments.size() < 2U) {
      return std::unexpected(InternalFailure("conditional expression has too few arguments"));
    }
    auto end = CreateLabel();
    if (!end.has_value()) {
      return std::unexpected(std::move(end.error()));
    }
    const std::size_t pair_count = conditional.arguments.size() / 2U;
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
      const BoundExpressionId condition = conditional.arguments[pair * 2U];
      const BoundExpressionId value = conditional.arguments[pair * 2U + 1U];
      const RegisterId condition_register = Home(condition);
      if (auto emitted = EmitExpression(condition, condition_register); !emitted.has_value()) {
        return emitted;
      }
      auto next = CreateLabel();
      if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
      }
      if (auto jumped = EmitJumpIf(condition_register, JumpCondition::kIfFalse, *next);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto jumped = EmitJumpIf(condition_register, JumpCondition::kIfNull, *next);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto emitted = EmitExpression(value, destination); !emitted.has_value()) {
        return emitted;
      }
      if (auto jumped = EmitJump(*end); !jumped.has_value()) {
        return jumped;
      }
      if (auto bound = BindLabel(*next); !bound.has_value()) {
        return bound;
      }
    }

    if ((conditional.arguments.size() % 2U) != 0U) {
      if (auto emitted = EmitExpression(conditional.arguments.back(), destination);
          !emitted.has_value()) {
        return emitted;
      }
    } else {
      auto null_constant = EnsureNullConstant();
      if (!null_constant.has_value()) {
        return std::unexpected(std::move(null_constant.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *null_constant,
              .output = destination,
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    return BindLabel(*end);
  }

  [[nodiscard]] std::optional<RegisterId> RetainedAggregateRegister(
      const BoundSourceLeafIdentity& identity) const noexcept {
    const auto& mapping =
        global_aggregate_query_ && global_representative_query_
            ? global_representative_register_map_
        : aggregate_expression_source_ == lowering_internal::AggregateExpressionSource::SortedRow
            ? grouped_sorted_register_map_
            : grouped_representative_register_map_;
    const auto found = std::ranges::find(mapping, identity, &AggregateLeafRegister::leaf);
    return found != mapping.end() ? std::optional{found->reg} : std::nullopt;
  }

  [[nodiscard]] std::optional<RegisterId> GroupedColumnRegister(
      BoundSourceColumnId column) const noexcept {
    const BoundExpression expression{.payload = BoundColumnExpression{.column = column}};
    const auto identity = BoundSourceLeafIdentityOf(expression, bound_select_->source_columns());
    if (!identity.has_value()) {
      std::terminate();
    }
    return RetainedAggregateRegister(*identity);
  }

  [[nodiscard]] std::optional<RegisterId> GroupedRowIdRegister(
      BoundSourceId source) const noexcept {
    return RetainedAggregateRegister(BoundSourceLeafIdentity{.source = source});
  }

  [[nodiscard]] LoweringResult<void> EmitColumnValue(const BoundColumnExpression& column,
                                                     RegisterId destination) {
    if ((grouped_aggregate_query_ || global_representative_query_) &&
        aggregate_expression_source_ != lowering_internal::AggregateExpressionSource::AccessRow) {
      const std::optional<RegisterId> source = GroupedColumnRegister(column.column);
      if (!source.has_value()) {
        return std::unexpected(InternalFailure("grouped column has no retained source register"));
      }
      return Append(CopyInstruction{.input = *source, .output = destination});
    }
    if (!source_snapshot_registers_.empty()) {
      if (column.column.value() >= source_snapshot_registers_.size() ||
          column.column.value() >= source_field_real_affinity_.size()) {
        return std::unexpected(InternalFailure("bound column has no source snapshot field"));
      }
      if (auto copied = Append(CopyInstruction{
              .input = source_snapshot_registers_[column.column.value()],
              .output = destination,
          });
          !copied.has_value()) {
        return copied;
      }
      if (source_field_real_affinity_[column.column.value()]) {
        return Append(RealAffinityInstruction{.input = destination, .output = destination});
      }
      return {};
    }
    if (column.column.value() >= source_field_real_affinity_.size() ||
        column.column.value() >= source_cursor_fields_.size() ||
        !source_cursor_fields_[column.column.value()].has_value()) {
      return std::unexpected(InternalFailure("bound column has no cursor field"));
    }
    auto binding = SourceCursor(bound_select_->source_columns()[column.column.value()].source);
    if (!binding.has_value()) {
      return std::unexpected(std::move(binding.error()));
    }
    if (auto read = Append(ReadFieldInstruction{
            .cursor = binding->cursor,
            .field = AssumeValue(source_cursor_fields_[column.column.value()]),
            .output = destination,
        });
        !read.has_value()) {
      return read;
    }
    if (source_field_real_affinity_[column.column.value()]) {
      return Append(RealAffinityInstruction{.input = destination, .output = destination});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitCursorRowIdValue(BoundSourceId source,
                                                          RegisterId destination) {
    auto binding = SourceCursor(source);
    if (!binding.has_value()) {
      return std::unexpected(std::move(binding.error()));
    }
    if (binding->rowid_field.has_value()) {
      return Append(ReadFieldInstruction{
          .cursor = binding->cursor, .field = *binding->rowid_field, .output = destination});
    }
    return Append(ReadRowIdInstruction{.cursor = binding->cursor, .output = destination});
  }

  [[nodiscard]] LoweringResult<void> EmitRowIdValue(const BoundRowIdExpression& rowid,
                                                    RegisterId destination) {
    if ((grouped_aggregate_query_ || global_representative_query_) &&
        aggregate_expression_source_ != lowering_internal::AggregateExpressionSource::AccessRow) {
      const std::optional<RegisterId> source = GroupedRowIdRegister(rowid.source);
      if (!source.has_value()) {
        return std::unexpected(InternalFailure("grouped rowid has no retained source register"));
      }
      return Append(CopyInstruction{.input = *source, .output = destination});
    }
    if (source_rowid_register_.has_value()) {
      return Append(CopyInstruction{
          .input = AssumeValue(source_rowid_register_),
          .output = destination,
      });
    }
    return EmitCursorRowIdValue(rowid.source, destination);
  }

  [[nodiscard]] LoweringResult<void> EmitUnaryValue(const BoundUnaryExpression& unary,
                                                    RegisterId destination) {
    if (auto emitted = EmitExpression(unary.operand, destination); !emitted.has_value()) {
      return emitted;
    }
    if (unary.operation == BoundUnaryOperation::kPositive) {
      return {};
    }
    return Append(UnaryInstruction{.operation = ToUnaryOperation(unary.operation),
                                   .input = destination,
                                   .output = destination});
  }

  [[nodiscard]] LoweringResult<void> EmitBinaryValue(const BoundBinaryExpression& binary,
                                                     RegisterId destination) {
    if (const std::optional<BoundExpressionId> selected = SimplifiedLogicalOperand(binary);
        selected.has_value()) {
      if (auto emitted = EmitExpression(*selected, destination); !emitted.has_value()) {
        return emitted;
      }
      return Append(BinaryInstruction{
          .operation = ToBinaryOperation(binary.operation),
          .left = destination,
          .right = destination,
          .output = destination,
      });
    }
    const RegisterId left = Home(binary.left);
    const RegisterId right = Home(binary.right);
    if (auto emitted = EmitExpression(binary.left, left); !emitted.has_value()) {
      return emitted;
    }
    if (auto emitted = EmitExpression(binary.right, right); !emitted.has_value()) {
      return emitted;
    }
    return Append(BinaryInstruction{
        .operation = ToBinaryOperation(binary.operation),
        .left = left,
        .right = right,
        .output = destination,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitComparisonValue(
      const BoundComparisonExpression& comparison, RegisterId destination) {
    const RegisterId left = Home(comparison.left);
    const RegisterId right = Home(comparison.right);
    if (auto emitted = EmitExpression(comparison.left, left); !emitted.has_value()) {
      return emitted;
    }
    if (auto emitted = EmitExpression(comparison.right, right); !emitted.has_value()) {
      return emitted;
    }
    auto collation = CollationSymbol(comparison.collation);
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    return Append(CompareInstruction{
        .comparison = comparison.comparison,
        .affinity = comparison.affinity,
        .collation = *collation,
        .left = left,
        .right = right,
        .output = destination,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitScalarCallValue(BoundExpressionId id,
                                                         const BoundScalarCallExpression& call,
                                                         RegisterId destination) {
    const std::optional<RegisterId>& argument_block = call_argument_blocks_[id.value()];
    if (!argument_block.has_value()) {
      return std::unexpected(InternalFailure("scalar call has no argument block"));
    }
    const RegisterId first = *argument_block;
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
      if (auto emitted = EmitExpression(
              call.arguments[index], RegisterId{first.value() + static_cast<std::uint32_t>(index)});
          !emitted.has_value()) {
        return emitted;
      }
    }
    auto function = FunctionSymbol(call.function);
    auto collation = CollationSymbol(call.collation);
    if (!function.has_value()) {
      return std::unexpected(std::move(function.error()));
    }
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    return Append(CallScalarInstruction{
        .function = *function,
        .collation = *collation,
        .first_argument = first,
        .argument_count = static_cast<std::uint32_t>(call.arguments.size()),
        .output = destination,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitExpression(BoundExpressionId id,
                                                    RegisterId destination) override {
    if (id.value() >= expressions_.size()) {
      return std::unexpected(InternalFailure("bound expression ID is out of range"));
    }
    if (grouped_aggregate_query_ &&
        aggregate_expression_source_ ==
            lowering_internal::AggregateExpressionSource::Representative &&
        id.value() < grouped_key_expression_registers_.size() &&
        grouped_key_expression_registers_[id.value()].has_value()) {
      return Append(CopyInstruction{
          .input = AssumeValue(grouped_key_expression_registers_[id.value()]),
          .output = destination,
      });
    }
    const BoundExpression& expression = Expression(id);

    if (std::holds_alternative<BoundLiteralExpression>(expression.payload)) {
      const std::optional<ConstantId>& constant = literal_constants_[id.value()];
      if (!constant.has_value()) {
        return std::unexpected(InternalFailure("bound literal has no bytecode constant"));
      }
      return Append(LoadConstantInstruction{
          .constant = AssumeValue(constant),
          .output = destination,
      });
    }
    if (const auto* parameter = std::get_if<BoundParameterExpression>(&expression.payload);
        parameter != nullptr) {
      return Append(LoadParameterInstruction{
          .parameter = ParameterId{parameter->parameter.value()},
          .output = destination,
      });
    }
    if (const auto* column = std::get_if<BoundColumnExpression>(&expression.payload);
        column != nullptr) {
      return EmitColumnValue(*column, destination);
    }
    if (const auto* rowid = std::get_if<BoundRowIdExpression>(&expression.payload);
        rowid != nullptr) {
      return EmitRowIdValue(*rowid, destination);
    }
    if (const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
        unary != nullptr) {
      return EmitUnaryValue(*unary, destination);
    }
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
        binary != nullptr) {
      return EmitBinaryValue(*binary, destination);
    }
    if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.payload);
        comparison != nullptr) {
      return EmitComparisonValue(*comparison, destination);
    }
    if (const auto* truth_test = std::get_if<BoundTruthTestExpression>(&expression.payload);
        truth_test != nullptr) {
      return EmitTruthTest(*truth_test, destination);
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      return EmitExpression(alias->target, destination);
    }
    if (const auto* call = std::get_if<BoundScalarCallExpression>(&expression.payload);
        call != nullptr) {
      return EmitScalarCallValue(id, *call, destination);
    }
    if (const auto* call = std::get_if<BoundAggregateCallExpression>(&expression.payload);
        call != nullptr) {
      if ((!global_aggregate_query_ && !grouped_aggregate_query_) ||
          call->invocation.value() >= aggregate_result_registers_.size() ||
          !aggregate_result_registers_[call->invocation.value()].has_value()) {
        return std::unexpected(InternalFailure("aggregate call has no finalized register"));
      }
      return Append(CopyInstruction{
          .input = AssumeValue(aggregate_result_registers_[call->invocation.value()]),
          .output = destination,
      });
    }
    if (const auto* coalesce = std::get_if<BoundCoalesceExpression>(&expression.payload);
        coalesce != nullptr) {
      return EmitCoalesce(*coalesce, destination);
    }
    if (const auto* conditional = std::get_if<BoundConditionalExpression>(&expression.payload);
        conditional != nullptr) {
      return EmitConditional(*conditional, destination);
    }
    if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&expression.payload);
        likelihood != nullptr) {
      return EmitExpression(likelihood->operand, destination);
    }
    if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
        collate != nullptr) {
      return EmitExpression(collate->operand, destination);
    }
    if (const auto* subquery = std::get_if<BoundSubqueryExpression>(&expression.payload);
        subquery != nullptr) {
      return EmitSubqueryValue(subquery->query, destination);
    }
    return std::unexpected(InternalFailure("bound expression kind is unsupported"));
  }

  [[nodiscard]] LoweringResult<void> EmitAggregateExpression(
      BoundExpressionId id, RegisterId destination,
      lowering_internal::AggregateExpressionSource source) override {
    const lowering_internal::AggregateExpressionSource previous =
        std::exchange(aggregate_expression_source_, source);
    LoweringResult<void> emitted = EmitExpression(id, destination);
    aggregate_expression_source_ = previous;
    return emitted;
  }

  [[nodiscard]] LoweringResult<void> OpenGroupedAggregateInvocations() override {
    auto input = MakeAggregateModifierInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    return lowering_internal::EmitAggregateModifierOpen(*input);
  }

  [[nodiscard]] LoweringResult<void> StepGroupedAggregateInvocations(
      lowering_internal::AggregateExpressionSource source, bool first_row) override {
    auto input = MakeAggregateModifierInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    return lowering_internal::EmitAggregateModifierSteps(
        *input, *this, source,
        first_row ? lowering_internal::AggregateRowKind::FirstGroupRow
                  : lowering_internal::AggregateRowKind::SubsequentGroupRow);
  }

  [[nodiscard]] LoweringResult<void> FinalizeGroupedAggregateInvocations() override {
    auto input = MakeAggregateModifierInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    return lowering_internal::EmitAggregateModifierFinalization(*input);
  }

  [[nodiscard]] LoweringResult<void> ResetGroupedAggregateInvocations() override {
    auto input = MakeAggregateModifierInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    return lowering_internal::EmitAggregateModifierReset(*input);
  }

  [[nodiscard]] LoweringResult<void> CloseGroupedAggregateInvocations() override {
    auto input = MakeAggregateModifierInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    return lowering_internal::EmitAggregateModifierClose(*input);
  }

  [[nodiscard]] LoweringResult<void> EmitPredicate(BoundExpressionId predicate,
                                                   Label false_target) {
    const RegisterId value = Home(predicate);
    if (auto emitted = EmitExpression(predicate, value); !emitted.has_value()) {
      return emitted;
    }
    if (auto jumped = EmitJumpIf(value, JumpCondition::kIfFalse, false_target);
        !jumped.has_value()) {
      return jumped;
    }
    return EmitJumpIf(value, JumpCondition::kIfNull, false_target);
  }

  [[nodiscard]] LoweringResult<void> EmitLimitInitialization(
      std::optional<Label> completion) override {
    if (limit_ == nullptr || limit_initialized_) {
      return {};
    }
    if (!completion.has_value() || !limit_register_.has_value()) {
      return std::unexpected(InternalFailure("LIMIT has no completion or counter register"));
    }
    if (auto emitted = EmitExpression(limit_->limit, AssumeValue(limit_register_));
        !emitted.has_value()) {
      return emitted;
    }
    if (auto converted = Append(MustBeIntegerInstruction{
            .input = AssumeValue(limit_register_),
            .output = AssumeValue(limit_register_),
        });
        !converted.has_value()) {
      return converted;
    }
    if (auto jumped =
            EmitJumpIf(AssumeValue(limit_register_), JumpCondition::kIfFalse, *completion);
        !jumped.has_value()) {
      return jumped;
    }

    if (limit_->offset.has_value()) {
      if (!offset_register_.has_value() || !zero_register_.has_value() ||
          !one_register_.has_value() || !comparison_register_.has_value()) {
        return std::unexpected(InternalFailure("OFFSET has no control registers"));
      }
      if (auto emitted = EmitExpression(*limit_->offset, AssumeValue(offset_register_));
          !emitted.has_value()) {
        return emitted;
      }
      if (auto converted = Append(MustBeIntegerInstruction{
              .input = AssumeValue(offset_register_),
              .output = AssumeValue(offset_register_),
          });
          !converted.has_value()) {
        return converted;
      }
    }

    if (zero_register_.has_value()) {
      auto zero = EnsureZeroConstant();
      if (!zero.has_value()) {
        return std::unexpected(std::move(zero.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *zero,
              .output = AssumeValue(zero_register_),
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (one_register_.has_value()) {
      auto one = EnsureOneConstant();
      if (!one.has_value()) {
        return std::unexpected(std::move(one.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *one,
              .output = AssumeValue(one_register_),
          });
          !loaded.has_value()) {
        return loaded;
      }
    }

    if (limit_->offset.has_value()) {
      auto binary = EnsureBinarySymbol();
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      if (auto compared = Append(CompareInstruction{
              .comparison = SqlComparison::kLess,
              .affinity = TypeAffinity::kNumeric,
              .collation = *binary,
              .left = AssumeValue(offset_register_),
              .right = AssumeValue(zero_register_),
              .output = AssumeValue(comparison_register_),
          });
          !compared.has_value()) {
        return compared;
      }
      auto nonnegative = CreateLabel();
      if (!nonnegative.has_value()) {
        return std::unexpected(std::move(nonnegative.error()));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(comparison_register_), JumpCondition::kIfFalse, *nonnegative);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto copied = Append(CopyInstruction{
              .input = AssumeValue(zero_register_),
              .output = AssumeValue(offset_register_),
          });
          !copied.has_value()) {
        return copied;
      }
      if (auto bound = BindLabel(*nonnegative); !bound.has_value()) {
        return bound;
      }
    }

    if (negative_limit_register_.has_value()) {
      if (!zero_register_.has_value()) {
        return std::unexpected(InternalFailure("negative LIMIT has no zero register"));
      }
      auto binary = EnsureBinarySymbol();
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      if (auto compared = Append(CompareInstruction{
              .comparison = SqlComparison::kLess,
              .affinity = TypeAffinity::kNumeric,
              .collation = *binary,
              .left = AssumeValue(limit_register_),
              .right = AssumeValue(zero_register_),
              .output = AssumeValue(negative_limit_register_),
          });
          !compared.has_value()) {
        return compared;
      }
    }
    limit_initialized_ = true;
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGuards(std::optional<Label> completion) {
    if (guard_ != nullptr) {
      if (!completion.has_value()) {
        return std::unexpected(InternalFailure("physical guard has no completion label"));
      }
      for (const BoundExpressionId predicate : guard_->predicates) {
        if (auto emitted = EmitPredicate(predicate, *completion); !emitted.has_value()) {
          return emitted;
        }
      }
    }
    if (grouped_aggregate_query_ && filter_ != nullptr && !filter_->guards.empty()) {
      if (!completion.has_value()) {
        return std::unexpected(InternalFailure("grouped guard has no completion label"));
      }
      for (const BoundExpressionId predicate : filter_->guards) {
        if (auto emitted = EmitPredicate(predicate, *completion); !emitted.has_value()) {
          return emitted;
        }
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitFilters(Label rejected) override {
    if (filter_ == nullptr) {
      return {};
    }
    for (const BoundExpressionId predicate : filter_->predicates) {
      if (auto emitted = EmitPredicate(predicate, rejected); !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateGuards(Label empty_input) override {
    if (filter_ == nullptr) {
      return {};
    }
    for (const BoundExpressionId predicate : filter_->guards) {
      if (auto emitted = EmitPredicate(predicate, empty_input); !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> InitializeGlobalRepresentatives() {
    if (!global_representative_query_) {
      return {};
    }
    auto null_constant = EnsureNullConstant();
    if (!null_constant.has_value()) {
      return std::unexpected(std::move(null_constant.error()));
    }
    for (const RegisterId representative : global_representative_registers_) {
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *null_constant,
              .output = representative,
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (HasAggregateRepresentativeController()) {
      if (!global_representative_replace_register_.has_value()) {
        return std::unexpected(
            InternalFailure("global representative replacement register is unavailable"));
      }
      if (!global_representative_replace_constant_.has_value()) {
        auto replacement = AddConstant(SqlValue::Integer(
            static_cast<std::int64_t>(BytecodeAggregateStepEffect::kReplaceRepresentative)));
        if (!replacement.has_value()) {
          return std::unexpected(std::move(replacement.error()));
        }
        global_representative_replace_constant_ = *replacement;
      }
      if (global_filtered_controller_defaults_) {
        auto zero = EnsureZeroConstant();
        auto one = EnsureOneConstant();
        if (!zero.has_value()) {
          return std::unexpected(std::move(zero.error()));
        }
        if (!one.has_value()) {
          return std::unexpected(std::move(one.error()));
        }
      }
      return Append(LoadConstantInstruction{
          .constant = AssumeValue(global_representative_replace_constant_),
          .output = AssumeValue(global_representative_replace_register_),
      });
    }
    if (!global_has_representative_row_register_.has_value()) {
      return std::unexpected(InternalFailure("global representative row flag is unavailable"));
    }
    auto zero = EnsureZeroConstant();
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    return Append(LoadConstantInstruction{
        .constant = *zero,
        .output = AssumeValue(global_has_representative_row_register_),
    });
  }

  [[nodiscard]] LoweringResult<void> CaptureGlobalRepresentatives() {
    for (std::size_t index = 0; index < global_representative_leaves_.size(); ++index) {
      if (auto emitted = EmitAggregateExpression(
              global_representative_leaves_[index], global_representative_registers_[index],
              lowering_internal::AggregateExpressionSource::AccessRow);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> OpenGlobalAggregates() override {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("global aggregate program is unavailable"));
    }
    if (auto initialized = InitializeGlobalRepresentatives(); !initialized.has_value()) {
      return initialized;
    }
    auto input = MakeAggregateModifierInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    return lowering_internal::EmitAggregateModifierOpen(*input);
  }

  [[nodiscard]] LoweringResult<void> StepGlobalAggregates() override {
    auto input = MakeAggregateModifierInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    if (auto stepped = lowering_internal::EmitAggregateModifierSteps(
            *input, *this, lowering_internal::AggregateExpressionSource::AccessRow,
            lowering_internal::AggregateRowKind::Global);
        !stepped.has_value()) {
      return stepped;
    }
    return UpdateGlobalRepresentative();
  }

  [[nodiscard]] LoweringResult<void> StepIndexedMinMaxAggregate(
      BoundAggregateInvocationId invocation_id, RegisterId argument) override {
    if (invocation_id.value() >= aggregate_invocations_.size() ||
        invocation_id.value() >= aggregate_descriptors_.size() ||
        invocation_id.value() >= aggregate_argument_blocks_.size() ||
        !aggregate_descriptors_[invocation_id.value()].has_value() ||
        !aggregate_argument_blocks_[invocation_id.value()].has_value() ||
        AssumeValue(aggregate_argument_blocks_[invocation_id.value()]) != argument) {
      return std::unexpected(InternalFailure("indexed MIN/MAX step metadata is incomplete"));
    }
    const BoundAggregateInvocation& invocation = aggregate_invocations_[invocation_id.value()];
    if (invocation.function.value() >= aggregate_functions_.size() ||
        aggregate_functions_[invocation.function.value()].extremum ==
            BoundAggregateExtremumKind::kNone) {
      return std::unexpected(InternalFailure("indexed MIN/MAX step function is invalid"));
    }
    if (auto stepped = Append(StepAggregateInstruction{
            .aggregate = AssumeValue(aggregate_descriptors_[invocation_id.value()]),
            .first_argument = argument,
            .effect_output = global_representative_query_ ? global_representative_effect_register_
                                                          : std::nullopt,
        });
        !stepped.has_value()) {
      return stepped;
    }
    return UpdateGlobalRepresentative();
  }

  [[nodiscard]] LoweringResult<void> UpdateGlobalRepresentative() {
    if (!global_representative_query_) {
      return {};
    }
    auto keep = CreateLabel();
    if (!keep.has_value()) {
      return std::unexpected(std::move(keep.error()));
    }
    if (HasAggregateRepresentativeController()) {
      if (!global_representative_effect_register_.has_value() ||
          !global_representative_replace_register_.has_value() ||
          !global_representative_comparison_register_.has_value()) {
        return std::unexpected(
            InternalFailure("global representative effect metadata is unavailable"));
      }
      auto binary = EnsureBinarySymbol();
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      if (auto compared = Append(CompareInstruction{
              .comparison = SqlComparison::kEqual,
              .affinity = TypeAffinity::kNone,
              .collation = *binary,
              .left = AssumeValue(global_representative_effect_register_),
              .right = AssumeValue(global_representative_replace_register_),
              .output = AssumeValue(global_representative_comparison_register_),
          });
          !compared.has_value()) {
        return compared;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(global_representative_comparison_register_),
                                   JumpCondition::kIfFalse, *keep);
          !jumped.has_value()) {
        return jumped;
      }
    } else {
      if (!global_has_representative_row_register_.has_value()) {
        return std::unexpected(InternalFailure("global representative row flag is unavailable"));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(global_has_representative_row_register_),
                                   JumpCondition::kIfTrue, *keep);
          !jumped.has_value()) {
        return jumped;
      }
    }
    if (auto captured = CaptureGlobalRepresentatives(); !captured.has_value()) {
      return captured;
    }
    if (!HasAggregateRepresentativeController()) {
      auto one = EnsureOneConstant();
      if (!one.has_value()) {
        return std::unexpected(std::move(one.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *one,
              .output = AssumeValue(global_has_representative_row_register_),
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (auto bound = BindLabel(*keep); !bound.has_value()) {
      return bound;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> FinalizeAndCloseGlobalAggregates() {
    auto input = MakeAggregateModifierInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    if (auto finalized = lowering_internal::EmitAggregateModifierFinalization(*input);
        !finalized.has_value()) {
      return finalized;
    }
    return lowering_internal::EmitAggregateModifierClose(*input);
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateOffset(Label rejected) {
    if (limit_ == nullptr || !limit_->offset.has_value()) {
      return {};
    }
    if (!offset_register_.has_value() || !one_register_.has_value()) {
      return std::unexpected(InternalFailure("global aggregate OFFSET has no registers"));
    }
    auto project = CreateLabel();
    if (!project.has_value()) {
      return std::unexpected(std::move(project.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(offset_register_), JumpCondition::kIfFalse, *project);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto decremented = Append(BinaryInstruction{
            .operation = BinaryOperation::kSubtract,
            .left = AssumeValue(offset_register_),
            .right = AssumeValue(one_register_),
            .output = AssumeValue(offset_register_),
        });
        !decremented.has_value()) {
      return decremented;
    }
    if (auto jumped = EmitJump(rejected); !jumped.has_value()) {
      return jumped;
    }
    return BindLabel(*project);
  }

  [[nodiscard]] LoweringResult<void> EmitAggregateCoreProjection() {
    if (aggregate_core_ == nullptr ||
        aggregate_core_->result_columns.size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("aggregate core projection width is invalid"));
    }
    for (std::size_t index = 0; index < aggregate_core_->result_columns.size(); ++index) {
      if (auto emitted = EmitProjectionValue(
              aggregate_core_->result_columns[index].expression,
              RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)},
              active_aggregate_output_.has_value() ? active_aggregate_output_->mode
                                                   : SelectOutputMode::Streaming);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitActiveAggregateOutput(Label rejected, Label stopped) {
    if (!active_aggregate_output_.has_value() || active_aggregate_output_->core == nullptr) {
      return std::unexpected(InternalFailure("aggregate output context is unavailable"));
    }
    const SelectOutputContext& output = *active_aggregate_output_;
    const StreamingCorePlan& core = *output.core;
    switch (output.mode) {
      case SelectOutputMode::Streaming:
        if (!core.relation.has_value()) {
          if (auto offset = EmitOffset(rejected); !offset.has_value()) {
            return offset;
          }
        }
        if (auto projected = EmitAggregateCoreProjection(); !projected.has_value()) {
          return projected;
        }
        if (auto membership = EmitStreamingMembership(core, rejected); !membership.has_value()) {
          return membership;
        }
        return EmitStreamingResult(rejected, stopped, core.relation.has_value());
      case SelectOutputMode::Ordered:
        if (output.ordered_layout == nullptr || !output.ordered_record_first.has_value()) {
          return std::unexpected(InternalFailure("ordered aggregate output is incomplete"));
        }
        if (OrderedLayoutUsesInputFields(*output.ordered_layout)) {
          if (auto projected = EmitAggregateCoreProjection(); !projected.has_value()) {
            return projected;
          }
          if (auto membership = EmitStreamingMembership(core, rejected); !membership.has_value()) {
            return membership;
          }
        }
        return EmitOrderedCandidate(*output.ordered_layout, output.ordered_sorter,
                                    output.ordered_top_n, output.ordered_full_key,
                                    *output.ordered_record_first, rejected);
      case SelectOutputMode::Materialized:
        if (!output.materialized_target.has_value()) {
          return std::unexpected(InternalFailure("materialized aggregate target is unavailable"));
        }
        if (auto projected = EmitAggregateCoreProjection(); !projected.has_value()) {
          return projected;
        }
        return EmitMaterializedSetInsert(*output.materialized_target, output.materialized_count,
                                         core.relation);
      case SelectOutputMode::DemandOne:
        if (auto projected = EmitAggregateCoreProjection(); !projected.has_value()) {
          return projected;
        }
        if (auto result = Append(ResultRowInstruction{
                .first = result_block_first_,
                .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
            });
            !result.has_value()) {
          return result;
        }
        return EmitJump(stopped);
    }
    return std::unexpected(InternalFailure("aggregate output mode is invalid"));
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateProjection() {
    if (auto projected = EmitAggregateCoreProjection(); !projected.has_value()) {
      return projected;
    }
    return Append(ResultRowInstruction{
        .first = result_block_first_,
        .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateCompletion() override {
    if (auto finalized = FinalizeAndCloseGlobalAggregates(); !finalized.has_value()) {
      return finalized;
    }
    const lowering_internal::AggregateExpressionSource previous = std::exchange(
        aggregate_expression_source_, lowering_internal::AggregateExpressionSource::Representative);
    const auto restore = [this, previous](LoweringResult<void> result) {
      aggregate_expression_source_ = previous;
      return result;
    };
    if (active_aggregate_output_.has_value()) {
      if (!aggregate_core_completion_label_.has_value() ||
          !aggregate_core_stopped_label_.has_value()) {
        return restore(
            std::unexpected(InternalFailure("active aggregate output labels are unavailable")));
      }
      const Label completed = *aggregate_core_completion_label_;
      const Label stopped = *aggregate_core_stopped_label_;
      const SelectOutputMode mode = active_aggregate_output_->mode;
      if (having_ != nullptr) {
        if (auto having = EmitPredicate(having_->predicate, completed); !having.has_value()) {
          return restore(std::move(having));
        }
      }
      if (auto output = EmitActiveAggregateOutput(completed, stopped); !output.has_value()) {
        return restore(std::move(output));
      }
      if (mode == SelectOutputMode::DemandOne) {
        return restore({});
      }
      return restore(EmitJump(completed));
    }
    const bool has_rejection =
        having_ != nullptr || (limit_ != nullptr && limit_->offset.has_value());
    std::optional<Label> rejected;
    if (has_rejection) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return restore(std::unexpected(std::move(label.error())));
      }
      rejected = *label;
    }
    if (having_ != nullptr) {
      if (auto having = EmitPredicate(having_->predicate, AssumeValue(rejected));
          !having.has_value()) {
        return restore(std::move(having));
      }
    }
    if (rejected.has_value()) {
      if (auto offset = EmitGlobalAggregateOffset(*rejected); !offset.has_value()) {
        return restore(std::move(offset));
      }
    }
    if (auto projection = EmitGlobalAggregateProjection(); !projection.has_value()) {
      return restore(std::move(projection));
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return restore(std::move(halted));
    }
    if (rejected.has_value()) {
      if (auto bound = BindLabel(*rejected); !bound.has_value()) {
        return restore(std::move(bound));
      }
      return restore(Append(HaltInstruction{}));
    }
    aggregate_expression_source_ = previous;
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateEarlyStopCompletion() override {
    if (active_aggregate_output_.has_value()) {
      if (!aggregate_core_stopped_label_.has_value()) {
        return std::unexpected(InternalFailure("active aggregate early-stop label is unavailable"));
      }
      return EmitJump(*aggregate_core_stopped_label_);
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitGroupedAggregateProjection() {
    if (auto projected = EmitAggregateCoreProjection(); !projected.has_value()) {
      return projected;
    }
    return Append(ResultRowInstruction{
        .first = result_block_first_,
        .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitCompletedGroupedAggregate(
      lowering_internal::GroupedCompletionTargets targets) override {
    const lowering_internal::AggregateExpressionSource previous = std::exchange(
        aggregate_expression_source_, lowering_internal::AggregateExpressionSource::Representative);
    const bool previous_output_phase = std::exchange(grouped_output_phase_, true);
    const auto restore = [this, previous, previous_output_phase](LoweringResult<void> result) {
      aggregate_expression_source_ = previous;
      grouped_output_phase_ = previous_output_phase;
      return result;
    };

    if (having_ != nullptr) {
      if (auto having = EmitPredicate(having_->predicate, targets.skipped); !having.has_value()) {
        return restore(std::move(having));
      }
    }
    if (active_aggregate_output_.has_value()) {
      return restore(EmitActiveAggregateOutput(targets.skipped, targets.stopped));
    }
    if (HasGroupedOuterOrder()) {
      return restore(EmitOrderedCandidate(targets.skipped));
    }
    if (limit_ != nullptr && limit_->offset.has_value()) {
      if (auto offset = EmitOffset(targets.skipped); !offset.has_value()) {
        return restore(std::move(offset));
      }
    }
    if (auto projection = EmitGroupedAggregateProjection(); !projection.has_value()) {
      return restore(std::move(projection));
    }
    if (limit_ != nullptr) {
      if (!negative_limit_register_.has_value() || !limit_register_.has_value() ||
          !one_register_.has_value()) {
        return restore(
            std::unexpected(InternalFailure("grouped aggregate LIMIT counters are unavailable")));
      }
      auto unlimited = CreateLabel();
      if (!unlimited.has_value()) {
        return restore(std::unexpected(std::move(unlimited.error())));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(negative_limit_register_), JumpCondition::kIfTrue, *unlimited);
          !jumped.has_value()) {
        return restore(std::move(jumped));
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(limit_register_),
              .right = AssumeValue(one_register_),
              .output = AssumeValue(limit_register_),
          });
          !decremented.has_value()) {
        return restore(std::move(decremented));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(limit_register_), JumpCondition::kIfFalse, targets.stopped);
          !jumped.has_value()) {
        return restore(std::move(jumped));
      }
      if (auto bound = BindLabel(*unlimited); !bound.has_value()) {
        return restore(std::move(bound));
      }
    }
    aggregate_expression_source_ = previous;
    grouped_output_phase_ = previous_output_phase;
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGroupedOutputCompletion() override {
    if (active_aggregate_output_.has_value()) {
      if (!aggregate_core_completion_label_.has_value()) {
        return std::unexpected(InternalFailure("active grouped completion label is unavailable"));
      }
      return EmitJump(*aggregate_core_completion_label_);
    }
    if (HasGroupedOuterOrder()) {
      return EmitOrdinaryReadCompletion();
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitGroupedEarlyStopCompletion(
      bool source_row_active) override {
    if (source_row_active &&
        (active_aggregate_output_.has_value() ||
         bound_select_->query().destination != BoundQueryDestination::kClient)) {
      if (auto closed = EmitCloseReadCursors(cursor_, index_cursor_); !closed.has_value()) {
        return closed;
      }
    }
    if (active_aggregate_output_.has_value()) {
      if (!aggregate_core_stopped_label_.has_value()) {
        return std::unexpected(InternalFailure("active grouped early-stop label is unavailable"));
      }
      const Label stopped = *aggregate_core_stopped_label_;
      return EmitJump(stopped);
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctGuards(std::optional<Label> completion) {
    if (filter_ == nullptr || filter_->guards.empty()) {
      return {};
    }
    if (!completion.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT guard has no completion label"));
    }
    for (const BoundExpressionId predicate : filter_->guards) {
      if (auto emitted = EmitPredicate(predicate, *completion); !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitOffset(Label skipped, bool ordered_drain = false) {
    if (grouped_aggregate_query_ && !grouped_output_phase_ && !ordered_drain) {
      return {};
    }
    if (limit_ == nullptr || !limit_->offset.has_value()) {
      return {};
    }
    if (sort_ != nullptr && !sort_->input_order_satisfied && !ordered_drain) {
      return {};
    }
    if (!offset_register_.has_value() || !one_register_.has_value()) {
      return std::unexpected(InternalFailure("OFFSET has no row-loop registers"));
    }
    auto project = CreateLabel();
    if (!project.has_value()) {
      return std::unexpected(std::move(project.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(offset_register_), JumpCondition::kIfFalse, *project);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto decremented = Append(BinaryInstruction{
            .operation = BinaryOperation::kSubtract,
            .left = AssumeValue(offset_register_),
            .right = AssumeValue(one_register_),
            .output = AssumeValue(offset_register_),
        });
        !decremented.has_value()) {
      return decremented;
    }
    if (auto jumped = EmitJump(skipped); !jumped.has_value()) {
      return jumped;
    }
    return BindLabel(*project);
  }

  [[nodiscard]] LoweringResult<void> EmitOrderingKeys() {
    if (sort_ == nullptr || !sorter_record_first_.has_value()) {
      return std::unexpected(InternalFailure("ordering keys have no record registers"));
    }
    const RegisterId record_first = AssumeValue(sorter_record_first_);
    for (std::size_t index = 0; index < sort_->terms.size(); ++index) {
      const BoundExpressionId expression = sort_->terms[index].expression;
      if (auto emitted = EmitExpression(expression, Home(expression)); !emitted.has_value()) {
        return emitted;
      }
      if (auto copied = Append(CopyInstruction{
              .input = Home(expression),
              .output = RegisterId{record_first.value() + static_cast<std::uint32_t>(index)},
          });
          !copied.has_value()) {
        return copied;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitOrderingPayload() {
    if (sort_ == nullptr || !sorter_record_first_.has_value()) {
      return std::unexpected(InternalFailure("ordering payload has no record registers"));
    }
    const RegisterId record_first = AssumeValue(sorter_record_first_);
    const auto key_count = static_cast<std::uint32_t>(sort_->terms.size());
    for (std::size_t index = 0; index < sort_->payload_expressions.size(); ++index) {
      const BoundExpressionId expression = sort_->payload_expressions[index];
      if (auto emitted = EmitExpression(expression, Home(expression)); !emitted.has_value()) {
        return emitted;
      }
      if (auto copied = Append(CopyInstruction{
              .input = Home(expression),
              .output =
                  RegisterId{record_first.value() + key_count + static_cast<std::uint32_t>(index)},
          });
          !copied.has_value()) {
        return copied;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedCandidate(Label rejected) {
    if (sort_ == nullptr || !sorter_record_first_.has_value()) {
      return std::unexpected(InternalFailure("ordered candidate has no record registers"));
    }
    const RegisterId record_first = AssumeValue(sorter_record_first_);
    const auto key_count = static_cast<std::uint32_t>(sort_->terms.size());
    const auto value_count =
        static_cast<std::uint32_t>(sort_->terms.size() + sort_->payload_expressions.size());

    if (sort_->schedule == OrderEvaluationSchedule::kPayloadThenKeys) {
      if (auto payload = EmitOrderingPayload(); !payload.has_value()) {
        return payload;
      }
      if (auto keys = EmitOrderingKeys(); !keys.has_value()) {
        return keys;
      }
    } else {
      if (auto keys = EmitOrderingKeys(); !keys.has_value()) {
        return keys;
      }
      if (ordering_runtime_ == OrderingRuntime::kTopN) {
        if (!top_n_.has_value()) {
          return std::unexpected(InternalFailure("top-N candidate has no descriptor"));
        }
        auto checked =
            ConvertProgramResult(AssumeValue(builder_).EmitCheckTopN(
                                     AssumeValue(top_n_), record_first, key_count, rejected),
                                 "unable to emit top-N admission");
        if (!checked.has_value()) {
          return std::unexpected(std::move(checked.error()));
        }
      }
      if (auto payload = EmitOrderingPayload(); !payload.has_value()) {
        return payload;
      }
    }

    if (ordering_runtime_ == OrderingRuntime::kTopN) {
      return Append(InsertTopNInstruction{
          .top_n = AssumeValue(top_n_),
          .first_value = record_first,
          .value_count = value_count,
      });
    }
    if (ordering_runtime_ != OrderingRuntime::kSorter || !sorter_.has_value()) {
      return std::unexpected(InternalFailure("ordered candidate has no active capability"));
    }
    return Append(InsertSorterInstruction{
        .sorter = AssumeValue(sorter_),
        .first_value = record_first,
        .value_count = value_count,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitCloseReadCursors(std::optional<CursorId> cursor,
                                                          std::optional<CursorId> index_cursor) {
    if (cursor == index_cursor) {
      index_cursor.reset();
    }
    for (const std::optional<CursorId> open : {index_cursor, cursor}) {
      if (open.has_value()) {
        if (auto closed = Append(CloseCursorInstruction{.cursor = *open}); !closed.has_value()) {
          return closed;
        }
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitReadCompletion(std::optional<CursorId> cursor = {},
                                                        std::optional<CursorId> index_cursor = {}) {
    if ((grouped_aggregate_query_ && active_aggregate_output_.has_value()) ||
        bound_select_->query().destination != BoundQueryDestination::kClient) {
      if (auto closed = EmitCloseReadCursors(cursor, index_cursor); !closed.has_value()) {
        return closed;
      }
    }
    if (grouped_aggregate_query_) {
      auto input = MakeGroupedAggregateInput();
      if (!input.has_value()) {
        return std::unexpected(std::move(input.error()));
      }
      return lowering_internal::EmitGroupedAggregateCompletion(*input, *this);
    }
    return EmitOrdinaryReadCompletion();
  }

  [[nodiscard]] LoweringResult<void> EmitOrdinaryReadCompletion() {
    if (sort_ == nullptr) {
      return Append(HaltInstruction{});
    }
    if (output_ == nullptr ||
        sort_->output_fields.size() != bound_select_->result_columns().size() ||
        ordering_runtime_ == OrderingRuntime::kNone) {
      return std::unexpected(InternalFailure("ordered completion has invalid output metadata"));
    }

    auto empty = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    std::optional<Label> positioned_completion;
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
    }
    ProgramResult<InstructionAddress> rewind_result =
        ordering_runtime_ == OrderingRuntime::kSorter
            ? AssumeValue(builder_).EmitRewindSorter(AssumeValue(sorter_), *empty)
            : AssumeValue(builder_).EmitRewindTopN(AssumeValue(top_n_), *empty);
    auto rewound = ConvertProgramResult(std::move(rewind_result), "unable to emit ordering rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (limit_ != nullptr) {
      if (auto offset = EmitOffset(*advance, true); !offset.has_value()) {
        return offset;
      }
    }
    for (std::size_t index = 0; index < sort_->output_fields.size(); ++index) {
      const SortOutputField& output = sort_->output_fields[index];
      const std::uint32_t field =
          output.kind == SortOutputFieldKind::kKey
              ? output.field_index
              : static_cast<std::uint32_t>(sort_->terms.size()) + output.field_index;
      const Instruction read =
          ordering_runtime_ == OrderingRuntime::kSorter
              ? Instruction{ReadSorterFieldInstruction{
                    .sorter = AssumeValue(sorter_),
                    .field = field,
                    .output =
                        RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)},
                }}
              : Instruction{ReadTopNFieldInstruction{
                    .top_n = AssumeValue(top_n_),
                    .field = field,
                    .output =
                        RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)},
                }};
      if (auto read_result = Append(read); !read_result.has_value()) {
        return read_result;
      }
    }
    if (auto result = Append(ResultRowInstruction{
            .first = result_block_first_,
            .count = static_cast<std::uint32_t>(sort_->output_fields.size()),
        });
        !result.has_value()) {
      return result;
    }
    if (limit_ != nullptr) {
      if (!negative_limit_register_.has_value() || !limit_register_.has_value() ||
          !one_register_.has_value()) {
        return std::unexpected(InternalFailure("ordered LIMIT drain has no counters"));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(negative_limit_register_), JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(limit_register_),
              .right = AssumeValue(one_register_),
              .output = AssumeValue(limit_register_),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(limit_register_), JumpCondition::kIfFalse,
                                   AssumeValue(positioned_completion));
          !jumped.has_value()) {
        return jumped;
      }
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    ProgramResult<InstructionAddress> next_result =
        ordering_runtime_ == OrderingRuntime::kSorter
            ? AssumeValue(builder_).EmitNextSorter(AssumeValue(sorter_), *row)
            : AssumeValue(builder_).EmitNextTopN(AssumeValue(top_n_), *row);
    auto next = ConvertProgramResult(std::move(next_result), "unable to emit ordering advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    const Instruction close =
        ordering_runtime_ == OrderingRuntime::kSorter
            ? Instruction{CloseSorterInstruction{.sorter = AssumeValue(sorter_)}}
            : Instruction{CloseTopNInstruction{.top_n = AssumeValue(top_n_)}};
    if (auto closed = Append(close); !closed.has_value()) {
      return closed;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      const Instruction positioned_close =
          ordering_runtime_ == OrderingRuntime::kSorter
              ? Instruction{CloseSorterInstruction{.sorter = AssumeValue(sorter_)}}
              : Instruction{CloseTopNInstruction{.top_n = AssumeValue(top_n_)}};
      if (auto closed = Append(positioned_close); !closed.has_value()) {
        return closed;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitProjection(Label rejected) {
    if (grouped_aggregate_query_) {
      auto input = MakeGroupedAggregateInput();
      if (!input.has_value()) {
        return std::unexpected(std::move(input.error()));
      }
      return lowering_internal::EmitGroupedAggregateRow(*input, *this);
    }
    if (sort_ != nullptr) {
      return EmitOrderedCandidate(rejected);
    }
    const LogicalNode& logical_projection = logical_plan_->node(projection_->logical_projection);
    const auto* projection = std::get_if<LogicalProjectionNode>(&logical_projection.payload);
    if (projection == nullptr ||
        projection->expressions.size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("logical projection shape is invalid"));
    }
    for (std::size_t index = 0; index < projection->expressions.size(); ++index) {
      if (auto emitted = EmitProjectionValue(
              projection->expressions[index],
              RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)});
          !emitted.has_value()) {
        return emitted;
      }
    }
    return Append(ResultRowInstruction{
        .first = result_block_first_,
        .count = static_cast<std::uint32_t>(projection->expressions.size()),
    });
  }

  [[nodiscard]] bool NeedsPreopenCompletion() const noexcept {
    return guard_ != nullptr ||
           (grouped_aggregate_query_ && filter_ != nullptr && !filter_->guards.empty()) ||
           (limit_ != nullptr && !limit_initialized_);
  }

  [[nodiscard]] bool AppliesLimitToSourceRows() const noexcept {
    return !grouped_aggregate_query_ && limit_ != nullptr && sort_ == nullptr;
  }

  [[nodiscard]] LoweringResult<void> EmitEmpty() {
    auto completion = CreateLabel();
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto limit = EmitLimitInitialization(*completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitGuards(*completion); !guards.has_value()) {
      return guards;
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return EmitReadCompletion();
  }

  [[nodiscard]] LoweringResult<void> EmitSingleRow() {
    auto completion = CreateLabel();
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto limit = EmitLimitInitialization(*completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitGuards(*completion); !guards.has_value()) {
      return guards;
    }
    if (auto filters = EmitFilters(*completion); !filters.has_value()) {
      return filters;
    }
    if (auto offset = EmitOffset(*completion); !offset.has_value()) {
      return offset;
    }
    if (auto projection = EmitProjection(*completion); !projection.has_value()) {
      return projection;
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return EmitReadCompletion();
  }

  struct OrderedIndexKey {
    std::size_t conjunct_position = 0;
    BoundExpressionId expression{0};
    bool reject_null = true;
  };

  [[nodiscard]] LoweringResult<void> EmitIndexKeyExpressions(
      const PhysicalIndexScanNode& index, std::optional<Label> null_target) override {
    std::vector<OrderedIndexKey> keys;
    keys.reserve(
        index.equalities.size() +
        static_cast<std::size_t>(index.range.has_value() && index.range->lower.has_value()) +
        static_cast<std::size_t>(index.range.has_value() && index.range->upper.has_value()));
    for (const PhysicalIndexEquality& equality : index.equalities) {
      keys.push_back(OrderedIndexKey{
          .conjunct_position = equality.conjunct_position,
          .expression = equality.key,
          .reject_null = equality.reject_null,
      });
    }
    if (index.range.has_value() && index.range->lower.has_value()) {
      keys.push_back(OrderedIndexKey{
          .conjunct_position = index.range->lower->conjunct_position,
          .expression = index.range->lower->key,
          .reject_null = true,
      });
    }
    if (index.range.has_value() && index.range->upper.has_value()) {
      keys.push_back(OrderedIndexKey{
          .conjunct_position = index.range->upper->conjunct_position,
          .expression = index.range->upper->key,
          .reject_null = true,
      });
    }
    std::ranges::sort(keys, {}, &OrderedIndexKey::conjunct_position);
    for (const OrderedIndexKey& key : keys) {
      const RegisterId home = Home(key.expression);
      if (auto emitted = EmitExpression(key.expression, home); !emitted.has_value()) {
        return emitted;
      }
      if (key.reject_null) {
        if (!null_target.has_value()) {
          return std::unexpected(
              InternalFailure("NULL-rejecting covering index key has no completion label"));
        }
        if (auto jumped = EmitJumpIf(home, JumpCondition::kIfNull, *null_target);
            !jumped.has_value()) {
          return jumped;
        }
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexKeyBlock(
      const PhysicalIndexScanNode& index, const PhysicalIndexBound* range_bound) override {
    if (!index_key_first_.has_value()) {
      return std::unexpected(InternalFailure("covering index key register block is invalid"));
    }
    return EmitIndexKeyBlock(index, range_bound, *index_key_first_, index_key_capacity_);
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexKeyBlock(
      const PhysicalIndexScanNode& index, const PhysicalIndexBound* range_bound, RegisterId first,
      std::uint32_t capacity) {
    const std::size_t count =
        index.equalities.size() + static_cast<std::size_t>(range_bound != nullptr);
    if (count == 0U || count > capacity) {
      return std::unexpected(InternalFailure("covering index key register block is invalid"));
    }
    std::size_t offset = 0;
    const auto copy_term = [&](BoundExpressionId expression,
                               TypeAffinity affinity) -> LoweringResult<void> {
      const RegisterId destination{first.value() + static_cast<std::uint32_t>(offset++)};
      if (auto copied = Append(CopyInstruction{
              .input = Home(expression),
              .output = destination,
          });
          !copied.has_value()) {
        return copied;
      }
      const TypeAffinity key_affinity =
          affinity == TypeAffinity::kReal ? TypeAffinity::kNumeric : affinity;
      return Append(ApplyAffinityInstruction{
          .input = destination,
          .affinity = key_affinity,
          .output = destination,
      });
    };
    for (const PhysicalIndexEquality& equality : index.equalities) {
      if (auto copied = copy_term(equality.key, equality.affinity); !copied.has_value()) {
        return std::unexpected(std::move(copied.error()));
      }
    }
    if (range_bound != nullptr) {
      if (!index.range.has_value()) {
        return std::unexpected(InternalFailure("covering index range bound has no range metadata"));
      }
      if (auto copied = copy_term(range_bound->key, index.range->affinity); !copied.has_value()) {
        return std::unexpected(std::move(copied.error()));
      }
    }
    return static_cast<std::uint32_t>(count);
  }

  [[nodiscard]] LoweringResult<void> EmitCoveringIndexScan(const PhysicalIndexScanNode& index) {
    if (!cursor_.has_value()) {
      return std::unexpected(InternalFailure("covering index scan has no cursor"));
    }
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
    const PhysicalIndexBound* physical_start = reverse ? forward_end : forward_start;
    const PhysicalIndexBound* physical_end = reverse ? forward_start : forward_end;
    const bool has_start_key = !index.equalities.empty() || physical_start != nullptr;
    const bool has_end_key = !index.equalities.empty() || physical_end != nullptr;
    const bool rejects_null = std::ranges::any_of(index.equalities,
                                                  [](const PhysicalIndexEquality& equality) {
                                                    return equality.reject_null;
                                                  }) ||
                              index.range.has_value();

    std::optional<Label> closed_completion;
    if (NeedsPreopenCompletion() || rejects_null) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      closed_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(closed_completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitGuards(closed_completion); !guards.has_value()) {
      return guards;
    }
    if (!index.equalities.empty() || index.range.has_value()) {
      if (auto keys = EmitIndexKeyExpressions(index, closed_completion); !keys.has_value()) {
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

    auto unpositioned_completion = CreateLabel();
    auto row_loop = CreateLabel();
    auto advance = CreateLabel();
    if (!unpositioned_completion.has_value()) {
      return std::unexpected(std::move(unpositioned_completion.error()));
    }
    if (!row_loop.has_value()) {
      return std::unexpected(std::move(row_loop.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (has_start_key) {
      const bool inclusive = physical_start == nullptr || physical_start->inclusive;
      const IndexSeekMode mode =
          reverse ? (inclusive ? IndexSeekMode::kLessOrEqual : IndexSeekMode::kLess)
                  : (inclusive ? IndexSeekMode::kGreaterOrEqual : IndexSeekMode::kGreater);
      auto sought = ConvertProgramResult(
          AssumeValue(builder_).EmitSeekIndex(AssumeValue(cursor_), AssumeValue(index_key_first_),
                                              start_key_count, *unpositioned_completion, mode),
          "unable to emit covering index seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      ProgramResult<InstructionAddress> positioned =
          reverse
              ? AssumeValue(builder_).EmitLast(AssumeValue(cursor_), *unpositioned_completion)
              : AssumeValue(builder_).EmitRewind(AssumeValue(cursor_), *unpositioned_completion);
      auto rewound =
          ConvertProgramResult(std::move(positioned), "unable to emit covering index positioning");
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

    std::optional<Label> positioned_completion;
    if (has_end_key || AppliesLimitToSourceRows()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
    }
    if (auto bound = BindLabel(*row_loop); !bound.has_value()) {
      return bound;
    }
    if (has_end_key) {
      const bool inclusive = physical_end == nullptr || physical_end->inclusive;
      const IndexRangeEndMode mode =
          reverse ? (inclusive ? IndexRangeEndMode::kReverseInclusive
                               : IndexRangeEndMode::kReverseExclusive)
                  : (inclusive ? IndexRangeEndMode::kInclusive : IndexRangeEndMode::kExclusive);
      auto checked =
          ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
                                   AssumeValue(cursor_), AssumeValue(index_key_first_),
                                   end_key_count, AssumeValue(positioned_completion), mode),
                               "unable to emit covering index end check");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (auto filters = EmitFilters(*advance); !filters.has_value()) {
      return filters;
    }
    if (auto offset = EmitOffset(*advance); !offset.has_value()) {
      return offset;
    }
    if (auto projection = EmitProjection(*advance); !projection.has_value()) {
      return projection;
    }

    if (AppliesLimitToSourceRows()) {
      if (!positioned_completion.has_value() || !negative_limit_register_.has_value() ||
          !limit_register_.has_value() || !one_register_.has_value()) {
        return std::unexpected(InternalFailure("covering index LIMIT has no loop registers"));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(negative_limit_register_), JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(limit_register_),
              .right = AssumeValue(one_register_),
              .output = AssumeValue(limit_register_),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(limit_register_), JumpCondition::kIfFalse,
                                   *positioned_completion);
          !jumped.has_value()) {
        return jumped;
      }
    }

    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    ProgramResult<InstructionAddress> advanced =
        reverse ? AssumeValue(builder_).EmitPrevious(AssumeValue(cursor_), *row_loop)
                : AssumeValue(builder_).EmitNext(AssumeValue(cursor_), *row_loop);
    auto next = ConvertProgramResult(std::move(advanced), "unable to emit covering index advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*unpositioned_completion); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(cursor_); !completed.has_value()) {
      return completed;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(cursor_); !completed.has_value()) {
        return completed;
      }
    }
    if (closed_completion.has_value()) {
      if (auto bound = BindLabel(*closed_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitNoncoveringIndexScan(const PhysicalIndexScanNode& index) {
    const bool grouped_full_scan = grouped_aggregate_query_ && aggregate_ != nullptr &&
                                   aggregate_->strategy == PhysicalAggregateStrategy::kOrderedGroup;
    if (index.covering || !cursor_.has_value() || !index_cursor_.has_value() ||
        !index_rowid_register_.has_value() ||
        (index.equalities.empty() && !index.range.has_value() && !grouped_full_scan)) {
      return std::unexpected(InternalFailure("noncovering index scan resources are incomplete"));
    }
    const CursorId index_cursor = AssumeValue(index_cursor_);
    const CursorId table_cursor = AssumeValue(cursor_);
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
    const PhysicalIndexBound* physical_start = reverse ? forward_end : forward_start;
    const PhysicalIndexBound* physical_end = reverse ? forward_start : forward_end;
    const bool has_start_key = !index.equalities.empty() || physical_start != nullptr;
    const bool has_end_key = !index.equalities.empty() || physical_end != nullptr;
    const bool rejects_null = std::ranges::any_of(index.equalities,
                                                  [](const PhysicalIndexEquality& equality) {
                                                    return equality.reject_null;
                                                  }) ||
                              index.range.has_value();

    std::optional<Label> closed_completion;
    if (NeedsPreopenCompletion() || rejects_null) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      closed_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(closed_completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitGuards(closed_completion); !guards.has_value()) {
      return guards;
    }
    if (auto keys = EmitIndexKeyExpressions(index, closed_completion); !keys.has_value()) {
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
      const bool inclusive = physical_start == nullptr || physical_start->inclusive;
      const IndexSeekMode mode =
          reverse ? (inclusive ? IndexSeekMode::kLessOrEqual : IndexSeekMode::kLess)
                  : (inclusive ? IndexSeekMode::kGreaterOrEqual : IndexSeekMode::kGreater);
      auto sought = ConvertProgramResult(
          AssumeValue(builder_).EmitSeekIndex(index_cursor, AssumeValue(index_key_first_),
                                              start_key_count, *index_missing, mode),
          "unable to emit noncovering index seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      ProgramResult<InstructionAddress> positioned =
          reverse ? AssumeValue(builder_).EmitLast(index_cursor, *index_missing)
                  : AssumeValue(builder_).EmitRewind(index_cursor, *index_missing);
      auto rewound = ConvertProgramResult(std::move(positioned),
                                          "unable to emit noncovering index positioning");
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
    std::optional<Label> positioned_completion;
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
      positioned_completion = *positioned;
    } else if (AppliesLimitToSourceRows()) {
      auto positioned = CreateLabel();
      if (!positioned.has_value()) {
        return std::unexpected(std::move(positioned.error()));
      }
      positioned_completion = *positioned;
    }

    auto subsequent_candidate = CreateLabel();
    auto process_candidate = CreateLabel();
    auto advance = CreateLabel();
    auto index_exhausted = CreateLabel();
    if (!subsequent_candidate.has_value()) {
      return std::unexpected(std::move(subsequent_candidate.error()));
    }
    if (!process_candidate.has_value()) {
      return std::unexpected(std::move(process_candidate.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (!index_exhausted.has_value()) {
      return std::unexpected(std::move(index_exhausted.error()));
    }

    if (has_end_key) {
      const bool inclusive = physical_end == nullptr || physical_end->inclusive;
      const IndexRangeEndMode mode =
          reverse ? (inclusive ? IndexRangeEndMode::kReverseInclusive
                               : IndexRangeEndMode::kReverseExclusive)
                  : (inclusive ? IndexRangeEndMode::kInclusive : IndexRangeEndMode::kExclusive);
      auto checked = ConvertProgramResult(
          AssumeValue(builder_).EmitCheckIndexRange(index_cursor, AssumeValue(index_key_first_),
                                                    end_key_count, AssumeValue(first_end), mode),
          "unable to emit first noncovering index end check");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (auto sought = EmitIndexTableLookup(AssumeValue(builder_), index_cursor, table_cursor,
                                           AssumeValue(index_rowid_register_), true);
        !sought.has_value()) {
      return sought;
    }
    if (auto jumped = EmitJump(*process_candidate); !jumped.has_value()) {
      return jumped;
    }

    if (auto bound = BindLabel(*subsequent_candidate); !bound.has_value()) {
      return bound;
    }
    if (has_end_key) {
      const bool inclusive = physical_end == nullptr || physical_end->inclusive;
      const IndexRangeEndMode mode =
          reverse ? (inclusive ? IndexRangeEndMode::kReverseInclusive
                               : IndexRangeEndMode::kReverseExclusive)
                  : (inclusive ? IndexRangeEndMode::kInclusive : IndexRangeEndMode::kExclusive);
      auto checked =
          ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
                                   index_cursor, AssumeValue(index_key_first_), end_key_count,
                                   AssumeValue(positioned_completion), mode),
                               "unable to emit noncovering index end check");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (auto sought = EmitIndexTableLookup(AssumeValue(builder_), index_cursor, table_cursor,
                                           AssumeValue(index_rowid_register_));
        !sought.has_value()) {
      return sought;
    }

    if (auto bound = BindLabel(*process_candidate); !bound.has_value()) {
      return bound;
    }
    if (auto filters = EmitFilters(*advance); !filters.has_value()) {
      return filters;
    }
    if (auto offset = EmitOffset(*advance); !offset.has_value()) {
      return offset;
    }
    if (auto projection = EmitProjection(*advance); !projection.has_value()) {
      return projection;
    }
    if (AppliesLimitToSourceRows()) {
      if (!positioned_completion.has_value() || !negative_limit_register_.has_value() ||
          !limit_register_.has_value() || !one_register_.has_value()) {
        return std::unexpected(InternalFailure("noncovering index LIMIT has no loop registers"));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(negative_limit_register_), JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(limit_register_),
              .right = AssumeValue(one_register_),
              .output = AssumeValue(limit_register_),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(limit_register_), JumpCondition::kIfFalse,
                                   AssumeValue(positioned_completion));
          !jumped.has_value()) {
        return jumped;
      }
    }

    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    ProgramResult<InstructionAddress> advanced =
        reverse ? AssumeValue(builder_).EmitPrevious(index_cursor, *subsequent_candidate)
                : AssumeValue(builder_).EmitNext(index_cursor, *subsequent_candidate);
    auto next =
        ConvertProgramResult(std::move(advanced), "unable to emit noncovering index advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*index_exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(cursor_, index_cursor_); !completed.has_value()) {
      return completed;
    }
    if (first_end.has_value()) {
      if (auto bound = BindLabel(*first_end); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(std::nullopt, index_cursor_);
          !completed.has_value()) {
        return completed;
      }
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(cursor_, index_cursor_); !completed.has_value()) {
        return completed;
      }
    }
    if (auto bound = BindLabel(*index_missing); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(std::nullopt, index_cursor_); !completed.has_value()) {
      return completed;
    }
    if (closed_completion.has_value()) {
      if (auto bound = BindLabel(*closed_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitTableScan() {
    if (!cursor_.has_value()) {
      return std::unexpected(InternalFailure("table scan has no cursor"));
    }
    std::optional<Label> closed_completion;
    if (NeedsPreopenCompletion()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      closed_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(closed_completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitGuards(closed_completion); !guards.has_value()) {
      return guards;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }

    auto empty = CreateLabel();
    auto row_loop = CreateLabel();
    auto advance = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row_loop.has_value()) {
      return std::unexpected(std::move(row_loop.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    ProgramResult<InstructionAddress> positioned =
        table_scan_->direction == PhysicalScanDirection::kForward
            ? AssumeValue(builder_).EmitRewind(AssumeValue(cursor_), AssumeValue(empty))
            : AssumeValue(builder_).EmitLast(AssumeValue(cursor_), AssumeValue(empty));
    auto rewound = ConvertProgramResult(std::move(positioned), "unable to emit table positioning");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row_loop); !bound.has_value()) {
      return bound;
    }
    if (auto filters = EmitFilters(*advance); !filters.has_value()) {
      return filters;
    }
    if (auto offset = EmitOffset(*advance); !offset.has_value()) {
      return offset;
    }
    if (auto projection = EmitProjection(*advance); !projection.has_value()) {
      return projection;
    }

    std::optional<Label> positioned_completion;
    if (AppliesLimitToSourceRows()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
      if (!negative_limit_register_.has_value() || !limit_register_.has_value() ||
          !one_register_.has_value()) {
        return std::unexpected(InternalFailure("table LIMIT has no loop registers"));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(negative_limit_register_), JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(limit_register_),
              .right = AssumeValue(one_register_),
              .output = AssumeValue(limit_register_),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(limit_register_), JumpCondition::kIfFalse,
                                   AssumeValue(positioned_completion));
          !jumped.has_value()) {
        return jumped;
      }
    }

    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    ProgramResult<InstructionAddress> advanced =
        table_scan_->direction == PhysicalScanDirection::kForward
            ? AssumeValue(builder_).EmitNext(AssumeValue(cursor_), AssumeValue(row_loop))
            : AssumeValue(builder_).EmitPrevious(AssumeValue(cursor_), AssumeValue(row_loop));
    auto next = ConvertProgramResult(std::move(advanced), "unable to emit table advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(cursor_); !completed.has_value()) {
      return completed;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(cursor_); !completed.has_value()) {
        return completed;
      }
    }
    if (closed_completion.has_value()) {
      if (auto bound = BindLabel(*closed_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<RegisterId> EmitRowIdLookupKey(
      BoundExpressionId expression) override {
    const RegisterId key = Home(expression);
    if (auto emitted = EmitExpression(expression, key); !emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return key;
  }

  [[nodiscard]] LoweringResult<void> EmitRowIdLookup(const PhysicalRowIdLookupNode& lookup) {
    if (!cursor_.has_value()) {
      return std::unexpected(InternalFailure("rowid lookup has no cursor"));
    }
    std::optional<Label> closed_completion;
    if (NeedsPreopenCompletion()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      closed_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(closed_completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitGuards(closed_completion); !guards.has_value()) {
      return guards;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }

    auto key = EmitRowIdLookupKey(lookup.key);
    if (!key.has_value()) {
      return std::unexpected(std::move(key.error()));
    }
    auto missing = CreateLabel();
    auto positioned = CreateLabel();
    if (!missing.has_value()) {
      return std::unexpected(std::move(missing.error()));
    }
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }
    auto sought = ConvertProgramResult(
        AssumeValue(builder_).EmitSeekRowId(AssumeValue(cursor_), *key, AssumeValue(missing)),
        "unable to emit rowid seek");
    if (!sought.has_value()) {
      return std::unexpected(std::move(sought.error()));
    }
    if (auto filters = EmitFilters(*positioned); !filters.has_value()) {
      return filters;
    }
    if (auto offset = EmitOffset(*positioned); !offset.has_value()) {
      return offset;
    }
    if (auto projection = EmitProjection(*positioned); !projection.has_value()) {
      return projection;
    }
    if (auto bound = BindLabel(*positioned); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(cursor_); !completed.has_value()) {
      return completed;
    }
    if (auto bound = BindLabel(*missing); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(cursor_); !completed.has_value()) {
      return completed;
    }
    if (closed_completion.has_value()) {
      if (auto bound = BindLabel(*closed_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitMutationPredicates(
      std::span<const BoundExpressionId> predicates, Label rejected) {
    for (const BoundExpressionId predicate : predicates) {
      if (auto emitted = EmitPredicate(predicate, rejected); !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> SnapshotMutationRow(bool close_cursor = true) {
    if (!cursor_.has_value() || !source_rowid_register_.has_value() ||
        source_snapshot_registers_.size() != source_columns_.size() ||
        source_field_real_affinity_.size() != source_snapshot_registers_.size()) {
      return std::unexpected(InternalFailure("mutation source snapshot metadata is incomplete"));
    }
    if (auto rowid = Append(ReadRowIdInstruction{
            .cursor = AssumeValue(cursor_),
            .output = AssumeValue(source_rowid_register_),
        });
        !rowid.has_value()) {
      return rowid;
    }
    for (std::size_t index = 0; index < source_snapshot_registers_.size(); ++index) {
      const RegisterId destination = source_snapshot_registers_[index];
      if (auto field = Append(ReadFieldInstruction{
              .cursor = AssumeValue(cursor_),
              .field = CursorFieldId{static_cast<std::uint32_t>(index)},
              .output = destination,
          });
          !field.has_value()) {
        return field;
      }
    }
    if (close_cursor) {
      return Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDeletePoint() {
    if (!write_cursor_.has_value() || !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("DELETE point mutation resources are incomplete"));
    }
    const BoundMutationTarget& target = bound_delete_->target();
    if (index_write_cursors_.size() != target.indexes.size()) {
      return std::unexpected(InternalFailure("DELETE index cursor metadata is incomplete"));
    }
    for (std::size_t index = 0; index < target.indexes.size(); ++index) {
      auto key = EmitMutationIndexKey(target, target.indexes[index],
                                      MutationIndexValueRegisters{
                                          .first_values = std::nullopt,
                                          .rowid = AssumeValue(source_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (auto deleted = Append(DeleteIndexInstruction{
              .cursor = index_write_cursors_[index],
              .first_value = AssumeValue(mutation_index_key_first_),
              .value_count = *key,
          });
          !deleted.has_value()) {
        return deleted;
      }
    }
    return Append(DeleteTableInstruction{
        .cursor = AssumeValue(write_cursor_),
        .rowid = AssumeValue(source_rowid_register_),
    });
  }

  [[nodiscard]] std::optional<RegisterId> SourceColumnRegister(ColumnId column) const noexcept {
    for (std::size_t index = 0; index < source_columns_.size(); ++index) {
      if (source_columns_[index].catalog_column == column) {
        return source_snapshot_registers_[index];
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] LoweringResult<void> PrepareUpdateRecord() {
    if (!update_values_first_.has_value() || !update_new_rowid_register_.has_value() ||
        !update_record_register_.has_value() || !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("UPDATE replacement resources are incomplete"));
    }
    const BoundMutationTarget& target = bound_update_->target();
    for (std::size_t index = 0; index < target.columns.size(); ++index) {
      const std::optional<RegisterId> source = SourceColumnRegister(target.columns[index].column);
      if (!source.has_value()) {
        return std::unexpected(InternalFailure("UPDATE target has no source snapshot column"));
      }
      if (auto copied = Append(CopyInstruction{
              .input = AssumeValue(source),
              .output = RegisterId{AssumeValue(update_values_first_).value() +
                                   static_cast<std::uint32_t>(index)},
          });
          !copied.has_value()) {
        return copied;
      }
    }
    if (auto copied = Append(CopyInstruction{
            .input = AssumeValue(source_rowid_register_),
            .output = AssumeValue(update_new_rowid_register_),
        });
        !copied.has_value()) {
      return copied;
    }

    for (const BoundUpdateAssignment& assignment : bound_update_->assignments()) {
      RegisterId destination = Home(assignment.expression);
      if (assignment.effective) {
        if (assignment.target.rowid) {
          destination = AssumeValue(update_new_rowid_register_);
        } else if (assignment.target.column.has_value()) {
          const std::optional<std::uint32_t> index =
              TargetColumnIndex(target, *assignment.target.column);
          if (!index.has_value()) {
            return std::unexpected(
                InternalFailure("effective UPDATE assignment targets an unknown column"));
          }
          destination = RegisterId{AssumeValue(update_values_first_).value() + AssumeValue(index)};
        } else {
          return std::unexpected(
              InternalFailure("effective UPDATE assignment has no column or rowid target"));
        }
      }
      if (auto emitted = EmitExpression(assignment.expression, destination); !emitted.has_value()) {
        return emitted;
      }
    }
    if (auto integer = Append(MustBeIntegerInstruction{
            .input = AssumeValue(update_new_rowid_register_),
            .output = AssumeValue(update_new_rowid_register_),
        });
        !integer.has_value()) {
      return integer;
    }
    return Append(BuildTableRecordInstruction{
        .cursor = AssumeValue(write_cursor_),
        .first_value = AssumeValue(update_values_first_),
        .value_count = static_cast<std::uint32_t>(target.columns.size()),
        .output = AssumeValue(update_record_register_),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitUpdatePoint() {
    if (!write_cursor_.has_value() || !source_rowid_register_.has_value() ||
        !update_new_rowid_register_.has_value() || !update_record_register_.has_value()) {
      return std::unexpected(InternalFailure("UPDATE point mutation resources are incomplete"));
    }
    if (auto prepared = PrepareUpdateRecord(); !prepared.has_value()) {
      return prepared;
    }
    const BoundMutationTarget& target = bound_update_->target();
    if (index_write_cursors_.size() != target.indexes.size()) {
      return std::unexpected(InternalFailure("UPDATE index cursor metadata is incomplete"));
    }
    if (!target.indexes.empty()) {
      if (auto checked = Append(CheckUpdateRowIdInstruction{
              .cursor = AssumeValue(write_cursor_),
              .old_rowid = AssumeValue(source_rowid_register_),
              .new_rowid = AssumeValue(update_new_rowid_register_),
          });
          !checked.has_value()) {
        return checked;
      }
    }
    for (std::size_t index = 0; index < target.indexes.size(); ++index) {
      const BoundIndexMaintenance& maintenance = target.indexes[index];
      auto key = EmitMutationIndexKey(target, maintenance,
                                      MutationIndexValueRegisters{
                                          .first_values = AssumeValue(update_values_first_),
                                          .rowid = AssumeValue(update_new_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (maintenance.unique) {
        if (auto checked = Append(CheckUniqueIndexInstruction{
                .cursor = index_write_cursors_[index],
                .first_key = AssumeValue(mutation_index_key_first_),
                .key_count = maintenance.key_term_count,
                .ignored_rowid = AssumeValue(source_rowid_register_),
            });
            !checked.has_value()) {
          return checked;
        }
      }
    }
    for (std::size_t index = 0; index < target.indexes.size(); ++index) {
      auto key = EmitMutationIndexKey(target, target.indexes[index],
                                      MutationIndexValueRegisters{
                                          .first_values = std::nullopt,
                                          .rowid = AssumeValue(source_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (auto deleted = Append(DeleteIndexInstruction{
              .cursor = index_write_cursors_[index],
              .first_value = AssumeValue(mutation_index_key_first_),
              .value_count = *key,
          });
          !deleted.has_value()) {
        return deleted;
      }
    }
    for (std::size_t index = 0; index < target.indexes.size(); ++index) {
      auto key = EmitMutationIndexKey(target, target.indexes[index],
                                      MutationIndexValueRegisters{
                                          .first_values = AssumeValue(update_values_first_),
                                          .rowid = AssumeValue(update_new_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (auto inserted = Append(InsertIndexInstruction{
              .cursor = index_write_cursors_[index],
              .first_value = AssumeValue(mutation_index_key_first_),
              .value_count = *key,
          });
          !inserted.has_value()) {
        return inserted;
      }
    }
    return Append(UpdateTableInstruction{
        .cursor = AssumeValue(write_cursor_),
        .old_rowid = AssumeValue(source_rowid_register_),
        .new_rowid = AssumeValue(update_new_rowid_register_),
        .record = AssumeValue(update_record_register_),
    });
  }

  enum class PointMutationKind : std::uint8_t {
    kDelete,
    kUpdate,
  };

  [[nodiscard]] LoweringResult<void> EmitPointMutation(PointMutationKind kind) {
    switch (kind) {
      case PointMutationKind::kDelete:
        return EmitDeletePoint();
      case PointMutationKind::kUpdate:
        return EmitUpdatePoint();
    }
    return std::unexpected(InternalFailure("point mutation kind is invalid"));
  }

  [[nodiscard]] LoweringResult<void> OpenMutationWriteCursors() {
    if (!write_cursor_.has_value()) {
      return std::unexpected(InternalFailure("mutation table write cursor is missing"));
    }
    if (auto opened = Append(OpenWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !opened.has_value()) {
      return opened;
    }
    for (const WriteCursorId cursor : index_write_cursors_) {
      if (auto opened = Append(OpenWriteCursorInstruction{.cursor = cursor}); !opened.has_value()) {
        return opened;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> CloseMutationWriteCursors() {
    for (const WriteCursorId cursor : index_write_cursors_) {
      if (auto closed = Append(CloseWriteCursorInstruction{.cursor = cursor});
          !closed.has_value()) {
        return closed;
      }
    }
    if (!write_cursor_.has_value()) {
      return std::unexpected(InternalFailure("mutation table write cursor is missing"));
    }
    return Append(CloseWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
  }

  [[nodiscard]] LoweringResult<void> EmitMutationExact(const PhysicalMutationAccess& access,
                                                       PointMutationKind kind) {
    if (!access.key.has_value() || !cursor_.has_value() || !write_cursor_.has_value()) {
      return std::unexpected(InternalFailure("exact mutation access metadata is incomplete"));
    }

    std::optional<Label> guard_completion;
    if (!access.guards.empty()) {
      auto completion = CreateLabel();
      if (!completion.has_value()) {
        return std::unexpected(std::move(completion.error()));
      }
      guard_completion = *completion;
      if (auto guards = EmitMutationPredicates(access.guards, *guard_completion);
          !guards.has_value()) {
        return guards;
      }
    }

    if (auto opened = OpenMutationWriteCursors(); !opened.has_value()) {
      return opened;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }
    const RegisterId key = Home(*access.key);
    if (auto emitted = EmitExpression(*access.key, key); !emitted.has_value()) {
      return emitted;
    }

    auto missing = CreateLabel();
    auto rejected = CreateLabel();
    if (!missing.has_value()) {
      return std::unexpected(std::move(missing.error()));
    }
    if (!rejected.has_value()) {
      return std::unexpected(std::move(rejected.error()));
    }
    auto sought =
        ConvertProgramResult(AssumeValue(builder_).EmitSeekRowId(AssumeValue(cursor_), key,
                                                                 *missing, RowIdSeekMode::kEqual),
                             "unable to emit exact mutation rowid seek");
    if (!sought.has_value()) {
      return std::unexpected(std::move(sought.error()));
    }
    if (auto snapshot = SnapshotMutationRow(); !snapshot.has_value()) {
      return snapshot;
    }
    if (auto residuals = EmitMutationPredicates(access.residuals, *rejected);
        !residuals.has_value()) {
      return residuals;
    }
    if (auto mutated = EmitPointMutation(kind); !mutated.has_value()) {
      return mutated;
    }
    if (auto bound = BindLabel(*rejected); !bound.has_value()) {
      return bound;
    }
    if (auto closed = CloseMutationWriteCursors(); !closed.has_value()) {
      return closed;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }

    if (auto bound = BindLabel(*missing); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
        !closed.has_value()) {
      return closed;
    }
    if (auto closed = CloseMutationWriteCursors(); !closed.has_value()) {
      return closed;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }

    if (guard_completion.has_value()) {
      if (auto bound = BindLabel(*guard_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDeleteScan(const PhysicalMutationAccess& access) {
    if (!cursor_.has_value() || !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("one-pass DELETE scan resources are incomplete"));
    }
    const CursorId cursor = AssumeValue(cursor_);

    std::optional<Label> guard_completion;
    if (!access.guards.empty()) {
      auto completion = CreateLabel();
      if (!completion.has_value()) {
        return std::unexpected(std::move(completion.error()));
      }
      guard_completion = *completion;
      if (auto guards = EmitMutationPredicates(access.guards, *guard_completion);
          !guards.has_value()) {
        return guards;
      }
    }

    if (auto opened = Append(OpenMutationCursorInstruction{.cursor = cursor});
        !opened.has_value()) {
      return opened;
    }
    auto exhausted = CreateLabel();
    auto candidate = CreateLabel();
    std::optional<Label> advance;
    if (!access.residuals.empty()) {
      auto created = CreateLabel();
      if (!created.has_value()) {
        return std::unexpected(std::move(created.error()));
      }
      advance = *created;
    }
    if (!exhausted.has_value()) {
      return std::unexpected(std::move(exhausted.error()));
    }
    if (!candidate.has_value()) {
      return std::unexpected(std::move(candidate.error()));
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *exhausted),
                                        "unable to emit one-pass DELETE rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*candidate); !bound.has_value()) {
      return bound;
    }
    if (auto snapshot = SnapshotMutationRow(false); !snapshot.has_value()) {
      return snapshot;
    }
    if (advance.has_value()) {
      if (auto residuals = EmitMutationPredicates(access.residuals, *advance);
          !residuals.has_value()) {
        return residuals;
      }
    }
    auto deleted =
        ConvertProgramResult(AssumeValue(builder_).EmitDeleteCurrentTable(cursor, *exhausted),
                             "unable to emit one-pass DELETE mutation");
    if (!deleted.has_value()) {
      return std::unexpected(std::move(deleted.error()));
    }
    if (auto jumped = EmitJump(*candidate); !jumped.has_value()) {
      return jumped;
    }
    if (advance.has_value()) {
      if (auto bound = BindLabel(*advance); !bound.has_value()) {
        return bound;
      }
      auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *candidate),
                                       "unable to emit one-pass DELETE advance");
      if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
      }
    }
    if (auto bound = BindLabel(*exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (guard_completion.has_value()) {
      if (auto bound = BindLabel(*guard_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitUpdateScan(const PhysicalMutationAccess& access) {
    if (!cursor_.has_value() || !write_cursor_.has_value() || !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("one-pass UPDATE scan resources are incomplete"));
    }
    const CursorId cursor = AssumeValue(cursor_);

    std::optional<Label> guard_completion;
    if (!access.guards.empty()) {
      auto completion = CreateLabel();
      if (!completion.has_value()) {
        return std::unexpected(std::move(completion.error()));
      }
      guard_completion = *completion;
      if (auto guards = EmitMutationPredicates(access.guards, *guard_completion);
          !guards.has_value()) {
        return guards;
      }
    }

    if (auto opened = Append(OpenWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !opened.has_value()) {
      return opened;
    }
    if (auto opened = Append(OpenMutationCursorInstruction{.cursor = cursor});
        !opened.has_value()) {
      return opened;
    }
    auto exhausted = CreateLabel();
    auto candidate = CreateLabel();
    auto advance = CreateLabel();
    if (!exhausted.has_value()) {
      return std::unexpected(std::move(exhausted.error()));
    }
    if (!candidate.has_value()) {
      return std::unexpected(std::move(candidate.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *exhausted),
                                        "unable to emit one-pass UPDATE rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*candidate); !bound.has_value()) {
      return bound;
    }
    if (auto snapshot = SnapshotMutationRow(false); !snapshot.has_value()) {
      return snapshot;
    }
    if (auto residuals = EmitMutationPredicates(access.residuals, *advance);
        !residuals.has_value()) {
      return residuals;
    }
    if (auto prepared = PrepareUpdateRecord(); !prepared.has_value()) {
      return prepared;
    }
    if (auto updated = Append(UpdateCurrentTableInstruction{
            .cursor = cursor,
            .record = AssumeValue(update_record_register_),
        });
        !updated.has_value()) {
      return updated;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *candidate),
                                     "unable to emit one-pass UPDATE advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto closed = Append(CloseWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !closed.has_value()) {
      return closed;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (guard_completion.has_value()) {
      if (auto bound = BindLabel(*guard_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDelete(const PhysicalDeleteMutation& delete_plan) {
    switch (delete_plan.access.kind) {
      case MutationAccessKind::kEmpty:
        return Append(HaltInstruction{});
      case MutationAccessKind::kRowIdLookup:
        return EmitMutationExact(delete_plan.access, PointMutationKind::kDelete);
      case MutationAccessKind::kTableScan:
        if (delete_plan.collect_original_rowids) {
          return EmitCollectedMutationScan(delete_plan.access, PointMutationKind::kDelete);
        }
        return EmitDeleteScan(delete_plan.access);
    }
    return std::unexpected(InternalFailure("physical DELETE access kind is invalid"));
  }

  [[nodiscard]] LoweringResult<void> EmitCollectedMutationScan(const PhysicalMutationAccess& access,
                                                               PointMutationKind kind) {
    if (!cursor_.has_value() || !write_cursor_.has_value() || !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("collected mutation scan resources are incomplete"));
    }

    std::optional<Label> guard_completion;
    if (!access.guards.empty()) {
      auto completion = CreateLabel();
      if (!completion.has_value()) {
        return std::unexpected(std::move(completion.error()));
      }
      guard_completion = *completion;
      if (auto guards = EmitMutationPredicates(access.guards, *guard_completion);
          !guards.has_value()) {
        return guards;
      }
    }

    if (auto cleared = Append(ClearRowIdListInstruction{}); !cleared.has_value()) {
      return cleared;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }
    auto collection_exhausted = CreateLabel();
    auto collection_candidate = CreateLabel();
    auto collection_advance = CreateLabel();
    if (!collection_exhausted.has_value()) {
      return std::unexpected(std::move(collection_exhausted.error()));
    }
    if (!collection_candidate.has_value()) {
      return std::unexpected(std::move(collection_candidate.error()));
    }
    if (!collection_advance.has_value()) {
      return std::unexpected(std::move(collection_advance.error()));
    }
    auto rewound = ConvertProgramResult(
        AssumeValue(builder_).EmitRewind(AssumeValue(cursor_), *collection_exhausted),
        "unable to emit UPDATE collection rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*collection_candidate); !bound.has_value()) {
      return bound;
    }
    if (auto snapshot = SnapshotMutationRow(); !snapshot.has_value()) {
      return snapshot;
    }
    if (auto residuals = EmitMutationPredicates(access.residuals, *collection_advance);
        !residuals.has_value()) {
      return residuals;
    }
    if (auto appended = Append(AppendRowIdListInstruction{
            .input = AssumeValue(source_rowid_register_),
        });
        !appended.has_value()) {
      return appended;
    }
    if (auto bound = BindLabel(*collection_advance); !bound.has_value()) {
      return bound;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }
    auto sought =
        ConvertProgramResult(AssumeValue(builder_).EmitSeekRowId(
                                 AssumeValue(cursor_), AssumeValue(source_rowid_register_),
                                 *collection_exhausted, RowIdSeekMode::kGreater),
                             "unable to emit UPDATE collection advance");
    if (!sought.has_value()) {
      return std::unexpected(std::move(sought.error()));
    }
    if (auto jumped = EmitJump(*collection_candidate); !jumped.has_value()) {
      return jumped;
    }

    if (auto bound = BindLabel(*collection_exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
        !closed.has_value()) {
      return closed;
    }

    auto no_matches = CreateLabel();
    auto mutation_loop = CreateLabel();
    auto missing_row = CreateLabel();
    auto list_advance = CreateLabel();
    if (!no_matches.has_value()) {
      return std::unexpected(std::move(no_matches.error()));
    }
    if (!mutation_loop.has_value()) {
      return std::unexpected(std::move(mutation_loop.error()));
    }
    if (!missing_row.has_value()) {
      return std::unexpected(std::move(missing_row.error()));
    }
    if (!list_advance.has_value()) {
      return std::unexpected(std::move(list_advance.error()));
    }
    auto list_rewound = ConvertProgramResult(
        AssumeValue(builder_).EmitRewindRowIdList(AssumeValue(source_rowid_register_), *no_matches),
        "unable to rewind collected mutation rowids");
    if (!list_rewound.has_value()) {
      return std::unexpected(std::move(list_rewound.error()));
    }
    if (auto opened = OpenMutationWriteCursors(); !opened.has_value()) {
      return opened;
    }
    if (auto bound = BindLabel(*mutation_loop); !bound.has_value()) {
      return bound;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }
    auto row_sought =
        ConvertProgramResult(AssumeValue(builder_).EmitSeekRowId(
                                 AssumeValue(cursor_), AssumeValue(source_rowid_register_),
                                 *missing_row, RowIdSeekMode::kEqual),
                             "unable to seek a collected mutation rowid");
    if (!row_sought.has_value()) {
      return std::unexpected(std::move(row_sought.error()));
    }
    if (auto snapshot = SnapshotMutationRow(); !snapshot.has_value()) {
      return snapshot;
    }
    if (auto mutated = EmitPointMutation(kind); !mutated.has_value()) {
      return mutated;
    }
    if (auto jumped = EmitJump(*list_advance); !jumped.has_value()) {
      return jumped;
    }

    if (auto bound = BindLabel(*missing_row); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
        !closed.has_value()) {
      return closed;
    }
    if (auto bound = BindLabel(*list_advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextRowIdList(
                                         AssumeValue(source_rowid_register_), *mutation_loop),
                                     "unable to advance collected mutation rowids");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto closed = CloseMutationWriteCursors(); !closed.has_value()) {
      return closed;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }

    if (auto bound = BindLabel(*no_matches); !bound.has_value()) {
      return bound;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (guard_completion.has_value()) {
      if (auto bound = BindLabel(*guard_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitUpdate(const PhysicalUpdateMutation& update) {
    switch (update.access.kind) {
      case MutationAccessKind::kEmpty:
        return Append(HaltInstruction{});
      case MutationAccessKind::kRowIdLookup:
        return EmitMutationExact(update.access, PointMutationKind::kUpdate);
      case MutationAccessKind::kTableScan:
        if (update.collect_original_rowids) {
          return EmitCollectedMutationScan(update.access, PointMutationKind::kUpdate);
        }
        return EmitUpdateScan(update.access);
    }
    return std::unexpected(InternalFailure("physical UPDATE access kind is invalid"));
  }

  [[nodiscard]] LoweringResult<void> EmitSchemaRecord(std::string_view type_name,
                                                      std::string_view object_name,
                                                      std::string_view table_name,
                                                      std::string_view canonical_sql) {
    if (!schema_write_cursor_.has_value() || type_name.empty() || object_name.empty() ||
        table_name.empty() || canonical_sql.empty()) {
      return std::unexpected(InternalFailure("schema record metadata is incomplete"));
    }
    auto type = AddConstant(SqlValue::Text(std::string{type_name}));
    if (!type.has_value()) {
      return std::unexpected(std::move(type.error()));
    }
    auto name = AddConstant(SqlValue::Text(std::string{object_name}));
    if (!name.has_value()) {
      return std::unexpected(std::move(name.error()));
    }
    auto table = AddConstant(SqlValue::Text(std::string{table_name}));
    if (!table.has_value()) {
      return std::unexpected(std::move(table.error()));
    }
    auto sql = AddConstant(SqlValue::Text(std::string{canonical_sql}));
    if (!sql.has_value()) {
      return std::unexpected(std::move(sql.error()));
    }
    auto null_rowid = AddConstant(SqlValue{});
    if (!null_rowid.has_value()) {
      return std::unexpected(std::move(null_rowid.error()));
    }
    const std::array loads{
        LoadConstantInstruction{.constant = AssumeValue(type), .output = RegisterId{0}},
        LoadConstantInstruction{.constant = AssumeValue(name), .output = RegisterId{1}},
        LoadConstantInstruction{.constant = AssumeValue(table), .output = RegisterId{2}},
        LoadConstantInstruction{.constant = AssumeValue(sql), .output = RegisterId{4}},
        LoadConstantInstruction{.constant = AssumeValue(null_rowid), .output = RegisterId{5}},
    };
    for (const LoadConstantInstruction& load : loads) {
      if (auto loaded = Append(load); !loaded.has_value()) {
        return loaded;
      }
    }
    const WriteCursorId schema_cursor = AssumeValue(schema_write_cursor_);
    if (auto opened = Append(OpenWriteCursorInstruction{.cursor = schema_cursor});
        !opened.has_value()) {
      return opened;
    }
    if (auto rowid = Append(ResolveInsertRowIdInstruction{
            .cursor = schema_cursor,
            .input = RegisterId{5},
            .output = RegisterId{5},
        });
        !rowid.has_value()) {
      return rowid;
    }
    if (auto record = Append(BuildTableRecordInstruction{
            .cursor = schema_cursor,
            .first_value = RegisterId{0},
            .value_count = 5,
            .output = RegisterId{6},
        });
        !record.has_value()) {
      return record;
    }
    if (auto inserted = Append(InsertTableInstruction{
            .cursor = schema_cursor,
            .rowid = RegisterId{5},
            .record = RegisterId{6},
        });
        !inserted.has_value()) {
      return inserted;
    }
    if (auto closed = Append(CloseWriteCursorInstruction{.cursor = schema_cursor});
        !closed.has_value()) {
      return closed;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitCreateTable() {
    if (bound_create_->table_name().empty() || bound_create_->canonical_sql().empty()) {
      return std::unexpected(InternalFailure("CREATE TABLE metadata is incomplete"));
    }
    if (auto ensured = Append(EnsureDatabaseInitializedInstruction{}); !ensured.has_value()) {
      return ensured;
    }
    if (auto root = Append(CreateTableRootInstruction{.output = RegisterId{3}});
        !root.has_value()) {
      return root;
    }
    if (auto schema = EmitSchemaRecord("table", bound_create_->table_name(),
                                       bound_create_->table_name(), bound_create_->canonical_sql());
        !schema.has_value()) {
      return schema;
    }
    if (auto cookie = Append(IncrementSchemaCookieInstruction{.output = RegisterId{7}});
        !cookie.has_value()) {
      return cookie;
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitCreateIndex() {
    if (!create_index_table_cursor_.has_value() || !schema_write_cursor_.has_value() ||
        !create_index_write_cursor_.has_value() || bound_create_index_->index_name().empty() ||
        bound_create_index_->table_name().empty() || bound_create_index_->canonical_sql().empty()) {
      return std::unexpected(InternalFailure("CREATE INDEX lowering resources are incomplete"));
    }
    const std::span<const BoundCreateIndexTerm> terms = bound_create_index_->terms();
    const auto key_count = static_cast<std::uint32_t>(terms.size());
    constexpr RegisterId kRootPage{3};
    constexpr RegisterId kSchemaCookie{7};
    constexpr RegisterId kFirstKey{8};
    const CursorId table_cursor = AssumeValue(create_index_table_cursor_);
    const WriteCursorId index_cursor = AssumeValue(create_index_write_cursor_);

    if (auto root = Append(CreateIndexRootInstruction{
            .cursor = index_cursor,
            .output = kRootPage,
        });
        !root.has_value()) {
      return root;
    }
    if (auto schema = EmitSchemaRecord("index", bound_create_index_->index_name(),
                                       bound_create_index_->table_name(),
                                       bound_create_index_->canonical_sql());
        !schema.has_value()) {
      return schema;
    }

    if (auto opened = Append(OpenReadCursorInstruction{.cursor = table_cursor});
        !opened.has_value()) {
      return opened;
    }
    auto exhausted = CreateLabel();
    auto loop = CreateLabel();
    if (!exhausted.has_value()) {
      return std::unexpected(std::move(exhausted.error()));
    }
    if (!loop.has_value()) {
      return std::unexpected(std::move(loop.error()));
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(table_cursor, *exhausted),
                                        "unable to emit CREATE INDEX table rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*loop); !bound.has_value()) {
      return bound;
    }
    for (std::size_t index = 0; index < terms.size(); ++index) {
      const RegisterId output{kFirstKey.value() + static_cast<std::uint32_t>(index)};
      if (auto field = Append(ReadFieldInstruction{
              .cursor = table_cursor,
              .field = CursorFieldId{static_cast<std::uint32_t>(index)},
              .output = output,
          });
          !field.has_value()) {
        return field;
      }
      const TypeAffinity affinity = terms[index].affinity == TypeAffinity::kReal
                                        ? TypeAffinity::kNumeric
                                        : terms[index].affinity;
      if (auto applied = Append(ApplyAffinityInstruction{
              .input = output,
              .affinity = affinity,
              .output = output,
          });
          !applied.has_value()) {
        return applied;
      }
    }
    const RegisterId rowid{kFirstKey.value() + key_count};
    if (auto read = Append(ReadRowIdInstruction{
            .cursor = table_cursor,
            .output = rowid,
        });
        !read.has_value()) {
      return read;
    }
    if (bound_create_index_->unique()) {
      if (auto checked = Append(CheckUniqueIndexInstruction{
              .cursor = index_cursor,
              .first_key = kFirstKey,
              .key_count = key_count,
              .ignored_rowid = std::nullopt,
          });
          !checked.has_value()) {
        return checked;
      }
    }
    if (auto inserted = Append(InsertIndexInstruction{
            .cursor = index_cursor,
            .first_value = kFirstKey,
            .value_count = key_count + 1U,
        });
        !inserted.has_value()) {
      return inserted;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(table_cursor, *loop),
                                     "unable to emit CREATE INDEX table advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = table_cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto closed = Append(CloseWriteCursorInstruction{.cursor = index_cursor});
        !closed.has_value()) {
      return closed;
    }
    if (auto cookie = Append(IncrementSchemaCookieInstruction{.output = kSchemaCookie});
        !cookie.has_value()) {
      return cookie;
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitStat1Record(std::string_view table_name,
                                                     std::optional<std::string_view> index_name) {
    if (!stat1_write_cursor_.has_value() || table_name.empty()) {
      return std::unexpected(InternalFailure("sqlite_stat1 record metadata is incomplete"));
    }
    auto table = AddConstant(SqlValue::Text(std::string{table_name}));
    if (!table.has_value()) {
      return std::unexpected(std::move(table.error()));
    }
    LoweringResult<ConstantId> index = index_name.has_value()
                                           ? AddConstant(SqlValue::Text(std::string{*index_name}))
                                           : EnsureNullConstant();
    if (!index.has_value()) {
      return std::unexpected(std::move(index.error()));
    }
    auto null_rowid = EnsureNullConstant();
    if (!null_rowid.has_value()) {
      return std::unexpected(std::move(null_rowid.error()));
    }
    auto skip = CreateLabel();
    if (!skip.has_value()) {
      return std::unexpected(std::move(skip.error()));
    }
    if (auto jumped = EmitJumpIf(RegisterId{2}, JumpCondition::kIfNull, *skip);
        !jumped.has_value()) {
      return jumped;
    }
    const std::array loads{
        LoadConstantInstruction{.constant = *table, .output = RegisterId{0}},
        LoadConstantInstruction{.constant = *index, .output = RegisterId{1}},
        LoadConstantInstruction{.constant = *null_rowid, .output = RegisterId{3}},
    };
    for (const LoadConstantInstruction& load : loads) {
      if (auto loaded = Append(load); !loaded.has_value()) {
        return loaded;
      }
    }
    const WriteCursorId cursor = AssumeValue(stat1_write_cursor_);
    if (auto rowid = Append(ResolveInsertRowIdInstruction{
            .cursor = cursor,
            .input = RegisterId{3},
            .output = RegisterId{3},
        });
        !rowid.has_value()) {
      return rowid;
    }
    if (auto record = Append(BuildTableRecordInstruction{
            .cursor = cursor,
            .first_value = RegisterId{0},
            .value_count = 3,
            .output = RegisterId{4},
        });
        !record.has_value()) {
      return record;
    }
    if (auto inserted = Append(InsertTableInstruction{
            .cursor = cursor,
            .rowid = RegisterId{3},
            .record = RegisterId{4},
        });
        !inserted.has_value()) {
      return inserted;
    }
    return BindLabel(*skip);
  }

  [[nodiscard]] LoweringResult<void> EmitAnalyze(const PhysicalAnalyzeMutation& analyze) {
    if (!stat1_write_cursor_.has_value() ||
        analyze_index_cursors_.size() != bound_analyze_->indexes().size() ||
        analyze_table_cursors_.size() != bound_analyze_->tables().size()) {
      return std::unexpected(InternalFailure("ANALYZE lowering resources are incomplete"));
    }
    const WriteCursorId stat1_cursor = AssumeValue(stat1_write_cursor_);
    if (analyze.creates_stat1) {
      if (!schema_write_cursor_.has_value()) {
        return std::unexpected(InternalFailure("ANALYZE schema cursor is missing"));
      }
      if (auto root = Append(CreateTableRootInstruction{
              .cursor = stat1_cursor,
              .output = RegisterId{3},
          });
          !root.has_value()) {
        return root;
      }
      if (auto schema = EmitSchemaRecord("table", "sqlite_stat1", "sqlite_stat1",
                                         "CREATE TABLE sqlite_stat1(tbl,idx,stat)");
          !schema.has_value()) {
        return schema;
      }
    } else {
      if (auto opened = Append(OpenWriteCursorInstruction{.cursor = stat1_cursor});
          !opened.has_value()) {
        return opened;
      }
    }

    Stat1ClearScope clear_scope = Stat1ClearScope::kDatabase;
    std::optional<RegisterId> clear_name;
    if (bound_analyze_->scope() != BoundAnalyzeScope::kDatabase) {
      auto name = AddConstant(SqlValue::Text(std::string{bound_analyze_->scope_name()}));
      if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *name,
              .output = RegisterId{0},
          });
          !loaded.has_value()) {
        return loaded;
      }
      clear_name = RegisterId{0};
      clear_scope = bound_analyze_->scope() == BoundAnalyzeScope::kTable ? Stat1ClearScope::kTable
                                                                         : Stat1ClearScope::kIndex;
    }
    if (auto cleared = Append(ClearStat1Instruction{
            .cursor = stat1_cursor,
            .scope = clear_scope,
            .name = clear_name,
        });
        !cleared.has_value()) {
      return cleared;
    }

    for (std::size_t index = 0; index < bound_analyze_->indexes().size(); ++index) {
      const BoundAnalyzeIndex& metadata = bound_analyze_->indexes()[index];
      if (auto computed = Append(ComputeIndexStat1Instruction{
              .cursor = analyze_index_cursors_[index],
              .key_term_count = metadata.key_term_count,
              .emit_empty = metadata.partial,
              .output = RegisterId{2},
          });
          !computed.has_value()) {
        return computed;
      }
      if (auto inserted = EmitStat1Record(metadata.table_name, metadata.index_name);
          !inserted.has_value()) {
        return inserted;
      }
    }
    for (std::size_t table = 0; table < bound_analyze_->tables().size(); ++table) {
      const BoundAnalyzeTable& metadata = bound_analyze_->tables()[table];
      if (auto computed = Append(ComputeTableStat1Instruction{
              .cursor = analyze_table_cursors_[table],
              .output = RegisterId{2},
          });
          !computed.has_value()) {
        return computed;
      }
      if (auto inserted = EmitStat1Record(metadata.table_name, std::nullopt);
          !inserted.has_value()) {
        return inserted;
      }
    }
    if (auto closed = Append(CloseWriteCursorInstruction{.cursor = stat1_cursor});
        !closed.has_value()) {
      return closed;
    }
    if (analyze.creates_stat1) {
      if (auto cookie = Append(IncrementSchemaCookieInstruction{.output = RegisterId{7}});
          !cookie.has_value()) {
        return cookie;
      }
    }
    return Append(HaltInstruction{});
  }

  struct MutationIndexValueRegisters {
    std::optional<RegisterId> first_values{};
    RegisterId rowid;
  };

  [[nodiscard]] LoweringResult<std::uint32_t> EmitMutationIndexKey(
      const BoundMutationTarget& target, const BoundIndexMaintenance& index,
      MutationIndexValueRegisters registers) {
    if (!mutation_index_key_first_.has_value() || index.terms.empty() ||
        index.terms.size() > mutation_index_key_capacity_) {
      return std::unexpected(InternalFailure("mutation index key register block is invalid"));
    }
    for (std::size_t term_index = 0; term_index < index.terms.size(); ++term_index) {
      const BoundIndexTerm& term = index.terms[term_index];
      const RegisterId destination{AssumeValue(mutation_index_key_first_).value() +
                                   static_cast<std::uint32_t>(term_index)};
      RegisterId source = registers.rowid;
      std::optional<TypeAffinity> affinity;
      bool normalize_real = false;
      if (term.column.has_value()) {
        const std::optional<std::uint32_t> column_index = TargetColumnIndex(target, *term.column);
        if (!column_index.has_value()) {
          return std::unexpected(
              InternalFailure("index maintenance term targets an unknown column"));
        }
        if (registers.first_values.has_value()) {
          source = RegisterId{registers.first_values->value() +
                              static_cast<std::uint32_t>(*column_index)};
        } else {
          const std::optional<RegisterId> source_register = SourceColumnRegister(*term.column);
          if (!source_register.has_value()) {
            return std::unexpected(
                InternalFailure("index maintenance term has no source snapshot column"));
          }
          source = *source_register;
        }
        const TypeAffinity column_affinity = target.columns[*column_index].affinity;
        normalize_real =
            column_affinity == TypeAffinity::kReal && registers.first_values.has_value();
        affinity =
            column_affinity == TypeAffinity::kReal ? TypeAffinity::kNumeric : column_affinity;
      } else if (!term.rowid) {
        return std::unexpected(InternalFailure("index maintenance term has no physical source"));
      }
      if (auto copied = Append(CopyInstruction{
              .input = source,
              .output = destination,
          });
          !copied.has_value()) {
        return std::unexpected(std::move(copied.error()));
      }
      if (normalize_real) {
        if (auto applied = Append(RealStorageAffinityInstruction{
                .input = destination,
                .output = destination,
            });
            !applied.has_value()) {
          return std::unexpected(std::move(applied.error()));
        }
      }
      if (affinity.has_value()) {
        if (auto applied = Append(ApplyAffinityInstruction{
                .input = destination,
                .affinity = *affinity,
                .output = destination,
            });
            !applied.has_value()) {
          return std::unexpected(std::move(applied.error()));
        }
      }
    }
    return static_cast<std::uint32_t>(index.terms.size());
  }

  [[nodiscard]] LoweringResult<void> EmitInsert() {
    if (!write_cursor_.has_value() || !insert_values_first_.has_value() ||
        !insert_rowid_register_.has_value() || !insert_record_register_.has_value()) {
      return std::unexpected(InternalFailure("INSERT lowering resources are incomplete"));
    }
    const BoundMutationTarget& target = bound_insert_->target();
    if (insert_default_constants_.size() != target.columns.size()) {
      return std::unexpected(InternalFailure("INSERT default register metadata is incomplete"));
    }

    auto null_constant = EnsureNullConstant();
    if (!null_constant.has_value()) {
      return std::unexpected(std::move(null_constant.error()));
    }
    for (std::size_t index = 0; index < target.columns.size(); ++index) {
      const ConstantId initial =
          insert_default_constants_[index].value_or(AssumeValue(null_constant));
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = initial,
              .output = RegisterId{AssumeValue(insert_values_first_).value() +
                                   static_cast<std::uint32_t>(index)},
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = AssumeValue(null_constant),
            .output = AssumeValue(insert_rowid_register_),
        });
        !loaded.has_value()) {
      return loaded;
    }

    for (const BoundInsertValue& value : bound_insert_->values()) {
      RegisterId destination = Home(value.expression);
      if (value.effective) {
        if (value.target.rowid) {
          destination = AssumeValue(insert_rowid_register_);
        } else if (value.target.column.has_value()) {
          const std::optional<std::uint32_t> index =
              TargetColumnIndex(target, *value.target.column);
          if (!index.has_value()) {
            return std::unexpected(
                InternalFailure("effective INSERT value targets an unknown column"));
          }
          destination = RegisterId{AssumeValue(insert_values_first_).value() + AssumeValue(index)};
        } else {
          return std::unexpected(
              InternalFailure("effective INSERT value has no column or rowid target"));
        }
      }
      if (auto emitted = EmitExpression(value.expression, destination); !emitted.has_value()) {
        return emitted;
      }
    }

    if (auto opened = Append(OpenWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !opened.has_value()) {
      return opened;
    }
    if (auto resolved = Append(ResolveInsertRowIdInstruction{
            .cursor = AssumeValue(write_cursor_),
            .input = AssumeValue(insert_rowid_register_),
            .output = AssumeValue(insert_rowid_register_),
        });
        !resolved.has_value()) {
      return resolved;
    }
    if (auto checked = Append(CheckInsertRowIdInstruction{
            .cursor = AssumeValue(write_cursor_),
            .rowid = AssumeValue(insert_rowid_register_),
        });
        !checked.has_value()) {
      return checked;
    }
    if (auto built = Append(BuildTableRecordInstruction{
            .cursor = AssumeValue(write_cursor_),
            .first_value = AssumeValue(insert_values_first_),
            .value_count = static_cast<std::uint32_t>(target.columns.size()),
            .output = AssumeValue(insert_record_register_),
        });
        !built.has_value()) {
      return built;
    }
    if (index_write_cursors_.size() != target.indexes.size()) {
      return std::unexpected(InternalFailure("INSERT index cursor metadata is incomplete"));
    }
    for (const WriteCursorId cursor : index_write_cursors_) {
      if (auto opened = Append(OpenWriteCursorInstruction{.cursor = cursor}); !opened.has_value()) {
        return opened;
      }
    }
    for (std::size_t index = 0; index < target.indexes.size(); ++index) {
      const BoundIndexMaintenance& maintenance = target.indexes[index];
      auto key = EmitMutationIndexKey(target, maintenance,
                                      MutationIndexValueRegisters{
                                          .first_values = AssumeValue(insert_values_first_),
                                          .rowid = AssumeValue(insert_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (maintenance.unique) {
        if (auto checked = Append(CheckUniqueIndexInstruction{
                .cursor = index_write_cursors_[index],
                .first_key = AssumeValue(mutation_index_key_first_),
                .key_count = maintenance.key_term_count,
                .ignored_rowid = std::nullopt,
            });
            !checked.has_value()) {
          return checked;
        }
      }
    }
    for (std::size_t index = 0; index < target.indexes.size(); ++index) {
      const BoundIndexMaintenance& maintenance = target.indexes[index];
      auto key = EmitMutationIndexKey(target, maintenance,
                                      MutationIndexValueRegisters{
                                          .first_values = AssumeValue(insert_values_first_),
                                          .rowid = AssumeValue(insert_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (auto inserted = Append(InsertIndexInstruction{
              .cursor = index_write_cursors_[index],
              .first_value = AssumeValue(mutation_index_key_first_),
              .value_count = *key,
          });
          !inserted.has_value()) {
        return inserted;
      }
    }
    if (auto inserted = Append(InsertTableInstruction{
            .cursor = AssumeValue(write_cursor_),
            .rowid = AssumeValue(insert_rowid_register_),
            .record = AssumeValue(insert_record_register_),
        });
        !inserted.has_value()) {
      return inserted;
    }
    for (const WriteCursorId cursor : index_write_cursors_) {
      if (auto closed = Append(CloseWriteCursorInstruction{.cursor = cursor});
          !closed.has_value()) {
        return closed;
      }
    }
    if (auto closed = Append(CloseWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !closed.has_value()) {
      return closed;
    }
    return Append(HaltInstruction{});
  }

  void SetStreamingCoreContext(const StreamingCorePlan& core) noexcept {
    filter_ = core.filter;
    projection_ = core.projection;
    cursor_ = core.cursor;
    index_cursor_ = core.index_cursor;
    leaf_ = core.leaf;
    table_scan_ = core.table_scan;
    rowid_lookup_ = core.rowid_lookup;
    index_scan_ = core.index_scan;
    distinct_ = core.distinct;
    distinct_relation_ = core.relation;
    if (const auto* select = std::get_if<BoundSelectCore>(core.bound); select != nullptr) {
      active_source_column_begin_ = select->source_column_begin;
      active_source_column_count_ = select->source_column_count;
    } else {
      active_source_column_begin_ = 0;
      active_source_column_count_ = 0;
    }
  }

  void SetStreamingAggregateCoreBasics(const StreamingCorePlan& core) noexcept {
    SetStreamingCoreContext(core);
    aggregate_core_ = std::get_if<BoundSelectCore>(core.bound);
    aggregate_program_ = aggregate_core_ != nullptr && aggregate_core_->aggregate.has_value()
                             ? &*aggregate_core_->aggregate
                             : nullptr;
    aggregate_ = core.aggregate;
    having_ = core.having;
    grouped_sorter_ = core.group_sorter;
    global_aggregate_query_ = aggregate_program_ != nullptr && aggregate_program_->group_by.empty();
    grouped_aggregate_query_ =
        aggregate_program_ != nullptr && !aggregate_program_->group_by.empty();
    global_representative_query_ =
        global_aggregate_query_ && !core.global_representative_leaves.empty();
    global_filtered_controller_defaults_ = core.global_filtered_controller_defaults;
    sort_ = nullptr;
    output_ = nullptr;
  }

  [[nodiscard]] LoweringResult<void> SetStreamingAggregateCoreContext(
      const StreamingCorePlan& core) {
    SetStreamingAggregateCoreBasics(core);
    if (aggregate_core_ == nullptr || aggregate_program_ == nullptr || aggregate_ == nullptr) {
      return std::unexpected(InternalFailure("streaming aggregate context is incomplete"));
    }

    if (global_aggregate_query_) {
      global_representative_leaves_ = core.global_representative_leaves;
      global_representative_registers_.assign(
          streaming_global_representative_registers_.begin(),
          streaming_global_representative_registers_.begin() +
              static_cast<std::ptrdiff_t>(global_representative_leaves_.size()));
      if (auto mapped = MapAggregateLeafRegisters(global_representative_leaves_,
                                                  global_representative_registers_,
                                                  global_representative_register_map_);
          !mapped.has_value()) {
        return mapped;
      }
    } else {
      global_representative_leaves_.clear();
      global_representative_registers_.clear();
      global_representative_register_map_.clear();
    }

    if (grouped_aggregate_query_) {
      if (streaming_grouped_representative_registers_.size() <
              aggregate_program_->representative_leaves.size() ||
          streaming_grouped_retained_registers_.size() < core.grouped_retained_leaves.size()) {
        return std::unexpected(
            InternalFailure("streaming grouped aggregate register pool is incomplete"));
      }
      grouped_representative_registers_.assign(
          streaming_grouped_representative_registers_.begin(),
          streaming_grouped_representative_registers_.begin() +
              static_cast<std::ptrdiff_t>(aggregate_program_->representative_leaves.size()));
      if (auto mapped = MapAggregateLeafRegisters(aggregate_program_->representative_leaves,
                                                  grouped_representative_registers_,
                                                  grouped_representative_register_map_);
          !mapped.has_value()) {
        return mapped;
      }

      grouped_retained_leaves_ = core.grouped_retained_leaves;
      grouped_retained_leaf_registers_.assign(
          streaming_grouped_retained_registers_.begin(),
          streaming_grouped_retained_registers_.begin() +
              static_cast<std::ptrdiff_t>(grouped_retained_leaves_.size()));
      if (auto mapped =
              MapAggregateLeafRegisters(grouped_retained_leaves_, grouped_retained_leaf_registers_,
                                        grouped_sorted_register_map_);
          !mapped.has_value()) {
        return mapped;
      }
      BuildGroupedKeyRegisterMap();
    } else {
      grouped_representative_registers_.clear();
      grouped_representative_register_map_.clear();
      grouped_retained_leaves_.clear();
      grouped_retained_leaf_registers_.clear();
      grouped_sorted_register_map_.clear();
      grouped_key_expression_registers_.assign(expressions_.size(), std::nullopt);
    }
    return {};
  }

  void ClearStreamingAggregateContext() noexcept {
    aggregate_core_ = nullptr;
    aggregate_program_ = nullptr;
    aggregate_ = nullptr;
    having_ = nullptr;
    leaf_ = nullptr;
    table_scan_ = nullptr;
    rowid_lookup_ = nullptr;
    index_scan_ = nullptr;
    filter_ = nullptr;
    projection_ = nullptr;
    distinct_ = nullptr;
    cursor_.reset();
    index_cursor_.reset();
    distinct_relation_.reset();
    grouped_sorter_.reset();
    global_aggregate_query_ = false;
    global_representative_query_ = false;
    grouped_aggregate_query_ = false;
    global_filtered_controller_defaults_ = false;
    active_source_column_begin_ = 0;
    active_source_column_count_ = bound_select_->source_columns().size();
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingGuards(const StreamingCorePlan& core,
                                                         Label skipped) {
    if (core.filter == nullptr) {
      return {};
    }
    for (const BoundExpressionId predicate : core.filter->guards) {
      if (auto emitted = EmitPredicate(predicate, skipped); !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingFilters(const StreamingCorePlan& core,
                                                          Label rejected) {
    if (core.filter == nullptr) {
      return {};
    }
    for (const BoundExpressionId predicate : core.filter->predicates) {
      if (auto emitted = EmitPredicate(predicate, rejected); !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingProjection(
      const BoundSelectCore& select, SelectOutputMode mode = SelectOutputMode::Streaming) {
    if (select.result_columns.size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("streaming projection width is invalid"));
    }
    for (std::size_t index = 0; index < select.result_columns.size(); ++index) {
      if (auto emitted = EmitProjectionValue(
              select.result_columns[index].expression,
              RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)}, mode);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingMembership(const StreamingCorePlan& core,
                                                             Label duplicate) {
    if (!core.relation.has_value()) {
      return {};
    }
    auto inserted =
        ConvertProgramResult(AssumeValue(builder_).EmitInsertRelation(
                                 *core.relation, result_block_first_,
                                 static_cast<std::uint32_t>(bound_select_->result_columns().size()),
                                 RelationInsertMode::kKeepExisting, duplicate),
                             "unable to emit streaming DISTINCT membership");
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingClose(const StreamingCorePlan& core,
                                                        bool close_cursor) {
    if (close_cursor) {
      if (!core.cursor.has_value()) {
        return std::unexpected(InternalFailure("streaming core has no cursor to close"));
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = *core.cursor});
          !closed.has_value()) {
        return closed;
      }
    }
    if (core.relation.has_value()) {
      if (auto closed = Append(CloseRelationInstruction{.relation = *core.relation});
          !closed.has_value()) {
        return closed;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingResult(Label skipped, Label limit_completion,
                                                         bool apply_offset) {
    if (apply_offset) {
      if (auto offset = EmitOffset(skipped); !offset.has_value()) {
        return offset;
      }
    }
    if (auto result = Append(ResultRowInstruction{
            .first = result_block_first_,
            .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
        });
        !result.has_value()) {
      return result;
    }
    if (limit_ != nullptr) {
      return EmitDistinctLimitAfterRow(DistinctLimitTargets{
          .advance = skipped,
          .completion = limit_completion,
      });
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJoinedRow(const StreamingCorePlan& core,
                                                   const BoundSelectCore& select,
                                                   const SelectOutputContext& output,
                                                   Label rejected, std::optional<Label> stopped) {
    switch (output.mode) {
      case SelectOutputMode::Streaming:
        if (!core.relation.has_value()) {
          if (auto offset = EmitOffset(rejected); !offset.has_value()) {
            return offset;
          }
        }
        if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
          return projected;
        }
        if (auto membership = EmitStreamingMembership(core, rejected); !membership.has_value()) {
          return membership;
        }
        return EmitStreamingResult(rejected, stopped.value_or(rejected), core.relation.has_value());
      case SelectOutputMode::Ordered:
        if (output.ordered_layout == nullptr || !output.ordered_record_first.has_value()) {
          return std::unexpected(InternalFailure("joined ordering destination is incomplete"));
        }
        if (OrderedLayoutUsesInputFields(*output.ordered_layout)) {
          if (auto projected = EmitStreamingProjection(select, output.mode);
              !projected.has_value()) {
            return projected;
          }
          if (auto membership = EmitStreamingMembership(core, rejected); !membership.has_value()) {
            return membership;
          }
        }
        return EmitOrderedCandidate(*output.ordered_layout, output.ordered_sorter,
                                    output.ordered_top_n, output.ordered_full_key,
                                    *output.ordered_record_first, rejected);
      case SelectOutputMode::Materialized:
        if (!output.materialized_target.has_value()) {
          return std::unexpected(InternalFailure("joined set destination is incomplete"));
        }
        if (auto projected = EmitStreamingProjection(select, output.mode); !projected.has_value()) {
          return projected;
        }
        return EmitMaterializedSetInsert(*output.materialized_target, output.materialized_count,
                                         core.relation);
      case SelectOutputMode::DemandOne:
        if (!stopped.has_value()) {
          return std::unexpected(InternalFailure("joined demand-one destination has no stop edge"));
        }
        if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
          return projected;
        }
        if (auto result = Append(ResultRowInstruction{
                .first = result_block_first_,
                .count = static_cast<std::uint32_t>(select.result_columns.size()),
            });
            !result.has_value()) {
          return result;
        }
        return EmitJump(*stopped);
    }
    return std::unexpected(InternalFailure("joined output destination is invalid"));
  }

  [[nodiscard]] LoweringResult<void> EmitRightJoinKey(const StreamingCorePlan& core,
                                                      std::size_t depth) {
    const JoinedSourcePlan& source = core.joined_sources[depth];
    if (!source.cursor.has_value() || !source.right_key_first.has_value() ||
        source.right_key_count == 0U) {
      return std::unexpected(InternalFailure("RIGHT identity has no cursor or registers"));
    }
    const auto& select = std::get<BoundSelectCore>(*core.bound);
    const BoundTableSource& bound_source = select.table_sources[depth];
    const CatalogSnapshot& catalog = *bound_select_->catalog();
    if (!bound_source.table.has_value() || !catalog.table(*bound_source.table).without_rowid) {
      return EmitCursorRowIdValue(source.source, *source.right_key_first);
    }
    const auto primary_id = catalog.primary_key_index(*bound_source.table);
    if (!primary_id.has_value()) {
      return std::unexpected(InternalFailure("RIGHT identity has no primary index"));
    }
    const CatalogIndex& primary = catalog.index(*primary_id);
    for (std::uint32_t index = 0; index < source.right_key_count; ++index) {
      const auto* column = std::get_if<ColumnId>(&primary.terms[index].target);
      if (column == nullptr || column->value >= bound_source.source_column_count) {
        return std::unexpected(InternalFailure("RIGHT identity has an invalid primary term"));
      }
      const std::size_t field = bound_source.source_column_begin + column->value;
      if (field >= source_cursor_fields_.size()) {
        return std::unexpected(InternalFailure("RIGHT identity has no source field"));
      }
      const auto& mapped_field = source_cursor_fields_[field];
      if (!mapped_field.has_value()) {
        return std::unexpected(InternalFailure("RIGHT identity has no source field"));
      }
      if (auto read = Append(ReadFieldInstruction{
              .cursor = *source.cursor,
              .field = *mapped_field,
              .output = RegisterId{source.right_key_first->value() + index},
          });
          !read.has_value()) {
        return read;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJoinedNullRow(const JoinedSourcePlan& source,
                                                       bool enabled = true) {
    if (auto driver = Append(NullRowInstruction{
            .cursor = AssumeValue(source.cursor),
            .enabled = enabled,
        });
        !driver.has_value()) {
      return driver;
    }
    if (source.table_cursor.has_value()) {
      return Append(NullRowInstruction{.cursor = *source.table_cursor, .enabled = enabled});
    }
    return {};
  }

  template <typename RowConsumer>
  [[nodiscard]] LoweringResult<void> EmitJoinedLoop(
      const StreamingCorePlan& core, std::size_t depth, RowConsumer& consume, bool always_stops,
      std::optional<Label> stopped, std::span<const RightJoinContinuation> replays) {
    const JoinedSourcePlan& source = core.joined_sources[depth];
    if (source.access == nullptr || !source.cursor.has_value()) {
      return std::unexpected(InternalFailure("joined loop has no source cursor"));
    }
    const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&source.access->payload);
    const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&source.access->payload);
    const bool has_inner = depth + 1U < core.joined_sources.size();
    const bool left = source.join != nullptr && SourceJoinPreservesLeft(source.join->join);
    const bool right = source.join != nullptr && SourceJoinPreservesRight(source.join->join);
    const auto replay = std::ranges::find(replays, depth, &RightJoinContinuation::depth);
    if (right && (replay == replays.end() || !source.replay_mode.has_value() ||
                  !source.right_matches.has_value() || !source.right_key_first.has_value())) {
      return std::unexpected(InternalFailure("RIGHT source has no replay state"));
    }
    const bool has_on =
        std::ranges::any_of(core.joined_sources, [&](const JoinedSourcePlan& candidate) {
          return candidate.join != nullptr && candidate.join->on.has_value() &&
                 candidate.join->condition_source == source.source;
        });
    const bool has_predicate = core.filter != nullptr &&
                               std::ranges::contains(core.filter->predicate_sources, source.source);
    const bool can_advance = right || has_inner || has_predicate || has_on || !always_stops;
    auto row = CreateLabel();
    auto done = CreateLabel();
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    std::optional<Label> index_end;
    if (index_scan != nullptr) {
      if (!source.index_key_first.has_value()) {
        return std::unexpected(InternalFailure("joined index has no retained prefix registers"));
      }
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      index_end = *label;
    }
    std::optional<Label> published;
    if (right) {
      published = replay->published;
    }
    if (left) {
      if (!source.matched.has_value()) {
        return std::unexpected(InternalFailure("LEFT source has no match register"));
      }
      if (!right) {
        auto label = CreateLabel();
        if (!label.has_value()) {
          return std::unexpected(std::move(label.error()));
        }
        published = *label;
      }
      auto zero = EnsureZeroConstant();
      if (!zero.has_value()) {
        return std::unexpected(std::move(zero.error()));
      }
      if (auto initialized =
              Append(LoadConstantInstruction{.constant = *zero, .output = *source.matched});
          !initialized.has_value()) {
        return initialized;
      }
    }
    std::optional<Label> advance;
    if (can_advance) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      advance = *label;
    }
    const CursorId cursor = *source.cursor;
    if (lookup != nullptr) {
      auto key = EmitRowIdLookupKey(lookup->key);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      auto sought = ConvertProgramResult(AssumeValue(builder_).EmitSeekRowId(cursor, *key, *done),
                                         "unable to position joined rowid lookup");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else if (index_scan != nullptr) {
      if (auto keys = EmitIndexKeyExpressions(*index_scan, *done); !keys.has_value()) {
        return keys;
      }
      const RegisterId first = AssumeValue(source.index_key_first);
      auto count = EmitIndexKeyBlock(*index_scan, nullptr, first,
                                     static_cast<std::uint32_t>(index_scan->equalities.size()));
      if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
      }
      auto sought =
          ConvertProgramResult(AssumeValue(builder_).EmitSeekIndex(cursor, first, *count, *done,
                                                                   IndexSeekMode::kGreaterOrEqual),
                               "unable to position joined index prefix");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *done),
                                          "unable to position joined source");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (index_scan != nullptr) {
      auto checked =
          ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
                                   cursor, AssumeValue(source.index_key_first),
                                   static_cast<std::uint32_t>(index_scan->equalities.size()),
                                   AssumeValue(index_end), IndexRangeEndMode::kInclusive),
                               "unable to check joined index prefix");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (source.table_cursor.has_value()) {
      if (!index_rowid_register_.has_value()) {
        return std::unexpected(InternalFailure("joined table lookup has no rowid register"));
      }
      if (auto sought = EmitIndexTableLookup(AssumeValue(builder_), cursor, *source.table_cursor,
                                             AssumeValue(index_rowid_register_));
          !sought.has_value()) {
        return sought;
      }
    }
    if (left || right) {
      if (source.join->on.has_value()) {
        if (auto on = EmitPredicate(*source.join->on, advance.value_or(*done)); !on.has_value()) {
          return on;
        }
      }
      if (left) {
        auto one = EnsureOneConstant();
        if (!one.has_value()) {
          return std::unexpected(std::move(one.error()));
        }
        if (auto matched = Append(
                LoadConstantInstruction{.constant = *one, .output = AssumeValue(source.matched)});
            !matched.has_value()) {
          return matched;
        }
      }
    }
    if (right) {
      if (auto key = EmitRightJoinKey(core, depth); !key.has_value()) {
        return key;
      }
      auto inserted = ConvertProgramResult(
          AssumeValue(builder_).EmitInsertRelation(
              *source.right_matches, *source.right_key_first, source.right_key_count,
              RelationInsertMode::kKeepExisting, replay->published),
          "unable to record RIGHT match");
      if (!inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
    }
    if (left || right) {
      if (auto bound = BindLabel(AssumeValue(published)); !bound.has_value()) {
        return bound;
      }
    }
    for (const JoinedSourcePlan& candidate : core.joined_sources) {
      if (candidate.join != nullptr && !SourceJoinPreservesLeft(candidate.join->join) &&
          !SourceJoinPreservesRight(candidate.join->join)) {
        const auto& condition = candidate.join->on;
        if (condition.has_value() && candidate.join->condition_source == source.source) {
          if (auto on = EmitPredicate(*condition, advance.value_or(*done)); !on.has_value()) {
            return on;
          }
        }
      }
    }
    if (core.filter != nullptr) {
      for (std::size_t index = 0; index < core.filter->predicates.size(); ++index) {
        if (core.filter->predicate_sources[index] == source.source) {
          if (auto filtered =
                  EmitPredicate(core.filter->predicates[index], advance.value_or(*done));
              !filtered.has_value()) {
            return filtered;
          }
        }
      }
    }
    if (has_inner) {
      if (auto inner = EmitJoinedLoop(core, depth + 1U, consume, always_stops, stopped, replays);
          !inner.has_value()) {
        return inner;
      }
    } else if (auto emitted = consume(advance.value_or(*done), stopped); !emitted.has_value()) {
      return emitted;
    }
    if (advance.has_value()) {
      if (auto bound = BindLabel(*advance); !bound.has_value()) {
        return bound;
      }
      if (right) {
        auto ordinary = CreateLabel();
        if (!ordinary.has_value()) {
          return std::unexpected(std::move(ordinary.error()));
        }
        if (auto normal = EmitJumpIf(*source.replay_mode, JumpCondition::kIfFalse, *ordinary);
            !normal.has_value()) {
          return normal;
        }
        auto next = ConvertProgramResult(
            AssumeValue(builder_).EmitNext(source.table_cursor.value_or(cursor),
                                           replay->replay_candidate),
            "unable to advance RIGHT replay");
        if (!next.has_value()) {
          return std::unexpected(std::move(next.error()));
        }
        if (auto finished = EmitJump(replay->replay_exhausted.value_or(replay->replay_done));
            !finished.has_value()) {
          return finished;
        }
        if (auto bound = BindLabel(*ordinary); !bound.has_value()) {
          return bound;
        }
      }
      if (source.table_cursor.has_value()) {
        if (auto cleared =
                Append(NullRowInstruction{.cursor = *source.table_cursor, .enabled = false});
            !cleared.has_value()) {
          return cleared;
        }
      }
      if (lookup != nullptr) {
        if (auto cleared = Append(NullRowInstruction{.cursor = cursor, .enabled = false});
            !cleared.has_value()) {
          return cleared;
        }
        if (auto finished = EmitJump(*done); !finished.has_value()) {
          return finished;
        }
      } else {
        auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *row),
                                         "unable to advance joined source");
        if (!next.has_value()) {
          return std::unexpected(std::move(next.error()));
        }
        if (index_scan != nullptr) {
          if (auto finished = EmitJump(*done); !finished.has_value()) {
            return finished;
          }
        }
      }
    }
    if (index_end.has_value()) {
      if (auto bound = BindLabel(*index_end); !bound.has_value()) {
        return bound;
      }
      if (auto cleared = Append(NullRowInstruction{.cursor = cursor, .enabled = false});
          !cleared.has_value()) {
        return cleared;
      }
    }
    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return bound;
    }
    if (right) {
      if (auto finished =
              EmitJumpIf(*source.replay_mode, JumpCondition::kIfTrue, replay->replay_done);
          !finished.has_value()) {
        return finished;
      }
    }
    if (left) {
      auto exhausted = CreateLabel();
      if (!exhausted.has_value()) {
        return std::unexpected(std::move(exhausted.error()));
      }
      if (auto skip = EmitJumpIf(AssumeValue(source.matched), JumpCondition::kIfTrue, *exhausted);
          !skip.has_value()) {
        return skip;
      }
      if (auto null_row = EmitJoinedNullRow(source); !null_row.has_value()) {
        return null_row;
      }
      auto one = EnsureOneConstant();
      if (!one.has_value()) {
        return std::unexpected(std::move(one.error()));
      }
      if (auto consumed = Append(
              LoadConstantInstruction{.constant = *one, .output = AssumeValue(source.matched)});
          !consumed.has_value()) {
        return consumed;
      }
      if (auto publish = EmitJump(AssumeValue(published)); !publish.has_value()) {
        return publish;
      }
      return BindLabel(*exhausted);
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitRightJoinReplay(const StreamingCorePlan& core,
                                                         const RightJoinContinuation& replay) {
    const JoinedSourcePlan& source = core.joined_sources[replay.depth];
    auto one = EnsureOneConstant();
    if (!one.has_value()) {
      return std::unexpected(std::move(one.error()));
    }
    if (auto mode = Append(LoadConstantInstruction{
            .constant = *one,
            .output = AssumeValue(source.replay_mode),
        });
        !mode.has_value()) {
      return mode;
    }
    for (std::size_t index = 0; index < replay.depth; ++index) {
      if (auto null_row = EmitJoinedNullRow(core.joined_sources[index]); !null_row.has_value()) {
        return null_row;
      }
    }
    if (source.table_cursor.has_value()) {
      if (auto parked = Append(NullRowInstruction{.cursor = AssumeValue(source.cursor)});
          !parked.has_value()) {
        return parked;
      }
    }
    const CursorId cursor = source.table_cursor.value_or(AssumeValue(source.cursor));
    const Label exhausted = replay.replay_exhausted.value_or(replay.replay_done);
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, exhausted),
                                        "unable to position RIGHT replay");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(replay.replay_candidate); !bound.has_value()) {
      return bound;
    }
    if (auto key = EmitRightJoinKey(core, replay.depth); !key.has_value()) {
      return key;
    }
    auto skipped = CreateLabel();
    if (!skipped.has_value()) {
      return std::unexpected(std::move(skipped.error()));
    }
    auto contains = ConvertProgramResult(
        AssumeValue(builder_).EmitContainsRelation(AssumeValue(source.right_matches),
                                                   AssumeValue(source.right_key_first),
                                                   source.right_key_count, *skipped),
        "unable to test RIGHT matched key");
    if (!contains.has_value()) {
      return std::unexpected(std::move(contains.error()));
    }
    if (auto publish = EmitJump(replay.published); !publish.has_value()) {
      return publish;
    }
    if (auto bound = BindLabel(*skipped); !bound.has_value()) {
      return bound;
    }
    auto next =
        ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, replay.replay_candidate),
                             "unable to skip matched RIGHT row");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (replay.replay_exhausted.has_value()) {
      if (auto bound = BindLabel(*replay.replay_exhausted); !bound.has_value()) {
        return bound;
      }
      if (auto cleared =
              Append(NullRowInstruction{.cursor = AssumeValue(source.cursor), .enabled = false});
          !cleared.has_value()) {
        return cleared;
      }
    }
    if (auto bound = BindLabel(replay.replay_done); !bound.has_value()) {
      return bound;
    }
    for (std::size_t index = 0; index < replay.depth; ++index) {
      if (auto cleared = EmitJoinedNullRow(core.joined_sources[index], false);
          !cleared.has_value()) {
        return cleared;
      }
    }
    return {};
  }

  template <typename RowConsumer>
  [[nodiscard]] LoweringResult<void> EmitJoinedSourceRows(const StreamingCorePlan& core,
                                                          RowConsumer& consume, bool always_stops,
                                                          std::optional<Label> final_completion,
                                                          std::optional<RelationId> membership) {
    if (core.join == nullptr || core.joined_sources.size() < 2U ||
        core.joined_sources.size() > kMaximumSelectSources) {
      return std::unexpected(InternalFailure("joined core has invalid source shape"));
    }
    std::vector<RightJoinContinuation> replays;
    for (std::size_t depth = 0; depth < core.joined_sources.size(); ++depth) {
      if (!core.joined_sources[depth].right_matches.has_value()) {
        continue;
      }
      auto published = CreateLabel();
      auto candidate = CreateLabel();
      auto done = CreateLabel();
      if (!published.has_value()) {
        return std::unexpected(std::move(published.error()));
      }
      if (!candidate.has_value()) {
        return std::unexpected(std::move(candidate.error()));
      }
      if (!done.has_value()) {
        return std::unexpected(std::move(done.error()));
      }
      std::optional<Label> exhausted;
      if (core.joined_sources[depth].table_cursor.has_value()) {
        auto label = CreateLabel();
        if (!label.has_value()) {
          return std::unexpected(std::move(label.error()));
        }
        exhausted = *label;
      }
      replays.push_back(RightJoinContinuation{
          .depth = depth,
          .published = *published,
          .replay_candidate = *candidate,
          .replay_done = *done,
          .replay_exhausted = exhausted,
      });
    }
    SetStreamingCoreContext(core);
    auto after = CreateLabel();
    if (!after.has_value()) {
      return std::unexpected(std::move(after.error()));
    }
    if (auto guards = EmitStreamingGuards(core, *after); !guards.has_value()) {
      return guards;
    }
    if (membership.has_value()) {
      if (auto opened = Append(OpenRelationInstruction{.relation = *membership});
          !opened.has_value()) {
        return opened;
      }
    }
    for (const JoinedSourcePlan& source : core.joined_sources) {
      if (!source.cursor.has_value()) {
        return std::unexpected(InternalFailure("joined source has no cursor descriptor"));
      }
      const CursorId cursor = *source.cursor;
      if (source.index_key_first.has_value() &&
          (!replays.empty() ||
           (source.join != nullptr && SourceJoinPreservesLeft(source.join->join)))) {
        const auto* index = std::get_if<PhysicalIndexScanNode>(&source.access->payload);
        if (index == nullptr) {
          return std::unexpected(InternalFailure("joined prefix initialization has no index"));
        }
        const RegisterId first = AssumeValue(source.index_key_first);
        auto null = EnsureNullConstant();
        if (!null.has_value()) {
          return std::unexpected(std::move(null.error()));
        }
        for (std::size_t term = 0; term < index->equalities.size(); ++term) {
          if (auto initialized = Append(LoadConstantInstruction{
                  .constant = *null,
                  .output = RegisterId{first.value() + static_cast<std::uint32_t>(term)},
              });
              !initialized.has_value()) {
            return initialized;
          }
        }
      }
      if (!replays.empty() && source.matched.has_value()) {
        auto zero = EnsureZeroConstant();
        if (!zero.has_value()) {
          return std::unexpected(std::move(zero.error()));
        }
        if (auto initialized = Append(LoadConstantInstruction{
                .constant = *zero,
                .output = AssumeValue(source.matched),
            });
            !initialized.has_value()) {
          return initialized;
        }
      }
      if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
        return opened;
      }
      if (source.table_cursor.has_value()) {
        if (auto opened =
                Append(OpenReadCursorInstruction{.cursor = AssumeValue(source.table_cursor)});
            !opened.has_value()) {
          return opened;
        }
      }
      if (source.right_matches.has_value()) {
        auto zero = EnsureZeroConstant();
        if (!zero.has_value()) {
          return std::unexpected(std::move(zero.error()));
        }
        if (auto initialized = Append(LoadConstantInstruction{
                .constant = *zero,
                .output = AssumeValue(source.replay_mode),
            });
            !initialized.has_value()) {
          return initialized;
        }
        if (auto opened =
                Append(OpenRelationInstruction{.relation = AssumeValue(source.right_matches)});
            !opened.has_value()) {
          return opened;
        }
      }
    }
    std::optional<Label> stopped;
    if (final_completion.has_value()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      stopped = *label;
    }
    if (auto loop = EmitJoinedLoop(core, 0U, consume, always_stops, stopped, replays);
        !loop.has_value()) {
      return loop;
    }
    for (const RightJoinContinuation& replay : replays) {
      if (auto emitted = EmitRightJoinReplay(core, replay); !emitted.has_value()) {
        return emitted;
      }
    }
    const auto close = [&]() -> LoweringResult<void> {
      for (const JoinedSourcePlan& source : std::views::reverse(core.joined_sources)) {
        if (source.right_matches.has_value()) {
          if (auto closed = Append(CloseRelationInstruction{.relation = *source.right_matches});
              !closed.has_value()) {
            return closed;
          }
        }
        if (source.table_cursor.has_value()) {
          if (auto closed = Append(CloseCursorInstruction{.cursor = *source.table_cursor});
              !closed.has_value()) {
            return closed;
          }
        }
        if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(source.cursor)});
            !closed.has_value()) {
          return closed;
        }
      }
      if (membership.has_value()) {
        return Append(CloseRelationInstruction{.relation = *membership});
      }
      return {};
    };
    if (auto closed = close(); !closed.has_value()) {
      return closed;
    }
    if (stopped.has_value()) {
      if (auto jumped = EmitJump(*after); !jumped.has_value()) {
        return jumped;
      }
      if (auto bound = BindLabel(*stopped); !bound.has_value()) {
        return bound;
      }
      if (auto closed = close(); !closed.has_value()) {
        return closed;
      }
      if (auto jumped = EmitJump(AssumeValue(final_completion)); !jumped.has_value()) {
        return jumped;
      }
    }
    return BindLabel(*after);
  }

  [[nodiscard]] LoweringResult<void> EmitJoinedCore(const StreamingCorePlan& core,
                                                    const BoundSelectCore& select,
                                                    const SelectOutputContext& output) {
    if (core.aggregate != nullptr) {
      return std::unexpected(InternalFailure("aggregate join bypassed its aggregate consumer"));
    }
    const bool may_stop = output.mode == SelectOutputMode::DemandOne ||
                          (output.mode == SelectOutputMode::Streaming && limit_ != nullptr);
    if (may_stop && !output.final_completion.has_value()) {
      return std::unexpected(InternalFailure("joined stop has no final completion"));
    }
    auto consume = [&](Label rejected, std::optional<Label> stopped) {
      return EmitJoinedRow(core, select, output, rejected, stopped);
    };
    return EmitJoinedSourceRows(
        core, consume, output.mode == SelectOutputMode::DemandOne,
        may_stop ? output.final_completion : std::nullopt,
        output.mode == SelectOutputMode::DemandOne ? std::nullopt : core.relation);
  }

  [[nodiscard]] LoweringResult<void> EmitJoinedAggregateInput(const StreamingCorePlan& core) {
    if (global_aggregate_query_) {
      if (auto opened = OpenGlobalAggregates(); !opened.has_value()) {
        return opened;
      }
    } else {
      if (auto constants = PrepareGroupedAggregateConstants(); !constants.has_value()) {
        return constants;
      }
      auto input = MakeGroupedAggregateInput();
      if (!input.has_value()) {
        return std::unexpected(std::move(input.error()));
      }
      if (auto initialized = lowering_internal::EmitGroupedAggregateInitialization(*input, *this);
          !initialized.has_value()) {
        return initialized;
      }
    }
    auto consume = [&](Label, std::optional<Label>) -> LoweringResult<void> {
      if (global_aggregate_query_) {
        return StepGlobalAggregates();
      }
      auto input = MakeGroupedAggregateInput();
      if (!input.has_value()) {
        return std::unexpected(std::move(input.error()));
      }
      return lowering_internal::EmitGroupedAggregateRow(*input, *this);
    };
    if (auto rows = EmitJoinedSourceRows(core, consume, false, std::nullopt, std::nullopt);
        !rows.has_value()) {
      return rows;
    }
    if (global_aggregate_query_) {
      return EmitGlobalAggregateCompletion();
    }
    auto input = MakeGroupedAggregateInput();
    if (!input.has_value()) {
      return std::unexpected(std::move(input.error()));
    }
    return lowering_internal::EmitGroupedAggregateCompletion(*input, *this);
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingTableCore(const StreamingCorePlan& core,
                                                            const BoundSelectCore& select,
                                                            Label final_completion) {
    if (!core.cursor.has_value()) {
      return std::unexpected(InternalFailure("streaming table core has no cursor"));
    }
    SetStreamingCoreContext(core);
    const CursorId cursor = *core.cursor;
    auto after_core = CreateLabel();
    auto opened_completion = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    std::optional<Label> limit_completion;
    if (!after_core.has_value()) {
      return std::unexpected(std::move(after_core.error()));
    }
    if (!opened_completion.has_value()) {
      return std::unexpected(std::move(opened_completion.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      limit_completion = *label;
    }
    if (auto guards = EmitStreamingGuards(core, *after_core); !guards.has_value()) {
      return guards;
    }
    if (core.relation.has_value()) {
      if (auto opened = Append(OpenRelationInstruction{.relation = *core.relation});
          !opened.has_value()) {
        return opened;
      }
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
      return opened;
    }
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *opened_completion),
                             "unable to emit streaming table rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto filters = EmitStreamingFilters(core, *advance); !filters.has_value()) {
      return filters;
    }
    if (!core.relation.has_value()) {
      if (auto offset = EmitOffset(*advance); !offset.has_value()) {
        return offset;
      }
    }
    if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
      return projected;
    }
    if (auto membership = EmitStreamingMembership(core, *advance); !membership.has_value()) {
      return membership;
    }
    const Label row_limit_completion =
        limit_completion.has_value() ? *limit_completion : final_completion;
    if (auto result =
            EmitStreamingResult(*advance, row_limit_completion, core.relation.has_value());
        !result.has_value()) {
      return result;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *row),
                                     "unable to emit streaming table advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*opened_completion); !bound.has_value()) {
      return bound;
    }
    if (auto closed = EmitStreamingClose(core, true); !closed.has_value()) {
      return closed;
    }
    if (auto jumped = EmitJump(*after_core); !jumped.has_value()) {
      return jumped;
    }
    if (limit_completion.has_value()) {
      if (auto bound = BindLabel(*limit_completion); !bound.has_value()) {
        return bound;
      }
      if (auto closed = EmitStreamingClose(core, true); !closed.has_value()) {
        return closed;
      }
      if (auto jumped = EmitJump(final_completion); !jumped.has_value()) {
        return jumped;
      }
    }
    return BindLabel(*after_core);
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingSingleCore(const StreamingCorePlan& core,
                                                             const BoundSelectCore& select,
                                                             Label final_completion) {
    if (core.join != nullptr) {
      return EmitJoinedCore(core, select,
                            SelectOutputContext{
                                .mode = SelectOutputMode::Streaming,
                                .core = &core,
                                .final_completion = final_completion,
                            });
    }
    SetStreamingCoreContext(core);
    auto after_core = CreateLabel();
    auto opened_completion = CreateLabel();
    std::optional<Label> limit_completion;
    if (!after_core.has_value()) {
      return std::unexpected(std::move(after_core.error()));
    }
    if (!opened_completion.has_value()) {
      return std::unexpected(std::move(opened_completion.error()));
    }
    if (limit_ != nullptr && core.relation.has_value()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      limit_completion = *label;
    }
    if (auto guards = EmitStreamingGuards(core, *after_core); !guards.has_value()) {
      return guards;
    }
    if (core.relation.has_value()) {
      if (auto opened = Append(OpenRelationInstruction{.relation = *core.relation});
          !opened.has_value()) {
        return opened;
      }
    }
    if (auto filters = EmitStreamingFilters(core, *opened_completion); !filters.has_value()) {
      return filters;
    }
    if (!core.relation.has_value()) {
      if (auto offset = EmitOffset(*opened_completion); !offset.has_value()) {
        return offset;
      }
    }
    if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
      return projected;
    }
    if (auto membership = EmitStreamingMembership(core, *opened_completion);
        !membership.has_value()) {
      return membership;
    }
    const Label row_limit_completion =
        limit_completion.has_value() ? *limit_completion : final_completion;
    if (auto result = EmitStreamingResult(*opened_completion, row_limit_completion,
                                          core.relation.has_value());
        !result.has_value()) {
      return result;
    }
    if (auto bound = BindLabel(*opened_completion); !bound.has_value()) {
      return bound;
    }
    if (auto closed = EmitStreamingClose(core, false); !closed.has_value()) {
      return closed;
    }
    if (auto jumped = EmitJump(*after_core); !jumped.has_value()) {
      return jumped;
    }
    if (limit_completion.has_value()) {
      if (auto bound = BindLabel(*limit_completion); !bound.has_value()) {
        return bound;
      }
      if (auto closed = EmitStreamingClose(core, false); !closed.has_value()) {
        return closed;
      }
      if (auto jumped = EmitJump(final_completion); !jumped.has_value()) {
        return jumped;
      }
    }
    return BindLabel(*after_core);
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingValuesCore(const BoundValuesCore& values,
                                                             Label final_completion) {
    cursor_.reset();
    filter_ = nullptr;
    projection_ = nullptr;
    distinct_ = nullptr;
    distinct_relation_.reset();
    for (const std::vector<BoundExpressionId>& row : values.rows) {
      if (row.size() != bound_select_->result_columns().size()) {
        return std::unexpected(InternalFailure("VALUES row width is invalid"));
      }
      auto next_row = CreateLabel();
      if (!next_row.has_value()) {
        return std::unexpected(std::move(next_row.error()));
      }
      if (values.schedule == BoundValuesEvaluationSchedule::kConsumerFiltered) {
        if (auto offset = EmitOffset(*next_row); !offset.has_value()) {
          return offset;
        }
      }
      for (std::size_t index = 0; index < row.size(); ++index) {
        if (auto emitted = EmitProjectionValue(
                row[index],
                RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return emitted;
        }
      }
      if (values.schedule == BoundValuesEvaluationSchedule::kProducerEager) {
        if (auto offset = EmitOffset(*next_row); !offset.has_value()) {
          return offset;
        }
      }
      if (auto result = Append(ResultRowInstruction{
              .first = result_block_first_,
              .count = static_cast<std::uint32_t>(row.size()),
          });
          !result.has_value()) {
        return result;
      }
      if (limit_ != nullptr) {
        if (auto limited = EmitDistinctLimitAfterRow(DistinctLimitTargets{
                .advance = *next_row,
                .completion = final_completion,
            });
            !limited.has_value()) {
          return limited;
        }
      }
      if (auto bound = BindLabel(*next_row); !bound.has_value()) {
        return bound;
      }
    }
    return {};
  }

  [[nodiscard]] static bool AggregateCoreAlwaysProducesRow(const StreamingCorePlan& core) noexcept {
    return core.aggregate != nullptr && core.having == nullptr &&
           (core.aggregate->strategy == PhysicalAggregateStrategy::kGlobal ||
            core.aggregate->strategy == PhysicalAggregateStrategy::kIndexedMinMax);
  }

  [[nodiscard]] LoweringResult<void> EmitAggregateCore(const StreamingCorePlan& core,
                                                       SelectOutputContext output) {
    if (core.aggregate == nullptr || output.core != &core) {
      return std::unexpected(InternalFailure("aggregate core output metadata is invalid"));
    }
    auto after = CreateLabel();
    if (!after.has_value()) {
      return std::unexpected(std::move(after.error()));
    }
    std::optional<Label> completed;
    if (output.mode != SelectOutputMode::DemandOne || !AggregateCoreAlwaysProducesRow(core)) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      completed = *label;
    }
    const bool needs_stopped = output.mode == SelectOutputMode::DemandOne ||
                               (output.mode == SelectOutputMode::Streaming && limit_ != nullptr);
    std::optional<Label> stopped;
    if (needs_stopped) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      stopped = *label;
    }
    if (auto context = SetStreamingAggregateCoreContext(core); !context.has_value()) {
      return context;
    }
    active_aggregate_output_ = output;
    aggregate_core_stopped_label_ = stopped.value_or(*after);
    aggregate_core_completion_label_ = completed.value_or(*aggregate_core_stopped_label_);

    const auto close_relation = [&]() -> LoweringResult<void> {
      if (!core.relation.has_value()) {
        return {};
      }
      return Append(CloseRelationInstruction{.relation = *core.relation});
    };
    if (core.relation.has_value()) {
      if (auto opened = Append(OpenRelationInstruction{.relation = *core.relation});
          !opened.has_value()) {
        return opened;
      }
    }
    LoweringResult<void> emitted = core.join != nullptr ? EmitJoinedAggregateInput(core)
                                   : aggregate_program_->group_by.empty()
                                       ? EmitGlobalAggregatePlan()
                                       : EmitGroupedAggregatePlan();
    if (!emitted.has_value()) {
      return emitted;
    }
    if (completed.has_value()) {
      if (auto bound = BindLabel(*completed); !bound.has_value()) {
        return bound;
      }
      if (auto closed = close_relation(); !closed.has_value()) {
        return closed;
      }
      if (stopped.has_value()) {
        if (auto jumped = EmitJump(*after); !jumped.has_value()) {
          return jumped;
        }
      }
    }
    if (stopped.has_value()) {
      if (auto bound = BindLabel(*stopped); !bound.has_value()) {
        return bound;
      }
      if (auto closed = close_relation(); !closed.has_value()) {
        return closed;
      }
      if (output.final_completion.has_value()) {
        if (auto jumped = EmitJump(*output.final_completion); !jumped.has_value()) {
          return jumped;
        }
      } else if (auto jumped = EmitJump(*after); !jumped.has_value()) {
        return jumped;
      }
    }
    if (auto bound = BindLabel(*after); !bound.has_value()) {
      return bound;
    }
    active_aggregate_output_.reset();
    aggregate_core_stopped_label_.reset();
    aggregate_core_completion_label_.reset();
    ClearStreamingAggregateContext();
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingPlan() {
    if (!streaming_query_ || streaming_cores_.empty()) {
      return std::unexpected(InternalFailure("streaming query emission has no cores"));
    }
    auto completion = CreateLabel();
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto limit = EmitLimitInitialization(*completion); !limit.has_value()) {
      return limit;
    }
    for (const StreamingCorePlan& core : streaming_cores_) {
      if (const auto* values = std::get_if<BoundValuesCore>(core.bound); values != nullptr) {
        if (auto emitted = EmitStreamingValuesCore(*values, *completion); !emitted.has_value()) {
          return emitted;
        }
        continue;
      }
      const auto& select = std::get<BoundSelectCore>(*core.bound);
      if (core.aggregate != nullptr) {
        if (auto emitted = EmitAggregateCore(core,
                                             SelectOutputContext{
                                                 .mode = SelectOutputMode::Streaming,
                                                 .core = &core,
                                                 .final_completion = *completion,
                                             });
            !emitted.has_value()) {
          return emitted;
        }
        continue;
      }
      if (core.table_scan != nullptr) {
        if (auto emitted = EmitStreamingTableCore(core, select, *completion);
            !emitted.has_value()) {
          return emitted;
        }
      } else {
        if (auto emitted = EmitStreamingSingleCore(core, select, *completion);
            !emitted.has_value()) {
          return emitted;
        }
      }
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] SorterId OrderedSorter(bool full_key, std::size_t slot) const noexcept {
    return AssumeValue(full_key ? ordered_full_sorters_[slot] : ordered_sorters_[slot]);
  }

  [[nodiscard]] LoweringResult<SorterId> ChooseOrderedSorter(
      bool full_key, std::span<const SorterId> excluded) const {
    const auto& sorters = full_key ? ordered_full_sorters_ : ordered_sorters_;
    for (const std::optional<SorterId>& sorter : sorters) {
      if (!sorter.has_value() || std::ranges::find(excluded, *sorter) != excluded.end()) {
        continue;
      }
      return *sorter;
    }
    return std::unexpected(InternalFailure("ordered compound has no free sorter"));
  }

  [[nodiscard]] bool OrderedLayoutUsesInputFields(
      const LogicalCoreOrderLayout& layout) const noexcept {
    const auto uses_input = [](const LogicalOrderValue& value) {
      return value.kind == LogicalOrderValueKind::kInputField;
    };
    return std::ranges::any_of(layout.key_values, uses_input) ||
           std::ranges::any_of(layout.payload_values, uses_input);
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedLayoutValue(const LogicalOrderValue& value,
                                                            RegisterId destination) {
    if (value.kind == LogicalOrderValueKind::kInputField) {
      if (value.expression.has_value() ||
          value.field_index >= bound_select_->result_columns().size()) {
        return std::unexpected(InternalFailure("ordered compound input field is invalid"));
      }
      return Append(CopyInstruction{
          .input = RegisterId{result_block_first_.value() + value.field_index},
          .output = destination,
      });
    }
    if (!value.expression.has_value() || value.field_index != 0U) {
      return std::unexpected(InternalFailure("ordered compound expression value is invalid"));
    }
    const BoundExpressionId expression = *value.expression;
    if (auto emitted = EmitExpression(expression, Home(expression)); !emitted.has_value()) {
      return emitted;
    }
    return Append(CopyInstruction{
        .input = Home(expression),
        .output = destination,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedCandidate(const LogicalCoreOrderLayout& layout,
                                                          std::optional<SorterId> sorter,
                                                          std::optional<TopNId> top_n,
                                                          bool full_key, RegisterId record_first,
                                                          Label rejected) {
    if (sorter.has_value() == top_n.has_value()) {
      return std::unexpected(InternalFailure("ordered compound candidate capability is invalid"));
    }
    const auto key_count = static_cast<std::uint32_t>(layout.key_values.size());
    const auto admission_key_count =
        full_key
            ? static_cast<std::uint32_t>(layout.key_values.size() + layout.payload_values.size())
            : key_count;
    const auto emit_payload = [&]() -> LoweringResult<void> {
      for (std::size_t index = 0; index < layout.payload_values.size(); ++index) {
        if (auto emitted = EmitOrderedLayoutValue(
                layout.payload_values[index],
                RegisterId{record_first.value() + key_count + static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return emitted;
        }
      }
      return {};
    };
    const auto emit_keys = [&]() -> LoweringResult<void> {
      for (std::size_t index = 0; index < layout.key_values.size(); ++index) {
        if (auto emitted = EmitOrderedLayoutValue(
                layout.key_values[index],
                RegisterId{record_first.value() + static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return emitted;
        }
      }
      return {};
    };
    if (layout.schedule == OrderEvaluationSchedule::kPayloadThenKeys) {
      if (auto payload = emit_payload(); !payload.has_value()) {
        return payload;
      }
      if (auto keys = emit_keys(); !keys.has_value()) {
        return keys;
      }
      if (top_n.has_value()) {
        auto checked =
            ConvertProgramResult(AssumeValue(builder_).EmitCheckTopN(*top_n, record_first,
                                                                     admission_key_count, rejected),
                                 "unable to emit ordered compound top-N admission");
        if (!checked.has_value()) {
          return std::unexpected(std::move(checked.error()));
        }
      }
    } else {
      if (auto keys = emit_keys(); !keys.has_value()) {
        return keys;
      }
      if (top_n.has_value()) {
        auto checked =
            ConvertProgramResult(AssumeValue(builder_).EmitCheckTopN(*top_n, record_first,
                                                                     admission_key_count, rejected),
                                 "unable to emit ordered compound top-N admission");
        if (!checked.has_value()) {
          return std::unexpected(std::move(checked.error()));
        }
      }
      if (auto payload = emit_payload(); !payload.has_value()) {
        return payload;
      }
    }
    const auto value_count =
        static_cast<std::uint32_t>(layout.key_values.size() + layout.payload_values.size());
    if (sorter.has_value()) {
      return Append(InsertSorterInstruction{
          .sorter = *sorter,
          .first_value = record_first,
          .value_count = value_count,
      });
    }
    return Append(InsertTopNInstruction{
        .top_n = *top_n,
        .first_value = record_first,
        .value_count = value_count,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedSelectCore(const StreamingCorePlan& core,
                                                           const BoundSelectCore& select,
                                                           const LogicalCoreOrderLayout& layout,
                                                           std::optional<SorterId> sorter,
                                                           std::optional<TopNId> top_n,
                                                           bool full_key, RegisterId record_first) {
    if (core.join != nullptr) {
      return EmitJoinedCore(core, select,
                            SelectOutputContext{
                                .mode = SelectOutputMode::Ordered,
                                .core = &core,
                                .ordered_layout = &layout,
                                .ordered_sorter = sorter,
                                .ordered_top_n = top_n,
                                .ordered_full_key = full_key,
                                .ordered_record_first = record_first,
                            });
    }
    SetStreamingCoreContext(core);
    auto skipped = CreateLabel();
    if (!skipped.has_value()) {
      return std::unexpected(std::move(skipped.error()));
    }
    if (auto guards = EmitStreamingGuards(core, *skipped); !guards.has_value()) {
      return guards;
    }
    if (core.relation.has_value()) {
      if (auto opened = Append(OpenRelationInstruction{.relation = *core.relation});
          !opened.has_value()) {
        return opened;
      }
    }
    const bool complete_row = OrderedLayoutUsesInputFields(layout);
    if (core.table_scan != nullptr) {
      if (!core.cursor.has_value()) {
        return std::unexpected(InternalFailure("ordered compound table core has no cursor"));
      }
      const CursorId cursor = *core.cursor;
      auto done = CreateLabel();
      auto row = CreateLabel();
      auto advance = CreateLabel();
      if (!done.has_value()) {
        return std::unexpected(std::move(done.error()));
      }
      if (!row.has_value()) {
        return std::unexpected(std::move(row.error()));
      }
      if (!advance.has_value()) {
        return std::unexpected(std::move(advance.error()));
      }
      if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
        return opened;
      }
      auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *done),
                                          "unable to emit ordered compound table rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
      if (auto bound = BindLabel(*row); !bound.has_value()) {
        return bound;
      }
      if (auto filters = EmitStreamingFilters(core, *advance); !filters.has_value()) {
        return filters;
      }
      if (complete_row) {
        if (auto projected = EmitStreamingProjection(select, SelectOutputMode::Ordered);
            !projected.has_value()) {
          return projected;
        }
        if (auto membership = EmitStreamingMembership(core, *advance); !membership.has_value()) {
          return membership;
        }
      }
      if (auto candidate =
              EmitOrderedCandidate(layout, sorter, top_n, full_key, record_first, *advance);
          !candidate.has_value()) {
        return candidate;
      }
      if (auto bound = BindLabel(*advance); !bound.has_value()) {
        return bound;
      }
      auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *row),
                                       "unable to emit ordered compound table advance");
      if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
      }
      if (auto bound = BindLabel(*done); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
        return closed;
      }
    } else {
      if (auto filters = EmitStreamingFilters(core, *skipped); !filters.has_value()) {
        return filters;
      }
      if (complete_row) {
        if (auto projected = EmitStreamingProjection(select, SelectOutputMode::Ordered);
            !projected.has_value()) {
          return projected;
        }
        if (auto membership = EmitStreamingMembership(core, *skipped); !membership.has_value()) {
          return membership;
        }
      }
      if (auto candidate =
              EmitOrderedCandidate(layout, sorter, top_n, full_key, record_first, *skipped);
          !candidate.has_value()) {
        return candidate;
      }
    }
    if (core.relation.has_value()) {
      if (auto closed = Append(CloseRelationInstruction{.relation = *core.relation});
          !closed.has_value()) {
        return closed;
      }
    }
    return BindLabel(*skipped);
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedValuesCore(const BoundValuesCore& values,
                                                           const LogicalCoreOrderLayout& layout,
                                                           std::optional<SorterId> sorter,
                                                           std::optional<TopNId> top_n,
                                                           bool full_key, RegisterId record_first) {
    for (const std::vector<BoundExpressionId>& row : values.rows) {
      auto rejected = CreateLabel();
      if (!rejected.has_value()) {
        return std::unexpected(std::move(rejected.error()));
      }
      if (row.size() != bound_select_->result_columns().size()) {
        return std::unexpected(InternalFailure("ordered compound VALUES width is invalid"));
      }
      for (std::size_t index = 0; index < row.size(); ++index) {
        if (auto emitted = EmitExpression(
                row[index],
                RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return emitted;
        }
      }
      if (auto candidate =
              EmitOrderedCandidate(layout, sorter, top_n, full_key, record_first, *rejected);
          !candidate.has_value()) {
        return candidate;
      }
      if (auto bound = BindLabel(*rejected); !bound.has_value()) {
        return bound;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedCoreRows(const StreamingCorePlan& core,
                                                         bool full_key,
                                                         std::optional<SorterId> sorter,
                                                         std::optional<TopNId> top_n) {
    if (advanced_order_ == nullptr || !ordered_right_first_.has_value() ||
        core.core_index >= advanced_order_->core_plans.size()) {
      return std::unexpected(InternalFailure("ordered compound core resources are incomplete"));
    }
    const LogicalCoreOrderLayout& layout = advanced_order_->core_plans[core.core_index].layout;
    if (core.aggregate != nullptr) {
      return EmitAggregateCore(core, SelectOutputContext{
                                         .mode = SelectOutputMode::Ordered,
                                         .core = &core,
                                         .ordered_layout = &layout,
                                         .ordered_sorter = sorter,
                                         .ordered_top_n = top_n,
                                         .ordered_full_key = full_key,
                                         .ordered_record_first = *ordered_right_first_,
                                     });
    }
    if (const auto* values = std::get_if<BoundValuesCore>(core.bound); values != nullptr) {
      if (auto emitted = EmitOrderedValuesCore(*values, layout, sorter, top_n, full_key,
                                               *ordered_right_first_);
          !emitted.has_value()) {
        return emitted;
      }
    } else {
      const auto& select = std::get<BoundSelectCore>(*core.bound);
      if (auto emitted = EmitOrderedSelectCore(core, select, layout, sorter, top_n, full_key,
                                               *ordered_right_first_);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitTransferOrderedTopN(TopNId source, SorterId target,
                                                             RegisterId record_first) {
    auto empty = CreateLabel();
    auto row = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindTopN(source, *empty),
                                        "unable to emit ordered compound top-N rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    for (std::size_t field = 0; field < OrderedRecordFieldCount(); ++field) {
      if (auto read = Append(ReadTopNFieldInstruction{
              .top_n = source,
              .field = static_cast<std::uint32_t>(field),
              .output = RegisterId{record_first.value() + static_cast<std::uint32_t>(field)},
          });
          !read.has_value()) {
        return read;
      }
    }
    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = target,
            .first_value = record_first,
            .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
        });
        !inserted.has_value()) {
      return inserted;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextTopN(source, *row),
                                     "unable to emit ordered compound top-N advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    return Append(CloseTopNInstruction{.top_n = source});
  }

  [[nodiscard]] LoweringResult<OrderedRun> EmitOrderedCoreRun(const StreamingCorePlan& core,
                                                              bool full_key,
                                                              std::span<const SorterId> excluded) {
    if (advanced_order_ == nullptr || core.core_index >= advanced_order_->core_plans.size()) {
      return std::unexpected(InternalFailure("ordered compound core plan is incomplete"));
    }
    auto selected = ChooseOrderedSorter(full_key, excluded);
    if (!selected.has_value()) {
      return std::unexpected(std::move(selected.error()));
    }
    const SorterId sorter = *selected;
    const PhysicalCoreOrderPlan& plan = advanced_order_->core_plans[core.core_index];
    if (plan.strategy == PhysicalSortStrategy::kExternal) {
      if (auto opened = Append(OpenSorterInstruction{.sorter = sorter}); !opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      if (auto emitted = EmitOrderedCoreRows(core, full_key, sorter, std::nullopt);
          !emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
    } else {
      if (!top_n_bound_register_.has_value() || !negative_limit_register_.has_value()) {
        return std::unexpected(InternalFailure("ordered compound top-N registers are incomplete"));
      }
      const std::optional<TopNId> top_n = full_key ? ordered_full_top_n_ : ordered_top_n_;
      if (!top_n.has_value()) {
        return std::unexpected(InternalFailure("ordered compound top-N descriptor is unavailable"));
      }
      auto external = CreateLabel();
      auto joined = CreateLabel();
      if (!external.has_value()) {
        return std::unexpected(std::move(external.error()));
      }
      if (!joined.has_value()) {
        return std::unexpected(std::move(joined.error()));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(negative_limit_register_), JumpCondition::kIfTrue, *external);
          !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
      if (ordered_overflow_register_.has_value()) {
        if (auto jumped = EmitJumpIf(AssumeValue(ordered_overflow_register_),
                                     JumpCondition::kIfTrue, *external);
            !jumped.has_value()) {
          return std::unexpected(std::move(jumped.error()));
        }
      }
      if (auto opened = Append(OpenTopNInstruction{
              .top_n = *top_n,
              .bound = AssumeValue(top_n_bound_register_),
          });
          !opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      if (auto emitted = EmitOrderedCoreRows(core, full_key, std::nullopt, *top_n);
          !emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
      if (auto opened = Append(OpenSorterInstruction{.sorter = sorter}); !opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      if (auto transferred =
              EmitTransferOrderedTopN(*top_n, sorter, AssumeValue(ordered_right_first_));
          !transferred.has_value()) {
        return std::unexpected(std::move(transferred.error()));
      }
      if (auto jumped = EmitJump(*joined); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
      if (auto bound = BindLabel(*external); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto opened = Append(OpenSorterInstruction{.sorter = sorter}); !opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      if (auto emitted = EmitOrderedCoreRows(core, full_key, sorter, std::nullopt);
          !emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
      if (auto bound = BindLabel(*joined); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
    }
    return OrderedRun{
        .sorter = sorter,
        .full_key = full_key,
        .unique = false,
    };
  }

  [[nodiscard]] LoweringResult<void> EmitReadOrderedSorterRow(SorterId sorter, RegisterId first) {
    const std::size_t field_count = OrderedRecordFieldCount();
    for (std::size_t field = 0; field < field_count; ++field) {
      if (auto read = Append(ReadSorterFieldInstruction{
              .sorter = sorter,
              .field = static_cast<std::uint32_t>(field),
              .output = RegisterId{first.value() + static_cast<std::uint32_t>(field)},
          });
          !read.has_value()) {
        return read;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitTransferOrderedSorter(SorterId source,
                                                               RegisterId record_first,
                                                               SorterId target) {
    auto empty = CreateLabel();
    auto row = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(source, *empty),
                                        "unable to emit ordered compound rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto read = EmitReadOrderedSorterRow(source, record_first); !read.has_value()) {
      return read;
    }
    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = target,
            .first_value = record_first,
            .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
        });
        !inserted.has_value()) {
      return inserted;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(source, *row),
                                     "unable to emit ordered compound advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    return Append(CloseSorterInstruction{.sorter = source});
  }

  [[nodiscard]] LoweringResult<OrderedRun> EmitOrderedUniqueFullRun(
      OrderedRun source, std::span<const SorterId> retained, bool already_positioned = false) {
    if (!ordered_full_comparison_.has_value() || !ordered_comparison_register_.has_value() ||
        !ordered_predicate_register_.has_value() || !zero_register_.has_value()) {
      return std::unexpected(
          InternalFailure("ordered compound deduplication resources are incomplete"));
    }
    std::array<SorterId, 2> excluded{source.sorter, source.sorter};
    std::size_t excluded_count = 1U;
    if (!retained.empty()) {
      excluded[excluded_count++] = retained.front();
    }
    auto destination =
        ChooseOrderedSorter(true, std::span<const SorterId>{excluded}.first(excluded_count));
    if (!destination.has_value()) {
      return std::unexpected(std::move(destination.error()));
    }
    if (auto opened = Append(OpenSorterInstruction{.sorter = *destination}); !opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }

    std::optional<Label> empty;
    if (!already_positioned) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      empty = *label;
    }
    auto read_next = CreateLabel();
    auto duplicate = CreateLabel();
    auto done = CreateLabel();
    if (!read_next.has_value()) {
      return std::unexpected(std::move(read_next.error()));
    }
    if (!duplicate.has_value()) {
      return std::unexpected(std::move(duplicate.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    if (!already_positioned) {
      auto rewound = ConvertProgramResult(
          AssumeValue(builder_).EmitRewindSorter(source.sorter, AssumeValue(empty)),
          "unable to emit ordered compound deduplication rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }
    if (auto read = EmitReadOrderedSorterRow(source.sorter, AssumeValue(ordered_left_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = *destination,
            .first_value = AssumeValue(ordered_left_first_),
            .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
        });
        !inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    auto next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(source.sorter, *read_next),
                             "unable to emit ordered compound deduplication advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = source.sorter});
        !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*read_next); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(source.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto compared = Append(CompareRecordsInstruction{
            .comparison = AssumeValue(ordered_full_comparison_),
            .left_first = AssumeValue(ordered_left_first_),
            .right_first = AssumeValue(ordered_right_first_),
            .output = AssumeValue(ordered_comparison_register_),
        });
        !compared.has_value()) {
      return std::unexpected(std::move(compared.error()));
    }
    auto binary = EnsureBinarySymbol();
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    if (auto equal = Append(CompareInstruction{
            .comparison = SqlComparison::kEqual,
            .affinity = TypeAffinity::kNumeric,
            .collation = *binary,
            .left = AssumeValue(ordered_comparison_register_),
            .right = AssumeValue(zero_register_),
            .output = AssumeValue(ordered_predicate_register_),
        });
        !equal.has_value()) {
      return std::unexpected(std::move(equal.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(ordered_predicate_register_), JumpCondition::kIfTrue,
                                 *duplicate);
        !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = *destination,
            .first_value = AssumeValue(ordered_right_first_),
            .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
        });
        !inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    for (std::size_t field = 0; field < OrderedRecordFieldCount(); ++field) {
      if (auto copied = Append(CopyInstruction{
              .input = RegisterId{AssumeValue(ordered_right_first_).value() +
                                  static_cast<std::uint32_t>(field)},
              .output = RegisterId{AssumeValue(ordered_left_first_).value() +
                                   static_cast<std::uint32_t>(field)},
          });
          !copied.has_value()) {
        return std::unexpected(std::move(copied.error()));
      }
    }
    if (auto bound = BindLabel(*duplicate); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    auto duplicate_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(source.sorter, *read_next),
                             "unable to emit ordered compound duplicate advance");
    if (!duplicate_next.has_value()) {
      return std::unexpected(std::move(duplicate_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = source.sorter});
        !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (empty.has_value()) {
      if (auto bound = BindLabel(*empty); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto closed = Append(CloseSorterInstruction{.sorter = source.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
    }
    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return OrderedRun{
        .sorter = *destination,
        .full_key = true,
        .unique = true,
    };
  }

  [[nodiscard]] LoweringResult<OrderedRun> EmitOrderedLastUniqueFullRun(
      OrderedRun source, std::span<const SorterId> retained) {
    if (!ordered_full_comparison_.has_value() || !ordered_comparison_register_.has_value() ||
        !ordered_predicate_register_.has_value() || !zero_register_.has_value()) {
      return std::unexpected(
          InternalFailure("ordered UNION deduplication resources are incomplete"));
    }
    std::array<SorterId, 2> excluded{source.sorter, source.sorter};
    std::size_t excluded_count = 1U;
    if (!retained.empty()) {
      excluded[excluded_count++] = retained.front();
    }
    auto destination =
        ChooseOrderedSorter(true, std::span<const SorterId>{excluded}.first(excluded_count));
    if (!destination.has_value()) {
      return std::unexpected(std::move(destination.error()));
    }
    if (auto opened = Append(OpenSorterInstruction{.sorter = *destination}); !opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }

    auto empty = CreateLabel();
    auto read_next = CreateLabel();
    auto replace_pending = CreateLabel();
    auto emit_pending = CreateLabel();
    auto done = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!read_next.has_value()) {
      return std::unexpected(std::move(read_next.error()));
    }
    if (!replace_pending.has_value()) {
      return std::unexpected(std::move(replace_pending.error()));
    }
    if (!emit_pending.has_value()) {
      return std::unexpected(std::move(emit_pending.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(source.sorter, *empty),
                             "unable to emit ordered UNION deduplication rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(source.sorter, AssumeValue(ordered_left_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    auto next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(source.sorter, *read_next),
                             "unable to emit ordered UNION deduplication advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto jumped = EmitJump(*emit_pending); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*read_next); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(source.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto compared = Append(CompareRecordsInstruction{
            .comparison = AssumeValue(ordered_full_comparison_),
            .left_first = AssumeValue(ordered_left_first_),
            .right_first = AssumeValue(ordered_right_first_),
            .output = AssumeValue(ordered_comparison_register_),
        });
        !compared.has_value()) {
      return std::unexpected(std::move(compared.error()));
    }
    auto binary = EnsureBinarySymbol();
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    if (auto equal = Append(CompareInstruction{
            .comparison = SqlComparison::kEqual,
            .affinity = TypeAffinity::kNumeric,
            .collation = *binary,
            .left = AssumeValue(ordered_comparison_register_),
            .right = AssumeValue(zero_register_),
            .output = AssumeValue(ordered_predicate_register_),
        });
        !equal.has_value()) {
      return std::unexpected(std::move(equal.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(ordered_predicate_register_), JumpCondition::kIfTrue,
                                 *replace_pending);
        !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto inserted = EmitInsertOrderedRecord(*destination, AssumeValue(ordered_left_first_));
        !inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    if (auto bound = BindLabel(*replace_pending); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    for (std::size_t field = 0; field < OrderedRecordFieldCount(); ++field) {
      if (auto copied = Append(CopyInstruction{
              .input = RegisterId{AssumeValue(ordered_right_first_).value() +
                                  static_cast<std::uint32_t>(field)},
              .output = RegisterId{AssumeValue(ordered_left_first_).value() +
                                   static_cast<std::uint32_t>(field)},
          });
          !copied.has_value()) {
        return std::unexpected(std::move(copied.error()));
      }
    }
    auto continued =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(source.sorter, *read_next),
                             "unable to emit ordered UNION duplicate advance");
    if (!continued.has_value()) {
      return std::unexpected(std::move(continued.error()));
    }
    if (auto jumped = EmitJump(*emit_pending); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*emit_pending); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto inserted = EmitInsertOrderedRecord(*destination, AssumeValue(ordered_left_first_));
        !inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = source.sorter});
        !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = source.sorter});
        !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return OrderedRun{
        .sorter = *destination,
        .full_key = true,
        .unique = true,
    };
  }

  [[nodiscard]] LoweringResult<void> EmitInsertOrderedRecord(SorterId sorter, RegisterId first) {
    return Append(InsertSorterInstruction{
        .sorter = sorter,
        .first_value = first,
        .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
    });
  }

  [[nodiscard]] LoweringResult<OrderedRun> EmitOrderedSetMerge(
      OrderedRun left, OrderedRun right, CompoundOperator operation,
      bool left_already_positioned = false) {
    if (operation == CompoundOperator::kUnionAll || !ordered_full_comparison_.has_value() ||
        !ordered_comparison_register_.has_value() || !ordered_predicate_register_.has_value() ||
        !zero_register_.has_value()) {
      return std::unexpected(InternalFailure("ordered set merge resources are incomplete"));
    }
    const std::array excluded{left.sorter, right.sorter};
    auto selected = ChooseOrderedSorter(true, excluded);
    if (!selected.has_value()) {
      return std::unexpected(std::move(selected.error()));
    }
    const SorterId output = *selected;
    if (auto opened = Append(OpenSorterInstruction{.sorter = output}); !opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }

    std::optional<Label> left_empty;
    std::optional<Label> right_empty_after_left;
    if (!left_already_positioned) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      left_empty = *label;
      if (operation == CompoundOperator::kUnion) {
        label = CreateLabel();
        if (!label.has_value()) {
          return std::unexpected(std::move(label.error()));
        }
        right_empty_after_left = *label;
      }
    }
    auto right_empty = CreateLabel();
    auto compare = CreateLabel();
    auto take_left = CreateLabel();
    auto take_right = CreateLabel();
    auto equal_left_read = CreateLabel();
    auto equal_right_read = CreateLabel();
    std::optional<Label> equal_right_drain_read;
    if (operation == CompoundOperator::kUnion) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      equal_right_drain_read = *label;
    }
    auto left_read = CreateLabel();
    auto right_read = CreateLabel();
    std::optional<Label> drain_left;
    std::optional<Label> drain_left_read;
    if (operation != CompoundOperator::kIntersect) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      drain_left = *label;
      label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      drain_left_read = *label;
    }
    std::optional<Label> drain_right;
    std::optional<Label> drain_right_read;
    if (operation == CompoundOperator::kUnion) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      drain_right = *label;
      label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      drain_right_read = *label;
    }
    auto done = CreateLabel();
    if (!right_empty.has_value()) {
      return std::unexpected(std::move(right_empty.error()));
    }
    if (!compare.has_value()) {
      return std::unexpected(std::move(compare.error()));
    }
    if (!take_left.has_value()) {
      return std::unexpected(std::move(take_left.error()));
    }
    if (!take_right.has_value()) {
      return std::unexpected(std::move(take_right.error()));
    }
    if (!equal_left_read.has_value()) {
      return std::unexpected(std::move(equal_left_read.error()));
    }
    if (!equal_right_read.has_value()) {
      return std::unexpected(std::move(equal_right_read.error()));
    }
    if (!left_read.has_value()) {
      return std::unexpected(std::move(left_read.error()));
    }
    if (!right_read.has_value()) {
      return std::unexpected(std::move(right_read.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }

    if (!left_already_positioned) {
      auto rewound =
          ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(left.sorter, *left_empty),
                               "unable to emit ordered set left rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }
    if (auto read = EmitReadOrderedSorterRow(left.sorter, AssumeValue(ordered_left_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    auto right_rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(right.sorter, *right_empty),
                             "unable to emit ordered set right rewind");
    if (!right_rewound.has_value()) {
      return std::unexpected(std::move(right_rewound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }

    if (auto bound = BindLabel(*compare); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto compared = Append(CompareRecordsInstruction{
            .comparison = AssumeValue(ordered_full_comparison_),
            .left_first = AssumeValue(ordered_left_first_),
            .right_first = AssumeValue(ordered_right_first_),
            .output = AssumeValue(ordered_comparison_register_),
        });
        !compared.has_value()) {
      return std::unexpected(std::move(compared.error()));
    }
    auto binary = EnsureBinarySymbol();
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    if (auto less = Append(CompareInstruction{
            .comparison = SqlComparison::kLess,
            .affinity = TypeAffinity::kNumeric,
            .collation = *binary,
            .left = AssumeValue(ordered_comparison_register_),
            .right = AssumeValue(zero_register_),
            .output = AssumeValue(ordered_predicate_register_),
        });
        !less.has_value()) {
      return std::unexpected(std::move(less.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(ordered_predicate_register_), JumpCondition::kIfTrue,
                                 *take_left);
        !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto greater = Append(CompareInstruction{
            .comparison = SqlComparison::kGreater,
            .affinity = TypeAffinity::kNumeric,
            .collation = *binary,
            .left = AssumeValue(ordered_comparison_register_),
            .right = AssumeValue(zero_register_),
            .output = AssumeValue(ordered_predicate_register_),
        });
        !greater.has_value()) {
      return std::unexpected(std::move(greater.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(ordered_predicate_register_), JumpCondition::kIfTrue,
                                 *take_right);
        !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (operation == CompoundOperator::kUnion) {
      if (auto inserted = EmitInsertOrderedRecord(output, AssumeValue(ordered_right_first_));
          !inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
    } else if (operation == CompoundOperator::kIntersect) {
      if (auto inserted = EmitInsertOrderedRecord(output, AssumeValue(ordered_left_first_));
          !inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
    }
    auto equal_left_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(left.sorter, *equal_left_read),
                             "unable to emit ordered set equal-left advance");
    if (!equal_left_next.has_value()) {
      return std::unexpected(std::move(equal_left_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (operation == CompoundOperator::kUnion) {
      auto equal_right_next = ConvertProgramResult(
          AssumeValue(builder_).EmitNextSorter(right.sorter, AssumeValue(equal_right_drain_read)),
          "unable to emit ordered UNION equal-right advance");
      if (!equal_right_next.has_value()) {
        return std::unexpected(std::move(equal_right_next.error()));
      }
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (operation == CompoundOperator::kUnion) {
      if (auto bound = BindLabel(AssumeValue(equal_right_drain_read)); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
          !read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      if (auto jumped = EmitJump(AssumeValue(drain_right)); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    }

    if (auto bound = BindLabel(*equal_left_read); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(left.sorter, AssumeValue(ordered_left_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    auto equal_right_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(right.sorter, *equal_right_read),
                             "unable to emit ordered set equal-right advance");
    if (!equal_right_next.has_value()) {
      return std::unexpected(std::move(equal_right_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (operation == CompoundOperator::kUnion || operation == CompoundOperator::kExcept) {
      if (auto jumped = EmitJump(AssumeValue(drain_left)); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    } else {
      if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto jumped = EmitJump(*done); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    }
    if (auto bound = BindLabel(*equal_right_read); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto jumped = EmitJump(*compare); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*take_left); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (operation != CompoundOperator::kIntersect) {
      if (auto inserted = EmitInsertOrderedRecord(output, AssumeValue(ordered_left_first_));
          !inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
    }
    auto left_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(left.sorter, *left_read),
                             "unable to emit ordered set left advance");
    if (!left_next.has_value()) {
      return std::unexpected(std::move(left_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (operation == CompoundOperator::kUnion) {
      if (auto jumped = EmitJump(AssumeValue(drain_right)); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    } else {
      if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto jumped = EmitJump(*done); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    }
    if (auto bound = BindLabel(*left_read); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(left.sorter, AssumeValue(ordered_left_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto jumped = EmitJump(*compare); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*take_right); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (operation == CompoundOperator::kUnion) {
      if (auto inserted = EmitInsertOrderedRecord(output, AssumeValue(ordered_right_first_));
          !inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
    }
    auto right_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(right.sorter, *right_read),
                             "unable to emit ordered set right advance");
    if (!right_next.has_value()) {
      return std::unexpected(std::move(right_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (operation == CompoundOperator::kUnion || operation == CompoundOperator::kExcept) {
      if (auto jumped = EmitJump(AssumeValue(drain_left)); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    } else {
      if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto jumped = EmitJump(*done); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    }
    if (auto bound = BindLabel(*right_read); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto jumped = EmitJump(*compare); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*right_empty); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (operation == CompoundOperator::kUnion || operation == CompoundOperator::kExcept) {
      if (auto jumped = EmitJump(AssumeValue(drain_left)); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    } else {
      if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto jumped = EmitJump(*done); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    }

    if (!left_already_positioned) {
      if (auto bound = BindLabel(*left_empty); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (operation == CompoundOperator::kUnion) {
        auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(
                                                right.sorter, AssumeValue(right_empty_after_left)),
                                            "unable to emit ordered UNION remaining-right rewind");
        if (!rewound.has_value()) {
          return std::unexpected(std::move(rewound.error()));
        }
        if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
            !read.has_value()) {
          return std::unexpected(std::move(read.error()));
        }
        if (auto jumped = EmitJump(AssumeValue(drain_right)); !jumped.has_value()) {
          return std::unexpected(std::move(jumped.error()));
        }
        if (auto bound = BindLabel(AssumeValue(right_empty_after_left)); !bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
      }
      if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto jumped = EmitJump(*done); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    }

    if (drain_left.has_value()) {
      if (auto bound = BindLabel(*drain_left); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto inserted = EmitInsertOrderedRecord(output, AssumeValue(ordered_left_first_));
          !inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
      auto drain_left_next = ConvertProgramResult(
          AssumeValue(builder_).EmitNextSorter(left.sorter, AssumeValue(drain_left_read)),
          "unable to emit ordered set left drain");
      if (!drain_left_next.has_value()) {
        return std::unexpected(std::move(drain_left_next.error()));
      }
      if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto jumped = EmitJump(*done); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
      if (auto bound = BindLabel(AssumeValue(drain_left_read)); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto read = EmitReadOrderedSorterRow(left.sorter, AssumeValue(ordered_left_first_));
          !read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      if (auto jumped = EmitJump(*drain_left); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    }

    if (drain_right.has_value()) {
      if (auto bound = BindLabel(*drain_right); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto inserted = EmitInsertOrderedRecord(output, AssumeValue(ordered_right_first_));
          !inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
      auto drain_right_next = ConvertProgramResult(
          AssumeValue(builder_).EmitNextSorter(right.sorter, AssumeValue(drain_right_read)),
          "unable to emit ordered set right drain");
      if (!drain_right_next.has_value()) {
        return std::unexpected(std::move(drain_right_next.error()));
      }
      if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter});
          !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto jumped = EmitJump(*done); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
      if (auto bound = BindLabel(AssumeValue(drain_right_read)); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
          !read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      if (auto jumped = EmitJump(*drain_right); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
    }

    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return OrderedRun{
        .sorter = output,
        .full_key = true,
        .unique = true,
    };
  }

  [[nodiscard]] LoweringResult<OrderedRun> EmitOrderedBarrierSet(
      OrderedRun left, const StreamingCorePlan& right_core, CompoundOperator operation) {
    if (operation != CompoundOperator::kExcept && operation != CompoundOperator::kIntersect) {
      return std::unexpected(InternalFailure("ordered barrier merge has an invalid operator"));
    }
    const SorterId original = left.sorter;
    auto empty_left = CreateLabel();
    auto done = CreateLabel();
    if (!empty_left.has_value()) {
      return std::unexpected(std::move(empty_left.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(left.sorter, *empty_left),
                             "unable to emit ordered barrier left rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }

    OrderedRun merge_left = left;
    bool left_positioned = true;
    if (!left.full_key || !left.unique) {
      auto unique = EmitOrderedUniqueFullRun(left, {}, true);
      if (!unique.has_value()) {
        return std::unexpected(std::move(unique.error()));
      }
      merge_left = *unique;
      left_positioned = false;
    }
    const std::array retained{merge_left.sorter};
    auto right = EmitOrderedCoreRun(right_core, true, retained);
    if (!right.has_value()) {
      return std::unexpected(std::move(right.error()));
    }
    auto unique_right = EmitOrderedUniqueFullRun(*right, retained);
    if (!unique_right.has_value()) {
      return std::unexpected(std::move(unique_right.error()));
    }
    auto merged = EmitOrderedSetMerge(merge_left, *unique_right, operation, left_positioned);
    if (!merged.has_value()) {
      return std::unexpected(std::move(merged.error()));
    }
    if (merged->sorter != original) {
      if (auto opened = Append(OpenSorterInstruction{.sorter = original}); !opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      if (auto transferred =
              EmitTransferOrderedSorter(merged->sorter, AssumeValue(ordered_left_first_), original);
          !transferred.has_value()) {
        return std::unexpected(std::move(transferred.error()));
      }
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*empty_left); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto reset = Append(ResetSorterInstruction{.sorter = original}); !reset.has_value()) {
      return std::unexpected(std::move(reset.error()));
    }
    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return OrderedRun{
        .sorter = original,
        .full_key = true,
        .unique = true,
    };
  }

  [[nodiscard]] LoweringResult<OrderedRun> EmitOrderedUnionAllGroup(OrderedRun accumulator,
                                                                    OrderedCoreRange cores,
                                                                    bool full_key) {
    const std::array excluded{accumulator.sorter};
    auto aggregate = ChooseOrderedSorter(full_key, excluded);
    if (!aggregate.has_value()) {
      return std::unexpected(std::move(aggregate.error()));
    }
    if (auto opened = Append(OpenSorterInstruction{.sorter = *aggregate}); !opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    if (auto transferred = EmitTransferOrderedSorter(accumulator.sorter,
                                                     AssumeValue(ordered_left_first_), *aggregate);
        !transferred.has_value()) {
      return std::unexpected(std::move(transferred.error()));
    }
    for (std::size_t core = cores.first; core <= cores.last; ++core) {
      const std::array retained{*aggregate};
      auto run =
          EmitOrderedCoreRun(streaming_cores_[core], OrderedCoreNeedsFullKey(core), retained);
      if (!run.has_value()) {
        return std::unexpected(std::move(run.error()));
      }
      if (auto transferred =
              EmitTransferOrderedSorter(run->sorter, AssumeValue(ordered_right_first_), *aggregate);
          !transferred.has_value()) {
        return std::unexpected(std::move(transferred.error()));
      }
    }
    return OrderedRun{
        .sorter = *aggregate,
        .full_key = full_key,
        .unique = false,
    };
  }

  [[nodiscard]] LoweringResult<OrderedRun> EmitOrderedUnionGroup(OrderedRun accumulator,
                                                                 OrderedCoreRange cores) {
    const std::array excluded{accumulator.sorter};
    auto aggregate = ChooseOrderedSorter(true, excluded);
    if (!aggregate.has_value()) {
      return std::unexpected(std::move(aggregate.error()));
    }
    if (auto opened = Append(OpenSorterInstruction{.sorter = *aggregate}); !opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    if (auto transferred = EmitTransferOrderedSorter(accumulator.sorter,
                                                     AssumeValue(ordered_left_first_), *aggregate);
        !transferred.has_value()) {
      return std::unexpected(std::move(transferred.error()));
    }
    for (std::size_t core = cores.first; core <= cores.last; ++core) {
      const std::array retained{*aggregate};
      auto run = EmitOrderedCoreRun(streaming_cores_[core], true, retained);
      if (!run.has_value()) {
        return std::unexpected(std::move(run.error()));
      }
      auto unique = EmitOrderedUniqueFullRun(*run, retained);
      if (!unique.has_value()) {
        return std::unexpected(std::move(unique.error()));
      }
      if (auto transferred = EmitTransferOrderedSorter(
              unique->sorter, AssumeValue(ordered_right_first_), *aggregate);
          !transferred.has_value()) {
        return std::unexpected(std::move(transferred.error()));
      }
    }
    return EmitOrderedLastUniqueFullRun(
        OrderedRun{
            .sorter = *aggregate,
            .full_key = true,
            .unique = false,
        },
        {});
  }

  [[nodiscard]] LoweringResult<OrderedRun> EmitOrderedUnionAll(OrderedRun left, OrderedRun right,
                                                               bool full_key) {
    const std::array excluded{left.sorter, right.sorter};
    auto selected = ChooseOrderedSorter(full_key, excluded);
    if (!selected.has_value()) {
      return std::unexpected(std::move(selected.error()));
    }
    const SorterId output = *selected;
    if (auto opened = Append(OpenSorterInstruction{.sorter = output}); !opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }

    auto left_empty = CreateLabel();
    auto right_empty = CreateLabel();
    auto compare = CreateLabel();
    auto take_right = CreateLabel();
    auto left_read = CreateLabel();
    auto right_read = CreateLabel();
    auto drain_left = CreateLabel();
    auto drain_right = CreateLabel();
    auto drain_left_read = CreateLabel();
    auto drain_right_read = CreateLabel();
    auto right_empty_after_left = CreateLabel();
    auto done = CreateLabel();
    if (!left_empty.has_value()) {
      return std::unexpected(std::move(left_empty.error()));
    }
    if (!right_empty.has_value()) {
      return std::unexpected(std::move(right_empty.error()));
    }
    if (!compare.has_value()) {
      return std::unexpected(std::move(compare.error()));
    }
    if (!take_right.has_value()) {
      return std::unexpected(std::move(take_right.error()));
    }
    if (!left_read.has_value()) {
      return std::unexpected(std::move(left_read.error()));
    }
    if (!right_read.has_value()) {
      return std::unexpected(std::move(right_read.error()));
    }
    if (!drain_left.has_value()) {
      return std::unexpected(std::move(drain_left.error()));
    }
    if (!drain_right.has_value()) {
      return std::unexpected(std::move(drain_right.error()));
    }
    if (!drain_left_read.has_value()) {
      return std::unexpected(std::move(drain_left_read.error()));
    }
    if (!drain_right_read.has_value()) {
      return std::unexpected(std::move(drain_right_read.error()));
    }
    if (!right_empty_after_left.has_value()) {
      return std::unexpected(std::move(right_empty_after_left.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }

    auto left_rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(left.sorter, *left_empty),
                             "unable to emit ordered UNION ALL left rewind");
    if (!left_rewound.has_value()) {
      return std::unexpected(std::move(left_rewound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(left.sorter, AssumeValue(ordered_left_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    auto right_rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(right.sorter, *right_empty),
                             "unable to emit ordered UNION ALL right rewind");
    if (!right_rewound.has_value()) {
      return std::unexpected(std::move(right_rewound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }

    if (auto bound = BindLabel(*compare); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (!ordered_comparison_.has_value() || !ordered_comparison_register_.has_value() ||
        !ordered_predicate_register_.has_value() || !zero_register_.has_value()) {
      return std::unexpected(
          InternalFailure("ordered UNION ALL comparison resources are incomplete"));
    }
    const RegisterId comparison_register = AssumeValue(ordered_comparison_register_);
    const RegisterId predicate_register = AssumeValue(ordered_predicate_register_);
    if (auto compared = Append(CompareRecordsInstruction{
            .comparison = AssumeValue(ordered_comparison_),
            .left_first = AssumeValue(ordered_left_first_),
            .right_first = AssumeValue(ordered_right_first_),
            .output = comparison_register,
        });
        !compared.has_value()) {
      return std::unexpected(std::move(compared.error()));
    }
    auto binary = EnsureBinarySymbol();
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    if (auto greater = Append(CompareInstruction{
            .comparison = SqlComparison::kGreater,
            .affinity = TypeAffinity::kNumeric,
            .collation = *binary,
            .left = comparison_register,
            .right = AssumeValue(zero_register_),
            .output = predicate_register,
        });
        !greater.has_value()) {
      return std::unexpected(std::move(greater.error()));
    }
    if (auto jumped = EmitJumpIf(predicate_register, JumpCondition::kIfTrue, *take_right);
        !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = output,
            .first_value = AssumeValue(ordered_left_first_),
            .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
        });
        !inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    auto left_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(left.sorter, *left_read),
                             "unable to emit ordered UNION ALL left advance");
    if (!left_next.has_value()) {
      return std::unexpected(std::move(left_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*drain_right); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto bound = BindLabel(*left_read); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(left.sorter, AssumeValue(ordered_left_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto jumped = EmitJump(*compare); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*take_right); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = output,
            .first_value = AssumeValue(ordered_right_first_),
            .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
        });
        !inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    auto right_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(right.sorter, *right_read),
                             "unable to emit ordered UNION ALL right advance");
    if (!right_next.has_value()) {
      return std::unexpected(std::move(right_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*drain_left); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto bound = BindLabel(*right_read); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto jumped = EmitJump(*compare); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*right_empty); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*drain_left); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*left_empty); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    auto remaining_right = ConvertProgramResult(
        AssumeValue(builder_).EmitRewindSorter(right.sorter, *right_empty_after_left),
        "unable to emit ordered UNION ALL remaining-right rewind");
    if (!remaining_right.has_value()) {
      return std::unexpected(std::move(remaining_right.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto jumped = EmitJump(*drain_right); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto bound = BindLabel(*right_empty_after_left); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*drain_left); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = output,
            .first_value = AssumeValue(ordered_left_first_),
            .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
        });
        !inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    auto drain_left_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(left.sorter, *drain_left_read),
                             "unable to emit ordered UNION ALL left drain");
    if (!drain_left_next.has_value()) {
      return std::unexpected(std::move(drain_left_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = left.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto bound = BindLabel(*drain_left_read); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(left.sorter, AssumeValue(ordered_left_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto jumped = EmitJump(*drain_left); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*drain_right); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = output,
            .first_value = AssumeValue(ordered_right_first_),
            .value_count = static_cast<std::uint32_t>(OrderedRecordFieldCount()),
        });
        !inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    auto drain_right_next =
        ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(right.sorter, *drain_right_read),
                             "unable to emit ordered UNION ALL right drain");
    if (!drain_right_next.has_value()) {
      return std::unexpected(std::move(drain_right_next.error()));
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = right.sorter}); !closed.has_value()) {
      return std::unexpected(std::move(closed.error()));
    }
    if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto bound = BindLabel(*drain_right_read); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    if (auto read = EmitReadOrderedSorterRow(right.sorter, AssumeValue(ordered_right_first_));
        !read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (auto jumped = EmitJump(*drain_right); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }

    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return OrderedRun{
        .sorter = output,
        .full_key = full_key,
        .unique = false,
    };
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedFinalDrain(OrderedRun run, Label final_completion) {
    if (advanced_order_ == nullptr || advanced_order_->core_plans.empty()) {
      return std::unexpected(InternalFailure("ordered compound drain has no layout"));
    }
    const LogicalCoreOrderLayout& layout = advanced_order_->core_plans.front().layout;
    auto empty = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    auto after = CreateLabel();
    std::optional<Label> limit_completion;
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (!after.has_value()) {
      return std::unexpected(std::move(after.error()));
    }
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      limit_completion = *label;
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(run.sorter, *empty),
                                        "unable to emit final ordered compound rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto offset = EmitOffset(*advance, true); !offset.has_value()) {
      return offset;
    }
    for (std::size_t index = 0; index < layout.output_fields.size(); ++index) {
      const SortOutputField& output = layout.output_fields[index];
      const std::uint32_t field =
          output.kind == SortOutputFieldKind::kKey
              ? output.field_index
              : static_cast<std::uint32_t>(layout.key_values.size()) + output.field_index;
      if (auto read = Append(ReadSorterFieldInstruction{
              .sorter = run.sorter,
              .field = field,
              .output = RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)},
          });
          !read.has_value()) {
        return read;
      }
    }
    if (auto result = Append(ResultRowInstruction{
            .first = result_block_first_,
            .count = static_cast<std::uint32_t>(layout.output_fields.size()),
        });
        !result.has_value()) {
      return result;
    }
    if (limit_completion.has_value()) {
      if (auto limited = EmitDistinctLimitAfterRow(DistinctLimitTargets{
              .advance = *advance,
              .completion = *limit_completion,
          });
          !limited.has_value()) {
        return limited;
      }
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(run.sorter, *row),
                                     "unable to emit final ordered compound advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = run.sorter}); !closed.has_value()) {
      return closed;
    }
    if (auto jumped = EmitJump(*after); !jumped.has_value()) {
      return jumped;
    }
    if (limit_completion.has_value()) {
      if (auto bound = BindLabel(*limit_completion); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseSorterInstruction{.sorter = run.sorter}); !closed.has_value()) {
        return closed;
      }
      if (auto jumped = EmitJump(final_completion); !jumped.has_value()) {
        return jumped;
      }
    }
    return BindLabel(*after);
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedRuntimeLimitInitialization() {
    if (!OrderedUsesRuntimeLimit()) {
      return {};
    }
    if (limit_ == nullptr || !limit_register_.has_value() || !top_n_bound_register_.has_value()) {
      return std::unexpected(InternalFailure("ordered compound runtime LIMIT is incomplete"));
    }
    if (!limit_->offset.has_value()) {
      return Append(CopyInstruction{
          .input = AssumeValue(limit_register_),
          .output = AssumeValue(top_n_bound_register_),
      });
    }
    if (!offset_register_.has_value() || !maximum_integer_register_.has_value() ||
        !bound_room_register_.has_value() || !ordered_overflow_register_.has_value()) {
      return std::unexpected(
          InternalFailure("ordered compound runtime bound registers are incomplete"));
    }
    auto maximum = EnsureMaximumIntegerConstant();
    if (!maximum.has_value()) {
      return std::unexpected(std::move(maximum.error()));
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = *maximum,
            .output = AssumeValue(maximum_integer_register_),
        });
        !loaded.has_value()) {
      return loaded;
    }
    if (auto room = Append(BinaryInstruction{
            .operation = BinaryOperation::kSubtract,
            .left = AssumeValue(maximum_integer_register_),
            .right = AssumeValue(offset_register_),
            .output = AssumeValue(bound_room_register_),
        });
        !room.has_value()) {
      return room;
    }
    auto binary = EnsureBinarySymbol();
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    if (auto overflow = Append(CompareInstruction{
            .comparison = SqlComparison::kLess,
            .affinity = TypeAffinity::kNumeric,
            .collation = *binary,
            .left = AssumeValue(bound_room_register_),
            .right = AssumeValue(limit_register_),
            .output = AssumeValue(ordered_overflow_register_),
        });
        !overflow.has_value()) {
      return overflow;
    }
    return Append(BinaryInstruction{
        .operation = BinaryOperation::kAdd,
        .left = AssumeValue(limit_register_),
        .right = AssumeValue(offset_register_),
        .output = AssumeValue(top_n_bound_register_),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedControlInitialization() {
    if (limit_ != nullptr) {
      return {};
    }
    auto zero = EnsureZeroConstant();
    auto one = EnsureOneConstant();
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    if (!one.has_value()) {
      return std::unexpected(std::move(one.error()));
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = *zero,
            .output = AssumeValue(zero_register_),
        });
        !loaded.has_value()) {
      return loaded;
    }
    return Append(LoadConstantInstruction{
        .constant = *one,
        .output = AssumeValue(one_register_),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitSetThenOrderRows(SetAccumulator state,
                                                          std::optional<SorterId> sorter,
                                                          std::optional<TopNId> top_n) {
    if (advanced_order_ == nullptr || advanced_order_->core_plans.empty() ||
        sorter.has_value() == top_n.has_value()) {
      return std::unexpected(InternalFailure("set-then-order output capability is invalid"));
    }
    const LogicalCoreOrderLayout& layout = advanced_order_->core_plans.front().layout;
    const RelationId relation = SetRelation(state.accumulator);
    auto empty = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindRelation(relation, *empty),
                                        "unable to emit set-then-order relation rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto read = EmitReadSetRow(relation); !read.has_value()) {
      return read;
    }
    if (auto candidate = EmitOrderedCandidate(layout, sorter, top_n, false,
                                              AssumeValue(ordered_right_first_), *advance);
        !candidate.has_value()) {
      return candidate;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextRelation(relation, *row),
                                     "unable to emit set-then-order relation advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseRelationInstruction{.relation = relation}); !closed.has_value()) {
      return closed;
    }

    std::optional<std::size_t> last_set;
    for (std::size_t operation = 0; operation < bound_select_->compound_operators().size();
         ++operation) {
      if (bound_select_->compound_operators()[operation] != CompoundOperator::kUnionAll) {
        last_set = operation;
      }
    }
    if (!last_set.has_value()) {
      return std::unexpected(InternalFailure("set-then-order query has no set operator"));
    }
    for (std::size_t core = *last_set + 2U; core < streaming_cores_.size(); ++core) {
      if (auto emitted = EmitOrderedCoreRows(streaming_cores_[core], false, sorter, top_n);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitSetThenOrderPlan(Label completion) {
    if (advanced_order_ == nullptr || !advanced_order_->set_then_order) {
      return std::unexpected(InternalFailure("set-then-order emission has no fallback plan"));
    }
    if (auto counters = EmitSetCounterInitialization(); !counters.has_value()) {
      return counters;
    }
    std::optional<std::size_t> last_set;
    for (std::size_t operation = 0; operation < bound_select_->compound_operators().size();
         ++operation) {
      if (bound_select_->compound_operators()[operation] != CompoundOperator::kUnionAll) {
        last_set = operation;
      }
    }
    if (!last_set.has_value()) {
      return std::unexpected(InternalFailure("set-then-order query has no set operator"));
    }
    SetAccumulator state;
    if (auto materialized = EmitSetMaterialization(*last_set, &state); !materialized.has_value()) {
      return materialized;
    }

    const SorterId sorter = OrderedSorter(false, 0U);
    if (advanced_order_->final_strategy == PhysicalSortStrategy::kExternal) {
      if (auto opened = Append(OpenSorterInstruction{.sorter = sorter}); !opened.has_value()) {
        return opened;
      }
      if (auto rows = EmitSetThenOrderRows(state, sorter, std::nullopt); !rows.has_value()) {
        return rows;
      }
      return EmitOrderedFinalDrain(OrderedRun{.sorter = sorter, .full_key = false, .unique = false},
                                   completion);
    }

    if (!ordered_top_n_.has_value() || !top_n_bound_register_.has_value() ||
        !negative_limit_register_.has_value()) {
      return std::unexpected(InternalFailure("set-then-order top-N resources are incomplete"));
    }
    auto external = CreateLabel();
    if (!external.has_value()) {
      return std::unexpected(std::move(external.error()));
    }
    if (auto jumped =
            EmitJumpIf(AssumeValue(negative_limit_register_), JumpCondition::kIfTrue, *external);
        !jumped.has_value()) {
      return jumped;
    }
    if (ordered_overflow_register_.has_value()) {
      if (auto jumped = EmitJumpIf(AssumeValue(ordered_overflow_register_), JumpCondition::kIfTrue,
                                   *external);
          !jumped.has_value()) {
        return jumped;
      }
    }
    const TopNId top_n = AssumeValue(ordered_top_n_);
    if (auto opened = Append(OpenTopNInstruction{
            .top_n = top_n,
            .bound = AssumeValue(top_n_bound_register_),
        });
        !opened.has_value()) {
      return opened;
    }
    if (auto rows = EmitSetThenOrderRows(state, std::nullopt, top_n); !rows.has_value()) {
      return rows;
    }
    if (auto opened = Append(OpenSorterInstruction{.sorter = sorter}); !opened.has_value()) {
      return opened;
    }
    if (auto transferred =
            EmitTransferOrderedTopN(top_n, sorter, AssumeValue(ordered_right_first_));
        !transferred.has_value()) {
      return transferred;
    }
    if (auto drained = EmitOrderedFinalDrain(
            OrderedRun{.sorter = sorter, .full_key = false, .unique = false}, completion);
        !drained.has_value()) {
      return drained;
    }
    if (auto jumped = EmitJump(completion); !jumped.has_value()) {
      return jumped;
    }

    if (auto bound = BindLabel(*external); !bound.has_value()) {
      return bound;
    }
    if (auto opened = Append(OpenSorterInstruction{.sorter = sorter}); !opened.has_value()) {
      return opened;
    }
    if (auto rows = EmitSetThenOrderRows(state, sorter, std::nullopt); !rows.has_value()) {
      return rows;
    }
    return EmitOrderedFinalDrain(OrderedRun{.sorter = sorter, .full_key = false, .unique = false},
                                 completion);
  }

  [[nodiscard]] LoweringResult<void> EmitOrderedCompoundPlan() {
    if (!ordered_compound_ || advanced_order_ == nullptr || streaming_cores_.empty()) {
      return std::unexpected(InternalFailure("ordered compound emission has no plan"));
    }
    auto completion = CreateLabel();
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto limit = EmitLimitInitialization(*completion); !limit.has_value()) {
      return limit;
    }
    if (auto bound = EmitOrderedRuntimeLimitInitialization(); !bound.has_value()) {
      return bound;
    }
    if (auto controls = EmitOrderedControlInitialization(); !controls.has_value()) {
      return controls;
    }
    if (advanced_order_->set_then_order) {
      if (auto emitted = EmitSetThenOrderPlan(*completion); !emitted.has_value()) {
        return emitted;
      }
      if (auto bound = BindLabel(*completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    auto first = EmitOrderedCoreRun(streaming_cores_.front(), OrderedCoreNeedsFullKey(0U), {});
    if (!first.has_value()) {
      return std::unexpected(std::move(first.error()));
    }
    OrderedRun accumulator = *first;
    for (std::size_t operation = 0; operation < bound_select_->compound_operators().size();
         ++operation) {
      const CompoundOperator kind = bound_select_->compound_operators()[operation];
      if (kind == CompoundOperator::kUnionAll) {
        std::size_t run_end = operation;
        while (run_end + 1U < bound_select_->compound_operators().size() &&
               bound_select_->compound_operators()[run_end + 1U] == CompoundOperator::kUnionAll) {
          ++run_end;
        }
        if (run_end > operation) {
          auto grouped = EmitOrderedUnionAllGroup(
              accumulator, OrderedCoreRange{.first = operation + 1U, .last = run_end + 1U},
              OrderedCoreNeedsFullKey(run_end + 1U));
          if (!grouped.has_value()) {
            return std::unexpected(std::move(grouped.error()));
          }
          accumulator = *grouped;
          operation = run_end;
          continue;
        }
        const std::array excluded{accumulator.sorter};
        auto right = EmitOrderedCoreRun(streaming_cores_[operation + 1U],
                                        OrderedCoreNeedsFullKey(operation + 1U), excluded);
        if (!right.has_value()) {
          return std::unexpected(std::move(right.error()));
        }
        const bool full_key = OrderedCoreNeedsFullKey(operation + 1U);
        auto combined = EmitOrderedUnionAll(accumulator, *right, full_key);
        if (!combined.has_value()) {
          return std::unexpected(std::move(combined.error()));
        }
        accumulator = *combined;
        continue;
      }

      if (kind == CompoundOperator::kExcept || kind == CompoundOperator::kIntersect) {
        auto barrier = EmitOrderedBarrierSet(accumulator, streaming_cores_[operation + 1U], kind);
        if (!barrier.has_value()) {
          return std::unexpected(std::move(barrier.error()));
        }
        accumulator = *barrier;
        continue;
      }

      if (!accumulator.full_key || !accumulator.unique) {
        auto unique = EmitOrderedUniqueFullRun(accumulator, {});
        if (!unique.has_value()) {
          return std::unexpected(std::move(unique.error()));
        }
        accumulator = *unique;
      }
      std::size_t run_end = operation;
      while (run_end + 1U < bound_select_->compound_operators().size() &&
             bound_select_->compound_operators()[run_end + 1U] == CompoundOperator::kUnion) {
        ++run_end;
      }
      if (run_end > operation) {
        auto grouped = EmitOrderedUnionGroup(
            accumulator, OrderedCoreRange{.first = operation + 1U, .last = run_end + 1U});
        if (!grouped.has_value()) {
          return std::unexpected(std::move(grouped.error()));
        }
        accumulator = *grouped;
        operation = run_end;
        continue;
      }
      const std::array retained{accumulator.sorter};
      auto right = EmitOrderedCoreRun(streaming_cores_[operation + 1U], true, retained);
      if (!right.has_value()) {
        return std::unexpected(std::move(right.error()));
      }
      auto unique_right = EmitOrderedUniqueFullRun(*right, retained);
      if (!unique_right.has_value()) {
        return std::unexpected(std::move(unique_right.error()));
      }
      auto united = EmitOrderedSetMerge(accumulator, *unique_right, CompoundOperator::kUnion);
      if (!united.has_value()) {
        return std::unexpected(std::move(united.error()));
      }
      accumulator = *united;
    }
    if (auto drained = EmitOrderedFinalDrain(accumulator, *completion); !drained.has_value()) {
      return drained;
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] RelationId SetRelation(std::size_t index) const noexcept {
    return AssumeValue(set_relations_[index]);
  }

  [[nodiscard]] RegisterId SetCount(std::size_t index) const noexcept {
    return AssumeValue(set_count_registers_[index]);
  }

  [[nodiscard]] LoweringResult<void> EmitSetCounterInitialization() {
    if (!zero_register_.has_value() || !one_register_.has_value()) {
      return std::unexpected(InternalFailure("set counters have no constants"));
    }
    auto zero = EnsureZeroConstant();
    auto one = EnsureOneConstant();
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    if (!one.has_value()) {
      return std::unexpected(std::move(one.error()));
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = *zero,
            .output = AssumeValue(zero_register_),
        });
        !loaded.has_value()) {
      return loaded;
    }
    return Append(LoadConstantInstruction{
        .constant = *one,
        .output = AssumeValue(one_register_),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitSetIncrement(RegisterId count) {
    return Append(BinaryInstruction{
        .operation = BinaryOperation::kAdd,
        .left = count,
        .right = AssumeValue(one_register_),
        .output = count,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitSetDecrement(RegisterId count) {
    return Append(BinaryInstruction{
        .operation = BinaryOperation::kSubtract,
        .left = count,
        .right = AssumeValue(one_register_),
        .output = count,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitOpenCountedRelation(RelationId relation,
                                                             RegisterId count) {
    if (auto opened = Append(OpenRelationInstruction{.relation = relation}); !opened.has_value()) {
      return opened;
    }
    return Append(CopyInstruction{
        .input = AssumeValue(zero_register_),
        .output = count,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitSetInsert(RelationId relation, RelationInsertMode mode,
                                                   std::optional<RegisterId> count) {
    auto existing = CreateLabel();
    if (!existing.has_value()) {
      return std::unexpected(std::move(existing.error()));
    }
    auto inserted = ConvertProgramResult(
        AssumeValue(builder_).EmitInsertRelation(
            relation, result_block_first_,
            static_cast<std::uint32_t>(bound_select_->result_columns().size()), mode, *existing),
        "unable to emit set relation insertion");
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    if (count.has_value()) {
      if (auto incremented = EmitSetIncrement(*count); !incremented.has_value()) {
        return incremented;
      }
    }
    return BindLabel(*existing);
  }

  [[nodiscard]] LoweringResult<void> EmitMaterializedSetInsert(
      RelationId target, std::optional<RegisterId> count,
      std::optional<RelationId> distinct_membership) {
    if (!distinct_membership.has_value()) {
      return EmitSetInsert(target, RelationInsertMode::kKeepExisting, count);
    }
    auto duplicate = CreateLabel();
    if (!duplicate.has_value()) {
      return std::unexpected(std::move(duplicate.error()));
    }
    auto inserted =
        ConvertProgramResult(AssumeValue(builder_).EmitInsertRelation(
                                 *distinct_membership, result_block_first_,
                                 static_cast<std::uint32_t>(bound_select_->result_columns().size()),
                                 RelationInsertMode::kKeepExisting, *duplicate),
                             "unable to emit set-arm DISTINCT membership");
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    if (auto target_inserted = EmitSetInsert(target, RelationInsertMode::kKeepExisting, count);
        !target_inserted.has_value()) {
      return target_inserted;
    }
    return BindLabel(*duplicate);
  }

  [[nodiscard]] LoweringResult<void> EmitMaterializeSelectRows(
      const StreamingCorePlan& core, const BoundSelectCore& select, RelationId target,
      std::optional<RegisterId> count, bool emit_guards,
      std::optional<RelationId> distinct_membership = std::nullopt) {
    SetStreamingCoreContext(core);
    auto after = CreateLabel();
    if (!after.has_value()) {
      return std::unexpected(std::move(after.error()));
    }
    if (emit_guards) {
      if (auto guards = EmitStreamingGuards(core, *after); !guards.has_value()) {
        return guards;
      }
    }
    if (core.table_scan != nullptr) {
      if (!core.cursor.has_value()) {
        return std::unexpected(InternalFailure("set table core has no cursor"));
      }
      const CursorId cursor = *core.cursor;
      auto done = CreateLabel();
      auto row = CreateLabel();
      auto advance = CreateLabel();
      if (!done.has_value()) {
        return std::unexpected(std::move(done.error()));
      }
      if (!row.has_value()) {
        return std::unexpected(std::move(row.error()));
      }
      if (!advance.has_value()) {
        return std::unexpected(std::move(advance.error()));
      }
      if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
        return opened;
      }
      auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *done),
                                          "unable to emit set table rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
      if (auto bound = BindLabel(*row); !bound.has_value()) {
        return bound;
      }
      if (auto filters = EmitStreamingFilters(core, *advance); !filters.has_value()) {
        return filters;
      }
      if (auto projected = EmitStreamingProjection(select, SelectOutputMode::Materialized);
          !projected.has_value()) {
        return projected;
      }
      if (auto inserted = EmitMaterializedSetInsert(target, count, distinct_membership);
          !inserted.has_value()) {
        return inserted;
      }
      if (auto bound = BindLabel(*advance); !bound.has_value()) {
        return bound;
      }
      auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *row),
                                       "unable to emit set table advance");
      if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
      }
      if (auto bound = BindLabel(*done); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
        return closed;
      }
    } else {
      if (auto filters = EmitStreamingFilters(core, *after); !filters.has_value()) {
        return filters;
      }
      if (auto projected = EmitStreamingProjection(select, SelectOutputMode::Materialized);
          !projected.has_value()) {
        return projected;
      }
      if (auto inserted = EmitMaterializedSetInsert(target, count, distinct_membership);
          !inserted.has_value()) {
        return inserted;
      }
    }
    return BindLabel(*after);
  }

  [[nodiscard]] LoweringResult<void> EmitMaterializeValuesRows(const BoundValuesCore& values,
                                                               RelationId target,
                                                               std::optional<RegisterId> count) {
    for (const std::vector<BoundExpressionId>& row : values.rows) {
      if (row.size() != bound_select_->result_columns().size()) {
        return std::unexpected(InternalFailure("set VALUES row width is invalid"));
      }
      for (std::size_t index = 0; index < row.size(); ++index) {
        if (auto emitted = EmitExpression(
                row[index],
                RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return emitted;
        }
      }
      if (auto inserted = EmitSetInsert(target, RelationInsertMode::kKeepExisting, count);
          !inserted.has_value()) {
        return inserted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitTransferRelation(RelationId source,
                                                          RelationInsertMode mode,
                                                          RelationId target,
                                                          std::optional<RegisterId> target_count) {
    auto empty = CreateLabel();
    auto row = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindRelation(source, *empty),
                                        "unable to emit set relation rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    for (std::size_t index = 0; index < bound_select_->result_columns().size(); ++index) {
      if (auto read = Append(ReadRelationFieldInstruction{
              .relation = source,
              .field = static_cast<std::uint32_t>(index),
              .output = RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)},
          });
          !read.has_value()) {
        return read;
      }
    }
    if (auto inserted = EmitSetInsert(target, mode, target_count); !inserted.has_value()) {
      return inserted;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextRelation(source, *row),
                                     "unable to emit set relation advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    return Append(CloseRelationInstruction{.relation = source});
  }

  [[nodiscard]] LoweringResult<void> EmitMaterializeCore(const StreamingCorePlan& core,
                                                         RelationId target, RegisterId count) {
    if (const auto* values = std::get_if<BoundValuesCore>(core.bound); values != nullptr) {
      return EmitMaterializeValuesRows(*values, target, count);
    }
    const auto& select = std::get<BoundSelectCore>(*core.bound);
    if (core.join != nullptr && core.aggregate == nullptr) {
      return EmitJoinedCore(core, select,
                            SelectOutputContext{
                                .mode = SelectOutputMode::Materialized,
                                .core = &core,
                                .materialized_target = target,
                                .materialized_count = count,
                            });
    }
    if (core.aggregate != nullptr) {
      return EmitAggregateCore(core, SelectOutputContext{
                                         .mode = SelectOutputMode::Materialized,
                                         .core = &core,
                                         .materialized_target = target,
                                         .materialized_count = count,
                                     });
    }
    if (core.relation == std::nullopt) {
      return EmitMaterializeSelectRows(core, select, target, count, true);
    }

    SetStreamingCoreContext(core);
    auto skipped = CreateLabel();
    if (!skipped.has_value()) {
      return std::unexpected(std::move(skipped.error()));
    }
    if (auto guards = EmitStreamingGuards(core, *skipped); !guards.has_value()) {
      return guards;
    }
    const RelationId local = *core.relation;
    if (auto opened = Append(OpenRelationInstruction{.relation = local}); !opened.has_value()) {
      return opened;
    }
    if (auto rows = EmitMaterializeSelectRows(core, select, target, count, false, local);
        !rows.has_value()) {
      return rows;
    }
    if (auto closed = Append(CloseRelationInstruction{.relation = local}); !closed.has_value()) {
      return closed;
    }
    return BindLabel(*skipped);
  }

  [[nodiscard]] LoweringResult<void> EmitReadSetRow(RelationId relation) {
    for (std::size_t index = 0; index < bound_select_->result_columns().size(); ++index) {
      if (auto read = Append(ReadRelationFieldInstruction{
              .relation = relation,
              .field = static_cast<std::uint32_t>(index),
              .output = RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)},
          });
          !read.has_value()) {
        return read;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitUnionStep(SetAccumulator state,
                                                   const StreamingCorePlan& right_core) {
    const RelationId accumulator = SetRelation(state.accumulator);
    const RelationId right = SetRelation(state.right);
    const RegisterId accumulator_count = SetCount(state.accumulator);
    const RegisterId right_count = SetCount(state.right);
    if (auto opened = EmitOpenCountedRelation(right, right_count); !opened.has_value()) {
      return opened;
    }
    if (auto materialized = EmitMaterializeCore(right_core, right, right_count);
        !materialized.has_value()) {
      return materialized;
    }
    return EmitTransferRelation(right, RelationInsertMode::kReplaceExisting, accumulator,
                                accumulator_count);
  }

  [[nodiscard]] LoweringResult<void> EmitExceptStep(SetAccumulator state,
                                                    const StreamingCorePlan& right_core) {
    const RelationId accumulator = SetRelation(state.accumulator);
    const RelationId right = SetRelation(state.right);
    const RegisterId accumulator_count = SetCount(state.accumulator);
    const RegisterId right_count = SetCount(state.right);
    auto after = CreateLabel();
    auto empty_right = CreateLabel();
    auto row = CreateLabel();
    auto found = CreateLabel();
    auto advance = CreateLabel();
    if (!after.has_value()) {
      return std::unexpected(std::move(after.error()));
    }
    if (!empty_right.has_value()) {
      return std::unexpected(std::move(empty_right.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (auto skipped = EmitJumpIf(accumulator_count, JumpCondition::kIfFalse, *after);
        !skipped.has_value()) {
      return skipped;
    }
    if (auto opened = EmitOpenCountedRelation(right, right_count); !opened.has_value()) {
      return opened;
    }
    if (auto materialized = EmitMaterializeCore(right_core, right, right_count);
        !materialized.has_value()) {
      return materialized;
    }
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewindRelation(right, *empty_right),
                             "unable to emit EXCEPT right rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto read = EmitReadSetRow(right); !read.has_value()) {
      return read;
    }
    auto contained = ConvertProgramResult(
        AssumeValue(builder_).EmitContainsRelation(
            accumulator, result_block_first_,
            static_cast<std::uint32_t>(bound_select_->result_columns().size()), *found),
        "unable to emit EXCEPT membership");
    if (!contained.has_value()) {
      return std::unexpected(std::move(contained.error()));
    }
    if (auto jumped = EmitJump(*advance); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*found); !bound.has_value()) {
      return bound;
    }
    if (auto erased = Append(DeleteRelationInstruction{
            .relation = accumulator,
            .first_key = result_block_first_,
            .key_count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
        });
        !erased.has_value()) {
      return erased;
    }
    if (auto decremented = EmitSetDecrement(accumulator_count); !decremented.has_value()) {
      return decremented;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextRelation(right, *row),
                                     "unable to emit EXCEPT right advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty_right); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseRelationInstruction{.relation = right}); !closed.has_value()) {
      return closed;
    }
    return BindLabel(*after);
  }

  [[nodiscard]] LoweringResult<void> EmitIntersectStep(SetAccumulator* state,
                                                       const StreamingCorePlan& right_core) {
    const RelationId accumulator = SetRelation(state->accumulator);
    const RelationId right = SetRelation(state->right);
    const RelationId output = SetRelation(state->output);
    const RegisterId accumulator_count = SetCount(state->accumulator);
    const RegisterId right_count = SetCount(state->right);
    const RegisterId output_count = SetCount(state->output);
    auto empty_scan = CreateLabel();
    auto row = CreateLabel();
    auto found = CreateLabel();
    auto advance = CreateLabel();
    auto done = CreateLabel();
    if (!empty_scan.has_value()) {
      return std::unexpected(std::move(empty_scan.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    if (auto skipped = EmitJumpIf(accumulator_count, JumpCondition::kIfFalse, *done);
        !skipped.has_value()) {
      return skipped;
    }
    if (auto opened = EmitOpenCountedRelation(right, right_count); !opened.has_value()) {
      return opened;
    }
    if (auto materialized = EmitMaterializeCore(right_core, right, right_count);
        !materialized.has_value()) {
      return materialized;
    }
    if (auto opened = EmitOpenCountedRelation(output, output_count); !opened.has_value()) {
      return opened;
    }
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewindRelation(accumulator, *empty_scan),
                             "unable to emit INTERSECT left rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto read = EmitReadSetRow(accumulator); !read.has_value()) {
      return read;
    }
    auto contained = ConvertProgramResult(
        AssumeValue(builder_).EmitContainsRelation(
            right, result_block_first_,
            static_cast<std::uint32_t>(bound_select_->result_columns().size()), *found),
        "unable to emit INTERSECT membership");
    if (!contained.has_value()) {
      return std::unexpected(std::move(contained.error()));
    }
    if (auto jumped = EmitJump(*advance); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*found); !bound.has_value()) {
      return bound;
    }
    if (auto inserted = EmitSetInsert(output, RelationInsertMode::kKeepExisting, output_count);
        !inserted.has_value()) {
      return inserted;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextRelation(accumulator, *row),
                                     "unable to emit INTERSECT left advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty_scan); !bound.has_value()) {
      return bound;
    }
    if (auto reset = Append(ResetRelationInstruction{.relation = accumulator});
        !reset.has_value()) {
      return reset;
    }
    if (auto cleared_count = Append(CopyInstruction{
            .input = AssumeValue(zero_register_),
            .output = accumulator_count,
        });
        !cleared_count.has_value()) {
      return cleared_count;
    }
    if (auto closed = Append(CloseRelationInstruction{.relation = right}); !closed.has_value()) {
      return closed;
    }
    if (auto transferred = EmitTransferRelation(output, RelationInsertMode::kKeepExisting,
                                                accumulator, accumulator_count);
        !transferred.has_value()) {
      return transferred;
    }
    return BindLabel(*done);
  }

  [[nodiscard]] LoweringResult<void> EmitSetMaterialization(std::size_t end_operation,
                                                            SetAccumulator* state) {
    const RelationId accumulator = SetRelation(state->accumulator);
    const RegisterId accumulator_count = SetCount(state->accumulator);
    if (auto opened = EmitOpenCountedRelation(accumulator, accumulator_count);
        !opened.has_value()) {
      return opened;
    }
    if (auto first = EmitMaterializeCore(streaming_cores_.front(), accumulator, accumulator_count);
        !first.has_value()) {
      return first;
    }
    for (std::size_t operation_index = 0; operation_index <= end_operation; ++operation_index) {
      const CompoundOperator operation = bound_select_->compound_operators()[operation_index];
      const StreamingCorePlan& right_core = streaming_cores_[operation_index + 1U];
      switch (operation) {
        case CompoundOperator::kUnionAll:
          if (auto materialized = EmitMaterializeCore(right_core, SetRelation(state->accumulator),
                                                      SetCount(state->accumulator));
              !materialized.has_value()) {
            return materialized;
          }
          break;
        case CompoundOperator::kUnion:
          if (auto united = EmitUnionStep(*state, right_core); !united.has_value()) {
            return united;
          }
          break;
        case CompoundOperator::kExcept:
          if (auto excepted = EmitExceptStep(*state, right_core); !excepted.has_value()) {
            return excepted;
          }
          break;
        case CompoundOperator::kIntersect:
          if (auto intersected = EmitIntersectStep(state, right_core); !intersected.has_value()) {
            return intersected;
          }
          break;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDrainSetRelation(RelationId relation,
                                                          Label final_completion) {
    auto empty = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    auto after = CreateLabel();
    std::optional<Label> limit_completion;
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (!after.has_value()) {
      return std::unexpected(std::move(after.error()));
    }
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      limit_completion = *label;
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindRelation(relation, *empty),
                                        "unable to emit final set rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto read = EmitReadSetRow(relation); !read.has_value()) {
      return read;
    }
    if (auto offset = EmitOffset(*advance); !offset.has_value()) {
      return offset;
    }
    if (auto result = Append(ResultRowInstruction{
            .first = result_block_first_,
            .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
        });
        !result.has_value()) {
      return result;
    }
    if (limit_completion.has_value()) {
      if (auto limited = EmitDistinctLimitAfterRow(DistinctLimitTargets{
              .advance = *advance,
              .completion = *limit_completion,
          });
          !limited.has_value()) {
        return limited;
      }
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextRelation(relation, *row),
                                     "unable to emit final set advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseRelationInstruction{.relation = relation}); !closed.has_value()) {
      return closed;
    }
    if (auto jumped = EmitJump(*after); !jumped.has_value()) {
      return jumped;
    }
    if (limit_completion.has_value()) {
      if (auto bound = BindLabel(*limit_completion); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseRelationInstruction{.relation = relation});
          !closed.has_value()) {
        return closed;
      }
      if (auto jumped = EmitJump(final_completion); !jumped.has_value()) {
        return jumped;
      }
    }
    return BindLabel(*after);
  }

  [[nodiscard]] LoweringResult<void> EmitDemandOneRelation(RelationId relation,
                                                           Label final_completion) {
    auto empty = CreateLabel();
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindRelation(relation, *empty),
                                        "unable to emit LIMIT-one set rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto read = EmitReadSetRow(relation); !read.has_value()) {
      return read;
    }
    if (auto result = Append(ResultRowInstruction{
            .first = result_block_first_,
            .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
        });
        !result.has_value()) {
      return result;
    }
    if (auto closed = Append(CloseRelationInstruction{.relation = relation}); !closed.has_value()) {
      return closed;
    }
    if (auto jumped = EmitJump(final_completion); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    return Append(CloseRelationInstruction{.relation = relation});
  }

  [[nodiscard]] LoweringResult<bool> EmitDemandOneCore(const StreamingCorePlan& core,
                                                       Label final_completion) {
    if (const auto* values = std::get_if<BoundValuesCore>(core.bound); values != nullptr) {
      if (values->rows.empty()) {
        return false;
      }
      const std::vector<BoundExpressionId>& row = values->rows.front();
      for (std::size_t index = 0; index < row.size(); ++index) {
        if (auto emitted = EmitProjectionValue(
                row[index],
                RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return std::unexpected(std::move(emitted.error()));
        }
      }
      if (auto result = Append(ResultRowInstruction{
              .first = result_block_first_,
              .count = static_cast<std::uint32_t>(row.size()),
          });
          !result.has_value()) {
        return std::unexpected(std::move(result.error()));
      }
      if (auto jumped = EmitJump(final_completion); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
      return true;
    }

    const auto& select = std::get<BoundSelectCore>(*core.bound);
    if (core.join != nullptr && core.aggregate == nullptr) {
      auto emitted = EmitJoinedCore(core, select,
                                    SelectOutputContext{
                                        .mode = SelectOutputMode::DemandOne,
                                        .core = &core,
                                        .final_completion = final_completion,
                                    });
      if (!emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
      return false;
    }
    if (core.aggregate != nullptr) {
      auto emitted = EmitAggregateCore(core, SelectOutputContext{
                                                 .mode = SelectOutputMode::DemandOne,
                                                 .core = &core,
                                                 .final_completion = final_completion,
                                             });
      if (!emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
      return AggregateCoreAlwaysProducesRow(core);
    }
    SetStreamingCoreContext(core);
    auto next_core = CreateLabel();
    if (!next_core.has_value()) {
      return std::unexpected(std::move(next_core.error()));
    }
    if (auto guards = EmitStreamingGuards(core, *next_core); !guards.has_value()) {
      return std::unexpected(std::move(guards.error()));
    }
    if (core.table_scan != nullptr) {
      if (!core.cursor.has_value()) {
        return std::unexpected(InternalFailure("LIMIT-one table core has no cursor"));
      }
      const CursorId cursor = *core.cursor;
      auto done = CreateLabel();
      auto row = CreateLabel();
      std::optional<Label> advance;
      if (core.filter != nullptr && !core.filter->predicates.empty()) {
        auto created = CreateLabel();
        if (!created.has_value()) {
          return std::unexpected(std::move(created.error()));
        }
        advance = *created;
      }
      if (!done.has_value()) {
        return std::unexpected(std::move(done.error()));
      }
      if (!row.has_value()) {
        return std::unexpected(std::move(row.error()));
      }
      if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *done),
                                          "unable to emit LIMIT-one table rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
      if (auto bound = BindLabel(*row); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto filters = EmitStreamingFilters(core, advance.value_or(*done));
          !filters.has_value()) {
        return std::unexpected(std::move(filters.error()));
      }
      if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
        return std::unexpected(std::move(projected.error()));
      }
      if (auto result = Append(ResultRowInstruction{
              .first = result_block_first_,
              .count = static_cast<std::uint32_t>(select.result_columns.size()),
          });
          !result.has_value()) {
        return std::unexpected(std::move(result.error()));
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto jumped = EmitJump(final_completion); !jumped.has_value()) {
        return std::unexpected(std::move(jumped.error()));
      }
      if (advance.has_value()) {
        if (auto bound = BindLabel(*advance); !bound.has_value()) {
          return std::unexpected(std::move(bound.error()));
        }
        auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *row),
                                         "unable to emit LIMIT-one table advance");
        if (!next.has_value()) {
          return std::unexpected(std::move(next.error()));
        }
      }
      if (auto bound = BindLabel(*done); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (auto bound = BindLabel(*next_core); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
      return false;
    }

    if (auto filters = EmitStreamingFilters(core, *next_core); !filters.has_value()) {
      return std::unexpected(std::move(filters.error()));
    }
    if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
      return std::unexpected(std::move(projected.error()));
    }
    if (auto result = Append(ResultRowInstruction{
            .first = result_block_first_,
            .count = static_cast<std::uint32_t>(select.result_columns.size()),
        });
        !result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    if (auto jumped = EmitJump(final_completion); !jumped.has_value()) {
      return std::unexpected(std::move(jumped.error()));
    }
    if (auto bound = BindLabel(*next_core); !bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    return core.filter == nullptr ||
           (core.filter->guards.empty() && core.filter->predicates.empty());
  }

  [[nodiscard]] LoweringResult<void> EmitSetLimitOnePlan(Label final_completion) {
    std::optional<std::size_t> last_barrier;
    for (std::size_t index = 0; index < bound_select_->compound_operators().size(); ++index) {
      const CompoundOperator operation = bound_select_->compound_operators()[index];
      if (operation == CompoundOperator::kExcept || operation == CompoundOperator::kIntersect) {
        last_barrier = index;
      }
    }
    std::size_t first_core = 0;
    if (last_barrier.has_value()) {
      SetAccumulator state;
      if (auto materialized = EmitSetMaterialization(*last_barrier, &state);
          !materialized.has_value()) {
        return materialized;
      }
      if (auto demanded = EmitDemandOneRelation(SetRelation(state.accumulator), final_completion);
          !demanded.has_value()) {
        return demanded;
      }
      first_core = *last_barrier + 2U;
    }
    for (std::size_t core_index = first_core; core_index < streaming_cores_.size(); ++core_index) {
      auto demanded = EmitDemandOneCore(streaming_cores_[core_index], final_completion);
      if (!demanded.has_value()) {
        return std::unexpected(std::move(demanded.error()));
      }
      if (*demanded) {
        break;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitSetPlan() {
    if (!set_query_ || bound_select_->compound_operators().empty()) {
      return std::unexpected(InternalFailure("set query emission has no operators"));
    }
    auto completion = CreateLabel();
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto limit = EmitLimitInitialization(*completion); !limit.has_value()) {
      return limit;
    }
    if (auto counters = EmitSetCounterInitialization(); !counters.has_value()) {
      return counters;
    }
    if (set_limit_one_) {
      if (auto limited = EmitSetLimitOnePlan(*completion); !limited.has_value()) {
        return limited;
      }
    } else {
      std::optional<std::size_t> last_set;
      for (std::size_t index = 0; index < bound_select_->compound_operators().size(); ++index) {
        if (bound_select_->compound_operators()[index] != CompoundOperator::kUnionAll) {
          last_set = index;
        }
      }
      if (!last_set.has_value()) {
        return std::unexpected(InternalFailure("set query has no set operator"));
      }
      SetAccumulator state;
      if (auto materialized = EmitSetMaterialization(*last_set, &state);
          !materialized.has_value()) {
        return materialized;
      }
      if (auto drained = EmitDrainSetRelation(SetRelation(state.accumulator), *completion);
          !drained.has_value()) {
        return drained;
      }
      for (std::size_t core_index = *last_set + 2U; core_index < streaming_cores_.size();
           ++core_index) {
        const StreamingCorePlan& core = streaming_cores_[core_index];
        if (core.aggregate != nullptr) {
          if (auto emitted = EmitAggregateCore(core,
                                               SelectOutputContext{
                                                   .mode = SelectOutputMode::Streaming,
                                                   .core = &core,
                                                   .final_completion = *completion,
                                               });
              !emitted.has_value()) {
            return emitted;
          }
          continue;
        }
        if (const auto* values = std::get_if<BoundValuesCore>(core.bound); values != nullptr) {
          if (auto emitted = EmitStreamingValuesCore(*values, *completion); !emitted.has_value()) {
            return emitted;
          }
        } else {
          const auto& select = std::get<BoundSelectCore>(*core.bound);
          if (core.table_scan != nullptr) {
            if (auto emitted = EmitStreamingTableCore(core, select, *completion);
                !emitted.has_value()) {
              return emitted;
            }
          } else {
            if (auto emitted = EmitStreamingSingleCore(core, select, *completion);
                !emitted.has_value()) {
              return emitted;
            }
          }
        }
      }
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctProjection() {
    if (projection_ == nullptr) {
      return std::unexpected(InternalFailure("DISTINCT projection is unavailable"));
    }
    const LogicalNode& logical_projection = logical_plan_->node(projection_->logical_projection);
    const auto* projection = std::get_if<LogicalProjectionNode>(&logical_projection.payload);
    if (projection == nullptr || projection->core_index != 0U ||
        projection->expressions.size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("DISTINCT projection shape is invalid"));
    }
    for (std::size_t index = 0; index < projection->expressions.size(); ++index) {
      if (auto emitted = EmitExpression(
              projection->expressions[index],
              RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)});
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctMembership(Label duplicate) {
    if (!distinct_relation_.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT relation descriptor is unavailable"));
    }
    auto inserted =
        ConvertProgramResult(AssumeValue(builder_).EmitInsertRelation(
                                 *distinct_relation_, result_block_first_,
                                 static_cast<std::uint32_t>(bound_select_->result_columns().size()),
                                 RelationInsertMode::kKeepExisting, duplicate),
                             "unable to emit DISTINCT membership insertion");
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctOrderValue(const LogicalOrderValue& value,
                                                            RegisterId destination) {
    if (value.kind == LogicalOrderValueKind::kInputField) {
      if (value.expression.has_value() ||
          value.field_index >= bound_select_->result_columns().size()) {
        return std::unexpected(InternalFailure("DISTINCT order input field is invalid"));
      }
      return Append(CopyInstruction{
          .input = RegisterId{result_block_first_.value() + value.field_index},
          .output = destination,
      });
    }
    if (!value.expression.has_value() || value.field_index != 0U) {
      return std::unexpected(InternalFailure("DISTINCT order expression is invalid"));
    }
    const BoundExpressionId expression = *value.expression;
    if (auto emitted = EmitExpression(expression, Home(expression)); !emitted.has_value()) {
      return emitted;
    }
    return Append(CopyInstruction{
        .input = Home(expression),
        .output = destination,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctOrderedCandidate() {
    if (advanced_order_ == nullptr || !sorter_.has_value() || !sorter_record_first_.has_value() ||
        advanced_order_->core_plans.size() != 1U) {
      return std::unexpected(InternalFailure("DISTINCT ordered candidate is incomplete"));
    }
    const LogicalCoreOrderLayout& layout = advanced_order_->core_plans[0].layout;
    if (layout.schedule != OrderEvaluationSchedule::kPayloadThenKeys ||
        layout.key_values.size() != advanced_order_->terms.size()) {
      return std::unexpected(InternalFailure("DISTINCT order schedule is invalid"));
    }
    const RegisterId first = AssumeValue(sorter_record_first_);
    const SorterId sorter = AssumeValue(sorter_);
    const auto key_count = static_cast<std::uint32_t>(layout.key_values.size());
    for (std::size_t index = 0; index < layout.payload_values.size(); ++index) {
      if (auto emitted = EmitDistinctOrderValue(
              layout.payload_values[index],
              RegisterId{first.value() + key_count + static_cast<std::uint32_t>(index)});
          !emitted.has_value()) {
        return emitted;
      }
    }
    for (std::size_t index = 0; index < layout.key_values.size(); ++index) {
      if (auto emitted =
              EmitDistinctOrderValue(layout.key_values[index],
                                     RegisterId{first.value() + static_cast<std::uint32_t>(index)});
          !emitted.has_value()) {
        return emitted;
      }
    }
    return Append(InsertSorterInstruction{
        .sorter = sorter,
        .first_value = first,
        .value_count =
            static_cast<std::uint32_t>(layout.key_values.size() + layout.payload_values.size()),
    });
  }

  struct DistinctLimitTargets {
    Label advance;
    Label completion;
  };

  [[nodiscard]] LoweringResult<void> EmitDistinctLimitAfterRow(DistinctLimitTargets targets) {
    if (limit_ == nullptr) {
      return {};
    }
    if (!negative_limit_register_.has_value() || !limit_register_.has_value() ||
        !one_register_.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT LIMIT has no row-loop counters"));
    }
    const RegisterId negative = AssumeValue(negative_limit_register_);
    const RegisterId limit = AssumeValue(limit_register_);
    const RegisterId one = AssumeValue(one_register_);
    if (auto jumped = EmitJumpIf(negative, JumpCondition::kIfTrue, targets.advance);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto decremented = Append(BinaryInstruction{
            .operation = BinaryOperation::kSubtract,
            .left = limit,
            .right = one,
            .output = limit,
        });
        !decremented.has_value()) {
      return decremented;
    }
    return EmitJumpIf(limit, JumpCondition::kIfFalse, targets.completion);
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctSorterDrain() {
    if (advanced_order_ == nullptr || !sorter_.has_value() ||
        advanced_order_->core_plans.size() != 1U) {
      return std::unexpected(InternalFailure("DISTINCT sorter drain is incomplete"));
    }
    const LogicalCoreOrderLayout& layout = advanced_order_->core_plans[0].layout;
    if (layout.output_fields.size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("DISTINCT sorter output shape is invalid"));
    }
    const SorterId sorter = AssumeValue(sorter_);

    auto empty = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    std::optional<Label> positioned_completion;
    if (!empty.has_value()) {
      return std::unexpected(std::move(empty.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(sorter, *empty),
                                        "unable to emit DISTINCT sorter rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (limit_ != nullptr) {
      if (auto offset = EmitOffset(*advance, true); !offset.has_value()) {
        return offset;
      }
    }
    for (std::size_t index = 0; index < layout.output_fields.size(); ++index) {
      const SortOutputField& output = layout.output_fields[index];
      const std::uint32_t field =
          output.kind == SortOutputFieldKind::kKey
              ? output.field_index
              : static_cast<std::uint32_t>(layout.key_values.size()) + output.field_index;
      if (auto read = Append(ReadSorterFieldInstruction{
              .sorter = sorter,
              .field = field,
              .output = RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)},
          });
          !read.has_value()) {
        return read;
      }
    }
    if (auto result = Append(ResultRowInstruction{
            .first = result_block_first_,
            .count = static_cast<std::uint32_t>(layout.output_fields.size()),
        });
        !result.has_value()) {
      return result;
    }
    if (limit_ != nullptr) {
      if (!negative_limit_register_.has_value() || !limit_register_.has_value() ||
          !one_register_.has_value() || !positioned_completion.has_value()) {
        return std::unexpected(InternalFailure("DISTINCT ordered LIMIT has no counters"));
      }
      const RegisterId negative = AssumeValue(negative_limit_register_);
      const RegisterId limit = AssumeValue(limit_register_);
      const RegisterId one = AssumeValue(one_register_);
      const Label completion = AssumeValue(positioned_completion);
      if (auto jumped = EmitJumpIf(negative, JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = limit,
              .right = one,
              .output = limit,
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(limit, JumpCondition::kIfFalse, completion);
          !jumped.has_value()) {
        return jumped;
      }
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(sorter, *row),
                                     "unable to emit DISTINCT sorter advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = sorter}); !closed.has_value()) {
      return closed;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto closed = Append(CloseSorterInstruction{.sorter = sorter}); !closed.has_value()) {
        return closed;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctOpenedCompletion(std::optional<CursorId> cursor) {
    if (cursor.has_value()) {
      if (auto closed = Append(CloseCursorInstruction{.cursor = *cursor}); !closed.has_value()) {
        return closed;
      }
    }
    if (!distinct_relation_.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT completion has no relation"));
    }
    if (auto closed = Append(CloseRelationInstruction{.relation = AssumeValue(distinct_relation_)});
        !closed.has_value()) {
      return closed;
    }
    return advanced_order_ == nullptr ? Append(HaltInstruction{}) : EmitDistinctSorterDrain();
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctTableScan() {
    if (table_scan_ == nullptr || !cursor_.has_value() || !distinct_relation_.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT table scan resources are incomplete"));
    }
    const CursorId cursor = AssumeValue(cursor_);
    const RelationId relation = AssumeValue(distinct_relation_);
    std::optional<Label> closed_completion;
    if (limit_ != nullptr || (filter_ != nullptr && !filter_->guards.empty())) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      closed_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(closed_completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitDistinctGuards(closed_completion); !guards.has_value()) {
      return guards;
    }
    if (auto opened = Append(OpenRelationInstruction{.relation = relation}); !opened.has_value()) {
      return opened;
    }
    if (advanced_order_ != nullptr) {
      if (!sorter_.has_value()) {
        return std::unexpected(InternalFailure("DISTINCT order has no sorter"));
      }
      if (auto opened = Append(OpenSorterInstruction{.sorter = AssumeValue(sorter_)});
          !opened.has_value()) {
        return opened;
      }
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
      return opened;
    }

    auto unpositioned_completion = CreateLabel();
    auto row = CreateLabel();
    auto advance = CreateLabel();
    std::optional<Label> positioned_completion;
    if (!unpositioned_completion.has_value()) {
      return std::unexpected(std::move(unpositioned_completion.error()));
    }
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (limit_ != nullptr && advanced_order_ == nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
    }
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *unpositioned_completion),
                             "unable to emit DISTINCT table rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (auto filters = EmitFilters(*advance); !filters.has_value()) {
      return filters;
    }
    if (auto projected = EmitDistinctProjection(); !projected.has_value()) {
      return projected;
    }
    if (auto inserted = EmitDistinctMembership(*advance); !inserted.has_value()) {
      return inserted;
    }
    if (advanced_order_ != nullptr) {
      if (auto ordered = EmitDistinctOrderedCandidate(); !ordered.has_value()) {
        return ordered;
      }
    } else {
      if (auto offset = EmitOffset(*advance); !offset.has_value()) {
        return offset;
      }
      if (auto result = Append(ResultRowInstruction{
              .first = result_block_first_,
              .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
          });
          !result.has_value()) {
        return result;
      }
      if (limit_ != nullptr) {
        if (!positioned_completion.has_value()) {
          return std::unexpected(InternalFailure("DISTINCT LIMIT completion is unavailable"));
        }
        if (auto limited = EmitDistinctLimitAfterRow(DistinctLimitTargets{
                .advance = *advance,
                .completion = AssumeValue(positioned_completion),
            });
            !limited.has_value()) {
          return limited;
        }
      }
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *row),
                                     "unable to emit DISTINCT table advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*unpositioned_completion); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitDistinctOpenedCompletion(cursor); !completed.has_value()) {
      return completed;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitDistinctOpenedCompletion(cursor); !completed.has_value()) {
        return completed;
      }
    }
    if (closed_completion.has_value()) {
      if (auto bound = BindLabel(*closed_completion); !bound.has_value()) {
        return bound;
      }
      if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
        return halted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctSingleRow() {
    if (!distinct_relation_.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT single row has no relation"));
    }
    const RelationId relation = AssumeValue(distinct_relation_);
    std::optional<Label> closed_completion;
    if (limit_ != nullptr || (filter_ != nullptr && !filter_->guards.empty())) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      closed_completion = *label;
    }
    if (auto limit = EmitLimitInitialization(closed_completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitDistinctGuards(closed_completion); !guards.has_value()) {
      return guards;
    }
    if (auto opened = Append(OpenRelationInstruction{.relation = relation}); !opened.has_value()) {
      return opened;
    }
    if (advanced_order_ != nullptr) {
      if (!sorter_.has_value()) {
        return std::unexpected(InternalFailure("DISTINCT order has no sorter"));
      }
      if (auto opened = Append(OpenSorterInstruction{.sorter = AssumeValue(sorter_)});
          !opened.has_value()) {
        return opened;
      }
    }
    auto completion = CreateLabel();
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto filters = EmitFilters(*completion); !filters.has_value()) {
      return filters;
    }
    if (auto projected = EmitDistinctProjection(); !projected.has_value()) {
      return projected;
    }
    if (auto inserted = EmitDistinctMembership(*completion); !inserted.has_value()) {
      return inserted;
    }
    if (advanced_order_ != nullptr) {
      if (auto ordered = EmitDistinctOrderedCandidate(); !ordered.has_value()) {
        return ordered;
      }
    } else {
      if (auto offset = EmitOffset(*completion); !offset.has_value()) {
        return offset;
      }
      if (auto result = Append(ResultRowInstruction{
              .first = result_block_first_,
              .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
          });
          !result.has_value()) {
        return result;
      }
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitDistinctOpenedCompletion(std::nullopt); !completed.has_value()) {
      return completed;
    }
    if (closed_completion.has_value()) {
      if (auto bound = BindLabel(*closed_completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctPlan() {
    if (table_scan_ != nullptr) {
      return EmitDistinctTableScan();
    }
    if (std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload)) {
      return EmitDistinctSingleRow();
    }
    return std::unexpected(UnsupportedFailure("DISTINCT access plan is not implemented"));
  }

  [[nodiscard]] LoweringResult<void> EmitAccessPlan() {
    if (grouped_aggregate_query_) {
      auto input = MakeGroupedAggregateInput();
      if (!input.has_value()) {
        return std::unexpected(std::move(input.error()));
      }
      if (auto initialized = lowering_internal::EmitGroupedAggregateInitialization(*input, *this);
          !initialized.has_value()) {
        return initialized;
      }
    }
    if (std::holds_alternative<PhysicalEmptyNode>(leaf_->payload)) {
      return EmitEmpty();
    }
    if (std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload)) {
      return EmitSingleRow();
    }
    if (std::holds_alternative<PhysicalTableScanNode>(leaf_->payload)) {
      return EmitTableScan();
    }
    if (const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&leaf_->payload);
        lookup != nullptr) {
      return EmitRowIdLookup(*lookup);
    }
    if (const auto* index = std::get_if<PhysicalIndexScanNode>(&leaf_->payload); index != nullptr) {
      return index->covering ? EmitCoveringIndexScan(*index) : EmitNoncoveringIndexScan(*index);
    }
    return std::unexpected(InternalFailure("physical access node is unsupported"));
  }

  [[nodiscard]] LoweringResult<void> EmitRuntimeLimitOrderedPlan() {
    if (sort_ == nullptr || limit_ == nullptr ||
        sort_->strategy != PhysicalSortStrategy::kRuntimeLimit || !sorter_.has_value() ||
        !top_n_.has_value() || !top_n_bound_register_.has_value() || !limit_register_.has_value() ||
        !negative_limit_register_.has_value()) {
      return std::unexpected(InternalFailure("runtime LIMIT ORDER BY resources are incomplete"));
    }

    auto external = CreateLabel();
    auto completion = CreateLabel();
    if (!external.has_value()) {
      return std::unexpected(std::move(external.error()));
    }
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto initialized = EmitLimitInitialization(*completion); !initialized.has_value()) {
      return initialized;
    }
    if (auto jumped =
            EmitJumpIf(AssumeValue(negative_limit_register_), JumpCondition::kIfTrue, *external);
        !jumped.has_value()) {
      return jumped;
    }

    if (limit_->offset.has_value()) {
      if (!offset_register_.has_value() || !maximum_integer_register_.has_value() ||
          !bound_room_register_.has_value() || !comparison_register_.has_value()) {
        return std::unexpected(InternalFailure("runtime LIMIT bound has incomplete registers"));
      }
      auto maximum = EnsureMaximumIntegerConstant();
      if (!maximum.has_value()) {
        return std::unexpected(std::move(maximum.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *maximum,
              .output = AssumeValue(maximum_integer_register_),
          });
          !loaded.has_value()) {
        return loaded;
      }
      if (auto room = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(maximum_integer_register_),
              .right = AssumeValue(offset_register_),
              .output = AssumeValue(bound_room_register_),
          });
          !room.has_value()) {
        return room;
      }
      auto binary = EnsureBinarySymbol();
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      if (auto overflow = Append(CompareInstruction{
              .comparison = SqlComparison::kLess,
              .affinity = TypeAffinity::kNumeric,
              .collation = *binary,
              .left = AssumeValue(bound_room_register_),
              .right = AssumeValue(limit_register_),
              .output = AssumeValue(comparison_register_),
          });
          !overflow.has_value()) {
        return overflow;
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(comparison_register_), JumpCondition::kIfTrue, *external);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto bound = Append(BinaryInstruction{
              .operation = BinaryOperation::kAdd,
              .left = AssumeValue(limit_register_),
              .right = AssumeValue(offset_register_),
              .output = AssumeValue(top_n_bound_register_),
          });
          !bound.has_value()) {
        return bound;
      }
    } else if (auto copied = Append(CopyInstruction{
                   .input = AssumeValue(limit_register_),
                   .output = AssumeValue(top_n_bound_register_),
               });
               !copied.has_value()) {
      return copied;
    }

    ordering_runtime_ = OrderingRuntime::kTopN;
    if (auto opened = Append(OpenTopNInstruction{
            .top_n = AssumeValue(top_n_),
            .bound = AssumeValue(top_n_bound_register_),
        });
        !opened.has_value()) {
      return opened;
    }
    if (auto emitted = EmitAccessPlan(); !emitted.has_value()) {
      return emitted;
    }

    if (auto bound = BindLabel(*external); !bound.has_value()) {
      return bound;
    }
    ordering_runtime_ = OrderingRuntime::kSorter;
    if (auto opened = Append(OpenSorterInstruction{.sorter = AssumeValue(sorter_)});
        !opened.has_value()) {
      return opened;
    }
    if (auto emitted = EmitAccessPlan(); !emitted.has_value()) {
      return emitted;
    }

    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregatePlan() {
    return lowering_internal::EmitGlobalAggregateAccess(
        lowering_internal::GlobalAggregateAccessInput{
            .builder = builder_.has_value() ? &*builder_ : nullptr,
            .bound_select = bound_select_,
            .expressions = expressions_,
            .aggregate_functions = aggregate_functions_,
            .aggregate_invocations = aggregate_invocations_,
            .aggregate_descriptors =
                std::span<const std::optional<AggregateId>>{aggregate_descriptors_},
            .aggregate_argument_blocks =
                std::span<const std::optional<RegisterId>>{aggregate_argument_blocks_},
            .leaf = leaf_,
            .table_scan = table_scan_,
            .rowid_lookup = rowid_lookup_,
            .index_scan = index_scan_,
            .filter = filter_,
            .limit = active_aggregate_output_.has_value() ? nullptr : limit_,
            .aggregate = aggregate_,
            .cursor = cursor_,
            .index_cursor = index_cursor_,
            .index_key_first = index_key_first_,
            .index_key_capacity = index_key_capacity_,
            .index_rowid_register = index_rowid_register_,
            .indexed_min_max_null_constant = indexed_min_max_null_constant_,
        },
        *this);
  }

  [[nodiscard]] LoweringResult<lowering_internal::AggregateModifierInput>
  MakeAggregateModifierInput() {
    if (!builder_.has_value() || aggregate_program_ == nullptr ||
        aggregate_modifier_runtimes_.size() != aggregate_invocations_.size()) {
      return std::unexpected(InternalFailure("aggregate modifier runtime input is incomplete"));
    }
    const std::optional<RegisterId> representative_effect =
        grouped_aggregate_query_ ? grouped_representative_effect_register_
                                 : global_representative_effect_register_;
    const std::optional<ConstantId> representative_replace =
        grouped_aggregate_query_ ? grouped_representative_replace_constant_
                                 : global_representative_replace_constant_;
    return lowering_internal::AggregateModifierInput{
        .builder = &*builder_,
        .program = aggregate_program_,
        .invocations = aggregate_invocations_,
        .aggregates = std::span<const std::optional<AggregateId>>{aggregate_descriptors_},
        .argument_blocks = std::span<const std::optional<RegisterId>>{aggregate_argument_blocks_},
        .result_registers = std::span<const std::optional<RegisterId>>{aggregate_result_registers_},
        .runtimes =
            std::span<const std::optional<lowering_internal::AggregateModifierRuntime>>{
                aggregate_modifier_runtimes_},
        .representative_effect_register = representative_effect,
        .representative_replace_constant = representative_replace,
        .representative_keep_constant = one_constant_,
        .global_seen_row_register = global_modifier_seen_row_register_,
        .zero_constant = zero_constant_,
        .global_filtered_controller_defaults = global_filtered_controller_defaults_,
    };
  }

  [[nodiscard]] LoweringResult<void> PrepareGroupedAggregateConstants() {
    auto zero = EnsureZeroConstant();
    auto one = EnsureOneConstant();
    auto binary = EnsureBinarySymbol();
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    if (!one.has_value()) {
      return std::unexpected(std::move(one.error()));
    }
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    if (grouped_representative_replace_register_.has_value() &&
        !grouped_representative_replace_constant_.has_value()) {
      auto replacement = AddConstant(SqlValue::Integer(
          static_cast<std::int64_t>(BytecodeAggregateStepEffect::kReplaceRepresentative)));
      if (!replacement.has_value()) {
        return std::unexpected(std::move(replacement.error()));
      }
      grouped_representative_replace_constant_ = *replacement;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<lowering_internal::GroupedAggregateInput>
  MakeGroupedAggregateInput() {
    if (!builder_.has_value() || aggregate_program_ == nullptr || aggregate_ == nullptr ||
        !grouped_current_group_first_.has_value() || !grouped_previous_group_first_.has_value() ||
        !grouped_has_group_register_.has_value() || !grouped_comparison_register_.has_value() ||
        !zero_constant_.has_value() || !one_constant_.has_value() || !binary_symbol_.has_value()) {
      return std::unexpected(InternalFailure("grouped aggregate runtime input is incomplete"));
    }
    lowering_internal::GroupedAggregateInput input{
        .builder = &*builder_,
        .program = aggregate_program_,
        .aggregate = aggregate_,
        .collation_symbols = collation_symbols_,
        .retained_leaves = grouped_retained_leaves_,
        .retained_leaf_registers = grouped_retained_leaf_registers_,
        .representative_registers = grouped_representative_registers_,
        .group_sorter = grouped_sorter_,
        .group_sorter_record_first = grouped_sorter_record_first_,
        .current_group_first = AssumeValue(grouped_current_group_first_),
        .previous_group_first = AssumeValue(grouped_previous_group_first_),
        .has_group_register = AssumeValue(grouped_has_group_register_),
        .comparison_register = AssumeValue(grouped_comparison_register_),
        .representative_effect_register = grouped_representative_effect_register_,
        .representative_replace_register = grouped_representative_replace_register_,
        .representative_replace_constant = grouped_representative_replace_constant_,
        .binary_collation = *binary_symbol_,
        .zero_constant = *zero_constant_,
        .one_constant = *one_constant_,
        .has_representative_controller = !aggregate_program_->representative_leaves.empty() &&
                                         HasAggregateRepresentativeController(),
        .may_stop_early =
            active_aggregate_output_.has_value()
                ? active_aggregate_output_->mode == SelectOutputMode::DemandOne ||
                      (active_aggregate_output_->mode == SelectOutputMode::Streaming &&
                       limit_ != nullptr)
                : limit_ != nullptr && !HasGroupedOuterOrder(),
        .stops_after_output = active_aggregate_output_.has_value() &&
                              active_aggregate_output_->mode == SelectOutputMode::DemandOne,
    };
    return input;
  }

  [[nodiscard]] LoweringResult<void> EmitGroupedAggregatePlan() {
    if (auto constants = PrepareGroupedAggregateConstants(); !constants.has_value()) {
      return constants;
    }
    if (active_aggregate_output_.has_value()) {
      return EmitAccessPlan();
    }
    if (HasGroupedOuterOrder()) {
      if (sort_->strategy == PhysicalSortStrategy::kRuntimeLimit) {
        return EmitRuntimeLimitOrderedPlan();
      }
      if (sort_->strategy != PhysicalSortStrategy::kExternal || !sorter_.has_value()) {
        return std::unexpected(
            InternalFailure("grouped external ordering resources are incomplete"));
      }
      ordering_runtime_ = OrderingRuntime::kSorter;
      if (auto opened = Append(OpenSorterInstruction{.sorter = *sorter_}); !opened.has_value()) {
        return opened;
      }
      return EmitAccessPlan();
    }

    std::optional<Label> limit_zero;
    if (limit_ != nullptr) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      limit_zero = *label;
    }
    if (auto initialized = EmitLimitInitialization(limit_zero); !initialized.has_value()) {
      return initialized;
    }
    if (auto emitted = EmitAccessPlan(); !emitted.has_value()) {
      return emitted;
    }
    if (limit_zero.has_value()) {
      if (auto bound = BindLabel(*limit_zero); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitPlan() {
    if (sort_ == nullptr) {
      return EmitAccessPlan();
    }
    if (sort_->strategy == PhysicalSortStrategy::kRuntimeLimit) {
      return EmitRuntimeLimitOrderedPlan();
    }
    if (!sorter_.has_value()) {
      return std::unexpected(InternalFailure("external ORDER BY has no sorter descriptor"));
    }
    ordering_runtime_ = OrderingRuntime::kSorter;
    if (auto opened = Append(OpenSorterInstruction{.sorter = AssumeValue(sorter_)});
        !opened.has_value()) {
      return opened;
    }
    return EmitAccessPlan();
  }

  [[nodiscard]] bool IsRowLoopAccess() const noexcept {
    return table_scan_ != nullptr || index_scan_ != nullptr;
  }

  [[nodiscard]] bool IsCursorAccess() const noexcept {
    return table_scan_ != nullptr || rowid_lookup_ != nullptr || index_scan_ != nullptr;
  }

  enum class OrderingRuntime : std::uint8_t {
    kNone,
    kSorter,
    kTopN,
  };

  std::optional<PhysicalQueryView> read_query_;
  std::optional<LogicalQueryView> logical_query_;
  std::optional<BoundQueryView> bound_query_;
  const PhysicalPlan* read_owner_ = nullptr;
  const PhysicalQueryView* read_plan_ = nullptr;
  const LogicalQueryView* logical_plan_ = nullptr;
  const BoundQueryView* bound_select_ = nullptr;
  const PhysicalMutationPlan* mutation_plan_ = nullptr;
  const BoundInsert* bound_insert_ = nullptr;
  const BoundDelete* bound_delete_ = nullptr;
  const BoundUpdate* bound_update_ = nullptr;
  const BoundCreateTable* bound_create_ = nullptr;
  const BoundCreateIndex* bound_create_index_ = nullptr;
  const BoundAnalyze* bound_analyze_ = nullptr;
  std::span<const BoundExpression> expressions_;
  std::span<const BoundParameter> parameters_;
  std::span<const BoundCollation> collations_;
  std::span<const BoundScalarFunction> functions_;
  std::span<const BoundAggregateFunction> aggregate_functions_;
  std::span<const BoundAggregateInvocation> aggregate_invocations_;
  std::span<const BoundSourceColumn> source_columns_;
  ProgramLimits limits_;
  StatementLoweringState& statement_;
  std::optional<Label> subquery_completion_;

  const PhysicalNode* leaf_ = nullptr;
  const PhysicalTableScanNode* table_scan_ = nullptr;
  const PhysicalRowIdLookupNode* rowid_lookup_ = nullptr;
  const PhysicalIndexScanNode* index_scan_ = nullptr;
  const PhysicalGuardNode* guard_ = nullptr;
  const PhysicalFilterNode* filter_ = nullptr;
  const PhysicalLimitNode* limit_ = nullptr;
  const PhysicalProjectionNode* projection_ = nullptr;
  const PhysicalSortNode* sort_ = nullptr;
  const PhysicalOutputNode* output_ = nullptr;
  const PhysicalDistinctNode* distinct_ = nullptr;
  const PhysicalAdvancedOrderNode* advanced_order_ = nullptr;
  const PhysicalAggregateNode* aggregate_ = nullptr;
  const PhysicalHavingNode* having_ = nullptr;
  const BoundSelectCore* aggregate_core_ = nullptr;
  const BoundAggregateProgram* aggregate_program_ = nullptr;
  bool global_aggregate_query_ = false;
  bool global_representative_query_ = false;
  bool grouped_aggregate_query_ = false;
  bool streaming_aggregate_query_ = false;
  bool streaming_query_ = false;
  bool set_query_ = false;
  bool set_limit_one_ = false;
  bool ordered_compound_ = false;
  bool streaming_requires_snapshot_ = false;
  std::size_t active_source_column_begin_ = 0;
  std::size_t active_source_column_count_ = 0;
  std::vector<StreamingCorePlan> streaming_cores_;
  std::optional<SelectOutputContext> active_aggregate_output_{};
  std::optional<Label> aggregate_core_stopped_label_{};
  std::optional<Label> aggregate_core_completion_label_{};
  std::array<std::optional<RelationId>, 3> set_relations_{};
  std::array<std::optional<RegisterId>, 3> set_count_registers_{};
  std::array<std::optional<SorterId>, 3> ordered_sorters_{};
  std::array<std::optional<SorterId>, 3> ordered_full_sorters_{};
  std::optional<TopNId> ordered_top_n_;
  std::optional<TopNId> ordered_full_top_n_;
  std::optional<RecordComparisonId> ordered_comparison_;
  std::optional<RecordComparisonId> ordered_full_comparison_;

  std::size_t& next_register_;
  std::uint32_t register_count_ = 0;
  std::uint32_t parameter_count_ = 0;
  RegisterId result_block_first_{0};
  std::vector<std::optional<RegisterId>>& call_argument_blocks_;
  std::vector<std::optional<RegisterId>> aggregate_argument_blocks_;
  std::vector<std::optional<RegisterId>> aggregate_result_registers_;
  std::vector<std::optional<AggregateId>> aggregate_descriptors_;
  std::vector<std::optional<lowering_internal::AggregateModifierRuntime>>
      aggregate_modifier_runtimes_;
  std::optional<RegisterId> limit_register_;
  std::optional<RegisterId> offset_register_;
  std::optional<RegisterId> zero_register_;
  std::optional<RegisterId> one_register_;
  std::optional<RegisterId> comparison_register_;
  std::optional<RegisterId> negative_limit_register_;
  std::optional<RegisterId> index_key_first_;
  std::uint32_t index_key_capacity_ = 0;
  std::optional<RegisterId> index_rowid_register_;
  std::optional<RegisterId> sorter_record_first_;
  std::optional<RegisterId> top_n_bound_register_;
  std::optional<RegisterId> maximum_integer_register_;
  std::optional<RegisterId> bound_room_register_;
  std::optional<RegisterId> ordered_left_first_;
  std::optional<RegisterId> ordered_right_first_;
  std::optional<RegisterId> ordered_comparison_register_;
  std::optional<RegisterId> ordered_predicate_register_;
  std::optional<RegisterId> ordered_overflow_register_;
  std::optional<RegisterId> grouped_current_group_first_;
  std::optional<RegisterId> grouped_previous_group_first_;
  std::optional<RegisterId> grouped_has_group_register_;
  std::optional<RegisterId> grouped_comparison_register_;
  std::optional<RegisterId> grouped_sorter_record_first_;
  std::optional<RegisterId> grouped_representative_effect_register_;
  std::optional<RegisterId> grouped_representative_replace_register_;
  std::vector<BoundExpressionId> grouped_retained_leaves_;
  std::vector<RegisterId> grouped_retained_leaf_registers_;
  std::vector<RegisterId> grouped_representative_registers_;
  std::vector<AggregateLeafRegister> grouped_sorted_register_map_;
  std::vector<AggregateLeafRegister> grouped_representative_register_map_;
  std::vector<std::optional<RegisterId>> grouped_key_expression_registers_;
  std::optional<RegisterId> global_has_representative_row_register_;
  std::optional<RegisterId> global_representative_effect_register_;
  std::optional<RegisterId> global_representative_replace_register_;
  std::optional<RegisterId> global_representative_comparison_register_;
  std::optional<RegisterId> global_modifier_seen_row_register_;
  std::vector<RegisterId> streaming_global_representative_registers_;
  std::vector<RegisterId> streaming_grouped_representative_registers_;
  std::vector<RegisterId> streaming_grouped_retained_registers_;
  std::vector<BoundExpressionId> global_representative_leaves_;
  std::vector<RegisterId> global_representative_registers_;
  std::vector<AggregateLeafRegister> global_representative_register_map_;

  std::optional<ProgramBuilder>& builder_;
  std::vector<std::optional<ConstantId>>& literal_constants_;
  std::optional<ConstantId>& null_constant_;
  std::optional<ConstantId>& zero_constant_;
  std::optional<ConstantId>& one_constant_;
  std::optional<ConstantId>& maximum_integer_constant_;
  std::optional<ConstantId> indexed_min_max_null_constant_;
  std::optional<ConstantId> grouped_representative_replace_constant_;
  std::optional<ConstantId> global_representative_replace_constant_;
  std::vector<SymbolId>& collation_symbols_;
  std::vector<SymbolId>& function_symbols_;
  std::vector<SymbolId>& aggregate_function_symbols_;
  std::vector<NamedSymbol>& symbol_names_;
  std::optional<SymbolId>& binary_symbol_;
  std::optional<CursorId> cursor_;
  std::optional<CursorId> index_cursor_;
  std::optional<SorterId> sorter_;
  std::optional<SorterId> grouped_sorter_;
  std::optional<TopNId> top_n_;
  std::optional<RelationId> distinct_relation_;
  std::vector<std::optional<CursorFieldId>> source_cursor_fields_;
  std::vector<std::optional<SourceCursorBinding>> source_cursor_bindings_;
  std::vector<bool> source_field_real_affinity_;
  std::vector<RegisterId> source_snapshot_registers_;
  std::optional<RegisterId> source_rowid_register_;
  std::optional<RegisterId> insert_values_first_;
  std::optional<RegisterId> insert_rowid_register_;
  std::optional<RegisterId> insert_record_register_;
  std::vector<std::optional<ConstantId>> insert_default_constants_;
  std::optional<RegisterId> mutation_index_key_first_;
  std::uint32_t mutation_index_key_capacity_ = 0;
  std::optional<RegisterId> update_values_first_;
  std::optional<RegisterId> update_new_rowid_register_;
  std::optional<RegisterId> update_record_register_;
  std::optional<WriteCursorId> write_cursor_;
  std::vector<WriteCursorId> index_write_cursors_;
  std::optional<CursorId> create_index_table_cursor_;
  std::optional<WriteCursorId> schema_write_cursor_;
  std::optional<WriteCursorId> create_index_write_cursor_;
  std::optional<WriteCursorId> stat1_write_cursor_;
  std::vector<CursorId> analyze_index_cursors_;
  std::vector<CursorId> analyze_table_cursors_;
  OrderingRuntime ordering_runtime_ = OrderingRuntime::kNone;
  lowering_internal::AggregateExpressionSource aggregate_expression_source_ =
      lowering_internal::AggregateExpressionSource::AccessRow;
  bool grouped_output_phase_ = false;
  bool global_filtered_controller_defaults_ = false;
  bool limit_initialized_ = false;
};

}  // namespace

ErrorCode PlanLoweringError::base_error_code() const noexcept {
  switch (code) {
    case PlanLoweringErrorCode::kInvalidInput:
      return ErrorCode::kMisuse;
    case PlanLoweringErrorCode::kUnsupportedPlan:
      return ErrorCode::kGeneric;
    case PlanLoweringErrorCode::kResourceLimit:
      return ErrorCode::kTooLarge;
    case PlanLoweringErrorCode::kInternalInvariant:
      return ErrorCode::kInternal;
  }
  return ErrorCode::kInternal;
}

std::string_view PlanLoweringErrorCodeName(PlanLoweringErrorCode code) noexcept {
  switch (code) {
    case PlanLoweringErrorCode::kInvalidInput:
      return "invalid_input";
    case PlanLoweringErrorCode::kUnsupportedPlan:
      return "unsupported_plan";
    case PlanLoweringErrorCode::kResourceLimit:
      return "resource_limit";
    case PlanLoweringErrorCode::kInternalInvariant:
      return "internal_invariant";
  }
  return "unknown";
}

LowerPlanResult LowerPlan(const PhysicalPlan& plan, ProgramLimits limits) {
  if (!plan.valid()) {
    return std::unexpected(PlanLoweringError{
        .code = PlanLoweringErrorCode::kInvalidInput,
        .detail = "physical plan is invalid",
    });
  }
  StatementLoweringState state;
  return PlanLowerer(plan, limits, state).Run();
}

LowerPlanResult LowerPlan(const PhysicalMutationPlan& plan, ProgramLimits limits) {
  if (!plan.valid()) {
    return std::unexpected(PlanLoweringError{
        .code = PlanLoweringErrorCode::kInvalidInput,
        .detail = "physical mutation plan is invalid",
    });
  }
  StatementLoweringState state;
  return PlanLowerer(plan, limits, state).Run();
}

}  // namespace modern_sqlite
