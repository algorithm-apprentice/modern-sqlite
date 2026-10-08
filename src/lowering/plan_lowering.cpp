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
    if (auto inspected = InspectPlan(); !inspected.has_value()) {
      return std::unexpected(std::move(inspected.error()));
    }
    if (auto layout = BuildReadRegisterLayout(); !layout.has_value()) {
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

    if (bound_select_->table_source() != nullptr) {
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
    if (auto cursor = AddCursorDescriptor(); !cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    if (auto emitted = EmitPlan(); !emitted.has_value()) {
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

  [[nodiscard]] LoweringResult<void> InspectPlan() {
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
      } else {
        return std::unexpected(InternalFailure("physical plan contains an unexpected node"));
      }
    }
    if (projection_ == nullptr) {
      return std::unexpected(InternalFailure("physical plan has no projection"));
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

  [[nodiscard]] LoweringResult<void> BuildReadRegisterLayout() {
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

      if (IsRowLoopAccess()) {
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
        if (column->column.value() >= source_snapshot_registers_.size()) {
          return std::unexpected(InternalFailure("bound column has no source snapshot field"));
        }
        return Append(CopyInstruction{
            .input = source_snapshot_registers_[column->column.value()],
            .output = destination,
        });
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
    if (limit_ == nullptr) {
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
      return Append(CompareInstruction{
          .comparison = SqlComparison::kLess,
          .affinity = TypeAffinity::kNumeric,
          .collation = *binary,
          .left = AssumeValue(limit_register_),
          .right = AssumeValue(zero_register_),
          .output = AssumeValue(negative_limit_register_),
      });
    }
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

  [[nodiscard]] LoweringResult<void> EmitOffset(Label skipped) {
    if (limit_ == nullptr || !limit_->offset.has_value()) {
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

  [[nodiscard]] LoweringResult<void> EmitProjection() {
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
    return limit_ != nullptr || guard_ != nullptr;
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
    return Append(HaltInstruction{});
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
    if (auto projection = EmitProjection(); !projection.has_value()) {
      return projection;
    }
    if (auto bound = BindLabel(*completion); !bound.has_value()) {
      return bound;
    }
    return Append(HaltInstruction{});
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
      return Append(ApplyAffinityInstruction{
          .input = destination,
          .affinity = affinity,
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
    if (has_end_key || limit_ != nullptr) {
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
    if (auto projection = EmitProjection(); !projection.has_value()) {
      return projection;
    }

    if (limit_ != nullptr) {
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
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
        return halted;
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
    } else if (limit_ != nullptr) {
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
    if (auto projection = EmitProjection(); !projection.has_value()) {
      return projection;
    }
    if (limit_ != nullptr) {
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
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (first_end.has_value()) {
      if (auto bound = BindLabel(*first_end); !bound.has_value()) {
        return bound;
      }
      if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
        return halted;
      }
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
        return halted;
      }
    }
    if (auto bound = BindLabel(*index_missing); !bound.has_value()) {
      return bound;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
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
    if (auto projection = EmitProjection(); !projection.has_value()) {
      return projection;
    }

    std::optional<Label> positioned_completion;
    if (limit_ != nullptr) {
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
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (positioned_completion.has_value()) {
      if (auto bound = BindLabel(*positioned_completion); !bound.has_value()) {
        return bound;
      }
      if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
        return halted;
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
    if (auto projection = EmitProjection(); !projection.has_value()) {
      return projection;
    }
    if (auto bound = BindLabel(*positioned); !bound.has_value()) {
      return bound;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
    }
    if (auto bound = BindLabel(*missing); !bound.has_value()) {
      return bound;
    }
    if (auto halted = Append(HaltInstruction{}); !halted.has_value()) {
      return halted;
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
      if (source_field_real_affinity_[index]) {
        if (auto affinity = Append(RealAffinityInstruction{
                .input = destination,
                .output = destination,
            });
            !affinity.has_value()) {
          return affinity;
        }
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

  [[nodiscard]] LoweringResult<void> EmitPlan() {
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

  std::size_t next_register_ = 0;
  std::uint32_t register_count_ = 0;
  std::uint32_t parameter_count_ = 0;
  RegisterId result_block_first_{0};
  std::vector<std::optional<RegisterId>> call_argument_blocks_;
  std::optional<RegisterId> limit_register_;
  std::optional<RegisterId> offset_register_;
  std::optional<RegisterId> zero_register_;
  std::optional<RegisterId> one_register_;
  std::optional<RegisterId> comparison_register_;
  std::optional<RegisterId> negative_limit_register_;
  std::optional<RegisterId> index_key_first_;
  std::uint32_t index_key_capacity_ = 0;
  std::optional<RegisterId> index_rowid_register_;

  std::optional<ProgramBuilder> builder_;
  std::vector<std::optional<ConstantId>> literal_constants_;
  std::optional<ConstantId> null_constant_;
  std::optional<ConstantId> zero_constant_;
  std::optional<ConstantId> one_constant_;
  std::vector<SymbolId> collation_symbols_;
  std::vector<SymbolId> function_symbols_;
  std::vector<NamedSymbol> symbol_names_;
  std::optional<SymbolId> binary_symbol_;
  std::optional<CursorId> cursor_;
  std::optional<CursorId> index_cursor_;
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
