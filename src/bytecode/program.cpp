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
  IndexAppend = 3,
};

enum class CapabilityState : std::uint8_t {
  kClosed = 0,
  kWriting = 1,
  kCandidatePending = 2,
  kPositioned = 3,
  kExhausted = 4,
  kLookupReady = 5,
  kLookupMaybeReady = 6,
};

enum class AggregateCapabilityState : std::uint8_t {
  kClosed = 0,
  kAccumulating = 1,
  kFinalized = 2,
};

[[nodiscard]] bool IsLookupReadinessState(CapabilityState state) noexcept {
  return state == CapabilityState::kWriting || state == CapabilityState::kLookupReady ||
         state == CapabilityState::kLookupMaybeReady;
}

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

[[nodiscard]] bool IsValid(TableRecordAffinityMode mode) noexcept {
  switch (mode) {
    case TableRecordAffinityMode::kApplyColumnAffinity:
    case TableRecordAffinityMode::kAffinityPreapplied:
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
    case CursorStorageKind::kEphemeralIndex:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(WriteCursorStorageKind kind) noexcept {
  switch (kind) {
    case WriteCursorStorageKind::kRowIdTable:
    case WriteCursorStorageKind::kIndex:
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
    case ProgramStatementKind::kCreateIndex:
    case ProgramStatementKind::kAnalyze:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(Stat1ClearScope scope) noexcept {
  switch (scope) {
    case Stat1ClearScope::kDatabase:
    case Stat1ClearScope::kTable:
    case Stat1ClearScope::kIndex:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(Stat1IndexPolicy policy) noexcept {
  switch (policy) {
    case Stat1IndexPolicy::Ordinary:
    case Stat1IndexPolicy::UniqueNullable:
    case Stat1IndexPolicy::UniqueNotNull:
    case Stat1IndexPolicy::PrimaryWithoutRowid:
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

[[nodiscard]] bool IsValid(BytecodeNullPlacement placement) noexcept {
  switch (placement) {
    case BytecodeNullPlacement::kFirst:
    case BytecodeNullPlacement::kLast:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(RelationInsertMode mode) noexcept {
  switch (mode) {
    case RelationInsertMode::kKeepExisting:
    case RelationInsertMode::kReplaceExisting:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(RowIdSeekMode mode) noexcept {
  switch (mode) {
    case RowIdSeekMode::kEqual:
    case RowIdSeekMode::kGreaterOrEqual:
    case RowIdSeekMode::kGreater:
    case RowIdSeekMode::kLessOrEqual:
    case RowIdSeekMode::kLess:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(IndexSeekMode mode) noexcept {
  switch (mode) {
    case IndexSeekMode::kEqual:
    case IndexSeekMode::kGreaterOrEqual:
    case IndexSeekMode::kGreater:
    case IndexSeekMode::kLessOrEqual:
    case IndexSeekMode::kLess:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(IndexRangeEndMode mode) noexcept {
  switch (mode) {
    case IndexRangeEndMode::kInclusive:
    case IndexRangeEndMode::kExclusive:
    case IndexRangeEndMode::kReverseInclusive:
    case IndexRangeEndMode::kReverseExclusive:
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
    case JumpCondition::kIfTrueOrNull:
    case JumpCondition::kIfFalseOrNull:
    case JumpCondition::kIfNull:
    case JumpCondition::kIfNotNull:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(SubroutineKind kind) noexcept {
  switch (kind) {
    case SubroutineKind::kValue:
    case SubroutineKind::kRead:
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
        !AddArrayBytes<IndexColumnMetadata>(ElementCount(cursor.index_columns, measure), &total) ||
        total > limits.maximum_owned_bytes) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<OrderingRecordDescriptor>(ElementCount(input.sorters, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& sorter : input.sorters) {
    if (!AddArrayBytes<OrderingColumnMetadata>(ElementCount(sorter.key_columns, measure), &total) ||
        total > limits.maximum_owned_bytes) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<OrderingRecordDescriptor>(ElementCount(input.top_ns, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& top_n : input.top_ns) {
    if (!AddArrayBytes<OrderingColumnMetadata>(ElementCount(top_n.key_columns, measure), &total) ||
        total > limits.maximum_owned_bytes) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<OrderingRecordDescriptor>(ElementCount(input.relations, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& relation : input.relations) {
    if (!AddArrayBytes<OrderingColumnMetadata>(ElementCount(relation.key_columns, measure),
                                               &total) ||
        total > limits.maximum_owned_bytes) {
      return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
    }
  }

  if (!AddArrayBytes<OrderingRecordDescriptor>(ElementCount(input.record_comparisons, measure),
                                               &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  for (const auto& comparison : input.record_comparisons) {
    if (!AddArrayBytes<OrderingColumnMetadata>(ElementCount(comparison.key_columns, measure),
                                               &total) ||
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
  if (!AddArrayBytes<AggregateDescriptor>(ElementCount(input.aggregates, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (!AddArrayBytes<SubroutineDescriptor>(ElementCount(input.subroutines, measure), &total) ||
      total > limits.maximum_owned_bytes) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (!AddArrayBytes<CoroutineDescriptor>(ElementCount(input.coroutines, measure), &total) ||
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
  clone.write_effects = input.write_effects;
  clone.mutation_result = input.mutation_result;
  clone.register_count = input.register_count;
  clone.parameter_count = input.parameter_count;
  clone.requires_database_snapshot =
      input.requires_database_snapshot ||
      input.transaction_access == ProgramTransactionAccess::kWrite ||
      std::ranges::any_of(input.cursors,
                          [](const ReadCursorDescriptor& cursor) {
                            return cursor.storage != CursorStorageKind::kEphemeralIndex;
                          }) ||
      !input.write_cursors.empty();

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
        .relation = cursor.relation,
        .index_key_field_count = cursor.index_key_field_count,
    });
  }

  clone.write_cursors.reserve(input.write_cursors.size());
  for (const auto& cursor : input.write_cursors) {
    clone.write_cursors.push_back(WriteCursorDescriptor{
        .root_page = cursor.root_page,
        .columns = std::vector<WriteColumnDescriptor>(cursor.columns.begin(), cursor.columns.end()),
        .rowid_alias = cursor.rowid_alias,
        .index_columns = std::vector<IndexColumnMetadata>(cursor.index_columns.begin(),
                                                          cursor.index_columns.end()),
        .key_term_count = cursor.key_term_count,
        .unique = cursor.unique,
        .unique_not_null = cursor.unique_not_null,
        .pending_root = cursor.pending_root,
        .storage = cursor.storage,
    });
  }

  clone.sorters.reserve(input.sorters.size());
  for (const auto& sorter : input.sorters) {
    clone.sorters.push_back(OrderingRecordDescriptor{
        .field_count = sorter.field_count,
        .key_field_count = sorter.key_field_count,
        .key_columns = std::vector<OrderingColumnMetadata>(sorter.key_columns.begin(),
                                                           sorter.key_columns.end()),
        .relation_purpose = sorter.relation_purpose,
        .index_refill_cursor = sorter.index_refill_cursor,
    });
  }

  clone.top_ns.reserve(input.top_ns.size());
  for (const auto& top_n : input.top_ns) {
    clone.top_ns.push_back(OrderingRecordDescriptor{
        .field_count = top_n.field_count,
        .key_field_count = top_n.key_field_count,
        .key_columns =
            std::vector<OrderingColumnMetadata>(top_n.key_columns.begin(), top_n.key_columns.end()),
        .relation_purpose = top_n.relation_purpose,
        .index_refill_cursor = top_n.index_refill_cursor,
    });
  }

  clone.relations.reserve(input.relations.size());
  for (const auto& relation : input.relations) {
    clone.relations.push_back(OrderingRecordDescriptor{
        .field_count = relation.field_count,
        .key_field_count = relation.key_field_count,
        .key_columns = std::vector<OrderingColumnMetadata>(relation.key_columns.begin(),
                                                           relation.key_columns.end()),
        .relation_purpose = relation.relation_purpose,
        .index_refill_cursor = relation.index_refill_cursor,
    });
  }

  clone.record_comparisons.reserve(input.record_comparisons.size());
  for (const auto& comparison : input.record_comparisons) {
    clone.record_comparisons.push_back(OrderingRecordDescriptor{
        .field_count = comparison.field_count,
        .key_field_count = comparison.key_field_count,
        .key_columns = std::vector<OrderingColumnMetadata>(comparison.key_columns.begin(),
                                                           comparison.key_columns.end()),
        .relation_purpose = comparison.relation_purpose,
        .index_refill_cursor = comparison.index_refill_cursor,
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
  clone.aggregates.assign(input.aggregates.begin(), input.aggregates.end());
  clone.subroutines.assign(input.subroutines.begin(), input.subroutines.end());
  clone.coroutines.assign(input.coroutines.begin(), input.coroutines.end());
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
  if (input.sorters.size() > limits.maximum_sorters ||
      input.sorters.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kSorterLimitExceeded));
  }
  if (input.top_ns.size() > limits.maximum_top_ns ||
      input.top_ns.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kTopNLimitExceeded));
  }
  if (input.relations.size() > limits.maximum_relations ||
      input.relations.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kRelationLimitExceeded));
  }
  if (input.record_comparisons.size() > limits.maximum_record_comparisons ||
      input.record_comparisons.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kRecordComparisonLimitExceeded));
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
  if (input.aggregates.size() > limits.maximum_aggregates ||
      input.aggregates.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAggregateLimitExceeded));
  }
  if (input.subroutines.size() > input.instructions.size()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidSubroutineDescriptor));
  }
  if (input.coroutines.size() > input.instructions.size() - input.subroutines.size()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::InvalidCoroutineDescriptor));
  }
  return {};
}

[[nodiscard]] ProgramResult<void> CheckExecutionMetadata(const ProgramInput& input) {
  if (!IsValid(input.statement_kind) || !IsValid(input.transaction_access) ||
      !IsValid(input.rollback_mode)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidEnumValue));
  }
  const bool is_select = input.statement_kind == ProgramStatementKind::kSelect;
  const bool is_insert = input.statement_kind == ProgramStatementKind::kInsert;
  const bool is_update = input.statement_kind == ProgramStatementKind::kUpdate;
  const bool is_delete = input.statement_kind == ProgramStatementKind::kDelete;
  const bool is_create_table = input.statement_kind == ProgramStatementKind::kCreateTable;
  const bool is_create_index = input.statement_kind == ProgramStatementKind::kCreateIndex;
  const bool is_analyze = input.statement_kind == ProgramStatementKind::kAnalyze;
  const bool mutation_results_valid =
      (is_select && !input.mutation_result.publishes_changes &&
       !input.mutation_result.publishes_last_insert_rowid) ||
      (is_insert && input.mutation_result.publishes_changes &&
       input.mutation_result.publishes_last_insert_rowid) ||
      ((is_update || is_delete) && input.mutation_result.publishes_changes &&
       !input.mutation_result.publishes_last_insert_rowid) ||
      ((is_create_table || is_create_index || is_analyze) &&
       !input.mutation_result.publishes_changes &&
       !input.mutation_result.publishes_last_insert_rowid);
  if ((is_select && input.transaction_access != ProgramTransactionAccess::kRead) ||
      (!is_select && input.transaction_access != ProgramTransactionAccess::kWrite) ||
      (is_select && input.rollback_mode != ProgramRollbackMode::kTransaction) ||
      (is_select && (input.write_effects.multi_write || input.write_effects.may_abort)) ||
      (is_select && !input.write_cursors.empty()) ||
      (!is_select && !input.result_columns.empty()) || !mutation_results_valid) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidExecutionMetadata));
  }
  return {};
}

[[nodiscard]] ProgramResult<void> CheckDescriptors(const ProgramInput& input) {
  const auto check_ordering_descriptor = [&](const OrderingRecordDescriptor& descriptor,
                                             std::size_t descriptor_index, bool relation = false,
                                             bool sorter = false) -> ProgramResult<void> {
    if (descriptor.field_count == 0U || descriptor.key_field_count == 0U ||
        descriptor.key_field_count > descriptor.field_count ||
        descriptor.key_columns.size() != descriptor.key_field_count ||
        (descriptor.relation_purpose == RelationPurpose::kLookup &&
         descriptor.key_field_count != descriptor.field_count) ||
        (descriptor.relation_purpose != RelationPurpose::kMembership &&
         (!relation || descriptor.relation_purpose != RelationPurpose::kLookup)) ||
        (descriptor.index_refill_cursor.has_value() && !sorter)) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor,
                                     ProgramError::kNoInstruction, descriptor_index));
    }
    for (const OrderingColumnMetadata& column : descriptor.key_columns) {
      if (column.collation.value() >= input.symbols.size() || !IsValid(column.order) ||
          !IsValid(column.null_placement)) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor,
                                       ProgramError::kNoInstruction, descriptor_index));
      }
    }
    return {};
  };

  for (std::size_t cursor_index = 0; cursor_index < input.cursors.size(); ++cursor_index) {
    const auto& cursor = input.cursors[cursor_index];
    const bool ephemeral = cursor.storage == CursorStorageKind::kEphemeralIndex;
    if (ephemeral ? cursor.root_page.value() != 0U : cursor.root_page.value() == 0U) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidRootPage, ProgramError::kNoInstruction, cursor_index));
    }
    if (!IsValid(cursor.storage) || cursor.record_field_count == 0 ||
        cursor.fields.size() > std::numeric_limits<std::uint32_t>::max() ||
        cursor.index_columns.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                     ProgramError::kNoInstruction, cursor_index));
    }
    if (cursor.storage == CursorStorageKind::kIndex
            ? cursor.index_key_field_count == 0U ||
                  cursor.index_key_field_count >= std::numeric_limits<std::uint16_t>::max() ||
                  cursor.index_key_field_count > cursor.index_columns.size() ||
                  cursor.index_columns.size() > std::numeric_limits<std::uint16_t>::max()
            : cursor.index_key_field_count != 0U) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                     ProgramError::kNoInstruction, cursor_index));
    }

    if (ephemeral) {
      if (!cursor.relation.has_value() || cursor.relation->value() >= input.relations.size() ||
          !cursor.index_columns.empty()) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
      const OrderingRecordDescriptor& owner = input.relations[cursor.relation->value()];
      if (owner.relation_purpose != RelationPurpose::kLookup ||
          owner.field_count != cursor.record_field_count) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
    } else if (cursor.relation.has_value() ||
               (cursor.storage == CursorStorageKind::kRowIdTable
                    ? !cursor.index_columns.empty()
                    : cursor.index_columns.size() != cursor.record_field_count)) {
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
    const bool root_is_valid =
        cursor.pending_root ? cursor.root_page.value() == 0 : cursor.root_page.value() != 0;
    if (!root_is_valid) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidRootPage, ProgramError::kNoInstruction, cursor_index));
    }
    if (!IsValid(cursor.storage)) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                     ProgramError::kNoInstruction, cursor_index));
    }
    if (cursor.storage == WriteCursorStorageKind::kRowIdTable) {
      if (cursor.columns.empty() ||
          cursor.columns.size() > std::numeric_limits<std::uint32_t>::max() ||
          (cursor.rowid_alias.has_value() && *cursor.rowid_alias >= cursor.columns.size()) ||
          !cursor.index_columns.empty() || cursor.key_term_count != 0U || cursor.unique ||
          cursor.unique_not_null) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
      std::optional<std::uint32_t> rowid_alias;
      for (std::size_t column_index = 0; column_index < cursor.columns.size(); ++column_index) {
        const WriteColumnDescriptor& column = cursor.columns[column_index];
        if (!IsValid(column.affinity) ||
            (column.default_value.has_value() &&
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
    } else {
      if (!cursor.columns.empty() || cursor.rowid_alias.has_value() ||
          cursor.index_columns.empty() ||
          cursor.index_columns.size() > std::numeric_limits<std::uint32_t>::max() ||
          cursor.key_term_count == 0U || cursor.key_term_count >= cursor.index_columns.size() ||
          (cursor.unique_not_null && !cursor.unique)) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                       ProgramError::kNoInstruction, cursor_index));
      }
      for (const IndexColumnMetadata& column : cursor.index_columns) {
        if (column.collation.value() >= input.symbols.size() || !IsValid(column.order)) {
          return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor,
                                         ProgramError::kNoInstruction, cursor_index));
        }
      }
    }
  }
  std::vector<bool> index_refill_targets;
  for (std::size_t sorter_index = 0; sorter_index < input.sorters.size(); ++sorter_index) {
    const OrderingRecordDescriptor& sorter = input.sorters[sorter_index];
    if (auto checked = check_ordering_descriptor(sorter, sorter_index, false, true); !checked) {
      return checked;
    }
    if (!sorter.index_refill_cursor.has_value()) {
      continue;
    }
    const std::size_t target = sorter.index_refill_cursor->value();
    const auto invalid_refill = [&]() {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor,
                                     ProgramError::kNoInstruction, sorter_index));
    };
    if (input.statement_kind != ProgramStatementKind::kCreateIndex ||
        !input.write_effects.multi_write || !input.write_effects.may_abort ||
        target >= input.write_cursors.size()) {
      return invalid_refill();
    }
    const WriteCursorDescriptor& cursor = input.write_cursors[target];
    if (cursor.storage != WriteCursorStorageKind::kIndex || !cursor.pending_root ||
        sorter.field_count != cursor.index_columns.size() ||
        sorter.key_field_count != cursor.key_term_count ||
        sorter.field_count - sorter.key_field_count != 1U ||
        cursor.index_columns.back().order != BytecodeSortOrder::kAscending) {
      return invalid_refill();
    }
    for (std::size_t term = 0; term < sorter.key_columns.size(); ++term) {
      const OrderingColumnMetadata& column = sorter.key_columns[term];
      const IndexColumnMetadata& index_column = cursor.index_columns[term];
      const BytecodeNullPlacement nulls = column.order == BytecodeSortOrder::kDescending
                                              ? BytecodeNullPlacement::kLast
                                              : BytecodeNullPlacement::kFirst;
      if (column.collation != index_column.collation || column.order != index_column.order ||
          column.null_placement != nulls) {
        return invalid_refill();
      }
    }
    if (index_refill_targets.empty()) {
      index_refill_targets.resize(input.write_cursors.size(), false);
    }
    if (index_refill_targets[target]) {
      return invalid_refill();
    }
    index_refill_targets[target] = true;
  }
  for (std::size_t top_n_index = 0; top_n_index < input.top_ns.size(); ++top_n_index) {
    if (auto checked = check_ordering_descriptor(input.top_ns[top_n_index], top_n_index);
        !checked) {
      return checked;
    }
  }
  for (std::size_t relation_index = 0; relation_index < input.relations.size(); ++relation_index) {
    if (auto checked =
            check_ordering_descriptor(input.relations[relation_index], relation_index, true);
        !checked) {
      return checked;
    }
  }
  for (std::size_t comparison_index = 0; comparison_index < input.record_comparisons.size();
       ++comparison_index) {
    const OrderingRecordDescriptor& descriptor = input.record_comparisons[comparison_index];
    if (descriptor.key_field_count != descriptor.field_count) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor,
                                     ProgramError::kNoInstruction, comparison_index));
    }
    if (auto checked = check_ordering_descriptor(descriptor, comparison_index); !checked) {
      return checked;
    }
  }
  for (std::size_t aggregate_index = 0; aggregate_index < input.aggregates.size();
       ++aggregate_index) {
    const AggregateDescriptor& descriptor = input.aggregates[aggregate_index];
    if (descriptor.function.value() >= input.symbols.size() ||
        descriptor.collation.value() >= input.symbols.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidAggregateDescriptor,
                                     ProgramError::kNoInstruction, aggregate_index));
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

struct InstructionOwnership {
  std::span<const std::uint32_t> regions;
  std::span<std::optional<std::uint32_t>> cursors;
  std::span<std::optional<std::uint32_t>> write_cursors;
  std::span<std::optional<std::uint32_t>> sorters;
  std::span<std::optional<std::uint32_t>> top_ns;
  std::span<std::optional<std::uint32_t>> relations;
  std::span<std::optional<std::uint32_t>> aggregates;
};

[[nodiscard]] ProgramResult<void> CheckInstructions(
    const ProgramInput& input, std::optional<InstructionOwnership> ownership = std::nullopt) {
  const auto check_owner = [&](std::span<std::optional<std::uint32_t>> owners, auto id,
                               std::size_t instruction) -> ProgramResult<void> {
    const std::uint32_t region = ownership->regions[instruction];
    if (owners[id.value()].has_value() && *owners[id.value()] != region) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidSubroutineControl, instruction, id.value()));
    }
    owners[id.value()] = region;
    return {};
  };
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
    if (ownership.has_value()) {
      if (auto checked = check_owner(ownership->cursors, id, instruction); !checked) {
        return checked;
      }
      if (input.cursors[id.value()].relation.has_value()) {
        return check_owner(ownership->relations, *input.cursors[id.value()].relation, instruction);
      }
    }
    return {};
  };
  const auto check_write_cursor = [&](WriteCursorId id,
                                      std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.write_cursors.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursor, instruction, id.value()));
    }
    return ownership.has_value() ? check_owner(ownership->write_cursors, id, instruction)
                                 : ProgramResult<void>{};
  };
  const auto check_sorter = [&](SorterId id, std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.sorters.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidSorter, instruction, id.value()));
    }
    if (ownership.has_value()) {
      if (auto checked = check_owner(ownership->sorters, id, instruction); !checked) {
        return checked;
      }
      if (input.sorters[id.value()].index_refill_cursor.has_value()) {
        return check_owner(ownership->write_cursors, *input.sorters[id.value()].index_refill_cursor,
                           instruction);
      }
    }
    return {};
  };
  const auto check_top_n = [&](TopNId id, std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.top_ns.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidTopN, instruction, id.value()));
    }
    return ownership.has_value() ? check_owner(ownership->top_ns, id, instruction)
                                 : ProgramResult<void>{};
  };
  const auto check_relation = [&](RelationId id, std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.relations.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidRelation, instruction, id.value()));
    }
    return ownership.has_value() ? check_owner(ownership->relations, id, instruction)
                                 : ProgramResult<void>{};
  };
  const auto check_record_comparison = [&](RecordComparisonId id,
                                           std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.record_comparisons.size()) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidRecordComparison, instruction, id.value()));
    }
    return {};
  };
  const auto check_aggregate = [&](AggregateId id, std::size_t instruction) -> ProgramResult<void> {
    if (id.value() >= input.aggregates.size()) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidAggregate, instruction, id.value()));
    }
    return ownership.has_value() ? check_owner(ownership->aggregates, id, instruction)
                                 : ProgramResult<void>{};
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

  const bool has_null_rows =
      std::ranges::any_of(input.instructions, [](const Instruction& instruction) {
        return std::holds_alternative<NullRowInstruction>(instruction);
      });
  std::vector<bool> mutation_cursor_ids(has_null_rows ? input.cursors.size() : 0U, false);
  if (has_null_rows) {
    for (const Instruction& instruction : input.instructions) {
      if (const auto* mutation = std::get_if<OpenMutationCursorInstruction>(&instruction);
          mutation != nullptr && mutation->cursor.value() < mutation_cursor_ids.size()) {
        mutation_cursor_ids[mutation->cursor.value()] = true;
      }
    }
  }
  for (std::size_t index = 0; index < input.instructions.size(); ++index) {
    const auto checked = std::visit(
        [&](const auto& operation) -> ProgramResult<void> {
          using Operation = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Operation, HaltInstruction>) {
            return {};
          } else if constexpr (std::is_same_v<Operation, CallSubroutineInstruction> ||
                               std::is_same_v<Operation, ReturnSubroutineInstruction>) {
            if (operation.subroutine.value() >= input.subroutines.size()) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidSubroutine, index,
                                             operation.subroutine.value()));
            }
            return {};
          } else if constexpr (std::is_same_v<Operation, InitCoroutineInstruction> ||
                               std::is_same_v<Operation, ResumeCoroutineInstruction> ||
                               std::is_same_v<Operation, YieldCoroutineInstruction> ||
                               std::is_same_v<Operation, EndCoroutineInstruction> ||
                               std::is_same_v<Operation, CancelCoroutineInstruction>) {
            if (operation.coroutine.value() >= input.coroutines.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::InvalidCoroutine, index, operation.coroutine.value()));
            }
            if constexpr (std::is_same_v<Operation, ResumeCoroutineInstruction>) {
              return check_target(operation.exhausted_target, index);
            }
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
                               std::is_same_v<Operation, RealAffinityInstruction> ||
                               std::is_same_v<Operation, RealStorageAffinityInstruction>) {
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
          } else if constexpr (std::is_same_v<Operation, NullRowInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (mutation_cursor_ids[operation.cursor.value()]) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index,
                                             operation.cursor.value()));
            }
            return {};
          } else if constexpr (std::is_same_v<Operation, OpenMutationCursorInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kDelete &&
                input.statement_kind != ProgramStatementKind::kUpdate) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (input.transaction_access != ProgramTransactionAccess::kWrite) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage != CursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kRowIdOperationRequiresRowIdTable,
                                             index, operation.cursor.value()));
            }
            return ProgramResult<void>{};
          } else if constexpr (std::is_same_v<Operation, OpenWriteCursorInstruction>) {
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.write_cursors[operation.cursor.value()].pending_root) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return ProgramResult<void>{};
          } else if constexpr (std::is_same_v<Operation, CloseWriteCursorInstruction>) {
            return check_write_cursor(operation.cursor, index);
          } else if constexpr (std::is_same_v<Operation, RewindInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage ==
                CursorStorageKind::kEphemeralIndex) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_target(operation.empty_target, index);
          } else if constexpr (std::is_same_v<Operation, NextInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage ==
                CursorStorageKind::kEphemeralIndex) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_target(operation.next_target, index);
          } else if constexpr (std::is_same_v<Operation, LastInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage ==
                CursorStorageKind::kEphemeralIndex) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_target(operation.empty_target, index);
          } else if constexpr (std::is_same_v<Operation, PreviousInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage ==
                CursorStorageKind::kEphemeralIndex) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_target(operation.previous_target, index);
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
          } else if constexpr (std::is_same_v<Operation, SeekTableRowIdInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage != CursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kRowIdOperationRequiresRowIdTable,
                                             index, operation.cursor.value()));
            }
            return check_register(operation.key, index);
          } else if constexpr (std::is_same_v<Operation, SeekIndexInstruction> ||
                               std::is_same_v<Operation, CheckIndexRangeInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (!IsValid(operation.mode)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (input.cursors[operation.cursor.value()].storage != CursorStorageKind::kIndex) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kIndexOperationRequiresIndex, index,
                                             operation.cursor.value()));
            }
            const ReadCursorDescriptor& cursor = input.cursors[operation.cursor.value()];
            if (operation.key_count == 0U || operation.key_count > cursor.index_columns.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.key_count));
            }
            if (auto result = check_range(operation.first_key, operation.key_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            if constexpr (std::is_same_v<Operation, SeekIndexInstruction>) {
              return check_target(operation.missing_target, index);
            } else {
              return check_target(operation.end_target, index);
            }
          } else if constexpr (std::is_same_v<Operation, SeekRelationPrefixInstruction> ||
                               std::is_same_v<Operation, NextRelationPrefixInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            const ReadCursorDescriptor& cursor = input.cursors[operation.cursor.value()];
            if (cursor.storage != CursorStorageKind::kEphemeralIndex) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            const OrderingRecordDescriptor& relation = input.relations[cursor.relation->value()];
            if (operation.key_count == 0U || operation.key_count > relation.key_field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.key_count));
            }
            if (auto result = check_range(operation.first_key, operation.key_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            if constexpr (std::is_same_v<Operation, SeekRelationPrefixInstruction>) {
              return check_target(operation.missing_target, index);
            } else {
              return check_target(operation.next_target, index);
            }
          } else if constexpr (std::is_same_v<Operation, ReadFieldInstruction> ||
                               std::is_same_v<Operation, CallColumnScalarInstruction>) {
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (operation.field.value() >= input.cursors[operation.cursor.value()].fields.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidField, index, operation.field.value()));
            }
            if constexpr (std::is_same_v<Operation, ReadFieldInstruction>) {
              if (operation.missing_value_affinity.has_value() &&
                  !IsValid(*operation.missing_value_affinity)) {
                return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
              }
            } else {
              if (auto result = check_symbol(operation.function, index); !result) {
                return result;
              }
              if (auto result = check_register(operation.argument, index); !result) {
                return result;
              }
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
            if (input.statement_kind != ProgramStatementKind::kInsert &&
                input.statement_kind != ProgramStatementKind::kCreateTable &&
                input.statement_kind != ProgramStatementKind::kCreateIndex &&
                input.statement_kind != ProgramStatementKind::kAnalyze) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.write_cursors[operation.cursor.value()].storage !=
                WriteCursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            if (auto result = check_register(operation.input, index); !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, CheckInsertRowIdInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kInsert) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.write_cursors[operation.cursor.value()].storage !=
                WriteCursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_register(operation.rowid, index);
          } else if constexpr (std::is_same_v<Operation, CheckUpdateRowIdInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kUpdate) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.write_cursors[operation.cursor.value()].storage !=
                WriteCursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            if (auto result = check_register(operation.old_rowid, index); !result) {
              return result;
            }
            return check_register(operation.new_rowid, index);
          } else if constexpr (std::is_same_v<Operation, BuildTableRecordInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kInsert &&
                input.statement_kind != ProgramStatementKind::kUpdate &&
                input.statement_kind != ProgramStatementKind::kCreateTable &&
                input.statement_kind != ProgramStatementKind::kCreateIndex &&
                input.statement_kind != ProgramStatementKind::kAnalyze) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (!IsValid(operation.affinity_mode)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.write_cursors[operation.cursor.value()].storage !=
                WriteCursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
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
          } else if constexpr (std::is_same_v<Operation, CheckUniqueIndexInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kInsert &&
                input.statement_kind != ProgramStatementKind::kUpdate &&
                input.statement_kind != ProgramStatementKind::kCreateIndex) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            const WriteCursorDescriptor& cursor = input.write_cursors[operation.cursor.value()];
            if (cursor.storage != WriteCursorStorageKind::kIndex || !cursor.unique) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            if (operation.key_count != cursor.key_term_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.key_count));
            }
            if (auto result = check_range(operation.first_key, operation.key_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            if (operation.ignored_rowid.has_value()) {
              return check_register(*operation.ignored_rowid, index);
            }
            return ProgramResult<void>{};
          } else if constexpr (std::is_same_v<Operation, InsertIndexInstruction> ||
                               std::is_same_v<Operation, DeleteIndexInstruction>) {
            const bool insert = std::is_same_v<Operation, InsertIndexInstruction>;
            if ((insert && input.statement_kind != ProgramStatementKind::kInsert &&
                 input.statement_kind != ProgramStatementKind::kUpdate &&
                 input.statement_kind != ProgramStatementKind::kCreateIndex) ||
                (!insert && input.statement_kind != ProgramStatementKind::kDelete &&
                 input.statement_kind != ProgramStatementKind::kUpdate)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            const WriteCursorDescriptor& cursor = input.write_cursors[operation.cursor.value()];
            if (cursor.storage != WriteCursorStorageKind::kIndex ||
                operation.value_count != cursor.index_columns.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.value_count));
            }
            return check_range(operation.first_value, operation.value_count,
                               InstructionAddress(static_cast<std::uint32_t>(index)));
          } else if constexpr (std::is_same_v<Operation, InsertSorterIndexInstruction>) {
            if (auto result = check_sorter(operation.sorter, index); !result) {
              return result;
            }
            if (!input.sorters[operation.sorter.value()].index_refill_cursor.has_value()) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor, index,
                                             operation.sorter.value()));
            }
            return ProgramResult<void>{};
          } else if constexpr (std::is_same_v<Operation, InsertTableInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kInsert &&
                input.statement_kind != ProgramStatementKind::kCreateTable &&
                input.statement_kind != ProgramStatementKind::kCreateIndex &&
                input.statement_kind != ProgramStatementKind::kAnalyze) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.write_cursors[operation.cursor.value()].storage !=
                WriteCursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            if (auto result = check_register(operation.rowid, index); !result) {
              return result;
            }
            return check_register(operation.record, index);
          } else if constexpr (std::is_same_v<Operation, DeleteTableInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kDelete) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.write_cursors[operation.cursor.value()].storage !=
                WriteCursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_register(operation.rowid, index);
          } else if constexpr (std::is_same_v<Operation, DeleteCurrentTableInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kDelete) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage != CursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kRowIdOperationRequiresRowIdTable,
                                             index, operation.cursor.value()));
            }
            return check_target(operation.exhausted_target, index);
          } else if constexpr (std::is_same_v<Operation, UpdateCurrentTableInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kUpdate) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage != CursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kRowIdOperationRequiresRowIdTable,
                                             index, operation.cursor.value()));
            }
            return check_register(operation.record, index);
          } else if constexpr (std::is_same_v<Operation, UpdateTableInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kUpdate) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.write_cursors[operation.cursor.value()].storage !=
                WriteCursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
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
          } else if constexpr (std::is_same_v<Operation, CreateTableRootInstruction>) {
            const bool create_table = input.statement_kind == ProgramStatementKind::kCreateTable &&
                                      !operation.cursor.has_value();
            const bool analyze = input.statement_kind == ProgramStatementKind::kAnalyze &&
                                 operation.cursor.has_value();
            if ((!create_table && !analyze) ||
                input.transaction_access != ProgramTransactionAccess::kWrite) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (operation.cursor.has_value()) {
              if (auto result = check_write_cursor(*operation.cursor, index); !result) {
                return result;
              }
              const WriteCursorDescriptor& cursor = input.write_cursors[operation.cursor->value()];
              if (cursor.storage != WriteCursorStorageKind::kRowIdTable || !cursor.pending_root) {
                return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                               operation.cursor->value()));
              }
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, CreateIndexRootInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kCreateIndex ||
                input.transaction_access != ProgramTransactionAccess::kWrite) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            const WriteCursorDescriptor& cursor = input.write_cursors[operation.cursor.value()];
            if (cursor.storage != WriteCursorStorageKind::kIndex || !cursor.pending_root) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, ClearStat1Instruction>) {
            if (input.statement_kind != ProgramStatementKind::kAnalyze ||
                !IsValid(operation.scope)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_write_cursor(operation.cursor, index); !result) {
              return result;
            }
            const WriteCursorDescriptor& cursor = input.write_cursors[operation.cursor.value()];
            if (cursor.storage != WriteCursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            if ((operation.scope == Stat1ClearScope::kDatabase) != !operation.name.has_value()) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidRegister, index));
            }
            return operation.name.has_value() ? check_register(*operation.name, index)
                                              : ProgramResult<void>{};
          } else if constexpr (std::is_same_v<Operation, ComputeIndexStat1Instruction>) {
            if (input.statement_kind != ProgramStatementKind::kAnalyze) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (!IsValid(operation.policy)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            const ReadCursorDescriptor& cursor = input.cursors[operation.cursor.value()];
            if (cursor.storage != CursorStorageKind::kIndex || operation.key_term_count == 0U ||
                operation.key_term_count > cursor.index_columns.size()) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, ComputeTableStat1Instruction>) {
            if (input.statement_kind != ProgramStatementKind::kAnalyze) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (auto result = check_cursor(operation.cursor, index); !result) {
              return result;
            }
            if (input.cursors[operation.cursor.value()].storage != CursorStorageKind::kRowIdTable) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, index,
                                             operation.cursor.value()));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, IncrementSchemaCookieInstruction>) {
            if ((input.statement_kind != ProgramStatementKind::kCreateTable &&
                 input.statement_kind != ProgramStatementKind::kCreateIndex &&
                 input.statement_kind != ProgramStatementKind::kAnalyze) ||
                input.transaction_access != ProgramTransactionAccess::kWrite) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, OpenSorterInstruction> ||
                               std::is_same_v<Operation, ResetSorterInstruction> ||
                               std::is_same_v<Operation, CloseSorterInstruction>) {
            return check_sorter(operation.sorter, index);
          } else if constexpr (std::is_same_v<Operation, InsertSorterInstruction>) {
            if (auto result = check_sorter(operation.sorter, index); !result) {
              return result;
            }
            const OrderingRecordDescriptor& descriptor = input.sorters[operation.sorter.value()];
            if (operation.value_count != descriptor.field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.value_count));
            }
            return check_range(operation.first_value, operation.value_count,
                               InstructionAddress(static_cast<std::uint32_t>(index)));
          } else if constexpr (std::is_same_v<Operation, RewindSorterInstruction>) {
            if (auto result = check_sorter(operation.sorter, index); !result) {
              return result;
            }
            return check_target(operation.empty_target, index);
          } else if constexpr (std::is_same_v<Operation, ReadSorterFieldInstruction>) {
            if (auto result = check_sorter(operation.sorter, index); !result) {
              return result;
            }
            if (operation.field >= input.sorters[operation.sorter.value()].field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidField, index, operation.field));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, NextSorterInstruction>) {
            if (auto result = check_sorter(operation.sorter, index); !result) {
              return result;
            }
            return check_target(operation.next_target, index);
          } else if constexpr (std::is_same_v<Operation, OpenTopNInstruction>) {
            if (auto result = check_top_n(operation.top_n, index); !result) {
              return result;
            }
            return check_register(operation.bound, index);
          } else if constexpr (std::is_same_v<Operation, CheckTopNInstruction>) {
            if (auto result = check_top_n(operation.top_n, index); !result) {
              return result;
            }
            const OrderingRecordDescriptor& descriptor = input.top_ns[operation.top_n.value()];
            if (operation.key_count != descriptor.key_field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.key_count));
            }
            if (auto result = check_range(operation.first_key, operation.key_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            return check_target(operation.rejected_target, index);
          } else if constexpr (std::is_same_v<Operation, InsertTopNInstruction>) {
            if (auto result = check_top_n(operation.top_n, index); !result) {
              return result;
            }
            const OrderingRecordDescriptor& descriptor = input.top_ns[operation.top_n.value()];
            if (operation.value_count != descriptor.field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.value_count));
            }
            return check_range(operation.first_value, operation.value_count,
                               InstructionAddress(static_cast<std::uint32_t>(index)));
          } else if constexpr (std::is_same_v<Operation, RewindTopNInstruction>) {
            if (auto result = check_top_n(operation.top_n, index); !result) {
              return result;
            }
            return check_target(operation.empty_target, index);
          } else if constexpr (std::is_same_v<Operation, ReadTopNFieldInstruction>) {
            if (auto result = check_top_n(operation.top_n, index); !result) {
              return result;
            }
            if (operation.field >= input.top_ns[operation.top_n.value()].field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidField, index, operation.field));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, NextTopNInstruction>) {
            if (auto result = check_top_n(operation.top_n, index); !result) {
              return result;
            }
            return check_target(operation.next_target, index);
          } else if constexpr (std::is_same_v<Operation, ResetTopNInstruction> ||
                               std::is_same_v<Operation, CloseTopNInstruction>) {
            return check_top_n(operation.top_n, index);
          } else if constexpr (std::is_same_v<Operation, OpenRelationInstruction> ||
                               std::is_same_v<Operation, ResetRelationInstruction> ||
                               std::is_same_v<Operation, CloseRelationInstruction>) {
            return check_relation(operation.relation, index);
          } else if constexpr (std::is_same_v<Operation, SealRelationInstruction> ||
                               std::is_same_v<Operation, CheckRelationLookupInstruction>) {
            if (auto result = check_relation(operation.relation, index); !result) {
              return result;
            }
            if (input.relations[operation.relation.value()].relation_purpose !=
                RelationPurpose::kLookup) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor, index,
                                             operation.relation.value()));
            }
            if constexpr (std::is_same_v<Operation, CheckRelationLookupInstruction>) {
              if (auto result = check_target(operation.ready_target, index); !result) {
                return result;
              }
              return check_target(operation.empty_target, index);
            }
            return {};
          } else if constexpr (std::is_same_v<Operation, InsertRelationInstruction>) {
            if (auto result = check_relation(operation.relation, index); !result) {
              return result;
            }
            if (!IsValid(operation.mode)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, index));
            }
            const OrderingRecordDescriptor& descriptor =
                input.relations[operation.relation.value()];
            if (operation.value_count != descriptor.field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.value_count));
            }
            if (auto result = check_range(operation.first_value, operation.value_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            return check_target(operation.duplicate_target, index);
          } else if constexpr (std::is_same_v<Operation, ContainsRelationInstruction>) {
            if (auto result = check_relation(operation.relation, index); !result) {
              return result;
            }
            const OrderingRecordDescriptor& descriptor =
                input.relations[operation.relation.value()];
            if (operation.key_count != descriptor.key_field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.key_count));
            }
            if (auto result = check_range(operation.first_key, operation.key_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            return check_target(operation.found_target, index);
          } else if constexpr (std::is_same_v<Operation, DeleteRelationInstruction>) {
            if (auto result = check_relation(operation.relation, index); !result) {
              return result;
            }
            const OrderingRecordDescriptor& descriptor =
                input.relations[operation.relation.value()];
            if (operation.key_count != descriptor.key_field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidRegisterRange, index, operation.key_count));
            }
            return check_range(operation.first_key, operation.key_count,
                               InstructionAddress(static_cast<std::uint32_t>(index)));
          } else if constexpr (std::is_same_v<Operation, RewindRelationInstruction>) {
            if (auto result = check_relation(operation.relation, index); !result) {
              return result;
            }
            if (input.relations[operation.relation.value()].relation_purpose ==
                RelationPurpose::kLookup) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor, index,
                                             operation.relation.value()));
            }
            return check_target(operation.empty_target, index);
          } else if constexpr (std::is_same_v<Operation, ReadRelationFieldInstruction>) {
            if (auto result = check_relation(operation.relation, index); !result) {
              return result;
            }
            if (input.relations[operation.relation.value()].relation_purpose ==
                RelationPurpose::kLookup) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor, index,
                                             operation.relation.value()));
            }
            if (operation.field >= input.relations[operation.relation.value()].field_count) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kInvalidField, index, operation.field));
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, NextRelationInstruction>) {
            if (auto result = check_relation(operation.relation, index); !result) {
              return result;
            }
            if (input.relations[operation.relation.value()].relation_purpose ==
                RelationPurpose::kLookup) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidOrderingDescriptor, index,
                                             operation.relation.value()));
            }
            return check_target(operation.next_target, index);
          } else if constexpr (std::is_same_v<Operation, CompareRecordsInstruction>) {
            if (auto result = check_record_comparison(operation.comparison, index); !result) {
              return result;
            }
            const std::uint32_t field_count =
                input.record_comparisons[operation.comparison.value()].field_count;
            if (auto result = check_range(operation.left_first, field_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            if (auto result = check_range(operation.right_first, field_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            return check_register(operation.output, index);
          } else if constexpr (std::is_same_v<Operation, OpenAggregateInstruction> ||
                               std::is_same_v<Operation, ResetAggregateInstruction> ||
                               std::is_same_v<Operation, CloseAggregateInstruction>) {
            return check_aggregate(operation.aggregate, index);
          } else if constexpr (std::is_same_v<Operation, StepAggregateInstruction>) {
            if (auto result = check_aggregate(operation.aggregate, index); !result) {
              return result;
            }
            const AggregateDescriptor& descriptor = input.aggregates[operation.aggregate.value()];
            if (auto result = check_range(operation.first_argument, descriptor.argument_count,
                                          InstructionAddress(static_cast<std::uint32_t>(index)));
                !result) {
              return result;
            }
            return operation.effect_output.has_value()
                       ? check_register(*operation.effect_output, index)
                       : ProgramResult<void>{};
          } else if constexpr (std::is_same_v<Operation, FinalizeAggregateInstruction>) {
            if (auto result = check_aggregate(operation.aggregate, index); !result) {
              return result;
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
          } else if constexpr (std::is_same_v<Operation, ResultRowInstruction>) {
            if (input.statement_kind != ProgramStatementKind::kSelect) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidExecutionMetadata, index));
            }
            if (operation.count != input.result_columns.size()) {
              return std::unexpected(
                  ErrorAt(ProgramErrorCode::kResultShapeMismatch, index, operation.count));
            }
            return check_range(operation.first, operation.count,
                               InstructionAddress(static_cast<std::uint32_t>(index)));
          } else {
            static_assert(std::is_same_v<Operation, void>);
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

struct SubroutineGraphFrame {
  std::uint32_t scope;
  std::size_t next_instruction;
};

enum class SubroutineVisitState : std::uint8_t {
  kUnvisited,
  kActive,
  kComplete,
};

struct SubroutineAnalysis {
  std::vector<std::uint32_t> owners;
  std::vector<std::uint32_t> order;
  std::vector<std::uint64_t> inputs;
  std::vector<std::uint64_t> outputs;
  std::vector<std::uint64_t> returned;
  std::vector<std::uint64_t> reads;
  std::vector<std::uint64_t> writes;
  std::vector<std::uint64_t> yielded;
  std::size_t analysis_words = 0;
  std::size_t processed_edge_words = 0;
  std::size_t maximum_depth = 0;
};

[[nodiscard]] std::size_t PrivateRegionCount(const ProgramInput& input) noexcept {
  return input.subroutines.size() + input.coroutines.size();
}

[[nodiscard]] bool IsCoroutineRegion(const ProgramInput& input, std::size_t region) noexcept {
  return region >= input.subroutines.size() && region < PrivateRegionCount(input);
}

[[nodiscard]] InstructionAddress PrivateRegionEntry(const ProgramInput& input,
                                                    std::size_t region) noexcept {
  return region < input.subroutines.size()
             ? input.subroutines[region].entry
             : input.coroutines[region - input.subroutines.size()].entry;
}

[[nodiscard]] InstructionAddress PrivateRegionEnd(const ProgramInput& input,
                                                  std::size_t region) noexcept {
  return region < input.subroutines.size()
             ? input.subroutines[region].end
             : input.coroutines[region - input.subroutines.size()].end;
}

[[nodiscard]] std::optional<CoroutineId> ReferencedCoroutine(
    const Instruction& instruction) noexcept {
  if (const auto* operation = std::get_if<InitCoroutineInstruction>(&instruction)) {
    return operation->coroutine;
  }
  if (const auto* operation = std::get_if<ResumeCoroutineInstruction>(&instruction)) {
    return operation->coroutine;
  }
  if (const auto* operation = std::get_if<YieldCoroutineInstruction>(&instruction)) {
    return operation->coroutine;
  }
  if (const auto* operation = std::get_if<EndCoroutineInstruction>(&instruction)) {
    return operation->coroutine;
  }
  if (const auto* operation = std::get_if<CancelCoroutineInstruction>(&instruction)) {
    return operation->coroutine;
  }
  return std::nullopt;
}

[[nodiscard]] bool IsValueSubroutineInstruction(const Instruction& instruction) noexcept {
  switch (InstructionKindOf(instruction)) {
    case InstructionKind::kLoadConstant:
    case InstructionKind::kLoadParameter:
    case InstructionKind::kCopy:
    case InstructionKind::kUnary:
    case InstructionKind::kBinary:
    case InstructionKind::kApplyAffinity:
    case InstructionKind::kMustBeInteger:
    case InstructionKind::kRealAffinity:
    case InstructionKind::kRealStorageAffinity:
    case InstructionKind::kCast:
    case InstructionKind::kCompare:
    case InstructionKind::kCompareRecords:
    case InstructionKind::kCallScalar:
    case InstructionKind::kJump:
    case InstructionKind::kJumpIf:
    case InstructionKind::kCallSubroutine:
    case InstructionKind::kReturnSubroutine:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool IsReadSubroutineInstruction(const Instruction& instruction) noexcept {
  if (IsValueSubroutineInstruction(instruction)) {
    return true;
  }
  switch (InstructionKindOf(instruction)) {
    case InstructionKind::kOpenRead:
    case InstructionKind::kClose:
    case InstructionKind::kRewind:
    case InstructionKind::kNext:
    case InstructionKind::kLast:
    case InstructionKind::kPrevious:
    case InstructionKind::kSeekRowId:
    case InstructionKind::kSeekTableRowId:
    case InstructionKind::kSeekIndex:
    case InstructionKind::kCheckIndexRange:
    case InstructionKind::kReadField:
    case InstructionKind::CallColumnScalar:
    case InstructionKind::kReadRowId:
    case InstructionKind::kNullRow:
    case InstructionKind::kOpenSorter:
    case InstructionKind::kInsertSorter:
    case InstructionKind::kRewindSorter:
    case InstructionKind::kReadSorterField:
    case InstructionKind::kNextSorter:
    case InstructionKind::kResetSorter:
    case InstructionKind::kCloseSorter:
    case InstructionKind::kOpenTopN:
    case InstructionKind::kCheckTopN:
    case InstructionKind::kInsertTopN:
    case InstructionKind::kRewindTopN:
    case InstructionKind::kReadTopNField:
    case InstructionKind::kNextTopN:
    case InstructionKind::kResetTopN:
    case InstructionKind::kCloseTopN:
    case InstructionKind::kOpenRelation:
    case InstructionKind::kInsertRelation:
    case InstructionKind::kContainsRelation:
    case InstructionKind::kDeleteRelation:
    case InstructionKind::kRewindRelation:
    case InstructionKind::kReadRelationField:
    case InstructionKind::kNextRelation:
    case InstructionKind::kResetRelation:
    case InstructionKind::kCloseRelation:
    case InstructionKind::kCheckRelationLookup:
    case InstructionKind::kSealRelation:
    case InstructionKind::kSeekRelationPrefix:
    case InstructionKind::kNextRelationPrefix:
    case InstructionKind::kOpenAggregate:
    case InstructionKind::kStepAggregate:
    case InstructionKind::kFinalizeAggregate:
    case InstructionKind::kResetAggregate:
    case InstructionKind::kCloseAggregate:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] ProgramResult<SubroutineAnalysis> AnalyzeSubroutineGraph(
    const ProgramInput& input, const ProgramLimits& limits, std::size_t existing_analysis_words) {
  SubroutineAnalysis result;
  const std::size_t count = PrivateRegionCount(input);
  if (count == 0U) {
    return result;
  }
  const std::size_t register_words =
      CeilingDivide(static_cast<std::size_t>(input.register_count), kBitsPerWord);
  const bool has_read = std::ranges::any_of(
      input.subroutines,
      [](const SubroutineDescriptor& region) { return region.kind == SubroutineKind::kRead; });
  std::size_t resource_owner_count = 0;
  if (has_read) {
    for (const std::size_t size :
         {input.cursors.size(), input.write_cursors.size(), input.sorters.size(),
          input.top_ns.size(), input.relations.size(), input.aggregates.size()}) {
      if (!CheckedAdd(size, &resource_owner_count)) {
        return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
      }
    }
  }
  const auto main = static_cast<std::uint32_t>(count);
  std::size_t summary_words = 0;
  std::size_t effect_words = 0;
  std::size_t bytes = 0;
  if (!input.coroutines.empty() && !CheckedMultiply(count + 1U, register_words, &effect_words)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
  }
  if (!CheckedMultiply(count, register_words, &summary_words) ||
      !AddArrayBytes<std::uint32_t>(input.instructions.size(), &bytes) ||
      !AddArrayBytes<std::uint32_t>(count + 1U, &bytes) ||
      !AddArrayBytes<SubroutineVisitState>(count + 1U, &bytes) ||
      !AddArrayBytes<std::size_t>(count + 1U, &bytes) ||
      !AddArrayBytes<SubroutineGraphFrame>(count + 1U, &bytes) ||
      !AddArrayBytes<std::uint64_t>(summary_words, &bytes) ||
      !AddArrayBytes<std::uint64_t>(summary_words, &bytes) ||
      !AddArrayBytes<std::uint64_t>(effect_words, &bytes) ||
      !AddArrayBytes<std::uint64_t>(effect_words, &bytes) ||
      !AddArrayBytes<std::optional<std::uint32_t>>(
          input.coroutines.empty() ? 0U : input.register_count, &bytes) ||
      !AddArrayBytes<std::uint64_t>(CeilingDivide(input.coroutines.size(), kBitsPerWord), &bytes) ||
      !AddArrayBytes<std::optional<std::uint32_t>>(resource_owner_count, &bytes) ||
      !AddArrayBytes<std::uint64_t>(CeilingDivide(count, kBitsPerWord), &bytes)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
  }
  result.analysis_words = CeilingDivide(bytes, sizeof(std::uint64_t));
  if (result.analysis_words > limits.maximum_analysis_words - existing_analysis_words) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
  }
  result.owners.resize(input.instructions.size(), main);
  for (std::size_t index = 0; index < count; ++index) {
    const InstructionAddress entry = PrivateRegionEntry(input, index);
    const InstructionAddress end = PrivateRegionEnd(input, index);
    const bool coroutine = IsCoroutineRegion(input, index);
    if (!coroutine && !IsValid(input.subroutines[index].kind)) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, entry.value(), index));
    }
    const auto invalid_region = coroutine ? ProgramErrorCode::InvalidCoroutineDescriptor
                                          : ProgramErrorCode::kInvalidSubroutineDescriptor;
    if (entry.value() == 0U || entry >= end || end.value() > input.instructions.size()) {
      return std::unexpected(ErrorAt(invalid_region, entry.value(), index));
    }
    if (coroutine) {
      const auto& descriptor = input.coroutines[index - input.subroutines.size()];
      if (descriptor.kind != CoroutineKind::Value) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidEnumValue, entry.value(), index));
      }
      if (descriptor.first_output.value() > input.register_count ||
          descriptor.output_count > input.register_count - descriptor.first_output.value()) {
        return std::unexpected(ErrorAt(invalid_region, entry.value(), index));
      }
    }
    for (std::size_t address = entry.value(); address < end.value(); ++address) {
      if (result.owners[address] != main) {
        return std::unexpected(ErrorAt(invalid_region, address, index));
      }
      result.owners[address] = static_cast<std::uint32_t>(index);
    }
  }
  for (std::size_t address = 0; address < input.instructions.size(); ++address) {
    const Instruction& instruction = input.instructions[address];
    const auto* returned = std::get_if<ReturnSubroutineInstruction>(&instruction);
    const auto* called = std::get_if<CallSubroutineInstruction>(&instruction);
    const std::uint32_t owner = result.owners[address];
    const auto coroutine = ReferencedCoroutine(instruction);
    const bool producer_control = std::holds_alternative<YieldCoroutineInstruction>(instruction) ||
                                  std::holds_alternative<EndCoroutineInstruction>(instruction);
    if (coroutine.has_value()) {
      const std::size_t producer = input.subroutines.size() + coroutine->value();
      if ((producer_control && owner != producer) || (!producer_control && owner != main)) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::InvalidCoroutineControl, address, coroutine->value()));
      }
    }
    const bool owner_is_coroutine = IsCoroutineRegion(input, owner);
    const bool permitted =
        owner == main ||
        (owner_is_coroutine ? producer_control || (IsValueSubroutineInstruction(instruction) &&
                                                   called == nullptr && returned == nullptr)
         : input.subroutines[owner].kind == SubroutineKind::kRead
             ? IsReadSubroutineInstruction(instruction)
             : IsValueSubroutineInstruction(instruction));
    const bool indirect_read =
        owner != main && !owner_is_coroutine && called != nullptr &&
        input.subroutines[owner].kind == SubroutineKind::kValue &&
        input.subroutines[called->subroutine.value()].kind == SubroutineKind::kRead;
    if (!permitted || indirect_read ||
        (returned != nullptr && returned->subroutine.value() != result.owners[address])) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidSubroutineControl, address));
    }
  }
  if (has_read) {
    std::vector<std::optional<std::uint32_t>> resource_owners(resource_owner_count);
    std::size_t offset = 0;
    const auto take = [&](std::size_t size) {
      const auto owners =
          std::span<std::optional<std::uint32_t>>{resource_owners}.subspan(offset, size);
      offset += size;
      return owners;
    };
    const InstructionOwnership ownership{
        .regions = result.owners,
        .cursors = take(input.cursors.size()),
        .write_cursors = take(input.write_cursors.size()),
        .sorters = take(input.sorters.size()),
        .top_ns = take(input.top_ns.size()),
        .relations = take(input.relations.size()),
        .aggregates = take(input.aggregates.size()),
    };
    if (auto checked = CheckInstructions(input, ownership); !checked) {
      return std::unexpected(checked.error());
    }
  }

  std::vector<SubroutineVisitState> colors(count + 1U);
  std::vector<std::size_t> depths(count + 1U);
  std::vector<SubroutineGraphFrame> stack;
  stack.reserve(count + 1U);
  result.order.reserve(count + 1U);
  stack.push_back(SubroutineGraphFrame{.scope = main, .next_instruction = 0});
  colors[main] = SubroutineVisitState::kActive;
  while (!stack.empty()) {
    SubroutineGraphFrame& frame = stack.back();
    const std::size_t end = frame.scope == main ? input.instructions.size()
                                                : PrivateRegionEnd(input, frame.scope).value();
    if (frame.next_instruction == end) {
      const std::uint32_t completed = frame.scope;
      result.order.push_back(completed);
      colors[completed] = SubroutineVisitState::kComplete;
      stack.pop_back();
      if (!stack.empty()) {
        depths[stack.back().scope] = std::max(depths[stack.back().scope], depths[completed] + 1U);
      }
      continue;
    }
    const std::size_t address = frame.next_instruction++;
    if (result.processed_edge_words >= limits.maximum_analysis_edge_words) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kAnalysisWorkLimitExceeded, address));
    }
    ++result.processed_edge_words;
    if (result.owners[address] != frame.scope) {
      continue;
    }
    const auto* call = std::get_if<CallSubroutineInstruction>(&input.instructions[address]);
    const auto* resume = std::get_if<ResumeCoroutineInstruction>(&input.instructions[address]);
    if (call == nullptr && resume == nullptr) {
      continue;
    }
    const std::uint32_t target =
        call != nullptr
            ? call->subroutine.value()
            : static_cast<std::uint32_t>(input.subroutines.size()) + resume->coroutine.value();
    if (colors[target] == SubroutineVisitState::kActive) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kRecursiveSubroutine, address, target));
    }
    if (colors[target] == SubroutineVisitState::kUnvisited) {
      colors[target] = SubroutineVisitState::kActive;
      stack.push_back(SubroutineGraphFrame{
          .scope = target,
          .next_instruction = PrivateRegionEntry(input, target).value(),
      });
    } else {
      depths[frame.scope] = std::max(depths[frame.scope], depths[target] + 1U);
    }
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (colors[index] == SubroutineVisitState::kUnvisited) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kUnreachableInstruction,
                                     PrivateRegionEntry(input, index).value()));
    }
  }
  result.maximum_depth = depths[main];
  result.inputs.resize(summary_words);
  result.outputs.resize(summary_words);
  result.returned.resize(CeilingDivide(count, kBitsPerWord));
  result.reads.resize(effect_words);
  result.writes.resize(effect_words);
  result.yielded.resize(CeilingDivide(input.coroutines.size(), kBitsPerWord));
  return result;
}

struct ProgramAnalysis {
  ProgramVerificationMetrics metrics;
  std::vector<std::optional<CursorId>> relation_borrowers;
};

constexpr std::uint64_t kCoroutineNotInitialized = 1U;
constexpr std::uint64_t kCoroutineLive = 2U;
constexpr std::uint64_t kCoroutineEnded = 4U;

[[nodiscard]] ProgramResult<ProgramAnalysis> AnalyzeProgram(const ProgramInput& input,
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
  const std::size_t capability_count =
      input.sorters.size() + input.top_ns.size() + input.relations.size() + input.aggregates.size();
  const std::size_t capability_offset = register_words + cursor_words;
  const std::size_t top_n_offset = capability_offset + input.sorters.size();
  const std::size_t relation_offset = top_n_offset + input.top_ns.size();
  const std::size_t aggregate_offset = relation_offset + input.relations.size();
  const std::size_t coroutine_offset = aggregate_offset + input.aggregates.size();
  const std::size_t state_words = coroutine_offset + input.coroutines.size();

  std::size_t stored_state_words = 0;
  if (!CheckedMultiply(instruction_count, state_words, &stored_state_words)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
  }
  const std::size_t bitset_words = CeilingDivide(instruction_count, kBitsPerWord);
  const bool has_relation_borrowers =
      std::ranges::any_of(input.cursors, [](const ReadCursorDescriptor& cursor) {
        return cursor.storage == CursorStorageKind::kEphemeralIndex;
      });
  std::size_t borrower_bytes = 0;
  if (!AddArrayBytes<std::optional<CursorId>>(has_relation_borrowers ? input.relations.size() : 0U,
                                              &borrower_bytes)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
  }
  std::size_t analysis_words = stored_state_words;
  if (!CheckedAdd(bitset_words, &analysis_words) || !CheckedAdd(bitset_words, &analysis_words) ||
      !CheckedAdd(CeilingDivide(instruction_count, 2), &analysis_words) ||
      !CheckedAdd(state_words, &analysis_words) ||
      !CheckedAdd(CeilingDivide(borrower_bytes, sizeof(std::uint64_t)), &analysis_words) ||
      analysis_words > limits.maximum_analysis_words) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisLimitExceeded));
  }
  std::vector<std::optional<CursorId>> relation_borrowers(
      has_relation_borrowers ? input.relations.size() : 0U);
  for (std::size_t index = 0; index < input.cursors.size(); ++index) {
    const ReadCursorDescriptor& cursor = input.cursors[index];
    if (!cursor.relation.has_value()) {
      continue;
    }
    std::optional<CursorId>& borrower = relation_borrowers[cursor.relation->value()];
    if (borrower.has_value()) {
      return std::unexpected(
          ErrorAt(ProgramErrorCode::kInvalidCursorDescriptor, ProgramError::kNoInstruction, index));
    }
    borrower = CursorId{static_cast<std::uint32_t>(index)};
  }
  auto subroutines = AnalyzeSubroutineGraph(input, limits, analysis_words);
  if (!subroutines) {
    return std::unexpected(subroutines.error());
  }
  analysis_words += subroutines->analysis_words;

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

  std::size_t reachable_count = 0;
  std::size_t processed_edge_words = subroutines->processed_edge_words;
  const auto main_scope = static_cast<std::uint32_t>(PrivateRegionCount(input));
  std::uint32_t active_scope = main_scope;

  const auto merge_state = [&](std::size_t target,
                               std::span<const std::uint64_t> incoming) -> ProgramResult<void> {
    if (!subroutines->owners.empty() && subroutines->owners[target] != active_scope) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidSubroutineControl, target));
    }
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
      for (std::size_t capability = 0; capability < capability_count; ++capability) {
        const std::size_t index = capability_offset + capability;
        if (current[index] != incoming[index]) {
          if (index >= relation_offset && index < aggregate_offset &&
              input.relations[index - relation_offset].relation_purpose ==
                  RelationPurpose::kLookup &&
              IsLookupReadinessState(static_cast<CapabilityState>(current[index])) &&
              IsLookupReadinessState(static_cast<CapabilityState>(incoming[index]))) {
            if (current[index] != static_cast<std::uint64_t>(CapabilityState::kLookupMaybeReady)) {
              current[index] = static_cast<std::uint64_t>(CapabilityState::kLookupMaybeReady);
              changed = true;
            }
            continue;
          }
          return std::unexpected(
              ErrorAt(ProgramErrorCode::kCapabilityStateConflict, target, capability));
        }
      }
      for (std::size_t coroutine = 0; coroutine < input.coroutines.size(); ++coroutine) {
        const std::size_t index = coroutine_offset + coroutine;
        const auto merged = current[index] | incoming[index];
        if (merged != current[index]) {
          current[index] = merged;
          changed = true;
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

  const bool has_private_regions = PrivateRegionCount(input) != 0U;
  const std::size_t scope_count = has_private_regions ? subroutines->order.size() : 1U;
  std::size_t next_scope = 0;
  const auto enter_scope = [&]() {
    active_scope = has_private_regions ? subroutines->order[next_scope] : main_scope;
    ++next_scope;
    const std::size_t entry =
        active_scope == main_scope ? 0U : PrivateRegionEntry(input, active_scope).value();
    for (std::size_t coroutine = 0; coroutine < input.coroutines.size(); ++coroutine) {
      state_at(entry)[coroutine_offset + coroutine] = kCoroutineNotInitialized;
    }
    ++reachable_count;
    SetBit(reachable, entry, true);
    SetBit(queued, entry, true);
    worklist.push_back(static_cast<std::uint32_t>(entry));
  };
  enter_scope();
  while (!worklist.empty()) {
    const std::size_t instruction_index = worklist.back();
    worklist.pop_back();
    SetBit(queued, instruction_index, false);
    std::ranges::copy(const_state_at(instruction_index), scratch.begin());
    auto state = std::span<std::uint64_t>{scratch};

    const auto require_initialized = [&](RegisterId id) -> ProgramResult<void> {
      if (!subroutines->reads.empty()) {
        SetBit(std::span<std::uint64_t>{subroutines->reads}.subspan(
                   static_cast<std::size_t>(active_scope) * register_words, register_words),
               id.value(), true);
      }
      if (!BitIsSet(std::span<const std::uint64_t>{state}.first(register_words), id.value())) {
        if (active_scope != main_scope) {
          // Callee bits track definite writes; other reads become caller requirements.
          SetBit(std::span<std::uint64_t>{subroutines->inputs}.subspan(
                     static_cast<std::size_t>(active_scope) * register_words, register_words),
                 id.value(), true);
          return {};
        }
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
      if (!subroutines->writes.empty()) {
        SetBit(std::span<std::uint64_t>{subroutines->writes}.subspan(
                   static_cast<std::size_t>(active_scope) * register_words, register_words),
               id.value(), true);
      }
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
    const auto sorter_state = [&](SorterId sorter) noexcept {
      return static_cast<CapabilityState>(state[capability_offset + sorter.value()]);
    };
    const auto set_sorter_state = [&](SorterId sorter, CapabilityState value) noexcept {
      state[capability_offset + sorter.value()] = static_cast<std::uint64_t>(value);
    };
    const auto top_n_state = [&](TopNId top_n) noexcept {
      return static_cast<CapabilityState>(state[top_n_offset + top_n.value()]);
    };
    const auto set_top_n_state = [&](TopNId top_n, CapabilityState value) noexcept {
      state[top_n_offset + top_n.value()] = static_cast<std::uint64_t>(value);
    };
    const auto relation_state = [&](RelationId relation) noexcept {
      return static_cast<CapabilityState>(state[relation_offset + relation.value()]);
    };
    const auto set_relation_state = [&](RelationId relation, CapabilityState value) noexcept {
      state[relation_offset + relation.value()] = static_cast<std::uint64_t>(value);
    };
    const auto aggregate_state = [&](AggregateId aggregate) noexcept {
      return static_cast<AggregateCapabilityState>(state[aggregate_offset + aggregate.value()]);
    };
    const auto set_aggregate_state = [&](AggregateId aggregate,
                                         AggregateCapabilityState value) noexcept {
      state[aggregate_offset + aggregate.value()] = static_cast<std::uint64_t>(value);
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
      if (write_cursor_state(cursor) != CursorState::kUnpositioned) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCursorStateConflict, instruction_index, cursor.value()));
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
    const auto require_sorter_open = [&](SorterId sorter) -> ProgramResult<void> {
      if (sorter_state(sorter) == CapabilityState::kClosed) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotOpen, instruction_index, sorter.value()));
      }
      return {};
    };
    const auto require_sorter_writing = [&](SorterId sorter) -> ProgramResult<void> {
      if (sorter_state(sorter) != CapabilityState::kWriting) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotWriting, instruction_index, sorter.value()));
      }
      return {};
    };
    const auto require_sorter_positioned = [&](SorterId sorter) -> ProgramResult<void> {
      if (sorter_state(sorter) != CapabilityState::kPositioned) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotPositioned, instruction_index, sorter.value()));
      }
      return {};
    };
    const auto require_top_n_open = [&](TopNId top_n) -> ProgramResult<void> {
      if (top_n_state(top_n) == CapabilityState::kClosed) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotOpen, instruction_index, top_n.value()));
      }
      return {};
    };
    const auto require_top_n_writing = [&](TopNId top_n) -> ProgramResult<void> {
      if (top_n_state(top_n) != CapabilityState::kWriting) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotWriting, instruction_index, top_n.value()));
      }
      return {};
    };
    const auto require_top_n_positioned = [&](TopNId top_n) -> ProgramResult<void> {
      if (top_n_state(top_n) != CapabilityState::kPositioned) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotPositioned, instruction_index, top_n.value()));
      }
      return {};
    };
    const auto require_relation_open = [&](RelationId relation) -> ProgramResult<void> {
      if (relation_state(relation) == CapabilityState::kClosed) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotOpen, instruction_index, relation.value()));
      }
      return {};
    };
    const auto require_relation_writing = [&](RelationId relation) -> ProgramResult<void> {
      if (relation_state(relation) != CapabilityState::kWriting) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotWriting, instruction_index, relation.value()));
      }
      return {};
    };
    const auto require_relation_positioned = [&](RelationId relation) -> ProgramResult<void> {
      if (relation_state(relation) != CapabilityState::kPositioned) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityNotPositioned,
                                       instruction_index, relation.value()));
      }
      return {};
    };
    const auto require_relation_lookup_ready = [&](RelationId relation) -> ProgramResult<void> {
      if (auto result = require_relation_open(relation); !result) {
        return result;
      }
      if (relation_state(relation) != CapabilityState::kLookupReady) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityStateConflict,
                                       instruction_index, relation.value()));
      }
      return {};
    };
    const auto require_relation_borrower = [&](RelationId relation,
                                               bool closed) -> ProgramResult<void> {
      if (relation_borrowers.empty() || !relation_borrowers[relation.value()].has_value()) {
        return {};
      }
      const CursorId borrower = *relation_borrowers[relation.value()];
      const CursorState actual = cursor_state(borrower);
      if (closed ? actual != CursorState::kClosed : actual == CursorState::kPositioned) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCursorStateConflict, instruction_index, borrower.value()));
      }
      return {};
    };
    const auto require_aggregate_open = [&](AggregateId aggregate) -> ProgramResult<void> {
      if (aggregate_state(aggregate) == AggregateCapabilityState::kClosed) {
        return std::unexpected(
            ErrorAt(ProgramErrorCode::kCapabilityNotOpen, instruction_index, aggregate.value()));
      }
      return {};
    };
    const auto require_aggregate_accumulating = [&](AggregateId aggregate) -> ProgramResult<void> {
      if (auto result = require_aggregate_open(aggregate); !result) {
        return result;
      }
      if (aggregate_state(aggregate) != AggregateCapabilityState::kAccumulating) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityStateConflict,
                                       instruction_index, aggregate.value()));
      }
      return {};
    };
    const auto require_aggregate_finalized = [&](AggregateId aggregate) -> ProgramResult<void> {
      if (auto result = require_aggregate_open(aggregate); !result) {
        return result;
      }
      if (aggregate_state(aggregate) != AggregateCapabilityState::kFinalized) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityStateConflict,
                                       instruction_index, aggregate.value()));
      }
      return {};
    };

    const auto transferred = std::visit(
        [&](const auto& operation) -> ProgramResult<void> {
          using Operation = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Operation, HaltInstruction>) {
            for (std::size_t aggregate = 0; aggregate < input.aggregates.size(); ++aggregate) {
              if (aggregate_state(AggregateId(static_cast<std::uint32_t>(aggregate))) ==
                  AggregateCapabilityState::kAccumulating) {
                return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityStateConflict,
                                               instruction_index, aggregate));
              }
            }
            return {};
          } else if constexpr (std::is_same_v<Operation, LoadConstantInstruction> ||
                               std::is_same_v<Operation, LoadParameterInstruction> ||
                               std::is_same_v<Operation, ComputeIndexStat1Instruction> ||
                               std::is_same_v<Operation, ComputeTableStat1Instruction> ||
                               std::is_same_v<Operation, IncrementSchemaCookieInstruction>) {
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, OpenSorterInstruction>) {
            if (sorter_state(operation.sorter) != CapabilityState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityAlreadyOpen,
                                             instruction_index, operation.sorter.value()));
            }
            set_sorter_state(operation.sorter, CapabilityState::kWriting);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, InsertSorterInstruction>) {
            if (auto result = require_sorter_writing(operation.sorter); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_value, operation.value_count);
                !result) {
              return result;
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, RewindSorterInstruction>) {
            if (auto result = require_sorter_writing(operation.sorter); !result) {
              return result;
            }
            const auto target = input.sorters[operation.sorter.value()].index_refill_cursor;
            if (target.has_value()) {
              if (auto result = require_write_open(*target); !result) {
                return result;
              }
              set_write_cursor_state(*target, CursorState::IndexAppend);
            }
            set_sorter_state(operation.sorter, CapabilityState::kPositioned);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_sorter_state(operation.sorter, CapabilityState::kExhausted);
            return merge_state(operation.empty_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, InsertSorterIndexInstruction>) {
            if (auto result = require_sorter_positioned(operation.sorter); !result) {
              return result;
            }
            const WriteCursorId target =
                *input.sorters[operation.sorter.value()].index_refill_cursor;
            if (write_cursor_state(target) != CursorState::IndexAppend) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorStateConflict,
                                             instruction_index, target.value()));
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ReadSorterFieldInstruction>) {
            if (auto result = require_sorter_positioned(operation.sorter); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, NextSorterInstruction>) {
            if (auto result = require_sorter_positioned(operation.sorter); !result) {
              return result;
            }
            if (auto result = merge_state(operation.next_target.value(), state); !result) {
              return result;
            }
            set_sorter_state(operation.sorter, CapabilityState::kExhausted);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ResetSorterInstruction>) {
            if (auto result = require_sorter_open(operation.sorter); !result) {
              return result;
            }
            const auto target = input.sorters[operation.sorter.value()].index_refill_cursor;
            if (target.has_value() && write_cursor_state(*target) == CursorState::IndexAppend) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorStateConflict,
                                             instruction_index, target->value()));
            }
            set_sorter_state(operation.sorter, CapabilityState::kWriting);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CloseSorterInstruction>) {
            if (sorter_state(operation.sorter) == CapabilityState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityAlreadyClosed,
                                             instruction_index, operation.sorter.value()));
            }
            set_sorter_state(operation.sorter, CapabilityState::kClosed);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, OpenTopNInstruction>) {
            if (top_n_state(operation.top_n) != CapabilityState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityAlreadyOpen,
                                             instruction_index, operation.top_n.value()));
            }
            if (auto result = require_initialized(operation.bound); !result) {
              return result;
            }
            set_top_n_state(operation.top_n, CapabilityState::kWriting);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CheckTopNInstruction>) {
            if (top_n_state(operation.top_n) == CapabilityState::kCandidatePending) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kTopNCandidatePending,
                                             instruction_index, operation.top_n.value()));
            }
            if (auto result = require_top_n_writing(operation.top_n); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_key, operation.key_count); !result) {
              return result;
            }
            set_top_n_state(operation.top_n, CapabilityState::kCandidatePending);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_top_n_state(operation.top_n, CapabilityState::kWriting);
            return merge_state(operation.rejected_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, InsertTopNInstruction>) {
            if (top_n_state(operation.top_n) != CapabilityState::kCandidatePending) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kTopNCandidateRequired,
                                             instruction_index, operation.top_n.value()));
            }
            if (auto result = require_range(operation.first_value, operation.value_count);
                !result) {
              return result;
            }
            set_top_n_state(operation.top_n, CapabilityState::kWriting);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, RewindTopNInstruction>) {
            if (auto result = require_top_n_writing(operation.top_n); !result) {
              return result;
            }
            set_top_n_state(operation.top_n, CapabilityState::kPositioned);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_top_n_state(operation.top_n, CapabilityState::kExhausted);
            return merge_state(operation.empty_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, ReadTopNFieldInstruction>) {
            if (auto result = require_top_n_positioned(operation.top_n); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, NextTopNInstruction>) {
            if (auto result = require_top_n_positioned(operation.top_n); !result) {
              return result;
            }
            if (auto result = merge_state(operation.next_target.value(), state); !result) {
              return result;
            }
            set_top_n_state(operation.top_n, CapabilityState::kExhausted);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ResetTopNInstruction>) {
            if (auto result = require_top_n_open(operation.top_n); !result) {
              return result;
            }
            set_top_n_state(operation.top_n, CapabilityState::kWriting);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CloseTopNInstruction>) {
            if (top_n_state(operation.top_n) == CapabilityState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityAlreadyClosed,
                                             instruction_index, operation.top_n.value()));
            }
            set_top_n_state(operation.top_n, CapabilityState::kClosed);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, OpenRelationInstruction>) {
            if (relation_state(operation.relation) != CapabilityState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityAlreadyOpen,
                                             instruction_index, operation.relation.value()));
            }
            set_relation_state(operation.relation, CapabilityState::kWriting);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, InsertRelationInstruction>) {
            if (auto result = require_relation_writing(operation.relation); !result) {
              return result;
            }
            if (auto result = require_relation_borrower(operation.relation, false); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_value, operation.value_count);
                !result) {
              return result;
            }
            if (auto result = fallthrough(); !result) {
              return result;
            }
            return merge_state(operation.duplicate_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, ContainsRelationInstruction>) {
            if (auto result = require_relation_writing(operation.relation); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_key, operation.key_count); !result) {
              return result;
            }
            if (auto result = fallthrough(); !result) {
              return result;
            }
            return merge_state(operation.found_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, DeleteRelationInstruction>) {
            if (auto result = require_relation_writing(operation.relation); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_key, operation.key_count); !result) {
              return result;
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, RewindRelationInstruction>) {
            if (auto result = require_relation_writing(operation.relation); !result) {
              return result;
            }
            set_relation_state(operation.relation, CapabilityState::kPositioned);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_relation_state(operation.relation, CapabilityState::kExhausted);
            return merge_state(operation.empty_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, ReadRelationFieldInstruction>) {
            if (auto result = require_relation_positioned(operation.relation); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, NextRelationInstruction>) {
            if (auto result = require_relation_positioned(operation.relation); !result) {
              return result;
            }
            if (auto result = merge_state(operation.next_target.value(), state); !result) {
              return result;
            }
            set_relation_state(operation.relation, CapabilityState::kExhausted);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ResetRelationInstruction>) {
            if (auto result = require_relation_open(operation.relation); !result) {
              return result;
            }
            if (auto result = require_relation_borrower(operation.relation, true); !result) {
              return result;
            }
            set_relation_state(operation.relation, CapabilityState::kWriting);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CloseRelationInstruction>) {
            if (relation_state(operation.relation) == CapabilityState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityAlreadyClosed,
                                             instruction_index, operation.relation.value()));
            }
            if (auto result = require_relation_borrower(operation.relation, true); !result) {
              return result;
            }
            set_relation_state(operation.relation, CapabilityState::kClosed);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CheckRelationLookupInstruction>) {
            if (auto result = require_relation_open(operation.relation); !result) {
              return result;
            }
            const CapabilityState readiness = relation_state(operation.relation);
            if (!IsLookupReadinessState(readiness)) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityStateConflict,
                                             instruction_index, operation.relation.value()));
            }
            if (readiness != CapabilityState::kWriting) {
              set_relation_state(operation.relation, CapabilityState::kLookupReady);
              if (auto result = merge_state(operation.ready_target.value(), state); !result) {
                return result;
              }
              if (auto result = merge_state(operation.empty_target.value(), state); !result) {
                return result;
              }
            }
            if (readiness != CapabilityState::kLookupReady) {
              set_relation_state(operation.relation, CapabilityState::kWriting);
              return fallthrough();
            }
            return {};
          } else if constexpr (std::is_same_v<Operation, SealRelationInstruction>) {
            if (auto result = require_relation_writing(operation.relation); !result) {
              return result;
            }
            if (auto result = require_relation_borrower(operation.relation, false); !result) {
              return result;
            }
            set_relation_state(operation.relation, CapabilityState::kLookupReady);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, OpenAggregateInstruction>) {
            if (aggregate_state(operation.aggregate) != AggregateCapabilityState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityAlreadyOpen,
                                             instruction_index, operation.aggregate.value()));
            }
            set_aggregate_state(operation.aggregate, AggregateCapabilityState::kAccumulating);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, StepAggregateInstruction>) {
            if (auto result = require_aggregate_accumulating(operation.aggregate); !result) {
              return result;
            }
            const AggregateDescriptor& descriptor = input.aggregates[operation.aggregate.value()];
            if (auto result = require_range(operation.first_argument, descriptor.argument_count);
                !result) {
              return result;
            }
            if (operation.effect_output.has_value()) {
              initialize(*operation.effect_output);
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, FinalizeAggregateInstruction>) {
            if (auto result = require_aggregate_accumulating(operation.aggregate); !result) {
              return result;
            }
            initialize(operation.output);
            set_aggregate_state(operation.aggregate, AggregateCapabilityState::kFinalized);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ResetAggregateInstruction>) {
            if (auto result = require_aggregate_finalized(operation.aggregate); !result) {
              return result;
            }
            set_aggregate_state(operation.aggregate, AggregateCapabilityState::kAccumulating);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CloseAggregateInstruction>) {
            if (aggregate_state(operation.aggregate) == AggregateCapabilityState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityAlreadyClosed,
                                             instruction_index, operation.aggregate.value()));
            }
            if (auto result = require_aggregate_finalized(operation.aggregate); !result) {
              return result;
            }
            set_aggregate_state(operation.aggregate, AggregateCapabilityState::kClosed);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CompareRecordsInstruction>) {
            const std::uint32_t field_count =
                input.record_comparisons[operation.comparison.value()].field_count;
            if (auto result = require_range(operation.left_first, field_count); !result) {
              return result;
            }
            if (auto result = require_range(operation.right_first, field_count); !result) {
              return result;
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CreateTableRootInstruction>) {
            if (operation.cursor.has_value()) {
              if (write_cursor_state(*operation.cursor) != CursorState::kClosed) {
                return std::unexpected(ErrorAt(ProgramErrorCode::kCursorAlreadyOpen,
                                               instruction_index, operation.cursor->value()));
              }
              set_write_cursor_state(*operation.cursor, CursorState::kUnpositioned);
            }
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CreateIndexRootInstruction>) {
            if (write_cursor_state(operation.cursor) != CursorState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorAlreadyOpen,
                                             instruction_index, operation.cursor.value()));
            }
            set_write_cursor_state(operation.cursor, CursorState::kUnpositioned);
            initialize(operation.output);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ClearStat1Instruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (operation.name.has_value()) {
              if (auto result = require_initialized(*operation.name); !result) {
                return result;
              }
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CopyInstruction> ||
                               std::is_same_v<Operation, UnaryInstruction> ||
                               std::is_same_v<Operation, ApplyAffinityInstruction> ||
                               std::is_same_v<Operation, MustBeIntegerInstruction> ||
                               std::is_same_v<Operation, RealAffinityInstruction> ||
                               std::is_same_v<Operation, RealStorageAffinityInstruction> ||
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
          } else if constexpr (std::is_same_v<Operation, OpenReadCursorInstruction> ||
                               std::is_same_v<Operation, OpenMutationCursorInstruction>) {
            if (cursor_state(operation.cursor) != CursorState::kClosed) {
              return std::unexpected(ErrorAt(ProgramErrorCode::kCursorAlreadyOpen,
                                             instruction_index, operation.cursor.value()));
            }
            if (input.cursors[operation.cursor.value()].relation.has_value()) {
              if (auto result =
                      require_relation_open(*input.cursors[operation.cursor.value()].relation);
                  !result) {
                return result;
              }
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
          } else if constexpr (std::is_same_v<Operation, NullRowInstruction>) {
            if (auto result = require_open(operation.cursor); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, operation.enabled ? CursorState::kPositioned
                                                                 : CursorState::kUnpositioned);
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
          } else if constexpr (std::is_same_v<Operation, LastInstruction>) {
            if (auto result = require_open(operation.cursor); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kPositioned);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return merge_state(operation.empty_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, PreviousInstruction>) {
            if (auto result = require_positioned(operation.cursor); !result) {
              return result;
            }
            if (auto result = merge_state(operation.previous_target.value(), state); !result) {
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
          } else if constexpr (std::is_same_v<Operation, SeekTableRowIdInstruction>) {
            if (auto result = require_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.key); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kPositioned);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, SeekRelationPrefixInstruction> ||
                               std::is_same_v<Operation, NextRelationPrefixInstruction>) {
            const RelationId relation = *input.cursors[operation.cursor.value()].relation;
            if (auto result = require_relation_lookup_ready(relation); !result) {
              return result;
            }
            if constexpr (std::is_same_v<Operation, SeekRelationPrefixInstruction>) {
              if (auto result = require_open(operation.cursor); !result) {
                return result;
              }
            } else {
              if (auto result = require_positioned(operation.cursor); !result) {
                return result;
              }
            }
            if (auto result = require_range(operation.first_key, operation.key_count); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kPositioned);
            if constexpr (std::is_same_v<Operation, SeekRelationPrefixInstruction>) {
              if (auto result = fallthrough(); !result) {
                return result;
              }
              set_cursor_state(operation.cursor, CursorState::kUnpositioned);
              return merge_state(operation.missing_target.value(), state);
            } else {
              if (auto result = merge_state(operation.next_target.value(), state); !result) {
                return result;
              }
              set_cursor_state(operation.cursor, CursorState::kUnpositioned);
              return fallthrough();
            }
          } else if constexpr (std::is_same_v<Operation, SeekIndexInstruction>) {
            if (auto result = require_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_key, operation.key_count); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kPositioned);
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return merge_state(operation.missing_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, CheckIndexRangeInstruction>) {
            if (auto result = require_positioned(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_key, operation.key_count); !result) {
              return result;
            }
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return merge_state(operation.end_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, ReadFieldInstruction> ||
                               std::is_same_v<Operation, ReadRowIdInstruction> ||
                               std::is_same_v<Operation, CallColumnScalarInstruction>) {
            if (auto result = require_positioned(operation.cursor); !result) {
              return result;
            }
            initialize(operation.output);
            if constexpr (std::is_same_v<Operation, CallColumnScalarInstruction>) {
              initialize(operation.argument);
            }
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
          } else if constexpr (std::is_same_v<Operation, CheckInsertRowIdInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.rowid); !result) {
              return result;
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, CheckUpdateRowIdInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.old_rowid); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.new_rowid); !result) {
              return result;
            }
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
          } else if constexpr (std::is_same_v<Operation, CheckUniqueIndexInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_key, operation.key_count); !result) {
              return result;
            }
            if (operation.ignored_rowid.has_value()) {
              if (auto result = require_initialized(*operation.ignored_rowid); !result) {
                return result;
              }
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, InsertIndexInstruction> ||
                               std::is_same_v<Operation, DeleteIndexInstruction>) {
            if (auto result = require_write_open(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_range(operation.first_value, operation.value_count);
                !result) {
              return result;
            }
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
          } else if constexpr (std::is_same_v<Operation, DeleteCurrentTableInstruction>) {
            if (auto result = require_positioned(operation.cursor); !result) {
              return result;
            }
            if (auto result = fallthrough(); !result) {
              return result;
            }
            set_cursor_state(operation.cursor, CursorState::kUnpositioned);
            return merge_state(operation.exhausted_target.value(), state);
          } else if constexpr (std::is_same_v<Operation, UpdateCurrentTableInstruction>) {
            if (auto result = require_positioned(operation.cursor); !result) {
              return result;
            }
            if (auto result = require_initialized(operation.record); !result) {
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
          } else if constexpr (std::is_same_v<Operation, CallSubroutineInstruction>) {
            const std::size_t offset =
                static_cast<std::size_t>(operation.subroutine.value()) * register_words;
            for (std::size_t word = 0; word < register_words; ++word) {
              std::uint64_t missing = subroutines->inputs[offset + word] & ~state[word];
              while (missing != 0U) {
                const auto bit = static_cast<std::size_t>(std::countr_zero(missing));
                if (auto checked = require_initialized(
                        RegisterId{static_cast<std::uint32_t>(word * kBitsPerWord + bit)});
                    !checked) {
                  return checked;
                }
                missing &= missing - 1U;
              }
              state[word] |= subroutines->outputs[offset + word];
              if (!subroutines->writes.empty()) {
                const std::size_t caller = static_cast<std::size_t>(active_scope) * register_words;
                subroutines->writes[caller + word] |= subroutines->writes[offset + word];
                subroutines->reads[caller + word] |= subroutines->reads[offset + word];
              }
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, InitCoroutineInstruction> ||
                               std::is_same_v<Operation, CancelCoroutineInstruction>) {
            const auto& descriptor = input.coroutines[operation.coroutine.value()];
            const std::size_t slot = coroutine_offset + operation.coroutine.value();
            if constexpr (std::is_same_v<Operation, InitCoroutineInstruction>) {
              if ((state[slot] & kCoroutineLive) != 0U) {
                return std::unexpected(ErrorAt(ProgramErrorCode::CoroutineStateConflict,
                                               instruction_index, operation.coroutine.value()));
              }
              state[slot] = kCoroutineLive;
            } else {
              state[slot] = kCoroutineNotInitialized;
            }
            for (std::uint32_t index = 0; index < descriptor.output_count; ++index) {
              SetBit(state.first(register_words), descriptor.first_output.value() + index, false);
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, ResumeCoroutineInstruction>) {
            const std::size_t slot = coroutine_offset + operation.coroutine.value();
            const std::uint64_t phase = state[slot];
            if ((phase & kCoroutineNotInitialized) != 0U || phase == 0U) {
              return std::unexpected(ErrorAt(ProgramErrorCode::UninitializedCoroutine,
                                             instruction_index, operation.coroutine.value()));
            }
            const std::size_t producer = input.subroutines.size() + operation.coroutine.value();
            const std::size_t offset = producer * register_words;
            if ((phase & kCoroutineLive) != 0U) {
              for (std::size_t word = 0; word < register_words; ++word) {
                std::uint64_t missing = subroutines->inputs[offset + word] & ~state[word];
                while (missing != 0U) {
                  const auto bit = static_cast<std::size_t>(std::countr_zero(missing));
                  if (auto required = require_initialized(
                          RegisterId{static_cast<std::uint32_t>(word * kBitsPerWord + bit)});
                      !required) {
                    return required;
                  }
                  missing &= missing - 1U;
                }
              }
            }
            const auto& descriptor = input.coroutines[operation.coroutine.value()];
            for (std::uint32_t index = 0; index < descriptor.output_count; ++index) {
              SetBit(state.first(register_words), descriptor.first_output.value() + index, false);
            }
            state[slot] = kCoroutineEnded;
            if (auto exhausted = merge_state(operation.exhausted_target.value(), state);
                !exhausted) {
              return exhausted;
            }
            if ((phase & kCoroutineLive) == 0U ||
                !BitIsSet(subroutines->yielded, operation.coroutine.value())) {
              return {};
            }
            state[slot] = kCoroutineLive;
            for (std::uint32_t index = 0; index < descriptor.output_count; ++index) {
              SetBit(state.first(register_words), descriptor.first_output.value() + index, true);
            }
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, YieldCoroutineInstruction>) {
            const auto& descriptor = input.coroutines[operation.coroutine.value()];
            for (std::uint32_t index = 0; index < descriptor.output_count; ++index) {
              const std::uint32_t output = descriptor.first_output.value() + index;
              if (!BitIsSet(state.first(register_words), output)) {
                return std::unexpected(
                    ErrorAt(ProgramErrorCode::kUninitializedRegister, instruction_index, output));
              }
            }
            SetBit(subroutines->yielded, operation.coroutine.value(), true);
            return fallthrough();
          } else if constexpr (std::is_same_v<Operation, EndCoroutineInstruction>) {
            SetBit(subroutines->returned, active_scope, true);
            return {};
          } else if constexpr (std::is_same_v<Operation, ReturnSubroutineInstruction>) {
            if (input.subroutines[operation.subroutine.value()].kind == SubroutineKind::kRead) {
              for (std::size_t cursor = 0; cursor < state_cursor_count; ++cursor) {
                if (GetCursorState(state, register_words, cursor) != CursorState::kClosed) {
                  return std::unexpected(
                      ErrorAt(ProgramErrorCode::kCursorStateConflict, instruction_index, cursor));
                }
              }
              for (std::size_t capability = 0; capability < capability_count; ++capability) {
                if (state[capability_offset + capability] != 0U) {
                  return std::unexpected(ErrorAt(ProgramErrorCode::kCapabilityStateConflict,
                                                 instruction_index, capability));
                }
              }
            }
            const std::size_t offset =
                static_cast<std::size_t>(operation.subroutine.value()) * register_words;
            if (!BitIsSet(subroutines->returned, operation.subroutine.value())) {
              std::ranges::copy(state.first(register_words),
                                subroutines->outputs.begin() + static_cast<std::ptrdiff_t>(offset));
              SetBit(subroutines->returned, operation.subroutine.value(), true);
            } else {
              for (std::size_t word = 0; word < register_words; ++word) {
                subroutines->outputs[offset + word] &= state[word];
              }
            }
            return {};
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
          } else if constexpr (std::is_same_v<Operation, ResultRowInstruction>) {
            if (auto result = require_range(operation.first, operation.count); !result) {
              return result;
            }
            return fallthrough();
          } else {
            static_assert(std::is_same_v<Operation, void>);
          }
        },
        input.instructions[instruction_index]);
    if (!transferred) {
      return std::unexpected(transferred.error());
    }
    if (worklist.empty()) {
      if (active_scope != main_scope && !BitIsSet(subroutines->returned, active_scope)) {
        return std::unexpected(ErrorAt(ProgramErrorCode::kInvalidSubroutineControl,
                                       PrivateRegionEntry(input, active_scope).value()));
      }
      if (next_scope < scope_count) {
        enter_scope();
      }
    }
  }

  for (std::size_t index = 0; index < instruction_count; ++index) {
    if (!BitIsSet(reachable, index)) {
      return std::unexpected(ErrorAt(ProgramErrorCode::kUnreachableInstruction, index));
    }
  }

  std::vector<std::optional<std::uint32_t>> coroutine_register_owners(
      input.coroutines.empty() ? 0U : input.register_count);
  const auto charge_effect_work = [&]() {
    if (processed_edge_words >= limits.maximum_analysis_edge_words) {
      return false;
    }
    ++processed_edge_words;
    return true;
  };
  const auto low_bits = [](std::size_t count) noexcept {
    return count == kBitsPerWord ? std::numeric_limits<std::uint64_t>::max()
                                 : (std::uint64_t{1} << count) - 1U;
  };
  for (std::size_t coroutine = 0; coroutine < input.coroutines.size(); ++coroutine) {
    const auto& descriptor = input.coroutines[coroutine];
    const std::size_t producer = input.subroutines.size() + coroutine;
    const std::size_t output_begin = descriptor.first_output.value();
    const std::size_t output_end = output_begin + descriptor.output_count;
    for (std::size_t word = 0; word < register_words; ++word) {
      if (!charge_effect_work()) {
        return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisWorkLimitExceeded));
      }
      const std::size_t word_begin = word * kBitsPerWord;
      const std::size_t begin =
          output_begin <= word_begin ? 0U : std::min(output_begin - word_begin, kBitsPerWord);
      const std::size_t end =
          output_end <= word_begin ? 0U : std::min(output_end - word_begin, kBitsPerWord);
      auto owned = subroutines->writes[producer * register_words + word] |
                   (low_bits(end) & ~low_bits(begin));
      while (owned != 0U) {
        if (!charge_effect_work()) {
          return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisWorkLimitExceeded));
        }
        const auto bit = static_cast<std::size_t>(std::countr_zero(owned));
        auto& owner = coroutine_register_owners[word_begin + bit];
        if (owner.has_value() && *owner != producer) {
          return std::unexpected(ErrorAt(ProgramErrorCode::CoroutineRegisterConflict,
                                         descriptor.entry.value(), word_begin + bit));
        }
        owner = static_cast<std::uint32_t>(producer);
        owned &= owned - 1U;
      }
    }
  }
  for (std::size_t scope = 0; !input.coroutines.empty() && scope <= PrivateRegionCount(input);
       ++scope) {
    for (std::size_t word = 0; word < register_words; ++word) {
      if (!charge_effect_work()) {
        return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisWorkLimitExceeded));
      }
      auto accessed = subroutines->reads[scope * register_words + word] |
                      subroutines->writes[scope * register_words + word];
      while (accessed != 0U) {
        if (!charge_effect_work()) {
          return std::unexpected(MakeProgramError(ProgramErrorCode::kAnalysisWorkLimitExceeded));
        }
        const auto bit = static_cast<std::size_t>(std::countr_zero(accessed));
        const auto id = word * kBitsPerWord + bit;
        const auto owner = coroutine_register_owners[id];
        if (owner.has_value() && *owner != scope) {
          const auto& descriptor = input.coroutines[*owner - input.subroutines.size()];
          const bool written = (subroutines->writes[scope * register_words + word] &
                                (std::uint64_t{1} << bit)) != 0U;
          if (written || id < descriptor.first_output.value() ||
              id - descriptor.first_output.value() >= descriptor.output_count) {
            return std::unexpected(
                ErrorAt(ProgramErrorCode::CoroutineRegisterConflict, descriptor.entry.value(), id));
          }
        }
        accessed &= accessed - 1U;
      }
    }
  }

  return ProgramAnalysis{
      .metrics =
          ProgramVerificationMetrics{
              .owned_bytes = owned_bytes,
              .analysis_words = analysis_words,
              .processed_edge_words = processed_edge_words,
              .reachable_instruction_count = reachable_count,
              .maximum_subroutine_depth = subroutines->maximum_depth,
          },
      .relation_borrowers = std::move(relation_borrowers),
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
  std::size_t index_columns = 0;
  if (!CheckedMultiply(cursor.columns.size(), sizeof(WriteColumnDescriptor), &columns) ||
      !CheckedMultiply(cursor.index_columns.size(), sizeof(IndexColumnMetadata), &index_columns) ||
      !CheckedAdd(columns, &result) || !CheckedAdd(index_columns, &result)) {
    return std::numeric_limits<std::size_t>::max();
  }
  return result;
}

[[nodiscard]] std::size_t OrderingOwnedBytes(const OrderingRecordDescriptor& descriptor) noexcept {
  std::size_t result = sizeof(OrderingRecordDescriptor);
  std::size_t columns = 0;
  if (!CheckedMultiply(descriptor.key_columns.size(), sizeof(OrderingColumnMetadata), &columns) ||
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
    case InstructionKind::kRealStorageAffinity:
      return "real_storage_affinity";
    case InstructionKind::kCast:
      return "cast";
    case InstructionKind::kOpenRead:
      return "open_read";
    case InstructionKind::kOpenMutation:
      return "open_mutation";
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
    case InstructionKind::kLast:
      return "last";
    case InstructionKind::kPrevious:
      return "previous";
    case InstructionKind::kNullRow:
      return "null_row";
    case InstructionKind::kCallSubroutine:
      return "call_subroutine";
    case InstructionKind::kReturnSubroutine:
      return "return_subroutine";
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
    case InstructionKind::kSeekTableRowId:
      return "seek_table_rowid";
    case InstructionKind::kSeekIndex:
      return "seek_index";
    case InstructionKind::kCheckIndexRange:
      return "check_index_range";
    case InstructionKind::kReadField:
      return "read_field";
    case InstructionKind::kReadRowId:
      return "read_rowid";
    case InstructionKind::kResolveInsertRowId:
      return "resolve_insert_rowid";
    case InstructionKind::kCheckInsertRowId:
      return "check_insert_rowid";
    case InstructionKind::kCheckUpdateRowId:
      return "check_update_rowid";
    case InstructionKind::kBuildTableRecord:
      return "build_table_record";
    case InstructionKind::kCheckUniqueIndex:
      return "check_unique_index";
    case InstructionKind::kInsertIndex:
      return "insert_index";
    case InstructionKind::InsertSorterIndex:
      return "insert_sorter_index";
    case InstructionKind::kDeleteIndex:
      return "delete_index";
    case InstructionKind::kInsertTable:
      return "insert_table";
    case InstructionKind::kDeleteTable:
      return "delete_table";
    case InstructionKind::kDeleteCurrentTable:
      return "delete_current_table";
    case InstructionKind::kUpdateCurrentTable:
      return "update_current_table";
    case InstructionKind::kUpdateTable:
      return "update_table";
    case InstructionKind::kEnsureDatabaseInitialized:
      return "ensure_database_initialized";
    case InstructionKind::kCreateTableRoot:
      return "create_table_root";
    case InstructionKind::kCreateIndexRoot:
      return "create_index_root";
    case InstructionKind::kClearStat1:
      return "clear_stat1";
    case InstructionKind::kComputeIndexStat1:
      return "compute_index_stat1";
    case InstructionKind::kComputeTableStat1:
      return "compute_table_stat1";
    case InstructionKind::kIncrementSchemaCookie:
      return "increment_schema_cookie";
    case InstructionKind::kCompare:
      return "compare";
    case InstructionKind::kCallScalar:
      return "call_scalar";
    case InstructionKind::CallColumnScalar:
      return "call_column_scalar";
    case InstructionKind::InitCoroutine:
      return "init_coroutine";
    case InstructionKind::ResumeCoroutine:
      return "resume_coroutine";
    case InstructionKind::YieldCoroutine:
      return "yield_coroutine";
    case InstructionKind::EndCoroutine:
      return "end_coroutine";
    case InstructionKind::CancelCoroutine:
      return "cancel_coroutine";
    case InstructionKind::kJump:
      return "jump";
    case InstructionKind::kJumpIf:
      return "jump_if";
    case InstructionKind::kResultRow:
      return "result_row";
    case InstructionKind::kOpenSorter:
      return "open_sorter";
    case InstructionKind::kInsertSorter:
      return "insert_sorter";
    case InstructionKind::kRewindSorter:
      return "rewind_sorter";
    case InstructionKind::kReadSorterField:
      return "read_sorter_field";
    case InstructionKind::kNextSorter:
      return "next_sorter";
    case InstructionKind::kResetSorter:
      return "reset_sorter";
    case InstructionKind::kCloseSorter:
      return "close_sorter";
    case InstructionKind::kOpenTopN:
      return "open_top_n";
    case InstructionKind::kCheckTopN:
      return "check_top_n";
    case InstructionKind::kInsertTopN:
      return "insert_top_n";
    case InstructionKind::kRewindTopN:
      return "rewind_top_n";
    case InstructionKind::kReadTopNField:
      return "read_top_n_field";
    case InstructionKind::kNextTopN:
      return "next_top_n";
    case InstructionKind::kResetTopN:
      return "reset_top_n";
    case InstructionKind::kCloseTopN:
      return "close_top_n";
    case InstructionKind::kOpenRelation:
      return "open_relation";
    case InstructionKind::kInsertRelation:
      return "insert_relation";
    case InstructionKind::kContainsRelation:
      return "contains_relation";
    case InstructionKind::kDeleteRelation:
      return "delete_relation";
    case InstructionKind::kRewindRelation:
      return "rewind_relation";
    case InstructionKind::kReadRelationField:
      return "read_relation_field";
    case InstructionKind::kNextRelation:
      return "next_relation";
    case InstructionKind::kResetRelation:
      return "reset_relation";
    case InstructionKind::kCloseRelation:
      return "close_relation";
    case InstructionKind::kCompareRecords:
      return "compare_records";
    case InstructionKind::kOpenAggregate:
      return "open_aggregate";
    case InstructionKind::kStepAggregate:
      return "step_aggregate";
    case InstructionKind::kFinalizeAggregate:
      return "finalize_aggregate";
    case InstructionKind::kResetAggregate:
      return "reset_aggregate";
    case InstructionKind::kCloseAggregate:
      return "close_aggregate";
    case InstructionKind::kCheckRelationLookup:
      return "check_relation_lookup";
    case InstructionKind::kSealRelation:
      return "seal_relation";
    case InstructionKind::kSeekRelationPrefix:
      return "seek_relation_prefix";
    case InstructionKind::kNextRelationPrefix:
      return "next_relation_prefix";
  }
  return "unknown";
}

ErrorCode ProgramError::base_error_code() const noexcept {
  switch (code) {
    case ProgramErrorCode::kInstructionLimitExceeded:
    case ProgramErrorCode::kRegisterLimitExceeded:
    case ProgramErrorCode::kCursorLimitExceeded:
    case ProgramErrorCode::kSorterLimitExceeded:
    case ProgramErrorCode::kTopNLimitExceeded:
    case ProgramErrorCode::kRelationLimitExceeded:
    case ProgramErrorCode::kRecordComparisonLimitExceeded:
    case ProgramErrorCode::kParameterLimitExceeded:
    case ProgramErrorCode::kConstantLimitExceeded:
    case ProgramErrorCode::kSymbolLimitExceeded:
    case ProgramErrorCode::kResultColumnLimitExceeded:
    case ProgramErrorCode::kLabelLimitExceeded:
    case ProgramErrorCode::kOwnedBytesLimitExceeded:
    case ProgramErrorCode::kAnalysisLimitExceeded:
    case ProgramErrorCode::kAnalysisWorkLimitExceeded:
    case ProgramErrorCode::kAggregateLimitExceeded:
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
    case ProgramErrorCode::kInvalidSorter:
    case ProgramErrorCode::kInvalidTopN:
    case ProgramErrorCode::kInvalidRelation:
    case ProgramErrorCode::kInvalidRecordComparison:
    case ProgramErrorCode::kInvalidOrderingDescriptor:
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
    case ProgramErrorCode::kCapabilityAlreadyOpen:
    case ProgramErrorCode::kCapabilityAlreadyClosed:
    case ProgramErrorCode::kCapabilityNotOpen:
    case ProgramErrorCode::kCapabilityNotWriting:
    case ProgramErrorCode::kCapabilityNotPositioned:
    case ProgramErrorCode::kCapabilityStateConflict:
    case ProgramErrorCode::kTopNCandidatePending:
    case ProgramErrorCode::kTopNCandidateRequired:
    case ProgramErrorCode::kRowIdOperationRequiresRowIdTable:
    case ProgramErrorCode::kIndexOperationRequiresIndex:
    case ProgramErrorCode::kFallthroughPastEnd:
    case ProgramErrorCode::kUnreachableInstruction:
    case ProgramErrorCode::kInvalidAggregate:
    case ProgramErrorCode::kInvalidAggregateDescriptor:
    case ProgramErrorCode::kInvalidSubroutine:
    case ProgramErrorCode::kInvalidSubroutineDescriptor:
    case ProgramErrorCode::kRecursiveSubroutine:
    case ProgramErrorCode::kInvalidSubroutineControl:
    case ProgramErrorCode::InvalidCoroutine:
    case ProgramErrorCode::InvalidCoroutineDescriptor:
    case ProgramErrorCode::InvalidCoroutineControl:
    case ProgramErrorCode::UninitializedCoroutine:
    case ProgramErrorCode::CoroutineStateConflict:
    case ProgramErrorCode::CoroutineRegisterConflict:
      return ErrorCode::kMisuse;
  }
  return ErrorCode::kInternal;
}

namespace {

[[nodiscard]] ProgramResult<ProgramAnalysis> VerifyProgramAnalysis(const ProgramInput& input,
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

}  // namespace

ProgramResult<ProgramVerificationMetrics> VerifyProgram(const ProgramInput& input,
                                                        ProgramLimits limits) {
  auto analysis = VerifyProgramAnalysis(input, limits);
  if (!analysis.has_value()) {
    return std::unexpected(analysis.error());
  }
  return analysis->metrics;
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
  auto verified = VerifyProgramAnalysis(canonical, limits);
  if (!verified) {
    return std::unexpected(verified.error());
  }
  return BytecodeProgram(std::move(canonical), verified->metrics,
                         std::move(verified->relation_borrowers));
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

const OrderingRecordDescriptor& BytecodeProgram::sorter(SorterId id) const noexcept {
  return input_.sorters[id.value()];
}

const OrderingRecordDescriptor& BytecodeProgram::top_n(TopNId id) const noexcept {
  return input_.top_ns[id.value()];
}

const OrderingRecordDescriptor& BytecodeProgram::relation(RelationId id) const noexcept {
  return input_.relations[id.value()];
}

const OrderingRecordDescriptor& BytecodeProgram::record_comparison(
    RecordComparisonId id) const noexcept {
  return input_.record_comparisons[id.value()];
}

const Instruction& BytecodeProgram::instruction(InstructionAddress address) const noexcept {
  return input_.instructions[address.value()];
}

const AggregateDescriptor& BytecodeProgram::aggregate(AggregateId id) const noexcept {
  return input_.aggregates[id.value()];
}

const SubroutineDescriptor& BytecodeProgram::subroutine(SubroutineId id) const noexcept {
  return input_.subroutines[id.value()];
}

const CoroutineDescriptor& BytecodeProgram::coroutine(CoroutineId id) const noexcept {
  return input_.coroutines[id.value()];
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
      label_addresses_(std::move(other.label_addresses_)),
      subroutine_regions_(std::move(other.subroutine_regions_)),
      coroutine_regions_(std::move(other.coroutine_regions_)) {}

ProgramBuilder& ProgramBuilder::operator=(ProgramBuilder&& other) noexcept {
  if (this != &other) {
    owner_ = std::exchange(other.owner_, 0);
    limits_ = other.limits_;
    owned_bytes_ = std::exchange(other.owned_bytes_, 0);
    input_ = std::move(other.input_);
    pending_ = std::move(other.pending_);
    label_addresses_ = std::move(other.label_addresses_);
    subroutine_regions_ = std::move(other.subroutine_regions_);
    coroutine_regions_ = std::move(other.coroutine_regions_);
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
    input_.requires_database_snapshot |=
        input_.cursors.back().storage != CursorStorageKind::kEphemeralIndex;
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

ProgramResult<SorterId> ProgramBuilder::AddSorter(OrderingRecordDescriptor sorter) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.sorters.size() >= limits_.maximum_sorters ||
      input_.sorters.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kSorterLimitExceeded));
  }
  const std::size_t bytes = OrderingOwnedBytes(sorter);
  if (bytes == std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.sorters.push_back(std::move(sorter));
  } catch (...) {
    owned_bytes_ -= bytes;
    throw;
  }
  return SorterId(static_cast<std::uint32_t>(input_.sorters.size() - 1U));
}

ProgramResult<TopNId> ProgramBuilder::AddTopN(OrderingRecordDescriptor top_n) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.top_ns.size() >= limits_.maximum_top_ns ||
      input_.top_ns.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kTopNLimitExceeded));
  }
  const std::size_t bytes = OrderingOwnedBytes(top_n);
  if (bytes == std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.top_ns.push_back(std::move(top_n));
  } catch (...) {
    owned_bytes_ -= bytes;
    throw;
  }
  return TopNId(static_cast<std::uint32_t>(input_.top_ns.size() - 1U));
}

ProgramResult<RelationId> ProgramBuilder::AddRelation(OrderingRecordDescriptor relation) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.relations.size() >= limits_.maximum_relations ||
      input_.relations.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kRelationLimitExceeded));
  }
  const std::size_t bytes = OrderingOwnedBytes(relation);
  if (bytes == std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.relations.push_back(std::move(relation));
  } catch (...) {
    owned_bytes_ -= bytes;
    throw;
  }
  return RelationId(static_cast<std::uint32_t>(input_.relations.size() - 1U));
}

ProgramResult<RecordComparisonId> ProgramBuilder::AddRecordComparison(
    OrderingRecordDescriptor comparison) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.record_comparisons.size() >= limits_.maximum_record_comparisons ||
      input_.record_comparisons.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kRecordComparisonLimitExceeded));
  }
  const std::size_t bytes = OrderingOwnedBytes(comparison);
  if (bytes == std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kOwnedBytesLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(bytes); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.record_comparisons.push_back(std::move(comparison));
  } catch (...) {
    owned_bytes_ -= bytes;
    throw;
  }
  return RecordComparisonId(static_cast<std::uint32_t>(input_.record_comparisons.size() - 1U));
}

ProgramResult<AggregateId> ProgramBuilder::AddAggregate(AggregateDescriptor aggregate) {
  if (auto usable = CheckUsable(); !usable) {
    return std::unexpected(usable.error());
  }
  if (input_.aggregates.size() >= limits_.maximum_aggregates ||
      input_.aggregates.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kAggregateLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(sizeof(AggregateDescriptor)); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    input_.aggregates.push_back(aggregate);
  } catch (...) {
    owned_bytes_ -= sizeof(AggregateDescriptor);
    throw;
  }
  return AggregateId(static_cast<std::uint32_t>(input_.aggregates.size() - 1U));
}

ProgramResult<SubroutineId> ProgramBuilder::AddSubroutine(Label entry, Label last,
                                                          SubroutineKind kind) {
  if (auto checked = CheckLabel(entry); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckLabel(last); !checked) {
    return std::unexpected(checked.error());
  }
  if (!IsValid(kind)) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidEnumValue));
  }
  if (subroutine_regions_.size() >= limits_.maximum_instructions ||
      subroutine_regions_.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInstructionLimitExceeded));
  }
  if (auto reserved = ReserveOwnedBytes(sizeof(SubroutineDescriptor)); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    subroutine_regions_.push_back(PendingSubroutine{.entry = entry, .last = last, .kind = kind});
  } catch (...) {
    owned_bytes_ -= sizeof(SubroutineDescriptor);
    throw;
  }
  return SubroutineId(static_cast<std::uint32_t>(subroutine_regions_.size() - 1U));
}

ProgramResult<CoroutineId> ProgramBuilder::AddCoroutine(Label entry, Label last,
                                                        RegisterId first_output,
                                                        std::uint32_t output_count,
                                                        CoroutineKind kind) {
  if (auto checked = CheckLabel(entry); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckLabel(last); !checked) {
    return std::unexpected(checked.error());
  }
  if (kind != CoroutineKind::Value) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInvalidEnumValue));
  }
  if (coroutine_regions_.size() >= limits_.maximum_instructions ||
      coroutine_regions_.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInstructionLimitExceeded));
  }
  if (first_output.value() > input_.register_count ||
      output_count > input_.register_count - first_output.value()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::InvalidCoroutineDescriptor));
  }
  if (auto reserved = ReserveOwnedBytes(sizeof(CoroutineDescriptor)); !reserved) {
    return std::unexpected(reserved.error());
  }
  try {
    coroutine_regions_.push_back(PendingCoroutine{.entry = entry,
                                                  .last = last,
                                                  .first_output = first_output,
                                                  .output_count = output_count,
                                                  .kind = kind});
  } catch (...) {
    owned_bytes_ -= sizeof(CoroutineDescriptor);
    throw;
  }
  return CoroutineId(static_cast<std::uint32_t>(coroutine_regions_.size() - 1U));
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitResumeCoroutine(CoroutineId coroutine,
                                                                      Label exhausted_target) {
  if (auto checked = CheckLabel(exhausted_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingResumeCoroutine{.coroutine = coroutine, .target = exhausted_target});
}

ProgramResult<void> ProgramBuilder::SetExecutionMetadata(
    ProgramStatementKind statement_kind, ProgramTransactionAccess transaction_access,
    ProgramRollbackMode rollback_mode, MutationResultMetadata mutation_result,
    ProgramWriteEffects write_effects) {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  input_.statement_kind = statement_kind;
  input_.transaction_access = transaction_access;
  input_.rollback_mode = rollback_mode;
  input_.write_effects = write_effects;
  input_.mutation_result = mutation_result;
  if (transaction_access == ProgramTransactionAccess::kWrite) {
    input_.requires_database_snapshot = true;
  }
  return {};
}

ProgramResult<void> ProgramBuilder::MarkMayAbort() {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  input_.write_effects.may_abort = true;
  return {};
}

ProgramResult<void> ProgramBuilder::RequireDatabaseSnapshot() {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  input_.requires_database_snapshot = true;
  return {};
}

ProgramResult<void> ProgramBuilder::ReserveInstructionCapacity(std::size_t count) {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  if (count > limits_.maximum_instructions || count > std::numeric_limits<std::uint32_t>::max() ||
      count > pending_.max_size()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kInstructionLimitExceeded));
  }
  pending_.reserve(count);
  return {};
}

ProgramResult<void> ProgramBuilder::ReserveLabelCapacity(std::size_t count) {
  if (auto usable = CheckUsable(); !usable) {
    return usable;
  }
  if (count > limits_.maximum_labels || count > std::numeric_limits<std::uint32_t>::max() ||
      count > label_addresses_.max_size()) {
    return std::unexpected(MakeProgramError(ProgramErrorCode::kLabelLimitExceeded));
  }
  label_addresses_.reserve(count);
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
      std::holds_alternative<LastInstruction>(instruction) ||
      std::holds_alternative<PreviousInstruction>(instruction) ||
      std::holds_alternative<RewindRowIdListInstruction>(instruction) ||
      std::holds_alternative<NextRowIdListInstruction>(instruction) ||
      std::holds_alternative<RewindSorterInstruction>(instruction) ||
      std::holds_alternative<NextSorterInstruction>(instruction) ||
      std::holds_alternative<CheckTopNInstruction>(instruction) ||
      std::holds_alternative<RewindTopNInstruction>(instruction) ||
      std::holds_alternative<NextTopNInstruction>(instruction) ||
      std::holds_alternative<InsertRelationInstruction>(instruction) ||
      std::holds_alternative<ContainsRelationInstruction>(instruction) ||
      std::holds_alternative<RewindRelationInstruction>(instruction) ||
      std::holds_alternative<NextRelationInstruction>(instruction) ||
      std::holds_alternative<CheckRelationLookupInstruction>(instruction) ||
      std::holds_alternative<SeekRelationPrefixInstruction>(instruction) ||
      std::holds_alternative<NextRelationPrefixInstruction>(instruction) ||
      std::holds_alternative<SeekRowIdInstruction>(instruction) ||
      std::holds_alternative<SeekIndexInstruction>(instruction) ||
      std::holds_alternative<CheckIndexRangeInstruction>(instruction) ||
      std::holds_alternative<DeleteCurrentTableInstruction>(instruction) ||
      std::holds_alternative<ResumeCoroutineInstruction>(instruction) ||
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

ProgramResult<InstructionAddress> ProgramBuilder::EmitLast(CursorId cursor, Label empty_target) {
  if (auto checked = CheckLabel(empty_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingLast{.cursor = cursor, .target = empty_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitPrevious(CursorId cursor,
                                                               Label previous_target) {
  if (auto checked = CheckLabel(previous_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingPrevious{.cursor = cursor, .target = previous_target});
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

ProgramResult<InstructionAddress> ProgramBuilder::EmitRewindSorter(SorterId sorter,
                                                                   Label empty_target) {
  if (auto checked = CheckLabel(empty_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingRewindSorter{.sorter = sorter, .target = empty_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitNextSorter(SorterId sorter,
                                                                 Label next_target) {
  if (auto checked = CheckLabel(next_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingNextSorter{.sorter = sorter, .target = next_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitCheckTopN(TopNId top_n, RegisterId first_key,
                                                                std::uint32_t key_count,
                                                                Label rejected_target) {
  if (auto checked = CheckLabel(rejected_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingCheckTopN{
      .top_n = top_n,
      .first_key = first_key,
      .key_count = key_count,
      .target = rejected_target,
  });
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitRewindTopN(TopNId top_n, Label empty_target) {
  if (auto checked = CheckLabel(empty_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingRewindTopN{.top_n = top_n, .target = empty_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitNextTopN(TopNId top_n, Label next_target) {
  if (auto checked = CheckLabel(next_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingNextTopN{.top_n = top_n, .target = next_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitInsertRelation(RelationId relation,
                                                                     RegisterId first_value,
                                                                     std::uint32_t value_count,
                                                                     RelationInsertMode mode,
                                                                     Label duplicate_target) {
  if (auto checked = CheckLabel(duplicate_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingInsertRelation{
      .relation = relation,
      .first_value = first_value,
      .value_count = value_count,
      .mode = mode,
      .target = duplicate_target,
  });
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitContainsRelation(RelationId relation,
                                                                       RegisterId first_key,
                                                                       std::uint32_t key_count,
                                                                       Label found_target) {
  if (auto checked = CheckLabel(found_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingContainsRelation{
      .relation = relation,
      .first_key = first_key,
      .key_count = key_count,
      .target = found_target,
  });
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitRewindRelation(RelationId relation,
                                                                     Label empty_target) {
  if (auto checked = CheckLabel(empty_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingRewindRelation{.relation = relation, .target = empty_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitNextRelation(RelationId relation,
                                                                   Label next_target) {
  if (auto checked = CheckLabel(next_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingNextRelation{.relation = relation, .target = next_target});
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitCheckRelationLookup(RelationId relation,
                                                                          Label ready_target,
                                                                          Label empty_target) {
  if (auto checked = CheckLabel(ready_target); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = CheckLabel(empty_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingCheckRelationLookup{
      .relation = relation,
      .ready_target = ready_target,
      .empty_target = empty_target,
  });
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitSeekRelationPrefix(CursorId cursor,
                                                                         RegisterId first_key,
                                                                         std::uint32_t key_count,
                                                                         Label missing_target) {
  if (auto checked = CheckLabel(missing_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingSeekRelationPrefix{
      .cursor = cursor,
      .first_key = first_key,
      .key_count = key_count,
      .target = missing_target,
  });
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitNextRelationPrefix(CursorId cursor,
                                                                         RegisterId first_key,
                                                                         std::uint32_t key_count,
                                                                         Label next_target) {
  if (auto checked = CheckLabel(next_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingNextRelationPrefix{
      .cursor = cursor,
      .first_key = first_key,
      .key_count = key_count,
      .target = next_target,
  });
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

ProgramResult<InstructionAddress> ProgramBuilder::EmitSeekIndex(CursorId cursor,
                                                                RegisterId first_key,
                                                                std::uint32_t key_count,
                                                                Label missing_target,
                                                                IndexSeekMode mode) {
  if (auto checked = CheckLabel(missing_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingSeekIndex{
      .cursor = cursor,
      .first_key = first_key,
      .key_count = key_count,
      .target = missing_target,
      .mode = mode,
  });
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitCheckIndexRange(CursorId cursor,
                                                                      RegisterId first_key,
                                                                      std::uint32_t key_count,
                                                                      Label end_target,
                                                                      IndexRangeEndMode mode) {
  if (auto checked = CheckLabel(end_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingCheckIndexRange{
      .cursor = cursor,
      .first_key = first_key,
      .key_count = key_count,
      .target = end_target,
      .mode = mode,
  });
}

ProgramResult<InstructionAddress> ProgramBuilder::EmitDeleteCurrentTable(CursorId cursor,
                                                                         Label exhausted_target) {
  if (auto checked = CheckLabel(exhausted_target); !checked) {
    return std::unexpected(checked.error());
  }
  return AppendPending(PendingDeleteCurrentTable{.cursor = cursor, .target = exhausted_target});
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
  input_.subroutines.reserve(subroutine_regions_.size());
  for (const PendingSubroutine& region : subroutine_regions_) {
    input_.subroutines.push_back(SubroutineDescriptor{
        .entry = target(region.entry),
        .end = InstructionAddress{target(region.last).value() + 1U},
        .kind = region.kind,
    });
  }
  input_.coroutines.reserve(coroutine_regions_.size());
  for (const PendingCoroutine& region : coroutine_regions_) {
    input_.coroutines.push_back(CoroutineDescriptor{
        .entry = target(region.entry),
        .end = InstructionAddress{target(region.last).value() + 1U},
        .first_output = region.first_output,
        .output_count = region.output_count,
        .kind = region.kind,
    });
  }
  for (const auto& pending : pending_) {
    input_.instructions.push_back(std::visit(
        [&](const auto& operation) -> Instruction {
          using Operation = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Operation, Instruction>) {
            return operation;
          } else if constexpr (std::is_same_v<Operation, PendingResumeCoroutine>) {
            return ResumeCoroutineInstruction{.coroutine = operation.coroutine,
                                              .exhausted_target = target(operation.target)};
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
          } else if constexpr (std::is_same_v<Operation, PendingLast>) {
            return LastInstruction{
                .cursor = operation.cursor,
                .empty_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingPrevious>) {
            return PreviousInstruction{
                .cursor = operation.cursor,
                .previous_target = target(operation.target),
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
          } else if constexpr (std::is_same_v<Operation, PendingRewindSorter>) {
            return RewindSorterInstruction{
                .sorter = operation.sorter,
                .empty_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingNextSorter>) {
            return NextSorterInstruction{
                .sorter = operation.sorter,
                .next_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingCheckTopN>) {
            return CheckTopNInstruction{
                .top_n = operation.top_n,
                .first_key = operation.first_key,
                .key_count = operation.key_count,
                .rejected_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingRewindTopN>) {
            return RewindTopNInstruction{
                .top_n = operation.top_n,
                .empty_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingNextTopN>) {
            return NextTopNInstruction{
                .top_n = operation.top_n,
                .next_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingInsertRelation>) {
            return InsertRelationInstruction{
                .relation = operation.relation,
                .first_value = operation.first_value,
                .value_count = operation.value_count,
                .mode = operation.mode,
                .duplicate_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingContainsRelation>) {
            return ContainsRelationInstruction{
                .relation = operation.relation,
                .first_key = operation.first_key,
                .key_count = operation.key_count,
                .found_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingRewindRelation>) {
            return RewindRelationInstruction{
                .relation = operation.relation,
                .empty_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingNextRelation>) {
            return NextRelationInstruction{
                .relation = operation.relation,
                .next_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingCheckRelationLookup>) {
            return CheckRelationLookupInstruction{
                .relation = operation.relation,
                .ready_target = target(operation.ready_target),
                .empty_target = target(operation.empty_target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingSeekRelationPrefix>) {
            return SeekRelationPrefixInstruction{
                .cursor = operation.cursor,
                .first_key = operation.first_key,
                .key_count = operation.key_count,
                .missing_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingNextRelationPrefix>) {
            return NextRelationPrefixInstruction{
                .cursor = operation.cursor,
                .first_key = operation.first_key,
                .key_count = operation.key_count,
                .next_target = target(operation.target),
            };
          } else if constexpr (std::is_same_v<Operation, PendingSeekRowId>) {
            return SeekRowIdInstruction{
                .cursor = operation.cursor,
                .key = operation.key,
                .missing_target = target(operation.target),
                .mode = operation.mode,
            };
          } else if constexpr (std::is_same_v<Operation, PendingSeekIndex>) {
            return SeekIndexInstruction{
                .cursor = operation.cursor,
                .first_key = operation.first_key,
                .key_count = operation.key_count,
                .missing_target = target(operation.target),
                .mode = operation.mode,
            };
          } else if constexpr (std::is_same_v<Operation, PendingCheckIndexRange>) {
            return CheckIndexRangeInstruction{
                .cursor = operation.cursor,
                .first_key = operation.first_key,
                .key_count = operation.key_count,
                .end_target = target(operation.target),
                .mode = operation.mode,
            };
          } else if constexpr (std::is_same_v<Operation, PendingDeleteCurrentTable>) {
            return DeleteCurrentTableInstruction{
                .cursor = operation.cursor,
                .exhausted_target = target(operation.target),
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
