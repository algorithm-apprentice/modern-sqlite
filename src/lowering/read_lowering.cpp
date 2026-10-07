#include "modern_sqlite/lowering/read_lowering.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
using LoweringResult = std::expected<T, ReadLoweringError>;

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

[[nodiscard]] ReadLoweringError InternalFailure(std::string detail) {
  return ReadLoweringError{
      .code = ReadLoweringErrorCode::kInternalInvariant,
      .detail = std::move(detail),
  };
}

[[nodiscard]] ReadLoweringError ProgramFailure(ProgramError error, std::string detail) {
  return ReadLoweringError{
      .code = error.base_error_code() == ErrorCode::kTooLarge
                  ? ReadLoweringErrorCode::kResourceLimit
                  : ReadLoweringErrorCode::kInternalInvariant,
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

class ReadPlanLowerer final {
 public:
  ReadPlanLowerer(const PhysicalPlan& plan, ProgramLimits limits) noexcept
      : plan_(plan),
        logical_plan_(plan.logical_plan()),
        bound_select_(logical_plan_.bound_select()),
        limits_(limits) {}

  [[nodiscard]] LowerReadPlanResult Run() {
    if (bound_select_.catalog() == nullptr) {
      return std::unexpected(InternalFailure("bound SELECT does not retain a catalog"));
    }
    if (auto inspected = InspectPlan(); !inspected.has_value()) {
      return std::unexpected(std::move(inspected.error()));
    }
    if (auto layout = BuildRegisterLayout(); !layout.has_value()) {
      return std::unexpected(std::move(layout.error()));
    }

    const CatalogVersion version = bound_select_.catalog()->version();
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

    if (bound_select_.table_source() != nullptr) {
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
    result_columns.reserve(bound_select_.result_columns().size());
    for (const BoundResultColumn& column : bound_select_.result_columns()) {
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

 private:
  [[nodiscard]] LoweringResult<void> InspectPlan() {
    const std::span<const PhysicalNode> nodes = plan_.nodes();
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

  [[nodiscard]] LoweringResult<void> BuildRegisterLayout() {
    const std::size_t expression_count = bound_select_.expressions().size();
    next_register_ = expression_count;
    call_argument_blocks_.resize(expression_count);
    literal_constants_.resize(expression_count);

    for (std::size_t index = 0; index < expression_count; ++index) {
      const auto* call =
          std::get_if<BoundScalarCallExpression>(&bound_select_.expressions()[index].payload);
      if (call == nullptr) {
        continue;
      }
      auto block = AllocateRegisters(call->arguments.size());
      if (!block.has_value()) {
        return std::unexpected(std::move(block.error()));
      }
      call_argument_blocks_[index] = *block;
    }

    auto result_block = AllocateRegisters(bound_select_.result_columns().size());
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

    if (next_register_ > limits_.maximum_registers) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kRegisterLimitExceeded},
                         "lowering register count exceeds the configured limit"));
    }
    if (bound_select_.parameters().size() > std::numeric_limits<std::uint32_t>::max() ||
        bound_select_.parameters().size() > limits_.maximum_parameters) {
      return std::unexpected(
          ProgramFailure(ProgramError{.code = ProgramErrorCode::kParameterLimitExceeded},
                         "lowering parameter count exceeds the configured limit"));
    }

    register_count_ = static_cast<std::uint32_t>(next_register_);
    parameter_count_ = static_cast<std::uint32_t>(bound_select_.parameters().size());
    return {};
  }

  [[nodiscard]] LoweringResult<void> AddBoundConstants() {
    for (std::size_t index = 0; index < bound_select_.expressions().size(); ++index) {
      const auto* literal =
          std::get_if<BoundLiteralExpression>(&bound_select_.expressions()[index].payload);
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
    collation_symbols_.reserve(bound_select_.collations().size());
    for (const BoundCollation& collation : bound_select_.collations()) {
      auto symbol = AddNamedSymbol(collation.name);
      if (!symbol.has_value()) {
        return std::unexpected(std::move(symbol.error()));
      }
      collation_symbols_.push_back(*symbol);
    }

    function_symbols_.reserve(bound_select_.functions().size());
    for (const BoundScalarFunction& function : bound_select_.functions()) {
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
    source_field_real_affinity_.assign(bound_select_.source_columns().size(), false);

    if (source_kind == BoundSourceKind::kSchemaTable) {
      constexpr std::uint32_t kSchemaFieldCount = 5;
      if (bound_select_.source_columns().size() != kSchemaFieldCount) {
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
      const CatalogSnapshot& catalog = *bound_select_.catalog();
      const TableId table_id = AssumeValue(access_table);
      const CatalogTable& table = catalog.table(table_id);
      if (!table.without_rowid) {
        if (table.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(InternalFailure("table record field count is too large"));
        }
        descriptor.record_field_count = static_cast<std::uint32_t>(table.columns.size());
        descriptor.fields.reserve(bound_select_.source_columns().size());
        for (std::size_t index = 0; index < bound_select_.source_columns().size(); ++index) {
          const BoundSourceColumn& source_column = bound_select_.source_columns()[index];
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
        descriptor.fields.reserve(bound_select_.source_columns().size());
        for (std::size_t index = 0; index < bound_select_.source_columns().size(); ++index) {
          const BoundSourceColumn& source_column = bound_select_.source_columns()[index];
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
    const BoundTruthHint left = bound_select_.expression(binary.left).properties.truth_hint;
    const BoundTruthHint right = bound_select_.expression(binary.right).properties.truth_hint;
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
    if (id.value() >= bound_select_.expressions().size()) {
      return std::unexpected(InternalFailure("bound expression ID is out of range"));
    }
    const BoundExpression& expression = bound_select_.expression(id);

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
    const LogicalNode& logical_projection = logical_plan_.node(projection_->logical_projection);
    const auto* projection = std::get_if<LogicalProjectionNode>(&logical_projection.payload);
    if (projection == nullptr ||
        projection->expressions.size() != bound_select_.result_columns().size()) {
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

  const PhysicalPlan& plan_;
  const LogicalPlan& logical_plan_;
  const BoundSelect& bound_select_;
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
};

}  // namespace

ErrorCode ReadLoweringError::base_error_code() const noexcept {
  switch (code) {
    case ReadLoweringErrorCode::kInvalidInput:
      return ErrorCode::kMisuse;
    case ReadLoweringErrorCode::kResourceLimit:
      return ErrorCode::kTooLarge;
    case ReadLoweringErrorCode::kInternalInvariant:
      return ErrorCode::kInternal;
  }
  return ErrorCode::kInternal;
}

std::string_view ReadLoweringErrorCodeName(ReadLoweringErrorCode code) noexcept {
  switch (code) {
    case ReadLoweringErrorCode::kInvalidInput:
      return "invalid_input";
    case ReadLoweringErrorCode::kResourceLimit:
      return "resource_limit";
    case ReadLoweringErrorCode::kInternalInvariant:
      return "internal_invariant";
  }
  return "unknown";
}

LowerReadPlanResult LowerReadPlan(const PhysicalPlan& plan, ProgramLimits limits) {
  if (!plan.valid()) {
    return std::unexpected(ReadLoweringError{
        .code = ReadLoweringErrorCode::kInvalidInput,
        .detail = "physical plan is invalid",
    });
  }
  return ReadPlanLowerer(plan, limits).Run();
}

}  // namespace modern_sqlite
