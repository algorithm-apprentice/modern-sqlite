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
#include "modern_sqlite/runtime/function_registry.hpp"
#include "plan_lowering_internal.hpp"

namespace modern_sqlite {
namespace {

template <typename T>
using LoweringResult = lowering_internal::LoweringResult<T>;

using lowering_internal::AssumeValue;
using lowering_internal::ConvertProgramResult;
using lowering_internal::EmitIndexTableLookup;
using lowering_internal::EmitIndexTraversalEndCheck;
using lowering_internal::EmitIndexTraversalSeek;
using lowering_internal::EmitRowIdTraversalEndCheck;
using lowering_internal::IndexKeyPhase;
using lowering_internal::IndexTraversal;
using lowering_internal::IndexTraversalBound;
using lowering_internal::InternalFailure;
using lowering_internal::ProgramFailure;
using lowering_internal::ResolveIndexTraversal;
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

class PlanLowerer;

enum class ConditionalBranch : std::uint8_t {
  kTrue,
  kFalse,
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
  std::vector<std::unique_ptr<PlanLowerer>> query_frames;
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
        source_columns_(bound_select_->source_columns()),
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
  enum class ReadPlanKind : std::uint8_t {
    kOrdinary,
    kDistinct,
    kStreaming,
    kGlobalAggregate,
    kGroupedAggregate,
    NativeCompound,
  };

  [[nodiscard]] LoweringResult<void> PrepareReadLayout() {
    if (bound_select_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound SELECT does not retain a catalog"));
    }
    std::optional<PhysicalNodeId> compound_root;
    bool native = false;
    for (std::size_t index = 0; index < read_plan_->nodes().size(); ++index) {
      if (const auto* compound =
              std::get_if<PhysicalCompoundNode>(&read_plan_->nodes()[index].payload)) {
        compound_root = PhysicalNodeId{static_cast<std::uint32_t>(index)};
        native = native || compound->strategy == PhysicalCompoundStrategy::kOrderedMerge;
      }
    }
    if (native) {
      native_compound_root_ = compound_root;
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
    if (!native_compound_root_.has_value() && !streaming_cores_.empty() &&
        !bound_select_->order_by().empty()) {
      if (streaming_cores_.size() != 1U) {
        return std::unexpected(InternalFailure("ordered compound has no native producer tree"));
      }
      const StreamingCorePlan& core = streaming_cores_.front();
      if (core.projection != nullptr) {
        native_compound_root_ = PhysicalNodeId{core.projection->logical_projection.value()};
      } else if (core.values != nullptr && core.leaf != nullptr) {
        native_compound_root_ =
            PhysicalNodeId{static_cast<std::uint32_t>(core.leaf - read_plan_->nodes().data())};
      } else {
        return std::unexpected(InternalFailure("ordered source has no result producer"));
      }
    }
    read_plan_kind_ = native_compound_root_.has_value() ? ReadPlanKind::NativeCompound
                      : grouped_aggregate_query_        ? ReadPlanKind::kGroupedAggregate
                      : global_aggregate_query_         ? ReadPlanKind::kGlobalAggregate
                      : streaming_query_                ? ReadPlanKind::kStreaming
                      : distinct_ != nullptr            ? ReadPlanKind::kDistinct
                                                        : ReadPlanKind::kOrdinary;
    LoweringResult<void> layout = native_compound_root_.has_value() ? BuildNativeRegisterLayout()
                                  : streaming_aggregate_query_
                                      ? BuildStreamingAggregateRegisterLayout()
                                  : grouped_aggregate_query_ ? BuildGroupedAggregateRegisterLayout()
                                  : global_aggregate_query_  ? BuildGlobalAggregateRegisterLayout()
                                  : streaming_query_         ? BuildStreamingRegisterLayout()
                                  : distinct_ != nullptr     ? BuildDistinctRegisterLayout()
                                                             : BuildReadRegisterLayout();
    if (!layout.has_value()) {
      return std::unexpected(std::move(layout.error()));
    }
    return {};
  }

  [[nodiscard]] bool RequiresReadSnapshot() const noexcept {
    return streaming_requires_snapshot_ ||
           (!streaming_query_ && !bound_select_->table_sources().empty());
  }

  [[nodiscard]] LowerPlanResult RunRead() {
    if (auto prepared = PrepareReadLayout(); !prepared.has_value()) {
      return std::unexpected(std::move(prepared.error()));
    }
    const std::size_t nested_count = bound_select_->owner().query_count() - 1U;
    auto& children = statement_.query_frames;
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
    if (auto producers = EmitNativeCoroutineBodies(); !producers.has_value()) {
      return std::unexpected(std::move(producers.error()));
    }
    for (const auto& child : children) {
      if (auto emitted = child->EmitReadSubroutine(); !emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
      if (auto producers = child->EmitNativeCoroutineBodies(); !producers.has_value()) {
        return std::unexpected(std::move(producers.error()));
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
    if (read_descriptors_added_) {
      return {};
    }
    auto added = BuildReadDescriptors();
    if (added.has_value()) {
      read_descriptors_added_ = true;
    }
    return added;
  }

  [[nodiscard]] LoweringResult<void> BuildReadDescriptors() {
    if (native_compound_root_.has_value()) {
      return AddNativeReadDescriptors();
    }
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
      if (streaming_query_) {
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
    if (!streaming_query_) {
      if (auto cursor = AddCursorDescriptor(); !cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitReadPlan() {
    switch (read_plan_kind_) {
      case ReadPlanKind::kOrdinary:
        return EmitPlan();
      case ReadPlanKind::kDistinct:
        return EmitDistinctPlan();
      case ReadPlanKind::kStreaming:
        return EmitStreamingPlan();
      case ReadPlanKind::kGlobalAggregate:
        return EmitGlobalAggregatePlan();
      case ReadPlanKind::kGroupedAggregate:
        return EmitGroupedAggregatePlan();
      case ReadPlanKind::NativeCompound:
        return EmitNativeCompoundPlan();
    }
    return std::unexpected(InternalFailure("query emission strategy is invalid"));
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
    if (auto bound = BindLabel(AssumeValue(call.entry)); !bound.has_value()) {
      return bound;
    }
    if (auto emitted = EmitNestedQueryBody(); !emitted.has_value()) {
      return emitted;
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

  [[nodiscard]] LoweringResult<void> EmitNestedQueryBody() {
    if (read_registers_->active_aggregate_output.has_value() ||
        read_registers_->aggregate_core_stopped_label.has_value() ||
        read_registers_->aggregate_core_completion_label.has_value()) {
      return std::unexpected(InternalFailure("query emission retains an active output context"));
    }
    read_registers_->limit_initialized = false;
    read_registers_->ordering_runtime = OrderingRuntime::kNone;
    read_registers_->aggregate_expression_source =
        lowering_internal::AggregateExpressionSource::AccessRow;
    read_registers_->grouped_output_phase = false;
    auto completed = CreateLabel();
    if (!completed.has_value()) {
      return std::unexpected(std::move(completed.error()));
    }
    subquery_completion_ = *completed;
    const SubqueryCallBinding& call = statement_.subqueries[bound_select_->query().id.value() - 1U];
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
    return BindLabel(*completed);
  }

  [[nodiscard]] LoweringResult<void> EmitInlineQuery(PlanLowerer& parent) {
    if (bound_select_->query().parent != parent.bound_select_->query().id) {
      return std::unexpected(InternalFailure("inline query has an invalid lexical parent"));
    }
    if (auto descriptors = AddReadDescriptors(); !descriptors.has_value()) {
      return descriptors;
    }
    const auto binding = parent.CurrentReadEmissionBinding();
    if (native_compound_root_.has_value() && lexical_parent_.has_value() &&
        *lexical_parent_ != binding) {
      return std::unexpected(
          InternalFailure("native query is reused across different lexical read bindings"));
    }
    lexical_parent_ = binding;
    return EmitNestedQueryBody();
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
        AssumeValue(builder_).SetExecutionMetadata(
            ProgramStatementKind::kDelete, ProgramTransactionAccess::kWrite, rollback_mode,
            MutationResultMetadata{
                .publishes_changes = true,
                .publishes_last_insert_rowid = false,
            },
            ProgramWriteEffects{
                .multi_write = delete_plan.access.kind == MutationAccessKind::kTableScan,
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
        AssumeValue(builder_).SetExecutionMetadata(
            ProgramStatementKind::kUpdate, ProgramTransactionAccess::kWrite, rollback_mode,
            MutationResultMetadata{
                .publishes_changes = true,
                .publishes_last_insert_rowid = false,
            },
            ProgramWriteEffects{
                .multi_write = update.access.kind == MutationAccessKind::kTableScan,
                .may_abort = update.access.kind != MutationAccessKind::kEmpty &&
                             bound_update_->changes_rowid(),
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
      if (auto indexes =
              AddMutationIndexWriteCursorDescriptors(bound_update_->target(), bound_update_);
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
        AssumeValue(builder_).SetExecutionMetadata(
            ProgramStatementKind::kCreateTable, ProgramTransactionAccess::kWrite, rollback_mode, {},
            ProgramWriteEffects{
                .multi_write = !create.no_op,
                .may_abort = !create.no_op,
            }),
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
        AssumeValue(builder_).SetExecutionMetadata(
            ProgramStatementKind::kCreateIndex, ProgramTransactionAccess::kWrite, rollback_mode, {},
            ProgramWriteEffects{
                .multi_write = !create.no_op,
                .may_abort = !create.no_op,
            }),
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
      constexpr std::size_t kCreateIndexFixedInstructions = 25;
      constexpr std::size_t kCreateIndexBranchLabels = 4;
      const std::size_t key_term_count = bound_create_index_->terms().size();
      if (key_term_count >
          std::numeric_limits<std::uint32_t>::max() - kCreateIndexFixedInstructions) {
        return std::unexpected(
            ProgramFailure(ProgramError{.code = ProgramErrorCode::kInstructionLimitExceeded},
                           "CREATE INDEX instruction count exceeds the bytecode identity range"));
      }
      auto instructions = ConvertProgramResult(AssumeValue(builder_).ReserveInstructionCapacity(
                                                   key_term_count + kCreateIndexFixedInstructions),
                                               "unable to reserve CREATE INDEX instructions");
      if (!instructions.has_value()) {
        return std::unexpected(std::move(instructions.error()));
      }
      auto labels =
          ConvertProgramResult(AssumeValue(builder_).ReserveLabelCapacity(kCreateIndexBranchLabels),
                               "unable to reserve CREATE INDEX branch labels");
      if (!labels.has_value()) {
        return std::unexpected(std::move(labels.error()));
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
        AssumeValue(builder_).SetExecutionMetadata(
            ProgramStatementKind::kAnalyze, ProgramTransactionAccess::kWrite, rollback_mode, {},
            ProgramWriteEffects{
                .multi_write = analyze.creates_stat1 ||
                               bound_analyze_->scope() != BoundAnalyzeScope::kDatabase,
                .may_abort = analyze.creates_stat1 || !bound_analyze_->indexes().empty(),
            }),
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

  struct IndexNullScan {
    RegisterId pass;
    Label second_pass;
    Label exhausted;
    bool nulls_first = false;
  };

  struct JoinedSourcePlan {
    const PhysicalNode* access = nullptr;
    BoundSourceId source{0};
    const BoundTableSource* declaration = nullptr;
    std::optional<CursorId> cursor{};
    std::optional<CursorId> table_cursor{};
    std::optional<RegisterId> access_key_first{};
    std::uint32_t access_key_capacity = 0;
    std::optional<RegisterId> index_null_pass{};
    std::optional<RelationId> automatic_relation{};
    std::optional<CursorId> construction_cursor{};
    std::optional<RegisterId> construction_record_first{};
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
    const PhysicalRowIdRangeNode* rowid_range = nullptr;
    const PhysicalIndexScanNode* index_scan = nullptr;
    const PhysicalNestedLoopJoinNode* join = nullptr;
    std::vector<JoinedSourcePlan> joined_sources{};
    std::span<const PhysicalNode> source_nodes{};
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
    std::span<const BoundExpressionId> global_representative_leaves{};
    std::vector<BoundExpressionId> grouped_retained_leaves{};
    bool global_filtered_controller_defaults = false;
    std::optional<std::size_t> native_scope{};
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
    core.joined_sources.push_back(JoinedSourcePlan{
        .access = &access,
        .source = source,
        .declaration = &select->table_sources[source_index],
    });
    return true;
  }

  enum class SelectOutputMode : std::uint8_t {
    Streaming,
    Ordered,
  };

  [[nodiscard]] LoweringResult<void> EmitProjectionValue(
      BoundExpressionId expression, RegisterId destination,
      SelectOutputMode mode = SelectOutputMode::Streaming) {
    if (bound_select_->query().destination == BoundQueryDestination::kExists &&
        !read_registers_->row_destination.has_value() && mode == SelectOutputMode::Streaming) {
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
    std::optional<RegisterId> ordered_record_first{};
    std::optional<CoroutineId> coroutine{};
  };

  enum class OrderingRuntime : std::uint8_t {
    kNone,
    kSorter,
    kTopN,
  };

  struct ReadRegisterLayout {
    RegisterId result_block_first{0};
    std::optional<SelectOutputContext> row_destination;
    std::optional<RegisterId> limit_input;
    std::optional<RegisterId> limit_register;
    std::optional<RegisterId> offset_register;
    std::optional<RegisterId> zero_register;
    std::optional<RegisterId> one_register;
    std::optional<RegisterId> comparison_register;
    std::optional<RegisterId> negative_limit_register;
    std::optional<RegisterId> index_key_first;
    std::uint32_t index_key_capacity = 0;
    std::optional<RegisterId> index_null_pass;
    std::optional<RegisterId> index_rowid_register;
    std::optional<RegisterId> rowid_range_rowid_register;
    std::optional<RegisterId> rowid_range_comparison_register;
    std::optional<RegisterId> sorter_record_first;
    std::optional<RegisterId> top_n_bound_register;
    std::optional<RegisterId> maximum_integer_register;
    std::optional<RegisterId> bound_room_register;
    std::optional<RegisterId> ordered_left_first;
    std::optional<RegisterId> ordered_right_first;
    std::optional<RegisterId> ordered_comparison_register;
    std::optional<RegisterId> ordered_predicate_register;
    std::optional<RegisterId> compound_previous_first;
    std::optional<RegisterId> compound_has_previous;
    std::optional<RegisterId> grouped_current_group_first;
    std::optional<RegisterId> grouped_previous_group_first;
    std::optional<RegisterId> grouped_has_group_register;
    std::optional<RegisterId> grouped_comparison_register;
    std::optional<RegisterId> grouped_sorter_record_first;
    std::optional<RegisterId> grouped_representative_effect_register;
    std::optional<RegisterId> grouped_representative_replace_register;
    std::vector<RegisterId> grouped_retained_leaf_registers;
    std::vector<RegisterId> grouped_representative_registers;
    std::vector<AggregateLeafRegister> grouped_sorted_register_map;
    std::vector<AggregateLeafRegister> grouped_representative_register_map;
    std::optional<RegisterId> global_has_representative_row_register;
    std::optional<RegisterId> global_representative_effect_register;
    std::optional<RegisterId> global_representative_replace_register;
    std::optional<RegisterId> global_representative_comparison_register;
    std::optional<RegisterId> global_modifier_seen_row_register;
    std::vector<RegisterId> streaming_global_representative_registers;
    std::vector<RegisterId> streaming_grouped_representative_registers;
    std::vector<RegisterId> streaming_grouped_retained_registers;
    std::vector<RegisterId> global_representative_registers;
    std::vector<AggregateLeafRegister> global_representative_register_map;
    std::optional<CursorId> cursor;
    std::optional<CursorId> index_cursor;
    std::optional<SorterId> sorter;
    std::optional<SorterId> grouped_sorter;
    std::optional<TopNId> top_n;
    std::optional<RegisterId> order_prefix_previous_first;
    std::optional<RegisterId> order_prefix_present;
    std::optional<RegisterId> order_prefix_comparison_register;
    std::optional<RegisterId> encoded_order_record;
    std::optional<RecordComparisonId> order_prefix_comparison;
    std::optional<RelationId> distinct_relation;
    std::optional<RecordComparisonId> ordered_comparison;
    std::optional<RecordComparisonId> ordered_full_comparison;
    std::optional<SelectOutputContext> active_aggregate_output{};
    std::optional<Label> aggregate_core_stopped_label{};
    std::optional<Label> aggregate_core_completion_label{};
    OrderingRuntime ordering_runtime = OrderingRuntime::kNone;
    lowering_internal::AggregateExpressionSource aggregate_expression_source =
        lowering_internal::AggregateExpressionSource::AccessRow;
    bool grouped_output_phase = false;
    bool global_filtered_controller_defaults = false;
    bool limit_initialized = false;
  };

  struct NativeProducer {
    PhysicalNodeId root{0};
    std::optional<PhysicalNodeId> merge{};
    std::size_t source_begin = 0;
    std::size_t source_count = 0;
    std::optional<std::size_t> left{};
    std::optional<std::size_t> right{};
    std::optional<std::size_t> parent{};
    bool has_input_limit = false;
    // Whether this emitter has a Yield edge, not whether its table contains rows.
    bool emits_rows = true;
    bool required = true;
    std::optional<ReadRegisterLayout> registers{};
    std::optional<Label> entry{};
  };

  struct ReadEmissionBinding {
    PlanLowerer* frame;
    ReadRegisterLayout* registers;
    const StreamingCorePlan* core;
    const PhysicalCoreOrderPlan* order;
    lowering_internal::AggregateExpressionSource source;
    bool grouped_output;

    constexpr bool operator==(const ReadEmissionBinding&) const noexcept = default;
  };

  [[nodiscard]] ReadEmissionBinding CurrentReadEmissionBinding() noexcept {
    return ReadEmissionBinding{
        .frame = this,
        .registers = read_registers_,
        .core = active_streaming_core_,
        .order = active_core_order_,
        .source = read_registers_->aggregate_expression_source,
        .grouped_output = read_registers_->grouped_output_phase,
    };
  }

  [[nodiscard]] LoweringResult<void> ApplyReadEmissionBinding(const ReadEmissionBinding& binding) {
    if (binding.frame != this) {
      return std::unexpected(InternalFailure("read emission binding has a different query owner"));
    }
    read_registers_ = binding.registers;
    active_core_order_ = binding.order;
    if (binding.core != nullptr) {
      if (binding.core->aggregate != nullptr) {
        if (auto context = SetStreamingAggregateCoreContext(*binding.core); !context.has_value()) {
          return context;
        }
      } else {
        ClearStreamingAggregateContext();
        SetStreamingCoreContext(*binding.core);
      }
    } else if (native_compound_root_.has_value()) {
      ClearStreamingAggregateContext();
    }
    read_registers_->aggregate_expression_source = binding.source;
    read_registers_->grouped_output_phase = binding.grouped_output;
    return {};
  }

  struct JoinedRowTargets {
    Label rejected;
    std::optional<Label> stopped{};
    std::optional<Label> ordered_rejected{};
  };

  [[nodiscard]] LoweringResult<void> InspectStreamingPlan() {
    const std::span<const PhysicalNode> nodes = read_plan_->nodes();
    const std::span<const BoundQueryCore> cores = bound_select_->query_cores();
    if (nodes.empty() || cores.empty()) {
      return std::unexpected(InternalFailure("streaming query plan is empty"));
    }
    const bool ordered = !bound_select_->order_by().empty();
    streaming_query_ = true;
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
             core.rowid_lookup != nullptr || core.rowid_range != nullptr ||
             core.index_scan != nullptr || core.values != nullptr || core.join != nullptr ||
             !core.joined_sources.empty();
    };
    std::vector<std::optional<std::size_t>> node_cores(nodes.size());
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
      } else if (const auto* range = std::get_if<PhysicalRowIdRangeNode>(&physical.payload);
                 range != nullptr) {
        auto joined = InspectJoinedAccess(physical, range->core_index, range->source);
        if (!joined.has_value()) {
          return std::unexpected(std::move(joined.error()));
        }
        StreamingCorePlan& core = streaming_cores_[range->core_index];
        if (!*joined) {
          if (has_access(core)) {
            return std::unexpected(InternalFailure("streaming rowid-range core is invalid"));
          }
          core.leaf = &physical;
          core.rowid_range = range;
        }
        node_cores[index] = range->core_index;
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
      } else if (const auto* automatic = std::get_if<PhysicalAutomaticIndexNode>(&physical.payload);
                 automatic != nullptr) {
        auto joined = InspectJoinedAccess(physical, automatic->core_index, automatic->source);
        if (!joined.has_value()) {
          return std::unexpected(std::move(joined.error()));
        }
        if (!*joined) {
          return std::unexpected(InternalFailure("automatic index is not a joined source"));
        }
        node_cores[index] = automatic->core_index;
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
        if (!native_compound_root_.has_value()) {
          const bool concatenate = compound->operation == CompoundOperator::kUnionAll &&
                                   compound->strategy == PhysicalCompoundStrategy::kConcatenate;
          const bool one = compound->operation == CompoundOperator::kUnion &&
                           compound->strategy == PhysicalCompoundStrategy::kUnionLimitOne;
          if (ordered || (!concatenate && !one)) {
            return std::unexpected(
                UnsupportedFailure("ordered compound SELECT lowering is not supported"));
          }
        }
        ++compound_count;
      } else if (const auto* order = std::get_if<PhysicalAdvancedOrderNode>(&physical.payload);
                 order != nullptr) {
        if (!ordered || advanced_order_ != nullptr) {
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
        if (!ordered || output_ != nullptr || index + 1U != nodes.size()) {
          return std::unexpected(InternalFailure("streaming output metadata is invalid"));
        }
        output_ = output;
      } else {
        return std::unexpected(
            UnsupportedFailure("streaming query contains an unsupported physical node"));
      }
    }

    for (StreamingCorePlan& core : streaming_cores_) {
      if (core.join == nullptr) {
        continue;
      }
      if (core.joined_sources.empty() || core.leaf == nullptr ||
          core.joined_sources.front().access == nullptr) {
        return std::unexpected(InternalFailure("joined source arena is incomplete"));
      }
      const PhysicalNode* first = core.joined_sources.front().access;
      core.source_nodes = std::span{first, static_cast<std::size_t>(core.leaf - first) + 1U};
      const auto access = std::ranges::find(read_plan_->core_access_plans(), core.core_index,
                                            &PhysicalCoreAccessPlan::core_index);
      if (access == read_plan_->core_access_plans().end() ||
          access->source_order.size() != core.joined_sources.size()) {
        return std::unexpected(InternalFailure("joined source order is not published"));
      }
      for (std::size_t level = 0; level < access->source_order.size(); ++level) {
        const auto suffix = std::span{core.joined_sources}.subspan(level);
        const auto selected =
            std::ranges::find(suffix, access->source_order[level], &JoinedSourcePlan::source);
        if (selected == suffix.end()) {
          return std::unexpected(
              InternalFailure("joined source order is not an exact permutation"));
        }
        std::ranges::iter_swap(core.joined_sources.begin() + static_cast<std::ptrdiff_t>(level),
                               selected);
      }
    }

    if (compound_count != bound_select_->compound_operators().size()) {
      return std::unexpected(InternalFailure("streaming compound count is invalid"));
    }
    if (ordered) {
      if (advanced_order_ == nullptr || output_ == nullptr ||
          read_plan_->core_order_plans().size() != cores.size() ||
          advanced_order_->terms.size() != bound_select_->order_by().size()) {
        return std::unexpected(InternalFailure("ordered compound metadata is incomplete"));
      }
      if (advanced_order_->input_order_satisfied()) {
        if (cores.size() != 1U || !bound_select_->compound_operators().empty() ||
            (!streaming_aggregate_query_ && streaming_cores_.front().join == nullptr) ||
            advanced_order_->set_then_order) {
          return std::unexpected(InternalFailure("satisfied advanced input order is invalid"));
        }
      }
    } else if (advanced_order_ != nullptr || output_ != nullptr) {
      return std::unexpected(InternalFailure("unordered compound has ordering metadata"));
    }
    if (native_compound_root_.has_value() &&
        read_plan_->core_order_plans().size() != cores.size()) {
      return std::unexpected(InternalFailure("native compound core orders are incomplete"));
    }
    for (const StreamingCorePlan& core : streaming_cores_) {
      if (core.bound == nullptr) {
        return std::unexpected(InternalFailure("streaming core has no bound metadata"));
      }
      if (std::holds_alternative<BoundValuesCore>(*core.bound)) {
        if (core.values == nullptr || core.empty != nullptr || core.single_row != nullptr ||
            core.table_scan != nullptr || core.rowid_lookup != nullptr ||
            core.rowid_range != nullptr || core.index_scan != nullptr || core.filter != nullptr ||
            core.aggregate != nullptr || core.having != nullptr || core.projection != nullptr ||
            core.distinct != nullptr) {
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
                                     core.rowid_range != nullptr || core.index_scan != nullptr ||
                                     core.join != nullptr;
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
        if (core.aggregate != nullptr || core.having != nullptr) {
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
    rowid_lookup_ = std::get_if<PhysicalRowIdLookupNode>(&leaf_->payload);
    rowid_range_ = std::get_if<PhysicalRowIdRangeNode>(&leaf_->payload);
    index_scan_ = std::get_if<PhysicalIndexScanNode>(&leaf_->payload);
    if (!std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload) &&
        !std::holds_alternative<PhysicalEmptyNode>(leaf_->payload) && table_scan_ == nullptr &&
        rowid_lookup_ == nullptr && rowid_range_ == nullptr && index_scan_ == nullptr) {
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
            read_plan_->core_order_plans().size() != 1U ||
            read_plan_->core_order_plans()[0].layout.core_index != 0U ||
            read_plan_->core_order_plans()[0].strategy != PhysicalSortStrategy::kExternal ||
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
    global_representative_leaves_ = aggregate_program_->representative_leaves;
    global_representative_query_ = !global_representative_leaves_.empty();
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      if (invocation_id.value() >= aggregate_invocations_.size()) {
        return std::unexpected(InternalFailure("aggregate invocation ID is out of range"));
      }
    }

    leaf_ = &nodes.front();
    table_scan_ = std::get_if<PhysicalTableScanNode>(&leaf_->payload);
    rowid_lookup_ = std::get_if<PhysicalRowIdLookupNode>(&leaf_->payload);
    rowid_range_ = std::get_if<PhysicalRowIdRangeNode>(&leaf_->payload);
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
        if (sort_ != nullptr || aggregate_ == nullptr || !sort->input_order_satisfied()) {
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
        rowid_lookup_ == nullptr && rowid_range_ == nullptr && index_scan_ == nullptr) {
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
    rowid_range_ = std::get_if<PhysicalRowIdRangeNode>(&leaf_->payload);
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
    if (sort_ != nullptr && !sort_->input_order_satisfied() &&
        sort_->strategy == PhysicalSortStrategy::kExternal &&
        (sort_->schedule != OrderEvaluationSchedule::kPayloadThenKeys || limit_ != nullptr)) {
      return std::unexpected(InternalFailure("grouped external ORDER BY plan shape is invalid"));
    }
    if (sort_ != nullptr && !sort_->input_order_satisfied() &&
        sort_->strategy == PhysicalSortStrategy::kRuntimeLimit &&
        (sort_->schedule != OrderEvaluationSchedule::kKeysThenAdmissionThenPayload ||
         limit_ == nullptr)) {
      return std::unexpected(
          InternalFailure("grouped runtime LIMIT ORDER BY plan shape is invalid"));
    }
    if (!std::holds_alternative<PhysicalEmptyNode>(leaf_->payload) &&
        !std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload) && table_scan_ == nullptr &&
        rowid_lookup_ == nullptr && rowid_range_ == nullptr && index_scan_ == nullptr) {
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
    rowid_range_ = std::get_if<PhysicalRowIdRangeNode>(&leaf_->payload);
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
        !std::holds_alternative<PhysicalRowIdRangeNode>(leaf_->payload) &&
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
    read_registers_->result_block_first = *result_block;
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
    read_registers_->global_representative_registers.reserve(global_representative_leaves_.size());
    for (std::size_t index = 0; index < global_representative_leaves_.size(); ++index) {
      read_registers_->global_representative_registers.emplace_back(
          representative->value() + static_cast<std::uint32_t>(index));
    }
    if (auto mapped = MapAggregateLeafRegisters(
            global_representative_leaves_, read_registers_->global_representative_registers,
            read_registers_->global_representative_register_map);
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
      read_registers_->global_representative_comparison_register = *comparison;
      read_registers_->global_representative_effect_register = *effect;
      read_registers_->global_representative_replace_register = *replacement;
      return {};
    }
    auto has_row = AllocateRegisters(1);
    if (!has_row.has_value()) {
      return std::unexpected(std::move(has_row.error()));
    }
    read_registers_->global_has_representative_row_register = *has_row;
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
    read_registers_->limit_register = *limit_register;
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
    read_registers_->offset_register = *offset_register;
    read_registers_->zero_register = *zero_register;
    read_registers_->one_register = *one_register;
    read_registers_->comparison_register = *comparison_register;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateIndexAccessRegisters(std::size_t key_count,
                                                                  bool table_lookup,
                                                                  bool two_pass_null_order) {
    if (key_count != 0U) {
      auto key = AllocateRegisters(key_count);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      read_registers_->index_key_first = *key;
      read_registers_->index_key_capacity = static_cast<std::uint32_t>(key_count);
    }
    if (two_pass_null_order) {
      auto pass = AllocateRegisters(1);
      if (!pass.has_value()) {
        return std::unexpected(std::move(pass.error()));
      }
      read_registers_->index_null_pass = *pass;
    }
    if (table_lookup && !read_registers_->index_rowid_register.has_value()) {
      auto rowid = AllocateRegisters(1);
      if (!rowid.has_value()) {
        return std::unexpected(std::move(rowid.error()));
      }
      read_registers_->index_rowid_register = *rowid;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateAccessRegisters(bool nullable_indexed_min_max) {
    if (index_scan_ == nullptr) {
      return {};
    }
    std::size_t key_count =
        index_scan_->equalities.size() + static_cast<std::size_t>(index_scan_->range.has_value() ||
                                                                  index_scan_->two_pass_null_order);
    if (nullable_indexed_min_max) {
      key_count = std::max(key_count, index_scan_->equalities.size() + 1U);
    }
    return AllocateIndexAccessRegisters(key_count, !index_scan_->covering,
                                        index_scan_->two_pass_null_order);
  }

  [[nodiscard]] LoweringResult<void> AllocateStreamingAccessRegisters(
      std::span<const StreamingCorePlan> cores) {
    std::size_t maximum_keys = 0;
    bool table_lookup = false;
    bool two_pass_null_order = false;
    for (const StreamingCorePlan& core : cores) {
      if (core.index_scan == nullptr) {
        continue;
      }
      std::size_t key_count = core.index_scan->equalities.size() +
                              static_cast<std::size_t>(core.index_scan->range.has_value() ||
                                                       core.index_scan->two_pass_null_order);
      if (core.aggregate != nullptr &&
          core.aggregate->strategy == PhysicalAggregateStrategy::kIndexedMinMax &&
          core.aggregate->min_max.has_value() &&
          AssumeValue(core.aggregate->min_max).null_strategy ==
              PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback) {
        key_count = std::max(key_count, core.index_scan->equalities.size() + 1U);
      }
      maximum_keys = std::max(maximum_keys, key_count);
      table_lookup = table_lookup || !core.index_scan->covering;
      two_pass_null_order = two_pass_null_order || core.index_scan->two_pass_null_order;
    }
    return AllocateIndexAccessRegisters(maximum_keys, table_lookup, two_pass_null_order);
  }

  [[nodiscard]] static bool RowIdRangeHasEnd(const PhysicalRowIdRangeNode& range) noexcept {
    return range.direction == PhysicalScanDirection::kForward ? range.upper.has_value()
                                                              : range.lower.has_value();
  }

  [[nodiscard]] bool HasRowIdRangeEndAccess(
      std::span<const StreamingCorePlan> cores) const noexcept {
    return (rowid_range_ != nullptr && RowIdRangeHasEnd(*rowid_range_)) ||
           std::ranges::any_of(cores, [](const StreamingCorePlan& core) {
             return (core.rowid_range != nullptr && RowIdRangeHasEnd(*core.rowid_range)) ||
                    std::ranges::any_of(core.joined_sources, [](const JoinedSourcePlan& source) {
                      const auto* range =
                          source.access != nullptr
                              ? std::get_if<PhysicalRowIdRangeNode>(&source.access->payload)
                              : nullptr;
                      return range != nullptr && RowIdRangeHasEnd(*range);
                    });
           });
  }

  [[nodiscard]] LoweringResult<void> AllocateRowIdRangeRegisters(
      std::span<const StreamingCorePlan> cores) {
    if (!HasRowIdRangeEndAccess(cores)) {
      return {};
    }
    auto rowid = AllocateRegisters(1);
    auto comparison = AllocateRegisters(1);
    if (!rowid.has_value()) {
      return std::unexpected(std::move(rowid.error()));
    }
    if (!comparison.has_value()) {
      return std::unexpected(std::move(comparison.error()));
    }
    read_registers_->rowid_range_rowid_register = *rowid;
    read_registers_->rowid_range_comparison_register = *comparison;
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
    if (auto access = AllocateAccessRegisters(nullable_indexed_min_max); !access.has_value()) {
      return access;
    }
    if (auto range = AllocateRowIdRangeRegisters(streaming_cores_); !range.has_value()) {
      return range;
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

    read_registers_->global_filtered_controller_defaults =
        global_aggregate_query_ && has_filtered_controller && !has_unfiltered_controller;
    if (read_registers_->global_filtered_controller_defaults &&
        !read_registers_->global_modifier_seen_row_register.has_value()) {
      auto seen = AllocateRegisters(1);
      if (!seen.has_value()) {
        return std::unexpected(std::move(seen.error()));
      }
      read_registers_->global_modifier_seen_row_register = *seen;
    }
    return {};
  }

  [[nodiscard]] bool HasOrdering() const noexcept {
    if (read_registers_->active_aggregate_output.has_value()) {
      return false;
    }
    if (active_core_order_ != nullptr) {
      return !active_core_order_->terms.empty() &&
             active_core_order_->ordered_prefix != active_core_order_->terms.size();
    }
    if (wrapper_order_layout_ != nullptr && read_registers_ == &pooled_read_registers_ &&
        (!read_registers_->row_destination.has_value() ||
         read_registers_->row_destination->mode != SelectOutputMode::Ordered)) {
      return true;
    }
    return sort_ != nullptr && !sort_->input_order_satisfied();
  }

  [[nodiscard]] bool HasLimit() const noexcept {
    return read_registers_->limit_register.has_value() &&
           (!read_registers_->row_destination.has_value() ||
            read_registers_->row_destination->mode != SelectOutputMode::Ordered);
  }

  [[nodiscard]] std::span<const BoundOrderingTerm> RequestedOrderTerms() const noexcept {
    if (active_core_order_ != nullptr) {
      return active_core_order_->terms;
    }
    if (sort_ != nullptr) {
      return sort_->terms;
    }
    return advanced_order_ != nullptr ? std::span<const BoundOrderingTerm>{advanced_order_->terms}
                                      : std::span<const BoundOrderingTerm>{};
  }

  [[nodiscard]] std::uint32_t OrderedPrefix() const noexcept {
    if (active_core_order_ != nullptr) {
      return active_core_order_->ordered_prefix;
    }
    return sort_ != nullptr ? sort_->ordered_prefix
                            : (advanced_order_ != nullptr ? advanced_order_->ordered_prefix : 0U);
  }

  [[nodiscard]] bool HasBlockOrdering() const noexcept {
    return OrderedPrefix() != 0U && OrderedPrefix() < RequestedOrderTerms().size();
  }

  [[nodiscard]] std::span<const SortOutputField> BlockOutputFields() const noexcept {
    if (active_core_order_ != nullptr) {
      return active_core_order_->layout.output_fields;
    }
    if (wrapper_order_layout_ != nullptr && read_registers_ == &pooled_read_registers_) {
      return wrapper_order_layout_->output_fields;
    }
    if (sort_ != nullptr) {
      return sort_->output_fields;
    }
    return read_plan_->core_order_plan(0).layout.output_fields;
  }

  [[nodiscard]] RegisterId BlockRecordFirst() const noexcept {
    return AssumeValue(read_registers_->sorter_record_first);
  }

  [[nodiscard]] SorterId BlockSorter() const noexcept {
    return AssumeValue(read_registers_->sorter);
  }

  [[nodiscard]] TopNId BlockTopN() const noexcept { return AssumeValue(read_registers_->top_n); }

  [[nodiscard]] LoweringResult<void> AllocateOrderBlockRegisters() {
    if (!HasBlockOrdering()) {
      return {};
    }
    auto previous = AllocateRegisters(OrderedPrefix());
    auto present = AllocateRegisters(1);
    auto comparison = AllocateRegisters(1);
    auto record = AllocateRegisters(1);
    if (!previous.has_value()) {
      return std::unexpected(std::move(previous.error()));
    }
    if (!present.has_value()) {
      return std::unexpected(std::move(present.error()));
    }
    if (!comparison.has_value()) {
      return std::unexpected(std::move(comparison.error()));
    }
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    read_registers_->order_prefix_previous_first = *previous;
    read_registers_->order_prefix_present = *present;
    read_registers_->order_prefix_comparison_register = *comparison;
    read_registers_->encoded_order_record = *record;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateGroupedOutputRegisters() {
    if (auto result = AllocateAggregateResultRegisters(); !result.has_value()) {
      return result;
    }
    if (!HasOrdering()) {
      return {};
    }
    const std::size_t field_count = sort_->terms.size() + sort_->payload_expressions.size();
    auto sorter_record = AllocateRegisters(field_count);
    if (!sorter_record.has_value()) {
      return std::unexpected(std::move(sorter_record.error()));
    }
    read_registers_->sorter_record_first = *sorter_record;
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
    read_registers_->limit_register = *limit_register;
    read_registers_->zero_register = *zero_register;
    read_registers_->one_register = *one_register;
    read_registers_->negative_limit_register = *negative_register;

    if (limit_->offset.has_value()) {
      auto offset_register = AllocateRegisters(1);
      auto comparison_register = AllocateRegisters(1);
      if (!offset_register.has_value()) {
        return std::unexpected(std::move(offset_register.error()));
      }
      if (!comparison_register.has_value()) {
        return std::unexpected(std::move(comparison_register.error()));
      }
      read_registers_->offset_register = *offset_register;
      read_registers_->comparison_register = *comparison_register;
    }

    if (!HasOrdering() || sort_->strategy != PhysicalSortStrategy::kRuntimeLimit) {
      return {};
    }
    auto top_n_bound = AllocateRegisters(1);
    if (!top_n_bound.has_value()) {
      return std::unexpected(std::move(top_n_bound.error()));
    }
    read_registers_->top_n_bound_register = *top_n_bound;
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
    read_registers_->maximum_integer_register = *maximum_integer;
    read_registers_->bound_room_register = *bound_room;
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
    read_registers_->grouped_current_group_first = *current_group;
    read_registers_->grouped_previous_group_first = *previous_group;
    read_registers_->grouped_has_group_register = *has_group;
    read_registers_->grouped_comparison_register = *group_comparison;
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
    read_registers_->grouped_representative_registers.reserve(
        aggregate_program_->representative_leaves.size());
    for (std::size_t index = 0; index < aggregate_program_->representative_leaves.size(); ++index) {
      read_registers_->grouped_representative_registers.emplace_back(
          representative->value() + static_cast<std::uint32_t>(index));
    }
    if (auto mapped =
            MapAggregateLeafRegisters(aggregate_program_->representative_leaves,
                                      read_registers_->grouped_representative_registers,
                                      read_registers_->grouped_representative_register_map);
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
    read_registers_->grouped_representative_effect_register = *effect;
    read_registers_->grouped_representative_replace_register = *replacement;
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
    read_registers_->grouped_retained_leaf_registers.reserve(grouped_retained_leaves_.size());
    for (std::size_t index = 0; index < grouped_retained_leaves_.size(); ++index) {
      read_registers_->grouped_retained_leaf_registers.emplace_back(
          retained_registers->value() + static_cast<std::uint32_t>(index));
    }
    read_registers_->grouped_sorter_record_first = *sorter_record;
    return MapAggregateLeafRegisters(grouped_retained_leaves_,
                                     read_registers_->grouped_retained_leaf_registers,
                                     read_registers_->grouped_sorted_register_map);
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
          RegisterId{AssumeValue(read_registers_->grouped_previous_group_first).value() +
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
    if (auto access = AllocateAccessRegisters(false); !access.has_value()) {
      return access;
    }
    if (auto range = AllocateRowIdRangeRegisters(streaming_cores_); !range.has_value()) {
      return range;
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
    read_registers_->result_block_first = *result_block;

    if (HasOrdering()) {
      const std::size_t field_count = sort_->terms.size() + sort_->payload_expressions.size();
      auto sorter_record = AllocateRegisters(field_count);
      if (!sorter_record.has_value()) {
        return std::unexpected(std::move(sorter_record.error()));
      }
      read_registers_->sorter_record_first = *sorter_record;
    }
    if (auto block = AllocateOrderBlockRegisters(); !block.has_value()) {
      return block;
    }

    if (limit_ != nullptr) {
      auto limit_register = AllocateRegisters(1);
      if (!limit_register.has_value()) {
        return std::unexpected(std::move(limit_register.error()));
      }
      read_registers_->limit_register = *limit_register;

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
        read_registers_->offset_register = *offset_register;
        read_registers_->zero_register = *zero_register;
        read_registers_->one_register = *one_register;
        read_registers_->comparison_register = *comparison_register;
      }

      if (IsRowLoopAccess() || sort_ != nullptr) {
        if (!read_registers_->zero_register.has_value()) {
          auto zero_register = AllocateRegisters(1);
          if (!zero_register.has_value()) {
            return std::unexpected(std::move(zero_register.error()));
          }
          read_registers_->zero_register = *zero_register;
        }
        if (!read_registers_->one_register.has_value()) {
          auto one_register = AllocateRegisters(1);
          if (!one_register.has_value()) {
            return std::unexpected(std::move(one_register.error()));
          }
          read_registers_->one_register = *one_register;
        }
        auto negative_register = AllocateRegisters(1);
        if (!negative_register.has_value()) {
          return std::unexpected(std::move(negative_register.error()));
        }
        read_registers_->negative_limit_register = *negative_register;
      }

      if (HasOrdering() && sort_->strategy == PhysicalSortStrategy::kRuntimeLimit) {
        auto top_n_bound = AllocateRegisters(1);
        if (!top_n_bound.has_value()) {
          return std::unexpected(std::move(top_n_bound.error()));
        }
        read_registers_->top_n_bound_register = *top_n_bound;
        if (limit_->offset.has_value()) {
          auto maximum_integer = AllocateRegisters(1);
          auto bound_room = AllocateRegisters(1);
          if (!maximum_integer.has_value()) {
            return std::unexpected(std::move(maximum_integer.error()));
          }
          if (!bound_room.has_value()) {
            return std::unexpected(std::move(bound_room.error()));
          }
          read_registers_->maximum_integer_register = *maximum_integer;
          read_registers_->bound_room_register = *bound_room;
        }
      }
    }

    if (auto access = AllocateAccessRegisters(false); !access.has_value()) {
      return access;
    }
    if (auto range = AllocateRowIdRangeRegisters(streaming_cores_); !range.has_value()) {
      return range;
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
    read_registers_->result_block_first = *result_block;

    if (advanced_order_ != nullptr) {
      const LogicalCoreOrderLayout& layout = read_plan_->core_order_plans()[0].layout;
      const std::size_t field_count = layout.key_values.size() + layout.payload_values.size();
      auto sorter_record = AllocateRegisters(field_count);
      if (!sorter_record.has_value()) {
        return std::unexpected(std::move(sorter_record.error()));
      }
      read_registers_->sorter_record_first = *sorter_record;
    }

    if (limit_ != nullptr) {
      auto limit_register = AllocateRegisters(1);
      if (!limit_register.has_value()) {
        return std::unexpected(std::move(limit_register.error()));
      }
      read_registers_->limit_register = *limit_register;

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
        read_registers_->offset_register = *offset_register;
        read_registers_->zero_register = *zero_register;
        read_registers_->one_register = *one_register;
        read_registers_->comparison_register = *comparison_register;
      }

      if (IsCursorAccess() || advanced_order_ != nullptr) {
        if (!read_registers_->zero_register.has_value()) {
          auto zero_register = AllocateRegisters(1);
          if (!zero_register.has_value()) {
            return std::unexpected(std::move(zero_register.error()));
          }
          read_registers_->zero_register = *zero_register;
        }
        if (!read_registers_->one_register.has_value()) {
          auto one_register = AllocateRegisters(1);
          if (!one_register.has_value()) {
            return std::unexpected(std::move(one_register.error()));
          }
          read_registers_->one_register = *one_register;
        }
        auto negative_register = AllocateRegisters(1);
        if (!negative_register.has_value()) {
          return std::unexpected(std::move(negative_register.error()));
        }
        read_registers_->negative_limit_register = *negative_register;
      }
    }

    if (auto access = AllocateAccessRegisters(false); !access.has_value()) {
      return access;
    }
    if (auto range = AllocateRowIdRangeRegisters(streaming_cores_); !range.has_value()) {
      return range;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> AllocateStreamingCoreRegisters(
      std::span<StreamingCorePlan> cores) {
    for (StreamingCorePlan& core : cores) {
      for (JoinedSourcePlan& source : core.joined_sources) {
        if (source.access == nullptr || source.declaration == nullptr) {
          return std::unexpected(InternalFailure("joined source has no register-layout access"));
        }
        if (const auto* scan = std::get_if<PhysicalIndexScanNode>(&source.access->payload);
            scan != nullptr) {
          const std::size_t capacity =
              scan->equalities.size() +
              static_cast<std::size_t>(scan->range.has_value() || scan->two_pass_null_order);
          if (capacity != 0U) {
            auto keys = AllocateRegisters(capacity);
            if (!keys.has_value()) {
              return std::unexpected(std::move(keys.error()));
            }
            source.access_key_first = *keys;
            source.access_key_capacity = static_cast<std::uint32_t>(capacity);
          }
          if (scan->two_pass_null_order) {
            auto pass = AllocateRegisters(1);
            if (!pass.has_value()) {
              return std::unexpected(std::move(pass.error()));
            }
            source.index_null_pass = *pass;
          }
          if (!scan->covering && !read_registers_->index_rowid_register.has_value()) {
            auto rowid = AllocateRegisters(1);
            if (!rowid.has_value()) {
              return std::unexpected(std::move(rowid.error()));
            }
            read_registers_->index_rowid_register = *rowid;
          }
        } else if (const auto* range = std::get_if<PhysicalRowIdRangeNode>(&source.access->payload);
                   range != nullptr && RowIdRangeHasEnd(*range)) {
          auto key = AllocateRegisters(1);
          if (!key.has_value()) {
            return std::unexpected(std::move(key.error()));
          }
          source.access_key_first = *key;
          source.access_key_capacity = 1U;
        } else if (const auto* automatic =
                       std::get_if<PhysicalAutomaticIndexNode>(&source.access->payload);
                   automatic != nullptr) {
          if (automatic->equalities.empty() ||
              automatic->direction != PhysicalScanDirection::kForward) {
            return std::unexpected(InternalFailure("automatic index prefix metadata is invalid"));
          }
          auto keys = AllocateRegisters(automatic->equalities.size());
          auto record = AllocateRegisters(automatic->equalities.size() +
                                          automatic->covering_columns.size() + 1U);
          if (!keys.has_value()) {
            return std::unexpected(std::move(keys.error()));
          }
          if (!record.has_value()) {
            return std::unexpected(std::move(record.error()));
          }
          source.access_key_first = *keys;
          source.access_key_capacity = static_cast<std::uint32_t>(automatic->equalities.size());
          source.construction_record_first = *record;
        }
        if (source.join != nullptr && SourceJoinPreservesLeft(source.join->join)) {
          auto matched = AllocateRegisters(1);
          if (!matched.has_value()) {
            return std::unexpected(std::move(matched.error()));
          }
          source.matched = *matched;
        }
        if (source.join != nullptr && SourceJoinPreservesRight(source.join->join)) {
          const BoundTableSource& bound_source = *source.declaration;
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
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateStreamingLimitRegisters(bool offset) {
    auto limit = AllocateRegisters(1);
    auto zero = AllocateRegisters(1);
    auto one = AllocateRegisters(1);
    auto negative = AllocateRegisters(1);
    if (!limit.has_value()) {
      return std::unexpected(std::move(limit.error()));
    }
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    if (!one.has_value()) {
      return std::unexpected(std::move(one.error()));
    }
    if (!negative.has_value()) {
      return std::unexpected(std::move(negative.error()));
    }
    read_registers_->limit_register = *limit;
    read_registers_->zero_register = *zero;
    read_registers_->one_register = *one;
    read_registers_->negative_limit_register = *negative;
    if (offset) {
      auto count = AllocateRegisters(1);
      auto comparison = AllocateRegisters(1);
      if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
      }
      if (!comparison.has_value()) {
        return std::unexpected(std::move(comparison.error()));
      }
      read_registers_->offset_register = *count;
      read_registers_->comparison_register = *comparison;
    }
    return {};
  }

  [[nodiscard]] ReadRegisterLayout& NativeRegisters(std::size_t producer) noexcept {
    auto& registers = native_producers_[producer].registers;
    return registers.has_value() ? *registers : pooled_read_registers_;
  }

  [[nodiscard]] LoweringResult<std::size_t> NativeBoundaryCore(PhysicalNodeId node,
                                                               bool last) const {
    while (const auto* compound =
               std::get_if<PhysicalCompoundNode>(&read_plan_->node(node).payload)) {
      node = last ? compound->right : compound->left;
    }
    const auto& payload = read_plan_->node(node).payload;
    if (const auto* projection = std::get_if<PhysicalProjectionNode>(&payload)) {
      return projection->core_index;
    }
    if (const auto* distinct = std::get_if<PhysicalDistinctNode>(&payload)) {
      return distinct->core_index;
    }
    if (const auto* values = std::get_if<PhysicalValuesNode>(&payload)) {
      return values->core_index;
    }
    return std::unexpected(InternalFailure("native compound leaf has no query-core identity"));
  }

  [[nodiscard]] LoweringResult<void> PrepareNativeProducerScopes() {
    if (!native_compound_root_.has_value() || !native_producers_.empty()) {
      return std::unexpected(InternalFailure("native producer scopes have invalid preparation"));
    }
    native_producers_.reserve(streaming_cores_.size() * 2U - 1U);
    native_producers_.push_back(NativeProducer{.root = *native_compound_root_});
    const bool wrapper = advanced_order_ != nullptr && advanced_order_->set_then_order;
    const bool one = !wrapper && BoundQueryHasSingleRowDemand(bound_select_->query(), expressions_);
    for (std::size_t index = 0; index < native_producers_.size(); ++index) {
      const PhysicalNodeId root = native_producers_[index].root;
      auto last = NativeBoundaryCore(root, true);
      if (!last.has_value()) {
        return std::unexpected(std::move(last.error()));
      }
      PhysicalNodeId node = root;
      const PhysicalCompoundNode* merge = nullptr;
      while (const auto* compound =
                 std::get_if<PhysicalCompoundNode>(&read_plan_->node(node).payload)) {
        if (compound->strategy == PhysicalCompoundStrategy::kOrderedMerge) {
          merge = compound;
          break;
        }
        if (compound->strategy != PhysicalCompoundStrategy::kConcatenate &&
            compound->strategy != PhysicalCompoundStrategy::kUnionLimitOne) {
          return std::unexpected(InternalFailure("native producer contains an eager compound"));
        }
        if (std::holds_alternative<PhysicalCompoundNode>(
                read_plan_->node(compound->right).payload)) {
          return std::unexpected(InternalFailure("native concatenation has a compound right arm"));
        }
        node = compound->left;
      }
      auto first = NativeBoundaryCore(root, false);
      if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
      }
      if (merge != nullptr) {
        auto merge_last = NativeBoundaryCore(node, true);
        if (!merge_last.has_value()) {
          return std::unexpected(std::move(merge_last.error()));
        }
        native_producers_[index].merge = node;
        first = *merge_last + 1U;
        const bool bounded =
            index == 0U ? !wrapper && limit_ != nullptr : native_producers_[index].has_input_limit;
        const bool share =
            bounded && (merge->operation == CompoundOperator::kUnionAll ||
                        (merge->operation == CompoundOperator::kUnion && index == 0U && one));
        const std::size_t left = native_producers_.size();
        NativeProducer left_producer{
            .root = merge->left, .parent = index, .has_input_limit = share};
        NativeProducer right_producer{
            .root = merge->right, .parent = index, .has_input_limit = share};
        left_producer.registers.emplace();
        right_producer.registers.emplace();
        native_producers_.push_back(std::move(left_producer));
        native_producers_.push_back(std::move(right_producer));
        native_producers_[index].left = left;
        native_producers_[index].right = left + 1U;
      }
      native_producers_[index].source_begin = *first;
      native_producers_[index].source_count = *first <= *last ? *last - *first + 1U : 0U;
    }
    for (std::size_t remaining = native_producers_.size(); remaining != 0U; --remaining) {
      NativeProducer& producer = native_producers_[remaining - 1U];
      bool rows = false;
      for (std::size_t core = producer.source_begin;
           core < producer.source_begin + producer.source_count; ++core) {
        rows = rows || streaming_cores_[core].empty == nullptr ||
               streaming_cores_[core].aggregate != nullptr;
      }
      if (producer.merge.has_value()) {
        const auto operation =
            std::get<PhysicalCompoundNode>(read_plan_->node(*producer.merge).payload).operation;
        const bool left = native_producers_[*producer.left].emits_rows;
        const bool right = native_producers_[*producer.right].emits_rows;
        rows = rows || (operation == CompoundOperator::kIntersect ? left && right
                        : operation == CompoundOperator::kExcept  ? left
                                                                  : left || right);
      }
      producer.emits_rows = rows;
    }
    for (const auto& producer : native_producers_) {
      if (producer.merge.has_value()) {
        const auto operation =
            std::get<PhysicalCompoundNode>(read_plan_->node(*producer.merge).payload).operation;
        native_producers_[*producer.left].required = producer.required;
        native_producers_[*producer.right].required =
            producer.required &&
            (native_producers_[*producer.left].emits_rows ||
             operation == CompoundOperator::kUnionAll || operation == CompoundOperator::kUnion);
      }
    }
    for (std::size_t index = 0; index < native_producers_.size(); ++index) {
      const auto& producer = native_producers_[index];
      if (!producer.required) {
        continue;
      }
      for (std::size_t core = producer.source_begin;
           core < producer.source_begin + producer.source_count; ++core) {
        if (streaming_cores_[core].native_scope.has_value()) {
          return std::unexpected(InternalFailure("native source core has more than one workspace"));
        }
        streaming_cores_[core].native_scope = index;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> BuildNativeRegisterLayout() {
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }
    if (auto scopes = PrepareNativeProducerScopes(); !scopes.has_value()) {
      return scopes;
    }
    if (streaming_aggregate_query_) {
      if (auto invocations = AllocateQueryAggregateInvocationRegisters();
          !invocations.has_value()) {
        return invocations;
      }
    }
    if (advanced_order_ != nullptr && advanced_order_->set_then_order) {
      for (const auto& node : logical_plan_->nodes()) {
        if (const auto* order = std::get_if<LogicalAdvancedOrderNode>(&node.payload)) {
          if (order->core_layouts.empty()) {
            return std::unexpected(InternalFailure("native outer ordering has no row layout"));
          }
          wrapper_order_layout_ = &order->core_layouts.front();
          break;
        }
      }
      if (wrapper_order_layout_ == nullptr) {
        return std::unexpected(InternalFailure("native outer ordering lost its logical layout"));
      }
    }
    ReadRegisterLayout* previous = read_registers_;
    const PhysicalCoreOrderPlan* previous_order = active_core_order_;
    for (std::size_t index = 0; index < native_producers_.size(); ++index) {
      if (!native_producers_[index].required) {
        continue;
      }
      read_registers_ = &NativeRegisters(index);
      active_core_order_ = nullptr;
      ClearStreamingAggregateContext();
      const auto allocate = [&]() -> LoweringResult<void> {
        const auto assign = [&](std::optional<RegisterId>& slot,
                                std::size_t count = 1U) -> LoweringResult<void> {
          auto registers = AllocateRegisters(count);
          if (!registers.has_value()) {
            return std::unexpected(std::move(registers.error()));
          }
          slot = *registers;
          return {};
        };
        auto result = AllocateRegisters(bound_select_->result_columns().size());
        if (!result.has_value()) {
          return std::unexpected(std::move(result.error()));
        }
        read_registers_->result_block_first = *result;
        const auto& producer = native_producers_[index];
        const auto cores =
            std::span{streaming_cores_}.subspan(producer.source_begin, producer.source_count);
        if (auto sources = AllocateStreamingCoreRegisters(cores); !sources.has_value()) {
          return sources;
        }
        const bool bounded = index == 0U ? limit_ != nullptr : producer.has_input_limit;
        if (bounded) {
          if (auto counters =
                  AllocateStreamingLimitRegisters(index == 0U && limit_->offset.has_value());
              !counters.has_value()) {
            return counters;
          }
          if (index != 0U) {
            const auto& parent = NativeRegisters(*producer.parent);
            if (!parent.top_n_bound_register.has_value()) {
              return std::unexpected(InternalFailure("native child has no parent row bound"));
            }
            read_registers_->limit_input = parent.top_n_bound_register;
          }
        }
        if (auto access = AllocateStreamingAccessRegisters(cores); !access.has_value()) {
          return access;
        }
        if (auto range = AllocateRowIdRangeRegisters(cores); !range.has_value()) {
          return range;
        }
        if (auto aggregates = AllocateStreamingAggregateWorkspace(cores); !aggregates.has_value()) {
          return aggregates;
        }
        if (producer.merge.has_value()) {
          const auto& merge =
              std::get<PhysicalCompoundNode>(read_plan_->node(*producer.merge).payload);
          for (auto* row :
               {&read_registers_->ordered_left_first, &read_registers_->ordered_right_first}) {
            if (auto assigned = assign(*row, bound_select_->result_columns().size());
                !assigned.has_value()) {
              return assigned;
            }
          }
          for (auto* control : {&read_registers_->ordered_comparison_register,
                                &read_registers_->ordered_predicate_register}) {
            if (auto assigned = assign(*control); !assigned.has_value()) {
              return assigned;
            }
          }
          if (auto constants = AllocateReadConstantRegisters(); !constants.has_value()) {
            return constants;
          }
          if (merge.operation != CompoundOperator::kUnionAll) {
            if (auto row = assign(read_registers_->compound_previous_first,
                                  bound_select_->result_columns().size());
                !row.has_value()) {
              return row;
            }
            if (auto flag = assign(read_registers_->compound_has_previous); !flag.has_value()) {
              return flag;
            }
          }
        } else if (producer.source_count == 1U) {
          active_core_order_ = &read_plan_->core_order_plan(producer.source_begin);
        }
        const bool ordered = active_core_order_ != nullptr && HasOrdering();
        const bool outer_order = index == 0U && wrapper_order_layout_ != nullptr;
        if (ordered || outer_order) {
          const auto& layout = ordered ? active_core_order_->layout : *wrapper_order_layout_;
          if (auto record = assign(read_registers_->sorter_record_first,
                                   layout.key_values.size() + layout.payload_values.size());
              !record.has_value()) {
            return record;
          }
          if (auto block = AllocateOrderBlockRegisters(); !block.has_value()) {
            return block;
          }
        }
        const bool sort_bound =
            (ordered && active_core_order_->strategy == PhysicalSortStrategy::kRuntimeLimit) ||
            (outer_order && advanced_order_->final_strategy == PhysicalSortStrategy::kRuntimeLimit);
        const bool shared_bound =
            producer.left.has_value() && native_producers_[*producer.left].has_input_limit;
        if (sort_bound || shared_bound) {
          if (auto bound = assign(read_registers_->top_n_bound_register); !bound.has_value()) {
            return bound;
          }
          if (read_registers_->offset_register.has_value()) {
            for (auto* scratch : {&read_registers_->maximum_integer_register,
                                  &read_registers_->bound_room_register}) {
              if (auto reg = assign(*scratch); !reg.has_value()) {
                return reg;
              }
            }
          }
        }
        return {};
      }();
      read_registers_ = previous;
      active_core_order_ = previous_order;
      ClearStreamingAggregateContext();
      if (!allocate.has_value()) {
        return allocate;
      }
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> BuildStreamingRegisterLayout() {
    if (!streaming_query_ || streaming_cores_.empty()) {
      return std::unexpected(InternalFailure("streaming register layout has no cores"));
    }
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
    }
    auto result_block = AllocateRegisters(bound_select_->result_columns().size());
    if (!result_block.has_value()) {
      return std::unexpected(std::move(result_block.error()));
    }
    read_registers_->result_block_first = *result_block;
    if (auto sources = AllocateStreamingCoreRegisters(streaming_cores_); !sources.has_value()) {
      return sources;
    }

    if (limit_ != nullptr) {
      if (auto counters = AllocateStreamingLimitRegisters(limit_->offset.has_value());
          !counters.has_value()) {
        return counters;
      }
    }
    if (auto access = AllocateStreamingAccessRegisters(streaming_cores_); !access.has_value()) {
      return access;
    }
    if (auto range = AllocateRowIdRangeRegisters(streaming_cores_); !range.has_value()) {
      return range;
    }
    return FinishRegisterLayout();
  }

  [[nodiscard]] LoweringResult<void> AllocateReadConstantRegisters() {
    for (auto* slot : {&read_registers_->zero_register, &read_registers_->one_register}) {
      if (slot->has_value()) {
        continue;
      }
      auto allocated = AllocateRegisters(1);
      if (!allocated.has_value()) {
        return std::unexpected(std::move(allocated.error()));
      }
      *slot = *allocated;
    }
    return {};
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
      core->global_representative_leaves = select->aggregate->representative_leaves;
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
    core->global_filtered_controller_defaults =
        read_registers_->global_filtered_controller_defaults;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AllocateStreamingAggregateWorkspace(
      std::span<StreamingCorePlan> cores) {
    if (!streaming_aggregate_query_) {
      return {};
    }
    std::size_t maximum_global_representatives = 0;
    std::size_t maximum_group_terms = 0;
    std::size_t maximum_grouped_representatives = 0;
    std::size_t maximum_grouped_retained = 0;
    std::size_t maximum_grouped_sorter_fields = 0;
    bool has_global_representatives = false;
    bool has_global_controller = false;
    bool has_grouped_controller = false;

    for (StreamingCorePlan& core : cores) {
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
    }

    if (auto allocated =
            AllocateRegisterPool(maximum_global_representatives,
                                 &read_registers_->streaming_global_representative_registers);
        !allocated.has_value()) {
      return allocated;
    }
    if (has_global_representatives) {
      auto has_row = AllocateRegisters(1);
      if (!has_row.has_value()) {
        return std::unexpected(std::move(has_row.error()));
      }
      read_registers_->global_has_representative_row_register = *has_row;
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
      read_registers_->global_representative_comparison_register = *comparison;
      read_registers_->global_representative_effect_register = *effect;
      read_registers_->global_representative_replace_register = *replacement;
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
      read_registers_->grouped_current_group_first = *current;
      read_registers_->grouped_previous_group_first = *previous;
      read_registers_->grouped_has_group_register = *has_group;
      read_registers_->grouped_comparison_register = *comparison;
    }
    if (auto allocated =
            AllocateRegisterPool(maximum_grouped_representatives,
                                 &read_registers_->streaming_grouped_representative_registers);
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
      read_registers_->grouped_representative_effect_register = *effect;
      read_registers_->grouped_representative_replace_register = *replacement;
    }
    if (auto allocated = AllocateRegisterPool(
            maximum_grouped_retained, &read_registers_->streaming_grouped_retained_registers);
        !allocated.has_value()) {
      return allocated;
    }
    if (maximum_grouped_sorter_fields != 0U) {
      auto record = AllocateRegisters(maximum_grouped_sorter_fields);
      if (!record.has_value()) {
        return std::unexpected(std::move(record.error()));
      }
      read_registers_->grouped_sorter_record_first = *record;
    }
    ClearStreamingAggregateContext();
    return {};
  }

  [[nodiscard]] LoweringResult<void> BuildStreamingAggregateRegisterLayout() {
    LoweringResult<void> base = BuildStreamingRegisterLayout();
    if (!base.has_value()) {
      return base;
    }
    if (auto invocations = AllocateQueryAggregateInvocationRegisters(); !invocations.has_value()) {
      return invocations;
    }
    if (auto aggregate = AllocateStreamingAggregateWorkspace(streaming_cores_);
        !aggregate.has_value()) {
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
      read_registers_->grouped_sorter = *sorter;
    }
    if (HasOrdering()) {
      return AddOrderingDescriptors();
    }
    return {};
  }

  static void AppendOrderingColumn(std::vector<OrderingColumnMetadata>& columns,
                                   OrderingColumnMetadata column) {
    columns.push_back(column);
  }

  static void AppendOrderingColumn(std::vector<RecordComparisonColumnMetadata>& columns,
                                   OrderingColumnMetadata column) {
    columns.push_back(RecordComparisonColumnMetadata{
        .field_index = static_cast<std::uint32_t>(columns.size()), .ordering = column});
  }

  [[nodiscard]] LoweringResult<OrderingColumnMetadata> OrderingColumnFor(
      BoundCollationId collation, SortOrder order, BoundNullPlacement nulls) const {
    if (collation.value() >= collation_symbols_.size()) {
      return std::unexpected(InternalFailure("ordering collation is not published"));
    }
    return OrderingColumnMetadata{
        .collation = collation_symbols_[collation.value()],
        .order = order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                 : BytecodeSortOrder::kAscending,
        .null_placement = nulls == BoundNullPlacement::kLast ? BytecodeNullPlacement::kLast
                                                             : BytecodeNullPlacement::kFirst,
    };
  }

  template <typename Column>
  [[nodiscard]] LoweringResult<void> AppendOrderKeyColumns(std::span<const BoundOrderingTerm> terms,
                                                           std::vector<Column>& columns) {
    for (const BoundOrderingTerm& term : terms) {
      auto column = OrderingColumnFor(term.collation, term.order, term.null_placement);
      if (!column.has_value()) {
        return std::unexpected(std::move(column.error()));
      }
      AppendOrderingColumn(columns, *column);
    }
    return {};
  }

  [[nodiscard]] LoweringResult<OrderingRecordDescriptor> BuildOrderKeyDescriptor(
      std::span<const BoundOrderingTerm> terms, std::size_t payload_count) {
    const std::size_t field_count = terms.size() + payload_count;
    if (field_count > std::numeric_limits<std::uint32_t>::max() ||
        terms.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded},
                         "sorter descriptor exceeds bytecode field identities"));
    }
    OrderingRecordDescriptor descriptor{
        .field_count = static_cast<std::uint32_t>(field_count),
        .key_field_count = static_cast<std::uint32_t>(terms.size()),
        .key_columns = {},
    };
    descriptor.key_columns.reserve(terms.size());
    if (auto appended = AppendOrderKeyColumns(terms, descriptor.key_columns);
        !appended.has_value()) {
      return std::unexpected(std::move(appended.error()));
    }
    return descriptor;
  }

  [[nodiscard]] LoweringResult<OrderingRecordDescriptor> BuildOrderingDescriptor() {
    if (active_core_order_ != nullptr) {
      return BuildOrderKeyDescriptor(
          std::span<const BoundOrderingTerm>{active_core_order_->terms}.subspan(
              active_core_order_->ordered_prefix),
          active_core_order_->layout.payload_values.size());
    }
    if (wrapper_order_layout_ != nullptr && read_registers_ == &pooled_read_registers_) {
      return BuildOrderKeyDescriptor(advanced_order_->terms,
                                     wrapper_order_layout_->payload_values.size());
    }
    if (sort_ == nullptr) {
      return std::unexpected(InternalFailure("ordering descriptor has no physical sort"));
    }
    return BuildOrderKeyDescriptor(
        std::span<const BoundOrderingTerm>{sort_->terms}.subspan(sort_->ordered_prefix),
        sort_->payload_expressions.size());
  }

  [[nodiscard]] LoweringResult<void> AddOrderBlockComparison() {
    if (!HasBlockOrdering()) {
      return {};
    }
    const auto terms = RequestedOrderTerms().first(OrderedPrefix());
    if (terms.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kOwnedBytesLimitExceeded},
                         "ordered-prefix comparison exceeds bytecode field identities"));
    }
    RecordComparisonDescriptor descriptor{
        .field_count = static_cast<std::uint32_t>(terms.size()),
        .key_columns = {},
    };
    descriptor.key_columns.reserve(terms.size());
    if (auto appended = AppendOrderKeyColumns(terms, descriptor.key_columns);
        !appended.has_value()) {
      return appended;
    }
    auto added =
        ConvertProgramResult(AssumeValue(builder_).AddRecordComparison(std::move(descriptor)),
                             "unable to add ordered-prefix comparison");
    if (!added.has_value()) {
      return std::unexpected(std::move(added.error()));
    }
    read_registers_->order_prefix_comparison = *added;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddOrderingDescriptors() {
    if (!HasOrdering()) {
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
    read_registers_->sorter = *added_sorter;
    const auto strategy = active_core_order_ != nullptr ? active_core_order_->strategy
                          : sort_ != nullptr            ? sort_->strategy
                                                        : advanced_order_->final_strategy;
    if (strategy == PhysicalSortStrategy::kRuntimeLimit) {
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
      read_registers_->top_n = *added_top_n;
    }
    return AddOrderBlockComparison();
  }

  [[nodiscard]] LoweringResult<void> AddNativeReadDescriptors() {
    if (streaming_aggregate_query_) {
      if (auto aggregates = AddStreamingAggregateDescriptors(); !aggregates.has_value()) {
        return aggregates;
      }
    }
    if (auto sources = AddStreamingDescriptors(); !sources.has_value()) {
      return sources;
    }
    ReadRegisterLayout* previous = read_registers_;
    const PhysicalCoreOrderPlan* previous_order = active_core_order_;
    for (std::size_t index = 0; index < native_producers_.size(); ++index) {
      if (!native_producers_[index].required) {
        continue;
      }
      read_registers_ = &NativeRegisters(index);
      active_core_order_ = nullptr;
      const auto add = [&]() -> LoweringResult<void> {
        NativeProducer& producer = native_producers_[index];
        if (!producer.merge.has_value() && producer.source_count == 1U) {
          active_core_order_ = &read_plan_->core_order_plan(producer.source_begin);
        }
        if (active_core_order_ != nullptr || (index == 0U && wrapper_order_layout_ != nullptr)) {
          if (auto ordering = AddOrderingDescriptors(); !ordering.has_value()) {
            return ordering;
          }
        }
        if (producer.merge.has_value()) {
          const auto& merge =
              std::get<PhysicalCompoundNode>(read_plan_->node(*producer.merge).payload);
          const auto comparison = [&](bool duplicate) -> LoweringResult<RecordComparisonId> {
            RecordComparisonDescriptor descriptor{
                .field_count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
                .key_columns = {},
            };
            const std::size_t count =
                duplicate ? bound_select_->result_columns().size() : merge.order_by.size();
            if (count == 0U || (duplicate && merge.collations.size() != count)) {
              return std::unexpected(InternalFailure("native merge comparison has no keys"));
            }
            descriptor.key_columns.reserve(count);
            for (std::size_t key = 0; key < count; ++key) {
              std::size_t field = key;
              BoundCollationId collation{0};
              SortOrder order = SortOrder::kAscending;
              BoundNullPlacement nulls = BoundNullPlacement::kFirst;
              if (duplicate) {
                collation = merge.collations[key];
              } else {
                const BoundOrderingTerm& term = merge.order_by[key];
                if (!term.result_column.has_value()) {
                  return std::unexpected(InternalFailure("native merge key has no result field"));
                }
                field = *term.result_column;
                collation = term.collation;
                order = term.order;
                nulls = term.null_placement;
              }
              if (field >= bound_select_->result_columns().size()) {
                return std::unexpected(InternalFailure("native merge result field is invalid"));
              }
              auto metadata = OrderingColumnFor(collation, order, nulls);
              if (!metadata.has_value()) {
                return std::unexpected(std::move(metadata.error()));
              }
              descriptor.key_columns.push_back(RecordComparisonColumnMetadata{
                  .field_index = static_cast<std::uint32_t>(field), .ordering = *metadata});
            }
            return ConvertProgramResult(
                AssumeValue(builder_).AddRecordComparison(std::move(descriptor)),
                "unable to add native merge comparison");
          };
          auto merge_comparison = comparison(false);
          if (!merge_comparison.has_value()) {
            return std::unexpected(std::move(merge_comparison.error()));
          }
          read_registers_->ordered_comparison = *merge_comparison;
          if (merge.operation != CompoundOperator::kUnionAll) {
            auto duplicate = comparison(true);
            if (!duplicate.has_value()) {
              return std::unexpected(std::move(duplicate.error()));
            }
            read_registers_->ordered_full_comparison = *duplicate;
          }
        }
        if (index == 0U && !producer.merge.has_value() && producer.source_count == 1U) {
          auto last = CreateLabel();
          if (!last.has_value()) {
            return std::unexpected(std::move(last.error()));
          }
          read_registers_->row_destination = SelectOutputContext{.final_completion = *last};
        } else if (index != 0U) {
          auto entry = CreateLabel();
          auto last = CreateLabel();
          if (!entry.has_value()) {
            return std::unexpected(std::move(entry.error()));
          }
          if (!last.has_value()) {
            return std::unexpected(std::move(last.error()));
          }
          auto coroutine = ConvertProgramResult(
              AssumeValue(builder_).AddCoroutine(
                  *entry, *last, read_registers_->result_block_first,
                  static_cast<std::uint32_t>(bound_select_->result_columns().size()),
                  CoroutineKind::Read),
              "unable to add native producer coroutine");
          if (!coroutine.has_value()) {
            return std::unexpected(std::move(coroutine.error()));
          }
          producer.entry = *entry;
          read_registers_->row_destination =
              SelectOutputContext{.final_completion = *last, .coroutine = *coroutine};
        }
        return {};
      }();
      read_registers_ = previous;
      active_core_order_ = previous_order;
      if (!add.has_value()) {
        return add;
      }
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
    read_registers_->distinct_relation = *relation;

    if (advanced_order_ == nullptr) {
      return {};
    }
    const LogicalCoreOrderLayout& layout = read_plan_->core_order_plans()[0].layout;
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
    read_registers_->sorter = *sorter;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddStreamingCursorDescriptor(StreamingCorePlan& core,
                                                                  const BoundSelectCore& select) {
    const std::optional<BoundSourceId> source =
        core.table_scan != nullptr     ? std::optional{core.table_scan->source}
        : core.rowid_lookup != nullptr ? std::optional{core.rowid_lookup->source}
        : core.rowid_range != nullptr  ? std::optional{core.rowid_range->source}
        : core.index_scan != nullptr   ? std::optional{core.index_scan->source}
                                       : std::nullopt;
    if (!source.has_value() || select.table_sources.size() != 1U ||
        *source != select.table_sources.front().id) {
      return std::unexpected(InternalFailure("streaming cursor metadata is invalid"));
    }
    SetStreamingCoreContext(core);
    if (auto added = AddCursorDescriptor(); !added.has_value()) {
      return added;
    }
    core.cursor = read_registers_->cursor;
    core.index_cursor = read_registers_->index_cursor;
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
          for (JoinedSourcePlan& joined : core.joined_sources) {
            if (joined.access == nullptr || joined.declaration == nullptr) {
              return std::unexpected(InternalFailure("joined source has no access leaf"));
            }
            const BoundTableSource& source = *joined.declaration;
            table_scan_ = std::get_if<PhysicalTableScanNode>(&joined.access->payload);
            rowid_lookup_ = std::get_if<PhysicalRowIdLookupNode>(&joined.access->payload);
            rowid_range_ = std::get_if<PhysicalRowIdRangeNode>(&joined.access->payload);
            index_scan_ = std::get_if<PhysicalIndexScanNode>(&joined.access->payload);
            leaf_ = joined.access;
            active_source_column_begin_ = source.source_column_begin;
            active_source_column_count_ = source.source_column_count;
            if (const auto* automatic =
                    std::get_if<PhysicalAutomaticIndexNode>(&joined.access->payload);
                automatic != nullptr) {
              if (auto added = AddAutomaticIndexDescriptors(joined, *automatic);
                  !added.has_value()) {
                return added;
              }
            } else {
              if (auto added = AddCursorDescriptor(); !added.has_value()) {
                return added;
              }
              joined.cursor =
                  index_scan_ != nullptr ? read_registers_->index_cursor : read_registers_->cursor;
              if (index_scan_ != nullptr && !index_scan_->covering) {
                joined.table_cursor = read_registers_->cursor;
              }
            }
            if (auto added = AddRightJoinDescriptor(joined, source); !added.has_value()) {
              return added;
            }
          }
        }
        if ((core.table_scan != nullptr || core.rowid_lookup != nullptr ||
             core.rowid_range != nullptr || core.index_scan != nullptr) &&
            core.aggregate == nullptr) {
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
    ReadRegisterLayout* previous = read_registers_;
    for (StreamingCorePlan& core : streaming_cores_) {
      const auto* select = std::get_if<BoundSelectCore>(core.bound);
      if (select == nullptr || !select->aggregate.has_value()) {
        continue;
      }
      if (native_compound_root_.has_value() && !core.native_scope.has_value()) {
        continue;
      }
      const auto described = [&]() -> LoweringResult<void> {
        if (native_compound_root_.has_value()) {
          if (!core.native_scope.has_value()) {
            return std::unexpected(InternalFailure("native aggregate has no lexical workspace"));
          }
          read_registers_ = &NativeRegisters(*core.native_scope);
        }
        if (auto context = SetStreamingAggregateCoreContext(core); !context.has_value()) {
          return context;
        }
        if (IsCursorAccess()) {
          if (auto cursor = AddCursorDescriptor(); !cursor.has_value()) {
            return cursor;
          }
          core.cursor = read_registers_->cursor;
          core.index_cursor = read_registers_->index_cursor;
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
          core.group_sorter = read_registers_->grouped_sorter;
        }
        return {};
      }();
      read_registers_ = previous;
      if (!described.has_value()) {
        return described;
      }
    }
    ClearStreamingAggregateContext();
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

  [[nodiscard]] LoweringResult<CursorFieldSource> CatalogFieldSource(const CatalogTable& table,
                                                                     ColumnId column) {
    if (column.value >= table.columns.size() ||
        column.value > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InternalFailure("table cursor column is invalid"));
    }
    if (table.rowid_alias.has_value() && *table.rowid_alias == column) {
      return CursorFieldSource{.kind = CursorFieldSourceKind::kRowId, .record_field = 0};
    }
    return CatalogFieldSource(table.columns[column.value],
                              static_cast<std::uint32_t>(column.value));
  }

  [[nodiscard]] LoweringResult<void> MapCoveringSourceFields(
      BoundSourceId owner, const CatalogTable& table,
      std::span<std::optional<std::uint32_t>> field_by_column,
      std::optional<CursorFieldId> rowid_field) {
    if (field_by_column.size() != table.columns.size() ||
        active_source_column_begin_ > bound_select_->source_columns().size() ||
        active_source_column_count_ >
            bound_select_->source_columns().size() - active_source_column_begin_) {
      return std::unexpected(InternalFailure("covering source field shape is invalid"));
    }
    if (table.rowid_alias.has_value() && rowid_field.has_value() &&
        !field_by_column[table.rowid_alias->value].has_value()) {
      field_by_column[table.rowid_alias->value] = rowid_field->value();
    }
    const auto columns = bound_select_->source_columns().subspan(active_source_column_begin_,
                                                                 active_source_column_count_);
    for (std::size_t local_index = 0; local_index < columns.size(); ++local_index) {
      const std::size_t source_index = active_source_column_begin_ + local_index;
      const BoundSourceColumn& column = columns[local_index];
      if (column.source != owner || !column.catalog_column.has_value() ||
          column.catalog_column->value >= field_by_column.size()) {
        return std::unexpected(InternalFailure("covering source column is invalid"));
      }
      const auto field = field_by_column[column.catalog_column->value];
      source_cursor_fields_[source_index] =
          field.has_value() ? std::optional{CursorFieldId{*field}} : std::nullopt;
      source_field_real_affinity_[source_index] =
          field.has_value() && column.affinity == TypeAffinity::kReal;
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddAutomaticIndexDescriptors(
      JoinedSourcePlan& source, const PhysicalAutomaticIndexNode& automatic) {
    const CatalogSnapshot& catalog = *bound_select_->catalog();
    if (automatic.table.value >= catalog.tables().size()) {
      return std::unexpected(InternalFailure("automatic index table is invalid"));
    }
    const CatalogTable& table = catalog.table(automatic.table);
    const std::size_t field_count =
        automatic.equalities.size() + automatic.covering_columns.size() + 1U;
    if (table.without_rowid || table.root_page != automatic.table_root_page ||
        automatic.equalities.empty() ||
        table.columns.size() > std::numeric_limits<std::uint32_t>::max() ||
        field_count > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InternalFailure("automatic index record shape is invalid"));
    }
    ReadCursorDescriptor construction{
        .root_page = RootPageNumber(table.root_page.value),
        .storage = CursorStorageKind::kRowIdTable,
        .record_field_count = static_cast<std::uint32_t>(table.columns.size()),
        .fields = {},
        .index_columns = {},
    };
    OrderingRecordDescriptor relation{
        .field_count = static_cast<std::uint32_t>(field_count),
        .key_field_count = static_cast<std::uint32_t>(field_count),
        .key_columns = {},
        .relation_purpose = RelationPurpose::kLookup,
    };
    ReadCursorDescriptor view{
        .root_page = RootPageNumber(0),
        .storage = CursorStorageKind::kEphemeralIndex,
        .record_field_count = static_cast<std::uint32_t>(field_count),
        .fields = {},
        .index_columns = {},
    };
    construction.fields.reserve(field_count);
    relation.key_columns.reserve(field_count);
    view.fields.reserve(field_count);
    std::vector<std::optional<std::uint32_t>> field_by_column(table.columns.size());
    const auto add_column = [&](ColumnId column, SymbolId collation) -> LoweringResult<void> {
      auto field = CatalogFieldSource(table, column);
      if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
      }
      const auto position = static_cast<std::uint32_t>(construction.fields.size());
      construction.fields.push_back(*field);
      view.fields.push_back(
          CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = position});
      relation.key_columns.push_back(OrderingColumnMetadata{.collation = collation});
      if (!field_by_column[column.value].has_value()) {
        field_by_column[column.value] = position;
      }
      return {};
    };
    for (const PhysicalIndexEquality& equality : automatic.equalities) {
      auto collation = SymbolForName(equality.collation_name);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      if (auto added = add_column(equality.column, *collation); !added.has_value()) {
        return added;
      }
    }
    auto binary = SymbolForName("BINARY");
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    for (const ColumnId column : automatic.covering_columns) {
      if (auto added = add_column(column, *binary); !added.has_value()) {
        return added;
      }
    }
    const CursorFieldId rowid_field{static_cast<std::uint32_t>(field_count - 1U)};
    construction.fields.push_back(
        CursorFieldSource{.kind = CursorFieldSourceKind::kRowId, .record_field = 0});
    view.fields.push_back(CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField,
                                            .record_field = rowid_field.value()});
    relation.key_columns.push_back(OrderingColumnMetadata{.collation = *binary});
    auto owner = ConvertProgramResult(AssumeValue(builder_).AddRelation(std::move(relation)),
                                      "unable to add automatic lookup relation");
    if (!owner.has_value()) {
      return std::unexpected(std::move(owner.error()));
    }
    view.relation = *owner;
    auto build_cursor =
        ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(construction)),
                             "unable to add automatic index construction cursor");
    if (!build_cursor.has_value()) {
      return std::unexpected(std::move(build_cursor.error()));
    }
    auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(view)),
                                       "unable to add automatic index view");
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    source.automatic_relation = *owner;
    source.construction_cursor = *build_cursor;
    source.cursor = *cursor;
    if (auto mapped =
            MapCoveringSourceFields(automatic.source, table, field_by_column, rowid_field);
        !mapped.has_value()) {
      return mapped;
    }
    return PublishSourceCursor(automatic.source, *cursor, rowid_field);
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
        : rowid_range_ != nullptr  ? std::optional{rowid_range_->source}
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
          .index_key_field_count = static_cast<std::uint32_t>(
              index.unique_not_null ? index.key_term_count : index.terms.size()),
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
        if (auto mapped = MapCoveringSourceFields(owner, table, field_by_column, rowid_field);
            !mapped.has_value()) {
          return mapped;
        }
      }

      auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                         "unable to add the index cursor descriptor");
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      read_registers_->index_cursor = *cursor;
      if (index_scan_->covering) {
        read_registers_->cursor = *cursor;
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
    } else if (rowid_range_ != nullptr) {
      source_kind = rowid_range_->source_kind;
      access_table = rowid_range_->table;
      root_page = rowid_range_->root_page.value;
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
          auto field = CatalogFieldSource(table, column_id);
          if (!field.has_value()) {
            return std::unexpected(std::move(field.error()));
          }
          descriptor.fields.push_back(*field);
          source_field_real_affinity_[source_index] = source_column.affinity == TypeAffinity::kReal;
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
        descriptor.index_key_field_count = static_cast<std::uint32_t>(primary.key_term_count);
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
    read_registers_->cursor = *cursor;
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
      auto field = CatalogFieldSource(table, column_id);
      if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
      }
      descriptor.fields.push_back(*field);
      source_field_real_affinity_[index] = source_column.affinity == TypeAffinity::kReal;
    }

    auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                       "unable to add the mutation read cursor descriptor");
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    read_registers_->cursor = *cursor;
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
      const bool check_not_null =
          column.not_null &&
          (bound_update_ == nullptr || column.rowid_alias ||
           std::ranges::any_of(
               bound_update_->assignments(), [&](const BoundUpdateAssignment& assignment) {
                 return assignment.effective && assignment.target.column == column.column;
               }));
      if (check_not_null && !column.rowid_alias &&
          (bound_insert_ != nullptr || bound_update_ != nullptr)) {
        auto marked = ConvertProgramResult(AssumeValue(builder_).MarkMayAbort(),
                                           "unable to mark NOT NULL failure");
        if (!marked.has_value()) {
          return std::unexpected(std::move(marked.error()));
        }
      }
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
          .not_null = check_not_null,
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
      const BoundMutationTarget& target, const BoundUpdate* update = nullptr) {
    index_write_cursors_.clear();
    index_write_cursors_.reserve(target.indexes.size());
    const auto add_index = [&](std::size_t index_position) -> LoweringResult<void> {
      if (index_position >= target.indexes.size()) {
        return std::unexpected(InternalFailure("selected mutation index is invalid"));
      }
      const BoundIndexMaintenance& index = target.indexes[index_position];
      if (update != nullptr) {
        auto affected = UpdateAffectsIndex(*update, index);
        if (!affected.has_value()) {
          return std::unexpected(std::move(affected.error()));
        }
        if (!*affected) {
          return {};
        }
      }
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
      index_write_cursors_.push_back(MutationIndexCursor{
          .target_index = index_position,
          .cursor = *cursor,
      });
      return {};
    };
    for (std::size_t index_position = 0; index_position < target.indexes.size(); ++index_position) {
      if (auto added = add_index(index_position); !added.has_value()) {
        return added;
      }
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
    OrderingRecordDescriptor sorter_descriptor{
        .field_count = static_cast<std::uint32_t>(terms.size() + 1U),
        .key_field_count = static_cast<std::uint32_t>(terms.size()),
        .key_columns = {},
    };
    index_descriptor.index_columns.reserve(terms.size() + 1U);
    sorter_descriptor.key_columns.reserve(terms.size());
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
      sorter_descriptor.key_columns.push_back(OrderingColumnMetadata{
          .collation = *collation,
          .order = term.order == SortOrder::kDescending ? BytecodeSortOrder::kDescending
                                                        : BytecodeSortOrder::kAscending,
          .null_placement = term.order == SortOrder::kDescending ? BytecodeNullPlacement::kLast
                                                                 : BytecodeNullPlacement::kFirst,
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
    sorter_descriptor.index_refill_cursor = *index_cursor;
    auto sorter =
        ConvertProgramResult(AssumeValue(builder_).AddSorter(std::move(sorter_descriptor)),
                             "unable to add the CREATE INDEX sorter");
    if (!sorter.has_value()) {
      return std::unexpected(std::move(sorter.error()));
    }
    create_index_sorter_ = *sorter;
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
          .index_key_field_count = index.unique_not_null || index.primary_without_rowid
                                       ? index.key_term_count
                                       : static_cast<std::uint32_t>(index.columns.size()),
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

  [[nodiscard]] static LoweringResult<bool> UpdateAffectsIndex(const BoundUpdate& update,
                                                               const BoundIndexMaintenance& index) {
    const BoundMutationTarget& target = update.target();
    for (const BoundIndexTerm& term : index.terms) {
      if (term.rowid) {
        if (std::ranges::any_of(update.assignments(), [](const BoundUpdateAssignment& assignment) {
              return assignment.effective && assignment.target.rowid;
            })) {
          return true;
        }
        continue;
      }
      if (!term.column.has_value()) {
        return std::unexpected(
            InternalFailure("bound index maintenance term has no physical source"));
      }
      const ColumnId indexed_column = term.column.value();
      if (!TargetColumnIndex(target, indexed_column).has_value()) {
        return std::unexpected(
            InternalFailure("bound index maintenance term targets an unknown column"));
      }
      for (const BoundUpdateAssignment& assignment : update.assignments()) {
        if (!assignment.effective || !assignment.target.column.has_value()) {
          continue;
        }
        const ColumnId assigned_column = assignment.target.column.value();
        if (!TargetColumnIndex(target, assigned_column).has_value()) {
          return std::unexpected(
              InternalFailure("effective UPDATE assignment targets an unknown column"));
        }
        if (assigned_column == indexed_column) {
          return true;
        }
      }
    }
    return false;
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

  [[nodiscard]] LoweringResult<SymbolId> EnsureBinarySymbol() override {
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
    if (mutation_plan_ != nullptr &&
        (std::holds_alternative<CallScalarInstruction>(instruction) ||
         std::holds_alternative<CallColumnScalarInstruction>(instruction) ||
         std::holds_alternative<CheckInsertRowIdInstruction>(instruction) ||
         std::holds_alternative<CheckUniqueIndexInstruction>(instruction))) {
      auto marked = ConvertProgramResult(AssumeValue(builder_).MarkMayAbort(),
                                         "unable to mark abortable mutation");
      if (!marked.has_value()) {
        return std::unexpected(std::move(marked.error()));
      }
    }
    if (read_registers_->row_destination.has_value()) {
      const SelectOutputContext& destination = *read_registers_->row_destination;
      if (const auto* row = std::get_if<ResultRowInstruction>(&instruction); row != nullptr) {
        if (row->count != bound_select_->result_columns().size()) {
          return std::unexpected(InternalFailure("coroutine row destination has invalid width"));
        }
        if (row->first != read_registers_->result_block_first) {
          if (auto moved =
                  Append(MoveInstruction{.first_input = row->first,
                                         .first_output = read_registers_->result_block_first,
                                         .count = row->count});
              !moved.has_value()) {
            return moved;
          }
        }
        if (destination.coroutine.has_value()) {
          instruction = YieldCoroutineInstruction{.coroutine = *destination.coroutine};
        } else if (destination.mode == SelectOutputMode::Ordered &&
                   destination.ordered_layout != nullptr &&
                   destination.ordered_record_first.has_value()) {
          auto rejected = CreateLabel();
          if (!rejected.has_value()) {
            return std::unexpected(std::move(rejected.error()));
          }
          if (auto candidate = EmitOrderedCandidate(
                  *destination.ordered_layout, destination.ordered_sorter,
                  destination.ordered_top_n, *destination.ordered_record_first, *rejected);
              !candidate.has_value()) {
            return candidate;
          }
          return BindLabel(*rejected);
        } else if (destination.mode != SelectOutputMode::Streaming) {
          return std::unexpected(InternalFailure("internal row destination is incomplete"));
        }
      } else if (std::holds_alternative<HaltInstruction>(instruction)) {
        if (!destination.final_completion.has_value()) {
          return std::unexpected(InternalFailure("coroutine row destination has no completion"));
        }
        return EmitJump(*destination.final_completion);
      }
    }
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
    if (!target.captures.empty()) {
      if (query.value() > statement_.query_frames.size()) {
        return std::unexpected(InternalFailure("inline query has no prepared compiler frame"));
      }
      if (auto emitted = statement_.query_frames[query.value() - 1U]->EmitInlineQuery(*this);
          !emitted.has_value()) {
        return emitted;
      }
      return Append(CopyInstruction{.input = call.result, .output = destination});
    }
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

  [[nodiscard]] bool ExpressionContainsQuery(BoundExpressionId id) const {
    const BoundExpression& expression = Expression(id);
    return std::visit(
        [&](const auto& payload) -> bool {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, BoundSubqueryExpression>) {
            return true;
          } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                               std::is_same_v<Payload, BoundTruthTestExpression> ||
                               std::is_same_v<Payload, BoundLikelihoodExpression> ||
                               std::is_same_v<Payload, BoundCollateExpression>) {
            return ExpressionContainsQuery(payload.operand);
          } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression> ||
                               std::is_same_v<Payload, BoundComparisonExpression>) {
            return ExpressionContainsQuery(payload.left) || ExpressionContainsQuery(payload.right);
          } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
            return ExpressionContainsQuery(payload.target);
          } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression> ||
                               std::is_same_v<Payload, BoundCoalesceExpression> ||
                               std::is_same_v<Payload, BoundConditionalExpression>) {
            return std::ranges::any_of(payload.arguments, [this](BoundExpressionId argument) {
              return ExpressionContainsQuery(argument);
            });
          } else if constexpr (std::is_same_v<Payload, BoundAggregateCallExpression>) {
            assert(payload.invocation.value() < aggregate_invocations_.size());
            const BoundAggregateInvocation& invocation =
                aggregate_invocations_[payload.invocation.value()];
            return std::ranges::any_of(invocation.arguments,
                                       [this](BoundExpressionId argument) {
                                         return ExpressionContainsQuery(argument);
                                       }) ||
                   (invocation.filter.has_value() && ExpressionContainsQuery(*invocation.filter)) ||
                   std::ranges::any_of(invocation.order_by, [this](const BoundOrderingTerm& term) {
                     return ExpressionContainsQuery(term.expression);
                   });
          }
          return false;
        },
        expression.payload);
  }

  [[nodiscard]] const BoundTableSource* FindSource(BoundSourceId source) const noexcept {
    if (bound_select_ == nullptr) {
      return nullptr;
    }
    for (const BoundQueryCore& core : bound_select_->query_cores()) {
      const auto* select = std::get_if<BoundSelectCore>(&core);
      if (select == nullptr) {
        continue;
      }
      const auto found = std::ranges::find(select->table_sources, source, &BoundTableSource::id);
      if (found != select->table_sources.end()) {
        return &*found;
      }
    }
    return lexical_parent_.has_value() ? lexical_parent_->frame->FindSource(source) : nullptr;
  }

  [[nodiscard]] bool SourceCanBeNullExtended(BoundSourceId source) const noexcept {
    if (bound_select_ == nullptr) {
      return false;
    }
    for (const BoundQueryCore& core : bound_select_->query_cores()) {
      const auto* select = std::get_if<BoundSelectCore>(&core);
      if (select == nullptr) {
        continue;
      }
      const auto found = std::ranges::find(select->table_sources, source, &BoundTableSource::id);
      if (found == select->table_sources.end()) {
        continue;
      }
      const auto index =
          static_cast<std::size_t>(std::distance(select->table_sources.begin(), found));
      if (index != 0U && SourceJoinPreservesLeft(found->join)) {
        return true;
      }
      return std::ranges::any_of(
          select->table_sources.begin() + static_cast<std::ptrdiff_t>(index + 1U),
          select->table_sources.end(), [](const BoundTableSource& candidate) {
            return SourceJoinPreservesRight(candidate.join);
          });
    }
    return lexical_parent_.has_value() ? lexical_parent_->frame->SourceCanBeNullExtended(source)
                                       : true;
  }

  [[nodiscard]] bool ExpressionMayBeNull(BoundExpressionId id) const noexcept {
    while (true) {
      const BoundExpression& expression = Expression(id);
      const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
      if (unary == nullptr || (unary->operation != BoundUnaryOperation::kPositive &&
                               unary->operation != BoundUnaryOperation::kNegative)) {
        break;
      }
      id = unary->operand;
    }

    const BoundExpression& expression = Expression(id);
    if (const auto* literal = std::get_if<BoundLiteralExpression>(&expression.payload);
        literal != nullptr) {
      return literal->value.type() == SqlValueType::kNull;
    }
    if (const auto* rowid = std::get_if<BoundRowIdExpression>(&expression.payload);
        rowid != nullptr) {
      return SourceCanBeNullExtended(rowid->source);
    }
    const auto* column = std::get_if<BoundColumnExpression>(&expression.payload);
    if (column == nullptr || column->column.value() >= source_columns_.size()) {
      return true;
    }
    const BoundSourceColumn& source_column = source_columns_[column->column.value()];
    if (SourceCanBeNullExtended(source_column.source)) {
      return true;
    }
    if (source_column.rowid_alias) {
      return false;
    }
    const BoundTableSource* source = FindSource(source_column.source);
    if (source == nullptr || !source->table.has_value() ||
        !source_column.catalog_column.has_value() || bound_select_->catalog() == nullptr) {
      return true;
    }
    const CatalogTable& table = bound_select_->catalog()->table(*source->table);
    if (source_column.catalog_column->value >= table.columns.size()) {
      return true;
    }
    return !table.columns[source_column.catalog_column->value]
                .effective_not_null_conflict.has_value();
  }

  [[nodiscard]] LoweringResult<void> EmitRegisterBranch(RegisterId value, Label target,
                                                        ConditionalBranch branch,
                                                        bool jump_on_null) {
    const JumpCondition condition =
        branch == ConditionalBranch::kTrue
            ? (jump_on_null ? JumpCondition::kIfTrueOrNull : JumpCondition::kIfTrue)
            : (jump_on_null ? JumpCondition::kIfFalseOrNull : JumpCondition::kIfFalse);
    return EmitJumpIf(value, condition, target);
  }

  [[nodiscard]] LoweringResult<void> EmitComparisonBranch(
      BoundExpressionId id, const BoundComparisonExpression& comparison, Label target,
      ConditionalBranch branch, bool jump_on_null) {
    const bool null_equal = comparison.comparison == SqlComparison::kIs ||
                            comparison.comparison == SqlComparison::kIsNot;
    const bool left_query = ExpressionContainsQuery(comparison.left);
    const bool right_query = ExpressionContainsQuery(comparison.right);
    if (null_equal || (!left_query && !right_query)) {
      const RegisterId value = Home(id);
      if (auto emitted = EmitComparisonValue(comparison, value); !emitted.has_value()) {
        return emitted;
      }
      return EmitRegisterBranch(value, target, branch, jump_on_null);
    }

    std::optional<Label> null_fallthrough;
    const auto emit_null_edge = [&](RegisterId value) -> LoweringResult<void> {
      if (jump_on_null) {
        return EmitJumpIf(value, JumpCondition::kIfNull, target);
      }
      auto fallthrough = CreateLabel();
      if (!fallthrough.has_value()) {
        return std::unexpected(std::move(fallthrough.error()));
      }
      null_fallthrough = *fallthrough;
      return EmitJumpIf(value, JumpCondition::kIfNull, *fallthrough);
    };

    const RegisterId left = Home(comparison.left);
    const RegisterId right = Home(comparison.right);
    const bool right_first = left_query && !right_query && ExpressionMayBeNull(comparison.right);
    if (right_first) {
      if (auto emitted = EmitExpression(comparison.right, right); !emitted.has_value()) {
        return emitted;
      }
      if (auto skipped = emit_null_edge(right); !skipped.has_value()) {
        return skipped;
      }
      if (auto emitted = EmitExpression(comparison.left, left); !emitted.has_value()) {
        return emitted;
      }
    } else {
      if (auto emitted = EmitExpression(comparison.left, left); !emitted.has_value()) {
        return emitted;
      }
      if (right_query && ExpressionMayBeNull(comparison.left)) {
        if (auto skipped = emit_null_edge(left); !skipped.has_value()) {
          return skipped;
        }
      }
      if (auto emitted = EmitExpression(comparison.right, right); !emitted.has_value()) {
        return emitted;
      }
    }

    const RegisterId value = Home(id);
    if (auto compared = EmitComparisonResult(comparison, value); !compared.has_value()) {
      return compared;
    }
    if (auto jumped = EmitRegisterBranch(value, target, branch, jump_on_null);
        !jumped.has_value()) {
      return jumped;
    }
    return null_fallthrough.has_value() ? BindLabel(*null_fallthrough) : LoweringResult<void>{};
  }

  [[nodiscard]] LoweringResult<void> EmitConditionalBranch(BoundExpressionId id, Label target,
                                                           ConditionalBranch branch,
                                                           bool jump_on_null) {
    if (id.value() >= expressions_.size()) {
      return std::unexpected(InternalFailure("conditional expression ID is out of range"));
    }
    if (grouped_aggregate_query_ &&
        read_registers_->aggregate_expression_source ==
            lowering_internal::AggregateExpressionSource::Representative &&
        id.value() < grouped_key_expression_registers_.size() &&
        grouped_key_expression_registers_[id.value()].has_value()) {
      const RegisterId value = Home(id);
      if (auto emitted = EmitExpression(id, value); !emitted.has_value()) {
        return emitted;
      }
      return EmitRegisterBranch(value, target, branch, jump_on_null);
    }
    const BoundExpression& expression = Expression(id);
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
        binary != nullptr && (binary->operation == BoundBinaryOperation::kLogicalAnd ||
                              binary->operation == BoundBinaryOperation::kLogicalOr)) {
      if (const std::optional<BoundExpressionId> selected = SimplifiedLogicalOperand(*binary);
          selected.has_value()) {
        return EmitConditionalBranch(*selected, target, branch, jump_on_null);
      }
      BoundExpressionId first = binary->left;
      BoundExpressionId second = binary->right;
      if (ExpressionContainsQuery(first) && !ExpressionContainsQuery(second)) {
        std::swap(first, second);
      }
      const bool is_and = binary->operation == BoundBinaryOperation::kLogicalAnd;
      if ((branch == ConditionalBranch::kFalse && is_and) ||
          (branch == ConditionalBranch::kTrue && !is_and)) {
        if (auto emitted = EmitConditionalBranch(first, target, branch, jump_on_null);
            !emitted.has_value()) {
          return emitted;
        }
        return EmitConditionalBranch(second, target, branch, jump_on_null);
      }
      auto other = CreateLabel();
      if (!other.has_value()) {
        return std::unexpected(std::move(other.error()));
      }
      const ConditionalBranch opposite =
          branch == ConditionalBranch::kTrue ? ConditionalBranch::kFalse : ConditionalBranch::kTrue;
      if (auto emitted = EmitConditionalBranch(first, *other, opposite, !jump_on_null);
          !emitted.has_value()) {
        return emitted;
      }
      if (auto emitted = EmitConditionalBranch(second, target, branch, jump_on_null);
          !emitted.has_value()) {
        return emitted;
      }
      return BindLabel(*other);
    }
    if (const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
        unary != nullptr && unary->operation == BoundUnaryOperation::kLogicalNot) {
      const ConditionalBranch opposite =
          branch == ConditionalBranch::kTrue ? ConditionalBranch::kFalse : ConditionalBranch::kTrue;
      return EmitConditionalBranch(unary->operand, target, opposite, jump_on_null);
    }
    if (const auto* truth = std::get_if<BoundTruthTestExpression>(&expression.payload);
        truth != nullptr) {
      const bool target_true = branch == ConditionalBranch::kTrue;
      const bool operand_matches = target_true != truth->negated;
      const bool expected_true = truth->expected == BoundTruthValue::kTrue;
      const bool operand_true = operand_matches ? expected_true : !expected_true;
      return EmitConditionalBranch(
          truth->operand, target,
          operand_true ? ConditionalBranch::kTrue : ConditionalBranch::kFalse, !operand_matches);
    }
    if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.payload);
        comparison != nullptr) {
      return EmitComparisonBranch(id, *comparison, target, branch, jump_on_null);
    }
    if (const auto* alias = std::get_if<BoundAliasReferenceExpression>(&expression.payload);
        alias != nullptr) {
      return EmitConditionalBranch(alias->target, target, branch, jump_on_null);
    }
    if (const auto* likelihood = std::get_if<BoundLikelihoodExpression>(&expression.payload);
        likelihood != nullptr) {
      return EmitConditionalBranch(likelihood->operand, target, branch, jump_on_null);
    }
    if (const auto* collate = std::get_if<BoundCollateExpression>(&expression.payload);
        collate != nullptr) {
      return EmitConditionalBranch(collate->operand, target, branch, jump_on_null);
    }
    const RegisterId value = Home(id);
    if (auto emitted = EmitExpression(id, value); !emitted.has_value()) {
      return emitted;
    }
    return EmitRegisterBranch(value, target, branch, jump_on_null);
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
      auto next = CreateLabel();
      if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
      }
      if (auto rejected = EmitConditionalBranch(condition, *next, ConditionalBranch::kFalse, true);
          !rejected.has_value()) {
        return rejected;
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
    const auto& mapping = global_aggregate_query_ && global_representative_query_
                              ? read_registers_->global_representative_register_map
                          : read_registers_->aggregate_expression_source ==
                                  lowering_internal::AggregateExpressionSource::SortedRow
                              ? read_registers_->grouped_sorted_register_map
                              : read_registers_->grouped_representative_register_map;
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

  [[nodiscard]] LoweringResult<void> EmitCursorFieldValue(CursorId cursor, CursorFieldId field,
                                                          RegisterId destination,
                                                          bool real_affinity = false) {
    if (auto read =
            Append(ReadFieldInstruction{.cursor = cursor, .field = field, .output = destination});
        !read.has_value()) {
      return read;
    }
    if (real_affinity) {
      return Append(RealAffinityInstruction{.input = destination, .output = destination});
    }
    return {};
  }

  struct DirectColumnSource {
    CursorId cursor;
    CursorFieldId field;
    bool real_affinity;
  };

  [[nodiscard]] LoweringResult<std::optional<DirectColumnSource>> FindDirectColumnSource(
      const BoundColumnExpression& column) {
    if (bound_select_ == nullptr) {
      return std::optional<DirectColumnSource>{};
    }
    const BoundExpression expression{.payload = column};
    const auto identity = BoundSourceLeafIdentityOf(expression, bound_select_->source_columns());
    auto owner = SourceFrame(AssumeValue(identity));
    if (!owner.has_value()) {
      return std::unexpected(std::move(owner.error()));
    }
    if (*owner != this) {
      return (*owner)->FindDirectColumnSource(column);
    }
    if (((grouped_aggregate_query_ || global_representative_query_) &&
         read_registers_->aggregate_expression_source !=
             lowering_internal::AggregateExpressionSource::AccessRow) ||
        !source_snapshot_registers_.empty()) {
      return std::optional<DirectColumnSource>{};
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
    return std::optional<DirectColumnSource>{DirectColumnSource{
        .cursor = binding->cursor,
        .field = AssumeValue(source_cursor_fields_[column.column.value()]),
        .real_affinity = source_field_real_affinity_[column.column.value()],
    }};
  }

  [[nodiscard]] LoweringResult<void> EmitColumnValue(const BoundColumnExpression& column,
                                                     RegisterId destination) {
    if (bound_select_ != nullptr) {
      const BoundExpression expression{.payload = column};
      const auto identity = BoundSourceLeafIdentityOf(expression, bound_select_->source_columns());
      auto owner = SourceFrame(AssumeValue(identity));
      if (!owner.has_value()) {
        return std::unexpected(std::move(owner.error()));
      }
      if (*owner != this) {
        return (*owner)->EmitColumnValue(column, destination);
      }
    }
    if ((grouped_aggregate_query_ || global_representative_query_) &&
        read_registers_->aggregate_expression_source !=
            lowering_internal::AggregateExpressionSource::AccessRow) {
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
    auto direct = FindDirectColumnSource(column);
    if (!direct.has_value()) {
      return std::unexpected(std::move(direct.error()));
    }
    if (!direct->has_value()) {
      return std::unexpected(InternalFailure("bound column has no direct source"));
    }
    return EmitCursorFieldValue((**direct).cursor, (**direct).field, destination,
                                (**direct).real_affinity);
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
    auto owner = SourceFrame(BoundSourceLeafIdentity{.source = rowid.source});
    if (!owner.has_value()) {
      return std::unexpected(std::move(owner.error()));
    }
    if (*owner != this) {
      return (*owner)->EmitRowIdValue(rowid, destination);
    }
    if ((grouped_aggregate_query_ || global_representative_query_) &&
        read_registers_->aggregate_expression_source !=
            lowering_internal::AggregateExpressionSource::AccessRow) {
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

  [[nodiscard]] LoweringResult<PlanLowerer*> SourceFrame(const BoundSourceLeafIdentity& identity) {
    if (bound_select_ == nullptr) {
      return this;
    }
    const auto& captures = bound_select_->query().captures;
    const auto* capture = binder_detail::FindSourceCapture(std::span{captures}, identity);
    if (capture == nullptr) {
      return this;
    }
    if (!capture->value.has_value() || !lexical_parent_.has_value()) {
      return std::unexpected(InternalFailure("source leaf has no lexical read binding"));
    }
    return lexical_parent_->frame->SourceFrame(identity);
  }

  [[nodiscard]] LoweringResult<PlanLowerer*> QueryFrame(BoundQueryId query) {
    if (bound_select_ == nullptr) {
      return std::unexpected(InternalFailure("aggregate owner has no bound query"));
    }
    if (bound_select_->query().id == query) {
      return this;
    }
    if (!lexical_parent_.has_value()) {
      return std::unexpected(InternalFailure("aggregate owner has no lexical read binding"));
    }
    return lexical_parent_->frame->QueryFrame(query);
  }

  [[nodiscard]] LoweringResult<void> EmitAggregateResult(const BoundAggregateCallExpression& call,
                                                         RegisterId destination) {
    if (call.invocation.value() >= aggregate_invocations_.size()) {
      return std::unexpected(InternalFailure("aggregate call has invalid invocation"));
    }
    const BoundAggregateInvocation& invocation = aggregate_invocations_[call.invocation.value()];
    if (bound_select_ == nullptr) {
      return std::unexpected(InternalFailure("aggregate call has no bound query"));
    }
    if (invocation.owner != bound_select_->query().id) {
      auto owner = QueryFrame(invocation.owner);
      if (!owner.has_value()) {
        return std::unexpected(std::move(owner.error()));
      }
      return (*owner)->EmitAggregateResult(call, destination);
    }
    if ((!global_aggregate_query_ && !grouped_aggregate_query_) ||
        call.invocation.value() >= aggregate_result_registers_.size() ||
        !aggregate_result_registers_[call.invocation.value()].has_value()) {
      return std::unexpected(InternalFailure("aggregate call has no finalized register"));
    }
    return Append(CopyInstruction{
        .input = AssumeValue(aggregate_result_registers_[call.invocation.value()]),
        .output = destination,
    });
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

  [[nodiscard]] LoweringResult<void> EmitComparisonResult(
      const BoundComparisonExpression& comparison, RegisterId destination) {
    auto collation = CollationSymbol(comparison.collation);
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    return Append(CompareInstruction{
        .comparison = comparison.comparison,
        .affinity = comparison.affinity,
        .collation = *collation,
        .left = Home(comparison.left),
        .right = Home(comparison.right),
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
    return EmitComparisonResult(comparison, destination);
  }

  [[nodiscard]] LoweringResult<void> EmitScalarCallValue(BoundExpressionId id,
                                                         const BoundScalarCallExpression& call,
                                                         RegisterId destination) {
    const std::optional<RegisterId>& argument_block = call_argument_blocks_[id.value()];
    if (!argument_block.has_value()) {
      return std::unexpected(InternalFailure("scalar call has no argument block"));
    }
    const RegisterId first = *argument_block;
    auto function = FunctionSymbol(call.function);
    auto collation = CollationSymbol(call.collation);
    if (!function.has_value()) {
      return std::unexpected(std::move(function.error()));
    }
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    if (call.function.value() >= functions_.size()) {
      return std::unexpected(InternalFailure("bound function ID is out of range"));
    }
    if (call.arguments.size() == 1U &&
        functions_[call.function.value()].column_argument_use != ColumnArgumentUse::FullValue &&
        !functions_[call.function.value()].uses_collation &&
        call.arguments.front().value() < expressions_.size()) {
      const auto* column =
          std::get_if<BoundColumnExpression>(&Expression(call.arguments.front()).payload);
      if (column != nullptr) {
        auto direct = FindDirectColumnSource(*column);
        if (!direct.has_value()) {
          return std::unexpected(std::move(direct.error()));
        }
        if (direct->has_value()) {
          return Append(CallColumnScalarInstruction{
              .function = *function,
              .cursor = (**direct).cursor,
              .field = (**direct).field,
              .argument = first,
              .output = destination,
              .real_affinity = (**direct).real_affinity,
          });
        }
      }
    }
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
      if (auto emitted = EmitExpression(
              call.arguments[index], RegisterId{first.value() + static_cast<std::uint32_t>(index)});
          !emitted.has_value()) {
        return emitted;
      }
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
        read_registers_->aggregate_expression_source ==
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
      return EmitAggregateResult(*call, destination);
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
        std::exchange(read_registers_->aggregate_expression_source, source);
    LoweringResult<void> emitted = EmitExpression(id, destination);
    read_registers_->aggregate_expression_source = previous;
    return emitted;
  }

  [[nodiscard]] LoweringResult<void> EmitAggregatePredicate(
      BoundExpressionId id, Label rejected,
      lowering_internal::AggregateExpressionSource source) override {
    const lowering_internal::AggregateExpressionSource previous =
        std::exchange(read_registers_->aggregate_expression_source, source);
    LoweringResult<void> emitted = EmitPredicate(id, rejected);
    read_registers_->aggregate_expression_source = previous;
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
    return EmitConditionalBranch(predicate, false_target, ConditionalBranch::kFalse, true);
  }

  [[nodiscard]] LoweringResult<void> EmitReadConstantInitialization() {
    if (read_registers_->zero_register.has_value()) {
      auto zero = EnsureZeroConstant();
      if (!zero.has_value()) {
        return std::unexpected(std::move(zero.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *zero,
              .output = *read_registers_->zero_register,
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (read_registers_->one_register.has_value()) {
      auto one = EnsureOneConstant();
      if (!one.has_value()) {
        return std::unexpected(std::move(one.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *one,
              .output = *read_registers_->one_register,
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitLimitInitialization(
      std::optional<Label> completion) override {
    if (read_registers_->limit_initialized ||
        (limit_ == nullptr && !read_registers_->limit_input.has_value()) ||
        (read_registers_->row_destination.has_value() && !HasLimit())) {
      return {};
    }
    if (!completion.has_value() || !read_registers_->limit_register.has_value()) {
      return std::unexpected(InternalFailure("LIMIT has no completion or counter register"));
    }
    if (read_registers_->limit_input.has_value()) {
      if (auto copied = Append(CopyInstruction{.input = *read_registers_->limit_input,
                                               .output = *read_registers_->limit_register});
          !copied.has_value()) {
        return copied;
      }
    } else {
      if (auto emitted = EmitExpression(limit_->limit, *read_registers_->limit_register);
          !emitted.has_value()) {
        return emitted;
      }
      if (auto converted = Append(MustBeIntegerInstruction{
              .input = *read_registers_->limit_register,
              .output = *read_registers_->limit_register,
          });
          !converted.has_value()) {
        return converted;
      }
    }
    if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->limit_register),
                                 JumpCondition::kIfFalse, *completion);
        !jumped.has_value()) {
      return jumped;
    }

    const bool has_offset = !read_registers_->limit_input.has_value() && limit_ != nullptr &&
                            limit_->offset.has_value();
    if (has_offset) {
      if (!read_registers_->offset_register.has_value() ||
          !read_registers_->zero_register.has_value() ||
          !read_registers_->one_register.has_value() ||
          !read_registers_->comparison_register.has_value()) {
        return std::unexpected(InternalFailure("OFFSET has no control registers"));
      }
      if (auto emitted =
              EmitExpression(*limit_->offset, AssumeValue(read_registers_->offset_register));
          !emitted.has_value()) {
        return emitted;
      }
      if (auto converted = Append(MustBeIntegerInstruction{
              .input = AssumeValue(read_registers_->offset_register),
              .output = AssumeValue(read_registers_->offset_register),
          });
          !converted.has_value()) {
        return converted;
      }
    }

    if (auto constants = EmitReadConstantInitialization(); !constants.has_value()) {
      return constants;
    }

    if (has_offset) {
      auto binary = EnsureBinarySymbol();
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      if (auto compared = Append(CompareInstruction{
              .comparison = SqlComparison::kLess,
              .affinity = TypeAffinity::kNumeric,
              .collation = *binary,
              .left = AssumeValue(read_registers_->offset_register),
              .right = AssumeValue(read_registers_->zero_register),
              .output = AssumeValue(read_registers_->comparison_register),
          });
          !compared.has_value()) {
        return compared;
      }
      auto nonnegative = CreateLabel();
      if (!nonnegative.has_value()) {
        return std::unexpected(std::move(nonnegative.error()));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->comparison_register),
                                   JumpCondition::kIfFalse, *nonnegative);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto copied = Append(CopyInstruction{
              .input = AssumeValue(read_registers_->zero_register),
              .output = AssumeValue(read_registers_->offset_register),
          });
          !copied.has_value()) {
        return copied;
      }
      if (auto bound = BindLabel(*nonnegative); !bound.has_value()) {
        return bound;
      }
    }

    if (read_registers_->negative_limit_register.has_value()) {
      if (!read_registers_->zero_register.has_value()) {
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
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->zero_register),
              .output = AssumeValue(read_registers_->negative_limit_register),
          });
          !compared.has_value()) {
        return compared;
      }
    }
    read_registers_->limit_initialized = true;
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
    for (const RegisterId representative : read_registers_->global_representative_registers) {
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *null_constant,
              .output = representative,
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    if (HasAggregateRepresentativeController()) {
      if (!read_registers_->global_representative_replace_register.has_value()) {
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
      if (read_registers_->global_filtered_controller_defaults) {
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
          .output = AssumeValue(read_registers_->global_representative_replace_register),
      });
    }
    if (!read_registers_->global_has_representative_row_register.has_value()) {
      return std::unexpected(InternalFailure("global representative row flag is unavailable"));
    }
    auto zero = EnsureZeroConstant();
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    return Append(LoadConstantInstruction{
        .constant = *zero,
        .output = AssumeValue(read_registers_->global_has_representative_row_register),
    });
  }

  [[nodiscard]] LoweringResult<void> CaptureGlobalRepresentatives() {
    for (std::size_t index = 0; index < global_representative_leaves_.size(); ++index) {
      if (auto emitted =
              EmitAggregateExpression(global_representative_leaves_[index],
                                      read_registers_->global_representative_registers[index],
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
            .effect_output = global_representative_query_
                                 ? read_registers_->global_representative_effect_register
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
      if (!read_registers_->global_representative_effect_register.has_value() ||
          !read_registers_->global_representative_replace_register.has_value() ||
          !read_registers_->global_representative_comparison_register.has_value()) {
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
              .left = AssumeValue(read_registers_->global_representative_effect_register),
              .right = AssumeValue(read_registers_->global_representative_replace_register),
              .output = AssumeValue(read_registers_->global_representative_comparison_register),
          });
          !compared.has_value()) {
        return compared;
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(read_registers_->global_representative_comparison_register),
                         JumpCondition::kIfFalse, *keep);
          !jumped.has_value()) {
        return jumped;
      }
    } else {
      if (!read_registers_->global_has_representative_row_register.has_value()) {
        return std::unexpected(InternalFailure("global representative row flag is unavailable"));
      }
      if (auto jumped =
              EmitJumpIf(AssumeValue(read_registers_->global_has_representative_row_register),
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
              .output = AssumeValue(read_registers_->global_has_representative_row_register),
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
    if (!HasLimit() || !read_registers_->offset_register.has_value()) {
      return {};
    }
    if (!read_registers_->offset_register.has_value() ||
        !read_registers_->one_register.has_value()) {
      return std::unexpected(InternalFailure("global aggregate OFFSET has no registers"));
    }
    auto project = CreateLabel();
    if (!project.has_value()) {
      return std::unexpected(std::move(project.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->offset_register),
                                 JumpCondition::kIfFalse, *project);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto decremented = Append(BinaryInstruction{
            .operation = BinaryOperation::kSubtract,
            .left = AssumeValue(read_registers_->offset_register),
            .right = AssumeValue(read_registers_->one_register),
            .output = AssumeValue(read_registers_->offset_register),
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
      if (auto emitted =
              EmitProjectionValue(aggregate_core_->result_columns[index].expression,
                                  RegisterId{read_registers_->result_block_first.value() +
                                             static_cast<std::uint32_t>(index)},
                                  read_registers_->active_aggregate_output.has_value()
                                      ? read_registers_->active_aggregate_output->mode
                                      : SelectOutputMode::Streaming);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitActiveAggregateOutput(Label rejected, Label stopped) {
    if (!read_registers_->active_aggregate_output.has_value() ||
        read_registers_->active_aggregate_output->core == nullptr) {
      return std::unexpected(InternalFailure("aggregate output context is unavailable"));
    }
    const SelectOutputContext& output = *read_registers_->active_aggregate_output;
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
                                    output.ordered_top_n, *output.ordered_record_first, rejected);
    }
    return std::unexpected(InternalFailure("aggregate output mode is invalid"));
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateProjection() {
    if (auto projected = EmitAggregateCoreProjection(); !projected.has_value()) {
      return projected;
    }
    return Append(ResultRowInstruction{
        .first = read_registers_->result_block_first,
        .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateCompletion() override {
    if (auto finalized = FinalizeAndCloseGlobalAggregates(); !finalized.has_value()) {
      return finalized;
    }
    const lowering_internal::AggregateExpressionSource previous =
        std::exchange(read_registers_->aggregate_expression_source,
                      lowering_internal::AggregateExpressionSource::Representative);
    const auto restore = [this, previous](LoweringResult<void> result) {
      read_registers_->aggregate_expression_source = previous;
      return result;
    };
    if (read_registers_->active_aggregate_output.has_value()) {
      if (!read_registers_->aggregate_core_completion_label.has_value() ||
          !read_registers_->aggregate_core_stopped_label.has_value()) {
        return restore(
            std::unexpected(InternalFailure("active aggregate output labels are unavailable")));
      }
      const Label completed = *read_registers_->aggregate_core_completion_label;
      const Label stopped = *read_registers_->aggregate_core_stopped_label;
      if (having_ != nullptr) {
        if (auto having = EmitPredicate(having_->predicate, completed); !having.has_value()) {
          return restore(std::move(having));
        }
      }
      if (auto output = EmitActiveAggregateOutput(completed, stopped); !output.has_value()) {
        return restore(std::move(output));
      }
      return restore(EmitJump(completed));
    }
    const bool has_rejection =
        having_ != nullptr || (HasLimit() && read_registers_->offset_register.has_value());
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
    read_registers_->aggregate_expression_source = previous;
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateEarlyStopCompletion() override {
    if (read_registers_->active_aggregate_output.has_value()) {
      if (!read_registers_->aggregate_core_stopped_label.has_value()) {
        return std::unexpected(InternalFailure("active aggregate early-stop label is unavailable"));
      }
      return EmitJump(*read_registers_->aggregate_core_stopped_label);
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitGroupedAggregateProjection() {
    if (auto projected = EmitAggregateCoreProjection(); !projected.has_value()) {
      return projected;
    }
    return Append(ResultRowInstruction{
        .first = read_registers_->result_block_first,
        .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitCompletedGroupedAggregate(
      lowering_internal::GroupedCompletionTargets targets) override {
    const lowering_internal::AggregateExpressionSource previous =
        std::exchange(read_registers_->aggregate_expression_source,
                      lowering_internal::AggregateExpressionSource::Representative);
    const bool previous_output_phase = std::exchange(read_registers_->grouped_output_phase, true);
    const auto restore = [this, previous, previous_output_phase](LoweringResult<void> result) {
      read_registers_->aggregate_expression_source = previous;
      read_registers_->grouped_output_phase = previous_output_phase;
      return result;
    };

    if (having_ != nullptr) {
      if (auto having = EmitPredicate(having_->predicate, targets.skipped); !having.has_value()) {
        return restore(std::move(having));
      }
    }
    if (read_registers_->active_aggregate_output.has_value()) {
      return restore(EmitActiveAggregateOutput(targets.skipped, targets.stopped));
    }
    if (HasOrdering()) {
      return restore(EmitOrderedCandidate(targets.skipped));
    }
    if (HasLimit() && read_registers_->offset_register.has_value()) {
      if (auto offset = EmitOffset(targets.skipped); !offset.has_value()) {
        return restore(std::move(offset));
      }
    }
    if (auto projection = EmitGroupedAggregateProjection(); !projection.has_value()) {
      return restore(std::move(projection));
    }
    if (HasLimit()) {
      if (!read_registers_->negative_limit_register.has_value() ||
          !read_registers_->limit_register.has_value() ||
          !read_registers_->one_register.has_value()) {
        return restore(
            std::unexpected(InternalFailure("grouped aggregate LIMIT counters are unavailable")));
      }
      auto unlimited = CreateLabel();
      if (!unlimited.has_value()) {
        return restore(std::unexpected(std::move(unlimited.error())));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                   JumpCondition::kIfTrue, *unlimited);
          !jumped.has_value()) {
        return restore(std::move(jumped));
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->one_register),
              .output = AssumeValue(read_registers_->limit_register),
          });
          !decremented.has_value()) {
        return restore(std::move(decremented));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->limit_register),
                                   JumpCondition::kIfFalse, targets.stopped);
          !jumped.has_value()) {
        return restore(std::move(jumped));
      }
      if (auto bound = BindLabel(*unlimited); !bound.has_value()) {
        return restore(std::move(bound));
      }
    }
    read_registers_->aggregate_expression_source = previous;
    read_registers_->grouped_output_phase = previous_output_phase;
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitGroupedOutputCompletion() override {
    if (read_registers_->active_aggregate_output.has_value()) {
      if (!read_registers_->aggregate_core_completion_label.has_value()) {
        return std::unexpected(InternalFailure("active grouped completion label is unavailable"));
      }
      return EmitJump(*read_registers_->aggregate_core_completion_label);
    }
    if (HasOrdering()) {
      return EmitOrdinaryReadCompletion();
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitGroupedEarlyStopCompletion(
      bool source_row_active) override {
    if (source_row_active &&
        (read_registers_->active_aggregate_output.has_value() ||
         bound_select_->query().destination != BoundQueryDestination::kClient)) {
      if (auto closed =
              EmitCloseReadCursors(read_registers_->cursor, read_registers_->index_cursor);
          !closed.has_value()) {
        return closed;
      }
    }
    if (read_registers_->active_aggregate_output.has_value()) {
      if (!read_registers_->aggregate_core_stopped_label.has_value()) {
        return std::unexpected(InternalFailure("active grouped early-stop label is unavailable"));
      }
      const Label stopped = *read_registers_->aggregate_core_stopped_label;
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
    if (grouped_aggregate_query_ && !read_registers_->grouped_output_phase && !ordered_drain) {
      return {};
    }
    if (!HasLimit() || !read_registers_->offset_register.has_value()) {
      return {};
    }
    if (sort_ != nullptr && !sort_->input_order_satisfied() && !ordered_drain) {
      return {};
    }
    if (!read_registers_->offset_register.has_value() ||
        !read_registers_->one_register.has_value()) {
      return std::unexpected(InternalFailure("OFFSET has no row-loop registers"));
    }
    auto project = CreateLabel();
    if (!project.has_value()) {
      return std::unexpected(std::move(project.error()));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->offset_register),
                                 JumpCondition::kIfFalse, *project);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto decremented = Append(BinaryInstruction{
            .operation = BinaryOperation::kSubtract,
            .left = AssumeValue(read_registers_->offset_register),
            .right = AssumeValue(read_registers_->one_register),
            .output = AssumeValue(read_registers_->offset_register),
        });
        !decremented.has_value()) {
      return decremented;
    }
    if (auto jumped = EmitJump(skipped); !jumped.has_value()) {
      return jumped;
    }
    return BindLabel(*project);
  }

  [[nodiscard]] LoweringResult<void> InitializeOrderBlock() {
    if (!HasBlockOrdering()) {
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
    for (std::uint32_t field = 0; field < OrderedPrefix(); ++field) {
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *null,
              .output =
                  RegisterId{AssumeValue(read_registers_->order_prefix_previous_first).value() +
                             field},
          });
          !loaded.has_value()) {
        return loaded;
      }
    }
    return Append(LoadConstantInstruction{
        .constant = *zero,
        .output = AssumeValue(read_registers_->order_prefix_present),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitOrderBlockDrain() {
    if (!HasBlockOrdering() || read_registers_->ordering_runtime == OrderingRuntime::kNone) {
      return std::unexpected(InternalFailure("ordered block has no active sorting capability"));
    }
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
    auto rewound =
        ConvertProgramResult(read_registers_->ordering_runtime == OrderingRuntime::kSorter
                                 ? AssumeValue(builder_).EmitRewindSorter(BlockSorter(), *empty)
                                 : AssumeValue(builder_).EmitRewindTopN(BlockTopN(), *empty),
                             "unable to rewind ordered block");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (HasLimit()) {
      if (auto offset = EmitOffset(*advance, true); !offset.has_value()) {
        return offset;
      }
    }
    const auto fields = BlockOutputFields();
    const auto key_count =
        static_cast<std::uint32_t>(RequestedOrderTerms().size() - OrderedPrefix());
    for (std::size_t column = 0; column < fields.size(); ++column) {
      const auto& field = fields[column];
      const std::uint32_t position = field.kind == SortOutputFieldKind::kKey
                                         ? field.field_index
                                         : key_count + field.field_index;
      const RegisterId output{read_registers_->result_block_first.value() +
                              static_cast<std::uint32_t>(column)};
      const Instruction read = read_registers_->ordering_runtime == OrderingRuntime::kSorter
                                   ? Instruction{ReadSorterFieldInstruction{
                                         .sorter = BlockSorter(),
                                         .field = position,
                                         .output = output,
                                     }}
                                   : Instruction{ReadTopNFieldInstruction{
                                         .top_n = BlockTopN(),
                                         .field = position,
                                         .output = output,
                                     }};
      if (auto emitted = Append(read); !emitted.has_value()) {
        return emitted;
      }
    }
    if (auto result = Append(ResultRowInstruction{
            .first = read_registers_->result_block_first,
            .count = static_cast<std::uint32_t>(fields.size()),
        });
        !result.has_value()) {
      return result;
    }
    if (HasLimit()) {
      if (auto unbounded = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                      JumpCondition::kIfTrue, *advance);
          !unbounded.has_value()) {
        return unbounded;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->one_register),
              .output = AssumeValue(read_registers_->limit_register),
          });
          !decremented.has_value()) {
        return decremented;
      }
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(read_registers_->ordering_runtime == OrderingRuntime::kSorter
                                         ? AssumeValue(builder_).EmitNextSorter(BlockSorter(), *row)
                                         : AssumeValue(builder_).EmitNextTopN(BlockTopN(), *row),
                                     "unable to advance ordered block");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    return BindLabel(*empty);
  }

  [[nodiscard]] LoweringResult<void> CloseOrderBlock() {
    return read_registers_->ordering_runtime == OrderingRuntime::kSorter
               ? Append(CloseSorterInstruction{.sorter = BlockSorter()})
               : Append(CloseTopNInstruction{.top_n = BlockTopN()});
  }

  [[nodiscard]] LoweringResult<void> CloseOrderBlockSources() {
    if (sort_ != nullptr) {
      return EmitCloseReadCursors(read_registers_->cursor, read_registers_->index_cursor);
    }
    const StreamingCorePlan& core = active_core_order_ != nullptr
                                        ? streaming_cores_[active_core_order_->layout.core_index]
                                        : streaming_cores_.front();
    return core.join != nullptr ? EmitJoinedSourceClose(core, core.relation)
                                : EmitStreamingClose(core, core.cursor.has_value());
  }

  [[nodiscard]] LoweringResult<void> ResetOrderBlock() {
    if (read_registers_->ordering_runtime == OrderingRuntime::kSorter) {
      return Append(ResetSorterInstruction{.sorter = BlockSorter()});
    }
    const RegisterId bound = AssumeValue(read_registers_->top_n_bound_register);
    if (read_registers_->offset_register.has_value()) {
      if (auto added = Append(BinaryInstruction{
              .operation = BinaryOperation::kAdd,
              .left = AssumeValue(read_registers_->limit_register),
              .right = *read_registers_->offset_register,
              .output = bound,
          });
          !added.has_value()) {
        return added;
      }
    } else if (auto copied = Append(CopyInstruction{
                   .input = AssumeValue(read_registers_->limit_register), .output = bound});
               !copied.has_value()) {
      return copied;
    }
    return Append(ResetTopNInstruction{
        .top_n = BlockTopN(),
        .residual_bound = bound,
    });
  }

  [[nodiscard]] LoweringResult<void> EmitOrderBlockLimitCheck() {
    if (!HasLimit()) {
      return {};
    }
    auto remaining = CreateLabel();
    if (!remaining.has_value()) {
      return std::unexpected(std::move(remaining.error()));
    }
    if (auto unlimited = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                    JumpCondition::kIfTrue, *remaining);
        !unlimited.has_value()) {
      return unlimited;
    }
    if (auto more = EmitJumpIf(AssumeValue(read_registers_->limit_register), JumpCondition::kIfTrue,
                               *remaining);
        !more.has_value()) {
      return more;
    }
    if (auto sources = CloseOrderBlockSources(); !sources.has_value()) {
      return sources;
    }
    if (auto closed = CloseOrderBlock(); !closed.has_value()) {
      return closed;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    return BindLabel(*remaining);
  }

  [[nodiscard]] LoweringResult<void> EmitOrderBlockCandidate(RegisterId record_first,
                                                             std::uint32_t value_count,
                                                             Label rejected) {
    if (!HasBlockOrdering() || read_registers_->ordering_runtime == OrderingRuntime::kNone) {
      return std::unexpected(InternalFailure("ordered block candidate has no active capability"));
    }
    const std::uint32_t prefix = OrderedPrefix();
    const RegisterId stored_first{record_first.value() + prefix};
    if (auto encoded = Append(EncodeRecordInstruction{
            .first_value = stored_first,
            .value_count = value_count - prefix,
            .output = AssumeValue(read_registers_->encoded_order_record),
        });
        !encoded.has_value()) {
      return encoded;
    }
    auto install = CreateLabel();
    auto admit = CreateLabel();
    if (!install.has_value()) {
      return std::unexpected(std::move(install.error()));
    }
    if (!admit.has_value()) {
      return std::unexpected(std::move(admit.error()));
    }
    if (auto first = EmitJumpIf(AssumeValue(read_registers_->order_prefix_present),
                                JumpCondition::kIfFalse, *install);
        !first.has_value()) {
      return first;
    }
    if (auto compared = Append(CompareRecordsInstruction{
            .comparison = AssumeValue(read_registers_->order_prefix_comparison),
            .left_first = AssumeValue(read_registers_->order_prefix_previous_first),
            .right_first = record_first,
            .output = AssumeValue(read_registers_->order_prefix_comparison_register),
        });
        !compared.has_value()) {
      return compared;
    }
    if (auto same = EmitJumpIf(AssumeValue(read_registers_->order_prefix_comparison_register),
                               JumpCondition::kIfFalse, *admit);
        !same.has_value()) {
      return same;
    }
    if (auto drained = EmitOrderBlockDrain(); !drained.has_value()) {
      return drained;
    }
    if (auto reset = ResetOrderBlock(); !reset.has_value()) {
      return reset;
    }
    if (auto limit = EmitOrderBlockLimitCheck(); !limit.has_value()) {
      return limit;
    }
    if (auto bound = BindLabel(*install); !bound.has_value()) {
      return bound;
    }
    if (auto moved = Append(MoveInstruction{
            .first_input = record_first,
            .first_output = AssumeValue(read_registers_->order_prefix_previous_first),
            .count = prefix,
        });
        !moved.has_value()) {
      return moved;
    }
    auto one = EnsureOneConstant();
    if (!one.has_value()) {
      return std::unexpected(std::move(one.error()));
    }
    if (auto loaded = Append(LoadConstantInstruction{
            .constant = *one,
            .output = AssumeValue(read_registers_->order_prefix_present),
        });
        !loaded.has_value()) {
      return loaded;
    }
    if (auto bound = BindLabel(*admit); !bound.has_value()) {
      return bound;
    }
    if (read_registers_->ordering_runtime == OrderingRuntime::kTopN) {
      const auto key_count = static_cast<std::uint32_t>(RequestedOrderTerms().size()) - prefix;
      auto checked = ConvertProgramResult(
          AssumeValue(builder_).EmitCheckTopN(BlockTopN(), stored_first, key_count, rejected),
          "unable to admit an ordered-block candidate");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
      return Append(InsertEncodedTopNInstruction{
          .top_n = BlockTopN(),
          .record = AssumeValue(read_registers_->encoded_order_record),
      });
    }
    return Append(InsertEncodedSorterInstruction{
        .sorter = BlockSorter(),
        .record = AssumeValue(read_registers_->encoded_order_record),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitOrderingKeys() {
    if (sort_ == nullptr || !read_registers_->sorter_record_first.has_value()) {
      return std::unexpected(InternalFailure("ordering keys have no record registers"));
    }
    const RegisterId record_first = AssumeValue(read_registers_->sorter_record_first);
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
    if (sort_ == nullptr || !read_registers_->sorter_record_first.has_value()) {
      return std::unexpected(InternalFailure("ordering payload has no record registers"));
    }
    const RegisterId record_first = AssumeValue(read_registers_->sorter_record_first);
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
    if (sort_ == nullptr || !read_registers_->sorter_record_first.has_value()) {
      return std::unexpected(InternalFailure("ordered candidate has no record registers"));
    }
    const RegisterId record_first = AssumeValue(read_registers_->sorter_record_first);
    const auto key_count = static_cast<std::uint32_t>(sort_->terms.size());
    const auto value_count =
        static_cast<std::uint32_t>(sort_->terms.size() + sort_->payload_expressions.size());

    if (HasBlockOrdering()) {
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
        if (auto payload = EmitOrderingPayload(); !payload.has_value()) {
          return payload;
        }
      }
      return EmitOrderBlockCandidate(record_first, value_count, rejected);
    }

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
      if (read_registers_->ordering_runtime == OrderingRuntime::kTopN) {
        if (!read_registers_->top_n.has_value()) {
          return std::unexpected(InternalFailure("top-N candidate has no descriptor"));
        }
        auto checked = ConvertProgramResult(
            AssumeValue(builder_).EmitCheckTopN(AssumeValue(read_registers_->top_n), record_first,
                                                key_count, rejected),
            "unable to emit top-N admission");
        if (!checked.has_value()) {
          return std::unexpected(std::move(checked.error()));
        }
      }
      if (auto payload = EmitOrderingPayload(); !payload.has_value()) {
        return payload;
      }
    }

    if (read_registers_->ordering_runtime == OrderingRuntime::kTopN) {
      return Append(InsertTopNInstruction{
          .top_n = AssumeValue(read_registers_->top_n),
          .first_value = record_first,
          .value_count = value_count,
      });
    }
    if (read_registers_->ordering_runtime != OrderingRuntime::kSorter ||
        !read_registers_->sorter.has_value()) {
      return std::unexpected(InternalFailure("ordered candidate has no active capability"));
    }
    return Append(InsertSorterInstruction{
        .sorter = AssumeValue(read_registers_->sorter),
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
    if ((grouped_aggregate_query_ && read_registers_->active_aggregate_output.has_value()) ||
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
    if (!HasOrdering()) {
      return Append(HaltInstruction{});
    }
    if (HasBlockOrdering()) {
      if (auto drained = EmitOrderBlockDrain(); !drained.has_value()) {
        return drained;
      }
      if (auto closed = CloseOrderBlock(); !closed.has_value()) {
        return closed;
      }
      return Append(HaltInstruction{});
    }
    const auto output_fields = BlockOutputFields();
    if ((active_core_order_ == nullptr && wrapper_order_layout_ == nullptr && output_ == nullptr) ||
        output_fields.size() != bound_select_->result_columns().size() ||
        read_registers_->ordering_runtime == OrderingRuntime::kNone) {
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
    if (HasLimit()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
    }
    ProgramResult<InstructionAddress> rewind_result =
        read_registers_->ordering_runtime == OrderingRuntime::kSorter
            ? AssumeValue(builder_).EmitRewindSorter(AssumeValue(read_registers_->sorter), *empty)
            : AssumeValue(builder_).EmitRewindTopN(AssumeValue(read_registers_->top_n), *empty);
    auto rewound = ConvertProgramResult(std::move(rewind_result), "unable to emit ordering rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (HasLimit()) {
      if (auto offset = EmitOffset(*advance, true); !offset.has_value()) {
        return offset;
      }
    }
    for (std::size_t index = 0; index < output_fields.size(); ++index) {
      const SortOutputField& output = output_fields[index];
      const std::uint32_t field =
          output.kind == SortOutputFieldKind::kKey
              ? output.field_index
              : static_cast<std::uint32_t>(RequestedOrderTerms().size()) + output.field_index;
      const Instruction read =
          read_registers_->ordering_runtime == OrderingRuntime::kSorter
              ? Instruction{ReadSorterFieldInstruction{
                    .sorter = AssumeValue(read_registers_->sorter),
                    .field = field,
                    .output = RegisterId{read_registers_->result_block_first.value() +
                                         static_cast<std::uint32_t>(index)},
                }}
              : Instruction{ReadTopNFieldInstruction{
                    .top_n = AssumeValue(read_registers_->top_n),
                    .field = field,
                    .output = RegisterId{read_registers_->result_block_first.value() +
                                         static_cast<std::uint32_t>(index)},
                }};
      if (auto read_result = Append(read); !read_result.has_value()) {
        return read_result;
      }
    }
    if (auto result = Append(ResultRowInstruction{
            .first = read_registers_->result_block_first,
            .count = static_cast<std::uint32_t>(output_fields.size()),
        });
        !result.has_value()) {
      return result;
    }
    if (HasLimit()) {
      if (!read_registers_->negative_limit_register.has_value() ||
          !read_registers_->limit_register.has_value() ||
          !read_registers_->one_register.has_value()) {
        return std::unexpected(InternalFailure("ordered LIMIT drain has no counters"));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                   JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->one_register),
              .output = AssumeValue(read_registers_->limit_register),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->limit_register),
                                   JumpCondition::kIfFalse, AssumeValue(positioned_completion));
          !jumped.has_value()) {
        return jumped;
      }
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    ProgramResult<InstructionAddress> next_result =
        read_registers_->ordering_runtime == OrderingRuntime::kSorter
            ? AssumeValue(builder_).EmitNextSorter(AssumeValue(read_registers_->sorter), *row)
            : AssumeValue(builder_).EmitNextTopN(AssumeValue(read_registers_->top_n), *row);
    auto next = ConvertProgramResult(std::move(next_result), "unable to emit ordering advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    const Instruction close =
        read_registers_->ordering_runtime == OrderingRuntime::kSorter
            ? Instruction{CloseSorterInstruction{.sorter = AssumeValue(read_registers_->sorter)}}
            : Instruction{CloseTopNInstruction{.top_n = AssumeValue(read_registers_->top_n)}};
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
          read_registers_->ordering_runtime == OrderingRuntime::kSorter
              ? Instruction{CloseSorterInstruction{.sorter = AssumeValue(read_registers_->sorter)}}
              : Instruction{CloseTopNInstruction{.top_n = AssumeValue(read_registers_->top_n)}};
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
    if (HasOrdering()) {
      return EmitOrderedCandidate(rejected);
    }
    if (sort_ != nullptr) {
      const auto* core = std::get_if<BoundSelectCore>(&bound_select_->query_cores().front());
      if (core == nullptr) {
        return std::unexpected(InternalFailure("input-ordered projection has no SELECT core"));
      }
      if (auto projected = EmitStreamingProjection(*core); !projected.has_value()) {
        return projected;
      }
    } else {
      const LogicalNode& logical_projection = logical_plan_->node(projection_->logical_projection);
      const auto* projection = std::get_if<LogicalProjectionNode>(&logical_projection.payload);
      if (projection == nullptr ||
          projection->expressions.size() != bound_select_->result_columns().size()) {
        return std::unexpected(InternalFailure("logical projection shape is invalid"));
      }
      for (std::size_t index = 0; index < projection->expressions.size(); ++index) {
        if (auto emitted =
                EmitProjectionValue(projection->expressions[index],
                                    RegisterId{read_registers_->result_block_first.value() +
                                               static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return emitted;
        }
      }
    }
    return Append(ResultRowInstruction{
        .first = read_registers_->result_block_first,
        .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
    });
  }

  [[nodiscard]] bool NeedsPreopenCompletion() const noexcept {
    return guard_ != nullptr ||
           (grouped_aggregate_query_ && filter_ != nullptr && !filter_->guards.empty()) ||
           (HasLimit() && !read_registers_->limit_initialized);
  }

  [[nodiscard]] bool AppliesLimitToSourceRows() const noexcept {
    return !grouped_aggregate_query_ && HasLimit() && !HasOrdering();
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

  [[nodiscard]] LoweringResult<void> EmitIndexKeyExpression(
      BoundExpressionId expression, bool reject_null, std::optional<Label> null_target,
      std::optional<CursorId> positioned_cursor = std::nullopt) {
    const RegisterId home = Home(expression);
    if (auto emitted = EmitExpression(expression, home); !emitted.has_value()) {
      return emitted;
    }
    if (reject_null) {
      if (!null_target.has_value()) {
        return std::unexpected(InternalFailure("NULL-rejecting index key has no completion label"));
      }
      if (positioned_cursor.has_value()) {
        auto accepted = CreateLabel();
        if (!accepted.has_value()) {
          return std::unexpected(std::move(accepted.error()));
        }
        if (auto jumped = EmitJumpIf(home, JumpCondition::kIfNotNull, *accepted);
            !jumped.has_value()) {
          return jumped;
        }
        // The rejection edge joins seek misses, which have no positioned row.
        if (auto cleared =
                Append(NullRowInstruction{.cursor = *positioned_cursor, .enabled = false});
            !cleared.has_value()) {
          return cleared;
        }
        if (auto jumped = EmitJump(*null_target); !jumped.has_value()) {
          return jumped;
        }
        return BindLabel(*accepted);
      }
      return EmitJumpIf(home, JumpCondition::kIfNull, *null_target);
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexEqualityExpressions(
      std::span<const PhysicalIndexEquality> equalities, std::optional<Label> null_target) {
    for (const auto& equality : equalities) {
      if (auto emitted = EmitIndexKeyExpression(equality.key, equality.reject_null, null_target);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitIndexKeyExpressions(
      const PhysicalIndexScanNode& index, const IndexTraversal& traversal, IndexKeyPhase phase,
      std::optional<Label> null_target,
      std::optional<CursorId> positioned_cursor = std::nullopt) override {
    if (phase == IndexKeyPhase::start) {
      if (auto equalities = EmitIndexEqualityExpressions(index.equalities, null_target);
          !equalities.has_value()) {
        return equalities;
      }
    }
    const auto& bound = phase == IndexKeyPhase::start ? traversal.start : traversal.end;
    if (phase == IndexKeyPhase::end && bound.expression != nullptr &&
        !positioned_cursor.has_value()) {
      return std::unexpected(InternalFailure("range-end key has no positioned cursor"));
    }
    return bound.expression != nullptr
               ? EmitIndexKeyExpression(bound.expression->key, true, null_target, positioned_cursor)
               : LoweringResult<void>{};
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexKeyBlock(
      const PhysicalIndexScanNode& index, IndexTraversalBound range_bound,
      IndexKeyPhase phase = IndexKeyPhase::start) override {
    if (!read_registers_->index_key_first.has_value()) {
      return std::unexpected(InternalFailure("covering index key register block is invalid"));
    }
    return EmitIndexKeyBlock(index, range_bound, *read_registers_->index_key_first,
                             read_registers_->index_key_capacity, phase);
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexKeyBlock(
      const PhysicalIndexScanNode& index, IndexTraversalBound range_bound, RegisterId first,
      std::uint32_t capacity, IndexKeyPhase phase = IndexKeyPhase::start) {
    return EmitIndexKeyBlock(index.equalities, index.range.has_value() ? &*index.range : nullptr,
                             range_bound, first, capacity, phase);
  }

  [[nodiscard]] LoweringResult<std::uint32_t> EmitIndexKeyBlock(
      std::span<const PhysicalIndexEquality> equalities, const PhysicalIndexRange* range,
      IndexTraversalBound range_bound, RegisterId first, std::uint32_t capacity,
      IndexKeyPhase phase = IndexKeyPhase::start) {
    if ((range_bound.expression != nullptr && range_bound.implicit_null) ||
        (range_bound.expression != nullptr && range == nullptr)) {
      return std::unexpected(InternalFailure("covering index range boundary is invalid"));
    }
    const std::size_t count =
        equalities.size() + static_cast<std::size_t>(range_bound.has_range_term());
    if (count == 0U || count > capacity) {
      return std::unexpected(InternalFailure("covering index key register block is invalid"));
    }
    std::size_t offset = phase == IndexKeyPhase::start ? 0U : equalities.size();
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
    if (phase == IndexKeyPhase::start) {
      for (const PhysicalIndexEquality& equality : equalities) {
        if (auto copied = copy_term(equality.key, equality.probe_affinity); !copied.has_value()) {
          return std::unexpected(std::move(copied.error()));
        }
      }
    }
    if (range_bound.expression != nullptr) {
      if (auto copied =
              copy_term(range_bound.expression->key, range_bound.expression->probe_affinity);
          !copied.has_value()) {
        return std::unexpected(std::move(copied.error()));
      }
    } else if (range_bound.implicit_null) {
      auto null = EnsureNullConstant();
      if (!null.has_value()) {
        return std::unexpected(std::move(null.error()));
      }
      const RegisterId destination{first.value() + static_cast<std::uint32_t>(offset)};
      if (auto loaded = Append(LoadConstantInstruction{.constant = *null, .output = destination});
          !loaded.has_value()) {
        return std::unexpected(std::move(loaded.error()));
      }
    }
    return static_cast<std::uint32_t>(count);
  }

  [[nodiscard]] LoweringResult<IndexNullScan> CreateIndexNullScan(const IndexTraversal& traversal,
                                                                  std::optional<RegisterId> pass) {
    if (!pass.has_value()) {
      return std::unexpected(InternalFailure("two-pass index ordering has no pass register"));
    }
    auto second = CreateLabel();
    auto exhausted = CreateLabel();
    auto zero = EnsureZeroConstant();
    if (!second.has_value()) {
      return std::unexpected(std::move(second.error()));
    }
    if (!exhausted.has_value()) {
      return std::unexpected(std::move(exhausted.error()));
    }
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    if (auto initialized = Append(LoadConstantInstruction{.constant = *zero, .output = *pass});
        !initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    return IndexNullScan{
        .pass = *pass,
        .second_pass = *second,
        .exhausted = *exhausted,
        .nulls_first = traversal.nulls_first,
    };
  }

  [[nodiscard]] LoweringResult<void> EmitIndexNullScanStart(const PhysicalIndexScanNode& index,
                                                            const IndexNullScan& scan,
                                                            CursorId cursor, RegisterId first,
                                                            std::uint32_t count) {
    if (count == 0U) {
      return std::unexpected(InternalFailure("two-pass index ordering has no NULL key"));
    }
    auto positioned = CreateLabel();
    auto one = EnsureOneConstant();
    auto zero = EnsureZeroConstant();
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }
    if (!one.has_value()) {
      return std::unexpected(std::move(one.error()));
    }
    if (!zero.has_value()) {
      return std::unexpected(std::move(zero.error()));
    }
    if (auto initialized = Append(LoadConstantInstruction{.constant = *one, .output = scan.pass});
        !initialized.has_value()) {
      return initialized;
    }
    auto sought = ConvertProgramResult(
        AssumeValue(builder_).EmitSeekIndex(
            cursor, first, count, scan.exhausted,
            lowering_internal::ResolveIndexSeekMode(index.direction, scan.nulls_first)),
        "unable to position the first NULL-order partition");
    if (!sought.has_value()) {
      return std::unexpected(std::move(sought.error()));
    }
    if (auto jumped = EmitJump(*positioned); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(scan.second_pass); !bound.has_value()) {
      return bound;
    }
    if (auto selected = Append(LoadConstantInstruction{.constant = *zero, .output = scan.pass});
        !selected.has_value()) {
      return selected;
    }
    ProgramResult<InstructionAddress> second =
        count > 1U ? AssumeValue(builder_).EmitSeekIndex(
                         cursor, first, count - 1U, scan.exhausted,
                         lowering_internal::ResolveIndexSeekMode(index.direction, true))
                   : (index.direction == PhysicalScanDirection::kReverse
                          ? AssumeValue(builder_).EmitLast(cursor, scan.exhausted)
                          : AssumeValue(builder_).EmitRewind(cursor, scan.exhausted));
    auto restarted = ConvertProgramResult(std::move(second),
                                          "unable to position the second NULL-order partition");
    if (!restarted.has_value()) {
      return std::unexpected(std::move(restarted.error()));
    }
    return BindLabel(*positioned);
  }

  [[nodiscard]] LoweringResult<void> EmitIndexNullScanEndCheck(const PhysicalIndexScanNode& index,
                                                               const IndexNullScan& scan,
                                                               CursorId cursor, RegisterId first,
                                                               std::uint32_t count) {
    if (count == 0U) {
      return std::unexpected(InternalFailure("two-pass index ordering has no NULL end key"));
    }
    auto second = CreateLabel();
    auto accepted = CreateLabel();
    if (!second.has_value()) {
      return std::unexpected(std::move(second.error()));
    }
    if (!accepted.has_value()) {
      return std::unexpected(std::move(accepted.error()));
    }
    const auto check = [&](std::uint32_t fields, bool inclusive) -> LoweringResult<void> {
      if (fields == 0U) {
        return {};
      }
      auto checked = ConvertProgramResult(
          AssumeValue(builder_).EmitCheckIndexRange(
              cursor, first, fields, scan.exhausted,
              lowering_internal::ResolveIndexRangeEndMode(index.direction, inclusive)),
          "unable to check a NULL-order partition boundary");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
      return {};
    };
    if (auto selected = EmitJumpIf(scan.pass, JumpCondition::kIfFalse, *second);
        !selected.has_value()) {
      return selected;
    }
    if (auto checked = check(scan.nulls_first ? count : count - 1U, true); !checked.has_value()) {
      return checked;
    }
    if (auto jumped = EmitJump(*accepted); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*second); !bound.has_value()) {
      return bound;
    }
    if (auto checked = check(count, !scan.nulls_first); !checked.has_value()) {
      return checked;
    }
    return BindLabel(*accepted);
  }

  [[nodiscard]] LoweringResult<void> EmitIndexNullScanCompletion(
      const IndexNullScan& scan, CursorId cursor, std::optional<CursorId> table_cursor,
      Label completion) {
    if (auto bound = BindLabel(scan.exhausted); !bound.has_value()) {
      return bound;
    }
    if (table_cursor.has_value()) {
      if (auto cleared = Append(NullRowInstruction{.cursor = *table_cursor, .enabled = false});
          !cleared.has_value()) {
        return cleared;
      }
    }
    if (auto cleared = Append(NullRowInstruction{.cursor = cursor, .enabled = false});
        !cleared.has_value()) {
      return cleared;
    }
    if (auto finished = EmitJumpIf(scan.pass, JumpCondition::kIfFalse, completion);
        !finished.has_value()) {
      return finished;
    }
    return EmitJump(scan.second_pass);
  }

  [[nodiscard]] LoweringResult<void> EmitCoveringIndexScan(const PhysicalIndexScanNode& index) {
    if (!read_registers_->cursor.has_value()) {
      return std::unexpected(InternalFailure("covering index scan has no cursor"));
    }
    auto resolved_traversal = ResolveIndexTraversal(index, *bound_select_->catalog());
    if (!resolved_traversal.has_value()) {
      return std::unexpected(std::move(resolved_traversal.error()));
    }
    const IndexTraversal& traversal = *resolved_traversal;
    const bool reverse = index.direction == PhysicalScanDirection::kReverse;
    const bool has_start_key = traversal.has_start_key;
    const bool has_end_key = traversal.has_end_key;
    const bool rejects_null = std::ranges::any_of(index.equalities,
                                                  [](const PhysicalIndexEquality& equality) {
                                                    return equality.reject_null;
                                                  }) ||
                              traversal.start.expression != nullptr;

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
    std::optional<IndexNullScan> null_scan;
    if (index.two_pass_null_order) {
      auto created = CreateIndexNullScan(traversal, read_registers_->index_null_pass);
      if (!created.has_value()) {
        return std::unexpected(std::move(created.error()));
      }
      null_scan = *created;
    }
    if (!index.equalities.empty() || index.range.has_value()) {
      if (auto keys =
              EmitIndexKeyExpressions(index, traversal, IndexKeyPhase::start, closed_completion);
          !keys.has_value()) {
        return keys;
      }
    }

    std::uint32_t start_key_count = 0;
    if (has_start_key) {
      auto key = EmitIndexKeyBlock(index, traversal.start);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      start_key_count = *key;
    }
    if (auto opened =
            Append(OpenReadCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
    if (null_scan.has_value()) {
      if (auto started = EmitIndexNullScanStart(
              index, *null_scan, AssumeValue(read_registers_->cursor),
              AssumeValue(read_registers_->index_key_first), start_key_count);
          !started.has_value()) {
        return started;
      }
    } else if (has_start_key) {
      auto sought =
          ConvertProgramResult(EmitIndexTraversalSeek(AssumeValue(builder_), index, traversal,
                                                      AssumeValue(read_registers_->cursor),
                                                      AssumeValue(read_registers_->index_key_first),
                                                      start_key_count, *unpositioned_completion),
                               "unable to emit covering index seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      ProgramResult<InstructionAddress> positioned =
          reverse ? AssumeValue(builder_).EmitLast(AssumeValue(read_registers_->cursor),
                                                   *unpositioned_completion)
                  : AssumeValue(builder_).EmitRewind(AssumeValue(read_registers_->cursor),
                                                     *unpositioned_completion);
      auto rewound =
          ConvertProgramResult(std::move(positioned), "unable to emit covering index positioning");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }

    std::uint32_t end_key_count = 0;
    if (has_end_key) {
      if (auto keys = EmitIndexKeyExpressions(index, traversal, IndexKeyPhase::end,
                                              *unpositioned_completion, read_registers_->cursor);
          !keys.has_value()) {
        return keys;
      }
      auto key = EmitIndexKeyBlock(index, traversal.end, IndexKeyPhase::end);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      end_key_count = *key;
    }

    std::optional<Label> positioned_completion;
    if (AppliesLimitToSourceRows()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
    }
    if (auto bound = BindLabel(*row_loop); !bound.has_value()) {
      return bound;
    }
    if (null_scan.has_value()) {
      if (auto checked = EmitIndexNullScanEndCheck(
              index, *null_scan, AssumeValue(read_registers_->cursor),
              AssumeValue(read_registers_->index_key_first), end_key_count);
          !checked.has_value()) {
        return checked;
      }
    } else if (has_end_key) {
      auto checked = ConvertProgramResult(
          EmitIndexTraversalEndCheck(AssumeValue(builder_), index, traversal,
                                     AssumeValue(read_registers_->cursor),
                                     AssumeValue(read_registers_->index_key_first), end_key_count,
                                     *unpositioned_completion),
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
      if (!positioned_completion.has_value() ||
          !read_registers_->negative_limit_register.has_value() ||
          !read_registers_->limit_register.has_value() ||
          !read_registers_->one_register.has_value()) {
        return std::unexpected(InternalFailure("covering index LIMIT has no loop registers"));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                   JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->one_register),
              .output = AssumeValue(read_registers_->limit_register),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->limit_register),
                                   JumpCondition::kIfFalse, *positioned_completion);
          !jumped.has_value()) {
        return jumped;
      }
    }

    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    ProgramResult<InstructionAddress> advanced =
        reverse
            ? AssumeValue(builder_).EmitPrevious(AssumeValue(read_registers_->cursor), *row_loop)
            : AssumeValue(builder_).EmitNext(AssumeValue(read_registers_->cursor), *row_loop);
    auto next = ConvertProgramResult(std::move(advanced), "unable to emit covering index advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (null_scan.has_value()) {
      if (auto completed =
              EmitIndexNullScanCompletion(*null_scan, AssumeValue(read_registers_->cursor),
                                          std::nullopt, *unpositioned_completion);
          !completed.has_value()) {
        return completed;
      }
    }
    if (auto bound = BindLabel(*unpositioned_completion); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
      return completed;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
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
    if (index.covering || !read_registers_->cursor.has_value() ||
        !read_registers_->index_cursor.has_value() ||
        !read_registers_->index_rowid_register.has_value()) {
      return std::unexpected(InternalFailure("noncovering index scan resources are incomplete"));
    }
    const CursorId index_cursor = AssumeValue(read_registers_->index_cursor);
    const CursorId table_cursor = AssumeValue(read_registers_->cursor);
    auto resolved_traversal = ResolveIndexTraversal(index, *bound_select_->catalog());
    if (!resolved_traversal.has_value()) {
      return std::unexpected(std::move(resolved_traversal.error()));
    }
    const IndexTraversal& traversal = *resolved_traversal;
    const bool reverse = index.direction == PhysicalScanDirection::kReverse;
    const bool has_start_key = traversal.has_start_key;
    const bool has_end_key = traversal.has_end_key;
    const bool rejects_null = std::ranges::any_of(index.equalities,
                                                  [](const PhysicalIndexEquality& equality) {
                                                    return equality.reject_null;
                                                  }) ||
                              traversal.start.expression != nullptr;

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
    std::optional<IndexNullScan> null_scan;
    if (index.two_pass_null_order) {
      auto created = CreateIndexNullScan(traversal, read_registers_->index_null_pass);
      if (!created.has_value()) {
        return std::unexpected(std::move(created.error()));
      }
      null_scan = *created;
    }
    if (auto keys =
            EmitIndexKeyExpressions(index, traversal, IndexKeyPhase::start, closed_completion);
        !keys.has_value()) {
      return keys;
    }

    std::uint32_t start_key_count = 0;
    if (has_start_key) {
      auto key = EmitIndexKeyBlock(index, traversal.start);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      start_key_count = *key;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = index_cursor});
        !opened.has_value()) {
      return opened;
    }
    if (null_scan.has_value()) {
      if (auto opened = Append(OpenReadCursorInstruction{.cursor = table_cursor});
          !opened.has_value()) {
        return opened;
      }
    }

    auto index_exhausted = CreateLabel();
    if (!index_exhausted.has_value()) {
      return std::unexpected(std::move(index_exhausted.error()));
    }
    auto index_missing = null_scan.has_value() ? index_exhausted : CreateLabel();
    if (!index_missing.has_value()) {
      return std::unexpected(std::move(index_missing.error()));
    }
    if (null_scan.has_value()) {
      if (auto started = EmitIndexNullScanStart(index, *null_scan, index_cursor,
                                                AssumeValue(read_registers_->index_key_first),
                                                start_key_count);
          !started.has_value()) {
        return started;
      }
    } else if (has_start_key) {
      auto sought = ConvertProgramResult(
          EmitIndexTraversalSeek(AssumeValue(builder_), index, traversal, index_cursor,
                                 AssumeValue(read_registers_->index_key_first), start_key_count,
                                 *index_missing),
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
      if (auto keys = EmitIndexKeyExpressions(index, traversal, IndexKeyPhase::end, *index_missing,
                                              index_cursor);
          !keys.has_value()) {
        return keys;
      }
      auto key = EmitIndexKeyBlock(index, traversal.end, IndexKeyPhase::end);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      end_key_count = *key;
    }

    std::optional<Label> first_end;
    std::optional<Label> positioned_completion;
    if (has_end_key && !null_scan.has_value()) {
      auto first = CreateLabel();
      if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
      }
      first_end = *first;
    }
    if (AppliesLimitToSourceRows()) {
      auto positioned = CreateLabel();
      if (!positioned.has_value()) {
        return std::unexpected(std::move(positioned.error()));
      }
      positioned_completion = *positioned;
    }

    auto subsequent_candidate = CreateLabel();
    auto process_candidate = CreateLabel();
    auto advance = CreateLabel();
    if (!subsequent_candidate.has_value()) {
      return std::unexpected(std::move(subsequent_candidate.error()));
    }
    if (!process_candidate.has_value()) {
      return std::unexpected(std::move(process_candidate.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }

    if (null_scan.has_value()) {
      if (auto jumped = EmitJump(*subsequent_candidate); !jumped.has_value()) {
        return jumped;
      }
    } else {
      if (has_end_key) {
        auto checked = ConvertProgramResult(
            EmitIndexTraversalEndCheck(AssumeValue(builder_), index, traversal, index_cursor,
                                       AssumeValue(read_registers_->index_key_first), end_key_count,
                                       AssumeValue(first_end)),
            "unable to emit first noncovering index end check");
        if (!checked.has_value()) {
          return std::unexpected(std::move(checked.error()));
        }
      }
      if (auto sought =
              EmitIndexTableLookup(AssumeValue(builder_), index_cursor, table_cursor,
                                   AssumeValue(read_registers_->index_rowid_register), true);
          !sought.has_value()) {
        return sought;
      }
      if (auto jumped = EmitJump(*process_candidate); !jumped.has_value()) {
        return jumped;
      }
    }

    if (auto bound = BindLabel(*subsequent_candidate); !bound.has_value()) {
      return bound;
    }
    if (null_scan.has_value()) {
      if (auto checked = EmitIndexNullScanEndCheck(index, *null_scan, index_cursor,
                                                   AssumeValue(read_registers_->index_key_first),
                                                   end_key_count);
          !checked.has_value()) {
        return checked;
      }
    } else if (has_end_key) {
      auto checked = ConvertProgramResult(
          EmitIndexTraversalEndCheck(AssumeValue(builder_), index, traversal, index_cursor,
                                     AssumeValue(read_registers_->index_key_first), end_key_count,
                                     *index_exhausted),
          "unable to emit noncovering index end check");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (auto sought = EmitIndexTableLookup(AssumeValue(builder_), index_cursor, table_cursor,
                                           AssumeValue(read_registers_->index_rowid_register));
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
      if (!positioned_completion.has_value() ||
          !read_registers_->negative_limit_register.has_value() ||
          !read_registers_->limit_register.has_value() ||
          !read_registers_->one_register.has_value()) {
        return std::unexpected(InternalFailure("noncovering index LIMIT has no loop registers"));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                   JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->one_register),
              .output = AssumeValue(read_registers_->limit_register),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->limit_register),
                                   JumpCondition::kIfFalse, AssumeValue(positioned_completion));
          !jumped.has_value()) {
        return jumped;
      }
    }

    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    if (null_scan.has_value()) {
      if (auto cleared = Append(NullRowInstruction{.cursor = table_cursor, .enabled = false});
          !cleared.has_value()) {
        return cleared;
      }
    }
    ProgramResult<InstructionAddress> advanced =
        reverse ? AssumeValue(builder_).EmitPrevious(index_cursor, *subsequent_candidate)
                : AssumeValue(builder_).EmitNext(index_cursor, *subsequent_candidate);
    auto next =
        ConvertProgramResult(std::move(advanced), "unable to emit noncovering index advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (null_scan.has_value()) {
      if (auto completed =
              EmitIndexNullScanCompletion(*null_scan, index_cursor, table_cursor, *index_exhausted);
          !completed.has_value()) {
        return completed;
      }
    }
    if (auto bound = BindLabel(*index_exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(read_registers_->cursor, read_registers_->index_cursor);
        !completed.has_value()) {
      return completed;
    }
    if (first_end.has_value()) {
      if (auto bound = BindLabel(*first_end); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(std::nullopt, read_registers_->index_cursor);
          !completed.has_value()) {
        return completed;
      }
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed =
              EmitReadCompletion(read_registers_->cursor, read_registers_->index_cursor);
          !completed.has_value()) {
        return completed;
      }
    }
    if (!null_scan.has_value()) {
      if (auto bound = BindLabel(*index_missing); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(std::nullopt, read_registers_->index_cursor);
          !completed.has_value()) {
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

  [[nodiscard]] LoweringResult<void> EmitTableScan() {
    if (!read_registers_->cursor.has_value()) {
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
    if (auto opened =
            Append(OpenReadCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
            ? AssumeValue(builder_).EmitRewind(AssumeValue(read_registers_->cursor),
                                               AssumeValue(empty))
            : AssumeValue(builder_).EmitLast(AssumeValue(read_registers_->cursor),
                                             AssumeValue(empty));
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
      if (!read_registers_->negative_limit_register.has_value() ||
          !read_registers_->limit_register.has_value() ||
          !read_registers_->one_register.has_value()) {
        return std::unexpected(InternalFailure("table LIMIT has no loop registers"));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                   JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->one_register),
              .output = AssumeValue(read_registers_->limit_register),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->limit_register),
                                   JumpCondition::kIfFalse, AssumeValue(positioned_completion));
          !jumped.has_value()) {
        return jumped;
      }
    }

    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    ProgramResult<InstructionAddress> advanced =
        table_scan_->direction == PhysicalScanDirection::kForward
            ? AssumeValue(builder_).EmitNext(AssumeValue(read_registers_->cursor),
                                             AssumeValue(row_loop))
            : AssumeValue(builder_).EmitPrevious(AssumeValue(read_registers_->cursor),
                                                 AssumeValue(row_loop));
    auto next = ConvertProgramResult(std::move(advanced), "unable to emit table advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
      return completed;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
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
    if (!read_registers_->cursor.has_value()) {
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
    if (auto opened =
            Append(OpenReadCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
    auto sought =
        ConvertProgramResult(AssumeValue(builder_).EmitSeekRowId(
                                 AssumeValue(read_registers_->cursor), *key, AssumeValue(missing)),
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
    if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
      return completed;
    }
    if (auto bound = BindLabel(*missing); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
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

  [[nodiscard]] LoweringResult<void> EmitRowIdRange(const PhysicalRowIdRangeNode& range) {
    if (!read_registers_->cursor.has_value()) {
      return std::unexpected(InternalFailure("rowid range has no cursor"));
    }
    auto resolved = lowering_internal::ResolveRowIdTraversal(range);
    if (!resolved.has_value()) {
      return std::unexpected(std::move(resolved.error()));
    }
    const lowering_internal::RowIdTraversal& traversal = *resolved;
    if (traversal.end != nullptr &&
        (!read_registers_->rowid_range_rowid_register.has_value() ||
         !read_registers_->rowid_range_comparison_register.has_value())) {
      return std::unexpected(InternalFailure("rowid range end resources are incomplete"));
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
    if (auto opened =
            Append(OpenReadCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
        !opened.has_value()) {
      return opened;
    }

    auto unpositioned = CreateLabel();
    auto row_loop = CreateLabel();
    auto advance = CreateLabel();
    if (!unpositioned.has_value()) {
      return std::unexpected(std::move(unpositioned.error()));
    }
    if (!row_loop.has_value()) {
      return std::unexpected(std::move(row_loop.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (traversal.start != nullptr) {
      const RegisterId key = Home(traversal.start->key);
      if (auto emitted = EmitExpression(traversal.start->key, key); !emitted.has_value()) {
        return emitted;
      }
      auto sought = ConvertProgramResult(
          AssumeValue(builder_).EmitSeekRowId(AssumeValue(read_registers_->cursor), key,
                                              *unpositioned, traversal.start_mode),
          "unable to emit rowid range seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      ProgramResult<InstructionAddress> positioned =
          traversal.reverse
              ? AssumeValue(builder_).EmitLast(AssumeValue(read_registers_->cursor), *unpositioned)
              : AssumeValue(builder_).EmitRewind(AssumeValue(read_registers_->cursor),
                                                 *unpositioned);
      auto rewound =
          ConvertProgramResult(std::move(positioned), "unable to emit rowid range positioning");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }

    std::optional<Label> positioned_end;
    std::optional<RegisterId> end_key;
    if (traversal.end != nullptr) {
      const RegisterId key = Home(traversal.end->key);
      if (auto emitted = EmitExpression(traversal.end->key, key); !emitted.has_value()) {
        return emitted;
      }
      end_key = key;
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_end = *label;
    }
    if (auto bound = BindLabel(*row_loop); !bound.has_value()) {
      return bound;
    }
    if (positioned_end.has_value()) {
      auto binary = EnsureBinarySymbol();
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      if (auto checked = EmitRowIdTraversalEndCheck(
              AssumeValue(builder_), traversal, AssumeValue(read_registers_->cursor),
              AssumeValue(end_key), AssumeValue(read_registers_->rowid_range_rowid_register),
              AssumeValue(read_registers_->rowid_range_comparison_register), *binary,
              *positioned_end);
          !checked.has_value()) {
        return checked;
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

    std::optional<Label> limit_completion;
    if (AppliesLimitToSourceRows()) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      limit_completion = *label;
      if (!read_registers_->negative_limit_register.has_value() ||
          !read_registers_->limit_register.has_value() ||
          !read_registers_->one_register.has_value()) {
        return std::unexpected(InternalFailure("rowid range LIMIT has no loop registers"));
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                   JumpCondition::kIfTrue, *advance);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto decremented = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->one_register),
              .output = AssumeValue(read_registers_->limit_register),
          });
          !decremented.has_value()) {
        return decremented;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->limit_register),
                                   JumpCondition::kIfFalse, *limit_completion);
          !jumped.has_value()) {
        return jumped;
      }
    }

    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    ProgramResult<InstructionAddress> advanced =
        traversal.reverse
            ? AssumeValue(builder_).EmitPrevious(AssumeValue(read_registers_->cursor), *row_loop)
            : AssumeValue(builder_).EmitNext(AssumeValue(read_registers_->cursor), *row_loop);
    auto next = ConvertProgramResult(std::move(advanced), "unable to emit rowid range advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*unpositioned); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
      return completed;
    }
    if (positioned_end.has_value()) {
      if (auto bound = BindLabel(*positioned_end); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
        return completed;
      }
    }
    if (limit_completion.has_value()) {
      if (auto bound = BindLabel(*limit_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(read_registers_->cursor); !completed.has_value()) {
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
    if (!read_registers_->cursor.has_value() || !source_rowid_register_.has_value() ||
        source_snapshot_registers_.size() != source_columns_.size() ||
        source_field_real_affinity_.size() != source_snapshot_registers_.size()) {
      return std::unexpected(InternalFailure("mutation source snapshot metadata is incomplete"));
    }
    if (auto rowid = Append(ReadRowIdInstruction{
            .cursor = AssumeValue(read_registers_->cursor),
            .output = AssumeValue(source_rowid_register_),
        });
        !rowid.has_value()) {
      return rowid;
    }
    for (std::size_t index = 0; index < source_snapshot_registers_.size(); ++index) {
      const RegisterId destination = source_snapshot_registers_[index];
      if (auto field = Append(ReadFieldInstruction{
              .cursor = AssumeValue(read_registers_->cursor),
              .field = CursorFieldId{static_cast<std::uint32_t>(index)},
              .output = destination,
          });
          !field.has_value()) {
        return field;
      }
    }
    if (close_cursor) {
      return Append(CloseCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      if (index_cursor.target_index >= target.indexes.size()) {
        return std::unexpected(InternalFailure("DELETE index cursor metadata is incomplete"));
      }
      auto key = EmitMutationIndexKey(target, target.indexes[index_cursor.target_index],
                                      MutationIndexValueRegisters{
                                          .first_values = std::nullopt,
                                          .rowid = AssumeValue(source_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (auto deleted = Append(DeleteIndexInstruction{
              .cursor = index_cursor.cursor,
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
    const TableRecordAffinityMode affinity_mode =
        index_write_cursors_.empty() ? TableRecordAffinityMode::kApplyColumnAffinity
                                     : TableRecordAffinityMode::kAffinityPreapplied;
    if (!index_write_cursors_.empty()) {
      if (auto applied = EmitMutationValueAffinity(target, AssumeValue(update_values_first_));
          !applied.has_value()) {
        return applied;
      }
    }
    return Append(BuildTableRecordInstruction{
        .cursor = AssumeValue(write_cursor_),
        .first_value = AssumeValue(update_values_first_),
        .value_count = static_cast<std::uint32_t>(target.columns.size()),
        .output = AssumeValue(update_record_register_),
        .affinity_mode = affinity_mode,
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
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      if (index_cursor.target_index >= target.indexes.size()) {
        return std::unexpected(InternalFailure("UPDATE index cursor metadata is incomplete"));
      }
      const BoundIndexMaintenance& maintenance = target.indexes[index_cursor.target_index];
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
                .cursor = index_cursor.cursor,
                .first_key = AssumeValue(mutation_index_key_first_),
                .key_count = maintenance.key_term_count,
                .ignored_rowid = AssumeValue(source_rowid_register_),
            });
            !checked.has_value()) {
          return checked;
        }
      }
    }
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      const BoundIndexMaintenance& maintenance = target.indexes[index_cursor.target_index];
      auto key = EmitMutationIndexKey(target, maintenance,
                                      MutationIndexValueRegisters{
                                          .first_values = std::nullopt,
                                          .rowid = AssumeValue(source_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (auto deleted = Append(DeleteIndexInstruction{
              .cursor = index_cursor.cursor,
              .first_value = AssumeValue(mutation_index_key_first_),
              .value_count = *key,
          });
          !deleted.has_value()) {
        return deleted;
      }
    }
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      const BoundIndexMaintenance& maintenance = target.indexes[index_cursor.target_index];
      auto key = EmitMutationIndexKey(target, maintenance,
                                      MutationIndexValueRegisters{
                                          .first_values = AssumeValue(update_values_first_),
                                          .rowid = AssumeValue(update_new_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (auto inserted = Append(InsertIndexInstruction{
              .cursor = index_cursor.cursor,
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
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      if (auto opened = Append(OpenWriteCursorInstruction{.cursor = index_cursor.cursor});
          !opened.has_value()) {
        return opened;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> CloseMutationWriteCursors() {
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      if (auto closed = Append(CloseWriteCursorInstruction{.cursor = index_cursor.cursor});
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
    if (!access.key.has_value() || !read_registers_->cursor.has_value() ||
        !write_cursor_.has_value()) {
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
    if (auto opened =
            Append(OpenReadCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
    auto sought = ConvertProgramResult(
        AssumeValue(builder_).EmitSeekRowId(AssumeValue(read_registers_->cursor), key, *missing,
                                            RowIdSeekMode::kEqual),
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
    if (auto closed =
            Append(CloseCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
    if (!read_registers_->cursor.has_value() || !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("one-pass DELETE scan resources are incomplete"));
    }
    const CursorId cursor = AssumeValue(read_registers_->cursor);

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
    if (!read_registers_->cursor.has_value() || !write_cursor_.has_value() ||
        !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("one-pass UPDATE scan resources are incomplete"));
    }
    const CursorId cursor = AssumeValue(read_registers_->cursor);

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
    if (!read_registers_->cursor.has_value() || !write_cursor_.has_value() ||
        !source_rowid_register_.has_value()) {
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
    if (auto opened =
            Append(OpenReadCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewind(AssumeValue(read_registers_->cursor),
                                                              *collection_exhausted),
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
    if (auto opened =
            Append(OpenReadCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
        !opened.has_value()) {
      return opened;
    }
    auto sought = ConvertProgramResult(
        AssumeValue(builder_).EmitSeekRowId(AssumeValue(read_registers_->cursor),
                                            AssumeValue(source_rowid_register_),
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
    if (auto closed =
            Append(CloseCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
    if (auto opened =
            Append(OpenReadCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
        !opened.has_value()) {
      return opened;
    }
    auto row_sought = ConvertProgramResult(
        AssumeValue(builder_).EmitSeekRowId(AssumeValue(read_registers_->cursor),
                                            AssumeValue(source_rowid_register_), *missing_row,
                                            RowIdSeekMode::kEqual),
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
    if (auto closed =
            Append(CloseCursorInstruction{.cursor = AssumeValue(read_registers_->cursor)});
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
        !create_index_write_cursor_.has_value() || !create_index_sorter_.has_value() ||
        bound_create_index_->index_name().empty() || bound_create_index_->table_name().empty() ||
        bound_create_index_->canonical_sql().empty()) {
      return std::unexpected(InternalFailure("CREATE INDEX lowering resources are incomplete"));
    }
    const std::span<const BoundCreateIndexTerm> terms = bound_create_index_->terms();
    const auto key_count = static_cast<std::uint32_t>(terms.size());
    constexpr RegisterId kRootPage{3};
    constexpr RegisterId kSchemaCookie{7};
    constexpr RegisterId kFirstKey{8};
    const CursorId table_cursor = AssumeValue(create_index_table_cursor_);
    const WriteCursorId index_cursor = AssumeValue(create_index_write_cursor_);
    const SorterId sorter = AssumeValue(create_index_sorter_);

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

    if (auto opened = Append(OpenSorterInstruction{.sorter = sorter}); !opened.has_value()) {
      return opened;
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
              .missing_value_affinity = terms[index].affinity,
          });
          !field.has_value()) {
        return field;
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
    if (auto inserted = Append(InsertSorterInstruction{
            .sorter = sorter,
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
    auto complete = CreateLabel();
    auto drain = CreateLabel();
    if (!complete.has_value()) {
      return std::unexpected(std::move(complete.error()));
    }
    if (!drain.has_value()) {
      return std::unexpected(std::move(drain.error()));
    }
    auto sorted = ConvertProgramResult(AssumeValue(builder_).EmitRewindSorter(sorter, *complete),
                                       "unable to emit CREATE INDEX sorter rewind");
    if (!sorted.has_value()) {
      return std::unexpected(std::move(sorted.error()));
    }
    if (auto bound = BindLabel(*drain); !bound.has_value()) {
      return bound;
    }
    if (auto inserted = Append(InsertSorterIndexInstruction{.sorter = sorter});
        !inserted.has_value()) {
      return inserted;
    }
    auto advanced = ConvertProgramResult(AssumeValue(builder_).EmitNextSorter(sorter, *drain),
                                         "unable to emit CREATE INDEX sorter advance");
    if (!advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    if (auto bound = BindLabel(*complete); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = table_cursor}); !closed.has_value()) {
      return closed;
    }
    if (auto closed = Append(CloseWriteCursorInstruction{.cursor = index_cursor});
        !closed.has_value()) {
      return closed;
    }
    if (auto closed = Append(CloseSorterInstruction{.sorter = sorter}); !closed.has_value()) {
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
      const Stat1IndexPolicy policy = metadata.primary_without_rowid
                                          ? Stat1IndexPolicy::PrimaryWithoutRowid
                                      : metadata.unique_not_null ? Stat1IndexPolicy::UniqueNotNull
                                      : metadata.unique          ? Stat1IndexPolicy::UniqueNullable
                                                                 : Stat1IndexPolicy::Ordinary;
      if (auto computed = Append(ComputeIndexStat1Instruction{
              .cursor = analyze_index_cursors_[index],
              .key_term_count = metadata.key_term_count,
              .emit_empty = metadata.partial,
              .policy = policy,
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

  struct MutationIndexCursor {
    std::size_t target_index;
    WriteCursorId cursor;
  };

  [[nodiscard]] LoweringResult<void> EmitMutationValueAffinity(const BoundMutationTarget& target,
                                                               RegisterId first_value) {
    for (std::size_t index = 0; index < target.columns.size(); ++index) {
      const BoundMutationColumn& column = target.columns[index];
      if (column.rowid_alias) {
        continue;
      }
      const RegisterId value{
          first_value.value() + static_cast<std::uint32_t>(index),
      };
      if (column.affinity == TypeAffinity::kReal) {
        if (auto applied = Append(RealStorageAffinityInstruction{.input = value, .output = value});
            !applied.has_value()) {
          return applied;
        }
      } else if (auto applied = Append(ApplyAffinityInstruction{
                     .input = value,
                     .affinity = column.affinity,
                     .output = value,
                 });
                 !applied.has_value()) {
        return applied;
      }
    }
    return {};
  }

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
    const TableRecordAffinityMode affinity_mode =
        index_write_cursors_.empty() ? TableRecordAffinityMode::kApplyColumnAffinity
                                     : TableRecordAffinityMode::kAffinityPreapplied;
    if (!index_write_cursors_.empty()) {
      if (auto applied = EmitMutationValueAffinity(target, AssumeValue(insert_values_first_));
          !applied.has_value()) {
        return applied;
      }
    }
    if (auto built = Append(BuildTableRecordInstruction{
            .cursor = AssumeValue(write_cursor_),
            .first_value = AssumeValue(insert_values_first_),
            .value_count = static_cast<std::uint32_t>(target.columns.size()),
            .output = AssumeValue(insert_record_register_),
            .affinity_mode = affinity_mode,
        });
        !built.has_value()) {
      return built;
    }
    if (index_write_cursors_.size() != target.indexes.size()) {
      return std::unexpected(InternalFailure("INSERT index cursor metadata is incomplete"));
    }
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      if (index_cursor.target_index >= target.indexes.size()) {
        return std::unexpected(InternalFailure("INSERT index cursor metadata is incomplete"));
      }
      if (auto opened = Append(OpenWriteCursorInstruction{.cursor = index_cursor.cursor});
          !opened.has_value()) {
        return opened;
      }
    }
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      const BoundIndexMaintenance& maintenance = target.indexes[index_cursor.target_index];
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
                .cursor = index_cursor.cursor,
                .first_key = AssumeValue(mutation_index_key_first_),
                .key_count = maintenance.key_term_count,
                .ignored_rowid = std::nullopt,
            });
            !checked.has_value()) {
          return checked;
        }
      }
    }
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      const BoundIndexMaintenance& maintenance = target.indexes[index_cursor.target_index];
      auto key = EmitMutationIndexKey(target, maintenance,
                                      MutationIndexValueRegisters{
                                          .first_values = AssumeValue(insert_values_first_),
                                          .rowid = AssumeValue(insert_rowid_register_),
                                      });
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      if (auto inserted = Append(InsertIndexInstruction{
              .cursor = index_cursor.cursor,
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
    for (const MutationIndexCursor& index_cursor : index_write_cursors_) {
      if (auto closed = Append(CloseWriteCursorInstruction{.cursor = index_cursor.cursor});
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
    active_streaming_core_ = &core;
    filter_ = core.filter;
    projection_ = core.projection;
    read_registers_->cursor = core.cursor;
    read_registers_->index_cursor = core.index_cursor;
    leaf_ = core.leaf;
    table_scan_ = core.table_scan;
    rowid_lookup_ = core.rowid_lookup;
    rowid_range_ = core.rowid_range;
    index_scan_ = core.index_scan;
    distinct_ = core.distinct;
    read_registers_->distinct_relation = core.relation;
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
    read_registers_->grouped_sorter = core.group_sorter;
    global_aggregate_query_ = aggregate_program_ != nullptr && aggregate_program_->group_by.empty();
    grouped_aggregate_query_ =
        aggregate_program_ != nullptr && !aggregate_program_->group_by.empty();
    global_representative_query_ =
        global_aggregate_query_ && !core.global_representative_leaves.empty();
    read_registers_->global_filtered_controller_defaults = core.global_filtered_controller_defaults;
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
      read_registers_->global_representative_registers.assign(
          read_registers_->streaming_global_representative_registers.begin(),
          read_registers_->streaming_global_representative_registers.begin() +
              static_cast<std::ptrdiff_t>(global_representative_leaves_.size()));
      if (auto mapped = MapAggregateLeafRegisters(
              global_representative_leaves_, read_registers_->global_representative_registers,
              read_registers_->global_representative_register_map);
          !mapped.has_value()) {
        return mapped;
      }
    } else {
      global_representative_leaves_ = {};
      read_registers_->global_representative_registers.clear();
      read_registers_->global_representative_register_map.clear();
    }

    if (grouped_aggregate_query_) {
      if (read_registers_->streaming_grouped_representative_registers.size() <
              aggregate_program_->representative_leaves.size() ||
          read_registers_->streaming_grouped_retained_registers.size() <
              core.grouped_retained_leaves.size()) {
        return std::unexpected(
            InternalFailure("streaming grouped aggregate register pool is incomplete"));
      }
      read_registers_->grouped_representative_registers.assign(
          read_registers_->streaming_grouped_representative_registers.begin(),
          read_registers_->streaming_grouped_representative_registers.begin() +
              static_cast<std::ptrdiff_t>(aggregate_program_->representative_leaves.size()));
      if (auto mapped =
              MapAggregateLeafRegisters(aggregate_program_->representative_leaves,
                                        read_registers_->grouped_representative_registers,
                                        read_registers_->grouped_representative_register_map);
          !mapped.has_value()) {
        return mapped;
      }

      grouped_retained_leaves_ = core.grouped_retained_leaves;
      read_registers_->grouped_retained_leaf_registers.assign(
          read_registers_->streaming_grouped_retained_registers.begin(),
          read_registers_->streaming_grouped_retained_registers.begin() +
              static_cast<std::ptrdiff_t>(grouped_retained_leaves_.size()));
      if (auto mapped = MapAggregateLeafRegisters(grouped_retained_leaves_,
                                                  read_registers_->grouped_retained_leaf_registers,
                                                  read_registers_->grouped_sorted_register_map);
          !mapped.has_value()) {
        return mapped;
      }
      BuildGroupedKeyRegisterMap();
    } else {
      read_registers_->grouped_representative_registers.clear();
      read_registers_->grouped_representative_register_map.clear();
      grouped_retained_leaves_.clear();
      read_registers_->grouped_retained_leaf_registers.clear();
      read_registers_->grouped_sorted_register_map.clear();
      grouped_key_expression_registers_.assign(expressions_.size(), std::nullopt);
    }
    return {};
  }

  void ClearStreamingAggregateContext() noexcept {
    active_streaming_core_ = nullptr;
    aggregate_core_ = nullptr;
    aggregate_program_ = nullptr;
    aggregate_ = nullptr;
    having_ = nullptr;
    leaf_ = nullptr;
    table_scan_ = nullptr;
    rowid_lookup_ = nullptr;
    rowid_range_ = nullptr;
    index_scan_ = nullptr;
    filter_ = nullptr;
    projection_ = nullptr;
    distinct_ = nullptr;
    read_registers_->cursor.reset();
    read_registers_->index_cursor.reset();
    read_registers_->distinct_relation.reset();
    read_registers_->grouped_sorter.reset();
    global_aggregate_query_ = false;
    global_representative_query_ = false;
    grouped_aggregate_query_ = false;
    read_registers_->global_filtered_controller_defaults = false;
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
      if (auto emitted =
              EmitProjectionValue(select.result_columns[index].expression,
                                  RegisterId{read_registers_->result_block_first.value() +
                                             static_cast<std::uint32_t>(index)},
                                  mode);
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
                                 *core.relation, read_registers_->result_block_first,
                                 static_cast<std::uint32_t>(bound_select_->result_columns().size()),
                                 RelationInsertMode::kKeepExisting, duplicate),
                             "unable to emit streaming DISTINCT membership");
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingCursorClose(const StreamingCorePlan& core) {
    if (!core.cursor.has_value()) {
      return std::unexpected(InternalFailure("streaming core has no cursor to close"));
    }
    if (core.index_cursor.has_value() && core.index_cursor != core.cursor) {
      if (auto closed = Append(CloseCursorInstruction{.cursor = *core.index_cursor});
          !closed.has_value()) {
        return closed;
      }
    }
    return Append(CloseCursorInstruction{.cursor = *core.cursor});
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingClose(const StreamingCorePlan& core,
                                                        bool close_cursor) {
    if (close_cursor) {
      if (auto closed = EmitStreamingCursorClose(core); !closed.has_value()) {
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
            .first = read_registers_->result_block_first,
            .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
        });
        !result.has_value()) {
      return result;
    }
    if (HasLimit()) {
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
                                                   const JoinedRowTargets& targets) {
    switch (output.mode) {
      case SelectOutputMode::Streaming:
        if (!core.relation.has_value()) {
          if (auto offset = EmitOffset(targets.rejected); !offset.has_value()) {
            return offset;
          }
        }
        if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
          return projected;
        }
        if (auto membership = EmitStreamingMembership(core, targets.rejected);
            !membership.has_value()) {
          return membership;
        }
        return EmitStreamingResult(targets.rejected, targets.stopped.value_or(targets.rejected),
                                   core.relation.has_value());
      case SelectOutputMode::Ordered:
        if (output.ordered_layout == nullptr || !output.ordered_record_first.has_value()) {
          return std::unexpected(InternalFailure("joined ordering destination is incomplete"));
        }
        if (OrderedLayoutUsesInputFields(*output.ordered_layout)) {
          if (auto projected = EmitStreamingProjection(select, output.mode);
              !projected.has_value()) {
            return projected;
          }
          if (auto membership = EmitStreamingMembership(core, targets.rejected);
              !membership.has_value()) {
            return membership;
          }
        }
        return EmitOrderedCandidate(*output.ordered_layout, output.ordered_sorter,
                                    output.ordered_top_n, *output.ordered_record_first,
                                    targets.ordered_rejected.value_or(targets.rejected));
    }
    return std::unexpected(InternalFailure("joined output destination is invalid"));
  }

  [[nodiscard]] LoweringResult<void> EmitRightJoinKey(const StreamingCorePlan& core,
                                                      std::size_t depth) {
    const JoinedSourcePlan& source = core.joined_sources[depth];
    if (!source.cursor.has_value() || !source.right_key_first.has_value() ||
        source.right_key_count == 0U || source.declaration == nullptr) {
      return std::unexpected(InternalFailure("RIGHT identity has no cursor or registers"));
    }
    const BoundTableSource& bound_source = *source.declaration;
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

  [[nodiscard]] LoweringResult<void> EmitAutomaticIndexBuild(
      const JoinedSourcePlan& source, const PhysicalAutomaticIndexNode& automatic) {
    if (!source.construction_cursor.has_value() || !source.construction_record_first.has_value() ||
        !source.automatic_relation.has_value()) {
      return std::unexpected(
          InternalFailure("automatic index construction resources are incomplete"));
    }
    const CursorId cursor = *source.construction_cursor;
    const RegisterId first = *source.construction_record_first;
    auto row = CreateLabel();
    auto advance = CreateLabel();
    auto done = CreateLabel();
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
      return opened;
    }
    auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *done),
                                        "unable to position automatic index construction");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (!automatic.construction_predicates.empty()) {
      auto binding = SourceCursor(automatic.source);
      if (!binding.has_value()) {
        return std::unexpected(std::move(binding.error()));
      }
      auto snapshots = std::exchange(source_snapshot_registers_, {});
      source_cursor_bindings_[automatic.source.value()] =
          SourceCursorBinding{.cursor = cursor, .rowid_field = binding->rowid_field};
      const auto filtered = [&]() -> LoweringResult<void> {
        for (const BoundExpressionId predicate : automatic.construction_predicates) {
          if (auto emitted = EmitAggregatePredicate(
                  predicate, *advance, lowering_internal::AggregateExpressionSource::AccessRow);
              !emitted.has_value()) {
            return emitted;
          }
        }
        return {};
      }();
      source_cursor_bindings_[automatic.source.value()] = *binding;
      source_snapshot_registers_ = std::move(snapshots);
      if (!filtered.has_value()) {
        return filtered;
      }
    }
    const CatalogTable& table = bound_select_->catalog()->table(automatic.table);
    std::uint32_t field = 0;
    const auto emit_column = [&](ColumnId column) -> LoweringResult<void> {
      const std::uint32_t position = field++;
      return EmitCursorFieldValue(cursor, CursorFieldId{position},
                                  RegisterId{first.value() + position},
                                  table.columns[column.value].affinity == TypeAffinity::kReal);
    };
    for (const PhysicalIndexEquality& equality : automatic.equalities) {
      if (auto read = emit_column(equality.column); !read.has_value()) {
        return read;
      }
    }
    for (const ColumnId column : automatic.covering_columns) {
      if (auto read = emit_column(column); !read.has_value()) {
        return read;
      }
    }
    if (auto rowid =
            EmitCursorFieldValue(cursor, CursorFieldId{field}, RegisterId{first.value() + field});
        !rowid.has_value()) {
      return rowid;
    }
    auto inserted = ConvertProgramResult(
        AssumeValue(builder_).EmitInsertRelation(*source.automatic_relation, first, field + 1U,
                                                 RelationInsertMode::kKeepExisting, *advance),
        "unable to insert automatic covering record");
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *row),
                                     "unable to advance automatic index construction");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = cursor}); !closed.has_value()) {
      return closed;
    }
    return Append(SealRelationInstruction{.relation = *source.automatic_relation});
  }

  template <typename RowConsumer>
  [[nodiscard]] LoweringResult<void> EmitJoinedLoop(const StreamingCorePlan& core,
                                                    std::size_t depth, RowConsumer& consume,
                                                    bool always_stops, std::optional<Label> stopped,
                                                    std::span<const RightJoinContinuation> replays,
                                                    std::optional<TopNId> ordered_top_n) {
    const JoinedSourcePlan& source = core.joined_sources[depth];
    if (source.access == nullptr || !source.cursor.has_value()) {
      return std::unexpected(InternalFailure("joined loop has no source cursor"));
    }
    const auto* lookup = std::get_if<PhysicalRowIdLookupNode>(&source.access->payload);
    const auto* rowid_range = std::get_if<PhysicalRowIdRangeNode>(&source.access->payload);
    const auto* index_scan = std::get_if<PhysicalIndexScanNode>(&source.access->payload);
    const auto* automatic = std::get_if<PhysicalAutomaticIndexNode>(&source.access->payload);
    const auto* table_scan = std::get_if<PhysicalTableScanNode>(&source.access->payload);
    const bool reverse =
        (index_scan != nullptr && index_scan->direction == PhysicalScanDirection::kReverse) ||
        (rowid_range != nullptr && rowid_range->direction == PhysicalScanDirection::kReverse) ||
        (table_scan != nullptr && table_scan->direction == PhysicalScanDirection::kReverse);
    const bool has_inner = depth + 1U < core.joined_sources.size();
    const bool left = source.join != nullptr && SourceJoinPreservesLeft(source.join->join);
    const bool right = source.join != nullptr && SourceJoinPreservesRight(source.join->join);
    const auto replay = std::ranges::find(replays, depth, &RightJoinContinuation::depth);
    if (right && (replay == replays.end() || !source.replay_mode.has_value() ||
                  !source.right_matches.has_value() || !source.right_key_first.has_value())) {
      return std::unexpected(InternalFailure("RIGHT source has no replay state"));
    }
    const bool has_on = std::ranges::any_of(core.source_nodes, [&](const PhysicalNode& node) {
      const auto* join = std::get_if<PhysicalNestedLoopJoinNode>(&node.payload);
      return join != nullptr && std::ranges::any_of(join->predicates, [&](const auto& predicate) {
               return predicate.source == source.source && !predicate.consumed_by_access;
             });
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
    std::optional<Label> ordered_rejected;
    if (ordered_top_n.has_value() && !has_inner && !right) {
      const auto access = std::ranges::find(read_plan_->core_access_plans(), core.core_index,
                                            &PhysicalCoreAccessPlan::core_index);
      if (access == read_plan_->core_access_plans().end()) {
        return std::unexpected(InternalFailure("joined inner loop has no access publication"));
      }
      if (access->ordered_inner_loop) {
        auto label = CreateLabel();
        if (!label.has_value()) {
          return std::unexpected(std::move(label.error()));
        }
        ordered_rejected = *label;
      }
    }
    std::optional<Label> positioned_end;
    std::optional<IndexTraversal> index_traversal;
    std::optional<IndexNullScan> null_scan;
    std::optional<lowering_internal::RowIdTraversal> rowid_traversal;
    if (index_scan != nullptr) {
      auto resolved_traversal = ResolveIndexTraversal(*index_scan, *bound_select_->catalog());
      if (!resolved_traversal.has_value()) {
        return std::unexpected(std::move(resolved_traversal.error()));
      }
      index_traversal = *resolved_traversal;
      if (index_scan->two_pass_null_order) {
        auto created = CreateIndexNullScan(*index_traversal, source.index_null_pass);
        if (!created.has_value()) {
          return std::unexpected(std::move(created.error()));
        }
        null_scan = *created;
      }
      if ((index_traversal->has_start_key || index_traversal->has_end_key) &&
          (!source.access_key_first.has_value() || source.access_key_capacity == 0U)) {
        return std::unexpected(InternalFailure("joined index has no retained prefix registers"));
      }
      if (index_traversal->has_end_key && !null_scan.has_value()) {
        auto label = CreateLabel();
        if (!label.has_value()) {
          return std::unexpected(std::move(label.error()));
        }
        positioned_end = *label;
      }
    } else if (rowid_range != nullptr) {
      auto resolved = lowering_internal::ResolveRowIdTraversal(*rowid_range);
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      rowid_traversal = *resolved;
      if (rowid_traversal->end != nullptr) {
        if (!source.access_key_first.has_value() || source.access_key_capacity != 1U ||
            !read_registers_->rowid_range_rowid_register.has_value() ||
            !read_registers_->rowid_range_comparison_register.has_value()) {
          return std::unexpected(
              InternalFailure("joined rowid range end resources are incomplete"));
        }
        auto label = CreateLabel();
        if (!label.has_value()) {
          return std::unexpected(std::move(label.error()));
        }
        positioned_end = *label;
      }
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
    std::uint32_t end_key_count = 0;
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
    } else if (rowid_range != nullptr) {
      const auto& traversal = AssumeValue(rowid_traversal);
      if (traversal.start != nullptr) {
        const RegisterId key = Home(traversal.start->key);
        if (auto emitted = EmitExpression(traversal.start->key, key); !emitted.has_value()) {
          return emitted;
        }
        auto sought = ConvertProgramResult(
            AssumeValue(builder_).EmitSeekRowId(cursor, key, *done, traversal.start_mode),
            "unable to position joined rowid range");
        if (!sought.has_value()) {
          return std::unexpected(std::move(sought.error()));
        }
      } else {
        auto rewound =
            ConvertProgramResult(reverse ? AssumeValue(builder_).EmitLast(cursor, *done)
                                         : AssumeValue(builder_).EmitRewind(cursor, *done),
                                 "unable to position joined rowid range");
        if (!rewound.has_value()) {
          return std::unexpected(std::move(rewound.error()));
        }
      }
      if (traversal.end != nullptr) {
        if (auto emitted = EmitExpression(traversal.end->key, AssumeValue(source.access_key_first));
            !emitted.has_value()) {
          return emitted;
        }
      }
    } else if (automatic != nullptr) {
      if (!source.automatic_relation.has_value() || !source.access_key_first.has_value() ||
          source.access_key_capacity == 0U) {
        return std::unexpected(InternalFailure("automatic lookup resources are incomplete"));
      }
      auto check = CreateLabel();
      auto ready = CreateLabel();
      if (!check.has_value()) {
        return std::unexpected(std::move(check.error()));
      }
      if (!ready.has_value()) {
        return std::unexpected(std::move(ready.error()));
      }
      if (auto bound = BindLabel(*check); !bound.has_value()) {
        return bound;
      }
      auto checked = ConvertProgramResult(
          AssumeValue(builder_).EmitCheckRelationLookup(*source.automatic_relation, *ready, *done),
          "unable to check automatic lookup readiness");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
      if (auto built = EmitAutomaticIndexBuild(source, *automatic); !built.has_value()) {
        return built;
      }
      if (auto repeated = EmitJump(*check); !repeated.has_value()) {
        return repeated;
      }
      if (auto bound = BindLabel(*ready); !bound.has_value()) {
        return bound;
      }
      if (auto keys = EmitIndexEqualityExpressions(automatic->equalities, *done);
          !keys.has_value()) {
        return keys;
      }
      auto count = EmitIndexKeyBlock(automatic->equalities, nullptr, {}, *source.access_key_first,
                                     source.access_key_capacity);
      if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
      }
      auto sought = ConvertProgramResult(AssumeValue(builder_).EmitSeekRelationPrefix(
                                             cursor, *source.access_key_first, *count, *done),
                                         "unable to position automatic lookup");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else if (index_scan != nullptr) {
      const IndexTraversal& traversal = AssumeValue(index_traversal);
      if (auto keys = EmitIndexKeyExpressions(*index_scan, traversal, IndexKeyPhase::start, *done);
          !keys.has_value()) {
        return keys;
      }
      if (traversal.has_start_key) {
        const RegisterId first = AssumeValue(source.access_key_first);
        auto count =
            EmitIndexKeyBlock(*index_scan, traversal.start, first, source.access_key_capacity);
        if (!count.has_value()) {
          return std::unexpected(std::move(count.error()));
        }
        if (null_scan.has_value()) {
          if (auto started = EmitIndexNullScanStart(*index_scan, *null_scan, cursor, first, *count);
              !started.has_value()) {
            return started;
          }
        } else {
          auto sought =
              ConvertProgramResult(EmitIndexTraversalSeek(AssumeValue(builder_), *index_scan,
                                                          traversal, cursor, first, *count, *done),
                                   "unable to position joined index prefix");
          if (!sought.has_value()) {
            return std::unexpected(std::move(sought.error()));
          }
        }
      } else {
        auto rewound =
            ConvertProgramResult(reverse ? AssumeValue(builder_).EmitLast(cursor, *done)
                                         : AssumeValue(builder_).EmitRewind(cursor, *done),
                                 "unable to position joined source");
        if (!rewound.has_value()) {
          return std::unexpected(std::move(rewound.error()));
        }
      }
      if (auto keys =
              EmitIndexKeyExpressions(*index_scan, traversal, IndexKeyPhase::end, *done, cursor);
          !keys.has_value()) {
        return keys;
      }
      if (traversal.end.has_range_term()) {
        auto count =
            EmitIndexKeyBlock(*index_scan, traversal.end, AssumeValue(source.access_key_first),
                              source.access_key_capacity, IndexKeyPhase::end);
        if (!count.has_value()) {
          return std::unexpected(std::move(count.error()));
        }
        end_key_count = *count;
      } else {
        end_key_count = static_cast<std::uint32_t>(index_scan->equalities.size());
      }
    } else {
      auto rewound = ConvertProgramResult(reverse ? AssumeValue(builder_).EmitLast(cursor, *done)
                                                  : AssumeValue(builder_).EmitRewind(cursor, *done),
                                          "unable to position joined source");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }
    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (null_scan.has_value()) {
      if (auto checked = EmitIndexNullScanEndCheck(
              *index_scan, *null_scan, cursor, AssumeValue(source.access_key_first), end_key_count);
          !checked.has_value()) {
        return checked;
      }
    } else if (index_scan != nullptr && positioned_end.has_value()) {
      auto checked = ConvertProgramResult(
          EmitIndexTraversalEndCheck(
              AssumeValue(builder_), *index_scan, AssumeValue(index_traversal), cursor,
              AssumeValue(source.access_key_first), end_key_count, *positioned_end),
          "unable to check joined index prefix");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    } else if (rowid_traversal.has_value() && positioned_end.has_value()) {
      auto binary = EnsureBinarySymbol();
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      if (auto checked = EmitRowIdTraversalEndCheck(
              AssumeValue(builder_), *rowid_traversal, cursor, AssumeValue(source.access_key_first),
              AssumeValue(read_registers_->rowid_range_rowid_register),
              AssumeValue(read_registers_->rowid_range_comparison_register), *binary,
              *positioned_end);
          !checked.has_value()) {
        return checked;
      }
    }
    if (source.table_cursor.has_value()) {
      if (!read_registers_->index_rowid_register.has_value()) {
        return std::unexpected(InternalFailure("joined table lookup has no rowid register"));
      }
      if (auto sought = EmitIndexTableLookup(AssumeValue(builder_), cursor, *source.table_cursor,
                                             AssumeValue(read_registers_->index_rowid_register));
          !sought.has_value()) {
        return sought;
      }
    }
    if (left || right) {
      for (const PhysicalJoinPredicatePlacement& predicate : source.join->predicates) {
        if (predicate.source != source.source) {
          return std::unexpected(
              InternalFailure("outer ON predicate is not owned by its right source"));
        }
        if (!predicate.consumed_by_access) {
          if (auto on = EmitPredicate(predicate.predicate, advance.value_or(*done));
              !on.has_value()) {
            return on;
          }
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
    for (const PhysicalNode& node : core.source_nodes) {
      const auto* join = std::get_if<PhysicalNestedLoopJoinNode>(&node.payload);
      if (join != nullptr && !SourceJoinPreservesLeft(join->join) &&
          !SourceJoinPreservesRight(join->join)) {
        for (const PhysicalJoinPredicatePlacement& predicate : join->predicates) {
          if (predicate.source == source.source && !predicate.consumed_by_access) {
            if (auto on = EmitPredicate(predicate.predicate, advance.value_or(*done));
                !on.has_value()) {
              return on;
            }
          }
        }
      }
    }
    if (has_inner) {
      if (auto inner = EmitJoinedLoop(core, depth + 1U, consume, always_stops, stopped, replays,
                                      ordered_top_n);
          !inner.has_value()) {
        return inner;
      }
    } else if (auto emitted = consume(JoinedRowTargets{
                   .rejected = advance.value_or(*done),
                   .stopped = stopped,
                   .ordered_rejected = ordered_rejected,
               });
               !emitted.has_value()) {
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
      } else if (automatic != nullptr) {
        auto next = ConvertProgramResult(
            AssumeValue(builder_).EmitNextRelationPrefix(
                cursor, AssumeValue(source.access_key_first), source.access_key_capacity, *row),
            "unable to advance automatic lookup prefix");
        if (!next.has_value()) {
          return std::unexpected(std::move(next.error()));
        }
      } else {
        auto next = ConvertProgramResult(reverse ? AssumeValue(builder_).EmitPrevious(cursor, *row)
                                                 : AssumeValue(builder_).EmitNext(cursor, *row),
                                         "unable to advance joined source");
        if (!next.has_value()) {
          return std::unexpected(std::move(next.error()));
        }
        if (index_scan != nullptr || positioned_end.has_value()) {
          if (auto finished = EmitJump(null_scan.has_value() ? null_scan->exhausted : *done);
              !finished.has_value()) {
            return finished;
          }
        }
      }
    }
    if (ordered_rejected.has_value()) {
      if (index_scan == nullptr && !positioned_end.has_value()) {
        if (auto finished = EmitJump(*done); !finished.has_value()) {
          return finished;
        }
      }
      if (auto bound = BindLabel(*ordered_rejected); !bound.has_value()) {
        return bound;
      }
      if (source.table_cursor.has_value()) {
        if (auto cleared =
                Append(NullRowInstruction{.cursor = *source.table_cursor, .enabled = false});
            !cleared.has_value()) {
          return cleared;
        }
      }
      if (auto cleared = Append(NullRowInstruction{.cursor = cursor, .enabled = false});
          !cleared.has_value()) {
        return cleared;
      }
      if (auto finished = EmitJump(*done); !finished.has_value()) {
        return finished;
      }
    }
    if (positioned_end.has_value()) {
      if (auto bound = BindLabel(*positioned_end); !bound.has_value()) {
        return bound;
      }
      if (auto cleared = Append(NullRowInstruction{.cursor = cursor, .enabled = false});
          !cleared.has_value()) {
        return cleared;
      }
    }
    if (null_scan.has_value()) {
      if (auto completed =
              EmitIndexNullScanCompletion(*null_scan, cursor, source.table_cursor, *done);
          !completed.has_value()) {
        return completed;
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

  [[nodiscard]] LoweringResult<void> EmitJoinedSourceClose(const StreamingCorePlan& core,
                                                           std::optional<RelationId> membership) {
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
      if (source.automatic_relation.has_value()) {
        if (auto closed = Append(CloseRelationInstruction{.relation = *source.automatic_relation});
            !closed.has_value()) {
          return closed;
        }
      }
    }
    return membership.has_value() ? Append(CloseRelationInstruction{.relation = *membership})
                                  : LoweringResult<void>{};
  }

  template <typename RowConsumer>
  [[nodiscard]] LoweringResult<void> EmitJoinedSourceRows(
      const StreamingCorePlan& core, RowConsumer& consume, bool always_stops,
      std::optional<Label> final_completion, std::optional<RelationId> membership,
      std::optional<TopNId> ordered_top_n = {}) {
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
    for (const PhysicalNode& node : core.source_nodes) {
      const auto* join = std::get_if<PhysicalNestedLoopJoinNode>(&node.payload);
      if (join == nullptr || SourceJoinPreservesLeft(join->join) ||
          SourceJoinPreservesRight(join->join)) {
        continue;
      }
      for (const PhysicalJoinPredicatePlacement& predicate : join->predicates) {
        if (!predicate.source.has_value()) {
          if (auto emitted = EmitPredicate(predicate.predicate, *after); !emitted.has_value()) {
            return emitted;
          }
        }
      }
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
      if (source.index_null_pass.has_value()) {
        auto zero = EnsureZeroConstant();
        if (!zero.has_value()) {
          return std::unexpected(std::move(zero.error()));
        }
        if (auto initialized = Append(
                LoadConstantInstruction{.constant = *zero, .output = *source.index_null_pass});
            !initialized.has_value()) {
          return initialized;
        }
      }
      if (source.access_key_first.has_value() &&
          (source.automatic_relation.has_value() || !replays.empty() ||
           (source.join != nullptr && SourceJoinPreservesLeft(source.join->join)))) {
        const RegisterId first = AssumeValue(source.access_key_first);
        auto null = EnsureNullConstant();
        if (!null.has_value()) {
          return std::unexpected(std::move(null.error()));
        }
        for (std::uint32_t term = 0; term < source.access_key_capacity; ++term) {
          if (auto initialized = Append(LoadConstantInstruction{
                  .constant = *null,
                  .output = RegisterId{first.value() + term},
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
      if (source.automatic_relation.has_value()) {
        if (auto opened =
                Append(OpenRelationInstruction{.relation = AssumeValue(source.automatic_relation)});
            !opened.has_value()) {
          return opened;
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
    if (auto loop =
            EmitJoinedLoop(core, 0U, consume, always_stops, stopped, replays, ordered_top_n);
        !loop.has_value()) {
      return loop;
    }
    for (const RightJoinContinuation& replay : replays) {
      if (auto emitted = EmitRightJoinReplay(core, replay); !emitted.has_value()) {
        return emitted;
      }
    }
    if (auto closed = EmitJoinedSourceClose(core, membership); !closed.has_value()) {
      return closed;
    }
    if (stopped.has_value()) {
      if (auto jumped = EmitJump(*after); !jumped.has_value()) {
        return jumped;
      }
      if (auto bound = BindLabel(*stopped); !bound.has_value()) {
        return bound;
      }
      if (auto closed = EmitJoinedSourceClose(core, membership); !closed.has_value()) {
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
    const bool may_stop = output.mode == SelectOutputMode::Streaming && HasLimit();
    if (may_stop && !output.final_completion.has_value()) {
      return std::unexpected(InternalFailure("joined stop has no final completion"));
    }
    auto consume = [&](const JoinedRowTargets& targets) {
      return EmitJoinedRow(core, select, output, targets);
    };
    return EmitJoinedSourceRows(
        core, consume, false, may_stop ? output.final_completion : std::nullopt, core.relation,
        output.mode == SelectOutputMode::Ordered ? output.ordered_top_n : std::nullopt);
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
    auto consume = [&](const JoinedRowTargets&) -> LoweringResult<void> {
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

  template <typename Consume>
  [[nodiscard]] LoweringResult<void> EmitStreamingCursorRows(
      const StreamingCorePlan& core, Label unpositioned_completion,
      std::optional<Label> positioned_completion, std::optional<Label> consumed_completion,
      Consume&& consume) {
    if (!core.cursor.has_value()) {
      return std::unexpected(InternalFailure("streaming cursor access has no cursor"));
    }
    const PhysicalIndexScanNode* index = core.index_scan;
    if (index != nullptr && !core.index_cursor.has_value()) {
      return std::unexpected(InternalFailure("streaming index access has no index cursor"));
    }
    const CursorId cursor = index != nullptr ? AssumeValue(core.index_cursor) : *core.cursor;
    const bool table_lookup = index != nullptr && !index->covering;
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = cursor}); !opened.has_value()) {
      return opened;
    }
    if (table_lookup) {
      if (auto opened = Append(OpenReadCursorInstruction{.cursor = *core.cursor});
          !opened.has_value()) {
        return opened;
      }
    }
    if (core.rowid_lookup != nullptr) {
      auto key = EmitRowIdLookupKey(core.rowid_lookup->key);
      if (!key.has_value()) {
        return std::unexpected(std::move(key.error()));
      }
      auto sought = ConvertProgramResult(
          AssumeValue(builder_).EmitSeekRowId(cursor, *key, unpositioned_completion),
          "unable to emit streaming rowid seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
      const bool may_reject = core.filter != nullptr && !core.filter->predicates.empty();
      if (!positioned_completion.has_value() && (!consumed_completion.has_value() || may_reject)) {
        return std::unexpected(
            InternalFailure("streaming rowid seek has no positioned completion"));
      }
      const Label rejected = positioned_completion.has_value() ? *positioned_completion
                                                               : AssumeValue(consumed_completion);
      if (auto consumed = std::forward<Consume>(consume)(rejected); !consumed.has_value()) {
        return consumed;
      }
      const Label completed = consumed_completion.has_value() ? *consumed_completion
                                                              : AssumeValue(positioned_completion);
      return EmitJump(completed);
    }

    auto row = CreateLabel();
    if (!row.has_value()) {
      return std::unexpected(std::move(row.error()));
    }
    const bool may_reject = core.filter != nullptr && !core.filter->predicates.empty();
    std::optional<Label> advance;
    if (!consumed_completion.has_value() || may_reject) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      advance = *label;
    }
    bool reverse = false;
    std::optional<lowering_internal::RowIdTraversal> range_traversal;
    std::optional<IndexTraversal> index_traversal;
    std::optional<IndexNullScan> null_scan;
    std::uint32_t index_end_key_count = 0;
    std::optional<Label> positioned_end;
    std::optional<RegisterId> end_key;
    if (index != nullptr) {
      auto resolved = ResolveIndexTraversal(*index, *bound_select_->catalog());
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      index_traversal = *resolved;
      reverse = index->direction == PhysicalScanDirection::kReverse;
      if (index->two_pass_null_order) {
        auto created = CreateIndexNullScan(*index_traversal, read_registers_->index_null_pass);
        if (!created.has_value()) {
          return std::unexpected(std::move(created.error()));
        }
        null_scan = *created;
      }
      if (!index->equalities.empty() || index->range.has_value()) {
        if (auto keys = EmitIndexKeyExpressions(*index, *index_traversal, IndexKeyPhase::start,
                                                unpositioned_completion);
            !keys.has_value()) {
          return keys;
        }
      }
      if (index_traversal->has_start_key) {
        auto key = EmitIndexKeyBlock(*index, index_traversal->start);
        if (!key.has_value()) {
          return std::unexpected(std::move(key.error()));
        }
        if (null_scan.has_value()) {
          if (auto started = EmitIndexNullScanStart(
                  *index, *null_scan, cursor, AssumeValue(read_registers_->index_key_first), *key);
              !started.has_value()) {
            return started;
          }
        } else {
          auto sought = ConvertProgramResult(
              EmitIndexTraversalSeek(AssumeValue(builder_), *index, *index_traversal, cursor,
                                     AssumeValue(read_registers_->index_key_first), *key,
                                     unpositioned_completion),
              "unable to emit streaming index seek");
          if (!sought.has_value()) {
            return std::unexpected(std::move(sought.error()));
          }
        }
      } else {
        auto positioned = ConvertProgramResult(
            reverse ? AssumeValue(builder_).EmitLast(cursor, unpositioned_completion)
                    : AssumeValue(builder_).EmitRewind(cursor, unpositioned_completion),
            "unable to emit streaming index positioning");
        if (!positioned.has_value()) {
          return std::unexpected(std::move(positioned.error()));
        }
      }
      if (index_traversal->has_end_key) {
        if (auto keys = EmitIndexKeyExpressions(*index, *index_traversal, IndexKeyPhase::end,
                                                unpositioned_completion, cursor);
            !keys.has_value()) {
          return keys;
        }
        auto key = EmitIndexKeyBlock(*index, index_traversal->end, IndexKeyPhase::end);
        if (!key.has_value()) {
          return std::unexpected(std::move(key.error()));
        }
        index_end_key_count = *key;
      }
    } else if (core.rowid_range != nullptr) {
      auto resolved = lowering_internal::ResolveRowIdTraversal(*core.rowid_range);
      if (!resolved.has_value()) {
        return std::unexpected(std::move(resolved.error()));
      }
      range_traversal = *resolved;
      if (range_traversal->end != nullptr &&
          (!read_registers_->rowid_range_rowid_register.has_value() ||
           !read_registers_->rowid_range_comparison_register.has_value())) {
        return std::unexpected(
            InternalFailure("streaming rowid range end registers are incomplete"));
      }
      reverse = range_traversal->reverse;
      if (range_traversal->start != nullptr) {
        const RegisterId key = Home(range_traversal->start->key);
        if (auto emitted = EmitExpression(range_traversal->start->key, key); !emitted.has_value()) {
          return emitted;
        }
        auto sought = ConvertProgramResult(
            AssumeValue(builder_).EmitSeekRowId(cursor, key, unpositioned_completion,
                                                range_traversal->start_mode),
            "unable to emit streaming rowid range seek");
        if (!sought.has_value()) {
          return std::unexpected(std::move(sought.error()));
        }
      } else {
        ProgramResult<InstructionAddress> positioned =
            reverse ? AssumeValue(builder_).EmitLast(cursor, unpositioned_completion)
                    : AssumeValue(builder_).EmitRewind(cursor, unpositioned_completion);
        auto rewound = ConvertProgramResult(std::move(positioned),
                                            "unable to emit streaming rowid range positioning");
        if (!rewound.has_value()) {
          return std::unexpected(std::move(rewound.error()));
        }
      }
      if (range_traversal->end != nullptr) {
        const RegisterId key = Home(range_traversal->end->key);
        if (auto emitted = EmitExpression(range_traversal->end->key, key); !emitted.has_value()) {
          return emitted;
        }
        end_key = key;
        auto label = CreateLabel();
        if (!label.has_value()) {
          return std::unexpected(std::move(label.error()));
        }
        positioned_end = *label;
      }
    } else {
      if (core.table_scan == nullptr) {
        return std::unexpected(InternalFailure("streaming cursor access kind is unsupported"));
      }
      reverse = core.table_scan->direction == PhysicalScanDirection::kReverse;
      ProgramResult<InstructionAddress> positioned =
          reverse ? AssumeValue(builder_).EmitLast(cursor, unpositioned_completion)
                  : AssumeValue(builder_).EmitRewind(cursor, unpositioned_completion);
      auto rewound =
          ConvertProgramResult(std::move(positioned), "unable to emit streaming table positioning");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
    }

    if (auto bound = BindLabel(*row); !bound.has_value()) {
      return bound;
    }
    if (null_scan.has_value()) {
      if (auto checked = EmitIndexNullScanEndCheck(*index, *null_scan, cursor,
                                                   AssumeValue(read_registers_->index_key_first),
                                                   index_end_key_count);
          !checked.has_value()) {
        return checked;
      }
    } else if (index_traversal.has_value() && index_traversal->has_end_key) {
      auto checked = ConvertProgramResult(
          EmitIndexTraversalEndCheck(AssumeValue(builder_), *index, *index_traversal, cursor,
                                     AssumeValue(read_registers_->index_key_first),
                                     index_end_key_count, unpositioned_completion),
          "unable to check streaming index boundary");
      if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
      }
    }
    if (table_lookup) {
      if (!read_registers_->index_rowid_register.has_value()) {
        return std::unexpected(InternalFailure("streaming index lookup has no rowid register"));
      }
      if (auto sought = EmitIndexTableLookup(AssumeValue(builder_), cursor, *core.cursor,
                                             *read_registers_->index_rowid_register);
          !sought.has_value()) {
        return sought;
      }
    }
    if (positioned_end.has_value()) {
      auto binary = EnsureBinarySymbol();
      if (!binary.has_value()) {
        return std::unexpected(std::move(binary.error()));
      }
      if (auto checked = EmitRowIdTraversalEndCheck(
              AssumeValue(builder_), AssumeValue(range_traversal), cursor, AssumeValue(end_key),
              AssumeValue(read_registers_->rowid_range_rowid_register),
              AssumeValue(read_registers_->rowid_range_comparison_register), *binary,
              *positioned_end);
          !checked.has_value()) {
        return checked;
      }
    }
    const Label rejected = advance.has_value() ? *advance : AssumeValue(consumed_completion);
    if (auto consumed = std::forward<Consume>(consume)(rejected); !consumed.has_value()) {
      return consumed;
    }
    if (consumed_completion.has_value()) {
      if (auto jumped = EmitJump(*consumed_completion); !jumped.has_value()) {
        return jumped;
      }
    }
    if (advance.has_value()) {
      if (auto bound = BindLabel(*advance); !bound.has_value()) {
        return bound;
      }
      if (table_lookup) {
        // Match the unpositioned table state on seek-miss and index-end edges.
        if (auto cleared = Append(NullRowInstruction{.cursor = *core.cursor, .enabled = false});
            !cleared.has_value()) {
          return cleared;
        }
      }
      ProgramResult<InstructionAddress> advanced =
          reverse ? AssumeValue(builder_).EmitPrevious(cursor, *row)
                  : AssumeValue(builder_).EmitNext(cursor, *row);
      auto next =
          ConvertProgramResult(std::move(advanced), "unable to emit streaming cursor advance");
      if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
      }
      if (!null_scan.has_value()) {
        if (auto jumped = EmitJump(unpositioned_completion); !jumped.has_value()) {
          return jumped;
        }
      }
    }
    if (null_scan.has_value()) {
      if (auto completed = EmitIndexNullScanCompletion(*null_scan, cursor,
                                                       table_lookup ? core.cursor : std::nullopt,
                                                       unpositioned_completion);
          !completed.has_value()) {
        return completed;
      }
    }
    if (positioned_end.has_value()) {
      if (!positioned_completion.has_value()) {
        return std::unexpected(
            InternalFailure("streaming rowid range has no positioned completion"));
      }
      if (auto bound = BindLabel(*positioned_end); !bound.has_value()) {
        return bound;
      }
      if (auto jumped = EmitJump(*positioned_completion); !jumped.has_value()) {
        return jumped;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingCursorCore(const StreamingCorePlan& core,
                                                             const BoundSelectCore& select,
                                                             Label final_completion) {
    if (!core.cursor.has_value()) {
      return std::unexpected(InternalFailure("streaming table core has no cursor"));
    }
    SetStreamingCoreContext(core);
    auto after_core = CreateLabel();
    auto opened_completion = CreateLabel();
    std::optional<Label> positioned_completion;
    std::optional<Label> limit_completion;
    if (!after_core.has_value()) {
      return std::unexpected(std::move(after_core.error()));
    }
    if (!opened_completion.has_value()) {
      return std::unexpected(std::move(opened_completion.error()));
    }
    if (core.rowid_lookup != nullptr ||
        (core.rowid_range != nullptr && RowIdRangeHasEnd(*core.rowid_range))) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
    }
    if (HasLimit()) {
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
    const auto consume = [&](Label advance) -> LoweringResult<void> {
      if (auto filters = EmitStreamingFilters(core, advance); !filters.has_value()) {
        return filters;
      }
      if (!core.relation.has_value()) {
        if (auto offset = EmitOffset(advance); !offset.has_value()) {
          return offset;
        }
      }
      if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
        return projected;
      }
      if (auto membership = EmitStreamingMembership(core, advance); !membership.has_value()) {
        return membership;
      }
      const Label row_limit_completion =
          limit_completion.has_value() ? *limit_completion : final_completion;
      return EmitStreamingResult(advance, row_limit_completion, core.relation.has_value());
    };
    if (auto rows = EmitStreamingCursorRows(core, *opened_completion, positioned_completion,
                                            std::nullopt, consume);
        !rows.has_value()) {
      return rows;
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
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto closed = EmitStreamingClose(core, true); !closed.has_value()) {
        return closed;
      }
      if (auto jumped = EmitJump(*after_core); !jumped.has_value()) {
        return jumped;
      }
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

  [[nodiscard]] LoweringResult<void> EmitStreamingEmptyCore(const StreamingCorePlan& core) {
    SetStreamingCoreContext(core);
    auto completion = CreateLabel();
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto guards = EmitStreamingGuards(core, *completion); !guards.has_value()) {
      return guards;
    }
    return BindLabel(*completion);
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
    if (HasLimit() && core.relation.has_value()) {
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
    read_registers_->cursor.reset();
    filter_ = nullptr;
    projection_ = nullptr;
    distinct_ = nullptr;
    read_registers_->distinct_relation.reset();
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
                row[index], RegisterId{read_registers_->result_block_first.value() +
                                       static_cast<std::uint32_t>(index)});
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
              .first = read_registers_->result_block_first,
              .count = static_cast<std::uint32_t>(row.size()),
          });
          !result.has_value()) {
        return result;
      }
      if (HasLimit()) {
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

  [[nodiscard]] LoweringResult<void> EmitAggregateCore(const StreamingCorePlan& core,
                                                       SelectOutputContext output) {
    if (core.aggregate == nullptr || output.core != &core) {
      return std::unexpected(InternalFailure("aggregate core output metadata is invalid"));
    }
    auto after = CreateLabel();
    if (!after.has_value()) {
      return std::unexpected(std::move(after.error()));
    }
    auto completed = CreateLabel();
    if (!completed.has_value()) {
      return std::unexpected(std::move(completed.error()));
    }
    const bool needs_stopped = output.mode == SelectOutputMode::Streaming && HasLimit();
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
    read_registers_->active_aggregate_output = output;
    read_registers_->aggregate_core_stopped_label = stopped.value_or(*after);
    read_registers_->aggregate_core_completion_label = *completed;

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
    read_registers_->active_aggregate_output.reset();
    read_registers_->aggregate_core_stopped_label.reset();
    read_registers_->aggregate_core_completion_label.reset();
    ClearStreamingAggregateContext();
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitStreamingCore(const StreamingCorePlan& core,
                                                       Label completion) {
    if (const auto* values = std::get_if<BoundValuesCore>(core.bound); values != nullptr) {
      return EmitStreamingValuesCore(*values, completion);
    }
    const auto& select = std::get<BoundSelectCore>(*core.bound);
    if (core.aggregate != nullptr) {
      return EmitAggregateCore(core, SelectOutputContext{
                                         .mode = SelectOutputMode::Streaming,
                                         .core = &core,
                                         .final_completion = completion,
                                     });
    }
    if (core.empty != nullptr) {
      return EmitStreamingEmptyCore(core);
    }
    if (core.table_scan != nullptr || core.rowid_lookup != nullptr || core.rowid_range != nullptr ||
        core.index_scan != nullptr) {
      return EmitStreamingCursorCore(core, select, completion);
    }
    return EmitStreamingSingleCore(core, select, completion);
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
      if (auto emitted = EmitStreamingCore(core, *completion); !emitted.has_value()) {
        return emitted;
      }
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitNativeOutputRow(RegisterId row, Label skipped,
                                                         Label completion) {
    if (read_registers_->ordered_full_comparison.has_value()) {
      if (!read_registers_->compound_previous_first.has_value() ||
          !read_registers_->compound_has_previous.has_value() ||
          !read_registers_->ordered_comparison_register.has_value()) {
        return std::unexpected(InternalFailure("native duplicate output state is incomplete"));
      }
      auto unique = CreateLabel();
      if (!unique.has_value()) {
        return std::unexpected(std::move(unique.error()));
      }
      if (auto first =
              EmitJumpIf(*read_registers_->compound_has_previous, JumpCondition::kIfFalse, *unique);
          !first.has_value()) {
        return first;
      }
      if (auto compared = Append(CompareRecordsInstruction{
              .comparison = *read_registers_->ordered_full_comparison,
              .left_first = *read_registers_->compound_previous_first,
              .right_first = row,
              .output = *read_registers_->ordered_comparison_register,
          });
          !compared.has_value()) {
        return compared;
      }
      if (auto duplicate = EmitJumpIf(*read_registers_->ordered_comparison_register,
                                      JumpCondition::kIfFalse, skipped);
          !duplicate.has_value()) {
        return duplicate;
      }
      if (auto bound = BindLabel(*unique); !bound.has_value()) {
        return bound;
      }
      for (std::size_t field = 0; field < bound_select_->result_columns().size(); ++field) {
        if (auto copied = Append(CopyInstruction{
                .input = RegisterId{row.value() + static_cast<std::uint32_t>(field)},
                .output = RegisterId{read_registers_->compound_previous_first->value() +
                                     static_cast<std::uint32_t>(field)},
            });
            !copied.has_value()) {
          return copied;
        }
      }
      auto one = EnsureOneConstant();
      if (!one.has_value()) {
        return std::unexpected(std::move(one.error()));
      }
      if (auto remembered = Append(LoadConstantInstruction{
              .constant = *one, .output = *read_registers_->compound_has_previous});
          !remembered.has_value()) {
        return remembered;
      }
    }
    if (auto offset = EmitOffset(skipped); !offset.has_value()) {
      return offset;
    }
    if (auto output = Append(ResultRowInstruction{
            .first = row,
            .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
        });
        !output.has_value()) {
      return output;
    }
    return EmitDistinctLimitAfterRow(
        DistinctLimitTargets{.advance = skipped, .completion = completion});
  }

  [[nodiscard]] LoweringResult<void> EmitNativeComparisonBranch(SqlComparison condition,
                                                                Label target) {
    if (!read_registers_->ordered_comparison_register.has_value() ||
        !read_registers_->ordered_predicate_register.has_value() ||
        !read_registers_->zero_register.has_value()) {
      return std::unexpected(InternalFailure("native comparison branch has no control registers"));
    }
    auto binary = EnsureBinarySymbol();
    if (!binary.has_value()) {
      return std::unexpected(std::move(binary.error()));
    }
    if (auto compared = Append(CompareInstruction{
            .comparison = condition,
            .affinity = TypeAffinity::kNumeric,
            .collation = *binary,
            .left = *read_registers_->ordered_comparison_register,
            .right = *read_registers_->zero_register,
            .output = *read_registers_->ordered_predicate_register,
        });
        !compared.has_value()) {
      return compared;
    }
    return EmitJumpIf(*read_registers_->ordered_predicate_register, JumpCondition::kIfTrue, target);
  }

  [[nodiscard]] LoweringResult<void> EmitNativeDrain(Label loop, CoroutineId coroutine,
                                                     RegisterId row, Label completion) {
    auto advance = CreateLabel();
    if (!advance.has_value()) {
      return std::unexpected(std::move(advance.error()));
    }
    if (auto bound = BindLabel(loop); !bound.has_value()) {
      return bound;
    }
    if (auto output = EmitNativeOutputRow(row, *advance, completion); !output.has_value()) {
      return output;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    auto resumed =
        ConvertProgramResult(AssumeValue(builder_).EmitResumeCoroutine(coroutine, completion, row),
                             "unable to emit native drain resume");
    if (!resumed.has_value()) {
      return std::unexpected(std::move(resumed.error()));
    }
    return EmitJump(loop);
  }

  [[nodiscard]] LoweringResult<void> EmitNativeEndOnlyMerge(std::size_t index, CoroutineId a,
                                                            std::optional<CoroutineId> b) {
    const auto& producer = native_producers_[index];
    if (!producer.merge.has_value() || !producer.left.has_value() || !producer.right.has_value() ||
        !read_registers_->ordered_left_first.has_value() ||
        !read_registers_->ordered_right_first.has_value()) {
      return std::unexpected(InternalFailure("native end-only merge metadata is incomplete"));
    }
    const auto operation =
        std::get<PhysicalCompoundNode>(read_plan_->node(*producer.merge).payload).operation;
    const bool a_rows = native_producers_[*producer.left].emits_rows;
    const bool b_rows = native_producers_[*producer.right].emits_rows;
    const bool outputs_b =
        operation == CompoundOperator::kUnionAll || operation == CompoundOperator::kUnion;
    const RegisterId row_a = *read_registers_->ordered_left_first;
    const RegisterId row_b = *read_registers_->ordered_right_first;
    auto a_eof = CreateLabel();
    auto done = CreateLabel();
    if (!a_eof.has_value()) {
      return std::unexpected(std::move(a_eof.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    std::optional<Label> drain_a;
    std::optional<Label> drain_b;
    if (a_rows && operation != CompoundOperator::kIntersect) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      drain_a = *label;
    }
    if (b_rows && outputs_b) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      drain_b = *label;
    }
    const auto resume = [&](CoroutineId coroutine, Label exhausted,
                            RegisterId row) -> LoweringResult<void> {
      auto resumed =
          ConvertProgramResult(AssumeValue(builder_).EmitResumeCoroutine(coroutine, exhausted, row),
                               "unable to emit native end-only producer resume");
      if (!resumed.has_value()) {
        return std::unexpected(std::move(resumed.error()));
      }
      return {};
    };
    if (auto initialized = Append(InitCoroutineInstruction{.coroutine = a});
        !initialized.has_value()) {
      return initialized;
    }
    if (auto first = resume(a, *a_eof, row_a); !first.has_value()) {
      return first;
    }
    if (a_rows) {
      if (!b.has_value()) {
        return std::unexpected(InternalFailure("entered native right producer was not declared"));
      }
      auto b_eof = CreateLabel();
      if (!b_eof.has_value()) {
        return std::unexpected(std::move(b_eof.error()));
      }
      if (auto initialized = Append(InitCoroutineInstruction{.coroutine = *b});
          !initialized.has_value()) {
        return initialized;
      }
      if (auto second = resume(*b, *b_eof, row_b); !second.has_value()) {
        return second;
      }
      if (auto bound = BindLabel(*b_eof); !bound.has_value()) {
        return bound;
      }
      if (auto next = EmitJump(drain_a.value_or(*done)); !next.has_value()) {
        return next;
      }
    }
    if (auto bound = BindLabel(*a_eof); !bound.has_value()) {
      return bound;
    }
    if (outputs_b) {
      if (!b.has_value()) {
        return std::unexpected(InternalFailure("native right drain was not declared"));
      }
      if (auto initialized = Append(InitCoroutineInstruction{.coroutine = *b});
          !initialized.has_value()) {
        return initialized;
      }
      if (auto second = resume(*b, *done, row_b); !second.has_value()) {
        return second;
      }
      if (drain_b.has_value()) {
        if (auto next = EmitJump(*drain_b); !next.has_value()) {
          return next;
        }
      }
    } else if (auto next = EmitJump(*done); !next.has_value()) {
      return next;
    }
    if (drain_a.has_value()) {
      if (auto drained = EmitNativeDrain(*drain_a, a, row_a, *done); !drained.has_value()) {
        return drained;
      }
    }
    if (drain_b.has_value()) {
      if (!b.has_value()) {
        return std::unexpected(InternalFailure("native right drain was not declared"));
      }
      if (auto drained = EmitNativeDrain(*drain_b, *b, row_b, *done); !drained.has_value()) {
        return drained;
      }
    }
    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return bound;
    }
    if (auto cancelled = Append(CancelCoroutineInstruction{.coroutine = a});
        !cancelled.has_value()) {
      return cancelled;
    }
    return b.has_value() ? Append(CancelCoroutineInstruction{.coroutine = *b})
                         : LoweringResult<void>{};
  }

  [[nodiscard]] LoweringResult<void> EmitNativeMergeInitialization(std::size_t index) {
    const NativeProducer& producer = native_producers_[index];
    if (!producer.left.has_value()) {
      return std::unexpected(InternalFailure("native merge initialization has no left producer"));
    }
    if (!HasLimit()) {
      if (auto controls = EmitReadConstantInitialization(); !controls.has_value()) {
        return controls;
      }
    }
    if (read_registers_->compound_has_previous.has_value()) {
      if (!read_registers_->compound_previous_first.has_value()) {
        return std::unexpected(InternalFailure("native duplicate comparison has no previous row"));
      }
      auto zero = EnsureZeroConstant();
      auto null = EnsureNullConstant();
      if (!zero.has_value()) {
        return std::unexpected(std::move(zero.error()));
      }
      if (!null.has_value()) {
        return std::unexpected(std::move(null.error()));
      }
      for (std::size_t field = 0; field < bound_select_->result_columns().size(); ++field) {
        if (auto initialized = Append(LoadConstantInstruction{
                .constant = *null,
                .output = RegisterId{read_registers_->compound_previous_first->value() +
                                     static_cast<std::uint32_t>(field)}});
            !initialized.has_value()) {
          return initialized;
        }
      }
      if (auto initialized = Append(LoadConstantInstruction{
              .constant = *zero, .output = *read_registers_->compound_has_previous});
          !initialized.has_value()) {
        return initialized;
      }
    }
    if (native_producers_[*producer.left].has_input_limit) {
      if (auto bound = EmitNativeInputBound(); !bound.has_value()) {
        return bound;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitNativeMerge(std::size_t index) {
    const NativeProducer& producer = native_producers_[index];
    if (!producer.merge.has_value() || !producer.left.has_value() || !producer.right.has_value() ||
        !read_registers_->ordered_left_first.has_value() ||
        !read_registers_->ordered_right_first.has_value() ||
        !read_registers_->ordered_comparison.has_value() ||
        !read_registers_->ordered_comparison_register.has_value()) {
      return std::unexpected(InternalFailure("native merge has incomplete producer state"));
    }
    const auto& node = std::get<PhysicalCompoundNode>(read_plan_->node(*producer.merge).payload);
    const auto& left = NativeRegisters(*producer.left);
    const auto& right = NativeRegisters(*producer.right);
    if (!left.row_destination.has_value() || !left.row_destination->coroutine.has_value()) {
      return std::unexpected(InternalFailure("native merge has no declared left coroutine"));
    }
    const CoroutineId a = *left.row_destination->coroutine;
    const auto right_coroutine =
        right.row_destination.has_value() ? right.row_destination->coroutine : std::nullopt;
    if (auto initialized = EmitNativeMergeInitialization(index); !initialized.has_value()) {
      return initialized;
    }
    if (!native_producers_[*producer.left].emits_rows ||
        !native_producers_[*producer.right].emits_rows) {
      return EmitNativeEndOnlyMerge(index, a, right_coroutine);
    }
    if (!right_coroutine.has_value()) {
      return std::unexpected(InternalFailure("native merge has no declared right coroutine"));
    }
    const CoroutineId b = *right_coroutine;
    const RegisterId row_a = *read_registers_->ordered_left_first;
    const RegisterId row_b = *read_registers_->ordered_right_first;
    const bool outputs_b =
        node.operation == CompoundOperator::kUnionAll || node.operation == CompoundOperator::kUnion;
    const bool drains_a = node.operation != CompoundOperator::kIntersect;
    auto compare = CreateLabel();
    auto output_a = CreateLabel();
    auto next_a = CreateLabel();
    auto next_b = CreateLabel();
    auto eof_a_no_b = CreateLabel();
    auto eof_a = CreateLabel();
    auto eof_b = CreateLabel();
    auto done = CreateLabel();
    if (!compare.has_value()) {
      return std::unexpected(std::move(compare.error()));
    }
    if (!output_a.has_value()) {
      return std::unexpected(std::move(output_a.error()));
    }
    if (!next_a.has_value()) {
      return std::unexpected(std::move(next_a.error()));
    }
    if (!next_b.has_value()) {
      return std::unexpected(std::move(next_b.error()));
    }
    if (!eof_a_no_b.has_value()) {
      return std::unexpected(std::move(eof_a_no_b.error()));
    }
    if (!eof_a.has_value()) {
      return std::unexpected(std::move(eof_a.error()));
    }
    if (!eof_b.has_value()) {
      return std::unexpected(std::move(eof_b.error()));
    }
    if (!done.has_value()) {
      return std::unexpected(std::move(done.error()));
    }
    std::optional<Label> output_b;
    std::optional<Label> drain_a;
    std::optional<Label> drain_b;
    if (outputs_b) {
      auto output = CreateLabel();
      auto drain = CreateLabel();
      if (!output.has_value()) {
        return std::unexpected(std::move(output.error()));
      }
      if (!drain.has_value()) {
        return std::unexpected(std::move(drain.error()));
      }
      output_b = *output;
      drain_b = *drain;
    }
    if (drains_a) {
      auto drain = CreateLabel();
      if (!drain.has_value()) {
        return std::unexpected(std::move(drain.error()));
      }
      drain_a = *drain;
    }
    const auto resume = [&](CoroutineId coroutine, Label exhausted,
                            RegisterId destination) -> LoweringResult<void> {
      auto emitted = ConvertProgramResult(
          AssumeValue(builder_).EmitResumeCoroutine(coroutine, exhausted, destination),
          "unable to emit native producer resume");
      if (!emitted.has_value()) {
        return std::unexpected(std::move(emitted.error()));
      }
      return {};
    };
    if (auto initialized = Append(InitCoroutineInstruction{.coroutine = a});
        !initialized.has_value()) {
      return initialized;
    }
    if (auto row = resume(a, *eof_a_no_b, row_a); !row.has_value()) {
      return row;
    }
    if (auto initialized = Append(InitCoroutineInstruction{.coroutine = b});
        !initialized.has_value()) {
      return initialized;
    }
    if (auto row = resume(b, *eof_b, row_b); !row.has_value()) {
      return row;
    }
    if (auto bound = BindLabel(*compare); !bound.has_value()) {
      return bound;
    }
    if (auto compared = Append(CompareRecordsInstruction{
            .comparison = *read_registers_->ordered_comparison,
            .left_first = row_a,
            .right_first = row_b,
            .output = *read_registers_->ordered_comparison_register,
        });
        !compared.has_value()) {
      return compared;
    }
    const Label equal =
        node.operation == CompoundOperator::kUnion || node.operation == CompoundOperator::kExcept
            ? *next_a
            : *output_a;
    if (auto jumped = EmitJumpIf(*read_registers_->ordered_comparison_register,
                                 JumpCondition::kIfFalse, equal);
        !jumped.has_value()) {
      return jumped;
    }
    if (auto less = EmitNativeComparisonBranch(
            SqlComparison::kLess,
            node.operation == CompoundOperator::kIntersect ? *next_a : *output_a);
        !less.has_value()) {
      return less;
    }
    if (auto greater = EmitJump(output_b.value_or(*next_b)); !greater.has_value()) {
      return greater;
    }
    if (auto bound = BindLabel(*output_a); !bound.has_value()) {
      return bound;
    }
    if (auto output = EmitNativeOutputRow(row_a, *next_a, *done); !output.has_value()) {
      return output;
    }
    if (auto advanced = EmitJump(*next_a); !advanced.has_value()) {
      return advanced;
    }
    if (output_b.has_value()) {
      if (auto bound = BindLabel(*output_b); !bound.has_value()) {
        return bound;
      }
      if (auto output = EmitNativeOutputRow(row_b, *next_b, *done); !output.has_value()) {
        return output;
      }
      if (auto advanced = EmitJump(*next_b); !advanced.has_value()) {
        return advanced;
      }
    }
    if (auto bound = BindLabel(*next_a); !bound.has_value()) {
      return bound;
    }
    if (auto row = resume(a, *eof_a, row_a); !row.has_value()) {
      return row;
    }
    if (auto jumped = EmitJump(*compare); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*next_b); !bound.has_value()) {
      return bound;
    }
    if (auto row = resume(b, *eof_b, row_b); !row.has_value()) {
      return row;
    }
    if (auto jumped = EmitJump(*compare); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*eof_a_no_b); !bound.has_value()) {
      return bound;
    }
    if (outputs_b) {
      if (auto initialized = Append(InitCoroutineInstruction{.coroutine = b});
          !initialized.has_value()) {
        return initialized;
      }
      if (auto row = resume(b, *done, row_b); !row.has_value()) {
        return row;
      }
      if (auto jumped = EmitJump(*drain_b); !jumped.has_value()) {
        return jumped;
      }
    } else if (auto jumped = EmitJump(*done); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*eof_a); !bound.has_value()) {
      return bound;
    }
    if (auto jumped = EmitJump(drain_b.value_or(*done)); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*eof_b); !bound.has_value()) {
      return bound;
    }
    if (auto jumped = EmitJump(drain_a.value_or(*done)); !jumped.has_value()) {
      return jumped;
    }
    if (drain_a.has_value()) {
      if (auto emitted = EmitNativeDrain(*drain_a, a, row_a, *done); !emitted.has_value()) {
        return emitted;
      }
    }
    if (drain_b.has_value()) {
      if (auto emitted = EmitNativeDrain(*drain_b, b, row_b, *done); !emitted.has_value()) {
        return emitted;
      }
    }
    if (auto bound = BindLabel(*done); !bound.has_value()) {
      return bound;
    }
    if (auto cancelled = Append(CancelCoroutineInstruction{.coroutine = a});
        !cancelled.has_value()) {
      return cancelled;
    }
    return Append(CancelCoroutineInstruction{.coroutine = b});
  }

  [[nodiscard]] LoweringResult<void> EmitNativeProducerBody(std::size_t index) {
    ReadRegisterLayout* previous = read_registers_;
    const PhysicalCoreOrderPlan* previous_order = active_core_order_;
    read_registers_ = &NativeRegisters(index);
    active_core_order_ = nullptr;
    ClearStreamingAggregateContext();
    std::vector<ReadEmissionBinding> restore;
    const auto emitted = [&]() -> LoweringResult<void> {
      const ReadEmissionBinding* binding =
          lexical_parent_.has_value() ? &*lexical_parent_ : nullptr;
      while (binding != nullptr) {
        PlanLowerer& frame = *binding->frame;
        restore.push_back(frame.CurrentReadEmissionBinding());
        if (auto applied = frame.ApplyReadEmissionBinding(*binding); !applied.has_value()) {
          return applied;
        }
        binding = frame.lexical_parent_.has_value() ? &*frame.lexical_parent_ : nullptr;
      }
      const NativeProducer& producer = native_producers_[index];
      if (!producer.merge.has_value() && producer.source_count == 1U) {
        active_core_order_ = &read_plan_->core_order_plan(producer.source_begin);
        SetStreamingCoreContext(streaming_cores_[producer.source_begin]);
        if (!read_registers_->row_destination.has_value() ||
            !read_registers_->row_destination->final_completion.has_value()) {
          return std::unexpected(InternalFailure("native source has no closed completion target"));
        }
        if (!HasOrdering() || active_core_order_->strategy != PhysicalSortStrategy::kRuntimeLimit) {
          if (auto limit =
                  EmitLimitInitialization(read_registers_->row_destination->final_completion);
              !limit.has_value()) {
            return limit;
          }
        }
        return EmitPlan();
      }
      auto completed = CreateLabel();
      if (!completed.has_value()) {
        return std::unexpected(std::move(completed.error()));
      }
      if (auto initialized = EmitLimitInitialization(*completed); !initialized.has_value()) {
        return initialized;
      }
      if (producer.merge.has_value()) {
        if (auto merged = EmitNativeMerge(index); !merged.has_value()) {
          return merged;
        }
        if (producer.source_count != 0U && HasLimit()) {
          auto remaining = CreateLabel();
          if (!remaining.has_value()) {
            return std::unexpected(std::move(remaining.error()));
          }
          if (auto unlimited = EmitJumpIf(*read_registers_->negative_limit_register,
                                          JumpCondition::kIfTrue, *remaining);
              !unlimited.has_value()) {
            return unlimited;
          }
          if (auto exhausted =
                  EmitJumpIf(*read_registers_->limit_register, JumpCondition::kIfFalse, *completed);
              !exhausted.has_value()) {
            return exhausted;
          }
          if (auto bound = BindLabel(*remaining); !bound.has_value()) {
            return bound;
          }
        }
      }
      for (std::size_t source = producer.source_begin;
           source < producer.source_begin + producer.source_count; ++source) {
        if (!read_plan_->core_order_plan(source).terms.empty()) {
          return std::unexpected(
              InternalFailure("native concatenated source unexpectedly requires ordering"));
        }
        SetStreamingCoreContext(streaming_cores_[source]);
        if (auto rows = EmitStreamingCore(streaming_cores_[source], *completed);
            !rows.has_value()) {
          return rows;
        }
      }
      if (auto bound = BindLabel(*completed); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }();
    read_registers_ = previous;
    active_core_order_ = previous_order;
    ClearStreamingAggregateContext();
    for (const auto& binding : restore | std::views::reverse) {
      if (auto restored = binding.frame->ApplyReadEmissionBinding(binding); !restored.has_value()) {
        return restored;
      }
    }
    return emitted;
  }

  [[nodiscard]] LoweringResult<void> EmitNativeCoroutineBodies() {
    for (std::size_t index = 1; index < native_producers_.size(); ++index) {
      const NativeProducer& producer = native_producers_[index];
      if (!producer.entry.has_value()) {
        continue;
      }
      const auto& destination = NativeRegisters(index).row_destination;
      if (!destination.has_value() || !destination->coroutine.has_value() ||
          !destination->final_completion.has_value()) {
        return std::unexpected(InternalFailure("native coroutine body lost its destination"));
      }
      if (auto entry = BindLabel(*producer.entry); !entry.has_value()) {
        return entry;
      }
      if (auto body = EmitNativeProducerBody(index); !body.has_value()) {
        return body;
      }
      if (auto last = BindLabel(*destination->final_completion); !last.has_value()) {
        return last;
      }
      if (auto ended = Append(EndCoroutineInstruction{.coroutine = *destination->coroutine});
          !ended.has_value()) {
        return ended;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitNativeWrapperInput() {
    if (wrapper_order_layout_ == nullptr || read_registers_->row_destination.has_value() ||
        !read_registers_->sorter_record_first.has_value()) {
      return std::unexpected(InternalFailure("native outer ordering has invalid row destination"));
    }
    auto completed = CreateLabel();
    if (!completed.has_value()) {
      return std::unexpected(std::move(completed.error()));
    }
    read_registers_->row_destination = SelectOutputContext{
        .mode = SelectOutputMode::Ordered,
        .final_completion = *completed,
        .ordered_layout = wrapper_order_layout_,
        .ordered_sorter = read_registers_->ordering_runtime == OrderingRuntime::kSorter
                              ? read_registers_->sorter
                              : std::nullopt,
        .ordered_top_n = read_registers_->ordering_runtime == OrderingRuntime::kTopN
                             ? read_registers_->top_n
                             : std::nullopt,
        .ordered_record_first = *read_registers_->sorter_record_first,
    };
    auto input = EmitNativeProducerBody(0);
    read_registers_->row_destination.reset();
    if (!input.has_value()) {
      return input;
    }
    if (auto bound = BindLabel(*completed); !bound.has_value()) {
      return bound;
    }
    return EmitOrdinaryReadCompletion();
  }

  [[nodiscard]] LoweringResult<void> EmitNativeCompoundPlan() {
    if (!native_compound_root_.has_value() || native_producers_.empty()) {
      return std::unexpected(InternalFailure("native compound plan has no producer scopes"));
    }
    if (wrapper_order_layout_ != nullptr) {
      return EmitPlan();
    }
    if (auto body = EmitNativeProducerBody(0); !body.has_value()) {
      return body;
    }
    if (read_registers_->row_destination.has_value()) {
      const auto completion = read_registers_->row_destination->final_completion;
      read_registers_->row_destination.reset();
      if (!completion.has_value()) {
        return std::unexpected(InternalFailure("native root source has no closed completion"));
      }
      if (auto bound = BindLabel(*completion); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
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
          .input = RegisterId{read_registers_->result_block_first.value() + value.field_index},
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
                                                          RegisterId record_first, Label rejected) {
    if (sorter.has_value() == top_n.has_value()) {
      return std::unexpected(InternalFailure("ordered compound candidate capability is invalid"));
    }
    const auto key_count = static_cast<std::uint32_t>(layout.key_values.size());
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
    if (HasBlockOrdering()) {
      if (layout.schedule == OrderEvaluationSchedule::kPayloadThenKeys) {
        if (auto payload = emit_payload(); !payload.has_value()) {
          return payload;
        }
        if (auto keys = emit_keys(); !keys.has_value()) {
          return keys;
        }
      } else {
        if (auto keys = emit_keys(); !keys.has_value()) {
          return keys;
        }
        if (auto payload = emit_payload(); !payload.has_value()) {
          return payload;
        }
      }
      return EmitOrderBlockCandidate(
          record_first,
          static_cast<std::uint32_t>(layout.key_values.size() + layout.payload_values.size()),
          rejected);
    }
    if (layout.schedule == OrderEvaluationSchedule::kPayloadThenKeys) {
      if (auto payload = emit_payload(); !payload.has_value()) {
        return payload;
      }
      if (auto keys = emit_keys(); !keys.has_value()) {
        return keys;
      }
      if (top_n.has_value()) {
        auto checked = ConvertProgramResult(
            AssumeValue(builder_).EmitCheckTopN(*top_n, record_first, key_count, rejected),
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
        auto checked = ConvertProgramResult(
            AssumeValue(builder_).EmitCheckTopN(*top_n, record_first, key_count, rejected),
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
                                                           RegisterId record_first) {
    if (core.join != nullptr) {
      return EmitJoinedCore(core, select,
                            SelectOutputContext{
                                .mode = SelectOutputMode::Ordered,
                                .core = &core,
                                .ordered_layout = &layout,
                                .ordered_sorter = sorter,
                                .ordered_top_n = top_n,
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
    if (core.empty != nullptr) {
      return BindLabel(*skipped);
    }
    if (core.relation.has_value()) {
      if (auto opened = Append(OpenRelationInstruction{.relation = *core.relation});
          !opened.has_value()) {
        return opened;
      }
    }
    const bool complete_row = OrderedLayoutUsesInputFields(layout);
    if (IsCursorAccess()) {
      if (!core.cursor.has_value()) {
        return std::unexpected(InternalFailure("ordered compound cursor core has no cursor"));
      }
      auto done = CreateLabel();
      auto closed = CreateLabel();
      std::optional<Label> positioned_done;
      if (!done.has_value()) {
        return std::unexpected(std::move(done.error()));
      }
      if (!closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
      }
      if (core.rowid_lookup != nullptr ||
          (core.rowid_range != nullptr && RowIdRangeHasEnd(*core.rowid_range))) {
        auto label = CreateLabel();
        if (!label.has_value()) {
          return std::unexpected(std::move(label.error()));
        }
        positioned_done = *label;
      }
      const auto consume = [&](Label advance) -> LoweringResult<void> {
        if (auto filters = EmitStreamingFilters(core, advance); !filters.has_value()) {
          return filters;
        }
        if (complete_row) {
          if (auto projected = EmitStreamingProjection(select, SelectOutputMode::Ordered);
              !projected.has_value()) {
            return projected;
          }
          if (auto membership = EmitStreamingMembership(core, advance); !membership.has_value()) {
            return membership;
          }
        }
        return EmitOrderedCandidate(layout, sorter, top_n, record_first, advance);
      };
      if (auto rows = EmitStreamingCursorRows(core, *done, positioned_done, std::nullopt, consume);
          !rows.has_value()) {
        return rows;
      }
      if (auto bound = BindLabel(*done); !bound.has_value()) {
        return bound;
      }
      if (auto close = EmitStreamingCursorClose(core); !close.has_value()) {
        return close;
      }
      if (auto jumped = EmitJump(*closed); !jumped.has_value()) {
        return jumped;
      }
      if (positioned_done.has_value()) {
        if (auto bound = BindLabel(*positioned_done); !bound.has_value()) {
          return bound;
        }
        if (auto close = EmitStreamingCursorClose(core); !close.has_value()) {
          return close;
        }
        if (auto jumped = EmitJump(*closed); !jumped.has_value()) {
          return jumped;
        }
      }
      if (auto bound = BindLabel(*closed); !bound.has_value()) {
        return bound;
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
      if (auto candidate = EmitOrderedCandidate(layout, sorter, top_n, record_first, *skipped);
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
                                                           RegisterId record_first) {
    for (const std::vector<BoundExpressionId>& row : values.rows) {
      auto rejected = CreateLabel();
      if (!rejected.has_value()) {
        return std::unexpected(std::move(rejected.error()));
      }
      if (row.size() != bound_select_->result_columns().size()) {
        return std::unexpected(InternalFailure("ordered compound VALUES width is invalid"));
      }
      for (std::size_t index = 0; index < row.size(); ++index) {
        if (auto emitted =
                EmitExpression(row[index], RegisterId{read_registers_->result_block_first.value() +
                                                      static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return emitted;
        }
      }
      if (auto candidate = EmitOrderedCandidate(layout, sorter, top_n, record_first, *rejected);
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
                                                         std::optional<SorterId> sorter,
                                                         std::optional<TopNId> top_n) {
    const auto record_first = read_registers_->sorter_record_first;
    if (active_core_order_ == nullptr || !record_first.has_value() ||
        core.core_index >= read_plan_->core_order_plans().size()) {
      return std::unexpected(InternalFailure("ordered compound core resources are incomplete"));
    }
    const LogicalCoreOrderLayout& layout = read_plan_->core_order_plans()[core.core_index].layout;
    if (core.aggregate != nullptr) {
      return EmitAggregateCore(core, SelectOutputContext{
                                         .mode = SelectOutputMode::Ordered,
                                         .core = &core,
                                         .ordered_layout = &layout,
                                         .ordered_sorter = sorter,
                                         .ordered_top_n = top_n,
                                         .ordered_record_first = *record_first,
                                     });
    }
    if (const auto* values = std::get_if<BoundValuesCore>(core.bound); values != nullptr) {
      if (auto emitted = EmitOrderedValuesCore(*values, layout, sorter, top_n, *record_first);
          !emitted.has_value()) {
        return emitted;
      }
    } else {
      const auto& select = std::get<BoundSelectCore>(*core.bound);
      if (auto emitted = EmitOrderedSelectCore(core, select, layout, sorter, top_n, *record_first);
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
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
      if (auto emitted = EmitExpression(projection->expressions[index],
                                        RegisterId{read_registers_->result_block_first.value() +
                                                   static_cast<std::uint32_t>(index)});
          !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctMembership(Label duplicate) {
    if (!read_registers_->distinct_relation.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT relation descriptor is unavailable"));
    }
    auto inserted = ConvertProgramResult(
        AssumeValue(builder_).EmitInsertRelation(
            *read_registers_->distinct_relation, read_registers_->result_block_first,
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
          .input = RegisterId{read_registers_->result_block_first.value() + value.field_index},
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
    if (advanced_order_ == nullptr || !read_registers_->sorter.has_value() ||
        !read_registers_->sorter_record_first.has_value() ||
        read_plan_->core_order_plans().size() != 1U) {
      return std::unexpected(InternalFailure("DISTINCT ordered candidate is incomplete"));
    }
    const LogicalCoreOrderLayout& layout = read_plan_->core_order_plans()[0].layout;
    if (layout.schedule != OrderEvaluationSchedule::kPayloadThenKeys ||
        layout.key_values.size() != advanced_order_->terms.size()) {
      return std::unexpected(InternalFailure("DISTINCT order schedule is invalid"));
    }
    const RegisterId first = AssumeValue(read_registers_->sorter_record_first);
    const SorterId sorter = AssumeValue(read_registers_->sorter);
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
    if (!HasLimit()) {
      return {};
    }
    if (!read_registers_->negative_limit_register.has_value() ||
        !read_registers_->limit_register.has_value() ||
        !read_registers_->one_register.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT LIMIT has no row-loop counters"));
    }
    const RegisterId negative = AssumeValue(read_registers_->negative_limit_register);
    const RegisterId limit = AssumeValue(read_registers_->limit_register);
    const RegisterId one = AssumeValue(read_registers_->one_register);
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
    if (advanced_order_ == nullptr || !read_registers_->sorter.has_value() ||
        read_plan_->core_order_plans().size() != 1U) {
      return std::unexpected(InternalFailure("DISTINCT sorter drain is incomplete"));
    }
    const LogicalCoreOrderLayout& layout = read_plan_->core_order_plans()[0].layout;
    if (layout.output_fields.size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("DISTINCT sorter output shape is invalid"));
    }
    const SorterId sorter = AssumeValue(read_registers_->sorter);

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
    if (HasLimit()) {
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
    if (HasLimit()) {
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
              .output = RegisterId{read_registers_->result_block_first.value() +
                                   static_cast<std::uint32_t>(index)},
          });
          !read.has_value()) {
        return read;
      }
    }
    if (auto result = Append(ResultRowInstruction{
            .first = read_registers_->result_block_first,
            .count = static_cast<std::uint32_t>(layout.output_fields.size()),
        });
        !result.has_value()) {
      return result;
    }
    if (HasLimit()) {
      if (!read_registers_->negative_limit_register.has_value() ||
          !read_registers_->limit_register.has_value() ||
          !read_registers_->one_register.has_value() || !positioned_completion.has_value()) {
        return std::unexpected(InternalFailure("DISTINCT ordered LIMIT has no counters"));
      }
      const RegisterId negative = AssumeValue(read_registers_->negative_limit_register);
      const RegisterId limit = AssumeValue(read_registers_->limit_register);
      const RegisterId one = AssumeValue(read_registers_->one_register);
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

  [[nodiscard]] LoweringResult<void> EmitDistinctOpenedCompletion(const StreamingCorePlan* access) {
    if (access != nullptr) {
      if (auto closed = EmitStreamingCursorClose(*access); !closed.has_value()) {
        return closed;
      }
    }
    if (!read_registers_->distinct_relation.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT completion has no relation"));
    }
    if (auto closed = Append(
            CloseRelationInstruction{.relation = AssumeValue(read_registers_->distinct_relation)});
        !closed.has_value()) {
      return closed;
    }
    return advanced_order_ == nullptr ? Append(HaltInstruction{}) : EmitDistinctSorterDrain();
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctEmpty() {
    auto completion = CreateLabel();
    if (!completion.has_value()) {
      return std::unexpected(std::move(completion.error()));
    }
    if (auto limit = EmitLimitInitialization(*completion); !limit.has_value()) {
      return limit;
    }
    if (auto guards = EmitDistinctGuards(*completion); !guards.has_value()) {
      return guards;
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return Append(HaltInstruction{});
  }

  [[nodiscard]] LoweringResult<void> EmitDistinctTableScan() {
    if (!IsCursorAccess() || !read_registers_->cursor.has_value() ||
        !read_registers_->distinct_relation.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT cursor resources are incomplete"));
    }
    const RelationId relation = AssumeValue(read_registers_->distinct_relation);
    std::optional<Label> closed_completion;
    if (HasLimit() || (filter_ != nullptr && !filter_->guards.empty())) {
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
      if (!read_registers_->sorter.has_value()) {
        return std::unexpected(InternalFailure("DISTINCT order has no sorter"));
      }
      if (auto opened =
              Append(OpenSorterInstruction{.sorter = AssumeValue(read_registers_->sorter)});
          !opened.has_value()) {
        return opened;
      }
    }
    auto unpositioned_completion = CreateLabel();
    std::optional<Label> positioned_completion;
    if (!unpositioned_completion.has_value()) {
      return std::unexpected(std::move(unpositioned_completion.error()));
    }
    if ((HasLimit() && advanced_order_ == nullptr) || rowid_lookup_ != nullptr ||
        (rowid_range_ != nullptr && RowIdRangeHasEnd(*rowid_range_))) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      positioned_completion = *label;
    }
    StreamingCorePlan access;
    access.table_scan = table_scan_;
    access.rowid_lookup = rowid_lookup_;
    access.rowid_range = rowid_range_;
    access.index_scan = index_scan_;
    access.cursor = read_registers_->cursor;
    access.index_cursor = read_registers_->index_cursor;
    access.filter = filter_;
    const auto consume = [&](Label advance) -> LoweringResult<void> {
      if (auto filters = EmitFilters(advance); !filters.has_value()) {
        return filters;
      }
      if (auto projected = EmitDistinctProjection(); !projected.has_value()) {
        return projected;
      }
      if (auto inserted = EmitDistinctMembership(advance); !inserted.has_value()) {
        return inserted;
      }
      if (advanced_order_ != nullptr) {
        return EmitDistinctOrderedCandidate();
      }
      if (auto offset = EmitOffset(advance); !offset.has_value()) {
        return offset;
      }
      if (auto result = Append(ResultRowInstruction{
              .first = read_registers_->result_block_first,
              .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
          });
          !result.has_value()) {
        return result;
      }
      if (!HasLimit()) {
        return {};
      }
      if (!positioned_completion.has_value()) {
        return std::unexpected(InternalFailure("DISTINCT LIMIT completion is unavailable"));
      }
      return EmitDistinctLimitAfterRow(DistinctLimitTargets{
          .advance = advance,
          .completion = AssumeValue(positioned_completion),
      });
    };
    if (auto rows = EmitStreamingCursorRows(access, *unpositioned_completion, positioned_completion,
                                            std::nullopt, consume);
        !rows.has_value()) {
      return rows;
    }
    if (auto bound = BindLabel(*unpositioned_completion); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitDistinctOpenedCompletion(&access); !completed.has_value()) {
      return completed;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitDistinctOpenedCompletion(&access); !completed.has_value()) {
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
    if (!read_registers_->distinct_relation.has_value()) {
      return std::unexpected(InternalFailure("DISTINCT single row has no relation"));
    }
    const RelationId relation = AssumeValue(read_registers_->distinct_relation);
    std::optional<Label> closed_completion;
    if (HasLimit() || (filter_ != nullptr && !filter_->guards.empty())) {
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
      if (!read_registers_->sorter.has_value()) {
        return std::unexpected(InternalFailure("DISTINCT order has no sorter"));
      }
      if (auto opened =
              Append(OpenSorterInstruction{.sorter = AssumeValue(read_registers_->sorter)});
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
              .first = read_registers_->result_block_first,
              .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
          });
          !result.has_value()) {
        return result;
      }
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitDistinctOpenedCompletion(nullptr); !completed.has_value()) {
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
    if (std::holds_alternative<PhysicalEmptyNode>(leaf_->payload)) {
      return EmitDistinctEmpty();
    }
    if (IsCursorAccess()) {
      return EmitDistinctTableScan();
    }
    if (std::holds_alternative<PhysicalSingleRowNode>(leaf_->payload)) {
      return EmitDistinctSingleRow();
    }
    return std::unexpected(UnsupportedFailure("DISTINCT access plan is not implemented"));
  }

  [[nodiscard]] LoweringResult<void> EmitAccessPlan() {
    if (auto initialized = InitializeOrderBlock(); !initialized.has_value()) {
      return initialized;
    }
    if (native_compound_root_.has_value() && wrapper_order_layout_ != nullptr &&
        read_registers_ == &pooled_read_registers_ && active_core_order_ == nullptr &&
        !read_registers_->row_destination.has_value()) {
      return EmitNativeWrapperInput();
    }
    if (active_core_order_ != nullptr && !read_registers_->active_aggregate_output.has_value()) {
      const std::size_t core_index = active_core_order_->layout.core_index;
      if (core_index >= streaming_cores_.size()) {
        return std::unexpected(InternalFailure("active ordered producer has no source core"));
      }
      if (!read_registers_->row_destination.has_value() ||
          !read_registers_->row_destination->final_completion.has_value()) {
        return std::unexpected(InternalFailure("active source has no coroutine completion"));
      }
      const Label completion = *read_registers_->row_destination->final_completion;
      const StreamingCorePlan& core = streaming_cores_[core_index];
      if (HasOrdering()) {
        const auto sorter = read_registers_->ordering_runtime == OrderingRuntime::kSorter
                                ? read_registers_->sorter
                                : std::nullopt;
        const auto top_n = read_registers_->ordering_runtime == OrderingRuntime::kTopN
                               ? read_registers_->top_n
                               : std::nullopt;
        if (auto rows = EmitOrderedCoreRows(core, sorter, top_n); !rows.has_value()) {
          return rows;
        }
        return EmitOrdinaryReadCompletion();
      } else if (auto rows = EmitStreamingCore(core, completion); !rows.has_value()) {
        return rows;
      }
      return Append(HaltInstruction{});
    }
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
    if (const auto* range = std::get_if<PhysicalRowIdRangeNode>(&leaf_->payload);
        range != nullptr) {
      return EmitRowIdRange(*range);
    }
    if (const auto* index = std::get_if<PhysicalIndexScanNode>(&leaf_->payload); index != nullptr) {
      return index->covering ? EmitCoveringIndexScan(*index) : EmitNoncoveringIndexScan(*index);
    }
    return std::unexpected(InternalFailure("physical access node is unsupported"));
  }

  [[nodiscard]] LoweringResult<void> EmitFiniteLimitBound(Label unbounded) {
    if (!HasLimit() || !read_registers_->negative_limit_register.has_value() ||
        !read_registers_->top_n_bound_register.has_value()) {
      return std::unexpected(InternalFailure("finite row bound has incomplete counters"));
    }
    if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->negative_limit_register),
                                 JumpCondition::kIfTrue, unbounded);
        !jumped.has_value()) {
      return jumped;
    }

    if (read_registers_->offset_register.has_value()) {
      if (!read_registers_->offset_register.has_value() ||
          !read_registers_->maximum_integer_register.has_value() ||
          !read_registers_->bound_room_register.has_value() ||
          !read_registers_->comparison_register.has_value()) {
        return std::unexpected(InternalFailure("runtime LIMIT bound has incomplete registers"));
      }
      auto maximum = EnsureMaximumIntegerConstant();
      if (!maximum.has_value()) {
        return std::unexpected(std::move(maximum.error()));
      }
      if (auto loaded = Append(LoadConstantInstruction{
              .constant = *maximum,
              .output = AssumeValue(read_registers_->maximum_integer_register),
          });
          !loaded.has_value()) {
        return loaded;
      }
      if (auto room = Append(BinaryInstruction{
              .operation = BinaryOperation::kSubtract,
              .left = AssumeValue(read_registers_->maximum_integer_register),
              .right = AssumeValue(read_registers_->offset_register),
              .output = AssumeValue(read_registers_->bound_room_register),
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
              .left = AssumeValue(read_registers_->bound_room_register),
              .right = AssumeValue(read_registers_->limit_register),
              .output = AssumeValue(read_registers_->comparison_register),
          });
          !overflow.has_value()) {
        return overflow;
      }
      if (auto jumped = EmitJumpIf(AssumeValue(read_registers_->comparison_register),
                                   JumpCondition::kIfTrue, unbounded);
          !jumped.has_value()) {
        return jumped;
      }
      if (auto bound = Append(BinaryInstruction{
              .operation = BinaryOperation::kAdd,
              .left = AssumeValue(read_registers_->limit_register),
              .right = AssumeValue(read_registers_->offset_register),
              .output = AssumeValue(read_registers_->top_n_bound_register),
          });
          !bound.has_value()) {
        return bound;
      }
    } else if (auto copied = Append(CopyInstruction{
                   .input = AssumeValue(read_registers_->limit_register),
                   .output = AssumeValue(read_registers_->top_n_bound_register),
               });
               !copied.has_value()) {
      return copied;
    }

    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitNativeInputBound() {
    if (!read_registers_->zero_register.has_value() || !read_registers_->one_register.has_value() ||
        !read_registers_->top_n_bound_register.has_value()) {
      return std::unexpected(InternalFailure("native input bound has no control constants"));
    }
    auto unbounded = CreateLabel();
    auto ready = CreateLabel();
    if (!unbounded.has_value()) {
      return std::unexpected(std::move(unbounded.error()));
    }
    if (!ready.has_value()) {
      return std::unexpected(std::move(ready.error()));
    }
    if (auto finite = EmitFiniteLimitBound(*unbounded); !finite.has_value()) {
      return finite;
    }
    if (auto jumped = EmitJump(*ready); !jumped.has_value()) {
      return jumped;
    }
    if (auto bound = BindLabel(*unbounded); !bound.has_value()) {
      return bound;
    }
    if (auto unlimited = Append(BinaryInstruction{
            .operation = BinaryOperation::kSubtract,
            .left = *read_registers_->zero_register,
            .right = *read_registers_->one_register,
            .output = *read_registers_->top_n_bound_register,
        });
        !unlimited.has_value()) {
      return unlimited;
    }
    return BindLabel(*ready);
  }

  [[nodiscard]] LoweringResult<void> EmitRuntimeLimitOrderedPlan() {
    const auto sorter = read_registers_->sorter;
    const auto top_n = read_registers_->top_n;
    const bool runtime_limit =
        active_core_order_ != nullptr
            ? active_core_order_->strategy == PhysicalSortStrategy::kRuntimeLimit
        : wrapper_order_layout_ != nullptr
            ? advanced_order_->final_strategy == PhysicalSortStrategy::kRuntimeLimit
            : sort_ != nullptr && sort_->strategy == PhysicalSortStrategy::kRuntimeLimit;
    if (!runtime_limit || !HasLimit() || !sorter.has_value() || !top_n.has_value() ||
        !read_registers_->top_n_bound_register.has_value() ||
        !read_registers_->limit_register.has_value() ||
        !read_registers_->negative_limit_register.has_value()) {
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
    if (auto bound = EmitFiniteLimitBound(*external); !bound.has_value()) {
      return bound;
    }

    read_registers_->ordering_runtime = OrderingRuntime::kTopN;
    if (auto opened = Append(OpenTopNInstruction{
            .top_n = *top_n,
            .bound = AssumeValue(read_registers_->top_n_bound_register),
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
    read_registers_->ordering_runtime = OrderingRuntime::kSorter;
    if (auto opened = Append(OpenSorterInstruction{.sorter = *sorter}); !opened.has_value()) {
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
            .rowid_range = rowid_range_,
            .index_scan = index_scan_,
            .filter = filter_,
            .limit = read_registers_->active_aggregate_output.has_value() ? nullptr : limit_,
            .aggregate = aggregate_,
            .cursor = read_registers_->cursor,
            .index_cursor = read_registers_->index_cursor,
            .index_key_first = read_registers_->index_key_first,
            .index_key_capacity = read_registers_->index_key_capacity,
            .index_rowid_register = read_registers_->index_rowid_register,
            .rowid_range_rowid_register = read_registers_->rowid_range_rowid_register,
            .rowid_range_comparison_register = read_registers_->rowid_range_comparison_register,
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
        grouped_aggregate_query_ ? read_registers_->grouped_representative_effect_register
                                 : read_registers_->global_representative_effect_register;
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
        .global_seen_row_register = read_registers_->global_modifier_seen_row_register,
        .zero_constant = zero_constant_,
        .global_filtered_controller_defaults = read_registers_->global_filtered_controller_defaults,
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
    if (read_registers_->grouped_representative_replace_register.has_value() &&
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
        !read_registers_->grouped_current_group_first.has_value() ||
        !read_registers_->grouped_previous_group_first.has_value() ||
        !read_registers_->grouped_has_group_register.has_value() ||
        !read_registers_->grouped_comparison_register.has_value() || !zero_constant_.has_value() ||
        !one_constant_.has_value() || !binary_symbol_.has_value()) {
      return std::unexpected(InternalFailure("grouped aggregate runtime input is incomplete"));
    }
    lowering_internal::GroupedAggregateInput input{
        .builder = &*builder_,
        .program = aggregate_program_,
        .aggregate = aggregate_,
        .collation_symbols = collation_symbols_,
        .retained_leaves = grouped_retained_leaves_,
        .retained_leaf_registers = read_registers_->grouped_retained_leaf_registers,
        .representative_registers = read_registers_->grouped_representative_registers,
        .group_sorter = read_registers_->grouped_sorter,
        .group_sorter_record_first = read_registers_->grouped_sorter_record_first,
        .current_group_first = AssumeValue(read_registers_->grouped_current_group_first),
        .previous_group_first = AssumeValue(read_registers_->grouped_previous_group_first),
        .has_group_register = AssumeValue(read_registers_->grouped_has_group_register),
        .comparison_register = AssumeValue(read_registers_->grouped_comparison_register),
        .representative_effect_register = read_registers_->grouped_representative_effect_register,
        .representative_replace_register = read_registers_->grouped_representative_replace_register,
        .representative_replace_constant = grouped_representative_replace_constant_,
        .binary_collation = *binary_symbol_,
        .zero_constant = *zero_constant_,
        .one_constant = *one_constant_,
        .has_representative_controller = !aggregate_program_->representative_leaves.empty() &&
                                         HasAggregateRepresentativeController(),
        .may_stop_early =
            read_registers_->active_aggregate_output.has_value()
                ? read_registers_->active_aggregate_output->mode == SelectOutputMode::Streaming &&
                      HasLimit()
                : HasLimit() && !HasOrdering(),
        .stops_after_output = false,
    };
    return input;
  }

  [[nodiscard]] LoweringResult<void> EmitGroupedAggregatePlan() {
    if (auto constants = PrepareGroupedAggregateConstants(); !constants.has_value()) {
      return constants;
    }
    if (read_registers_->active_aggregate_output.has_value()) {
      return EmitAccessPlan();
    }
    if (HasOrdering()) {
      if (sort_->strategy == PhysicalSortStrategy::kRuntimeLimit) {
        return EmitRuntimeLimitOrderedPlan();
      }
      if (sort_->strategy != PhysicalSortStrategy::kExternal ||
          !read_registers_->sorter.has_value()) {
        return std::unexpected(
            InternalFailure("grouped external ordering resources are incomplete"));
      }
      read_registers_->ordering_runtime = OrderingRuntime::kSorter;
      if (auto opened = Append(OpenSorterInstruction{.sorter = *read_registers_->sorter});
          !opened.has_value()) {
        return opened;
      }
      return EmitAccessPlan();
    }

    std::optional<Label> limit_zero;
    if (HasLimit()) {
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
    if (!HasOrdering()) {
      return EmitAccessPlan();
    }
    const auto strategy = active_core_order_ != nullptr ? active_core_order_->strategy
                          : sort_ != nullptr            ? sort_->strategy
                                                        : advanced_order_->final_strategy;
    if (strategy == PhysicalSortStrategy::kRuntimeLimit) {
      return EmitRuntimeLimitOrderedPlan();
    }
    if (!read_registers_->sorter.has_value()) {
      return std::unexpected(InternalFailure("external ORDER BY has no sorter descriptor"));
    }
    read_registers_->ordering_runtime = OrderingRuntime::kSorter;
    if (auto opened = Append(OpenSorterInstruction{.sorter = AssumeValue(read_registers_->sorter)});
        !opened.has_value()) {
      return opened;
    }
    return EmitAccessPlan();
  }

  [[nodiscard]] bool IsRowLoopAccess() const noexcept {
    return table_scan_ != nullptr || rowid_range_ != nullptr || index_scan_ != nullptr;
  }

  [[nodiscard]] bool IsCursorAccess() const noexcept {
    return table_scan_ != nullptr || rowid_lookup_ != nullptr || rowid_range_ != nullptr ||
           index_scan_ != nullptr;
  }

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
  ReadPlanKind read_plan_kind_ = ReadPlanKind::kOrdinary;
  bool read_descriptors_added_ = false;
  // Borrows compiler metadata only; captured SQL values stay at reached read sites.
  std::optional<ReadEmissionBinding> lexical_parent_{};
  const StreamingCorePlan* active_streaming_core_ = nullptr;

  const PhysicalNode* leaf_ = nullptr;
  const PhysicalTableScanNode* table_scan_ = nullptr;
  const PhysicalRowIdLookupNode* rowid_lookup_ = nullptr;
  const PhysicalRowIdRangeNode* rowid_range_ = nullptr;
  const PhysicalIndexScanNode* index_scan_ = nullptr;
  const PhysicalGuardNode* guard_ = nullptr;
  const PhysicalFilterNode* filter_ = nullptr;
  const PhysicalLimitNode* limit_ = nullptr;
  const PhysicalProjectionNode* projection_ = nullptr;
  const PhysicalSortNode* sort_ = nullptr;
  const PhysicalOutputNode* output_ = nullptr;
  const PhysicalDistinctNode* distinct_ = nullptr;
  const PhysicalAdvancedOrderNode* advanced_order_ = nullptr;
  const PhysicalCoreOrderPlan* active_core_order_ = nullptr;
  const LogicalCoreOrderLayout* wrapper_order_layout_ = nullptr;
  const PhysicalAggregateNode* aggregate_ = nullptr;
  const PhysicalHavingNode* having_ = nullptr;
  const BoundSelectCore* aggregate_core_ = nullptr;
  const BoundAggregateProgram* aggregate_program_ = nullptr;
  bool global_aggregate_query_ = false;
  bool global_representative_query_ = false;
  bool grouped_aggregate_query_ = false;
  bool streaming_aggregate_query_ = false;
  bool streaming_query_ = false;
  bool streaming_requires_snapshot_ = false;
  std::size_t active_source_column_begin_ = 0;
  std::size_t active_source_column_count_ = 0;
  std::vector<StreamingCorePlan> streaming_cores_;
  std::optional<PhysicalNodeId> native_compound_root_;
  std::vector<NativeProducer> native_producers_;
  ReadRegisterLayout pooled_read_registers_;
  ReadRegisterLayout* read_registers_ = &pooled_read_registers_;

  std::size_t& next_register_;
  std::uint32_t register_count_ = 0;
  std::uint32_t parameter_count_ = 0;
  std::vector<std::optional<RegisterId>>& call_argument_blocks_;
  std::vector<std::optional<RegisterId>> aggregate_argument_blocks_;
  std::vector<std::optional<RegisterId>> aggregate_result_registers_;
  std::vector<std::optional<AggregateId>> aggregate_descriptors_;
  std::vector<std::optional<lowering_internal::AggregateModifierRuntime>>
      aggregate_modifier_runtimes_;
  std::vector<BoundExpressionId> grouped_retained_leaves_;
  std::vector<std::optional<RegisterId>> grouped_key_expression_registers_;
  std::span<const BoundExpressionId> global_representative_leaves_;

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
  std::vector<MutationIndexCursor> index_write_cursors_;
  std::optional<CursorId> create_index_table_cursor_;
  std::optional<WriteCursorId> schema_write_cursor_;
  std::optional<WriteCursorId> create_index_write_cursor_;
  std::optional<SorterId> create_index_sorter_;
  std::optional<WriteCursorId> stat1_write_cursor_;
  std::vector<CursorId> analyze_index_cursors_;
  std::vector<CursorId> analyze_table_cursors_;
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
