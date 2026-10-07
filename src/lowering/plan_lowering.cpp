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
      if (auto write_cursor = AddMutationWriteCursorDescriptor(bound_delete_->target(), nullptr);
          !write_cursor.has_value()) {
        return std::unexpected(std::move(write_cursor.error()));
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

  [[nodiscard]] LoweringResult<void> InspectPlan() {
    const std::span<const PhysicalNode> nodes = read_plan_->nodes();
    if (nodes.size() < 2U) {
      return std::unexpected(InternalFailure("physical plan has no lowering chain"));
    }

    leaf_ = &nodes.front();
    table_scan_ = std::get_if<PhysicalTableScanNode>(&leaf_->payload);
    rowid_lookup_ = std::get_if<PhysicalRowIdLookupNode>(&leaf_->payload);
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
        !std::holds_alternative<PhysicalRowIdLookupNode>(leaf_->payload)) {
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

      if (IsTableScan()) {
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
    return FinishRegisterLayout();
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
    source_field_real_affinity_.assign(bound_select_->source_columns().size(), false);

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
      if (!cursor_.has_value() || column->column.value() >= source_field_real_affinity_.size()) {
        return std::unexpected(InternalFailure("bound column has no cursor field"));
      }
      if (auto read = Append(ReadFieldInstruction{
              .cursor = AssumeValue(cursor_),
              .field = CursorFieldId{column->column.value()},
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

  [[nodiscard]] LoweringResult<void> SnapshotMutationRow() {
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
    return Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
  }

  [[nodiscard]] LoweringResult<void> EmitDeletePoint() {
    if (!write_cursor_.has_value() || !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("DELETE point mutation resources are incomplete"));
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

    if (auto opened = Append(OpenWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !opened.has_value()) {
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
    if (auto closed = Append(CloseWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !closed.has_value()) {
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

  [[nodiscard]] LoweringResult<void> EmitMutationScan(const PhysicalMutationAccess& access,
                                                      PointMutationKind kind) {
    if (cursor_ == std::nullopt || write_cursor_ == std::nullopt ||
        !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("scan mutation access metadata is incomplete"));
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

    if (auto opened = Append(OpenWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !opened.has_value()) {
      return opened;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
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
    auto rewound =
        ConvertProgramResult(AssumeValue(builder_).EmitRewind(AssumeValue(cursor_), *exhausted),
                             "unable to emit mutation scan rewind");
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (auto bound = BindLabel(*candidate); !bound.has_value()) {
      return bound;
    }
    if (auto snapshot = SnapshotMutationRow(); !snapshot.has_value()) {
      return snapshot;
    }
    if (auto residuals = EmitMutationPredicates(access.residuals, *advance);
        !residuals.has_value()) {
      return residuals;
    }
    if (auto mutated = EmitPointMutation(kind); !mutated.has_value()) {
      return mutated;
    }
    if (auto bound = BindLabel(*advance); !bound.has_value()) {
      return bound;
    }
    if (auto opened = Append(OpenReadCursorInstruction{.cursor = AssumeValue(cursor_)});
        !opened.has_value()) {
      return opened;
    }
    auto sought =
        ConvertProgramResult(AssumeValue(builder_).EmitSeekRowId(
                                 AssumeValue(cursor_), AssumeValue(source_rowid_register_),
                                 *exhausted, RowIdSeekMode::kGreater),
                             "unable to emit mutation scan advance");
    if (!sought.has_value()) {
      return std::unexpected(std::move(sought.error()));
    }
    if (auto jumped = EmitJump(*candidate); !jumped.has_value()) {
      return jumped;
    }

    if (auto bound = BindLabel(*exhausted); !bound.has_value()) {
      return bound;
    }
    if (auto closed = Append(CloseCursorInstruction{.cursor = AssumeValue(cursor_)});
        !closed.has_value()) {
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
        return EmitMutationScan(delete_plan.access, PointMutationKind::kDelete);
    }
    return std::unexpected(InternalFailure("physical DELETE access kind is invalid"));
  }

  [[nodiscard]] LoweringResult<void> EmitCollectedUpdateScan(const PhysicalMutationAccess& access) {
    if (!cursor_.has_value() || !write_cursor_.has_value() || !source_rowid_register_.has_value()) {
      return std::unexpected(InternalFailure("collected UPDATE scan resources are incomplete"));
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
    auto update_loop = CreateLabel();
    auto missing_row = CreateLabel();
    auto list_advance = CreateLabel();
    if (!no_matches.has_value()) {
      return std::unexpected(std::move(no_matches.error()));
    }
    if (!update_loop.has_value()) {
      return std::unexpected(std::move(update_loop.error()));
    }
    if (!missing_row.has_value()) {
      return std::unexpected(std::move(missing_row.error()));
    }
    if (!list_advance.has_value()) {
      return std::unexpected(std::move(list_advance.error()));
    }
    auto list_rewound = ConvertProgramResult(
        AssumeValue(builder_).EmitRewindRowIdList(AssumeValue(source_rowid_register_), *no_matches),
        "unable to rewind collected UPDATE rowids");
    if (!list_rewound.has_value()) {
      return std::unexpected(std::move(list_rewound.error()));
    }
    if (auto opened = Append(OpenWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !opened.has_value()) {
      return opened;
    }
    if (auto bound = BindLabel(*update_loop); !bound.has_value()) {
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
                             "unable to seek a collected UPDATE rowid");
    if (!row_sought.has_value()) {
      return std::unexpected(std::move(row_sought.error()));
    }
    if (auto snapshot = SnapshotMutationRow(); !snapshot.has_value()) {
      return snapshot;
    }
    if (auto updated = EmitUpdatePoint(); !updated.has_value()) {
      return updated;
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
    auto next = ConvertProgramResult(
        AssumeValue(builder_).EmitNextRowIdList(AssumeValue(source_rowid_register_), *update_loop),
        "unable to advance collected UPDATE rowids");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (auto closed = Append(CloseWriteCursorInstruction{.cursor = AssumeValue(write_cursor_)});
        !closed.has_value()) {
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
          return EmitCollectedUpdateScan(update.access);
        }
        return EmitMutationScan(update.access, PointMutationKind::kUpdate);
    }
    return std::unexpected(InternalFailure("physical UPDATE access kind is invalid"));
  }

  [[nodiscard]] LoweringResult<void> EmitCreateTable() {
    if (bound_create_->table_name().empty() || bound_create_->canonical_sql().empty()) {
      return std::unexpected(InternalFailure("CREATE TABLE metadata is incomplete"));
    }
    auto type = AddConstant(SqlValue::Text("table"));
    if (!type.has_value()) {
      return std::unexpected(std::move(type.error()));
    }
    auto name = AddConstant(SqlValue::Text(std::string{bound_create_->table_name()}));
    if (!name.has_value()) {
      return std::unexpected(std::move(name.error()));
    }
    auto table_name = AddConstant(SqlValue::Text(std::string{bound_create_->table_name()}));
    if (!table_name.has_value()) {
      return std::unexpected(std::move(table_name.error()));
    }
    auto sql = AddConstant(SqlValue::Text(std::string{bound_create_->canonical_sql()}));
    if (!sql.has_value()) {
      return std::unexpected(std::move(sql.error()));
    }
    auto null_rowid = AddConstant(SqlValue{});
    if (!null_rowid.has_value()) {
      return std::unexpected(std::move(null_rowid.error()));
    }

    WriteCursorDescriptor descriptor{
        .root_page = RootPageNumber(1),
        .columns =
            {
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kText,
                    .not_null = true,
                    .rowid_alias = false,
                    .default_value = std::nullopt,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kText,
                    .not_null = true,
                    .rowid_alias = false,
                    .default_value = std::nullopt,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kText,
                    .not_null = true,
                    .rowid_alias = false,
                    .default_value = std::nullopt,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kInteger,
                    .not_null = true,
                    .rowid_alias = false,
                    .default_value = std::nullopt,
                },
                WriteColumnDescriptor{
                    .affinity = TypeAffinity::kText,
                    .not_null = true,
                    .rowid_alias = false,
                    .default_value = std::nullopt,
                },
            },
        .rowid_alias = std::nullopt,
    };
    auto cursor = ConvertProgramResult(AssumeValue(builder_).AddWriteCursor(std::move(descriptor)),
                                       "unable to add the sqlite_schema write cursor");
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    const WriteCursorId schema_cursor = AssumeValue(cursor);

    if (auto ensured = Append(EnsureDatabaseInitializedInstruction{}); !ensured.has_value()) {
      return ensured;
    }
    if (auto root = Append(CreateTableRootInstruction{.output = RegisterId{3}});
        !root.has_value()) {
      return root;
    }
    const std::array loads{
        LoadConstantInstruction{.constant = AssumeValue(type), .output = RegisterId{0}},
        LoadConstantInstruction{.constant = AssumeValue(name), .output = RegisterId{1}},
        LoadConstantInstruction{.constant = AssumeValue(table_name), .output = RegisterId{2}},
        LoadConstantInstruction{.constant = AssumeValue(sql), .output = RegisterId{4}},
        LoadConstantInstruction{.constant = AssumeValue(null_rowid), .output = RegisterId{5}},
    };
    for (const LoadConstantInstruction& load : loads) {
      if (auto loaded = Append(load); !loaded.has_value()) {
        return loaded;
      }
    }
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
    if (auto cookie = Append(IncrementSchemaCookieInstruction{.output = RegisterId{7}});
        !cookie.has_value()) {
      return cookie;
    }
    return Append(HaltInstruction{});
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
    if (auto built = Append(BuildTableRecordInstruction{
            .cursor = AssumeValue(write_cursor_),
            .first_value = AssumeValue(insert_values_first_),
            .value_count = static_cast<std::uint32_t>(target.columns.size()),
            .output = AssumeValue(insert_record_register_),
        });
        !built.has_value()) {
      return built;
    }
    if (auto inserted = Append(InsertTableInstruction{
            .cursor = AssumeValue(write_cursor_),
            .rowid = AssumeValue(insert_rowid_register_),
            .record = AssumeValue(insert_record_register_),
        });
        !inserted.has_value()) {
      return inserted;
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
    return std::unexpected(InternalFailure("physical access node is unsupported"));
  }

  [[nodiscard]] bool IsTableScan() const noexcept { return table_scan_ != nullptr; }

  [[nodiscard]] bool IsCursorAccess() const noexcept {
    return table_scan_ != nullptr || rowid_lookup_ != nullptr;
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
  std::span<const BoundExpression> expressions_;
  std::span<const BoundParameter> parameters_;
  std::span<const BoundCollation> collations_;
  std::span<const BoundScalarFunction> functions_;
  std::span<const BoundSourceColumn> source_columns_;
  ProgramLimits limits_;

  const PhysicalNode* leaf_ = nullptr;
  const PhysicalTableScanNode* table_scan_ = nullptr;
  const PhysicalRowIdLookupNode* rowid_lookup_ = nullptr;
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
  std::vector<bool> source_field_real_affinity_;
  std::vector<RegisterId> source_snapshot_registers_;
  std::optional<RegisterId> source_rowid_register_;
  std::optional<RegisterId> insert_values_first_;
  std::optional<RegisterId> insert_rowid_register_;
  std::optional<RegisterId> insert_record_register_;
  std::vector<std::optional<ConstantId>> insert_default_constants_;
  std::optional<RegisterId> update_values_first_;
  std::optional<RegisterId> update_new_rowid_register_;
  std::optional<RegisterId> update_record_register_;
  std::optional<WriteCursorId> write_cursor_;
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
