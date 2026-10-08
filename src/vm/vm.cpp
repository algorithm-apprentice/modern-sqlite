#include "modern_sqlite/vm/vm.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/storage/page_number.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"

namespace modern_sqlite {
namespace {

using DispatchResult = Result<std::optional<VmStep>>;

[[nodiscard]] Error VmError(ErrorCode code, std::string message) {
  return Error::Create(code, std::move(message));
}

[[nodiscard]] bool EqualAsciiCaseInsensitive(std::string_view left,
                                             std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    auto left_byte = static_cast<unsigned char>(left[index]);
    auto right_byte = static_cast<unsigned char>(right[index]);
    if (left_byte >= static_cast<unsigned char>('A') &&
        left_byte <= static_cast<unsigned char>('Z')) {
      left_byte = static_cast<unsigned char>(left_byte + static_cast<unsigned char>('a' - 'A'));
    }
    if (right_byte >= static_cast<unsigned char>('A') &&
        right_byte <= static_cast<unsigned char>('Z')) {
      right_byte = static_cast<unsigned char>(right_byte + static_cast<unsigned char>('a' - 'A'));
    }
    if (left_byte != right_byte) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::size_t ValueByteSize(const SqlValue& value) noexcept {
  if (const auto text = value.text_value(); text.has_value()) {
    return text->bytes().size();
  }
  if (const auto blob = value.blob_value(); blob.has_value()) {
    return blob->size();
  }
  return 0;
}

[[nodiscard]] double NumericAsDouble(const SqlValue& value) noexcept {
  if (const auto integer = value.integer_value(); integer.has_value()) {
    return static_cast<double>(*integer);
  }
  return value.real_value().value_or(0.0);
}

[[nodiscard]] std::int64_t NumericAsInteger(const SqlValue& value) {
  return CoerceIntegerForBitwise(value);
}

[[nodiscard]] SqlTruthValue TruthValue(const SqlValue& value) noexcept {
  return EvaluateSqlTruth(value);
}

[[nodiscard]] std::string_view ConcatenationBytes(const SqlValue& value,
                                                  std::optional<SqlValue>& converted) {
  if (const auto text = value.text_value(); text.has_value()) {
    return text->bytes();
  }
  if (const auto blob = value.blob_value(); blob.has_value()) {
    return AsStringView(*blob);
  }
  converted.emplace(CastValue(value.Clone(), CastTarget::kText));
  return converted->text_value().value_or(Utf8View{}).bytes();
}

[[nodiscard]] std::int64_t BitwiseAnd(std::int64_t left, std::int64_t right) noexcept {
  const std::uint64_t result =
      std::bit_cast<std::uint64_t>(left) & std::bit_cast<std::uint64_t>(right);
  return std::bit_cast<std::int64_t>(result);
}

[[nodiscard]] std::int64_t BitwiseOr(std::int64_t left, std::int64_t right) noexcept {
  const std::uint64_t result =
      std::bit_cast<std::uint64_t>(left) | std::bit_cast<std::uint64_t>(right);
  return std::bit_cast<std::int64_t>(result);
}

[[nodiscard]] std::int64_t BitwiseNot(std::int64_t value) noexcept {
  return std::bit_cast<std::int64_t>(~std::bit_cast<std::uint64_t>(value));
}

struct ShiftArguments {
  std::int64_t value;
  std::int64_t count;
};

[[nodiscard]] std::int64_t ShiftInteger(ShiftArguments arguments, bool shift_left) noexcept {
  const std::int64_t value = arguments.value;
  const std::int64_t count = arguments.count;
  std::uint64_t amount = 0;
  if (count < 0) {
    shift_left = !shift_left;
    amount = count <= -64 ? 64U : static_cast<std::uint64_t>(-count);
  } else {
    amount = static_cast<std::uint64_t>(count);
    if (amount > 64U) {
      amount = 64U;
    }
  }

  if (amount == 0U) {
    return value;
  }
  if (shift_left) {
    if (amount >= 64U) {
      return 0;
    }
    const std::uint64_t shifted = std::bit_cast<std::uint64_t>(value) << amount;
    return std::bit_cast<std::int64_t>(shifted);
  }
  if (amount >= 64U) {
    return value < 0 ? -1 : 0;
  }
  if (value >= 0) {
    const std::uint64_t shifted = std::bit_cast<std::uint64_t>(value) >> amount;
    return std::bit_cast<std::int64_t>(shifted);
  }
  std::uint64_t shifted = std::bit_cast<std::uint64_t>(value) >> amount;
  shifted |= std::numeric_limits<std::uint64_t>::max() << (64U - amount);
  return std::bit_cast<std::int64_t>(shifted);
}

[[nodiscard]] SqlValue LogicalBinary(SqlTruthValue left, SqlTruthValue right,
                                     bool is_and) noexcept {
  if (is_and) {
    if (left == SqlTruthValue::kFalse || right == SqlTruthValue::kFalse) {
      return SqlValue::Integer(0);
    }
    if (left == SqlTruthValue::kNull || right == SqlTruthValue::kNull) {
      return {};
    }
    return SqlValue::Integer(1);
  }

  if (left == SqlTruthValue::kTrue || right == SqlTruthValue::kTrue) {
    return SqlValue::Integer(1);
  }
  if (left == SqlTruthValue::kNull || right == SqlTruthValue::kNull) {
    return {};
  }
  return SqlValue::Integer(0);
}

[[nodiscard]] std::optional<std::int64_t> LosslessRowId(const SqlValue& value) {
  if (value.type() == SqlValueType::kInteger) {
    return value.integer_value();
  }
  if (value.type() == SqlValueType::kNull || value.type() == SqlValueType::kBlob) {
    return std::nullopt;
  }

  const SqlValue numeric = value.type() == SqlValueType::kText
                               ? ApplyAffinity(value.Clone(), TypeAffinity::kNumeric)
                               : value.Clone();
  if (numeric.type() == SqlValueType::kInteger) {
    return numeric.integer_value();
  }
  const auto real = numeric.real_value();
  if (!real.has_value() || !std::isfinite(*real)) {
    return std::nullopt;
  }
  const auto minimum = static_cast<double>(std::numeric_limits<std::int64_t>::min());
  const double maximum_exclusive = -minimum;
  if (*real < minimum || *real >= maximum_exclusive) {
    return std::nullopt;
  }
  const auto converted = static_cast<std::int64_t>(*real);
  if (static_cast<double>(converted) != *real) {
    return std::nullopt;
  }
  return converted;
}

[[nodiscard]] TypeAffinity StorageAffinity(TypeAffinity affinity) noexcept {
  return affinity == TypeAffinity::kReal ? TypeAffinity::kNumeric : affinity;
}

struct ResolvedCall {
  std::uint32_t address = 0;
  const ScalarFunction* function = nullptr;
  const Collation* collation = nullptr;
};

struct RuntimeCursor {
  using Storage =
      std::variant<std::monostate, TableBtreeCursor, IndexBtreeCursor, TableBtreeMutationCursor>;

  void ClearRecordCache() noexcept {
    for (RecordFieldView& field : decoded_fields) {
      field = RecordFieldView{};
    }
    fields_decoded = false;
    record.reset();
    owned_record.reset();
  }

  Storage storage;
  std::optional<ByteBuffer> owned_record;
  std::optional<RecordView> record;
  std::vector<RecordFieldView> decoded_fields;
  bool fields_decoded = false;
};

struct RuntimeWriteCursor {
  std::optional<TableBtreeWriter> table;
  std::optional<IndexBtreeWriter> index;
};

}  // namespace

struct Vm::Impl {
  Impl(const BytecodeProgram& program, VmEnvironment environment, VmLimits limits)
      : program_(&program),
        functions_(environment.functions_),
        available_collations_(environment.collations_),
        limits_(limits),
        registers_(program.register_count()),
        parameters_(program.parameter_count()),
        cursors_(program.cursors().size()),
        write_cursors_(program.write_cursors().size()),
        resolved_collations_(program.symbols().size(), nullptr) {}

  [[nodiscard]] Status Initialize() {
    if (functions_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "VM environment is incomplete"));
    }
    for (const Collation* collation : available_collations_) {
      if (collation == nullptr) {
        return std::unexpected(
            VmError(ErrorCode::kMisuse, "VM environment contains a null collation"));
      }
    }
    for (const SqlValue& constant : program_->constants()) {
      if (ValueByteSize(constant) > limits_.maximum_value_bytes) {
        return std::unexpected(
            VmError(ErrorCode::kTooLarge, "bytecode constant exceeds the VM value limit"));
      }
    }

    for (const ReadCursorDescriptor& descriptor : program_->cursors()) {
      if (descriptor.storage != CursorStorageKind::kIndex) {
        continue;
      }
      for (const IndexColumnMetadata& column : descriptor.index_columns) {
        auto collation = ResolveCollation(column.collation);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
      }
    }
    for (const WriteCursorDescriptor& descriptor : program_->write_cursors()) {
      if (descriptor.storage != WriteCursorStorageKind::kIndex) {
        continue;
      }
      for (const IndexColumnMetadata& column : descriptor.index_columns) {
        auto collation = ResolveCollation(column.collation);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
      }
    }

    const std::span<const Instruction> instructions = program_->instructions();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const Instruction& instruction = instructions[index];
      if (const auto* compare = std::get_if<CompareInstruction>(&instruction); compare != nullptr) {
        auto collation = ResolveCollation(compare->collation);
        if (!collation.has_value()) {
          return std::unexpected(std::move(collation.error()));
        }
        continue;
      }
      const auto* call = std::get_if<CallScalarInstruction>(&instruction);
      if (call == nullptr) {
        continue;
      }
      auto function = functions_->Resolve(program_->symbol(call->function), call->argument_count);
      if (!function.has_value()) {
        return std::unexpected(std::move(function.error()));
      }
      auto collation = ResolveCollation(call->collation);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      resolved_calls_.push_back(ResolvedCall{
          .address = static_cast<std::uint32_t>(index),
          .function = *function,
          .collation = *collation,
      });
    }
    functions_ = nullptr;
    available_collations_ = {};
    return {};
  }

  [[nodiscard]] Status AttachExecutionContext(VmExecutionContext context) {
    if (state_ != VmState::kReady || pager_ != nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "execution context can only attach to a detached ready VM"));
    }
    if (context.pager_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "VM execution context is incomplete"));
    }
    if (program_->transaction_access() == ProgramTransactionAccess::kWrite &&
        context.writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "write bytecode requires a transaction writer"));
    }
    pager_ = context.pager_;
    catalog_generation_ = context.catalog_generation_;
    writer_ = context.writer_;
    Status schema = ValidateSchema();
    if (!schema.has_value()) {
      pager_ = nullptr;
      catalog_generation_ = 0;
      writer_ = nullptr;
      return schema;
    }
    return {};
  }

  [[nodiscard]] Status DetachExecutionContext() {
    if (pager_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "VM execution context is not attached"));
    }
    if (state_ == VmState::kRow) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "cannot detach a VM while a result row is suspended"));
    }
    CloseAllCursors();
    execution_data_version_.reset();
    pager_ = nullptr;
    catalog_generation_ = 0;
    writer_ = nullptr;
    return {};
  }

  [[nodiscard]] Status Bind(ParameterId parameter, const SqlValue& value) {
    if (state_ != VmState::kReady) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "parameters can only be bound while the VM is ready"));
    }
    if (parameter.value() >= parameters_.size()) {
      return std::unexpected(VmError(ErrorCode::kOutOfRange, "parameter ID is out of range"));
    }
    if (ValueByteSize(value) > limits_.maximum_value_bytes) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "bound value exceeds the VM value limit"));
    }
    SqlValue replacement = value.Clone();
    parameters_[parameter.value()] = std::move(replacement);
    return {};
  }

  [[nodiscard]] Status ClearBindings() {
    if (state_ != VmState::kReady) {
      return std::unexpected(VmError(
          ErrorCode::kMisuse, "parameter bindings can only be cleared while the VM is ready"));
    }
    for (SqlValue& parameter : parameters_) {
      parameter = {};
    }
    return {};
  }

  [[nodiscard]] Result<VmStep> Step() {
    if (state_ != VmState::kReady && state_ != VmState::kRow) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "the VM cannot step from its current state"));
    }
    if (pager_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "the VM has no attached execution context"));
    }

    if (state_ == VmState::kReady) {
      Status schema = ValidateSchema();
      if (!schema.has_value()) {
        return Fail(std::move(schema.error()));
      }
      if (program_->requires_database_snapshot()) {
        execution_data_version_ = pager_->data_version();
      } else {
        execution_data_version_.reset();
      }
    } else {
      ClearRow();
      if (program_->requires_database_snapshot() && !pager_->in_read_transaction()) {
        return Fail(
            VmError(ErrorCode::kMisuse, "the database snapshot ended while the VM was suspended"));
      }
      if (program_->requires_database_snapshot() &&
          (!execution_data_version_.has_value() ||
           pager_->data_version() != *execution_data_version_)) {
        return Fail(VmError(ErrorCode::kSchemaChanged,
                            "the database snapshot changed while the VM was suspended"));
      }
    }

    std::uint64_t step_instruction_count = 0;
    while (true) {
      if (step_instruction_count >= limits_.maximum_instructions_per_step) {
        return Fail(VmError(ErrorCode::kInterrupted, "VM instruction budget exhausted"));
      }
      ++step_instruction_count;
      if (executed_instruction_count_ != std::numeric_limits<std::uint64_t>::max()) {
        ++executed_instruction_count_;
      }
      MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kVmInstructions, 1U);

      const std::uint32_t address = program_counter_;
      const Instruction& instruction = program_->instruction(InstructionAddress(address));
      ++program_counter_;
      DispatchResult result = Dispatch(address, instruction);
      if (!result.has_value()) {
        return Fail(std::move(result.error()));
      }
      if (result->has_value()) {
        return **result;
      }
    }
  }

  [[nodiscard]] Status Reset() {
    CloseAllCursors();
    for (SqlValue& value : registers_) {
      value = {};
    }
    ClearRow();
    execution_data_version_.reset();
    program_counter_ = 0;
    executed_instruction_count_ = 0;
    change_count_ = 0;
    last_insert_rowid_event_.reset();
    rowid_list_.clear();
    rowid_list_index_ = 0;
    rowid_list_positioned_ = false;
    state_ = VmState::kReady;
    pager_ = nullptr;
    catalog_generation_ = 0;
    writer_ = nullptr;
    return {};
  }

  [[nodiscard]] std::span<const SqlValue> row() const noexcept {
    if (state_ != VmState::kRow || row_count_ == 0U) {
      return {};
    }
    return std::span<const SqlValue>{registers_}.subspan(row_first_, row_count_);
  }

  [[nodiscard]] std::span<const SqlValue> bindings() const noexcept { return parameters_; }

  [[nodiscard]] Result<VmStep> Fail(Error error) noexcept {
    CloseAllCursors();
    ClearRow();
    execution_data_version_.reset();
    state_ = VmState::kError;
    return std::unexpected(std::move(error));
  }

  void FailForException() noexcept {
    CloseAllCursors();
    ClearRow();
    execution_data_version_.reset();
    state_ = VmState::kError;
  }

  [[nodiscard]] Status ValidateSchema() {
    const SchemaVersionRequirement expected = program_->schema_version();
    if (!pager_->in_read_transaction() && program_->requires_database_snapshot()) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "an active database snapshot is required"));
    }
    if (catalog_generation_ != expected.generation) {
      return std::unexpected(
          VmError(ErrorCode::kSchemaChanged, "the bytecode catalog generation is stale"));
    }
    if (!pager_->in_read_transaction()) {
      return {};
    }
    const DatabaseHeader* header = pager_->header();
    const std::uint32_t schema_cookie = header == nullptr ? 0U : header->schema_cookie();
    if (schema_cookie != expected.schema_cookie) {
      return std::unexpected(
          VmError(ErrorCode::kSchemaChanged, "the bytecode schema version is stale"));
    }

    const std::uint32_t raw_schema_format = header == nullptr ? 0U : header->schema_format();
    auto schema_format = NormalizeSchemaFormat(raw_schema_format);
    if (!schema_format.has_value()) {
      return std::unexpected(std::move(schema_format.error()));
    }
    const std::uint32_t raw_encoding = header == nullptr ? 0U : header->text_encoding();
    auto encoding = NormalizeTextEncoding(raw_encoding);
    if (!encoding.has_value()) {
      return std::unexpected(std::move(encoding.error()));
    }
    if (*encoding != DatabaseTextEncoding::kUtf8) {
      return std::unexpected(
          VmError(ErrorCode::kProtocol, "the VM currently supports only UTF-8 databases"));
    }
    record_options_.schema_format = *schema_format;
    return {};
  }

  [[nodiscard]] Result<const Collation*> ResolveCollation(SymbolId symbol) {
    const std::size_t index = symbol.value();
    if (resolved_collations_[index] != nullptr) {
      return resolved_collations_[index];
    }
    const std::string_view name = program_->symbol(symbol);
    for (const Collation* collation : available_collations_) {
      if (EqualAsciiCaseInsensitive(name, collation->name())) {
        resolved_collations_[index] = collation;
        return collation;
      }
    }
    return std::unexpected(VmError(ErrorCode::kGeneric, "required collation is not registered"));
  }

  [[nodiscard]] const Collation& CollationFor(SymbolId symbol) const noexcept {
    return *resolved_collations_[symbol.value()];
  }

  [[nodiscard]] DispatchResult Dispatch(std::uint32_t address, const Instruction& instruction) {
    return std::visit(
        [this, address](const auto& operation) { return Execute(address, operation); },
        instruction);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const LoadConstantInstruction& operation) {
    return SetRegister(operation.output, program_->constant(operation.constant).Clone());
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const LoadParameterInstruction& operation) {
    return SetRegister(operation.output, parameters_[operation.parameter.value()].Clone());
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CopyInstruction& operation) {
    return SetRegister(operation.output, Register(operation.input).Clone());
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const UnaryInstruction& operation) {
    const SqlValue& input = Register(operation.input);
    SqlValue output;
    switch (operation.operation) {
      case UnaryOperation::kNegate: {
        const SqlValue numeric = CoerceNumericForArithmetic(input);
        if (numeric.type() == SqlValueType::kNull) {
          break;
        }
        if (const auto integer = numeric.integer_value(); integer.has_value()) {
          if (*integer == std::numeric_limits<std::int64_t>::min()) {
            output = SqlValue::Real(-static_cast<double>(*integer));
          } else {
            output = SqlValue::Integer(-*integer);
          }
        } else {
          output = SqlValue::Real(-numeric.real_value().value_or(0.0));
        }
        break;
      }
      case UnaryOperation::kBitwiseNot:
        if (input.type() != SqlValueType::kNull) {
          output = SqlValue::Integer(BitwiseNot(NumericAsInteger(input)));
        }
        break;
      case UnaryOperation::kLogicalNot: {
        const SqlTruthValue truth = TruthValue(input);
        if (truth != SqlTruthValue::kNull) {
          output = SqlValue::Integer(truth == SqlTruthValue::kFalse ? 1 : 0);
        }
        break;
      }
    }
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult ExecuteArithmetic(const BinaryInstruction& operation) {
    const SqlValue left = CoerceNumericForArithmetic(Register(operation.left));
    const SqlValue right = CoerceNumericForArithmetic(Register(operation.right));
    SqlValue output;
    if (left.type() == SqlValueType::kNull || right.type() == SqlValueType::kNull) {
      return SetRegister(operation.output, std::move(output));
    }

    const auto left_integer = left.integer_value();
    const auto right_integer = right.integer_value();
    if (operation.operation == BinaryOperation::kRemainder) {
      const std::int64_t divisor = NumericAsInteger(right);
      if (divisor == 0) {
        return SetRegister(operation.output, std::move(output));
      }
      const std::int64_t dividend = NumericAsInteger(left);
      const std::int64_t remainder =
          dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1
              ? 0
              : dividend % divisor;
      output = left.type() == SqlValueType::kReal || right.type() == SqlValueType::kReal
                   ? SqlValue::Real(static_cast<double>(remainder))
                   : SqlValue::Integer(remainder);
      return SetRegister(operation.output, std::move(output));
    }

    if (left_integer.has_value() && right_integer.has_value()) {
      CodingResult<std::int64_t> checked = std::unexpected(CodingError::kOverflow);
      switch (operation.operation) {
        case BinaryOperation::kAdd:
          checked = CheckedAdd(*left_integer, *right_integer);
          break;
        case BinaryOperation::kSubtract:
          checked = CheckedSubtract(*left_integer, *right_integer);
          break;
        case BinaryOperation::kMultiply:
          checked = CheckedMultiply(*left_integer, *right_integer);
          break;
        case BinaryOperation::kDivide:
          if (*right_integer == 0) {
            return SetRegister(operation.output, std::move(output));
          }
          if (*left_integer == std::numeric_limits<std::int64_t>::min() && *right_integer == -1) {
            output = SqlValue::Real(static_cast<double>(*left_integer) /
                                    static_cast<double>(*right_integer));
          } else {
            output = SqlValue::Integer(*left_integer / *right_integer);
          }
          return SetRegister(operation.output, std::move(output));
        case BinaryOperation::kRemainder:
        case BinaryOperation::kConcatenate:
        case BinaryOperation::kBitwiseAnd:
        case BinaryOperation::kBitwiseOr:
        case BinaryOperation::kShiftLeft:
        case BinaryOperation::kShiftRight:
        case BinaryOperation::kLogicalAnd:
        case BinaryOperation::kLogicalOr:
          break;
      }
      if (checked.has_value()) {
        output = SqlValue::Integer(*checked);
      } else {
        const auto left_real = static_cast<double>(*left_integer);
        const auto right_real = static_cast<double>(*right_integer);
        switch (operation.operation) {
          case BinaryOperation::kAdd:
            output = SqlValue::Real(left_real + right_real);
            break;
          case BinaryOperation::kSubtract:
            output = SqlValue::Real(left_real - right_real);
            break;
          case BinaryOperation::kMultiply:
            output = SqlValue::Real(left_real * right_real);
            break;
          case BinaryOperation::kDivide:
          case BinaryOperation::kRemainder:
          case BinaryOperation::kConcatenate:
          case BinaryOperation::kBitwiseAnd:
          case BinaryOperation::kBitwiseOr:
          case BinaryOperation::kShiftLeft:
          case BinaryOperation::kShiftRight:
          case BinaryOperation::kLogicalAnd:
          case BinaryOperation::kLogicalOr:
            break;
        }
      }
      return SetRegister(operation.output, std::move(output));
    }

    const double left_real = NumericAsDouble(left);
    const double right_real = NumericAsDouble(right);
    switch (operation.operation) {
      case BinaryOperation::kAdd:
        output = SqlValue::Real(left_real + right_real);
        break;
      case BinaryOperation::kSubtract:
        output = SqlValue::Real(left_real - right_real);
        break;
      case BinaryOperation::kMultiply:
        output = SqlValue::Real(left_real * right_real);
        break;
      case BinaryOperation::kDivide:
        if (right_real != 0.0) {
          output = SqlValue::Real(left_real / right_real);
        }
        break;
      case BinaryOperation::kRemainder:
      case BinaryOperation::kConcatenate:
      case BinaryOperation::kBitwiseAnd:
      case BinaryOperation::kBitwiseOr:
      case BinaryOperation::kShiftLeft:
      case BinaryOperation::kShiftRight:
      case BinaryOperation::kLogicalAnd:
      case BinaryOperation::kLogicalOr:
        break;
    }
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult ExecuteConcatenate(const BinaryInstruction& operation) {
    const SqlValue& left = Register(operation.left);
    const SqlValue& right = Register(operation.right);
    SqlValue output;
    if (left.type() == SqlValueType::kNull || right.type() == SqlValueType::kNull) {
      return SetRegister(operation.output, std::move(output));
    }
    std::optional<SqlValue> converted_left;
    std::optional<SqlValue> converted_right;
    const std::string_view left_bytes = ConcatenationBytes(left, converted_left);
    const std::string_view right_bytes = ConcatenationBytes(right, converted_right);
    if (left_bytes.size() > limits_.maximum_value_bytes ||
        right_bytes.size() > limits_.maximum_value_bytes - left_bytes.size()) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "concatenated value exceeds the VM value limit"));
    }
    std::string bytes;
    bytes.reserve(left_bytes.size() + right_bytes.size());
    bytes.append(left_bytes);
    bytes.append(right_bytes);
    output = SqlValue::Text(std::move(bytes));
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult ExecuteBitwise(const BinaryInstruction& operation) {
    const SqlValue& left = Register(operation.left);
    const SqlValue& right = Register(operation.right);
    SqlValue output;
    if (left.type() == SqlValueType::kNull || right.type() == SqlValueType::kNull) {
      return SetRegister(operation.output, std::move(output));
    }
    const std::int64_t left_integer = NumericAsInteger(left);
    const std::int64_t right_integer = NumericAsInteger(right);
    switch (operation.operation) {
      case BinaryOperation::kBitwiseAnd:
        output = SqlValue::Integer(BitwiseAnd(left_integer, right_integer));
        break;
      case BinaryOperation::kBitwiseOr:
        output = SqlValue::Integer(BitwiseOr(left_integer, right_integer));
        break;
      case BinaryOperation::kShiftLeft:
        output = SqlValue::Integer(
            ShiftInteger(ShiftArguments{.value = left_integer, .count = right_integer}, true));
        break;
      case BinaryOperation::kShiftRight:
        output = SqlValue::Integer(
            ShiftInteger(ShiftArguments{.value = left_integer, .count = right_integer}, false));
        break;
      case BinaryOperation::kAdd:
      case BinaryOperation::kSubtract:
      case BinaryOperation::kMultiply:
      case BinaryOperation::kDivide:
      case BinaryOperation::kRemainder:
      case BinaryOperation::kConcatenate:
      case BinaryOperation::kLogicalAnd:
      case BinaryOperation::kLogicalOr:
        break;
    }
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult ExecuteLogical(const BinaryInstruction& operation) {
    SqlValue output =
        LogicalBinary(TruthValue(Register(operation.left)), TruthValue(Register(operation.right)),
                      operation.operation == BinaryOperation::kLogicalAnd);
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const BinaryInstruction& operation) {
    switch (operation.operation) {
      case BinaryOperation::kAdd:
      case BinaryOperation::kSubtract:
      case BinaryOperation::kMultiply:
      case BinaryOperation::kDivide:
      case BinaryOperation::kRemainder:
        return ExecuteArithmetic(operation);
      case BinaryOperation::kConcatenate:
        return ExecuteConcatenate(operation);
      case BinaryOperation::kBitwiseAnd:
      case BinaryOperation::kBitwiseOr:
      case BinaryOperation::kShiftLeft:
      case BinaryOperation::kShiftRight:
        return ExecuteBitwise(operation);
      case BinaryOperation::kLogicalAnd:
      case BinaryOperation::kLogicalOr:
        return ExecuteLogical(operation);
    }
    return std::unexpected(VmError(ErrorCode::kInternal, "unknown binary operation"));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ApplyAffinityInstruction& operation) {
    SqlValue output =
        modern_sqlite::ApplyAffinity(Register(operation.input).Clone(), operation.affinity);
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const MustBeIntegerInstruction& operation) {
    const std::optional<std::int64_t> integer = LosslessRowId(Register(operation.input));
    if (!integer.has_value()) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
    }
    return SetRegister(operation.output, SqlValue::Integer(*integer));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const RealAffinityInstruction& operation) {
    const SqlValue& input = Register(operation.input);
    SqlValue output = input.type() == SqlValueType::kInteger
                          ? SqlValue::Real(static_cast<double>(input.integer_value().value_or(0)))
                          : input.Clone();
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CastInstruction& operation) {
    SqlValue output = CastValue(Register(operation.input).Clone(), operation.target);
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CompareInstruction& operation) {
    const SqlValue& original_left = Register(operation.left);
    const SqlValue& original_right = Register(operation.right);
    const SqlValue* left = &original_left;
    const SqlValue* right = &original_right;
    std::optional<SqlValue> converted_left;
    std::optional<SqlValue> converted_right;

    if (original_left.type() != SqlValueType::kNull &&
        original_right.type() != SqlValueType::kNull &&
        !(original_left.type() == SqlValueType::kInteger &&
          original_right.type() == SqlValueType::kInteger)) {
      if (operation.affinity == TypeAffinity::kNumeric ||
          operation.affinity == TypeAffinity::kInteger ||
          operation.affinity == TypeAffinity::kReal) {
        if (original_left.type() == SqlValueType::kText) {
          converted_left.emplace(
              modern_sqlite::ApplyAffinity(original_left.Clone(), TypeAffinity::kNumeric));
          left = &*converted_left;
        }
        if (original_right.type() == SqlValueType::kText) {
          converted_right.emplace(
              modern_sqlite::ApplyAffinity(original_right.Clone(), TypeAffinity::kNumeric));
          right = &*converted_right;
        }
      } else if (operation.affinity == TypeAffinity::kText &&
                 (original_left.type() == SqlValueType::kText ||
                  original_right.type() == SqlValueType::kText)) {
        if (original_left.type() == SqlValueType::kInteger ||
            original_left.type() == SqlValueType::kReal) {
          converted_left.emplace(
              modern_sqlite::ApplyAffinity(original_left.Clone(), TypeAffinity::kText));
          left = &*converted_left;
        }
        if (original_right.type() == SqlValueType::kInteger ||
            original_right.type() == SqlValueType::kReal) {
          converted_right.emplace(
              modern_sqlite::ApplyAffinity(original_right.Clone(), TypeAffinity::kText));
          right = &*converted_right;
        }
      }
    }

    const SqlTruthValue comparison = EvaluateSqlComparison(*left, *right, operation.comparison,
                                                           CollationFor(operation.collation));
    SqlValue output;
    if (comparison != SqlTruthValue::kNull) {
      output = SqlValue::Integer(comparison == SqlTruthValue::kTrue ? 1 : 0);
    }
    return SetRegister(operation.output, std::move(output));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t address,
                                       const CallScalarInstruction& operation) {
    const auto iterator =
        std::ranges::lower_bound(resolved_calls_, address, {}, &ResolvedCall::address);
    if (iterator == resolved_calls_.end() || iterator->address != address) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "resolved scalar function call is missing"));
    }
    const std::span<const SqlValue> arguments =
        operation.argument_count == 0U
            ? std::span<const SqlValue>{}
            : std::span<const SqlValue>{registers_}.subspan(operation.first_argument.value(),
                                                            operation.argument_count);
    const ScalarFunctionContext context(*iterator->collation);
    auto value = iterator->function->Invoke(context, arguments);
    if (!value.has_value()) {
      return std::unexpected(std::move(value.error()));
    }
    return SetRegister(operation.output, std::move(*value));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const JumpInstruction& operation) {
    program_counter_ = operation.target.value();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const JumpIfInstruction& operation) {
    const SqlValue& input = Register(operation.input);
    bool should_jump = false;
    switch (operation.condition) {
      case JumpCondition::kIfTrue:
        should_jump = TruthValue(input) == SqlTruthValue::kTrue;
        break;
      case JumpCondition::kIfFalse:
        should_jump = TruthValue(input) == SqlTruthValue::kFalse;
        break;
      case JumpCondition::kIfNull:
        should_jump = input.type() == SqlValueType::kNull;
        break;
      case JumpCondition::kIfNotNull:
        should_jump = input.type() != SqlValueType::kNull;
        break;
    }
    if (should_jump) {
      program_counter_ = operation.target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const OpenReadCursorInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    const ReadCursorDescriptor& descriptor = program_->cursor(operation.cursor);
    runtime.ClearRecordCache();
    if (descriptor.storage == CursorStorageKind::kRowIdTable) {
      auto cursor = TableBtreeCursor::Open(*pager_, PageNumber(descriptor.root_page.value()));
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      runtime.storage = std::move(*cursor);
    } else {
      std::vector<IndexColumnOrder> columns;
      columns.reserve(descriptor.index_columns.size());
      for (const IndexColumnMetadata& column : descriptor.index_columns) {
        const bool descending = column.order == BytecodeSortOrder::kDescending;
        columns.emplace_back(
            CollationFor(column.collation),
            descending ? IndexSortDirection::kDescending : IndexSortDirection::kAscending,
            descending ? IndexNullPlacement::kLast : IndexNullPlacement::kFirst);
      }
      auto cursor =
          IndexBtreeCursor::Open(*pager_, PageNumber(descriptor.root_page.value()), columns);
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      runtime.storage = std::move(*cursor);
    }
    runtime.decoded_fields.resize(descriptor.record_field_count);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const OpenMutationCursorInstruction& operation) {
    if (writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "mutation cursor requires a transaction writer"));
    }
    RuntimeCursor& runtime = Cursor(operation.cursor);
    const ReadCursorDescriptor& descriptor = program_->cursor(operation.cursor);
    if (descriptor.storage != CursorStorageKind::kRowIdTable) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "mutation cursor requires a rowid table"));
    }
    runtime.ClearRecordCache();
    auto table = writer_->OpenTableBtree(PageNumber(descriptor.root_page.value()));
    if (!table.has_value()) {
      return std::unexpected(std::move(table.error()));
    }
    auto cursor = table->OpenMutationCursor();
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    runtime.storage = std::move(*cursor);
    runtime.decoded_fields.resize(descriptor.record_field_count);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CloseCursorInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    runtime.storage = std::monostate{};
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const OpenWriteCursorInstruction& operation) {
    if (writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "write cursor requires a transaction writer"));
    }
    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (descriptor.storage == WriteCursorStorageKind::kRowIdTable) {
      auto table = writer_->OpenTableBtree(PageNumber(descriptor.root_page.value()));
      if (!table.has_value()) {
        return std::unexpected(std::move(table.error()));
      }
      runtime.table = std::move(*table);
    } else {
      std::vector<IndexColumnOrder> columns;
      columns.reserve(descriptor.index_columns.size());
      for (const IndexColumnMetadata& column : descriptor.index_columns) {
        const bool descending = column.order == BytecodeSortOrder::kDescending;
        columns.emplace_back(
            CollationFor(column.collation),
            descending ? IndexSortDirection::kDescending : IndexSortDirection::kAscending,
            descending ? IndexNullPlacement::kLast : IndexNullPlacement::kFirst);
      }
      auto index = writer_->OpenIndexBtree(PageNumber(descriptor.root_page.value()), columns);
      if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
      }
      runtime.index = std::move(*index);
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const CloseWriteCursorInstruction& operation) {
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    runtime.table.reset();
    runtime.index.reset();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const ResolveInsertRowIdInstruction& operation) {
    const SqlValue& input = Register(operation.input);
    if (input.type() != SqlValueType::kNull) {
      const std::optional<std::int64_t> rowid = LosslessRowId(input);
      if (!rowid.has_value()) {
        return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
      }
      return SetRegister(operation.output, SqlValue::Integer(*rowid));
    }

    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    auto cursor = TableBtreeCursor::Open(*pager_, PageNumber(descriptor.root_page.value()));
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    auto has_last = cursor->Last();
    if (!has_last.has_value()) {
      return std::unexpected(std::move(has_last.error()));
    }
    if (!*has_last) {
      return SetRegister(operation.output, SqlValue::Integer(1));
    }
    auto maximum = cursor->rowid();
    if (!maximum.has_value()) {
      return std::unexpected(std::move(maximum.error()));
    }
    if (*maximum < std::numeric_limits<std::int64_t>::max()) {
      return SetRegister(operation.output, SqlValue::Integer(*maximum + 1));
    }

    constexpr std::uint64_t kRandomRowIdMask =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) >> 1U;
    for (std::size_t attempt = 0; attempt < 100U; ++attempt) {
      std::array<std::byte, sizeof(std::uint64_t)> random{};
      Status generated = writer_->RandomBytes(MutableByteView{random});
      if (!generated.has_value()) {
        return std::unexpected(std::move(generated.error()));
      }
      const auto bits = std::bit_cast<std::uint64_t>(random);
      const auto candidate = static_cast<std::int64_t>((bits & kRandomRowIdMask) + 1U);
      auto found = cursor->Seek(candidate, BtreeSeekMode::kEqual);
      if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
      }
      if (!*found) {
        return SetRegister(operation.output, SqlValue::Integer(candidate));
      }
    }
    return std::unexpected(VmError(ErrorCode::kFull, "database or disk is full"));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const BuildTableRecordInstruction& operation) {
    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    std::vector<SqlValue> values;
    values.reserve(descriptor.columns.size());
    for (std::size_t index = 0; index < descriptor.columns.size(); ++index) {
      const WriteColumnDescriptor& column = descriptor.columns[index];
      SqlValue value = column.rowid_alias
                           ? SqlValue{}
                           : ApplyAffinity(Register(RegisterId(operation.first_value.value() +
                                                               static_cast<std::uint32_t>(index)))
                                               .Clone(),
                                           StorageAffinity(column.affinity));
      if (!column.rowid_alias && column.not_null && value.type() == SqlValueType::kNull) {
        return std::unexpected(VmError(ErrorCode::kConstraint, "NOT NULL constraint failed"));
      }
      values.push_back(std::move(value));
    }
    auto encoded = EncodeRecord(values, record_options_);
    if (!encoded.has_value()) {
      return std::unexpected(std::move(encoded.error()));
    }
    return SetRegister(operation.output, SqlValue::Blob(std::move(*encoded)));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const CheckInsertRowIdInstruction& operation) {
    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    const std::optional<std::int64_t> rowid = Register(operation.rowid).integer_value();
    if (!rowid.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "INSERT rowid is not an integer"));
    }
    auto cursor = TableBtreeCursor::Open(*pager_, PageNumber(descriptor.root_page.value()));
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    auto found = cursor->Seek(*rowid, BtreeSeekMode::kEqual);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (*found) {
      return std::unexpected(VmError(ErrorCode::kConstraint, "UNIQUE constraint failed"));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const CheckUniqueIndexInstruction& operation) {
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (!runtime.index.has_value()) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "unique check used a closed index write cursor"));
    }
    const std::span<const SqlValue> key = std::span<const SqlValue>{registers_}.subspan(
        operation.first_key.value(), operation.key_count);
    if (std::ranges::any_of(
            key, [](const SqlValue& value) { return value.type() == SqlValueType::kNull; })) {
      return std::nullopt;
    }
    auto matched = runtime.index->FindPrefixRowId(key);
    if (!matched.has_value()) {
      return std::unexpected(std::move(matched.error()));
    }
    if (!matched->has_value()) {
      return std::nullopt;
    }
    if (operation.ignored_rowid.has_value()) {
      const std::optional<std::int64_t> ignored =
          Register(*operation.ignored_rowid).integer_value();
      if (!ignored.has_value()) {
        return std::unexpected(
            VmError(ErrorCode::kInternal, "unique check ignored rowid is not an integer"));
      }
      if (**matched == *ignored) {
        return std::nullopt;
      }
    }
    return std::unexpected(VmError(ErrorCode::kConstraint, "UNIQUE constraint failed"));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const InsertIndexInstruction& operation) {
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (!runtime.index.has_value()) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "insert used a closed index write cursor"));
    }
    const std::span<const SqlValue> values = std::span<const SqlValue>{registers_}.subspan(
        operation.first_value.value(), operation.value_count);
    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    if (descriptor.unique) {
      const std::span<const SqlValue> key = values.first(descriptor.key_term_count);
      if (!std::ranges::any_of(
              key, [](const SqlValue& value) { return value.type() == SqlValueType::kNull; })) {
        auto matched = runtime.index->FindPrefixRowId(key);
        if (!matched.has_value()) {
          return std::unexpected(std::move(matched.error()));
        }
        if (matched->has_value()) {
          return std::unexpected(VmError(ErrorCode::kConstraint, "UNIQUE constraint failed"));
        }
      }
    }
    Status inserted = runtime.index->Insert(values);
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const DeleteIndexInstruction& operation) {
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (!runtime.index.has_value()) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "delete used a closed index write cursor"));
    }
    const std::span<const SqlValue> values = std::span<const SqlValue>{registers_}.subspan(
        operation.first_value.value(), operation.value_count);
    auto deleted = runtime.index->Delete(values);
    if (!deleted.has_value()) {
      return std::unexpected(std::move(deleted.error()));
    }
    if (!*deleted) {
      return std::unexpected(VmError(ErrorCode::kCorruption, "old index key does not exist"));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const InsertTableInstruction& operation) {
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (!runtime.table.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "insert used a closed write cursor"));
    }
    const std::optional<std::int64_t> rowid = LosslessRowId(Register(operation.rowid));
    const std::optional<ByteView> record = Register(operation.record).blob_value();
    if (!rowid.has_value() || !record.has_value()) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
    }
    Status inserted = runtime.table->Insert(*rowid, *record, BtreeInsertMode::kInsertOnly);
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    if (program_->mutation_result().publishes_changes) {
      if (change_count_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(VmError(ErrorCode::kTooLarge, "VM change count is exhausted"));
      }
      ++change_count_;
    }
    if (program_->mutation_result().publishes_last_insert_rowid) {
      last_insert_rowid_event_ = *rowid;
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const DeleteTableInstruction& operation) {
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (!runtime.table.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "delete used a closed write cursor"));
    }
    const std::optional<std::int64_t> rowid = LosslessRowId(Register(operation.rowid));
    if (!rowid.has_value()) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
    }
    auto deleted = runtime.table->Delete(*rowid);
    if (!deleted.has_value()) {
      return std::unexpected(std::move(deleted.error()));
    }
    if (!*deleted) {
      return std::nullopt;
    }
    if (change_count_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "VM change count is exhausted"));
    }
    ++change_count_;
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const DeleteCurrentTableInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    TableBtreeMutationCursor* cursor = std::get_if<TableBtreeMutationCursor>(&runtime.storage);
    if (cursor == nullptr || !cursor->valid()) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "current delete used an invalid mutation cursor"));
    }
    auto has_next = cursor->DeleteAndNext();
    if (!has_next.has_value()) {
      return std::unexpected(std::move(has_next.error()));
    }
    if (change_count_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "VM change count is exhausted"));
    }
    ++change_count_;
    if (!*has_next) {
      program_counter_ = operation.exhausted_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const UpdateCurrentTableInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    TableBtreeMutationCursor* cursor = std::get_if<TableBtreeMutationCursor>(&runtime.storage);
    if (cursor == nullptr || !cursor->valid()) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "current update used an invalid mutation cursor"));
    }
    const std::optional<ByteView> record = Register(operation.record).blob_value();
    if (!record.has_value()) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
    }
    Status updated = cursor->ReplaceCurrent(*record);
    if (!updated.has_value()) {
      return std::unexpected(std::move(updated.error()));
    }
    if (change_count_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "VM change count is exhausted"));
    }
    ++change_count_;
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const UpdateTableInstruction& operation) {
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (!runtime.table.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "update used a closed write cursor"));
    }
    const std::optional<std::int64_t> old_rowid = LosslessRowId(Register(operation.old_rowid));
    const std::optional<std::int64_t> new_rowid = LosslessRowId(Register(operation.new_rowid));
    const std::optional<ByteView> record = Register(operation.record).blob_value();
    if (!old_rowid.has_value() || !new_rowid.has_value() || !record.has_value()) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
    }

    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    {
      auto cursor = TableBtreeCursor::Open(*pager_, PageNumber(descriptor.root_page.value()));
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      auto old_exists = cursor->Seek(*old_rowid, BtreeSeekMode::kEqual);
      if (!old_exists.has_value()) {
        return std::unexpected(std::move(old_exists.error()));
      }
      if (!*old_exists) {
        return std::nullopt;
      }
      if (*new_rowid != *old_rowid) {
        auto new_exists = cursor->Seek(*new_rowid, BtreeSeekMode::kEqual);
        if (!new_exists.has_value()) {
          return std::unexpected(std::move(new_exists.error()));
        }
        if (*new_exists) {
          return std::unexpected(VmError(ErrorCode::kConstraint, "UNIQUE constraint failed"));
        }
      }
    }

    if (*new_rowid == *old_rowid) {
      Status replaced = runtime.table->Insert(*new_rowid, *record, BtreeInsertMode::kReplace);
      if (!replaced.has_value()) {
        return std::unexpected(std::move(replaced.error()));
      }
    } else {
      auto deleted = runtime.table->Delete(*old_rowid);
      if (!deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
      }
      if (!*deleted) {
        return std::nullopt;
      }
      Status inserted = runtime.table->Insert(*new_rowid, *record, BtreeInsertMode::kInsertOnly);
      if (!inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
    }
    if (change_count_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "VM change count is exhausted"));
    }
    ++change_count_;
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const EnsureDatabaseInitializedInstruction&) {
    if (writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "database initialization requires a transaction writer"));
    }
    if (pager_->page_count() != 0U) {
      return std::nullopt;
    }
    Status initialized = writer_->InitializeDatabase();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    record_options_.schema_format = DatabaseSchemaFormat::kFour;
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CreateTableRootInstruction& operation) {
    if (writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "table root creation requires a transaction writer"));
    }
    auto table = writer_->CreateTableBtree();
    if (!table.has_value()) {
      return std::unexpected(std::move(table.error()));
    }
    return SetRegister(operation.output,
                       SqlValue::Integer(static_cast<std::int64_t>(table->root_page().value())));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const IncrementSchemaCookieInstruction& operation) {
    if (writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "schema cookie increment requires a transaction writer"));
    }
    auto cookie = writer_->IncrementSchemaCookie();
    if (!cookie.has_value()) {
      return std::unexpected(std::move(cookie.error()));
    }
    return SetRegister(operation.output, SqlValue::Integer(static_cast<std::int64_t>(*cookie)));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const RewindInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    auto has_entry = std::visit(
        [](auto& cursor) -> Result<bool> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            return std::unexpected(VmError(ErrorCode::kInternal, "rewind used a closed cursor"));
          } else {
            return cursor.First();
          }
        },
        runtime.storage);
    if (!has_entry.has_value()) {
      return std::unexpected(std::move(has_entry.error()));
    }
    if (!*has_entry) {
      program_counter_ = operation.empty_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const NextInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    auto has_entry = std::visit(
        [](auto& cursor) -> Result<bool> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            return std::unexpected(VmError(ErrorCode::kInternal, "next used a closed cursor"));
          } else {
            return cursor.Next();
          }
        },
        runtime.storage);
    if (!has_entry.has_value()) {
      return std::unexpected(std::move(has_entry.error()));
    }
    if (*has_entry) {
      program_counter_ = operation.next_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ClearRowIdListInstruction&) {
    rowid_list_.clear();
    rowid_list_index_ = 0;
    rowid_list_positioned_ = false;
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const AppendRowIdListInstruction& operation) {
    if (rowid_list_positioned_) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "cannot append while iterating the rowid list"));
    }
    const std::optional<std::int64_t> rowid = LosslessRowId(Register(operation.input));
    if (!rowid.has_value()) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
    }
    if (rowid_list_.size() >= limits_.maximum_value_bytes / sizeof(std::int64_t)) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "rowid list exceeds the VM value limit"));
    }
    rowid_list_.push_back(*rowid);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const RewindRowIdListInstruction& operation) {
    if (rowid_list_positioned_) {
      return std::unexpected(VmError(ErrorCode::kInternal, "rowid list is already positioned"));
    }
    if (rowid_list_.empty()) {
      program_counter_ = operation.empty_target.value();
      return std::nullopt;
    }
    rowid_list_index_ = 0;
    rowid_list_positioned_ = true;
    return SetRegister(operation.output, SqlValue::Integer(rowid_list_.front()));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const NextRowIdListInstruction& operation) {
    if (!rowid_list_positioned_ || rowid_list_index_ >= rowid_list_.size()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "rowid list is not positioned"));
    }
    ++rowid_list_index_;
    if (rowid_list_index_ >= rowid_list_.size()) {
      rowid_list_positioned_ = false;
      return std::nullopt;
    }
    if (auto stored =
            SetRegister(operation.output, SqlValue::Integer(rowid_list_[rowid_list_index_]));
        !stored.has_value()) {
      return stored;
    }
    program_counter_ = operation.next_target.value();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const SeekRowIdInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    TableBtreeCursor* cursor = std::get_if<TableBtreeCursor>(&runtime.storage);
    if (cursor == nullptr) {
      return std::unexpected(VmError(ErrorCode::kInternal, "rowid seek used a non-table cursor"));
    }
    const std::optional<std::int64_t> rowid = LosslessRowId(Register(operation.key));
    if (!rowid.has_value()) {
      program_counter_ = operation.missing_target.value();
      return std::nullopt;
    }
    BtreeSeekMode mode = BtreeSeekMode::kEqual;
    switch (operation.mode) {
      case RowIdSeekMode::kEqual:
        break;
      case RowIdSeekMode::kGreater:
        mode = BtreeSeekMode::kGreater;
        break;
      default:
        return std::unexpected(VmError(ErrorCode::kInternal, "unknown rowid seek mode"));
    }
    auto found = cursor->Seek(*rowid, mode);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (!*found) {
      program_counter_ = operation.missing_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const SeekTableRowIdInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    TableBtreeCursor* cursor = std::get_if<TableBtreeCursor>(&runtime.storage);
    if (cursor == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "exact table rowid seek used a non-table cursor"));
    }
    const std::optional<std::int64_t> rowid = Register(operation.key).integer_value();
    if (!rowid.has_value()) {
      return std::unexpected(
          VmError(ErrorCode::kCorruption, "index record contains a non-integer table rowid"));
    }
    auto found = cursor->Seek(*rowid, BtreeSeekMode::kEqual);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (!*found) {
      return std::unexpected(
          VmError(ErrorCode::kCorruption, "index rowid does not resolve to a table record"));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const SeekIndexInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    IndexBtreeCursor* cursor = std::get_if<IndexBtreeCursor>(&runtime.storage);
    if (cursor == nullptr) {
      return std::unexpected(VmError(ErrorCode::kInternal, "index seek used a non-index cursor"));
    }
    BtreeSeekMode mode = BtreeSeekMode::kEqual;
    switch (operation.mode) {
      case IndexSeekMode::kEqual:
        break;
      case IndexSeekMode::kGreaterOrEqual:
        mode = BtreeSeekMode::kGreaterOrEqual;
        break;
      case IndexSeekMode::kGreater:
        mode = BtreeSeekMode::kGreater;
        break;
      case IndexSeekMode::kLessOrEqual:
        mode = BtreeSeekMode::kLessOrEqual;
        break;
      case IndexSeekMode::kLess:
        mode = BtreeSeekMode::kLess;
        break;
      default:
        return std::unexpected(VmError(ErrorCode::kInternal, "unknown index seek mode"));
    }
    const std::span<const SqlValue> key = std::span<const SqlValue>{registers_}.subspan(
        operation.first_key.value(), operation.key_count);
    auto found = cursor->Seek(key, mode);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (!*found) {
      program_counter_ = operation.missing_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CheckIndexRangeInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    IndexBtreeCursor* cursor = std::get_if<IndexBtreeCursor>(&runtime.storage);
    if (cursor == nullptr || !cursor->valid()) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "index range check used an invalid index cursor"));
    }
    const std::span<const SqlValue> key = std::span<const SqlValue>{registers_}.subspan(
        operation.first_key.value(), operation.key_count);
    auto comparison = cursor->CompareCurrent(key);
    if (!comparison.has_value()) {
      return std::unexpected(std::move(comparison.error()));
    }
    bool ended = false;
    switch (operation.mode) {
      case IndexRangeEndMode::kInclusive:
        ended = *comparison == std::weak_ordering::greater;
        break;
      case IndexRangeEndMode::kExclusive:
        ended = *comparison != std::weak_ordering::less;
        break;
      default:
        return std::unexpected(VmError(ErrorCode::kInternal, "unknown index range end mode"));
    }
    if (ended) {
      program_counter_ = operation.end_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ReadFieldInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    const ReadCursorDescriptor& descriptor = program_->cursor(operation.cursor);
    const CursorFieldSource& source = descriptor.fields[operation.field.value()];
    SqlValue value;
    if (source.kind == CursorFieldSourceKind::kRowId) {
      if (const auto* read_cursor = std::get_if<TableBtreeCursor>(&runtime.storage);
          read_cursor != nullptr && read_cursor->valid()) {
        auto rowid = read_cursor->rowid();
        if (!rowid.has_value()) {
          return std::unexpected(std::move(rowid.error()));
        }
        value = SqlValue::Integer(*rowid);
      } else if (auto* mutation_cursor = std::get_if<TableBtreeMutationCursor>(&runtime.storage);
                 mutation_cursor != nullptr && mutation_cursor->valid()) {
        auto row = mutation_cursor->row();
        if (!row.has_value()) {
          return std::unexpected(std::move(row.error()));
        }
        value = SqlValue::Integer(row->rowid);
      } else {
        return std::unexpected(
            VmError(ErrorCode::kInternal, "rowid field used an invalid table cursor"));
      }
    } else {
      auto field = ReadRecordField(runtime, source.record_field);
      if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
      }
      const bool physically_missing =
          runtime.record.has_value() && source.record_field >= runtime.record->field_count();
      if (!physically_missing) {
        value = std::move(*field);
      } else {
        switch (source.missing_value_kind) {
          case MissingFieldValueKind::kNull:
            break;
          case MissingFieldValueKind::kConstant:
            if (!source.missing_value.has_value()) {
              return std::unexpected(VmError(ErrorCode::kInternal,
                                             "missing-field constant descriptor has no constant"));
            }
            value = program_->constant(*source.missing_value).Clone();
            break;
          case MissingFieldValueKind::kUnsupported:
            return std::unexpected(VmError(
                ErrorCode::kGeneric, "unsupported default for physically missing record field"));
        }
      }
    }
    return SetRegister(operation.output, std::move(value));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ReadRowIdInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    if (const auto* read_cursor = std::get_if<TableBtreeCursor>(&runtime.storage);
        read_cursor != nullptr && read_cursor->valid()) {
      auto rowid = read_cursor->rowid();
      if (!rowid.has_value()) {
        return std::unexpected(std::move(rowid.error()));
      }
      return SetRegister(operation.output, SqlValue::Integer(*rowid));
    }
    if (auto* mutation_cursor = std::get_if<TableBtreeMutationCursor>(&runtime.storage);
        mutation_cursor != nullptr && mutation_cursor->valid()) {
      auto row = mutation_cursor->row();
      if (!row.has_value()) {
        return std::unexpected(std::move(row.error()));
      }
      return SetRegister(operation.output, SqlValue::Integer(row->rowid));
    }
    return std::unexpected(
        VmError(ErrorCode::kInternal, "rowid read used an invalid table cursor"));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ResultRowInstruction& operation) {
    row_first_ = operation.first.value();
    row_count_ = operation.count;
    state_ = VmState::kRow;
    return VmStep::kRow;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const HaltInstruction&) {
    CloseAllCursors();
    ClearRow();
    execution_data_version_.reset();
    state_ = VmState::kDone;
    return VmStep::kDone;
  }

  [[nodiscard]] DispatchResult SetRegister(RegisterId destination, SqlValue value) {
    if (ValueByteSize(value) > limits_.maximum_value_bytes) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "value exceeds the VM value limit"));
    }
    registers_[destination.value()] = std::move(value);
    return std::nullopt;
  }

  [[nodiscard]] const SqlValue& Register(RegisterId register_id) const noexcept {
    return registers_[register_id.value()];
  }

  [[nodiscard]] RuntimeCursor& Cursor(CursorId cursor_id) noexcept {
    return cursors_[cursor_id.value()];
  }

  [[nodiscard]] RuntimeWriteCursor& WriteCursor(WriteCursorId cursor_id) noexcept {
    return write_cursors_[cursor_id.value()];
  }

  [[nodiscard]] Result<SqlValue> ReadRecordField(RuntimeCursor& runtime,
                                                 std::uint32_t field_index) {
    if (!runtime.fields_decoded) {
      auto record = CompleteRecord(runtime);
      if (!record.has_value()) {
        return std::unexpected(std::move(record.error()));
      }
      runtime.record.emplace(*record);
      RecordCursor cursor = runtime.record->cursor();
      const std::size_t available = runtime.record->field_count();
      const std::size_t requested = runtime.decoded_fields.size();
      const std::size_t decode_count = std::min(available, requested);
      for (std::size_t index = 0; index < decode_count; ++index) {
        const std::optional<RecordFieldView> field = cursor.Next();
        if (!field.has_value()) {
          return std::unexpected(
              VmError(ErrorCode::kCorruption, "record field decoding ended unexpectedly"));
        }
        runtime.decoded_fields[index] = *field;
      }
      runtime.fields_decoded = true;
    }
    const RecordFieldView& field = runtime.decoded_fields[field_index];
    if (const auto text = field.text_value();
        text.has_value() && text->bytes().size() > limits_.maximum_value_bytes) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "record text field exceeds the VM value limit"));
    }
    if (const auto blob = field.blob_value();
        blob.has_value() && blob->size() > limits_.maximum_value_bytes) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "record blob field exceeds the VM value limit"));
    }
    return field.ToOwned();
  }

  [[nodiscard]] Result<RecordView> CompleteRecord(RuntimeCursor& runtime) {
    if (auto* cursor = std::get_if<TableBtreeMutationCursor>(&runtime.storage); cursor != nullptr) {
      if (!cursor->valid()) {
        return std::unexpected(
            VmError(ErrorCode::kInternal, "record read used an invalid mutation cursor"));
      }
      auto row = cursor->row();
      if (!row.has_value()) {
        return std::unexpected(std::move(row.error()));
      }
      if (row->payload.size() > limits_.maximum_value_bytes) {
        return std::unexpected(
            VmError(ErrorCode::kTooLarge, "mutation record exceeds the VM value limit"));
      }
      return RecordView::Parse(row->payload, record_options_);
    }
    auto payload = std::visit(
        [](auto& cursor) -> Result<BtreePayloadView> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            return std::unexpected(
                VmError(ErrorCode::kInternal, "record read used a closed cursor"));
          } else if constexpr (std::is_same_v<T, TableBtreeMutationCursor>) {
            return std::unexpected(
                VmError(ErrorCode::kInternal, "mutation record bypassed its dedicated reader"));
          } else {
            return cursor.payload();
          }
        },
        runtime.storage);
    if (!payload.has_value()) {
      return std::unexpected(std::move(payload.error()));
    }

    ByteView encoded = payload->local_bytes();
    if (!payload->is_fully_local()) {
      if (payload->size().value() > limits_.maximum_value_bytes) {
        return std::unexpected(
            VmError(ErrorCode::kTooLarge, "overflow record exceeds the VM value limit"));
      }
      auto copied = std::visit(
          [](auto& cursor) -> Result<ByteBuffer> {
            using T = std::remove_cvref_t<decltype(cursor)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
              return std::unexpected(
                  VmError(ErrorCode::kInternal, "record copy used a closed cursor"));
            } else if constexpr (std::is_same_v<T, TableBtreeMutationCursor>) {
              return std::unexpected(
                  VmError(ErrorCode::kInternal, "mutation record bypassed its dedicated copier"));
            } else {
              return cursor.CopyPayload();
            }
          },
          runtime.storage);
      if (!copied.has_value()) {
        return std::unexpected(std::move(copied.error()));
      }
      runtime.owned_record.emplace(std::move(*copied));
      encoded = runtime.owned_record->view();
    }
    return RecordView::Parse(encoded, record_options_);
  }

  void ClearRow() noexcept {
    row_first_ = 0;
    row_count_ = 0;
  }

  void CloseAllCursors() noexcept {
    for (RuntimeCursor& cursor : cursors_) {
      cursor.ClearRecordCache();
      cursor.storage = std::monostate{};
    }
    for (RuntimeWriteCursor& cursor : write_cursors_) {
      cursor.table.reset();
      cursor.index.reset();
    }
    rowid_list_index_ = 0;
    rowid_list_positioned_ = false;
  }

  const BytecodeProgram* program_;
  Pager* pager_ = nullptr;
  std::uint64_t catalog_generation_ = 0;
  TransactionWriter* writer_ = nullptr;
  const FunctionRegistry* functions_;
  std::span<const Collation* const> available_collations_;
  VmLimits limits_;
  std::vector<SqlValue> registers_;
  std::vector<SqlValue> parameters_;
  std::vector<RuntimeCursor> cursors_;
  std::vector<RuntimeWriteCursor> write_cursors_;
  std::vector<const Collation*> resolved_collations_;
  std::vector<ResolvedCall> resolved_calls_;
  RecordCodecOptions record_options_;
  VmState state_ = VmState::kReady;
  std::uint32_t program_counter_ = 0;
  std::uint32_t row_first_ = 0;
  std::uint32_t row_count_ = 0;
  std::uint64_t executed_instruction_count_ = 0;
  std::uint64_t change_count_ = 0;
  std::optional<std::int64_t> last_insert_rowid_event_;
  std::optional<std::uint64_t> execution_data_version_;
  std::vector<std::int64_t> rowid_list_;
  std::size_t rowid_list_index_ = 0;
  bool rowid_list_positioned_ = false;
};

VmExecutionContext::VmExecutionContext(TransactionWriter& writer,
                                       std::uint64_t catalog_generation) noexcept
    : pager_(&writer.pager()), catalog_generation_(catalog_generation), writer_(&writer) {}

VmEnvironment VmEnvironment::Core() noexcept {
  static const std::array<const Collation*, 3> collations{
      &BinaryCollation(),
      &NoCaseCollation(),
      &RTrimCollation(),
  };
  return {CoreFunctionRegistry(), collations};
}

Result<Vm> Vm::Create(const BytecodeProgram& program, VmEnvironment environment, VmLimits limits) {
  try {
    auto impl = std::make_unique<Impl>(program, environment, limits);
    Status initialized = impl->Initialize();
    if (!initialized.has_value()) {
      return std::unexpected(std::move(initialized.error()));
    }
    return Vm(std::move(impl));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Vm::Vm(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Vm::Vm(Vm&&) noexcept = default;

Vm& Vm::operator=(Vm&&) noexcept = default;

Vm::~Vm() = default;

Status Vm::Bind(ParameterId parameter, const SqlValue& value) {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "cannot bind a moved-from VM"));
    }
    return impl_->Bind(parameter, value);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status Vm::ClearBindings() {
  if (impl_ == nullptr) {
    return std::unexpected(VmError(ErrorCode::kMisuse, "cannot clear bindings on a moved-from VM"));
  }
  return impl_->ClearBindings();
}

Status Vm::AttachExecutionContext(VmExecutionContext context) {
  if (impl_ == nullptr) {
    return std::unexpected(
        VmError(ErrorCode::kMisuse, "cannot attach execution context to a moved-from VM"));
  }
  return impl_->AttachExecutionContext(context);
}

Status Vm::DetachExecutionContext() {
  if (impl_ == nullptr) {
    return std::unexpected(
        VmError(ErrorCode::kMisuse, "cannot detach execution context from a moved-from VM"));
  }
  return impl_->DetachExecutionContext();
}

Result<VmStep> Vm::Step() {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "cannot step a moved-from VM"));
    }
    return impl_->Step();
  } catch (const std::bad_alloc&) {
    if (impl_ != nullptr) {
      impl_->FailForException();
    }
    return std::unexpected(Error::OutOfMemory());
  } catch (...) {
    if (impl_ != nullptr) {
      impl_->FailForException();
    }
    throw;
  }
}

Status Vm::Reset() {
  if (impl_ == nullptr) {
    return std::unexpected(VmError(ErrorCode::kMisuse, "cannot reset a moved-from VM"));
  }
  return impl_->Reset();
}

VmState Vm::state() const noexcept { return impl_ == nullptr ? VmState::kInvalid : impl_->state_; }

bool Vm::has_execution_context() const noexcept {
  return impl_ != nullptr && impl_->pager_ != nullptr;
}

std::span<const SqlValue> Vm::row() const noexcept {
  return impl_ == nullptr ? std::span<const SqlValue>{} : impl_->row();
}

std::span<const SqlValue> Vm::bindings() const noexcept {
  return impl_ == nullptr ? std::span<const SqlValue>{} : impl_->bindings();
}

std::uint64_t Vm::executed_instruction_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->executed_instruction_count_;
}

std::uint64_t Vm::change_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->change_count_;
}

std::optional<std::int64_t> Vm::last_insert_rowid_event() const noexcept {
  return impl_ == nullptr ? std::nullopt : impl_->last_insert_rowid_event_;
}

}  // namespace modern_sqlite
