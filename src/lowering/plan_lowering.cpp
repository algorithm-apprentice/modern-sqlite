#include "modern_sqlite/lowering/plan_lowering.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace modern_sqlite {
namespace {

template <typename T>
using LoweringResult = std::expected<T, PlanLoweringError>;

template <typename Optional>
[[nodiscard]] decltype(auto) AssumeValue(Optional& value) noexcept {
  assert(value.has_value());
  return *value;  // NOLINT(bugprone-unchecked-optional-access)
}

template <typename Optional>
[[nodiscard]] decltype(auto) AssumeValue(const Optional& value) noexcept {
  assert(value.has_value());
  return *value;  // NOLINT(bugprone-unchecked-optional-access)
}

[[nodiscard]] PlanLoweringError InternalFailure(std::string detail) {
  return PlanLoweringError{
      .code = PlanLoweringErrorCode::kInternalInvariant,
      .detail = std::move(detail),
  };
}

[[nodiscard]] PlanLoweringError UnsupportedFailure(std::string detail) {
  return PlanLoweringError{
      .code = PlanLoweringErrorCode::kUnsupportedPlan,
      .detail = std::move(detail),
  };
}

[[nodiscard]] PlanLoweringError ProgramFailure(ProgramError error, std::string detail) {
  return PlanLoweringError{
      .code = error.base_error_code() == ErrorCode::kTooLarge
                  ? PlanLoweringErrorCode::kResourceLimit
                  : PlanLoweringErrorCode::kInternalInvariant,
      .program_error = error,
      .detail = std::move(detail),
  };
}

template <typename T>
[[nodiscard]] LoweringResult<T> ConvertProgramResult(ProgramResult<T> result,
                                                     std::string_view detail) {
  if (!result.has_value()) {
    return std::unexpected(ProgramFailure(result.error(), std::string{detail}));
  }
  if constexpr (std::is_void_v<T>) {
    return {};
  } else {
    return std::move(*result);
  }
}

[[nodiscard]] bool NamesEqual(std::string_view left, std::string_view right) noexcept {
  return CatalogNamesEqual(left, right);
}

class PlanLowerer final {
 public:
  PlanLowerer(const PhysicalPlan& plan, ProgramLimits limits) noexcept
      : read_plan_(&plan),
        logical_plan_(&plan.logical_plan()),
        bound_select_(&logical_plan_->bound_select()),
        expressions_(bound_select_->expressions()),
        parameters_(bound_select_->parameters()),
        collations_(bound_select_->collations()),
        functions_(bound_select_->functions()),
        aggregate_functions_(bound_select_->aggregate_functions()),
        aggregate_invocations_(bound_select_->aggregate_invocations()),
        limits_(limits) {}

  PlanLowerer(const PhysicalMutationPlan& plan, ProgramLimits limits) noexcept
      : mutation_plan_(&plan), limits_(limits) {
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
  [[nodiscard]] LowerPlanResult RunRead() {
    if (bound_select_->catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound SELECT does not retain a catalog"));
    }
    const bool has_aggregate =
        std::ranges::any_of(read_plan_->nodes(), [](const PhysicalNode& node) {
          return std::holds_alternative<PhysicalAggregateNode>(node.payload);
        });
    LoweringResult<void> inspected = has_aggregate ? InspectGlobalAggregatePlan() : InspectPlan();
    if (!inspected.has_value()) {
      return std::unexpected(std::move(inspected.error()));
    }
    LoweringResult<void> layout =
        global_aggregate_query_ ? BuildGlobalAggregateRegisterLayout()
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

    if (streaming_requires_snapshot_ ||
        (!streaming_query_ && !set_query_ && bound_select_->table_source() != nullptr)) {
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
    if (global_aggregate_query_) {
      if (auto descriptors = AddGlobalAggregateDescriptors(); !descriptors.has_value()) {
        return std::unexpected(std::move(descriptors.error()));
      }
    } else if (ordered_compound_) {
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
    if (!streaming_query_ && !set_query_ && !ordered_compound_) {
      if (auto cursor = AddCursorDescriptor(); !cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
    }
    LoweringResult<void> emitted =
        global_aggregate_query_ ? EmitGlobalAggregatePlan()
        : ordered_compound_
            ? EmitOrderedCompoundPlan()
            : (set_query_
                   ? EmitSetPlan()
                   : (streaming_query_ ? EmitStreamingPlan()
                                       : (distinct_ != nullptr ? EmitDistinctPlan() : EmitPlan())));
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
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

  struct StreamingCorePlan {
    std::size_t core_index = 0;
    const BoundQueryCore* bound = nullptr;
    const PhysicalSingleRowNode* single_row = nullptr;
    const PhysicalTableScanNode* table_scan = nullptr;
    const PhysicalValuesNode* values = nullptr;
    const PhysicalFilterNode* filter = nullptr;
    const PhysicalProjectionNode* projection = nullptr;
    const PhysicalDistinctNode* distinct = nullptr;
    std::optional<CursorId> cursor{};
    std::optional<RelationId> relation{};
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
    }
    std::vector<std::optional<std::size_t>> node_cores(nodes.size());
    std::vector<PhysicalCompoundStrategy> compound_strategies;
    compound_strategies.reserve(bound_select_->compound_operators().size());
    std::size_t compound_count = 0;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      const PhysicalNode& physical = nodes[index];
      if (const auto* single = std::get_if<PhysicalSingleRowNode>(&physical.payload);
          single != nullptr) {
        if (single->core_index >= streaming_cores_.size() ||
            streaming_cores_[single->core_index].single_row != nullptr ||
            streaming_cores_[single->core_index].table_scan != nullptr ||
            streaming_cores_[single->core_index].values != nullptr) {
          return std::unexpected(InternalFailure("streaming single-row core is invalid"));
        }
        streaming_cores_[single->core_index].single_row = single;
        node_cores[index] = single->core_index;
      } else if (const auto* scan = std::get_if<PhysicalTableScanNode>(&physical.payload);
                 scan != nullptr) {
        if (scan->core_index >= streaming_cores_.size() ||
            streaming_cores_[scan->core_index].single_row != nullptr ||
            streaming_cores_[scan->core_index].table_scan != nullptr ||
            streaming_cores_[scan->core_index].values != nullptr) {
          return std::unexpected(InternalFailure("streaming table core is invalid"));
        }
        streaming_cores_[scan->core_index].table_scan = scan;
        node_cores[index] = scan->core_index;
        streaming_requires_snapshot_ = true;
      } else if (const auto* values = std::get_if<PhysicalValuesNode>(&physical.payload);
                 values != nullptr) {
        if (values->core_index >= streaming_cores_.size() ||
            streaming_cores_[values->core_index].single_row != nullptr ||
            streaming_cores_[values->core_index].table_scan != nullptr ||
            streaming_cores_[values->core_index].values != nullptr) {
          return std::unexpected(InternalFailure("streaming VALUES core is invalid"));
        }
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
      } else if (const auto* projection = std::get_if<PhysicalProjectionNode>(&physical.payload);
                 projection != nullptr) {
        if (projection->core_index >= streaming_cores_.size() ||
            streaming_cores_[projection->core_index].projection != nullptr) {
          return std::unexpected(InternalFailure("streaming projection is invalid"));
        }
        streaming_cores_[projection->core_index].projection = projection;
        node_cores[index] = projection->core_index;
      } else if (const auto* distinct = std::get_if<PhysicalDistinctNode>(&physical.payload);
                 distinct != nullptr) {
        if (distinct->core_index >= streaming_cores_.size() ||
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
    } else if (advanced_order_ != nullptr || output_ != nullptr) {
      return std::unexpected(InternalFailure("unordered compound has ordering metadata"));
    }
    for (const StreamingCorePlan& core : streaming_cores_) {
      if (core.bound == nullptr) {
        return std::unexpected(InternalFailure("streaming core has no bound metadata"));
      }
      if (std::holds_alternative<BoundValuesCore>(*core.bound)) {
        if (core.values == nullptr || core.single_row != nullptr || core.table_scan != nullptr ||
            core.filter != nullptr || core.projection != nullptr || core.distinct != nullptr) {
          return std::unexpected(InternalFailure("streaming VALUES core shape is invalid"));
        }
        continue;
      }
      const auto& select = std::get<BoundSelectCore>(*core.bound);
      const bool source_matches =
          select.table_source.has_value() ? core.table_scan != nullptr : core.single_row != nullptr;
      const bool distinct_matches =
          (select.quantifier == SelectQuantifier::kDistinct) == (core.distinct != nullptr);
      if (!source_matches || core.values != nullptr || core.projection == nullptr ||
          !distinct_matches) {
        return std::unexpected(InternalFailure("streaming SELECT core shape is invalid"));
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

  [[nodiscard]] bool ExpressionRequiresRepresentative(BoundExpressionId id) const {
    const BoundExpression& expression = expressions_[id.value()];
    return std::visit(
        [this](const auto& payload) {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, BoundColumnExpression> ||
                        std::is_same_v<Payload, BoundRowIdExpression>) {
            return true;
          } else if constexpr (std::is_same_v<Payload, BoundLiteralExpression> ||
                               std::is_same_v<Payload, BoundParameterExpression> ||
                               std::is_same_v<Payload, BoundAggregateCallExpression>) {
            return false;
          } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                               std::is_same_v<Payload, BoundTruthTestExpression> ||
                               std::is_same_v<Payload, BoundLikelihoodExpression> ||
                               std::is_same_v<Payload, BoundCollateExpression>) {
            return ExpressionRequiresRepresentative(payload.operand);
          } else if constexpr (std::is_same_v<Payload, BoundBinaryExpression> ||
                               std::is_same_v<Payload, BoundComparisonExpression>) {
            return ExpressionRequiresRepresentative(payload.left) ||
                   ExpressionRequiresRepresentative(payload.right);
          } else if constexpr (std::is_same_v<Payload, BoundAliasReferenceExpression>) {
            return ExpressionRequiresRepresentative(payload.target);
          } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression> ||
                               std::is_same_v<Payload, BoundCoalesceExpression> ||
                               std::is_same_v<Payload, BoundConditionalExpression>) {
            return std::ranges::any_of(payload.arguments, [this](BoundExpressionId argument) {
              return ExpressionRequiresRepresentative(argument);
            });
          }
          return false;
        },
        expression.payload);
  }

  [[nodiscard]] LoweringResult<void> InspectGlobalAggregatePlan() {
    const std::span<const PhysicalNode> nodes = read_plan_->nodes();
    if (bound_select_->query_cores().size() != 1U || !bound_select_->compound_operators().empty() ||
        nodes.size() < 3U) {
      return std::unexpected(
          UnsupportedFailure("global aggregate statement shape is not implemented"));
    }
    aggregate_core_ = std::get_if<BoundSelectCore>(&bound_select_->query_cores().front());
    if (aggregate_core_ == nullptr || aggregate_core_->quantifier != SelectQuantifier::kDefault ||
        !aggregate_core_->aggregate.has_value() || !aggregate_core_->aggregate->group_by.empty()) {
      return std::unexpected(
          UnsupportedFailure("grouped or DISTINCT aggregate lowering is not implemented"));
    }
    aggregate_program_ = &*aggregate_core_->aggregate;
    const bool result_requires_representative = std::ranges::any_of(
        aggregate_core_->result_columns, [this](const BoundResultColumn& result) {
          return ExpressionRequiresRepresentative(result.expression);
        });
    const bool having_requires_representative =
        aggregate_program_->having.has_value() &&
        ExpressionRequiresRepresentative(*aggregate_program_->having);
    if (result_requires_representative || having_requires_representative) {
      return std::unexpected(
          UnsupportedFailure("aggregate representative rows are not implemented"));
    }
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      if (invocation_id.value() >= aggregate_invocations_.size()) {
        return std::unexpected(InternalFailure("aggregate invocation ID is out of range"));
      }
      const BoundAggregateInvocation& invocation = aggregate_invocations_[invocation_id.value()];
      if (invocation.distinct || invocation.filter.has_value() || !invocation.order_by.empty()) {
        return std::unexpected(UnsupportedFailure("aggregate modifiers are not implemented"));
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
      if (invocation.arguments.size() != 1U) {
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

  [[nodiscard]] LoweringResult<void> InspectPlan() {
    if (std::ranges::any_of(read_plan_->nodes(), [](const PhysicalNode& node) {
          return std::holds_alternative<PhysicalValuesNode>(node.payload) ||
                 std::holds_alternative<PhysicalCompoundNode>(node.payload);
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

  [[nodiscard]] LoweringResult<void> BuildGlobalAggregateRegisterLayout() {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("global aggregate metadata is unavailable"));
    }
    if (auto expressions = BeginExpressionRegisterLayout(); !expressions.has_value()) {
      return expressions;
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

    auto result_block = AllocateRegisters(bound_select_->result_columns().size());
    if (!result_block.has_value()) {
      return std::unexpected(std::move(result_block.error()));
    }
    result_block_first_ = *result_block;

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
    }

    if (index_scan_ != nullptr) {
      std::size_t key_count =
          index_scan_->equalities.size() + static_cast<std::size_t>(index_scan_->range.has_value());
      if (aggregate_ != nullptr &&
          aggregate_->strategy == PhysicalAggregateStrategy::kIndexedMinMax &&
          aggregate_->min_max.has_value() &&
          aggregate_->min_max->null_strategy ==
              PhysicalMinMaxNullStrategy::kNonNullFirstWithSingleNullFallback) {
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
    if (core.table_scan == nullptr || !select.table_source.has_value() ||
        select.source_column_begin > bound_select_->source_columns().size() ||
        select.source_column_count >
            bound_select_->source_columns().size() - select.source_column_begin) {
      return std::unexpected(InternalFailure("streaming cursor metadata is invalid"));
    }
    const std::span<const BoundSourceColumn> columns = bound_select_->source_columns().subspan(
        select.source_column_begin, select.source_column_count);
    ReadCursorDescriptor descriptor{
        .root_page = RootPageNumber(core.table_scan->root_page.value),
        .storage = CursorStorageKind::kRowIdTable,
        .record_field_count = 0,
        .fields = {},
        .index_columns = {},
    };
    if (core.table_scan->source_kind == BoundSourceKind::kSchemaTable) {
      constexpr std::uint32_t kSchemaFieldCount = 5;
      if (columns.size() != kSchemaFieldCount) {
        return std::unexpected(InternalFailure("streaming schema core shape is invalid"));
      }
      descriptor.record_field_count = kSchemaFieldCount;
      descriptor.fields.reserve(kSchemaFieldCount);
      for (std::uint32_t index = 0; index < kSchemaFieldCount; ++index) {
        descriptor.fields.push_back(CursorFieldSource{
            .kind = CursorFieldSourceKind::kRecordField,
            .record_field = index,
        });
        source_cursor_fields_[select.source_column_begin + index] = CursorFieldId{index};
      }
    } else {
      if (!core.table_scan->table.has_value()) {
        return std::unexpected(InternalFailure("streaming table core has no table ID"));
      }
      const CatalogSnapshot& catalog = *bound_select_->catalog();
      const TableId table_id = *core.table_scan->table;
      const CatalogTable& table = catalog.table(table_id);
      if (!table.without_rowid) {
        if (table.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(InternalFailure("streaming table field count is too large"));
        }
        descriptor.record_field_count = static_cast<std::uint32_t>(table.columns.size());
        descriptor.fields.reserve(columns.size());
        for (std::size_t index = 0; index < columns.size(); ++index) {
          const BoundSourceColumn& source_column = columns[index];
          if (!source_column.catalog_column.has_value() ||
              source_column.catalog_column->value >= table.columns.size()) {
            return std::unexpected(InternalFailure("streaming bound source column is invalid"));
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
            source_field_real_affinity_[select.source_column_begin + index] =
                source_column.affinity == TypeAffinity::kReal;
          }
          source_cursor_fields_[select.source_column_begin + index] =
              CursorFieldId{static_cast<std::uint32_t>(descriptor.fields.size() - 1U)};
        }
      } else {
        descriptor.storage = CursorStorageKind::kIndex;
        const std::optional<IndexId> primary_id = catalog.primary_key_index(table_id);
        if (!primary_id.has_value()) {
          return std::unexpected(
              InternalFailure("streaming WITHOUT ROWID core has no primary index"));
        }
        const CatalogIndex& primary = catalog.index(*primary_id);
        if (primary.terms.empty() ||
            primary.terms.size() > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(
              InternalFailure("streaming WITHOUT ROWID primary shape is invalid"));
        }
        descriptor.record_field_count = static_cast<std::uint32_t>(primary.terms.size());
        descriptor.index_columns.reserve(primary.terms.size());
        std::vector<std::optional<std::uint32_t>> field_by_column(table.columns.size());
        for (std::size_t term_index = 0; term_index < primary.terms.size(); ++term_index) {
          const CatalogIndexTerm& term = primary.terms[term_index];
          const auto* column = std::get_if<ColumnId>(&term.target);
          if (column == nullptr || column->value >= table.columns.size() ||
              field_by_column[column->value].has_value()) {
            return std::unexpected(InternalFailure("streaming primary index term is invalid"));
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
        descriptor.fields.reserve(columns.size());
        for (std::size_t index = 0; index < columns.size(); ++index) {
          const BoundSourceColumn& source_column = columns[index];
          if (!source_column.catalog_column.has_value() ||
              source_column.catalog_column->value >= table.columns.size()) {
            return std::unexpected(
                InternalFailure("streaming WITHOUT ROWID source column is invalid"));
          }
          const ColumnId column_id = *source_column.catalog_column;
          if (!field_by_column[column_id.value].has_value()) {
            return std::unexpected(InternalFailure("streaming primary record omits a column"));
          }
          auto field = CatalogFieldSource(table.columns[column_id.value],
                                          AssumeValue(field_by_column[column_id.value]));
          if (!field.has_value()) {
            return std::unexpected(std::move(field.error()));
          }
          descriptor.fields.push_back(*field);
          source_cursor_fields_[select.source_column_begin + index] =
              CursorFieldId{static_cast<std::uint32_t>(descriptor.fields.size() - 1U)};
          source_field_real_affinity_[select.source_column_begin + index] =
              source_column.affinity == TypeAffinity::kReal;
        }
      }
    }
    auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                       "unable to add streaming core cursor");
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    core.cursor = *cursor;
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddStreamingDescriptors() {
    source_cursor_fields_.assign(bound_select_->source_columns().size(), std::nullopt);
    source_field_real_affinity_.assign(bound_select_->source_columns().size(), false);
    for (StreamingCorePlan& core : streaming_cores_) {
      if (const auto* select = std::get_if<BoundSelectCore>(core.bound); select != nullptr) {
        if (core.table_scan != nullptr) {
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

  [[nodiscard]] LoweringResult<void> AddSetDescriptors() {
    if (auto streaming = AddStreamingDescriptors(); !streaming.has_value()) {
      return streaming;
    }
    if (bound_select_->compound_collations().size() != bound_select_->result_columns().size() ||
        bound_select_->result_columns().size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InternalFailure("compound relation descriptor shape is invalid"));
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

    auto full_comparison = BuildOrderedCompoundDescriptor(true);
    if (!full_comparison.has_value()) {
      return std::unexpected(std::move(full_comparison.error()));
    }
    auto added_full_comparison =
        ConvertProgramResult(AssumeValue(builder_).AddRecordComparison(std::move(*full_comparison)),
                             "unable to add full-key compound comparison");
    if (!added_full_comparison.has_value()) {
      return std::unexpected(std::move(added_full_comparison.error()));
    }
    ordered_full_comparison_ = *added_full_comparison;

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

    source_cursor_fields_.assign(bound_select_->source_columns().size(), std::nullopt);
    source_field_real_affinity_.assign(bound_select_->source_columns().size(), false);
    source_rowid_cursor_field_.reset();

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
            source_rowid_cursor_field_ = CursorFieldId{field};
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
        if (table.rowid_alias.has_value() && source_rowid_cursor_field_.has_value() &&
            !field_by_column[table.rowid_alias->value].has_value()) {
          field_by_column[table.rowid_alias->value] = source_rowid_cursor_field_->value();
        }
        for (std::size_t source_index = 0; source_index < bound_select_->source_columns().size();
             ++source_index) {
          const BoundSourceColumn& source_column = bound_select_->source_columns()[source_index];
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
        return {};
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
      if (bound_select_->source_columns().size() != kSchemaFieldCount) {
        return std::unexpected(InternalFailure("sqlite_schema source column count is invalid"));
      }
      descriptor.record_field_count = kSchemaFieldCount;
      descriptor.fields.reserve(kSchemaFieldCount);
      for (std::uint32_t index = 0; index < kSchemaFieldCount; ++index) {
        descriptor.fields.push_back(CursorFieldSource{
            .kind = CursorFieldSourceKind::kRecordField,
            .record_field = index,
        });
        source_cursor_fields_[index] = CursorFieldId{index};
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
        descriptor.fields.reserve(bound_select_->source_columns().size());
        for (std::size_t index = 0; index < bound_select_->source_columns().size(); ++index) {
          const BoundSourceColumn& source_column = bound_select_->source_columns()[index];
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
            source_field_real_affinity_[index] = source_column.affinity == TypeAffinity::kReal;
          }
          source_cursor_fields_[index] =
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
        descriptor.fields.reserve(bound_select_->source_columns().size());
        for (std::size_t index = 0; index < bound_select_->source_columns().size(); ++index) {
          const BoundSourceColumn& source_column = bound_select_->source_columns()[index];
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
          source_cursor_fields_[index] =
              CursorFieldId{static_cast<std::uint32_t>(descriptor.fields.size() - 1U)};
          source_field_real_affinity_[index] = source_column.affinity == TypeAffinity::kReal;
        }
      }
    }

    auto cursor = ConvertProgramResult(AssumeValue(builder_).AddCursor(std::move(descriptor)),
                                       "unable to add the read cursor descriptor");
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    cursor_ = *cursor;
    return {};
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
                                        "unable to emit a bytecode jump");
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitJumpIf(RegisterId input, JumpCondition condition,
                                                Label target) {
    auto emitted = ConvertProgramResult(AssumeValue(builder_).EmitJumpIf(input, condition, target),
                                        "unable to emit a conditional bytecode jump");
    if (!emitted.has_value()) {
      return std::unexpected(std::move(emitted.error()));
    }
    return {};
  }

  [[nodiscard]] RegisterId Home(BoundExpressionId id) const noexcept {
    return RegisterId{id.value()};
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

  [[nodiscard]] LoweringResult<void> EmitExpression(BoundExpressionId id, RegisterId destination) {
    if (id.value() >= expressions_.size()) {
      return std::unexpected(InternalFailure("bound expression ID is out of range"));
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
      if (!source_snapshot_registers_.empty()) {
        if (column->column.value() >= source_snapshot_registers_.size() ||
            column->column.value() >= source_field_real_affinity_.size()) {
          return std::unexpected(InternalFailure("bound column has no source snapshot field"));
        }
        if (auto copied = Append(CopyInstruction{
                .input = source_snapshot_registers_[column->column.value()],
                .output = destination,
            });
            !copied.has_value()) {
          return copied;
        }
        if (source_field_real_affinity_[column->column.value()]) {
          return Append(RealAffinityInstruction{
              .input = destination,
              .output = destination,
          });
        }
        return {};
      }
      if (!cursor_.has_value() || column->column.value() >= source_field_real_affinity_.size() ||
          column->column.value() >= source_cursor_fields_.size() ||
          !source_cursor_fields_[column->column.value()].has_value()) {
        return std::unexpected(InternalFailure("bound column has no cursor field"));
      }
      if (auto read = Append(ReadFieldInstruction{
              .cursor = AssumeValue(cursor_),
              .field = AssumeValue(source_cursor_fields_[column->column.value()]),
              .output = destination,
          });
          !read.has_value()) {
        return read;
      }
      if (source_field_real_affinity_[column->column.value()]) {
        return Append(RealAffinityInstruction{
            .input = destination,
            .output = destination,
        });
      }
      return {};
    }
    if (std::holds_alternative<BoundRowIdExpression>(expression.payload)) {
      if (source_rowid_register_.has_value()) {
        return Append(CopyInstruction{
            .input = AssumeValue(source_rowid_register_),
            .output = destination,
        });
      }
      if (!cursor_.has_value()) {
        return std::unexpected(InternalFailure("rowid expression has no cursor"));
      }
      if (source_rowid_cursor_field_.has_value()) {
        return Append(ReadFieldInstruction{
            .cursor = AssumeValue(cursor_),
            .field = *source_rowid_cursor_field_,
            .output = destination,
        });
      }
      return Append(ReadRowIdInstruction{
          .cursor = AssumeValue(cursor_),
          .output = destination,
      });
    }
    if (const auto* unary = std::get_if<BoundUnaryExpression>(&expression.payload);
        unary != nullptr) {
      if (auto emitted = EmitExpression(unary->operand, destination); !emitted.has_value()) {
        return emitted;
      }
      if (unary->operation == BoundUnaryOperation::kPositive) {
        return {};
      }
      return Append(UnaryInstruction{
          .operation = ToUnaryOperation(unary->operation),
          .input = destination,
          .output = destination,
      });
    }
    if (const auto* binary = std::get_if<BoundBinaryExpression>(&expression.payload);
        binary != nullptr) {
      if (const std::optional<BoundExpressionId> selected = SimplifiedLogicalOperand(*binary);
          selected.has_value()) {
        if (auto emitted = EmitExpression(*selected, destination); !emitted.has_value()) {
          return emitted;
        }
        return Append(BinaryInstruction{
            .operation = ToBinaryOperation(binary->operation),
            .left = destination,
            .right = destination,
            .output = destination,
        });
      }
      const RegisterId left = Home(binary->left);
      const RegisterId right = Home(binary->right);
      if (auto emitted = EmitExpression(binary->left, left); !emitted.has_value()) {
        return emitted;
      }
      if (auto emitted = EmitExpression(binary->right, right); !emitted.has_value()) {
        return emitted;
      }
      return Append(BinaryInstruction{
          .operation = ToBinaryOperation(binary->operation),
          .left = left,
          .right = right,
          .output = destination,
      });
    }
    if (const auto* comparison = std::get_if<BoundComparisonExpression>(&expression.payload);
        comparison != nullptr) {
      const RegisterId left = Home(comparison->left);
      const RegisterId right = Home(comparison->right);
      if (auto emitted = EmitExpression(comparison->left, left); !emitted.has_value()) {
        return emitted;
      }
      if (auto emitted = EmitExpression(comparison->right, right); !emitted.has_value()) {
        return emitted;
      }
      auto collation = CollationSymbol(comparison->collation);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      return Append(CompareInstruction{
          .comparison = comparison->comparison,
          .affinity = comparison->affinity,
          .collation = *collation,
          .left = left,
          .right = right,
          .output = destination,
      });
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
      const std::optional<RegisterId>& argument_block = call_argument_blocks_[id.value()];
      if (!argument_block.has_value()) {
        return std::unexpected(InternalFailure("scalar call has no argument block"));
      }
      const RegisterId first = AssumeValue(argument_block);
      for (std::size_t index = 0; index < call->arguments.size(); ++index) {
        if (auto emitted =
                EmitExpression(call->arguments[index],
                               RegisterId{first.value() + static_cast<std::uint32_t>(index)});
            !emitted.has_value()) {
          return emitted;
        }
      }
      auto function = FunctionSymbol(call->function);
      auto collation = CollationSymbol(call->collation);
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
          .argument_count = static_cast<std::uint32_t>(call->arguments.size()),
          .output = destination,
      });
    }
    if (const auto* call = std::get_if<BoundAggregateCallExpression>(&expression.payload);
        call != nullptr) {
      if (!global_aggregate_query_ ||
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
    return std::unexpected(InternalFailure("bound expression kind is unsupported"));
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

  [[nodiscard]] LoweringResult<void> EmitLimitInitialization(std::optional<Label> completion) {
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
    if (guard_ == nullptr) {
      return {};
    }
    if (!completion.has_value()) {
      return std::unexpected(InternalFailure("physical guard has no completion label"));
    }
    for (const BoundExpressionId predicate : guard_->predicates) {
      if (auto emitted = EmitPredicate(predicate, *completion); !emitted.has_value()) {
        return emitted;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> EmitFilters(Label rejected) {
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

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateGuards(Label empty_input) {
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

  [[nodiscard]] LoweringResult<void> OpenGlobalAggregates() {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("global aggregate program is unavailable"));
    }
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      if (invocation_id.value() >= aggregate_descriptors_.size() ||
          !aggregate_descriptors_[invocation_id.value()].has_value()) {
        return std::unexpected(InternalFailure("global aggregate descriptor is unavailable"));
      }
      if (auto opened = Append(OpenAggregateInstruction{
              .aggregate = AssumeValue(aggregate_descriptors_[invocation_id.value()]),
          });
          !opened.has_value()) {
        return opened;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> StepGlobalAggregates() {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("global aggregate program is unavailable"));
    }
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      const BoundAggregateInvocation& invocation = aggregate_invocations_[invocation_id.value()];
      if (invocation_id.value() >= aggregate_argument_blocks_.size() ||
          invocation_id.value() >= aggregate_descriptors_.size() ||
          !aggregate_argument_blocks_[invocation_id.value()].has_value() ||
          !aggregate_descriptors_[invocation_id.value()].has_value()) {
        return std::unexpected(InternalFailure("global aggregate step metadata is incomplete"));
      }
      const RegisterId first = AssumeValue(aggregate_argument_blocks_[invocation_id.value()]);
      for (std::size_t argument = 0; argument < invocation.arguments.size(); ++argument) {
        if (auto emitted =
                EmitExpression(invocation.arguments[argument],
                               RegisterId{first.value() + static_cast<std::uint32_t>(argument)});
            !emitted.has_value()) {
          return emitted;
        }
      }
      if (auto stepped = Append(StepAggregateInstruction{
              .aggregate = AssumeValue(aggregate_descriptors_[invocation_id.value()]),
              .first_argument = first,
              .effect_output = std::nullopt,
          });
          !stepped.has_value()) {
        return stepped;
      }
    }
    return {};
  }

  [[nodiscard]] LoweringResult<void> FinalizeAndCloseGlobalAggregates() {
    if (aggregate_program_ == nullptr) {
      return std::unexpected(InternalFailure("global aggregate program is unavailable"));
    }
    for (const BoundAggregateInvocationId invocation_id : aggregate_program_->invocations) {
      if (invocation_id.value() >= aggregate_descriptors_.size() ||
          invocation_id.value() >= aggregate_result_registers_.size() ||
          !aggregate_descriptors_[invocation_id.value()].has_value() ||
          !aggregate_result_registers_[invocation_id.value()].has_value()) {
        return std::unexpected(
            InternalFailure("global aggregate finalization metadata is incomplete"));
      }
      if (auto finalized = Append(FinalizeAggregateInstruction{
              .aggregate = AssumeValue(aggregate_descriptors_[invocation_id.value()]),
              .output = AssumeValue(aggregate_result_registers_[invocation_id.value()]),
          });
          !finalized.has_value()) {
        return finalized;
      }
      if (auto closed = Append(CloseAggregateInstruction{
              .aggregate = AssumeValue(aggregate_descriptors_[invocation_id.value()]),
          });
          !closed.has_value()) {
        return closed;
      }
    }
    return {};
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

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateProjection() {
    for (std::size_t index = 0; index < bound_select_->result_columns().size(); ++index) {
      if (auto emitted = EmitExpression(
              bound_select_->result_columns()[index].expression,
              RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)});
          !emitted.has_value()) {
        return emitted;
      }
    }
    return Append(ResultRowInstruction{
        .first = result_block_first_,
        .count = static_cast<std::uint32_t>(bound_select_->result_columns().size()),
    });
  }

  [[nodiscard]] LoweringResult<void> EmitGlobalAggregateCompletion() {
    if (auto finalized = FinalizeAndCloseGlobalAggregates(); !finalized.has_value()) {
      return finalized;
    }
    const bool has_rejection =
        having_ != nullptr || (limit_ != nullptr && limit_->offset.has_value());
    std::optional<Label> rejected;
    if (has_rejection) {
      auto label = CreateLabel();
      if (!label.has_value()) {
        return std::unexpected(std::move(label.error()));
      }
      rejected = *label;
    }
    if (having_ != nullptr) {
      if (auto having = EmitPredicate(having_->predicate, AssumeValue(rejected));
          !having.has_value()) {
        return having;
      }
    }
    if (rejected.has_value()) {
      if (auto offset = EmitGlobalAggregateOffset(*rejected); !offset.has_value()) {
        return offset;
      }
    }
    if (auto projection = EmitGlobalAggregateProjection(); !projection.has_value()) {
      return projection;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (rejected.has_value()) {
      if (auto bound = BindLabel(*rejected); !bound.has_value()) {
        return bound;
      }
      return Append(HaltInstruction{});
    }
    return {};
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
    if (limit_ == nullptr || !limit_->offset.has_value()) {
      return {};
    }
    if (sort_ != nullptr && !ordered_drain) {
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

  [[nodiscard]] LoweringResult<void> EmitReadCompletion() {
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
      if (auto emitted = EmitExpression(
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
    return guard_ != nullptr || (limit_ != nullptr && !limit_initialized_);
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

  [[nodiscard]] LoweringResult<void> EmitIndexKeyExpressions(const PhysicalIndexScanNode& index,
                                                             std::optional<Label> null_target) {
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
      const PhysicalIndexScanNode& index, const PhysicalIndexBound* range_bound) {
    const std::size_t count =
        index.equalities.size() + static_cast<std::size_t>(range_bound != nullptr);
    if (!index_key_first_.has_value() || count == 0U || count > index_key_capacity_) {
      return std::unexpected(InternalFailure("covering index key register block is invalid"));
    }
    std::size_t offset = 0;
    const auto copy_term = [&](BoundExpressionId expression,
                               TypeAffinity affinity) -> LoweringResult<void> {
      const RegisterId destination{AssumeValue(index_key_first_).value() +
                                   static_cast<std::uint32_t>(offset++)};
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
      const IndexSeekMode mode = physical_start == nullptr || physical_start->inclusive
                                     ? IndexSeekMode::kGreaterOrEqual
                                     : IndexSeekMode::kGreater;
      auto sought = ConvertProgramResult(
          AssumeValue(builder_).EmitSeekIndex(AssumeValue(cursor_), AssumeValue(index_key_first_),
                                              start_key_count, *unpositioned_completion, mode),
          "unable to emit covering index seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      auto rewound = ConvertProgramResult(
          AssumeValue(builder_).EmitRewind(AssumeValue(cursor_), *unpositioned_completion),
          "unable to emit covering index rewind");
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
    if (has_end_key || (limit_ != nullptr && sort_ == nullptr)) {
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
      const IndexRangeEndMode mode = physical_end == nullptr || physical_end->inclusive
                                         ? IndexRangeEndMode::kInclusive
                                         : IndexRangeEndMode::kExclusive;
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

    if (limit_ != nullptr && sort_ == nullptr) {
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
    auto next =
        ConvertProgramResult(AssumeValue(builder_).EmitNext(AssumeValue(cursor_), *row_loop),
                             "unable to emit covering index advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*unpositioned_completion); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(); !completed.has_value()) {
      return completed;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(); !completed.has_value()) {
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
    if (index.covering || !cursor_.has_value() || !index_cursor_.has_value() ||
        !index_rowid_register_.has_value() ||
        (index.equalities.empty() && !index.range.has_value())) {
      return std::unexpected(InternalFailure("noncovering index scan resources are incomplete"));
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
      const IndexSeekMode mode = physical_start == nullptr || physical_start->inclusive
                                     ? IndexSeekMode::kGreaterOrEqual
                                     : IndexSeekMode::kGreater;
      auto sought = ConvertProgramResult(
          AssumeValue(builder_).EmitSeekIndex(index_cursor, AssumeValue(index_key_first_),
                                              start_key_count, *index_missing, mode),
          "unable to emit noncovering index seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      auto rewound =
          ConvertProgramResult(AssumeValue(builder_).EmitRewind(index_cursor, *index_missing),
                               "unable to emit noncovering index rewind");
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
    } else if (limit_ != nullptr && sort_ == nullptr) {
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
      const IndexRangeEndMode mode = physical_end == nullptr || physical_end->inclusive
                                         ? IndexRangeEndMode::kInclusive
                                         : IndexRangeEndMode::kExclusive;
      auto checked = ConvertProgramResult(
          AssumeValue(builder_).EmitCheckIndexRange(index_cursor, AssumeValue(index_key_first_),
                                                    end_key_count, AssumeValue(first_end), mode),
          "unable to emit first noncovering index end check");
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
    if (auto jumped = EmitJump(*process_candidate); !jumped.has_value()) {
      return jumped;
    }

    if (auto bound = BindLabel(*subsequent_candidate); !bound.has_value()) {
      return bound;
    }
    if (has_end_key) {
      const IndexRangeEndMode mode = physical_end == nullptr || physical_end->inclusive
                                         ? IndexRangeEndMode::kInclusive
                                         : IndexRangeEndMode::kExclusive;
      auto checked =
          ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
                                   index_cursor, AssumeValue(index_key_first_), end_key_count,
                                   AssumeValue(positioned_completion), mode),
                               "unable to emit noncovering index end check");
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
    if (limit_ != nullptr && sort_ == nullptr) {
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
    auto next =
        ConvertProgramResult(AssumeValue(builder_).EmitNext(index_cursor, *subsequent_candidate),
                             "unable to emit noncovering index advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*index_exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(); !completed.has_value()) {
      return completed;
    }
    if (first_end.has_value()) {
      if (auto bound = BindLabel(*first_end); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(); !completed.has_value()) {
        return completed;
      }
    }
    if (auto bound = BindLabel(*index_missing); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(); !completed.has_value()) {
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
    auto rewound = ConvertProgramResult(
        AssumeValue(builder_).EmitRewind(AssumeValue(cursor_), AssumeValue(empty)),
        "unable to emit table rewind");
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
    if (limit_ != nullptr && sort_ == nullptr) {
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
    auto next = ConvertProgramResult(
        AssumeValue(builder_).EmitNext(AssumeValue(cursor_), AssumeValue(row_loop)),
        "unable to emit table advance");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto bound = BindLabel(*empty); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(); !completed.has_value()) {
      return completed;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto completed = EmitReadCompletion(); !completed.has_value()) {
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

    const RegisterId key = Home(lookup.key);
    if (auto emitted = EmitExpression(lookup.key, key); !emitted.has_value()) {
      return emitted;
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
        AssumeValue(builder_).EmitSeekRowId(AssumeValue(cursor_), key, AssumeValue(missing)),
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
    if (auto completed = EmitReadCompletion(); !completed.has_value()) {
      return completed;
    }
    if (auto bound = BindLabel(*missing); !bound.has_value()) {
      return bound;
    }
    if (auto completed = EmitReadCompletion(); !completed.has_value()) {
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
    table_scan_ = core.table_scan;
    distinct_ = core.distinct;
    distinct_relation_ = core.relation;
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

  [[nodiscard]] LoweringResult<void> EmitStreamingProjection(const BoundSelectCore& select) {
    if (select.result_columns.size() != bound_select_->result_columns().size()) {
      return std::unexpected(InternalFailure("streaming projection width is invalid"));
    }
    for (std::size_t index = 0; index < select.result_columns.size(); ++index) {
      if (auto emitted = EmitExpression(
              select.result_columns[index].expression,
              RegisterId{result_block_first_.value() + static_cast<std::uint32_t>(index)});
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
        if (auto emitted = EmitExpression(
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
        if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
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
        if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
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
      if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
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
      if (auto projected = EmitStreamingProjection(select); !projected.has_value()) {
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
        if (auto emitted = EmitExpression(
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
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewind(AssumeValue(cursor_), *exhausted),
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
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(AssumeValue(cursor_), *row),
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
          AssumeValue(builder_).EmitSeekIndex(AssumeValue(cursor_), AssumeValue(index_key_first_),
                                              start_key_count, *unpositioned, mode),
          "unable to emit global aggregate index seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      auto rewound = ConvertProgramResult(
          AssumeValue(builder_).EmitRewind(AssumeValue(cursor_), *unpositioned),
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
      auto checked = ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
                                              AssumeValue(cursor_), AssumeValue(index_key_first_),
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
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(AssumeValue(cursor_), *row),
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
    auto sought = ConvertProgramResult(
        AssumeValue(builder_).EmitSeekRowId(AssumeValue(cursor_), key, *missing),
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
      auto sought = ConvertProgramResult(
          AssumeValue(builder_).EmitSeekIndex(index_cursor, AssumeValue(index_key_first_),
                                              start_key_count, *index_missing, mode),
          "unable to emit global aggregate noncovering seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
    } else {
      auto rewound =
          ConvertProgramResult(AssumeValue(builder_).EmitRewind(index_cursor, *index_missing),
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
          AssumeValue(builder_).EmitCheckIndexRange(index_cursor, AssumeValue(index_key_first_),
                                                    end_key_count, AssumeValue(first_end), mode),
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
      auto checked = ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
                                              index_cursor, AssumeValue(index_key_first_),
                                              end_key_count, AssumeValue(positioned_end), mode),
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
    auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(index_cursor, *subsequent),
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
      auto sought = ConvertProgramResult(
          AssumeValue(builder_).EmitSeekIndex(index_cursor, AssumeValue(index_key_first_),
                                              *key_count, missing, mode),
          "unable to emit indexed MIN/MAX seek");
      if (!sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
      }
      return {};
    }
    if (index.direction == PhysicalScanDirection::kForward) {
      auto rewound = ConvertProgramResult(AssumeValue(builder_).EmitRewind(index_cursor, missing),
                                          "unable to emit indexed MIN/MAX rewind");
      if (!rewound.has_value()) {
        return std::unexpected(std::move(rewound.error()));
      }
      return {};
    }
    auto last = ConvertProgramResult(AssumeValue(builder_).EmitLast(index_cursor, missing),
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
    auto sought = ConvertProgramResult(
        AssumeValue(builder_).EmitSeekIndex(index_cursor, AssumeValue(index_key_first_), *key_count,
                                            missing, mode),
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
    auto sought = ConvertProgramResult(
        AssumeValue(builder_).EmitSeekIndex(index_cursor, AssumeValue(index_key_first_), *key_count,
                                            missing, mode),
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
        ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
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
        ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
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
        ConvertProgramResult(AssumeValue(builder_).EmitCheckIndexRange(
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
      auto next = ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, candidate),
                                       "unable to emit indexed MIN/MAX next");
      if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
      }
      return {};
    }
    auto previous = ConvertProgramResult(AssumeValue(builder_).EmitPrevious(cursor, candidate),
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
            ? ConvertProgramResult(AssumeValue(builder_).EmitRewind(cursor, *exhausted),
                                   "unable to emit indexed MIN/MAX table rewind")
            : ConvertProgramResult(AssumeValue(builder_).EmitLast(cursor, *exhausted),
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
              ? ConvertProgramResult(AssumeValue(builder_).EmitNext(cursor, *row),
                                     "unable to emit indexed MIN/MAX table next")
              : ConvertProgramResult(AssumeValue(builder_).EmitPrevious(cursor, *row),
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

  struct NamedSymbol {
    std::string name;
    SymbolId id;
  };

  enum class OrderingRuntime : std::uint8_t {
    kNone,
    kSorter,
    kTopN,
  };

  const PhysicalPlan* read_plan_ = nullptr;
  const LogicalPlan* logical_plan_ = nullptr;
  const BoundSelect* bound_select_ = nullptr;
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
  bool streaming_query_ = false;
  bool set_query_ = false;
  bool set_limit_one_ = false;
  bool ordered_compound_ = false;
  bool streaming_requires_snapshot_ = false;
  std::vector<StreamingCorePlan> streaming_cores_;
  std::array<std::optional<RelationId>, 3> set_relations_{};
  std::array<std::optional<RegisterId>, 3> set_count_registers_{};
  std::array<std::optional<SorterId>, 3> ordered_sorters_{};
  std::array<std::optional<SorterId>, 3> ordered_full_sorters_{};
  std::optional<TopNId> ordered_top_n_;
  std::optional<TopNId> ordered_full_top_n_;
  std::optional<RecordComparisonId> ordered_comparison_;
  std::optional<RecordComparisonId> ordered_full_comparison_;

  std::size_t next_register_ = 0;
  std::uint32_t register_count_ = 0;
  std::uint32_t parameter_count_ = 0;
  RegisterId result_block_first_{0};
  std::vector<std::optional<RegisterId>> call_argument_blocks_;
  std::vector<std::optional<RegisterId>> aggregate_argument_blocks_;
  std::vector<std::optional<RegisterId>> aggregate_result_registers_;
  std::vector<std::optional<AggregateId>> aggregate_descriptors_;
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

  std::optional<ProgramBuilder> builder_;
  std::vector<std::optional<ConstantId>> literal_constants_;
  std::optional<ConstantId> null_constant_;
  std::optional<ConstantId> zero_constant_;
  std::optional<ConstantId> one_constant_;
  std::optional<ConstantId> maximum_integer_constant_;
  std::optional<ConstantId> indexed_min_max_null_constant_;
  std::vector<SymbolId> collation_symbols_;
  std::vector<SymbolId> function_symbols_;
  std::vector<SymbolId> aggregate_function_symbols_;
  std::vector<NamedSymbol> symbol_names_;
  std::optional<SymbolId> binary_symbol_;
  std::optional<CursorId> cursor_;
  std::optional<CursorId> index_cursor_;
  std::optional<SorterId> sorter_;
  std::optional<TopNId> top_n_;
  std::optional<RelationId> distinct_relation_;
  std::vector<std::optional<CursorFieldId>> source_cursor_fields_;
  std::optional<CursorFieldId> source_rowid_cursor_field_;
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
  return PlanLowerer(plan, limits).Run();
}

LowerPlanResult LowerPlan(const PhysicalMutationPlan& plan, ProgramLimits limits) {
  if (!plan.valid()) {
    return std::unexpected(PlanLoweringError{
        .code = PlanLoweringErrorCode::kInvalidInput,
        .detail = "physical mutation plan is invalid",
    });
  }
  return PlanLowerer(plan, limits).Run();
}

}  // namespace modern_sqlite
