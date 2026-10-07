#include "modern_sqlite/bytecode/program.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace modern_sqlite {
namespace {

constexpr std::size_t kUnboundLabel = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kBitsPerWord = 64;
constexpr std::size_t kCursorsPerWord = 32;

enum class CursorState : std::uint8_t {
  kClosed = 0,
  kUnpositioned = 1,
  kPositioned = 2,
};

[[nodiscard]] constexpr ProgramError MakeProgramError(ProgramErrorCode code) noexcept {
  return ProgramError{.code = code};
}

[[nodiscard]] constexpr ProgramError ErrorAt(ProgramErrorCode code, std::size_t instruction,
                                             std::uint64_t detail = 0) noexcept {
  return ProgramError{
      .code = code,
      .instruction = instruction,
      .detail = detail,
  };
}

[[nodiscard]] bool CheckedAdd(std::size_t value, std::size_t* total) noexcept {
  if (value > std::numeric_limits<std::size_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

[[nodiscard]] bool CheckedMultiply(std::size_t left, std::size_t right,
                                   std::size_t* result) noexcept {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  *result = left * right;
  return true;
}

[[nodiscard]] constexpr std::size_t CeilingDivide(std::size_t value, std::size_t divisor) noexcept {
  return (value / divisor) + static_cast<std::size_t>(value % divisor != 0);
}

template <typename T>
[[nodiscard]] bool AddArrayBytes(std::size_t count, std::size_t* total) noexcept {
  std::size_t bytes = 0;
  return CheckedMultiply(count, sizeof(T), &bytes) && CheckedAdd(bytes, total);
}

[[nodiscard]] bool IsValid(TypeAffinity affinity) noexcept {
  switch (affinity) {
    case TypeAffinity::kNone:
    case TypeAffinity::kBlob:
    case TypeAffinity::kText:
    case TypeAffinity::kNumeric:
    case TypeAffinity::kInteger:
    case TypeAffinity::kReal:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(CastTarget target) noexcept {
  switch (target) {
    case CastTarget::kInteger:
    case CastTarget::kReal:
    case CastTarget::kNumeric:
    case CastTarget::kText:
    case CastTarget::kBlob:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(SqlComparison comparison) noexcept {
  switch (comparison) {
    case SqlComparison::kEqual:
    case SqlComparison::kNotEqual:
    case SqlComparison::kLess:
    case SqlComparison::kLessEqual:
    case SqlComparison::kGreater:
    case SqlComparison::kGreaterEqual:
    case SqlComparison::kIs:
    case SqlComparison::kIsNot:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(CursorStorageKind kind) noexcept {
  switch (kind) {
    case CursorStorageKind::kRowIdTable:
    case CursorStorageKind::kIndex:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(ProgramStatementKind kind) noexcept {
  switch (kind) {
    case ProgramStatementKind::kSelect:
    case ProgramStatementKind::kInsert:
    case ProgramStatementKind::kUpdate:
    case ProgramStatementKind::kDelete:
    case ProgramStatementKind::kCreateTable:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(ProgramTransactionAccess access) noexcept {
  switch (access) {
    case ProgramTransactionAccess::kRead:
    case ProgramTransactionAccess::kWrite:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(ProgramRollbackMode mode) noexcept {
  switch (mode) {
    case ProgramRollbackMode::kTransaction:
    case ProgramRollbackMode::kStatement:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(CursorFieldSourceKind kind) noexcept {
  switch (kind) {
    case CursorFieldSourceKind::kRecordField:
    case CursorFieldSourceKind::kRowId:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(MissingFieldValueKind kind) noexcept {
  switch (kind) {
    case MissingFieldValueKind::kNull:
    case MissingFieldValueKind::kConstant:
    case MissingFieldValueKind::kUnsupported:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(BytecodeSortOrder order) noexcept {
  switch (order) {
    case BytecodeSortOrder::kAscending:
    case BytecodeSortOrder::kDescending:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(RowIdSeekMode mode) noexcept {
  switch (mode) {
    case RowIdSeekMode::kEqual:
    case RowIdSeekMode::kGreater:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(UnaryOperation operation) noexcept {
  switch (operation) {
    case UnaryOperation::kNegate:
    case UnaryOperation::kBitwiseNot:
    case UnaryOperation::kLogicalNot:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(BinaryOperation operation) noexcept {
  switch (operation) {
    case BinaryOperation::kAdd:
    case BinaryOperation::kSubtract:
    case BinaryOperation::kMultiply:
    case BinaryOperation::kDivide:
    case BinaryOperation::kRemainder:
    case BinaryOperation::kConcatenate:
    case BinaryOperation::kBitwiseAnd:
    case BinaryOperation::kBitwiseOr:
    case BinaryOperation::kShiftLeft:
    case BinaryOperation::kShiftRight:
    case BinaryOperation::kLogicalAnd:
    case BinaryOperation::kLogicalOr:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(JumpCondition condition) noexcept {
  switch (condition) {
    case JumpCondition::kIfTrue:
    case JumpCondition::kIfFalse:
    case JumpCondition::kIfNull:
    case JumpCondition::kIfNotNull:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValidText(std::string_view text, bool allow_empty) noexcept {
  if ((!allow_empty && text.empty()) || text.find('\0') != std::string_view::npos) {
    return false;
  }
  return ValidateUtf8(Utf8View{text}).has_value();
}

enum class OwnedByteMeasure : std::uint8_t {
  kLogicalSize,
  kRetainedCapacity,
};

template <typename T>
[[nodiscard]] std::size_t ElementCount(const std::vector<T>& values,
                                       OwnedByteMeasure measure) noexcept {
  return measure == OwnedByteMeasure::kRetainedCapacity ? values.capacity() : values.size();
}

[[nodiscard]] std::size_t StringBytes(const std::string& value, OwnedByteMeasure measure) noexcept {
  return measure == OwnedByteMeasure::kRetainedCapacity ? value.capacity() : value.size();
}

[[nodiscard]] std::size_t DynamicValueBytes(const SqlValue& value,
                                            OwnedByteMeasure measure) noexcept {
  if (measure == OwnedByteMeasure::kRetainedCapacity) {
    return value.owned_capacity_bytes();
  }
  if (const auto text = value.text_value(); text.has_value()) {
    return text->size_bytes();
  }
  if (const auto blob = value.blob_value(); blob.has_value()) {
    return blob->size();
  }
  return 0;
}

[[nodiscard]] ProgramResult<std::size_t> ComputeOwnedBytes(const ProgramInput& input,
                                                           const ProgramLimits& limits,
                                                           OwnedByteMeasure measure) {
  std::size_t total = 0;
  const auto add = [&](std::size_t bytes) -> bool {
    return CheckedAdd(bytes, &total) && total <= limits.maximum_owned_bytes;
  };

  if (!AddArrayBytes<SqlValue>(ElementCount(input.constants, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& value : input.constants) {
    if (!add(DynamicValueBytes(value, measure))) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<std::string>(ElementCount(input.symbols, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& symbol : input.symbols) {
    if (!add(StringBytes(symbol, measure))) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<ReadCursorDescriptor>(ElementCount(input.cursors, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& cursor : input.cursors) {
    if (!AddArrayBytes<CursorFieldSource>(ElementCount(cursor.fields, measure), &total) ||
        !AddArrayBytes<IndexColumnMetadata>(ElementCount(cursor.index_columns, measure), &total) ||
        total > limits.maximum_owned_bytes) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<WriteCursorDescriptor>(ElementCount(input.write_cursors, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& cursor : input.write_cursors) {
    if (!AddArrayBytes<WriteColumnDescriptor>(ElementCount(cursor.columns, measure), &total) ||
        total > limits.maximum_owned_bytes) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<ResultColumnMetadata>(ElementCount(input.result_columns, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& column : input.result_columns) {
    const std::size_t declared_type_bytes =
        column.declared_type
            .transform([measure](const std::string& value) { return StringBytes(value, measure); })
            .value_or(0);
    if (!add(StringBytes(column.name, measure)) || !add(declared_type_bytes)) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<Instruction>(ElementCount(input.instructions, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  return total;
}

[[nodiscard]] ProgramInput CloneProgramInput(const ProgramInput& input) {
  ProgramInput clone;
  clone.schema_version = input.schema_version;
  clone.statement_kind = input.statement_kind;
  clone.transaction_access = input.transaction_access;
  clone.rollback_mode = input.rollback_mode;
  clone.mutation_result = input.mutation_result;
  clone.register_count = input.register_count;
  clone.parameter_count = input.parameter_count;
  clone.requires_database_snapshot = input.requires_database_snapshot ||
                                     input.transaction_access == ProgramTransactionAccess::kWrite ||
                                     !input.cursors.empty() || !input.write_cursors.empty();

  clone.constants.reserve(input.constants.size());
  for (const auto& value : input.constants) {
    clone.constants.push_back(value.Clone());
  }

  clone.symbols.reserve(input.symbols.size());
  for (const auto& symbol : input.symbols) {
    clone.symbols.emplace_back(symbol.data(), symbol.size());
  }

  clone.cursors.reserve(input.cursors.size());
  for (const auto& cursor : input.cursors) {
    clone.cursors.push_back(ReadCursorDescriptor{
        .root_page = cursor.root_page,
        .storage = cursor.storage,
        .record_field_count = cursor.record_field_count,
        .fields = std::vector<CursorFieldSource>(cursor.fields.begin(), cursor.fields.end()),
        .index_columns = std::vector<IndexColumnMetadata>(cursor.index_columns.begin(),
                                                          cursor.index_columns.end()),
    });
  }

  clone.write_cursors.reserve(input.write_cursors.size());
  for (const auto& cursor : input.write_cursors) {
    clone.write_cursors.push_back(WriteCursorDescriptor{
        .root_page = cursor.root_page,
        .columns = std::vector<WriteColumnDescriptor>(cursor.columns.begin(), cursor.columns.end()),
        .rowid_alias = cursor.rowid_alias,
    });
  }

  clone.result_columns.reserve(input.result_columns.size());
  for (const auto& column : input.result_columns) {
    clone.result_columns.push_back(ResultColumnMetadata{
        .name = std::string(column.name.data(), column.name.size()),
        .declared_type = column.declared_type.transform(
            [](const std::string& value) { return std::string(value.data(), value.size()); }),
        .affinity = column.affinity,
    });
  }

  clone.instructions.assign(input.instructions.begin(), input.instructions.end());
  return clone;
}

[[nodiscard]] ProgramResult<void> CheckCounts(const ProgramInput& input,
                                              const ProgramLimits& limits) {
  if (input.instructions.empty()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kEmptyProgram));
  }
  if (input.instructions.size() > limits.maximum_instructions ||
      input.instructions.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInstructionLimitExceeded));
  }
  if (static_cast<std::size_t>(input.register_count) > limits.maximum_registers) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kRegisterLimitExceeded));
  }
  if (input.cursors.size() > limits.maximum_cursors ||
      input.write_cursors.size() > limits.maximum_cursors - input.cursors.size() ||
      input.cursors.size() > std::numeric_limits<std::uint32_t>::max() ||
      input.write_cursors.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kCursorLimitExceeded));
  }
  if (static_cast<std::size_t>(input.parameter_count) > limits.maximum_parameters) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kParameterLimitExceeded));
  }
  if (input.constants.size() > limits.maximum_constants ||
      input.constants.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kConstantLimitExceeded));
  }
  if (input.symbols.size() > limits.maximum_symbols ||
      input.symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kSymbolLimitExceeded));
  }
  if (input.result_columns.size() > limits.maximum_result_columns ||
      input.result_columns.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kResultColumnLimitExceeded));
  }
  return {};
}

[[nodiscard]] ProgramResult<void> CheckExecutionMetadata(const ProgramInput& input) {
  if (!IsValid(input.statement_kind) || !IsValid(input.transaction_access) ||
      !IsValid(input.rollback_mode)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidEnumValue));
  }
  const bool is_select = input.statement_kind == ProgramStatementKind::kSelect;
  if ((is_select && input.transaction_access != ProgramTransactionAccess::kRead) ||
      (!is_select && input.transaction_access != ProgramTransactionAccess::kWrite) ||
      (is_select && input.rollback_mode != ProgramRollbackMode::kTransaction) ||
      (is_select && (!input.write_cursors.empty() || input.mutation_result.publishes_changes ||
                     input.mutation_result.publishes_last_insert_rowid)) ||
      (!is_select && !input.result_columns.empty()) ||
      (input.mutation_result.publishes_last_insert_rowid &&
       input.statement_kind != ProgramStatementKind::kInsert) ||
      (input.mutation_result.publishes_changes &&
       input.statement_kind != ProgramStatementKind::kInsert &&
       input.statement_kind != ProgramStatementKind::kUpdate &&
       input.statement_kind != ProgramStatementKind::kDelete)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidExecutionMetadata));
  }
  return {};
}

[[nodiscard]] ProgramResult<void> CheckDescriptors(const ProgramInput& input) {
  for (std::size_t cursor_index = 0; cursor_index < input.cursors.size(); ++cursor_index) {
    const auto& cursor = input.cursors[cursor_index];
    if (cursor.root_page.value() == 0) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidRootPage, ProgramError::kNoInstruction, cursor_index));
    }
    if (!IsValid(cursor.storage) || cursor.record_field_count == 0 ||
        cursor.fields.size() > std::numeric_limits<std::uint32_t>::max() ||
        cursor.index_columns.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                     ProgramError::kNoInstruction, cursor_index));
    }

    if (cursor.storage == CursorStorageKind::kRowIdTable) {
      if (!cursor.index_columns.empty()) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
    } else if (cursor.index_columns.size() != cursor.record_field_count) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                     ProgramError::kNoInstruction, cursor_index));
    }

    for (const auto& field : cursor.fields) {
      if (!IsValid(field.kind) || !IsValid(field.missing_value_kind)) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
      if (field.kind == CursorFieldSourceKind::kRecordField) {
        const bool constant_is_valid =
            field.missing_value_kind == MissingFieldValueKind::kConstant &&
            field.missing_value.has_value() &&
            field.missing_value->value() < input.constants.size();
        const bool no_constant_is_valid =
            field.missing_value_kind != MissingFieldValueKind::kConstant &&
            !field.missing_value.has_value();
        if (field.record_field >= cursor.record_field_count ||
            (!constant_is_valid && !no_constant_is_valid)) {
          return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                         ProgramError::kNoInstruction, cursor_index));
        }
      } else if (cursor.storage != CursorStorageKind::kRowIdTable || field.record_field != 0 ||
                 field.missing_value_kind != MissingFieldValueKind::kNull ||
                 field.missing_value.has_value()) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
    }

    for (const auto& column : cursor.index_columns) {
      if (column.collation.value() >= input.symbols.size() || !IsValid(column.order)) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
    }
  }
  for (std::size_t cursor_index = 0; cursor_index < input.write_cursors.size(); ++cursor_index) {
    const WriteCursorDescriptor& cursor = input.write_cursors[cursor_index];
    if (cursor.root_page.value() == 0) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidRootPage, ProgramError::kNoInstruction, cursor_index));
    }
    if (cursor.columns.empty() ||
        cursor.columns.size() > std::numeric_limits<std::uint32_t>::max() ||
        (cursor.rowid_alias.has_value() && *cursor.rowid_alias >= cursor.columns.size())) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                     ProgramError::kNoInstruction, cursor_index));
    }
    std::optional<std::uint32_t> rowid_alias;
    for (std::size_t column_index = 0; column_index < cursor.columns.size(); ++column_index) {
      const WriteColumnDescriptor& column = cursor.columns[column_index];
      if (!IsValid(column.affinity) || (column.default_value.has_value() &&
                                        column.default_value->value() >= input.constants.size())) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
      if (column.rowid_alias) {
        if (rowid_alias.has_value()) {
          return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                         ProgramError::kNoInstruction, cursor_index));
        }
        rowid_alias = static_cast<std::uint32_t>(column_index);
      }
    }
    if (cursor.rowid_alias != rowid_alias) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                     ProgramError::kNoInstruction, cursor_index));
    }
  }
  return {};
}

[[nodiscard]] ProgramResult<void> CheckTextAndMetadata(const ProgramInput& input) {
  for (std::size_t index = 0; index < input.symbols.size(); ++index) {
    if (!IsValidText(input.symbols[index], false)) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidText, ProgramError::kNoInstruction, index));
    }
  }
  for (std::size_t index = 0; index < input.result_columns.size(); ++index) {
    const auto& column = input.result_columns[index];
    const bool declared_type_is_valid =
        column.declared_type
            .transform([](const std::string& value) { return IsValidText(value, true); })
            .value_or(true);
    if (!IsValidText(column.name, true) || !declared_type_is_valid || !IsValid(column.affinity)) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidText, ProgramError::kNoInstruction, index));
    }
  }
  return {};
}

[[nodiscard]] ProgramResult<void> CheckInstructions(const ProgramInput& input) {
  const auto check_register = [&](RegisterId id, std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.register_count) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidRegister, instruction, id.value()));
    }
    return {};
  };
  const auto check_range = [&](RegisterId first, std::uint32_t count,
                               InstructionAddress instruction) -> ProgramResult<void> {
    const std::uint64_t begin = first.value();
    const std::uint64_t end = begin + count;
    if (begin > input.register_count || end > input.register_count) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidRegisterRange, instruction.value(), end));
    }
    return {};
  };
  const auto check_cursor = [&](CursorId id, std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.cursors.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursor, instruction, id.value()));
    }
    return {};
  };
  const auto check_write_cursor = [&](WriteCursorId id,
                                      std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.write_cursors.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursor, instruction, id.value()));
    }
    return {};
  };
  const auto check_symbol = [&](SymbolId id, std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.symbols.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidSymbol, instruction, id.value()));
    }
    return {};
  };
  const auto check_target = [&](InstructionAddress address,
                                std::size_t instruction) -> ProgramResult<void> {
    if (address.value() >= input.instructions.size()) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidBranchTarget, instruction, address.value()));
    }
    return {};
  };

  for (std::size_t index = 0; index < input.instructions.size(); ++index) {
    const auto checked = std::visit(
        [&](const auto& operation) -> ProgramResult<void> {
          using Operation = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Operation, HaltInstruction>) {
            return {};
          } else if constexpr (std::is_same_v<Operation, LoadConstantInstruction>) {
            if (operation.constant.value() >= input.constants.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidConstant, index, operation.constant.value()));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, LoadParameterInstruction>) {
            if (operation.parameter.value() >= input.parameter_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidParameter, index, operation.parameter.value()));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, CopyInstruction>) {
            if (auto result = check_register(operation.input, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, UnaryInstruction>) {
            if (!IsValid(operation.operation)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (auto result = check_register(operation.input, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, BinaryInstruction>) {
            if (!IsValid(operation.operation)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (auto result = check_register(operation.left, index); !result) {
              return result;
            }
            if (auto result = check_register(operation.right, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, ApplyAffinityInstruction>) {
            if (!IsValid(operation.affinity)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (auto result = check_register(operation.input, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, MustBeIntegerInstruction> ||
                               std::is_same_v<Operation, RealAffinityInstruction>) {
            if (auto result = check_register(operation.input, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, CastInstruction>) {
            if (!IsValid(operation.target)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (auto result = check_register(operation.input, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, OpenReadCursorInstruction> ||
                               std::is_same_v<Operation, CloseCursorInstruction>) {
            return check_cursor(operation.cursor, index);
          } else if constexpr (std::is_same_v<Operation, OpenWriteCursorInstruction> ||
                               std::is_same_v<Operation, CloseWriteCursorInstruction>) {
            return check_write_cursor(operation.cursor, index);
          } else if constexpr (std::is_same_v<Operation, RewindInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            return check_target(operation.empty_target, index);
          } else if constexpr (std::is_same_v<Operation, NextInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            return check_target(operation.next_target, index);
          } else if constexpr (std::is_same_v<Operation, ClearRowIdListInstruction>) {
            return ProgramResult<void>{};
          } else if constexpr (std::is_same_v<Operation, AppendRowIdListInstruction>) {
            return check_register(operation.input, index);
          } else if constexpr (std::is_same_v<Operation, RewindRowIdListInstruction>) {
            if (auto result = check_register(operation.output, index); !result) {
              return result;
            }
            return check_target(operation.empty_target, index);
          } else if constexpr (std::is_same_v<Operation, NextRowIdListInstruction>) {
            if (auto result = check_register(operation.output, index); !result) {
              return result;
            }
            return check_target(operation.next_target, index);
          } else if constexpr (std::is_same_v<Operation, SeekRowIdInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (!IsValid(operation.mode)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (input.cursors[operation.cursor.value()].storage != CursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kRowIdOperationRequiresRowIdTable,
                                             index, operation.cursor.value()));
            }
            if (auto result = check_register(operation.key, index); !result) {
              return result;
            }
            return check_target(operation.missing_target, index);
          } else if constexpr (std::is_same_v<Operation, ReadFieldInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (operation.field.value() >= input.cursors[operation.cursor.value()].fields.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidField, index, operation.field.value()));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, ReadRowIdInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage != CursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kRowIdOperationRequiresRowIdTable,
                                             index, operation.cursor.value()));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, ResolveInsertRowIdInstruction>) {
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (auto result = check_register(operation.input, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, BuildTableRecordInstruction>) {
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (operation.value_count !=
                input.write_cursors[operation.cursor.value()].columns.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.value_count));
            }
            if (auto result = check_range(operation.first_value, operation.value_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, InsertTableInstruction>) {
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (auto result = check_register(operation.rowid, index); !result) {
              return result;
            }
            return check_register(operation.record, index);
          } else if constexpr (std::is_same_v<Operation, DeleteTableInstruction>) {
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            return check_register(operation.rowid, index);
          } else if constexpr (std::is_same_v<Operation, UpdateTableInstruction>) {
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (auto result = check_register(operation.old_rowid, index); !result) {
              return result;
            }
            if (auto result = check_register(operation.new_rowid, index); !result) {
              return result;
            }
            return check_register(operation.record, index);
          } else if constexpr (std::is_same_v<Operation, EnsureDatabaseInitializedInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kCreateTable ||
                input.transaction_access != ProgramTransactionAccess::kWrite) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            return ProgramResult<void>{};
          } else if constexpr (std::is_same_v<Operation, CreateTableRootInstruction> ||
                               std::is_same_v<Operation, IncrementSchemaCookieInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kCreateTable ||
                input.transaction_access != ProgramTransactionAccess::kWrite) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, CompareInstruction>) {
            if (!IsValid(operation.comparison) || !IsValid(operation.affinity)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (auto result = check_symbol(operation.collation, index); !result) {
              return result;
            }
            if (auto result = check_register(operation.left, index); !result) {
              return result;
            }
            if (auto result = check_register(operation.right, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, CallScalarInstruction>) {
            if (auto result = check_symbol(operation.function, index); !result) {
              return result;
            }
            if (auto result = check_symbol(operation.collation, index); !result) {
              return result;
            }
            if (auto result = check_range(operation.first_argument, operation.argument_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, JumpInstruction>) {
            return check_target(operation.target, index);
          } else if constexpr (std::is_same_v<Operation, JumpIfInstruction>) {
            if (!IsValid(operation.condition)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (auto result = check_register(operation.input, index); !result) {
              return result;
            }
            return check_target(operation.target, index);
          } else {
            static_assert(std::is_same_v<Operation, ResultRowInstruction>);
            if (operation.count != input.result_columns.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kResultShapeMismatch, index, operation.count));
            }
            return check_range(operation.first, operation.count,
                               InstructionAddress(static_cast<std::uint32_t>(index)));
          }
        },
        input.instructions[index]);
    if (!checked) {
      return checked;
    }
  }
  return {};
}

[[nodiscard]] bool BitIsSet(std::span<const std::uint64_t> words, std::size_t bit) noexcept {
  return (words[bit / kBitsPerWord] & (std::uint64_t{1} << (bit % kBitsPerWord))) != 0;
}

void SetBit(std::span<std::uint64_t> words, std::size_t bit, bool value) noexcept {
  const std::uint64_t mask = std::uint64_t{1} << (bit % kBitsPerWord);
  if (value) {
    words[bit / kBitsPerWord] |= mask;
  } else {
    words[bit / kBitsPerWord] &= ~mask;
  }
}

[[nodiscard]] CursorState GetCursorState(std::span<const std::uint64_t> state,
                                         std::size_t register_words, std::size_t index) noexcept {
  const std::size_t shift = (index % kCursorsPerWord) * 2;
  const std::uint64_t word = state[register_words + (index / kCursorsPerWord)];
  return static_cast<CursorState>((word >> shift) & 0x3U);
}

void SetCursorState(std::span<std::uint64_t> state, std::size_t register_words, CursorId cursor,
                    CursorState value) noexcept {
  const std::size_t index = cursor.value();
  const std::size_t shift = (index % kCursorsPerWord) * 2;
  const std::size_t word_index = register_words + (index / kCursorsPerWord);
  const std::uint64_t mask = std::uint64_t{0x3} << shift;
  state[word_index] = (state[word_index] & ~mask) | (static_cast<std::uint64_t>(value) << shift);
}

void SetCursorState(std::span<std::uint64_t> state, std::size_t register_words, std::size_t index,
                    CursorState value) noexcept {
  const std::size_t shift = (index % kCursorsPerWord) * 2;
  const std::size_t word_index = register_words + (index / kCursorsPerWord);
  const std::uint64_t mask = std::uint64_t{0x3} << shift;
  state[word_index] = (state[word_index] & ~mask) | (static_cast<std::uint64_t>(value) << shift);
}

[[nodiscard]] ProgramResult<ProgramVerificationMetrics> AnalyzeProgram(const ProgramInput& input,
                                                                       const ProgramLimits& limits,
                                                                       std::size_t owned_bytes) {
  const std::size_t instruction_count = input.instructions.size();
  const std::size_t register_words =
      CeilingDivide(static_cast<std::size_t>(input.register_count), kBitsPerWord);
  const bool has_rowid_list =
      std::ranges::any_of(input.instructions, [](const Instruction& instruction) {
        return std::holds_alternative<ClearRowIdListInstruction>(instruction) ||
               std::holds_alternative<AppendRowIdListInstruction>(instruction) ||
               std::holds_alternative<RewindRowIdListInstruction>(instruction) ||
               std::holds_alternative<NextRowIdListInstruction>(instruction);
      });
  const std::size_t storage_cursor_count = input.cursors.size() + input.write_cursors.size();
  const std::size_t state_cursor_count = storage_cursor_count + (has_rowid_list ? 1U : 0U);
  const std::size_t rowid_list_state_index = storage_cursor_count;
  const std::size_t cursor_words = CeilingDivide(state_cursor_count, kCursorsPerWord);
  const std::size_t state_words = register_words + cursor_words;

  std::size_t stored_state_words = 0;
  if (!CheckedMultiply(instruction_count, state_words, &stored_state_words)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
  }
  const std::size_t bitset_words = CeilingDivide(instruction_count, kBitsPerWord);
  std::size_t analysis_words = stored_state_words;
  if (!CheckedAdd(bitset_words, &analysis_words) || !CheckedAdd(bitset_words, &analysis_words) ||
      !CheckedAdd(CeilingDivide(instruction_count, 2), &analysis_words) ||
      !CheckedAdd(state_words, &analysis_words) || analysis_words > limits.maximum_analysis_words) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
  }

  std::vector<std::uint64_t> states(stored_state_words);
  std::vector<std::uint64_t> reachable(bitset_words);
  std::vector<std::uint64_t> queued(bitset_words);
  std::vector<std::uint32_t> worklist;
  worklist.reserve(instruction_count);
  std::vector<std::uint64_t> scratch(state_words);

  const auto state_at = [&](std::size_t instruction) -> std::span<std::uint64_t> {
    if (state_words == 0) {
      return {};
    }
    return std::span<std::uint64_t>{states}.subspan(instruction * state_words, state_words);
  };
  const auto const_state_at = [&](std::size_t instruction) -> std::span<const std::uint64_t> {
    if (state_words == 0) {
      return {};
    }
    return std::span<const std::uint64_t>{states}.subspan(instruction * state_words, state_words);
  };

  std::size_t reachable_count = 1;
  std::size_t processed_edge_words = 0;
  SetBit(reachable, 0, true);
  SetBit(queued, 0, true);
  worklist.push_back(0);

  const auto merge_state = [&](std::size_t target,
                               std::span<const std::uint64_t> incoming) -> ProgramResult<void> {
    const std::size_t edge_words = std::max<std::size_t>(state_words, 1);
    if (edge_words > limits.maximum_analysis_edge_words - processed_edge_words) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kAnalysisWorkLimitExceeded, target));
    }
    processed_edge_words += edge_words;

    bool changed = false;
    if (!BitIsSet(reachable, target)) {
      SetBit(reachable, target, true);
      ++reachable_count;
      std::ranges::copy(incoming, state_at(target).begin());
      changed = true;
    } else {
      const auto current = state_at(target);
      for (std::size_t word = 0; word < cursor_words; ++word) {
        const std::size_t index = register_words + word;
        if (current[index] != incoming[index]) {
          std::size_t cursor_index = word * kCursorsPerWord;
          for (; cursor_index < state_cursor_count; ++cursor_index) {
            if (GetCursorState(current, register_words, cursor_index) !=
                GetCursorState(incoming, register_words, cursor_index)) {
              break;
            }
          }
          return std::unexpected(
              ErrorAt(ProgramErrorCode::kCursorStateConflict, target, cursor_index));
        }
      }
      for (std::size_t word = 0; word < register_words; ++word) {
        const std::uint64_t merged = current[word] & incoming[word];
        if (merged != current[word]) {
          current[word] = merged;
          changed = true;
        }
      }
    }

    if (changed && !BitIsSet(queued, target)) {
      SetBit(queued, target, true);
      worklist.push_back(static_cast<std::uint32_t>(target));
    }
    return {};
  };

  while (!worklist.empty()) {
    const std::size_t instruction_index = worklist.back();
    worklist.pop_back();
    SetBit(queued, instruction_index, false);
    std::ranges::copy(const_state_at(instruction_index), scratch.begin());
    auto state = std::span<std::uint64_t>{scratch};

    const auto require_initialized = [&](RegisterId id) -> ProgramResult<void> {
      if (!BitIsSet(std::span<const std::uint64_t>{state}.first(register_words), id.value())) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kUninitializedRegister, instruction_index, id.value()));
      }
      return {};
    };
    const auto require_range = [&](RegisterId first, std::uint32_t count) -> ProgramResult<void> {
      for (std::uint32_t offset = 0; offset < count; ++offset) {
        const auto id = RegisterId(first.value() + offset);
        if (auto result = require_initialized(id); !result) {
          return result;
        }
      }
      return {};
    };
    const auto initialize = [&](RegisterId id) noexcept {
      SetBit(state.first(register_words), id.value(), true);
    };
    const auto cursor_state = [&](CursorId cursor) noexcept {
      return GetCursorState(state, register_words, cursor.value());
    };
    const auto set_cursor_state = [&](CursorId cursor, CursorState value) noexcept {
      SetCursorState(state, register_words, cursor, value);
    };
    const auto write_cursor_index = [&](WriteCursorId cursor) noexcept {
      return input.cursors.size() + cursor.value();
    };
    const auto write_cursor_state = [&](WriteCursorId cursor) noexcept {
      return GetCursorState(state, register_words, write_cursor_index(cursor));
    };
    const auto set_write_cursor_state = [&](WriteCursorId cursor, CursorState value) noexcept {
      SetCursorState(state, register_words, write_cursor_index(cursor), value);
    };
    const auto rowid_list_state = [&]() noexcept {
      return GetCursorState(state, register_words, rowid_list_state_index);
    };
    const auto set_rowid_list_state = [&](CursorState value) noexcept {
      SetCursorState(state, register_words, rowid_list_state_index, value);
    };
    const auto fallthrough = [&]() -> ProgramResult<void> {
      if (instruction_index + 1 >= instruction_count) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kFallthroughPastEnd, instruction_index));
      }
      return merge_state(instruction_index + 1, state);
    };
    const auto require_open = [&](CursorId cursor) -> ProgramResult<void> {
      if (cursor_state(cursor) == CursorState::kClosed) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCursorNotOpen, instruction_index, cursor.value()));
      }
      return {};
    };
    const auto require_positioned = [&](CursorId cursor) -> ProgramResult<void> {
      if (cursor_state(cursor) != CursorState::kPositioned) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCursorNotPositioned, instruction_index, cursor.value()));
      }
      return {};
    };
    const auto require_write_open = [&](WriteCursorId cursor) -> ProgramResult<void> {
      if (write_cursor_state(cursor) == CursorState::kClosed) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCursorNotOpen, instruction_index, cursor.value()));
      }
      return {};
    };
    const auto require_rowid_list_open = [&]() -> ProgramResult<void> {
      if (!has_rowid_list || rowid_list_state() == CursorState::kClosed) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kCursorNotOpen, instruction_index,
                                       static_cast<std::uint64_t>(rowid_list_state_index)));
      }
      return {};
    };
    const auto require_rowid_list_positioned = [&]() -> ProgramResult<void> {
      if (!has_rowid_list || rowid_list_state() != CursorState::kPositioned) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kCursorNotPositioned, instruction_index,
                                       static_cast<std::uint64_t>(rowid_list_state_index)));
      }
      return {};
    };

    const auto transferred = std::visit(
        [&](const auto& operation) -> ProgramResult<void> {
          using Operation = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Operation, HaltInstruction>) {
            return {};
          } else if constexpr (std::is_same_v<Operation, LoadConstantInstruction> ||
                               std::is_same_v<Operation, LoadParameterInstruction> ||
                               std::is_same_v<Operation, CreateTableRootInstruction> ||
                               std::is_same_v<Operation, IncrementSchemaCookieInstruction>) {
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CopyInstruction> ||
                               std::is_same_v<Operation, UnaryInstruction> ||
                               std::is_same_v<Operation, ApplyAffinityInstruction> ||
                               std::is_same_v<Operation, MustBeIntegerInstruction> ||
                               std::is_same_v<Operation, RealAffinityInstruction> ||
                               std::is_same_v<Operation, CastInstruction>) {
            if (auto result = require_initialized(operation.input); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, BinaryInstruction>) {
            if (auto result = require_initialized(operation.left); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.right); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, OpenReadCursorInstruction>) {
            if (cursor_state(operation.cursor) != CursorState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorAlreadyOpen,
                                             instruction_index, operation.cursor.value()));
            }
            set_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, OpenWriteCursorInstruction>) {
            if (write_cursor_state(operation.cursor) != CursorState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorAlreadyOpen,
                                             instruction_index, operation.cursor.value()));
            }
            set_write_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CloseCursorInstruction>) {
            if (cursor_state(operation.cursor) == CursorState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorAlreadyClosed,
                                             instruction_index, operation.cursor.value()));
            }
            set_cursor_state(operation.cursor, CursorState::kClosed);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CloseWriteCursorInstruction>) {
            if (write_cursor_state(operation.cursor) == CursorState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorAlreadyClosed,
                                             instruction_index, operation.cursor.value()));
            }
            set_write_cursor_state(operation.cursor, CursorState::kClosed);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, RewindInstruction>) {
            if (auto result = require_open(operation.cursor); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kPositioned);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return merge_state(operation.empty_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, NextInstruction>) {
            if (auto result = require_positioned(operation.cursor); !result) {
              return result;
            }
            if (auto result = merge_state(operation.next_target.value(), state); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ClearRowIdListInstruction>) {
            if (rowid_list_state() != CursorState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorAlreadyOpen,
                                             instruction_index,
                                             static_cast<std::uint64_t>(rowid_list_state_index)));
            }
            set_rowid_list_state(CursorState::kUnpositioned);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, AppendRowIdListInstruction>) {
            if (auto result = require_rowid_list_open(); !result) {
              return result;
            }
            if (rowid_list_state() != CursorState::kUnpositioned) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorStateConflict,
                                             instruction_index,
                                             static_cast<std::uint64_t>(rowid_list_state_index)));
            }
            if (auto result = require_initialized(operation.input); !result) {
              return result;
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, RewindRowIdListInstruction>) {
            if (auto result = require_rowid_list_open(); !result) {
              return result;
            }
            if (rowid_list_state() != CursorState::kUnpositioned) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorStateConflict,
                                             instruction_index,
                                             static_cast<std::uint64_t>(rowid_list_state_index)));
            }
            const bool output_was_initialized =
                BitIsSet(state.first(register_words), operation.output.value());
            initialize(operation.output);
            set_rowid_list_state(CursorState::kPositioned);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            SetBit(state.first(register_words), operation.output.value(), output_was_initialized);
            set_rowid_list_state(CursorState::kUnpositioned);
            return merge_state(operation.empty_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, NextRowIdListInstruction>) {
            if (auto result = require_rowid_list_positioned(); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.output); !result) {
              return result;
            }
            if (auto result = merge_state(operation.next_target.value(), state); !result) {
              return result;
            }
            set_rowid_list_state(CursorState::kUnpositioned);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, SeekRowIdInstruction>) {
            if (auto result = require_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.key); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kPositioned);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return merge_state(operation.missing_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, ReadFieldInstruction> ||
                               std::is_same_v<Operation, ReadRowIdInstruction>) {
            if (auto result = require_positioned(operation.cursor); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ResolveInsertRowIdInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.input); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, BuildTableRecordInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_value, operation.value_count);
                !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, InsertTableInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.rowid); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.record); !result) {
              return result;
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, DeleteTableInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.rowid); !result) {
              return result;
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, UpdateTableInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.old_rowid); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.new_rowid); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.record); !result) {
              return result;
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, EnsureDatabaseInitializedInstruction>) {
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CompareInstruction>) {
            if (auto result = require_initialized(operation.left); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.right); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CallScalarInstruction>) {
            if (auto result = require_range(operation.first_argument, operation.argument_count);
                !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, JumpInstruction>) {
            return merge_state(operation.target.value(), state);
          } else if constexpr (std::is_same_v<Operation, JumpIfInstruction>) {
            if (auto result = require_initialized(operation.input); !result) {
              return result;
            }
            if (auto result = merge_state(operation.target.value(), state); !result) {
              return result;
            }
            return fallthrough();
          } else {
            static_assert(std::is_same_v<Operation, ResultRowInstruction>);
            if (auto result = require_range(operation.first, operation.count); !result) {
              return result;
            }
            return fallthrough();
          }
        },
        input.instructions[instruction_index]);
    if (!transferred) {
      return std::unexpected(transferred.error());
    }
  }

  for (std::size_t index = 0; index < instruction_count; ++index) {
    if (!BitIsSet(reachable, index)) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kUnreachableInstruction, index));
    }
  }

  return ProgramVerificationMetrics{
      .owned_bytes = owned_bytes,
      .analysis_words = analysis_words,
      .processed_edge_words = processed_edge_words,
      .reachable_instruction_count = reachable_count,
  };
}

[[nodiscard]] std::size_t CursorOwnedBytes(const ReadCursorDescriptor& cursor) noexcept {
  std::size_t result = sizeof(ReadCursorDescriptor);
  std::size_t fields = 0;
  std::size_t index_columns = 0;
  if (!CheckedMultiply(cursor.fields.size(), sizeof(CursorFieldSource), &fields) ||
      !CheckedMultiply(cursor.index_columns.size(), sizeof(IndexColumnMetadata), &index_columns) ||
      !CheckedAdd(fields, &result) || !CheckedAdd(index_columns, &result)) {
    return std::numeric_limits<std::size_t>::max();
  }
  return result;
}

[[nodiscard]] std::size_t CursorOwnedBytes(const WriteCursorDescriptor& cursor) noexcept {
  std::size_t result = sizeof(WriteCursorDescriptor);
  std::size_t columns = 0;
  if (!CheckedMultiply(cursor.columns.size(), sizeof(WriteColumnDescriptor), &columns) ||
      !CheckedAdd(columns, &result)) {
    return std::numeric_limits<std::size_t>::max();
  }
  return result;
}

[[nodiscard]] std::size_t ColumnOwnedBytes(const ResultColumnMetadata& column) {
  std::size_t result = sizeof(ResultColumnMetadata);
  const std::size_t declared_type_size =
      column.declared_type.transform([](const std::string& value) { return value.size(); })
          .value_or(0);
  if (!CheckedAdd(column.name.size(), &result) || !CheckedAdd(declared_type_size, &result)) {
    return std::numeric_limits<std::size_t>::max();
  }
  return result;
}

}  // namespace

InstructionKind InstructionKindOf(const Instruction& instruction) noexcept {
  return static_cast<InstructionKind>(instruction.index());
}

std::string_view InstructionKindName(InstructionKind kind) noexcept {
  switch (kind) {
    case InstructionKind::kHalt:
      return "halt";
    case InstructionKind::kLoadConstant:
      return "load_constant";
    case InstructionKind::kLoadParameter:
      return "load_parameter";
    case InstructionKind::kCopy:
      return "copy";
    case InstructionKind::kUnary:
      return "unary";
    case InstructionKind::kBinary:
      return "binary";
    case InstructionKind::kApplyAffinity:
      return "apply_affinity";
    case InstructionKind::kMustBeInteger:
      return "must_be_integer";
    case InstructionKind::kRealAffinity:
      return "real_affinity";
    case InstructionKind::kCast:
      return "cast";
    case InstructionKind::kOpenRead:
      return "open_read";
    case InstructionKind::kOpenWrite:
      return "open_write";
    case InstructionKind::kClose:
      return "close";
    case InstructionKind::kCloseWrite:
      return "close_write";
    case InstructionKind::kRewind:
      return "rewind";
    case InstructionKind::kNext:
      return "next";
    case InstructionKind::kClearRowIdList:
      return "clear_rowid_list";
    case InstructionKind::kAppendRowIdList:
      return "append_rowid_list";
    case InstructionKind::kRewindRowIdList:
      return "rewind_rowid_list";
    case InstructionKind::kNextRowIdList:
      return "next_rowid_list";
    case InstructionKind::kSeekRowId:
      return "seek_rowid";
    case InstructionKind::kReadField:
      return "read_field";
    case InstructionKind::kReadRowId:
      return "read_rowid";
    case InstructionKind::kResolveInsertRowId:
      return "resolve_insert_rowid";
    case InstructionKind::kBuildTableRecord:
      return "build_table_record";
    case InstructionKind::kInsertTable:
      return "insert_table";
    case InstructionKind::kDeleteTable:
      return "delete_table";
    case InstructionKind::kUpdateTable:
      return "update_table";
    case InstructionKind::kEnsureDatabaseInitialized:
      return "ensure_database_initialized";
    case InstructionKind::kCreateTableRoot:
      return "create_table_root";
    case InstructionKind::kIncrementSchemaCookie:
      return "increment_schema_cookie";
    case InstructionKind::kCompare:
      return "compare";
    case InstructionKind::kCallScalar:
      return "call_scalar";
    case InstructionKind::kJump:
      return "jump";
    case InstructionKind::kJumpIf:
      return "jump_if";
    case InstructionKind::kResultRow:
      return "result_row";
  }
  return "unknown";
}

ErrorCode ProgramError::base_error_code() const noexcept {
  switch (code) {
    case ProgramErrorCode::kInstructionLimitExceeded:
    case ProgramErrorCode::kRegisterLimitExceeded:
    case ProgramErrorCode::kCursorLimitExceeded:
    case ProgramErrorCode::kParameterLimitExceeded:
    case ProgramErrorCode::kConstantLimitExceeded:
    case ProgramErrorCode::kSymbolLimitExceeded:
    case ProgramErrorCode::kResultColumnLimitExceeded:
    case ProgramErrorCode::kLabelLimitExceeded:
    case ProgramErrorCode::kOwnedBytesLimitExceeded:
    case ProgramErrorCode::kAnalysisLimitExceeded:
    case ProgramErrorCode::kAnalysisWorkLimitExceeded:
      return ErrorCode::kTooLarge;
    case ProgramErrorCode::kEmptyProgram:
    case ProgramErrorCode::kInvalidRootPage:
    case ProgramErrorCode::kInvalidCursorDescriptor:
    case ProgramErrorCode::kInvalidExecutionMetadata:
    case ProgramErrorCode::kInvalidRegister:
    case ProgramErrorCode::kInvalidRegisterRange:
    case ProgramErrorCode::kInvalidParameter:
    case ProgramErrorCode::kInvalidConstant:
    case ProgramErrorCode::kInvalidSymbol:
    case ProgramErrorCode::kInvalidCursor:
    case ProgramErrorCode::kInvalidField:
    case ProgramErrorCode::kInvalidBranchTarget:
    case ProgramErrorCode::kInvalidEnumValue:
    case ProgramErrorCode::kInvalidText:
    case ProgramErrorCode::kResultShapeMismatch:
    case ProgramErrorCode::kBranchRequiresLabel:
    case ProgramErrorCode::kInvalidBuilder:
    case ProgramErrorCode::kForeignLabel:
    case ProgramErrorCode::kInvalidLabel:
    case ProgramErrorCode::kLabelAlreadyBound:
    case ProgramErrorCode::kUnboundLabel:
    case ProgramErrorCode::kLabelTargetOutOfRange:
    case ProgramErrorCode::kUninitializedRegister:
    case ProgramErrorCode::kCursorAlreadyOpen:
    case ProgramErrorCode::kCursorAlreadyClosed:
    case ProgramErrorCode::kCursorNotOpen:
    case ProgramErrorCode::kCursorNotPositioned:
    case ProgramErrorCode::kCursorStateConflict:
    case ProgramErrorCode::kRowIdOperationRequiresRowIdTable:
    case ProgramErrorCode::kFallthroughPastEnd:
    case ProgramErrorCode::kUnreachableInstruction:
      return ErrorCode::kMisuse;
  }
  return ErrorCode::kInternal;
}

ProgramResult<ProgramVerificationMetrics> VerifyProgram(const ProgramInput& input,
                                                        ProgramLimits limits) {
  if (auto checked = CheckCounts(input, limits); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckExecutionMetadata(input); !checked) {
    return std::unexpected(checked.error());
  }
  auto owned_bytes = ComputeOwnedBytes(input, limits, OwnedByteMeasure::kRetainedCapacity);
  if (!owned_bytes) {
    return std::unexpected(owned_bytes.error());
  }
  if (auto checked = CheckTextAndMetadata(input); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckDescriptors(input); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckInstructions(input); !checked) {
    return std::unexpected(checked.error());
  }
  return AnalyzeProgram(input, limits, *owned_bytes);
}

ProgramResult<BytecodeProgram> BytecodeProgram::Create(const ProgramInput& input,
                                                       ProgramLimits limits) {
  if (auto checked = CheckCounts(input, limits); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckExecutionMetadata(input); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto owned_bytes = ComputeOwnedBytes(input, limits, OwnedByteMeasure::kLogicalSize);
      !owned_bytes) {
    return std::unexpected(owned_bytes.error());
  }
  if (auto checked = CheckTextAndMetadata(input); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckDescriptors(input); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckInstructions(input); !checked) {
    return std::unexpected(checked.error());
  }

  ProgramInput canonical = CloneProgramInput(input);
  auto verified = VerifyProgram(canonical, limits);
  if (!verified) {
    return std::unexpected(verified.error());
  }
  return BytecodeProgram(std::move(canonical), *verified);
}

const SqlValue& BytecodeProgram::constant(ConstantId id) const noexcept {
  return input_.constants[id.value()];
}

std::string_view BytecodeProgram::symbol(SymbolId id) const noexcept {
  return input_.symbols[id.value()];
}

const ReadCursorDescriptor& BytecodeProgram::cursor(CursorId id) const noexcept {
  return input_.cursors[id.value()];
}

const WriteCursorDescriptor& BytecodeProgram::write_cursor(WriteCursorId id) const noexcept {
  return input_.write_cursors[id.value()];
}

const Instruction& BytecodeProgram::instruction(InstructionAddress address) const noexcept {
  return input_.instructions[address.value()];
}

ProgramBuilder::ProgramBuilder(std::uint64_t owner, SchemaVersionRequirement schema_version,
                               ProgramResourceCounts resources, ProgramLimits limits) noexcept
    : owner_(owner), limits_(limits) {
  input_.schema_version = schema_version;
  input_.register_count = resources.registers;
  input_.parameter_count = resources.parameters;
}

ProgramBuilder::ProgramBuilder(ProgramBuilder&& other) noexcept
    : owner_(std::exchange(other.owner_, 0)),
      limits_(other.limits_),
      owned_bytes_(std::exchange(other.owned_bytes_, 0)),
      input_(std::move(other.input_)),
      pending_(std::move(other.pending_)),
      label_addresses_(std::move(other.label_addresses_)) {}

ProgramBuilder& ProgramBuilder::operator=(ProgramBuilder&& other) noexcept {
  if (this != &other) {
    owner_ = std::exchange(other.owner_, 0);
    limits_ = other.limits_;
    owned_bytes_ = std::exchange(other.owned_bytes_, 0);
    input_ = std::move(other.input_);
    pending_ = std::move(other.pending_);
    label_addresses_ = std::move(other.label_addresses_);
  }
  return *this;
}

ProgramResult<ProgramBuilder> ProgramBuilder::Create(SchemaVersionRequirement schema_version,
                                                     ProgramResourceCounts resources,
                                                     ProgramLimits limits) {
  if (static_cast<std::size_t>(resources.registers) > limits.maximum_registers) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kRegisterLimitExceeded));
  }
  if (static_cast<std::size_t>(resources.parameters) > limits.maximum_parameters) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kParameterLimitExceeded));
  }
  static std::atomic<std::uint64_t> next_owner{1};
  const std::uint64_t owner = next_owner.fetch_add(1, std::memory_order_relaxed);
  if (owner == 0) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidLabel));
  }
  return ProgramBuilder(owner, schema_version, resources, limits);
}

ProgramResult<void> ProgramBuilder::ReserveOwnedBytes(std::size_t bytes) noexcept {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  if (bytes > limits_.maximum_owned_bytes - owned_bytes_) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  owned_bytes_ += bytes;
  return {};
}

ProgramResult<ConstantId> ProgramBuilder::AddConstant(SqlValue value) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.constants.size() >= limits_.maximum_constants ||
      input_.constants.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kConstantLimitExceeded));
  }
  std::size_t bytes = sizeof(SqlValue);
  if (!CheckedAdd(DynamicValueBytes(value, OwnedByteMeasure::kLogicalSize), &bytes)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.constants.push_back(std::move(value));
  } catch (...) {
    owned_bytes_ -= bytes;
    throw;
  }
  return ConstantId(static_cast<std::uint32_t>(input_.constants.size() - 1));
}

ProgramResult<SymbolId> ProgramBuilder::AddSymbol(std::string symbol) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.symbols.size() >= limits_.maximum_symbols ||
      input_.symbols.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kSymbolLimitExceeded));
  }
  if (!IsValidText(symbol, false)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidText));
  }
  std::size_t bytes = sizeof(std::string);
  if (!CheckedAdd(symbol.size(), &bytes)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.symbols.push_back(std::move(symbol));
  } catch (...) {
    owned_bytes_ -= bytes;
    throw;
  }
  return SymbolId(static_cast<std::uint32_t>(input_.symbols.size() - 1));
}

ProgramResult<CursorId> ProgramBuilder::AddCursor(ReadCursorDescriptor cursor) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.cursors.size() >= limits_.maximum_cursors ||
      input_.cursors.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kCursorLimitExceeded));
  }
  const std::size_t bytes = CursorOwnedBytes(cursor);
  if (bytes == std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.cursors.push_back(std::move(cursor));
    input_.requires_database_snapshot = true;
  } catch (...) {
    owned_bytes_ -= bytes;
    throw;
  }
  return CursorId(static_cast<std::uint32_t>(input_.cursors.size() - 1));
}

ProgramResult<WriteCursorId> ProgramBuilder::AddWriteCursor(WriteCursorDescriptor cursor) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.cursors.size() >= limits_.maximum_cursors ||
      input_.write_cursors.size() >= limits_.maximum_cursors - input_.cursors.size() ||
      input_.write_cursors.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kCursorLimitExceeded));
  }
  const std::size_t bytes = CursorOwnedBytes(cursor);
  if (bytes == std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.write_cursors.push_back(std::move(cursor));
    input_.requires_database_snapshot = true;
  } catch (...) {
    owned_bytes_ -= bytes;
    throw;
  }
  return WriteCursorId(static_cast<std::uint32_t>(input_.write_cursors.size() - 1));
}

ProgramResult<void> ProgramBuilder::SetExecutionMetadata(
    ProgramStatementKind statement_kind, ProgramTransactionAccess transaction_access,
    ProgramRollbackMode rollback_mode, MutationResultMetadata mutation_result) {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  input_.statement_kind = statement_kind;
  input_.transaction_access = transaction_access;
  input_.rollback_mode = rollback_mode;
  input_.mutation_result = mutation_result;
  if (transaction_access == ProgramTransactionAccess::kWrite) {
    input_.requires_database_snapshot = true;
  }
  return {};
}

ProgramResult<void> ProgramBuilder::RequireDatabaseSnapshot() {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  input_.requires_database_snapshot = true;
  return {};
}

ProgramResult<Label> ProgramBuilder::CreateLabel() {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (label_addresses_.size() >= limits_.maximum_labels ||
      label_addresses_.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kLabelLimitExceeded));
  }
  label_addresses_.push_back(kUnboundLabel);
  return Label(owner_, static_cast<std::uint32_t>(label_addresses_.size() - 1));
}

ProgramResult<void> ProgramBuilder::CheckUsable() const noexcept {
  if (owner_ == 0) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidBuilder));
  }
  return {};
}

ProgramResult<void> ProgramBuilder::CheckLabel(Label label) const noexcept {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  if (label.owner_ != owner_) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kForeignLabel));
  }
  if (label.index_ >= label_addresses_.size()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidLabel));
  }
  return {};
}

ProgramResult<void> ProgramBuilder::BindLabel(Label label) {
  if (auto checked = CheckLabel(label); !checked) {
    return checked;
  }
  if (label_addresses_[label.index_] != kUnboundLabel) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kLabelAlreadyBound));
  }
  label_addresses_[label.index_] = pending_.size();
  return {};
}

ProgramResult<InstructionAddress> ProgramBuilder::AppendPending(PendingInstruction instruction) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (pending_.size() >= limits_.maximum_instructions ||
      pending_.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInstructionLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(sizeof(Instruction)); !reserved) {
    return std::unexpected(reserved.error());
  }
  const auto address = InstructionAddress(static_cast<std::uint32_t>(pending_.size()));
  try {
    pending_.push_back(instruction);
  } catch (...) {
    owned_bytes_ -= sizeof(Instruction);
    throw;
  }
  return address;
}

ProgramResult<InstructionAddress> ProgramBuilder::Append(Instruction instruction) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (std::holds_alternative<RewindInstruction>(instruction) ||
      std::holds_alternative<NextInstruction>(instruction) ||
      std::holds_alternative<RewindRowIdListInstruction>(instruction) ||
      std::holds_alternative<NextRowIdListInstruction>(instruction) ||
      std::holds_alternative<SeekRowIdInstruction>(instruction) ||
      std::holds_alternative<JumpInstruction>(instruction) ||
      std::holds_alternative<JumpIfInstruction>(instruction)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kBranchRequiresLabel));
  }
  return AppendPending(PendingInstruction{instruction});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitRewind(CursorId cursor, Label empty_target) {
  if (auto checked = CheckLabel(empty_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingRewind{.cursor = cursor, .target = empty_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitNext(CursorId cursor, Label next_target) {
  if (auto checked = CheckLabel(next_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingNext{.cursor = cursor, .target = next_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitRewindRowIdList(RegisterId output,
                                                                      Label empty_target) {
  if (auto checked = CheckLabel(empty_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingRewindRowIdList{.output = output, .target = empty_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitNextRowIdList(RegisterId output,
                                                                    Label next_target) {
  if (auto checked = CheckLabel(next_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingNextRowIdList{.output = output, .target = next_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitSeekRowId(CursorId cursor, RegisterId key,
                                                                Label missing_target,
                                                                RowIdSeekMode mode) {
  if (auto checked = CheckLabel(missing_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(
      PendingSeekRowId{.cursor = cursor, .key = key, .target = missing_target, .mode = mode});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitJump(Label target) {
  if (auto checked = CheckLabel(target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingJump{.target = target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitJumpIf(RegisterId input,
                                                             JumpCondition condition,
                                                             Label target) {
  if (auto checked = CheckLabel(target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingJumpIf{.input = input, .condition = condition, .target = target});
}

ProgramResult<BytecodeProgram> ProgramBuilder::Build(
    std::vector<ResultColumnMetadata> result_columns) && {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  for (std::size_t index = 0; index < label_addresses_.size(); ++index) {
    if (label_addresses_[index] == kUnboundLabel) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kUnboundLabel, ProgramError::kNoInstruction, index));
    }
    if (label_addresses_[index] >= pending_.size()) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kLabelTargetOutOfRange, ProgramError::kNoInstruction, index));
    }
  }
  if (result_columns.size() > limits_.maximum_result_columns ||
      result_columns.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kResultColumnLimitExceeded));
  }
  std::size_t result_bytes = 0;
  for (const auto& column : result_columns) {
    const std::size_t bytes = ColumnOwnedBytes(column);
    if (bytes == std::numeric_limits<std::size_t>::max() || !CheckedAdd(bytes, &result_bytes)) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }
  if (result_bytes > limits_.maximum_owned_bytes - owned_bytes_) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }

  input_.instructions.reserve(pending_.size());
  const auto target = [&](Label label) {
    return InstructionAddress(static_cast<std::uint32_t>(label_addresses_[label.index_]));
  };
  for (const auto& pending : pending_) {
    input_.instructions.push_back(std::visit(
        [&](const auto& operation) -> Instruction {
          using Operation = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Operation, Instruction>) {
            return operation;
          } else if constexpr (std::is_same_v<Operation, PendingRewind>) {
            return RewindInstruction{
                .cursor = operation.cursor,
                .empty_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingNext>) {
            return NextInstruction{
                .cursor = operation.cursor,
                .next_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingRewindRowIdList>) {
            return RewindRowIdListInstruction{
                .output = operation.output,
                .empty_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingNextRowIdList>) {
            return NextRowIdListInstruction{
                .output = operation.output,
                .next_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingSeekRowId>) {
            return SeekRowIdInstruction{
                .cursor = operation.cursor,
                .key = operation.key,
                .missing_target = target(operation.target),
                .mode = operation.mode,
            };
          } else if constexpr (std::is_same_v<Operation, PendingJump>) {
            return JumpInstruction{.target = target(operation.target)};
          } else {
            static_assert(std::is_same_v<Operation, PendingJumpIf>);
            return JumpIfInstruction{
                .condition = operation.condition,
                .input = operation.input,
                .target = target(operation.target),
            };
          }
        },
        pending));
  }
  input_.result_columns = std::move(result_columns);
  return BytecodeProgram::Create(input_, limits_);
}

}  // namespace modern_sqlite
