#include "modern_sqlite/vm/vm.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
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

#include "../runtime/sql_value_access.hpp"
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
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"

namespace modern_sqlite {
namespace {

using DispatchResult = Result<std::optional<VmStep>>;
constexpr std::size_t kSharedColumnThresholdBytes = 4'000U;

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

[[nodiscard]] SqlTruthValue LogicalBinary(SqlTruthValue left, SqlTruthValue right,
                                          bool is_and) noexcept {
  if (is_and) {
    if (left == SqlTruthValue::kFalse || right == SqlTruthValue::kFalse) {
      return SqlTruthValue::kFalse;
    }
    if (left == SqlTruthValue::kNull || right == SqlTruthValue::kNull) {
      return SqlTruthValue::kNull;
    }
    return SqlTruthValue::kTrue;
  }

  if (left == SqlTruthValue::kTrue || right == SqlTruthValue::kTrue) {
    return SqlTruthValue::kTrue;
  }
  if (left == SqlTruthValue::kNull || right == SqlTruthValue::kNull) {
    return SqlTruthValue::kNull;
  }
  return SqlTruthValue::kFalse;
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

enum class RowIdSeekDisposition : std::uint8_t {
  kSeek,
  kFirst,
  kLast,
  kMissing,
};

struct RowIdSeekTarget {
  RowIdSeekDisposition disposition = RowIdSeekDisposition::kMissing;
  std::int64_t rowid = 0;
  BtreeSeekMode mode = BtreeSeekMode::kEqual;
};

[[nodiscard]] RowIdSeekTarget NonNumericRowIdSeek(RowIdSeekMode mode) noexcept {
  switch (mode) {
    case RowIdSeekMode::kEqual:
    case RowIdSeekMode::kGreaterOrEqual:
    case RowIdSeekMode::kGreater:
      return {};
    case RowIdSeekMode::kLessOrEqual:
    case RowIdSeekMode::kLess:
      return RowIdSeekTarget{.disposition = RowIdSeekDisposition::kLast};
  }
  return {};
}

[[nodiscard]] BtreeSeekMode BtreeMode(RowIdSeekMode mode) noexcept {
  switch (mode) {
    case RowIdSeekMode::kEqual:
      return BtreeSeekMode::kEqual;
    case RowIdSeekMode::kGreaterOrEqual:
      return BtreeSeekMode::kGreaterOrEqual;
    case RowIdSeekMode::kGreater:
      return BtreeSeekMode::kGreater;
    case RowIdSeekMode::kLessOrEqual:
      return BtreeSeekMode::kLessOrEqual;
    case RowIdSeekMode::kLess:
      return BtreeSeekMode::kLess;
  }
  return BtreeSeekMode::kEqual;
}

[[nodiscard]] RowIdSeekTarget ConvertRowIdSeek(const SqlValue& value, RowIdSeekMode mode) {
  if (mode == RowIdSeekMode::kEqual) {
    const std::optional<std::int64_t> rowid = LosslessRowId(value);
    return rowid.has_value()
               ? RowIdSeekTarget{
                     .disposition = RowIdSeekDisposition::kSeek,
                     .rowid = *rowid,
                     .mode = BtreeSeekMode::kEqual,
                 }
               : RowIdSeekTarget{};
  }
  if (value.type() == SqlValueType::kNull) {
    return {};
  }

  const SqlValue numeric = value.type() == SqlValueType::kText
                               ? ApplyAffinity(value.Clone(), TypeAffinity::kNumeric)
                               : value.Clone();
  if (numeric.type() == SqlValueType::kInteger) {
    const auto integer = numeric.integer_value();
    if (!integer.has_value()) {
      std::terminate();
    }
    return RowIdSeekTarget{
        .disposition = RowIdSeekDisposition::kSeek,
        .rowid = *integer,
        .mode = BtreeMode(mode),
    };
  }
  if (numeric.type() != SqlValueType::kReal) {
    return NonNumericRowIdSeek(mode);
  }
  const auto real_value = numeric.real_value();
  if (!real_value.has_value()) {
    std::terminate();
  }
  const double real = *real_value;
  if (std::isnan(real)) {
    return {};
  }
  const std::int64_t rowid = CoerceIntegerForBitwise(numeric);
  const std::strong_ordering comparison = CompareSqlIntegerAndReal(rowid, real);
  RowIdSeekMode adjusted = mode;
  if (comparison == std::strong_ordering::greater) {
    if (adjusted == RowIdSeekMode::kGreater) {
      adjusted = RowIdSeekMode::kGreaterOrEqual;
    } else if (adjusted == RowIdSeekMode::kLessOrEqual) {
      adjusted = RowIdSeekMode::kLess;
    }
  } else if (comparison == std::strong_ordering::less) {
    if (adjusted == RowIdSeekMode::kGreaterOrEqual) {
      adjusted = RowIdSeekMode::kGreater;
    } else if (adjusted == RowIdSeekMode::kLess) {
      adjusted = RowIdSeekMode::kLessOrEqual;
    }
  }
  return RowIdSeekTarget{
      .disposition = RowIdSeekDisposition::kSeek,
      .rowid = rowid,
      .mode = BtreeMode(adjusted),
  };
}

[[nodiscard]] Result<SqlValue> ApplyTableRecordAffinity(SqlValue value, TypeAffinity affinity,
                                                        RecordCodecOptions options) {
  if (affinity == TypeAffinity::kReal) {
    if (value.type() == SqlValueType::kReal && !std::isfinite(value.real_value().value_or(0.0))) {
      return value;
    }
    value = ApplyAffinity(std::move(value), TypeAffinity::kNumeric);
    if (value.type() == SqlValueType::kInteger) {
      auto serial_type = SelectRecordSerialType(value, options);
      if (!serial_type.has_value()) {
        return std::unexpected(std::move(serial_type.error()));
      }
      if (serial_type->code() == 6U) {
        return SqlValue::Real(static_cast<double>(value.integer_value().value_or(std::int64_t{0})));
      }
    }
    return value;
  }
  return ApplyAffinity(std::move(value), affinity);
}

[[nodiscard]] SqlValue ApplyRealStorageAffinity(SqlValue value) {
  if (value.type() == SqlValueType::kReal && !std::isfinite(value.real_value().value_or(0.0))) {
    return value;
  }
  value = ApplyAffinity(std::move(value), TypeAffinity::kNumeric);
  if (value.type() == SqlValueType::kInteger) {
    return SqlValue::Real(static_cast<double>(value.integer_value().value_or(std::int64_t{0})));
  }
  return value;
}

struct ResolvedCall {
  std::uint32_t address = 0;
  const ScalarFunction* function = nullptr;
  const Collation* collation = nullptr;
};

struct ColumnFunctionConsumer {
  const ScalarFunction& function;
  RegisterId output;
  bool real_affinity;
  bool completed = false;
};

struct RuntimeAggregate {
  const AggregateFunction* function = nullptr;
  const Collation* collation = nullptr;
  std::optional<AggregateState> state{};
};

struct RuntimeColumnCache {
  std::shared_ptr<const ByteBuffer> bytes;
  FileOffset payload_offset{};
  std::uint32_t column = 0;
  std::uint32_t row_cache_counter = 0;
  std::uint32_t table_write_counter = 0;
};

struct RuntimeCursor {
  enum class RowState : std::uint8_t { Ordinary, VirtualNull, DeletedNull };
  using Storage = std::variant<std::monostate, TableBtreeCursor, IndexBtreeCursor,
                               TableBtreeMutationCursor, RelationId>;

  void ClearRecordCache() noexcept {
    row_state = RowState::Ordinary;
    columns.Invalidate();
    row_bytes = {};
    column_cache_status = 0;
  }

  Storage storage;
  RecordColumnLayout columns;
  RuntimeColumnCache column_cache;
  ByteView row_bytes;
  std::uint32_t column_cache_status = 0;
  RowState row_state = RowState::Ordinary;
};

struct RuntimeIndexRefill {
  IndexBtreeAppendCursor cursor;
  ByteBuffer previous_record{};
  std::size_t previous_size = 0;
};

struct RuntimeWriteCursor {
  std::optional<TableBtreeWriter> table;
  std::optional<IndexBtreeWriter> index;
  std::optional<RuntimeIndexRefill> index_refill;
};

struct Stat1PreviousField {
  ByteBuffer storage{};
  RecordFieldView value{};
};

struct Stat1Accumulator {
  explicit Stat1Accumulator(std::size_t physical_columns) : distinct_less(physical_columns, 0U) {}

  [[nodiscard]] Status Push(std::size_t first_changed) {
    if (first_changed >= distinct_less.size()) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "ANALYZE change boundary is invalid"));
    }
    if (rows == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "ANALYZE row count is exhausted"));
    }
    if (rows != 0U) {
      for (std::size_t field = first_changed; field < distinct_less.size(); ++field) {
        if (distinct_less[field] == std::numeric_limits<std::uint64_t>::max()) {
          return std::unexpected(
              VmError(ErrorCode::kTooLarge, "ANALYZE distinct count is exhausted"));
        }
        ++distinct_less[field];
      }
    }
    ++rows;
    return {};
  }

  [[nodiscard]] Result<SqlValue> ResultText(std::size_t key_count, bool emit_empty,
                                            std::size_t maximum_bytes) const {
    if (rows == 0U && !emit_empty) {
      return SqlValue{};
    }
    constexpr std::size_t kMaximumStat1NumberBytes = 22;
    if (rows == 0U) {
      if (key_count > (maximum_bytes > 0U ? (maximum_bytes - 1U) / 2U : 0U) ||
          maximum_bytes == 0U) {
        return std::unexpected(
            VmError(ErrorCode::kTooLarge, "ANALYZE stat text exceeds the VM limit"));
      }
    } else if (maximum_bytes < kMaximumStat1NumberBytes ||
               key_count + 1U > maximum_bytes / kMaximumStat1NumberBytes) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "ANALYZE stat text exceeds the VM limit"));
    }
    std::string stat = std::to_string(rows);
    for (std::size_t field = 0; field < key_count; ++field) {
      const std::uint64_t distinct = distinct_less[field] + 1U;
      std::uint64_t estimate = rows / distinct + static_cast<std::uint64_t>(rows % distinct != 0U);
      if (estimate == 2U &&
          rows <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
          distinct <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        constexpr std::int64_t kNearUniqueRowScale = 10;
        constexpr std::int64_t kNearUniqueDistinctScale = 11;
        auto left = CheckedMultiply(static_cast<std::int64_t>(rows), kNearUniqueRowScale);
        auto right = CheckedMultiply(static_cast<std::int64_t>(distinct), kNearUniqueDistinctScale);
        if (!left.has_value() || !right.has_value()) {
          return std::unexpected(VmError(ErrorCode::kTooLarge, "ANALYZE normalization overflow"));
        }
        if (*left <= *right) {
          estimate = 1U;
        }
      }
      stat.push_back(' ');
      stat.append(std::to_string(estimate));
    }
    return SqlValue::Text(std::move(stat));
  }

  std::uint64_t rows = 0;
  std::vector<std::uint64_t> distinct_less;
};

[[nodiscard]] Result<std::size_t> RetainStat1ChangedSuffix(
    const RecordPrefixView& record, std::span<Stat1PreviousField> previous,
    std::span<const IndexColumnOrder> columns, bool first_row) {
  if (record.field_count() < previous.size() || columns.size() < previous.size()) {
    return std::unexpected(VmError(ErrorCode::kCorruption, "ANALYZE index row has too few fields"));
  }
  std::size_t first_changed = first_row ? 0U : previous.size();
  RecordCursor current = record.cursor();
  for (std::size_t field = 0; field < previous.size(); ++field) {
    const auto value = current.Next();
    if (!value.has_value()) {
      return std::unexpected(VmError(ErrorCode::kCorruption, "ANALYZE index field is missing"));
    }
    if (first_changed == previous.size() &&
        CompareRecordFields(previous[field].value, *value, columns[field].collation()) ==
            std::weak_ordering::equivalent) {
      continue;
    }
    first_changed = std::min(first_changed, field);
    auto retained = value->CopyTo(previous[field].storage);
    if (!retained.has_value()) {
      return std::unexpected(std::move(retained.error()));
    }
    previous[field].value = *retained;
  }
  return first_changed;
}

}  // namespace

struct Vm::Impl {
  enum class CoroutinePhase : std::uint8_t {
    Uninitialized,
    Running,
    Suspended,
    Ended,
    Cancelled,
  };

  struct CoroutineContinuation {
    InstructionAddress resume_address{0};
    InstructionAddress return_address{0};
    InstructionAddress exhausted_target{0};
    CoroutinePhase phase = CoroutinePhase::Uninitialized;
  };

  struct SubroutineContinuation {
    SubroutineId subroutine;
    InstructionAddress return_address;
  };

  Impl(const BytecodeProgram& program, VmEnvironment environment, VmLimits limits)
      : program_(&program),
        functions_(environment.functions_),
        available_collations_(environment.collations_),
        limits_(limits),
        registers_(program.register_count()),
        parameters_(program.parameter_count()),
        cursors_(program.cursors().size()),
        write_cursors_(program.write_cursors().size()),
        sorters_(program.sorters().size()),
        top_ns_(program.top_ns().size()),
        relations_(program.relations().size()),
        aggregates_(program.aggregates().size()),
        record_comparison_columns_(program.record_comparisons().size()),
        coroutines_(program.coroutines().size()),
        resolved_collations_(program.symbols().size(), nullptr) {}

  ~Impl() { ClearExecution(); }

  [[nodiscard]] Status Initialize() {
    if (functions_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "VM environment is incomplete"));
    }
    subroutine_continuations_.reserve(program_->verification_metrics().maximum_subroutine_depth);
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
    const auto resolve_ordering_collations = [&](const auto& descriptors) -> Status {
      for (const auto& descriptor : descriptors) {
        for (const OrderingColumnMetadata& column : descriptor.key_columns) {
          auto collation = ResolveCollation(column.collation);
          if (!collation.has_value()) {
            return std::unexpected(std::move(collation.error()));
          }
        }
      }
      return {};
    };
    if (auto resolved = resolve_ordering_collations(program_->sorters()); !resolved.has_value()) {
      return resolved;
    }
    if (auto resolved = resolve_ordering_collations(program_->top_ns()); !resolved.has_value()) {
      return resolved;
    }
    if (auto resolved = resolve_ordering_collations(program_->relations()); !resolved.has_value()) {
      return resolved;
    }
    for (std::size_t index = 0; index < program_->record_comparisons().size(); ++index) {
      const auto& metadata = program_->record_comparisons()[index].key_columns;
      auto& columns = record_comparison_columns_[index];
      columns.reserve(metadata.size());
      for (const RecordComparisonColumnMetadata& key : metadata) {
        if (auto resolved = ResolveCollation(key.ordering.collation); !resolved.has_value()) {
          return std::unexpected(std::move(resolved.error()));
        }
        columns.push_back(RecordValueComparisonKey{
            .field_index = key.field_index, .ordering = RuntimeOrderingColumn(key.ordering)});
      }
    }
    for (std::size_t index = 0; index < program_->aggregates().size(); ++index) {
      const AggregateDescriptor& descriptor = program_->aggregates()[index];
      auto function = functions_->ResolveAggregate(program_->symbol(descriptor.function),
                                                   descriptor.argument_count);
      if (!function.has_value()) {
        return std::unexpected(std::move(function.error()));
      }
      auto collation = ResolveCollation(descriptor.collation);
      if (!collation.has_value()) {
        return std::unexpected(std::move(collation.error()));
      }
      aggregates_[index].function = *function;
      aggregates_[index].collation = *collation;
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
      const auto* column = std::get_if<CallColumnScalarInstruction>(&instruction);
      if (call == nullptr && column == nullptr) {
        continue;
      }
      const SymbolId function_symbol = call != nullptr ? call->function : column->function;
      const std::size_t argument_count = call != nullptr ? call->argument_count : 1U;
      auto function = functions_->Resolve(program_->symbol(function_symbol), argument_count);
      if (!function.has_value()) {
        return std::unexpected(std::move(function.error()));
      }
      auto collation = call != nullptr ? ResolveCollation(call->collation)
                                       : RegisteredCollation(BinaryCollation().name());
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
    if (context.temporary_storage_ != nullptr &&
        !context.temporary_storage_->attached_to(*context.pager_)) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "temporary storage factory does not match the VM pager"));
    }
    if (program_->transaction_access() == ProgramTransactionAccess::kWrite &&
        context.writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "write bytecode requires a transaction writer"));
    }
    database_ = context.database_;
    pager_ = context.pager_;
    catalog_generation_ = context.catalog_generation_;
    writer_ = context.writer_;
    temporary_storage_ = context.temporary_storage_;
    Status schema = ValidateSchema();
    if (!schema.has_value()) {
      database_ = nullptr;
      pager_ = nullptr;
      catalog_generation_ = 0;
      writer_ = nullptr;
      temporary_storage_ = nullptr;
      return schema;
    }
    return {};
  }

  [[nodiscard]] Status DetachExecutionContext() {
    if (pager_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "VM execution context is not attached"));
    }
    if (state_ == VmState::kRow || state_ == VmState::Running) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "cannot detach an executing or row-suspended VM"));
    }
    ClearExecution();
    pager_ = nullptr;
    database_ = nullptr;
    catalog_generation_ = 0;
    writer_ = nullptr;
    temporary_storage_ = nullptr;
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

    const bool first_step = state_ == VmState::kReady;
    state_ = VmState::Running;
    column_write_counter_ = 0;
    if (first_step) {
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
    if (state_ == VmState::Running) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "cannot reset a running VM"));
    }
    ClearExecution();
    program_counter_ = 0;
    executed_instruction_count_ = 0;
    change_count_ = 0;
    last_insert_rowid_event_.reset();
    rowid_list_.clear();
    rowid_list_index_ = 0;
    rowid_list_positioned_ = false;
    state_ = VmState::kReady;
    database_ = nullptr;
    pager_ = nullptr;
    catalog_generation_ = 0;
    writer_ = nullptr;
    temporary_storage_ = nullptr;
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
    ClearExecution();
    state_ = VmState::kError;
    return std::unexpected(std::move(error));
  }

  void FailForException() noexcept {
    ClearExecution();
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

  [[nodiscard]] Result<const Collation*> RegisteredCollation(std::string_view name) const {
    for (const Collation* collation : available_collations_) {
      if (EqualAsciiCaseInsensitive(name, collation->name())) {
        return collation;
      }
    }
    return std::unexpected(VmError(ErrorCode::kGeneric, "required collation is not registered"));
  }

  [[nodiscard]] Result<const Collation*> ResolveCollation(SymbolId symbol) {
    const std::size_t index = symbol.value();
    if (resolved_collations_[index] != nullptr) {
      return resolved_collations_[index];
    }
    auto collation = RegisteredCollation(program_->symbol(symbol));
    if (!collation.has_value()) {
      return std::unexpected(std::move(collation.error()));
    }
    resolved_collations_[index] = *collation;
    return *collation;
  }

  [[nodiscard]] const Collation& CollationFor(SymbolId symbol) const noexcept {
    return *resolved_collations_[symbol.value()];
  }

  [[nodiscard]] Result<IndexComparisonInfo> RuntimeIndexInfo(
      std::span<const IndexColumnMetadata> metadata, std::size_t key_field_count,
      bool creating = false) const try {
    std::vector<IndexColumnOrder> columns;
    columns.reserve(metadata.size());
    const bool legacy = creating && record_options_.schema_format != DatabaseSchemaFormat::kFour;
    for (const IndexColumnMetadata& column : metadata) {
      const bool descending = !legacy && column.order == BytecodeSortOrder::kDescending;
      columns.emplace_back(
          CollationFor(column.collation),
          descending ? IndexSortDirection::kDescending : IndexSortDirection::kAscending,
          descending ? IndexNullPlacement::kLast : IndexNullPlacement::kFirst);
    }
    return IndexComparisonInfo::CopyOf(columns, IndexKeyShape{.key_field_count = key_field_count,
                                                              .all_field_count = metadata.size()});
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }

  [[nodiscard]] IndexColumnOrder RuntimeOrderingColumn(const OrderingColumnMetadata& column,
                                                       bool legacy_index = false) const noexcept {
    return IndexColumnOrder{
        CollationFor(column.collation),
        !legacy_index && column.order == BytecodeSortOrder::kDescending
            ? IndexSortDirection::kDescending
            : IndexSortDirection::kAscending,
        !legacy_index && column.null_placement == BytecodeNullPlacement::kLast
            ? IndexNullPlacement::kLast
            : IndexNullPlacement::kFirst,
    };
  }

  [[nodiscard]] std::vector<IndexColumnOrder> RuntimeOrderingColumns(
      std::span<const OrderingColumnMetadata> metadata, bool legacy_index = false) const {
    std::vector<IndexColumnOrder> columns;
    columns.reserve(metadata.size());
    for (const OrderingColumnMetadata& column : metadata) {
      columns.push_back(RuntimeOrderingColumn(column, legacy_index));
    }
    return columns;
  }

  [[nodiscard]] RecordSorterDescriptor RuntimeOrderingDescriptor(
      const OrderingRecordDescriptor& descriptor) const {
    RecordSorterDescriptor runtime{
        .field_count = descriptor.field_count,
        .key_field_count = descriptor.key_field_count,
    };
    const bool legacy_index = descriptor.index_refill_cursor.has_value() &&
                              record_options_.schema_format != DatabaseSchemaFormat::kFour;
    runtime.key_columns = RuntimeOrderingColumns(descriptor.key_columns, legacy_index);
    runtime.record_options = record_options_;
    return runtime;
  }

  [[nodiscard]] EphemeralRelationDescriptor RuntimeRelationDescriptor(
      const OrderingRecordDescriptor& descriptor) const {
    EphemeralRelationDescriptor runtime{
        .field_count = descriptor.field_count,
        .key_field_count = descriptor.key_field_count,
        .purpose = descriptor.relation_purpose == RelationPurpose::kLookup
                       ? EphemeralRelationPurpose::kLookup
                       : EphemeralRelationPurpose::kMembership,
    };
    runtime.key_columns = RuntimeOrderingColumns(descriptor.key_columns);
    runtime.record_options = record_options_;
    return runtime;
  }

  [[nodiscard]] DispatchResult Dispatch(std::uint32_t address, const Instruction& instruction) {
    return std::visit(
        [this, address](const auto& operation) { return Execute(address, operation); },
        instruction);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const LoadConstantInstruction& operation) {
    SqlValueAccess::BorrowStable(registers_[operation.output.value()],
                                 program_->constant(operation.constant));
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const LoadParameterInstruction& operation) {
    SqlValueAccess::BorrowStable(registers_[operation.output.value()],
                                 parameters_[operation.parameter.value()]);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CopyInstruction& operation) {
    return CopyRegister(operation.output, Register(operation.input));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const EncodeRecordInstruction& operation) {
    const auto values = std::span<const SqlValue>{registers_}.subspan(operation.first_value.value(),
                                                                      operation.value_count);
    auto size = EncodedRecordSize(values, record_options_);
    if (!size.has_value()) {
      return std::unexpected(std::move(size.error()));
    }
    if (size->value() > limits_.maximum_value_bytes) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "encoded record exceeds value limit"));
    }
    SqlValue& output = registers_[operation.output.value()];
    auto destination = SqlValueAccess::PrepareBytes(output, *size);
    if (!destination.has_value()) {
      return std::unexpected(std::move(destination.error()));
    }
    auto encoded = EncodeRecordInto(values, *destination, record_options_);
    if (!encoded.has_value()) {
      return std::unexpected(std::move(encoded.error()));
    }
    SqlValueAccess::CommitBytes(output, *encoded, SqlValueType::kBlob);
    return std::nullopt;
  }

  void MoveRegisters(RegisterId first_input, RegisterId first_output,
                     std::uint32_t count) noexcept {
    for (std::uint32_t index = 0; index < count; ++index) {
      registers_[first_output.value() + index] = std::move(registers_[first_input.value() + index]);
    }
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const MoveInstruction& operation) {
    MoveRegisters(operation.first_input, operation.first_output, operation.count);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const UnaryInstruction& operation) {
    const SqlValue& input = Register(operation.input);
    switch (operation.operation) {
      case UnaryOperation::kNegate: {
        std::optional<SqlValue> converted;
        const SqlValue& numeric = SqlValueAccess::NumericOperand(
            input, converted, SqlValueAccess::NumericOperandPolicy::Arithmetic);
        if (numeric.type() == SqlValueType::kNull) {
          return SetNullRegister(operation.output);
        }
        if (const auto integer = numeric.integer_value(); integer.has_value()) {
          if (*integer == std::numeric_limits<std::int64_t>::min()) {
            return SetRegister(operation.output, -static_cast<double>(*integer));
          }
          return SetRegister(operation.output, -*integer);
        }
        return SetRegister(operation.output, -SqlValueAccess::Real(numeric));
      }
      case UnaryOperation::kBitwiseNot:
        if (input.type() == SqlValueType::kNull) {
          return SetNullRegister(operation.output);
        }
        return SetRegister(operation.output, BitwiseNot(NumericAsInteger(input)));
      case UnaryOperation::kLogicalNot: {
        const SqlTruthValue truth = TruthValue(input);
        if (truth == SqlTruthValue::kNull) {
          return SetNullRegister(operation.output);
        }
        return SetRegister(operation.output,
                           static_cast<std::int64_t>(truth == SqlTruthValue::kFalse));
      }
    }
    return std::unexpected(VmError(ErrorCode::kInternal, "unknown unary operation"));
  }

  [[nodiscard]] DispatchResult ExecuteArithmetic(const BinaryInstruction& operation) {
    const SqlValue& original_left = Register(operation.left);
    const SqlValue& original_right = Register(operation.right);
    if (original_left.type() == SqlValueType::kNull ||
        original_right.type() == SqlValueType::kNull) {
      return SetNullRegister(operation.output);
    }
    std::optional<SqlValue> converted_left;
    std::optional<SqlValue> converted_right;
    const SqlValue& left = SqlValueAccess::NumericOperand(
        original_left, converted_left, SqlValueAccess::NumericOperandPolicy::Arithmetic);
    const SqlValue& right = SqlValueAccess::NumericOperand(
        original_right, converted_right, SqlValueAccess::NumericOperandPolicy::Arithmetic);

    const auto left_integer = left.integer_value();
    const auto right_integer = right.integer_value();
    if (operation.operation == BinaryOperation::kRemainder) {
      const std::int64_t divisor = NumericAsInteger(right);
      if (divisor == 0) {
        return SetNullRegister(operation.output);
      }
      const std::int64_t dividend = NumericAsInteger(left);
      const std::int64_t remainder =
          dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1
              ? 0
              : dividend % divisor;
      if (left.type() == SqlValueType::kReal || right.type() == SqlValueType::kReal) {
        return SetRegister(operation.output, static_cast<double>(remainder));
      }
      return SetRegister(operation.output, remainder);
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
            return SetNullRegister(operation.output);
          }
          if (*left_integer != std::numeric_limits<std::int64_t>::min() || *right_integer != -1) {
            return SetRegister(operation.output, *left_integer / *right_integer);
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
          return std::unexpected(VmError(ErrorCode::kInternal, "unknown arithmetic operation"));
      }
      if (checked.has_value()) {
        return SetRegister(operation.output, *checked);
      }
    }

    const double left_real = NumericAsDouble(left);
    const double right_real = NumericAsDouble(right);
    switch (operation.operation) {
      case BinaryOperation::kAdd:
        return SetRegister(operation.output, left_real + right_real);
      case BinaryOperation::kSubtract:
        return SetRegister(operation.output, left_real - right_real);
      case BinaryOperation::kMultiply:
        return SetRegister(operation.output, left_real * right_real);
      case BinaryOperation::kDivide:
        if (right_real == 0.0) {
          return SetNullRegister(operation.output);
        }
        return SetRegister(operation.output, left_real / right_real);
      case BinaryOperation::kRemainder:
      case BinaryOperation::kConcatenate:
      case BinaryOperation::kBitwiseAnd:
      case BinaryOperation::kBitwiseOr:
      case BinaryOperation::kShiftLeft:
      case BinaryOperation::kShiftRight:
      case BinaryOperation::kLogicalAnd:
      case BinaryOperation::kLogicalOr:
        return std::unexpected(VmError(ErrorCode::kInternal, "unknown arithmetic operation"));
    }
    return std::unexpected(VmError(ErrorCode::kInternal, "unknown arithmetic operation"));
  }

  [[nodiscard]] DispatchResult ExecuteConcatenate(const BinaryInstruction& operation) {
    const SqlValue& left = Register(operation.left);
    const SqlValue& right = Register(operation.right);
    SqlValue output;
    if (left.type() == SqlValueType::kNull || right.type() == SqlValueType::kNull) {
      return SetNullRegister(operation.output);
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
    if (left.type() == SqlValueType::kNull || right.type() == SqlValueType::kNull) {
      return SetNullRegister(operation.output);
    }
    const std::int64_t left_integer = NumericAsInteger(left);
    const std::int64_t right_integer = NumericAsInteger(right);
    switch (operation.operation) {
      case BinaryOperation::kBitwiseAnd:
        return SetRegister(operation.output, BitwiseAnd(left_integer, right_integer));
      case BinaryOperation::kBitwiseOr:
        return SetRegister(operation.output, BitwiseOr(left_integer, right_integer));
      case BinaryOperation::kShiftLeft:
        return SetRegister(
            operation.output,
            ShiftInteger(ShiftArguments{.value = left_integer, .count = right_integer}, true));
      case BinaryOperation::kShiftRight:
        return SetRegister(
            operation.output,
            ShiftInteger(ShiftArguments{.value = left_integer, .count = right_integer}, false));
      case BinaryOperation::kAdd:
      case BinaryOperation::kSubtract:
      case BinaryOperation::kMultiply:
      case BinaryOperation::kDivide:
      case BinaryOperation::kRemainder:
      case BinaryOperation::kConcatenate:
      case BinaryOperation::kLogicalAnd:
      case BinaryOperation::kLogicalOr:
        return std::unexpected(VmError(ErrorCode::kInternal, "unknown bitwise operation"));
    }
    return std::unexpected(VmError(ErrorCode::kInternal, "unknown bitwise operation"));
  }

  [[nodiscard]] DispatchResult ExecuteLogical(const BinaryInstruction& operation) {
    return SetRegister(
        operation.output,
        LogicalBinary(TruthValue(Register(operation.left)), TruthValue(Register(operation.right)),
                      operation.operation == BinaryOperation::kLogicalAnd));
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
    return SetRegister(operation.output, *integer);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const RealAffinityInstruction& operation) {
    const SqlValue& input = Register(operation.input);
    if (input.type() == SqlValueType::kInteger) {
      return SetRegister(operation.output, static_cast<double>(SqlValueAccess::Integer(input)));
    }
    return CopyRegister(operation.output, input);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const RealStorageAffinityInstruction& operation) {
    return SetRegister(operation.output,
                       ApplyRealStorageAffinity(Register(operation.input).Clone()));
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
    return SetRegister(operation.output, comparison);
  }

  [[nodiscard]] Result<const ResolvedCall*> ResolvedScalarCall(std::uint32_t address) const {
    const auto iterator =
        std::ranges::lower_bound(resolved_calls_, address, {}, &ResolvedCall::address);
    if (iterator == resolved_calls_.end() || iterator->address != address) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "resolved scalar function call is missing"));
    }
    return &*iterator;
  }

  [[nodiscard]] DispatchResult InvokeScalar(const ResolvedCall& call,
                                            std::span<const SqlValue> arguments,
                                            RegisterId output) {
    const ScalarFunctionContext context(*call.collation);
    auto value = call.function->Invoke(context, arguments);
    if (!value.has_value()) {
      return std::unexpected(std::move(value.error()));
    }
    return SetRegister(output, std::move(*value));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t address,
                                       const CallScalarInstruction& operation) {
    auto call = ResolvedScalarCall(address);
    if (!call.has_value()) {
      return std::unexpected(std::move(call.error()));
    }
    const std::span<const SqlValue> arguments =
        operation.argument_count == 0U
            ? std::span<const SqlValue>{}
            : std::span<const SqlValue>{registers_}.subspan(operation.first_argument.value(),
                                                            operation.argument_count);
    return InvokeScalar(**call, arguments, operation.output);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t address,
                                       const CallColumnScalarInstruction& operation) {
    auto call = ResolvedScalarCall(address);
    if (!call.has_value()) {
      return std::unexpected(std::move(call.error()));
    }
    ColumnFunctionConsumer consumer{
        .function = *(*call)->function,
        .output = operation.output,
        .real_affinity = operation.real_affinity,
    };
    auto read = Execute(
        address,
        ReadFieldInstruction{
            .cursor = operation.cursor, .field = operation.field, .output = operation.argument},
        &consumer);
    if (!read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    if (consumer.completed) {
      return std::nullopt;
    }
    if (operation.real_affinity) {
      auto applied = Execute(address, RealAffinityInstruction{.input = operation.argument,
                                                              .output = operation.argument});
      if (!applied.has_value()) {
        return std::unexpected(std::move(applied.error()));
      }
    }
    return InvokeScalar(
        **call, std::span<const SqlValue>{registers_}.subspan(operation.argument.value(), 1),
        operation.output);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const OpenAggregateInstruction& operation) {
    RuntimeAggregate& runtime = aggregates_[operation.aggregate.value()];
    if (runtime.function == nullptr || runtime.collation == nullptr || runtime.state.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "aggregate runtime cannot be opened"));
    }
    runtime.state.emplace(runtime.function->CreateState());
    MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(instrumentation::AggregateWorkMetric::Open);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const StepAggregateInstruction& operation) {
    RuntimeAggregate& runtime = aggregates_[operation.aggregate.value()];
    if (runtime.function == nullptr || runtime.collation == nullptr || !runtime.state.has_value() ||
        runtime.state->finalized()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "aggregate runtime is not open"));
    }
    const AggregateDescriptor& descriptor = program_->aggregate(operation.aggregate);
    const std::span<const SqlValue> arguments =
        descriptor.argument_count == 0U
            ? std::span<const SqlValue>{}
            : std::span<const SqlValue>{registers_}.subspan(operation.first_argument.value(),
                                                            descriptor.argument_count);
    const AggregateFunctionContext context(*runtime.collation,
                                           ByteCount{limits_.maximum_value_bytes});
    auto effect = runtime.function->Step(*runtime.state, context, arguments);
    if (!effect.has_value()) {
      return std::unexpected(std::move(effect.error()));
    }
    MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(instrumentation::AggregateWorkMetric::Step);
    if (!operation.effect_output.has_value()) {
      return std::nullopt;
    }
    BytecodeAggregateStepEffect output = BytecodeAggregateStepEffect::kNoRepresentativePreference;
    switch (*effect) {
      case AggregateStepEffect::kNoRepresentativePreference:
        break;
      case AggregateStepEffect::kKeepRepresentative:
        output = BytecodeAggregateStepEffect::kKeepRepresentative;
        break;
      case AggregateStepEffect::kReplaceRepresentative:
        output = BytecodeAggregateStepEffect::kReplaceRepresentative;
        break;
    }
    return SetRegister(*operation.effect_output, static_cast<std::int64_t>(output));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const FinalizeAggregateInstruction& operation) {
    RuntimeAggregate& runtime = aggregates_[operation.aggregate.value()];
    if (runtime.function == nullptr || !runtime.state.has_value() || runtime.state->finalized()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "aggregate runtime is not open"));
    }
    auto result = runtime.function->Finalize(*runtime.state);
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(instrumentation::AggregateWorkMetric::Finalize);
    return SetRegister(operation.output, std::move(*result));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ResetAggregateInstruction& operation) {
    RuntimeAggregate& runtime = aggregates_[operation.aggregate.value()];
    if (!runtime.state.has_value() || !runtime.state->finalized()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "aggregate runtime is not open"));
    }
    runtime.state->Reset();
    MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(instrumentation::AggregateWorkMetric::Reset);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CloseAggregateInstruction& operation) {
    RuntimeAggregate& runtime = aggregates_[operation.aggregate.value()];
    if (!runtime.state.has_value() || !runtime.state->finalized()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "aggregate runtime is not open"));
    }
    runtime.state.reset();
    MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(instrumentation::AggregateWorkMetric::Close);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const JumpInstruction& operation) {
    program_counter_ = operation.target.value();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CallSubroutineInstruction& operation) {
    subroutine_continuations_.push_back(SubroutineContinuation{
        .subroutine = operation.subroutine,
        .return_address = InstructionAddress{program_counter_},
    });
    program_counter_ = program_->subroutine(operation.subroutine).entry.value();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ReturnSubroutineInstruction&) {
    program_counter_ = subroutine_continuations_.back().return_address.value();
    subroutine_continuations_.pop_back();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const InitCoroutineInstruction& operation) {
    auto& coroutine = coroutines_[operation.coroutine.value()];
    if (coroutine.phase == CoroutinePhase::Running ||
        coroutine.phase == CoroutinePhase::Suspended) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "cannot initialize a live coroutine"));
    }
    coroutine = CoroutineContinuation{
        .resume_address = program_->coroutine(operation.coroutine).entry,
        .phase = CoroutinePhase::Suspended,
    };
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ResumeCoroutineInstruction& operation) {
    auto& coroutine = coroutines_[operation.coroutine.value()];
    if (coroutine.phase == CoroutinePhase::Ended) {
      program_counter_ = operation.exhausted_target.value();
      return std::nullopt;
    }
    if (coroutine.phase != CoroutinePhase::Suspended) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "coroutine is not resumable"));
    }
    coroutine.return_address = InstructionAddress{program_counter_};
    coroutine.exhausted_target = operation.exhausted_target;
    coroutine.phase = CoroutinePhase::Running;
    program_counter_ = coroutine.resume_address.value();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t address,
                                       const YieldCoroutineInstruction& operation) {
    auto& coroutine = coroutines_[operation.coroutine.value()];
    const auto& descriptor = program_->coroutine(operation.coroutine);
    if (coroutine.phase != CoroutinePhase::Running || address < descriptor.entry.value() ||
        address >= descriptor.end.value()) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "coroutine cannot yield here"));
    }
    const ResumeCoroutineInstruction* receiver = nullptr;
    const std::uint32_t returned = coroutine.return_address.value();
    if (returned != 0U && returned <= program_->instructions().size()) {
      receiver = std::get_if<ResumeCoroutineInstruction>(&program_->instructions()[returned - 1U]);
    }
    if (receiver == nullptr || receiver->coroutine != operation.coroutine) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "coroutine continuation is not a matching resume"));
    }
    if (receiver->first_destination.has_value()) {
      MoveRegisters(descriptor.first_output, *receiver->first_destination, descriptor.output_count);
    }
    coroutine.resume_address = InstructionAddress{program_counter_};
    coroutine.phase = CoroutinePhase::Suspended;
    program_counter_ = coroutine.return_address.value();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t address,
                                       const EndCoroutineInstruction& operation) {
    auto& coroutine = coroutines_[operation.coroutine.value()];
    const auto& descriptor = program_->coroutine(operation.coroutine);
    if (coroutine.phase != CoroutinePhase::Running || address < descriptor.entry.value() ||
        address >= descriptor.end.value()) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "coroutine cannot end here"));
    }
    coroutine.phase = CoroutinePhase::Ended;
    program_counter_ = coroutine.exhausted_target.value();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CancelCoroutineInstruction& operation) {
    for (std::size_t index = 0; index < coroutines_.size(); ++index) {
      if (program_->coroutine_contains(operation.coroutine,
                                       CoroutineId{static_cast<std::uint32_t>(index)}) &&
          coroutines_[index].phase == CoroutinePhase::Running) {
        return std::unexpected(VmError(ErrorCode::kMisuse, "cannot cancel a running coroutine"));
      }
    }
    CloseCapabilities(operation.coroutine);
    for (std::size_t index = 0; index < coroutines_.size(); ++index) {
      if (program_->coroutine_contains(operation.coroutine,
                                       CoroutineId{static_cast<std::uint32_t>(index)})) {
        coroutines_[index] = CoroutineContinuation{.phase = CoroutinePhase::Cancelled};
      }
    }
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
      case JumpCondition::kIfTrueOrNull:
        should_jump = TruthValue(input) != SqlTruthValue::kFalse;
        break;
      case JumpCondition::kIfFalseOrNull:
        should_jump = TruthValue(input) != SqlTruthValue::kTrue;
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
    runtime.column_cache.bytes.reset();
    if (descriptor.storage == CursorStorageKind::kRowIdTable) {
      auto cursor = TableBtreeCursor::Open(*database_, PageNumber(descriptor.root_page.value()));
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      runtime.storage = std::move(*cursor);
    } else if (descriptor.storage == CursorStorageKind::kEphemeralIndex) {
      if (!descriptor.relation.has_value()) {
        return std::unexpected(
            VmError(ErrorCode::kInternal, "ephemeral source has no relation owner"));
      }
      const RelationId relation_id = *descriptor.relation;
      auto relation = Relation(relation_id);
      if (!relation.has_value()) {
        return std::unexpected(std::move(relation.error()));
      }
      runtime.storage = relation_id;
    } else {
      auto info = RuntimeIndexInfo(descriptor.index_columns, descriptor.index_key_field_count);
      if (!info.has_value()) {
        return std::unexpected(std::move(info.error()));
      }
      auto cursor = IndexBtreeCursor::Open(*database_, PageNumber(descriptor.root_page.value()),
                                           std::move(*info));
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      runtime.storage = std::move(*cursor);
    }
    auto prepared = runtime.columns.Prepare(descriptor.record_field_count);
    if (!prepared.has_value()) {
      return std::unexpected(std::move(prepared.error()));
    }
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
    runtime.column_cache.bytes.reset();
    auto table = writer_->OpenTableBtree(PageNumber(descriptor.root_page.value()));
    if (!table.has_value()) {
      return std::unexpected(std::move(table.error()));
    }
    auto cursor = table->OpenMutationCursor();
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    runtime.storage = std::move(*cursor);
    auto prepared = runtime.columns.Prepare(descriptor.record_field_count);
    if (!prepared.has_value()) {
      return std::unexpected(std::move(prepared.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CloseCursorInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    runtime.column_cache.bytes.reset();
    if (const auto* id = std::get_if<RelationId>(&runtime.storage); id != nullptr) {
      auto relation = Relation(*id);
      if (!relation.has_value()) {
        return std::unexpected(std::move(relation.error()));
      }
      auto cleared = (*relation)->ClearPosition();
      if (!cleared.has_value()) {
        return std::unexpected(std::move(cleared.error()));
      }
    }
    runtime.storage = std::monostate{};
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const NullRowInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    auto cleared = std::visit(
        [this](auto& cursor) -> Status {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, TableBtreeCursor> ||
                        std::is_same_v<T, IndexBtreeCursor>) {
            return cursor.ClearPosition();
          } else if constexpr (std::is_same_v<T, RelationId>) {
            auto relation = Relation(cursor);
            if (!relation.has_value()) {
              return std::unexpected(std::move(relation.error()));
            }
            return (*relation)->ClearPosition();
          } else {
            return std::unexpected(
                VmError(ErrorCode::kInternal, "NULL-row publication requires an open read cursor"));
          }
        },
        runtime.storage);
    if (!cleared.has_value()) {
      return std::unexpected(std::move(cleared.error()));
    }
    runtime.row_state = operation.enabled ? RuntimeCursor::RowState::VirtualNull
                                          : RuntimeCursor::RowState::Ordinary;
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const OpenWriteCursorInstruction& operation) {
    if (writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "write cursor requires a transaction writer"));
    }
    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    if (descriptor.pending_root) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "dynamic write cursor requires root creation"));
    }
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (descriptor.storage == WriteCursorStorageKind::kRowIdTable) {
      auto table = writer_->OpenTableBtree(PageNumber(descriptor.root_page.value()));
      if (!table.has_value()) {
        return std::unexpected(std::move(table.error()));
      }
      runtime.table = std::move(*table);
    } else {
      const std::size_t native_key_count =
          descriptor.unique_not_null ? descriptor.key_term_count : descriptor.index_columns.size();
      auto info = RuntimeIndexInfo(descriptor.index_columns, native_key_count);
      if (!info.has_value()) {
        return std::unexpected(std::move(info.error()));
      }
      auto index =
          writer_->OpenIndexBtree(PageNumber(descriptor.root_page.value()), std::move(*info));
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
    runtime.index_refill.reset();
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
      return SetRegister(operation.output, *rowid);
    }

    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    PageNumber root_page{descriptor.root_page.value()};
    if (descriptor.pending_root) {
      RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
      if (!runtime.table.has_value()) {
        return std::unexpected(
            VmError(ErrorCode::kInternal, "generated rowid used a closed dynamic table cursor"));
      }
      root_page = runtime.table->root_page();
    }
    auto cursor = TableBtreeCursor::Open(*database_, root_page);
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    auto has_last = cursor->Last();
    if (!has_last.has_value()) {
      return std::unexpected(std::move(has_last.error()));
    }
    if (!*has_last) {
      return SetRegister(operation.output, std::int64_t{1});
    }
    auto maximum = cursor->rowid();
    if (!maximum.has_value()) {
      return std::unexpected(std::move(maximum.error()));
    }
    if (*maximum < std::numeric_limits<std::int64_t>::max()) {
      return SetRegister(operation.output, *maximum + 1);
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
        return SetRegister(operation.output, candidate);
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
      SqlValue value;
      if (!column.rowid_alias) {
        value =
            Register(RegisterId(operation.first_value.value() + static_cast<std::uint32_t>(index)))
                .Clone();
        if (operation.affinity_mode == TableRecordAffinityMode::kApplyColumnAffinity) {
          auto applied =
              ApplyTableRecordAffinity(std::move(value), column.affinity, record_options_);
          if (!applied.has_value()) {
            return std::unexpected(std::move(applied.error()));
          }
          value = std::move(*applied);
        }
      }
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
    auto cursor = TableBtreeCursor::Open(*database_, PageNumber(descriptor.root_page.value()));
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
                                       const CheckUpdateRowIdInstruction& operation) {
    const std::optional<std::int64_t> old_rowid = LosslessRowId(Register(operation.old_rowid));
    const std::optional<std::int64_t> new_rowid = LosslessRowId(Register(operation.new_rowid));
    if (!old_rowid.has_value() || !new_rowid.has_value()) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
    }
    if (*old_rowid == *new_rowid) {
      return std::nullopt;
    }
    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    auto cursor = TableBtreeCursor::Open(*database_, PageNumber(descriptor.root_page.value()));
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    auto found = cursor->Seek(*new_rowid, BtreeSeekMode::kEqual);
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

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const InsertSorterIndexInstruction& operation) {
    std::optional<RecordSorter>& sorter = sorters_[operation.sorter.value()];
    const OrderingRecordDescriptor& ordering = program_->sorter(operation.sorter);
    if (!sorter.has_value() || !ordering.index_refill_cursor.has_value()) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "index refill sorter is not open"));
    }
    RuntimeWriteCursor& runtime = WriteCursor(*ordering.index_refill_cursor);
    if (!runtime.index_refill.has_value()) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "index append cursor is not open"));
    }
    auto current = sorter->current_record();
    if (!current.has_value()) {
      return std::unexpected(std::move(current.error()));
    }
    RuntimeIndexRefill& refill = *runtime.index_refill;
    const WriteCursorDescriptor& descriptor = program_->write_cursor(*ordering.index_refill_cursor);
    auto rowid_field = current->field(descriptor.key_term_count);
    if (!rowid_field.has_value()) {
      return std::unexpected(std::move(rowid_field.error()));
    }
    const std::optional<std::int64_t> rowid = rowid_field->integer_value();
    if (refill.previous_size != 0U) {
      auto previous = RecordView::Parse(refill.previous_record.view().first(refill.previous_size),
                                        record_options_);
      if (!previous.has_value()) {
        return std::unexpected(std::move(previous.error()));
      }
      auto compared = sorter->CompareCurrent(*previous);
      if (!compared.has_value()) {
        return std::unexpected(std::move(compared.error()));
      }
      if (*compared == std::weak_ordering::equivalent) {
        if (descriptor.unique) {
          bool contains_null = false;
          RecordCursor fields = current->cursor();
          for (std::uint32_t term = 0; term < descriptor.key_term_count; ++term) {
            const std::optional<RecordFieldView> field = fields.Next();
            if (!field.has_value()) {
              return std::unexpected(
                  VmError(ErrorCode::kCorruption, "index refill key field is missing"));
            }
            contains_null = contains_null || field->type() == SqlValueType::kNull;
          }
          if (!contains_null) {
            return std::unexpected(VmError(ErrorCode::kConstraint, "UNIQUE constraint failed"));
          }
        }
        auto previous_rowid = previous->field(descriptor.key_term_count);
        if (!previous_rowid.has_value()) {
          return std::unexpected(std::move(previous_rowid.error()));
        }
        const std::optional<std::int64_t> previous_integer = previous_rowid->integer_value();
        if (!rowid.has_value() || !previous_integer.has_value() || *rowid <= *previous_integer) {
          return std::unexpected(
              VmError(ErrorCode::kMisuse, "index refill physical keys are not increasing"));
        }
      } else if (*compared == std::weak_ordering::less) {
        return std::unexpected(
            VmError(ErrorCode::kMisuse, "index refill logical keys are not increasing"));
      }
    }
    if (!rowid.has_value()) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "index refill rowid is not an integer"));
    }
    const ByteView encoded = current->encoded();
    if (encoded.size() > limits_.maximum_value_bytes) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "index refill record exceeds the VM value limit"));
    }
    auto retained = SqlValueAccess::RetainBytes(refill.previous_record, encoded);
    if (!retained.has_value()) {
      return std::unexpected(std::move(retained.error()));
    }
    refill.previous_size = encoded.size();
    auto record = RecordView::Parse(refill.previous_record.view().first(refill.previous_size),
                                    record_options_);
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    auto appended = refill.cursor.AppendSorted(*record);
    if (!appended.has_value()) {
      return std::unexpected(std::move(appended.error()));
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
    ++column_write_counter_;
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
    ++column_write_counter_;
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
    if (cursor == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "current delete used an invalid mutation cursor"));
    }
    auto has_next = cursor->DeleteAndNext();
    ++column_write_counter_;
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
    if (cursor == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "current update used an invalid mutation cursor"));
    }
    const std::optional<ByteView> record = Register(operation.record).blob_value();
    if (!record.has_value()) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "datatype mismatch"));
    }
    Status updated = cursor->ReplaceCurrent(*record);
    ++column_write_counter_;
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
      auto cursor = TableBtreeCursor::Open(*database_, PageNumber(descriptor.root_page.value()));
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
      ++column_write_counter_;
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
      ++column_write_counter_;
      Status inserted = runtime.table->Insert(*new_rowid, *record, BtreeInsertMode::kInsertOnly);
      ++column_write_counter_;
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
    const std::int64_t root_page = static_cast<std::int64_t>(table->root_page().value());
    if (operation.cursor.has_value()) {
      WriteCursor(*operation.cursor).table = std::move(*table);
    }
    return SetRegister(operation.output, root_page);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CreateIndexRootInstruction& operation) {
    if (writer_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "index root creation requires a transaction writer"));
    }
    const WriteCursorDescriptor& descriptor = program_->write_cursor(operation.cursor);
    const std::size_t native_key_count =
        descriptor.unique_not_null ? descriptor.key_term_count : descriptor.index_columns.size();
    auto info = RuntimeIndexInfo(descriptor.index_columns, native_key_count, true);
    if (!info.has_value()) {
      return std::unexpected(std::move(info.error()));
    }
    auto index = writer_->CreateIndexBtree(std::move(*info));
    if (!index.has_value()) {
      return std::unexpected(std::move(index.error()));
    }
    const std::int64_t root_page = static_cast<std::int64_t>(index->root_page().value());
    WriteCursor(operation.cursor).index = std::move(*index);
    return SetRegister(operation.output, root_page);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ClearStat1Instruction& operation) {
    RuntimeWriteCursor& runtime = WriteCursor(operation.cursor);
    if (!runtime.table.has_value()) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "sqlite_stat1 clear used a closed table cursor"));
    }
    if (operation.scope == Stat1ClearScope::kDatabase) {
      auto cleared = runtime.table->Clear();
      if (!cleared.has_value()) {
        return std::unexpected(std::move(cleared.error()));
      }
      return std::nullopt;
    }
    if (!operation.name.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sqlite_stat1 clear name is missing"));
    }
    const std::optional<Utf8View> name = Register(*operation.name).text_value();
    if (!name.has_value()) {
      return std::unexpected(
          VmError(ErrorCode::kTypeMismatch, "sqlite_stat1 clear name is not text"));
    }
    std::vector<std::int64_t> rowids;
    {
      auto cursor = TableBtreeCursor::Open(*database_, runtime.table->root_page());
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      auto has_row = cursor->First();
      if (!has_row.has_value()) {
        return std::unexpected(std::move(has_row.error()));
      }
      while (*has_row) {
        auto payload = cursor->CopyPayload();
        if (!payload.has_value()) {
          return std::unexpected(std::move(payload.error()));
        }
        if (payload->size().value() > limits_.maximum_value_bytes) {
          return std::unexpected(VmError(ErrorCode::kTooLarge, "sqlite_stat1 row is too large"));
        }
        auto fields = DecodeRecord(payload->view(), record_options_);
        if (!fields.has_value()) {
          return std::unexpected(std::move(fields.error()));
        }
        if (fields->size() < 2U) {
          return std::unexpected(
              VmError(ErrorCode::kCorruption, "sqlite_stat1 row has too few fields"));
        }
        const std::size_t field = operation.scope == Stat1ClearScope::kTable ? 0U : 1U;
        const std::optional<Utf8View> value = (*fields)[field].text_value();
        if (value.has_value() && value->bytes() == name->bytes()) {
          if (rowids.size() >= limits_.maximum_value_bytes / sizeof(std::int64_t)) {
            return std::unexpected(
                VmError(ErrorCode::kTooLarge, "sqlite_stat1 clear rowids exceed the VM limit"));
          }
          auto rowid = cursor->rowid();
          if (!rowid.has_value()) {
            return std::unexpected(std::move(rowid.error()));
          }
          rowids.push_back(*rowid);
        }
        has_row = cursor->Next();
        if (!has_row.has_value()) {
          return std::unexpected(std::move(has_row.error()));
        }
      }
    }
    for (const std::int64_t rowid : rowids) {
      auto deleted = runtime.table->Delete(rowid);
      if (!deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
      }
      if (!*deleted) {
        return std::unexpected(
            VmError(ErrorCode::kCorruption, "sqlite_stat1 row disappeared during clear"));
      }
      ++column_write_counter_;
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const ComputeIndexStat1Instruction& operation) {
    const ReadCursorDescriptor& descriptor = program_->cursor(operation.cursor);
    if (descriptor.index_columns.size() >
        limits_.maximum_value_bytes /
            (sizeof(IndexColumnOrder) + SqlValueAccess::kOwnedStorageBytes)) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "ANALYZE index metadata exceeds the VM limit"));
    }
    auto info = RuntimeIndexInfo(descriptor.index_columns, descriptor.index_key_field_count);
    if (!info.has_value()) {
      return std::unexpected(std::move(info.error()));
    }
    const auto columns = info->columns();
    auto cursor =
        IndexBtreeCursor::Open(*database_, PageNumber(descriptor.root_page.value()), *info);
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    // Native unlimited STAT1 still performs Count before stat_init and Rewind.
    auto counted = cursor->CountEntries();
    if (!counted.has_value()) {
      return std::unexpected(std::move(counted.error()));
    }
    const std::size_t key_count = operation.key_term_count;
    const std::size_t physical_columns = operation.policy == Stat1IndexPolicy::PrimaryWithoutRowid
                                             ? key_count
                                             : descriptor.index_columns.size();
    const std::size_t tested_columns =
        operation.policy == Stat1IndexPolicy::UniqueNotNull ||
                operation.policy == Stat1IndexPolicy::PrimaryWithoutRowid
            ? key_count - 1U
            : physical_columns - 1U;
    Stat1Accumulator state{physical_columns};
    auto has_row = cursor->First();
    if (!has_row.has_value()) {
      return std::unexpected(std::move(has_row.error()));
    }
    if (*has_row &&
        key_count > limits_.maximum_value_bytes /
                        (sizeof(std::uint64_t) + 2U * SqlValueAccess::kOwnedStorageBytes)) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "ANALYZE prefix state exceeds the VM limit"));
    }
    std::vector<Stat1PreviousField> previous(*has_row ? tested_columns : 0U);
    ByteBuffer scratch;
    while (*has_row) {
      {
        auto current = cursor->ReadRecordPrefix(std::max<std::size_t>(tested_columns, 1U), scratch,
                                                ByteCount{limits_.maximum_value_bytes});
        if (!current.has_value()) {
          return std::unexpected(std::move(current.error()));
        }
        if (current->complete_field_count() < key_count) {
          return std::unexpected(
              VmError(ErrorCode::kCorruption, "ANALYZE index row has too few fields"));
        }
        const bool unique_shortcut = operation.policy == Stat1IndexPolicy::UniqueNullable &&
                                     key_count == 1U && tested_columns == 1U && state.rows != 0U &&
                                     previous.front().value.type() != SqlValueType::kNull;
        std::size_t first_changed = 0;
        if (!unique_shortcut) {
          auto retained = RetainStat1ChangedSuffix(*current, previous, columns, state.rows == 0U);
          if (!retained.has_value()) {
            return std::unexpected(std::move(retained.error()));
          }
          first_changed = *retained;
        }
        auto pushed = state.Push(first_changed);
        if (!pushed.has_value()) {
          return std::unexpected(std::move(pushed.error()));
        }
      }
      has_row = cursor->Next();
      if (!has_row.has_value()) {
        return std::unexpected(std::move(has_row.error()));
      }
    }

    auto result = state.ResultText(key_count, operation.emit_empty, limits_.maximum_value_bytes);
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    return SetRegister(operation.output, std::move(*result));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const ComputeTableStat1Instruction& operation) {
    const ReadCursorDescriptor& descriptor = program_->cursor(operation.cursor);
    auto cursor = TableBtreeCursor::Open(*database_, PageNumber(descriptor.root_page.value()));
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    auto rows = cursor->CountEntries();
    if (!rows.has_value()) {
      return std::unexpected(std::move(rows.error()));
    }
    if (*rows == 0U) {
      return SetNullRegister(operation.output);
    }
    return SetRegister(operation.output, SqlValue::Text(std::to_string(*rows)));
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
    return SetRegister(operation.output, static_cast<std::int64_t>(*cookie));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const RewindInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    auto has_entry = std::visit(
        [](auto& cursor) -> Result<bool> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, RelationId>) {
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
    auto ready = PrepareCursorStep(runtime);
    if (!ready.has_value()) {
      return std::unexpected(std::move(ready.error()));
    }
    if (!*ready) {
      runtime.ClearRecordCache();
      return std::nullopt;
    }
    runtime.ClearRecordCache();
    auto has_entry = std::visit(
        [](auto& cursor) -> Result<bool> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, RelationId>) {
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

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const LastInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    runtime.ClearRecordCache();
    auto has_entry = std::visit(
        [](auto& cursor) -> Result<bool> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            return std::unexpected(VmError(ErrorCode::kInternal, "last used a closed cursor"));
          } else if constexpr (std::is_same_v<T, TableBtreeMutationCursor> ||
                               std::is_same_v<T, RelationId>) {
            return std::unexpected(VmError(ErrorCode::kInternal, "last used a mutation cursor"));
          } else {
            return cursor.Last();
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

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const PreviousInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    auto ready = PrepareCursorStep(runtime);
    if (!ready.has_value()) {
      return std::unexpected(std::move(ready.error()));
    }
    if (!*ready) {
      runtime.ClearRecordCache();
      return std::nullopt;
    }
    runtime.ClearRecordCache();
    auto has_entry = std::visit(
        [](auto& cursor) -> Result<bool> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            return std::unexpected(VmError(ErrorCode::kInternal, "previous used a closed cursor"));
          } else if constexpr (std::is_same_v<T, TableBtreeMutationCursor> ||
                               std::is_same_v<T, RelationId>) {
            return std::unexpected(
                VmError(ErrorCode::kInternal, "previous used a mutation cursor"));
          } else {
            return cursor.Previous();
          }
        },
        runtime.storage);
    if (!has_entry.has_value()) {
      return std::unexpected(std::move(has_entry.error()));
    }
    if (*has_entry) {
      program_counter_ = operation.previous_target.value();
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
    return SetRegister(operation.output, rowid_list_.front());
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
    if (auto stored = SetRegister(operation.output, rowid_list_[rowid_list_index_]);
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
    const RowIdSeekTarget target = ConvertRowIdSeek(Register(operation.key), operation.mode);
    if (target.disposition == RowIdSeekDisposition::kMissing) {
      program_counter_ = operation.missing_target.value();
      return std::nullopt;
    }
    const auto position = [&]() -> Result<bool> {
      switch (target.disposition) {
        case RowIdSeekDisposition::kSeek:
          return cursor->Seek(target.rowid, target.mode);
        case RowIdSeekDisposition::kFirst:
          return cursor->First();
        case RowIdSeekDisposition::kLast:
          return cursor->Last();
        case RowIdSeekDisposition::kMissing:
          break;
      }
      return false;
    };
    auto found = position();
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
    if (runtime.row_state != RuntimeCursor::RowState::Ordinary) {
      runtime.ClearRecordCache();
      program_counter_ = operation.end_target.value();
      return std::nullopt;
    }
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
      case IndexRangeEndMode::kReverseInclusive:
        ended = *comparison == std::weak_ordering::less;
        break;
      case IndexRangeEndMode::kReverseExclusive:
        ended = *comparison != std::weak_ordering::greater;
        break;
      default:
        return std::unexpected(VmError(ErrorCode::kInternal, "unknown index range end mode"));
    }
    if (ended) {
      program_counter_ = operation.end_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ReadFieldInstruction& operation,
                                       ColumnFunctionConsumer* consumer = nullptr) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    auto restored = RestoreMovedCursor(runtime);
    if (!restored.has_value()) {
      return std::unexpected(std::move(restored.error()));
    }
    if (runtime.row_state != RuntimeCursor::RowState::Ordinary) {
      return SetNullRegister(operation.output);
    }
    const ReadCursorDescriptor& descriptor = program_->cursor(operation.cursor);
    const CursorFieldSource& source = descriptor.fields[operation.field.value()];
    if (source.kind == CursorFieldSourceKind::kRowId) {
      if (const auto* read_cursor = std::get_if<TableBtreeCursor>(&runtime.storage);
          read_cursor != nullptr && read_cursor->valid()) {
        auto rowid = read_cursor->rowid();
        if (!rowid.has_value()) {
          return std::unexpected(std::move(rowid.error()));
        }
        return SetRegister(operation.output, *rowid);
      } else if (auto* mutation_cursor = std::get_if<TableBtreeMutationCursor>(&runtime.storage);
                 mutation_cursor != nullptr && mutation_cursor->valid()) {
        auto rowid = mutation_cursor->rowid();
        if (!rowid.has_value()) {
          return std::unexpected(std::move(rowid.error()));
        }
        return SetRegister(operation.output, *rowid);
      } else {
        return std::unexpected(
            VmError(ErrorCode::kInternal, "rowid field used an invalid table cursor"));
      }
    }
    auto field = ReadRecordField(runtime, source.record_field, operation.output, consumer);
    if (!field.has_value()) {
      return std::unexpected(std::move(field.error()));
    }
    if (*field) {
      return std::nullopt;
    }
    SqlValue value;
    switch (source.missing_value_kind) {
      case MissingFieldValueKind::kNull:
        break;
      case MissingFieldValueKind::kConstant:
        if (!source.missing_value.has_value()) {
          return std::unexpected(
              VmError(ErrorCode::kInternal, "missing-field constant descriptor has no constant"));
        }
        value = program_->constant(*source.missing_value).Clone();
        break;
      case MissingFieldValueKind::kUnsupported:
        return std::unexpected(VmError(ErrorCode::kGeneric,
                                       "unsupported default for physically missing record field"));
    }
    if (operation.missing_value_affinity.has_value()) {
      auto applied = ApplyTableRecordAffinity(std::move(value), *operation.missing_value_affinity,
                                              record_options_);
      if (!applied.has_value()) {
        return std::unexpected(std::move(applied.error()));
      }
      value = std::move(*applied);
    }
    return SetRegister(operation.output, std::move(value));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ReadRowIdInstruction& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    auto restored = RestoreMovedCursor(runtime);
    if (!restored.has_value()) {
      return std::unexpected(std::move(restored.error()));
    }
    if (runtime.row_state != RuntimeCursor::RowState::Ordinary) {
      return SetNullRegister(operation.output);
    }
    if (const auto* read_cursor = std::get_if<TableBtreeCursor>(&runtime.storage);
        read_cursor != nullptr && read_cursor->valid()) {
      auto rowid = read_cursor->rowid();
      if (!rowid.has_value()) {
        return std::unexpected(std::move(rowid.error()));
      }
      return SetRegister(operation.output, *rowid);
    }
    if (auto* mutation_cursor = std::get_if<TableBtreeMutationCursor>(&runtime.storage);
        mutation_cursor != nullptr && mutation_cursor->valid()) {
      auto rowid = mutation_cursor->rowid();
      if (!rowid.has_value()) {
        return std::unexpected(std::move(rowid.error()));
      }
      return SetRegister(operation.output, *rowid);
    }
    return std::unexpected(
        VmError(ErrorCode::kInternal, "rowid read used an invalid table cursor"));
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ResultRowInstruction& operation) {
    column_cache_counter_ = (column_cache_counter_ + 2U) | 1U;
    row_first_ = operation.first.value();
    row_count_ = operation.count;
    state_ = VmState::kRow;
    return VmStep::kRow;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const OpenSorterInstruction& operation) {
    std::optional<RecordSorter>& runtime = sorters_[operation.sorter.value()];
    if (runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sorter is already open"));
    }

    auto storage = EnsureStatementTemporaryStorage();
    if (!storage.has_value()) {
      return std::unexpected(std::move(storage.error()));
    }
    auto created = (*storage)->CreateRecordSorter(
        RuntimeOrderingDescriptor(program_->sorter(operation.sorter)));
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    runtime.emplace(std::move(*created));
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const InsertSorterInstruction& operation) {
    std::optional<RecordSorter>& runtime = sorters_[operation.sorter.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sorter is not open"));
    }
    const auto values = std::span<const SqlValue>{registers_}.subspan(operation.first_value.value(),
                                                                      operation.value_count);
    auto inserted = runtime->Insert(values);
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const InsertEncodedSorterInstruction& operation) {
    auto& runtime = sorters_[operation.sorter.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sorter is not open"));
    }
    const SqlValue& record = Register(operation.record);
    if (record.type() != SqlValueType::kBlob) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "sorter record is not a BLOB"));
    }
    auto inserted = runtime->InsertEncoded(SqlValueAccess::Blob(record));
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const RewindSorterInstruction& operation) {
    std::optional<RecordSorter>& runtime = sorters_[operation.sorter.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sorter is not open"));
    }
    const auto target = program_->sorter(operation.sorter).index_refill_cursor;
    if (target.has_value()) {
      RuntimeWriteCursor& index = WriteCursor(*target);
      if (!index.index.has_value() || index.index_refill.has_value()) {
        return std::unexpected(
            VmError(ErrorCode::kMisuse, "index refill requires an unused index writer"));
      }
      auto cursor = index.index->OpenAppendCursor();
      if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
      }
      index.index_refill.emplace(RuntimeIndexRefill{.cursor = std::move(*cursor)});
    }
    auto rewound = runtime->Rewind();
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (runtime->state() == RecordSorterState::kExhausted) {
      program_counter_ = operation.empty_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ReadSorterFieldInstruction& operation) {
    std::optional<RecordSorter>& runtime = sorters_[operation.sorter.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sorter is not open"));
    }
    auto record = runtime->current_record();
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    auto field = record->field(operation.field);
    if (!field.has_value()) {
      return std::unexpected(std::move(field.error()));
    }
    return SetRecordField(operation.output, *field);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const NextSorterInstruction& operation) {
    std::optional<RecordSorter>& runtime = sorters_[operation.sorter.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sorter is not open"));
    }
    auto advanced = runtime->Next();
    if (!advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    if (*advanced) {
      program_counter_ = operation.next_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ResetSorterInstruction& operation) {
    std::optional<RecordSorter>& runtime = sorters_[operation.sorter.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sorter is not open"));
    }
    auto reset = runtime->Reset();
    if (!reset.has_value()) {
      return std::unexpected(std::move(reset.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CloseSorterInstruction& operation) {
    std::optional<RecordSorter>& runtime = sorters_[operation.sorter.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "sorter is not open"));
    }
    runtime->Close();
    runtime.reset();
    return std::nullopt;
  }

  [[nodiscard]] Result<std::size_t> ReadTopNBound(RegisterId input) const {
    const auto bound = Register(input).integer_value();
    if (!bound.has_value() || *bound < 0) {
      return std::unexpected(
          VmError(ErrorCode::kTypeMismatch, "top-N bound must be a nonnegative integer"));
    }
    const auto unsigned_bound = static_cast<std::uint64_t>(*bound);
    if (unsigned_bound > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "top-N bound is too large"));
    }
    return static_cast<std::size_t>(unsigned_bound);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const OpenTopNInstruction& operation) {
    if (temporary_storage_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "top-N bytecode requires temporary storage"));
    }
    std::optional<BoundedTopN>& runtime = top_ns_[operation.top_n.value()];
    if (runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is already open"));
    }
    auto bound = ReadTopNBound(operation.bound);
    if (!bound.has_value()) {
      return std::unexpected(std::move(bound.error()));
    }
    auto storage = EnsureStatementTemporaryStorage();
    if (!storage.has_value()) {
      return std::unexpected(std::move(storage.error()));
    }
    auto created =
        (*storage)->CreateTopN(RuntimeOrderingDescriptor(program_->top_n(operation.top_n)), *bound);
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    runtime.emplace(std::move(*created));
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CheckTopNInstruction& operation) {
    std::optional<BoundedTopN>& runtime = top_ns_[operation.top_n.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is not open"));
    }
    const auto key = std::span<const SqlValue>{registers_}.subspan(operation.first_key.value(),
                                                                   operation.key_count);
    auto encoded = EncodeRecord(key, record_options_);
    if (!encoded.has_value()) {
      return std::unexpected(std::move(encoded.error()));
    }
    auto checked = runtime->CheckCandidate(std::move(*encoded));
    if (!checked.has_value()) {
      return std::unexpected(std::move(checked.error()));
    }
    if (*checked == TopNCheckResult::kRejected) {
      program_counter_ = operation.rejected_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const InsertTopNInstruction& operation) {
    std::optional<BoundedTopN>& runtime = top_ns_[operation.top_n.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is not open"));
    }
    const auto values = std::span<const SqlValue>{registers_}.subspan(operation.first_value.value(),
                                                                      operation.value_count);
    auto encoded = EncodeRecord(values, record_options_);
    if (!encoded.has_value()) {
      return std::unexpected(std::move(encoded.error()));
    }
    auto inserted = runtime->Insert(std::move(*encoded));
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const InsertEncodedTopNInstruction& operation) {
    auto& runtime = top_ns_[operation.top_n.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is not open"));
    }
    const SqlValue& record = Register(operation.record);
    if (record.type() != SqlValueType::kBlob) {
      return std::unexpected(VmError(ErrorCode::kTypeMismatch, "top-N record is not a BLOB"));
    }
    auto inserted = runtime->InsertEncoded(SqlValueAccess::Blob(record));
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const RewindTopNInstruction& operation) {
    std::optional<BoundedTopN>& runtime = top_ns_[operation.top_n.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is not open"));
    }
    auto rewound = runtime->Rewind();
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (runtime->state() == BoundedTopNState::kExhausted) {
      program_counter_ = operation.empty_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ReadTopNFieldInstruction& operation) {
    std::optional<BoundedTopN>& runtime = top_ns_[operation.top_n.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is not open"));
    }
    auto record = runtime->current_record();
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    auto field = record->field(operation.field);
    if (!field.has_value()) {
      return std::unexpected(std::move(field.error()));
    }
    return SetRecordField(operation.output, *field);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const NextTopNInstruction& operation) {
    std::optional<BoundedTopN>& runtime = top_ns_[operation.top_n.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is not open"));
    }
    auto advanced = runtime->Next();
    if (!advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    if (*advanced) {
      program_counter_ = operation.next_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ResetTopNInstruction& operation) {
    std::optional<BoundedTopN>& runtime = top_ns_[operation.top_n.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is not open"));
    }
    std::optional<std::size_t> residual_bound;
    if (operation.residual_bound.has_value()) {
      auto decoded = ReadTopNBound(*operation.residual_bound);
      if (!decoded.has_value()) {
        return std::unexpected(std::move(decoded.error()));
      }
      residual_bound = *decoded;
    }
    auto reset =
        residual_bound.has_value() ? runtime->ResetBlock(*residual_bound) : runtime->Reset();
    if (!reset.has_value()) {
      return std::unexpected(std::move(reset.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CloseTopNInstruction& operation) {
    std::optional<BoundedTopN>& runtime = top_ns_[operation.top_n.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "top-N relation is not open"));
    }
    runtime->Close();
    runtime.reset();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const OpenRelationInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is already open"));
    }
    auto storage = EnsureStatementTemporaryStorage();
    if (!storage.has_value()) {
      return std::unexpected(std::move(storage.error()));
    }
    auto created = (*storage)->CreateEphemeralRelation(
        RuntimeRelationDescriptor(program_->relation(operation.relation)));
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    runtime.emplace(std::move(*created));
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const InsertRelationInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    const auto values = std::span<const SqlValue>{registers_}.subspan(operation.first_value.value(),
                                                                      operation.value_count);
    auto encoded = EncodeRecord(values, record_options_);
    if (!encoded.has_value()) {
      return std::unexpected(std::move(encoded.error()));
    }
    InvalidateRelationBorrower(operation.relation);
    auto inserted =
        runtime->Insert(std::move(*encoded), operation.mode == RelationInsertMode::kReplaceExisting
                                                 ? EphemeralInsertMode::kReplaceExisting
                                                 : EphemeralInsertMode::kKeepExisting);
    if (!inserted.has_value()) {
      return std::unexpected(std::move(inserted.error()));
    }
    if (*inserted != EphemeralInsertResult::kInserted) {
      program_counter_ = operation.duplicate_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const ContainsRelationInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    const auto key = std::span<const SqlValue>{registers_}.subspan(operation.first_key.value(),
                                                                   operation.key_count);
    auto encoded = EncodeRecord(key, record_options_);
    if (!encoded.has_value()) {
      return std::unexpected(std::move(encoded.error()));
    }
    auto contained = runtime->Contains(encoded->view());
    if (!contained.has_value()) {
      return std::unexpected(std::move(contained.error()));
    }
    if (*contained) {
      program_counter_ = operation.found_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const DeleteRelationInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    const auto key = std::span<const SqlValue>{registers_}.subspan(operation.first_key.value(),
                                                                   operation.key_count);
    auto encoded = EncodeRecord(key, record_options_);
    if (!encoded.has_value()) {
      return std::unexpected(std::move(encoded.error()));
    }
    auto erased = runtime->Erase(encoded->view());
    if (!erased.has_value()) {
      return std::unexpected(std::move(erased.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const RewindRelationInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    auto rewound = runtime->Rewind();
    if (!rewound.has_value()) {
      return std::unexpected(std::move(rewound.error()));
    }
    if (runtime->state() == EphemeralRelationState::kExhausted) {
      program_counter_ = operation.empty_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const ReadRelationFieldInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    auto record = runtime->current_record();
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    auto field = record->field(operation.field);
    if (!field.has_value()) {
      return std::unexpected(std::move(field.error()));
    }
    return SetRecordField(operation.output, *field);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const NextRelationInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    auto advanced = runtime->Next();
    if (!advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    if (*advanced) {
      program_counter_ = operation.next_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const ResetRelationInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    InvalidateRelationBorrower(operation.relation);
    auto reset = runtime->Reset();
    if (!reset.has_value()) {
      return std::unexpected(std::move(reset.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CloseRelationInstruction& operation) {
    std::optional<EphemeralRelation>& runtime = relations_[operation.relation.value()];
    if (!runtime.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    InvalidateRelationBorrower(operation.relation);
    runtime->Close();
    runtime.reset();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t,
                                       const CheckRelationLookupInstruction& operation) {
    auto relation = Relation(operation.relation);
    if (!relation.has_value()) {
      return std::unexpected(std::move(relation.error()));
    }
    const EphemeralRelationState state = (*relation)->state();
    if (state == EphemeralRelationState::kWriting) {
      return std::nullopt;
    }
    if (state != EphemeralRelationState::kLookupReady &&
        state != EphemeralRelationState::kLookupPositioned) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "lookup readiness requires a writing or sealed relation"));
    }
    program_counter_ = (*relation)->record_count() == 0U ? operation.empty_target.value()
                                                         : operation.ready_target.value();
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const SealRelationInstruction& operation) {
    auto relation = Relation(operation.relation);
    if (!relation.has_value()) {
      return std::unexpected(std::move(relation.error()));
    }
    InvalidateRelationBorrower(operation.relation);
    auto sealed = (*relation)->SealForLookup();
    if (!sealed.has_value()) {
      return std::unexpected(std::move(sealed.error()));
    }
    return std::nullopt;
  }

  template <typename Operation>
    requires(std::is_same_v<Operation, SeekRelationPrefixInstruction> ||
             std::is_same_v<Operation, NextRelationPrefixInstruction>)
  [[nodiscard]] DispatchResult Execute(std::uint32_t, const Operation& operation) {
    RuntimeCursor& runtime = Cursor(operation.cursor);
    if constexpr (std::is_same_v<Operation, NextRelationPrefixInstruction>) {
      if (runtime.row_state != RuntimeCursor::RowState::Ordinary) {
        runtime.ClearRecordCache();
        return std::nullopt;
      }
    }
    runtime.ClearRecordCache();
    const auto* id = std::get_if<RelationId>(&runtime.storage);
    if (id == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kInternal, "lookup requires an open ephemeral-index cursor"));
    }
    auto relation = Relation(*id);
    if (!relation.has_value()) {
      return std::unexpected(std::move(relation.error()));
    }
    const auto key = std::span<const SqlValue>{registers_}.subspan(operation.first_key.value(),
                                                                   operation.key_count);
    auto positioned = [&]() -> Result<bool> {
      if constexpr (std::is_same_v<Operation, SeekRelationPrefixInstruction>) {
        return (*relation)->SeekPrefix(key);
      } else {
        return (*relation)->NextPrefix(key);
      }
    }();
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }
    if constexpr (std::is_same_v<Operation, SeekRelationPrefixInstruction>) {
      if (!*positioned) {
        program_counter_ = operation.missing_target.value();
      }
    } else if (*positioned) {
      program_counter_ = operation.next_target.value();
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const CompareRecordsInstruction& operation) {
    const RecordComparisonDescriptor& descriptor =
        program_->record_comparison(operation.comparison);
    const auto left = std::span<const SqlValue>{registers_}.subspan(operation.left_first.value(),
                                                                    descriptor.field_count);
    const auto right = std::span<const SqlValue>{registers_}.subspan(operation.right_first.value(),
                                                                     descriptor.field_count);
    const auto& columns = record_comparison_columns_[operation.comparison.value()];
    auto comparison = CompareRecordValues(left, right, columns);
    if (!comparison.has_value()) {
      return std::unexpected(std::move(comparison.error()));
    }
    const std::int64_t result = *comparison == std::weak_ordering::less
                                    ? -1
                                    : (*comparison == std::weak_ordering::greater ? 1 : 0);
    return SetRegister(operation.output, result);
  }

  [[nodiscard]] DispatchResult Execute(std::uint32_t, const HaltInstruction&) {
    ClearExecution();
    state_ = VmState::kDone;
    return VmStep::kDone;
  }

  [[nodiscard]] DispatchResult CopyRegister(RegisterId destination, const SqlValue& source) {
    if (ValueByteSize(source) > limits_.maximum_value_bytes) {
      return std::unexpected(VmError(ErrorCode::kTooLarge, "value exceeds the VM value limit"));
    }
    auto copied = SqlValueAccess::Copy(registers_[destination.value()], source);
    if (!copied.has_value()) {
      return std::unexpected(std::move(copied.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult SetNullRegister(RegisterId destination) {
    SqlValueAccess::SetNull(registers_[destination.value()]);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult SetRegister(RegisterId destination, std::int64_t value) {
    SqlValueAccess::SetInteger(registers_[destination.value()], value);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult SetRegister(RegisterId destination, double value) {
    SqlValueAccess::SetReal(registers_[destination.value()], value);
    return std::nullopt;
  }

  [[nodiscard]] DispatchResult SetRegister(RegisterId destination, SqlTruthValue value) {
    switch (value) {
      case SqlTruthValue::kNull:
        return SetNullRegister(destination);
      case SqlTruthValue::kFalse:
        return SetRegister(destination, std::int64_t{0});
      case SqlTruthValue::kTrue:
        return SetRegister(destination, std::int64_t{1});
    }
    return std::unexpected(VmError(ErrorCode::kInternal, "unknown SQL truth value"));
  }

  [[nodiscard]] DispatchResult SetRegister(RegisterId destination, SqlValue value) {
    switch (value.type()) {
      case SqlValueType::kNull:
        return SetNullRegister(destination);
      case SqlValueType::kInteger:
        return SetRegister(destination, SqlValueAccess::Integer(value));
      case SqlValueType::kReal:
        return SetRegister(destination, SqlValueAccess::Real(value));
      case SqlValueType::kText:
      case SqlValueType::kBlob:
        if (ValueByteSize(value) > limits_.maximum_value_bytes) {
          return std::unexpected(VmError(ErrorCode::kTooLarge, "value exceeds the VM value limit"));
        }
        registers_[destination.value()] = std::move(value);
        return std::nullopt;
    }
    return std::unexpected(VmError(ErrorCode::kInternal, "unknown SQL value type"));
  }

  [[nodiscard]] DispatchResult SetRecordField(RegisterId destination,
                                              const RecordFieldView& field) {
    SqlValue& target = registers_[destination.value()];
    Status copied;
    switch (field.type()) {
      case SqlValueType::kNull:
        SqlValueAccess::SetNull(target);
        break;
      case SqlValueType::kInteger: {
        const auto integer = field.integer_value();
        if (!integer.has_value()) {
          std::terminate();
        }
        SqlValueAccess::SetInteger(target, *integer);
        break;
      }
      case SqlValueType::kReal: {
        const auto real = field.real_value();
        if (!real.has_value()) {
          std::terminate();
        }
        SqlValueAccess::SetReal(target, *real);
        break;
      }
      case SqlValueType::kText: {
        const auto text = field.text_value();
        if (!text.has_value()) {
          std::terminate();
        }
        if (text->size_bytes() > limits_.maximum_value_bytes) {
          return std::unexpected(
              VmError(ErrorCode::kTooLarge, "record text field exceeds the VM value limit"));
        }
        copied = SqlValueAccess::CopyText(target, *text);
        break;
      }
      case SqlValueType::kBlob: {
        const auto blob = field.blob_value();
        if (!blob.has_value()) {
          std::terminate();
        }
        if (blob->size() > limits_.maximum_value_bytes) {
          return std::unexpected(
              VmError(ErrorCode::kTooLarge, "record blob field exceeds the VM value limit"));
        }
        copied = SqlValueAccess::CopyBlob(target, *blob);
        break;
      }
    }
    if (!copied.has_value()) {
      return std::unexpected(std::move(copied.error()));
    }
    return std::nullopt;
  }

  [[nodiscard]] const SqlValue& Register(RegisterId register_id) const noexcept {
    return registers_[register_id.value()];
  }

  [[nodiscard]] RuntimeCursor& Cursor(CursorId cursor_id) noexcept {
    return cursors_[cursor_id.value()];
  }

  [[nodiscard]] Result<EphemeralRelation*> Relation(RelationId id) {
    std::optional<EphemeralRelation>& relation = relations_[id.value()];
    if (!relation.has_value()) {
      return std::unexpected(VmError(ErrorCode::kInternal, "ephemeral relation is not open"));
    }
    return &*relation;
  }

  void InvalidateRelationBorrower(RelationId id) noexcept {
    if (const std::optional<CursorId> borrower = program_->relation_borrower(id);
        borrower.has_value()) {
      Cursor(*borrower).ClearRecordCache();
    }
  }

  [[nodiscard]] RuntimeWriteCursor& WriteCursor(WriteCursorId cursor_id) noexcept {
    return write_cursors_[cursor_id.value()];
  }

  [[nodiscard]] static bool HasPhysicalNeighbor(const RuntimeCursor& runtime) noexcept {
    if (const auto* cursor = std::get_if<TableBtreeCursor>(&runtime.storage); cursor != nullptr) {
      return cursor->HasPosition();
    }
    if (const auto* cursor = std::get_if<TableBtreeMutationCursor>(&runtime.storage);
        cursor != nullptr) {
      return cursor->HasPosition();
    }
    if (const auto* cursor = std::get_if<IndexBtreeCursor>(&runtime.storage); cursor != nullptr) {
      return cursor->HasPosition();
    }
    return false;
  }

  [[nodiscard]] Status RestoreMovedCursor(RuntimeCursor& runtime) {
    if (runtime.row_state == RuntimeCursor::RowState::VirtualNull) {
      return {};
    }
    return std::visit(
        [&runtime](auto& cursor) -> Status {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, TableBtreeCursor> ||
                        std::is_same_v<T, TableBtreeMutationCursor> ||
                        std::is_same_v<T, IndexBtreeCursor>) {
            if (!cursor.HasMoved()) {
              return {};
            }
            const bool deleted = runtime.row_state == RuntimeCursor::RowState::DeletedNull;
            runtime.ClearRecordCache();
            runtime.column_cache.bytes.reset();
            auto restored = cursor.RestorePosition();
            if (!restored.has_value()) {
              return std::unexpected(std::move(restored.error()));
            }
            if (deleted || !*restored) {
              runtime.row_state = RuntimeCursor::RowState::DeletedNull;
            }
          }
          return {};
        },
        runtime.storage);
  }

  [[nodiscard]] Result<bool> PrepareCursorStep(RuntimeCursor& runtime) {
    if (runtime.row_state == RuntimeCursor::RowState::VirtualNull) {
      return false;
    }
    if (runtime.row_state == RuntimeCursor::RowState::DeletedNull) {
      auto restored = RestoreMovedCursor(runtime);
      if (!restored.has_value()) {
        return std::unexpected(std::move(restored.error()));
      }
      return HasPhysicalNeighbor(runtime);
    }
    return true;
  }

  [[nodiscard]] Result<std::pair<ByteView, ByteCount>> BorrowRecordPayload(RuntimeCursor& runtime) {
    return std::visit(
        [this](auto& cursor) -> Result<std::pair<ByteView, ByteCount>> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            return std::unexpected(
                VmError(ErrorCode::kInternal, "record read used a closed cursor"));
          } else if constexpr (std::is_same_v<T, RelationId>) {
            auto relation = Relation(cursor);
            if (!relation.has_value()) {
              return std::unexpected(std::move(relation.error()));
            }
            auto record = (*relation)->current_record();
            if (!record.has_value()) {
              return std::unexpected(std::move(record.error()));
            }
            const ByteView encoded = record->encoded();
            return std::pair{encoded, ByteCount{encoded.size()}};
          } else {
            auto payload = cursor.payload();
            if (!payload.has_value()) {
              return std::unexpected(std::move(payload.error()));
            }
            return std::pair{payload->local_bytes(), payload->size()};
          }
        },
        runtime.storage);
  }

  [[nodiscard]] Status ReadRecordPayload(RuntimeCursor& runtime, ByteOffset offset,
                                         MutableByteView destination) {
    return std::visit(
        [offset, destination](auto& cursor) -> Status {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, TableBtreeCursor> ||
                        std::is_same_v<T, IndexBtreeCursor> ||
                        std::is_same_v<T, TableBtreeMutationCursor>) {
            return cursor.ReadPayload(offset, destination);
          } else {
            return std::unexpected(
                VmError(ErrorCode::kInternal, "nonlocal record read requires a B-tree cursor"));
          }
        },
        runtime.storage);
  }

  [[nodiscard]] Result<std::optional<RecordColumnField>> ReadRecordFieldLayout(
      RuntimeCursor& runtime, std::uint32_t field_index) {
    if (runtime.column_cache_status != column_cache_counter_) {
      auto payload = BorrowRecordPayload(runtime);
      if (!payload.has_value()) {
        return std::unexpected(std::move(payload.error()));
      }
      auto reset = runtime.columns.Reset(payload->first, payload->second, record_options_);
      if (!reset.has_value()) {
        return std::unexpected(std::move(reset.error()));
      }
      runtime.row_bytes = payload->first;
      runtime.column_cache_status = column_cache_counter_;
    }
    ByteView header = runtime.row_bytes;
    ByteBuffer scratch;
    const std::size_t size = runtime.columns.header_size().value();
    if (runtime.columns.needs_header(field_index) && header.size() < size) {
      scratch = ByteBuffer{ByteCount{size + 1U}};
      auto read = ReadRecordPayload(runtime, ByteOffset{}, scratch.mutable_view().first(size));
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      header = scratch.view().first(size);
    }
    return runtime.columns.field(field_index, header);
  }

  [[nodiscard]] Result<bool> ReadSharedRecordField(RuntimeCursor& runtime,
                                                   std::uint32_t field_index,
                                                   const RecordColumnField& field,
                                                   RegisterId output) {
    auto offset = std::visit(
        [](auto& cursor) -> Result<FileOffset> {
          using T = std::remove_cvref_t<decltype(cursor)>;
          if constexpr (std::is_same_v<T, TableBtreeCursor> ||
                        std::is_same_v<T, TableBtreeMutationCursor>) {
            return cursor.payload_offset();
          } else {
            return std::unexpected(
                VmError(ErrorCode::kInternal, "shared column requires a table cursor"));
          }
        },
        runtime.storage);
    if (!offset.has_value()) {
      return std::unexpected(std::move(offset.error()));
    }
    RuntimeColumnCache& cache = runtime.column_cache;
    if (cache.bytes == nullptr || cache.column != field_index ||
        cache.row_cache_counter != column_cache_counter_ ||
        cache.table_write_counter != column_write_counter_ || cache.payload_offset != *offset) {
      cache.bytes.reset();
      const std::size_t size = field.size.value();
      const std::size_t trailing = SqlValueAccess::kSharedColumnTerminatorBytes.value();
      if (size > std::numeric_limits<std::size_t>::max() - trailing) {
        return std::unexpected(
            VmError(ErrorCode::kTooLarge, "shared column length exceeds the size limit"));
      }
      auto bytes = std::make_shared<ByteBuffer>(ByteCount{size + trailing});
      auto read = ReadRecordPayload(runtime, field.offset, bytes->mutable_view().first(size));
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      std::ranges::fill(bytes->mutable_view().subspan(size, trailing), std::byte{});
      cache.column = field_index;
      cache.row_cache_counter = column_cache_counter_;
      cache.table_write_counter = column_write_counter_;
      cache.payload_offset = *offset;
      cache.bytes = std::move(bytes);
    }
    SqlValueAccess::ShareColumnBytes(
        registers_[output.value()], cache.bytes, field.size,
        (field.serial_type & 1U) != 0U ? SqlValueType::kText : SqlValueType::kBlob);
    return true;
  }

  [[nodiscard]] Result<bool> ReadRecordField(RuntimeCursor& runtime, std::uint32_t field_index,
                                             RegisterId output,
                                             ColumnFunctionConsumer* consumer = nullptr) {
    auto layout = ReadRecordFieldLayout(runtime, field_index);
    if (!layout.has_value()) {
      return std::unexpected(std::move(layout.error()));
    }
    if (!layout->has_value()) {
      return false;
    }
    const RecordColumnField& field = **layout;
    const std::size_t size = field.size.value();
    const bool local = field.offset.value() <= runtime.row_bytes.size() &&
                       size <= runtime.row_bytes.size() - field.offset.value();
    if (!local && consumer != nullptr) {
      SqlValueType type = field.type();
      if (consumer->real_affinity && type == SqlValueType::kInteger) {
        type = SqlValueType::kReal;
      }
      auto value = consumer->function.InvokeColumnMetadata(type, field.size);
      if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
      }
      if (value->has_value()) {
        SqlValueAccess::SetNull(registers_[output.value()]);
        auto stored = SetRegister(consumer->output, std::move(**value));
        if (!stored.has_value()) {
          return std::unexpected(std::move(stored.error()));
        }
        consumer->completed = true;
        return true;
      }
    }
    if (size > limits_.maximum_value_bytes) {
      return std::unexpected(
          VmError(ErrorCode::kTooLarge, "record field exceeds the VM value limit"));
    }
    ByteView encoded;
    std::array<std::byte, sizeof(std::uint64_t)> scalar{};
    if (size == 0U) {
      encoded = {};
    } else if (local) {
      encoded = runtime.row_bytes.subspan(field.offset.value(), size);
    } else if (field.serial_type < kVariableSerialTypeBase) {
      if (size > scalar.size()) {
        std::terminate();
      }
      auto read = ReadRecordPayload(runtime, field.offset, MutableByteView{scalar}.first(size));
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      encoded = ByteView{scalar}.first(size);
    } else {
      if (size > kSharedColumnThresholdBytes &&
          (std::holds_alternative<TableBtreeCursor>(runtime.storage) ||
           std::holds_alternative<TableBtreeMutationCursor>(runtime.storage))) {
        return ReadSharedRecordField(runtime, field_index, field, output);
      }
      SqlValue& target = registers_[output.value()];
      auto destination = SqlValueAccess::PrepareBytes(target, field.size);
      if (!destination.has_value()) {
        return std::unexpected(std::move(destination.error()));
      }
      auto read = ReadRecordPayload(runtime, field.offset, *destination);
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      SqlValueAccess::CommitBytes(
          target, field.size,
          (field.serial_type & 1U) != 0U ? SqlValueType::kText : SqlValueType::kBlob);
      return true;
    }
    auto decoded = RecordFieldView::ParseColumn(field.serial_type, encoded);
    if (!decoded.has_value()) {
      return std::unexpected(std::move(decoded.error()));
    }
    auto stored = SetRecordField(output, *decoded);
    if (!stored.has_value()) {
      return std::unexpected(std::move(stored.error()));
    }
    return true;
  }

  void ClearRow() noexcept {
    row_first_ = 0;
    row_count_ = 0;
  }

  [[nodiscard]] Result<StatementTemporaryStorage*> EnsureStatementTemporaryStorage() {
    if (temporary_storage_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "temporary bytecode requires temporary storage"));
    }
    if (!statement_temporary_storage_.has_value()) {
      auto created = temporary_storage_->CreateStatementStorage();
      if (!created.has_value()) {
        return std::unexpected(std::move(created.error()));
      }
      statement_temporary_storage_.emplace(std::move(*created));
    }
    return &*statement_temporary_storage_;
  }

  void CloseCapabilities(std::optional<CoroutineId> scope) noexcept {
    const auto selected = [&](auto id) {
      if (!scope.has_value()) {
        return true;
      }
      const auto owner = program_->coroutine_owner(id);
      return owner.has_value() && program_->coroutine_contains(*scope, *owner);
    };
    for (std::size_t index = 0; index < cursors_.size(); ++index) {
      if (!selected(CursorId{static_cast<std::uint32_t>(index)})) {
        continue;
      }
      RuntimeCursor& cursor = cursors_[index];
      cursor.ClearRecordCache();
      cursor.column_cache.bytes.reset();
      cursor.storage = std::monostate{};
    }
    if (!scope.has_value()) {
      for (RuntimeWriteCursor& cursor : write_cursors_) {
        cursor.index_refill.reset();
        cursor.table.reset();
        cursor.index.reset();
      }
    }
    for (std::size_t index = 0; index < sorters_.size(); ++index) {
      if (selected(SorterId{static_cast<std::uint32_t>(index)})) {
        sorters_[index].reset();
      }
    }
    for (std::size_t index = 0; index < top_ns_.size(); ++index) {
      if (selected(TopNId{static_cast<std::uint32_t>(index)})) {
        top_ns_[index].reset();
      }
    }
    for (std::size_t index = 0; index < relations_.size(); ++index) {
      if (selected(RelationId{static_cast<std::uint32_t>(index)})) {
        relations_[index].reset();
      }
    }
    for (std::size_t index = 0; index < aggregates_.size(); ++index) {
      if (selected(AggregateId{static_cast<std::uint32_t>(index)})) {
        aggregates_[index].state.reset();
      }
    }
  }

  void ClearExecution() noexcept {
    subroutine_continuations_.clear();
    std::ranges::fill(coroutines_, CoroutineContinuation{});
    CloseCapabilities(std::nullopt);
    statement_temporary_storage_.reset();
    for (SqlValue& value : registers_) {
      SqlValueAccess::Release(value);
    }
    ClearRow();
    execution_data_version_.reset();
    column_cache_counter_ = 1U;
    column_write_counter_ = 0;
    rowid_list_index_ = 0;
    rowid_list_positioned_ = false;
  }

  const BytecodeProgram* program_;
  BtreeDatabase* database_ = nullptr;
  Pager* pager_ = nullptr;
  std::uint64_t catalog_generation_ = 0;
  TransactionWriter* writer_ = nullptr;
  const TemporaryStorageFactory* temporary_storage_ = nullptr;
  const FunctionRegistry* functions_;
  std::span<const Collation* const> available_collations_;
  VmLimits limits_;
  std::vector<SqlValue> registers_;
  std::vector<SqlValue> parameters_;
  std::vector<RuntimeCursor> cursors_;
  std::vector<RuntimeWriteCursor> write_cursors_;
  std::optional<StatementTemporaryStorage> statement_temporary_storage_;
  std::vector<std::optional<RecordSorter>> sorters_;
  std::vector<std::optional<BoundedTopN>> top_ns_;
  std::vector<std::optional<EphemeralRelation>> relations_;
  std::vector<RuntimeAggregate> aggregates_;
  std::vector<std::vector<RecordValueComparisonKey>> record_comparison_columns_;
  std::vector<CoroutineContinuation> coroutines_;
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
  std::uint32_t column_cache_counter_ = 1U;
  std::uint32_t column_write_counter_ = 0;
  std::vector<std::int64_t> rowid_list_;
  std::vector<SubroutineContinuation> subroutine_continuations_;
  std::size_t rowid_list_index_ = 0;
  bool rowid_list_positioned_ = false;
};

VmExecutionContext::VmExecutionContext(BtreeDatabase& database,
                                       std::uint64_t catalog_generation) noexcept
    : database_(&database), pager_(&database.pager()), catalog_generation_(catalog_generation) {}

VmExecutionContext::VmExecutionContext(BtreeDatabase& database, std::uint64_t catalog_generation,
                                       const TemporaryStorageFactory& temporary_storage) noexcept
    : VmExecutionContext(database, catalog_generation) {
  temporary_storage_ = &temporary_storage;
}

VmExecutionContext::VmExecutionContext(TransactionWriter& writer,
                                       std::uint64_t catalog_generation) noexcept
    : VmExecutionContext(writer.database(), catalog_generation) {
  writer_ = &writer;
}

VmExecutionContext::VmExecutionContext(TransactionWriter& writer, std::uint64_t catalog_generation,
                                       const TemporaryStorageFactory& temporary_storage) noexcept
    : VmExecutionContext(writer, catalog_generation) {
  temporary_storage_ = &temporary_storage;
}

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
  try {
    if (impl_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "cannot clear bindings on a moved-from VM"));
    }
    return impl_->ClearBindings();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status Vm::AttachExecutionContext(VmExecutionContext context) {
  if (impl_ == nullptr) {
    return std::unexpected(
        VmError(ErrorCode::kMisuse, "cannot attach execution context to a moved-from VM"));
  }
  return impl_->AttachExecutionContext(context);
}

Status Vm::DetachExecutionContext() {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(
          VmError(ErrorCode::kMisuse, "cannot detach execution context from a moved-from VM"));
    }
    return impl_->DetachExecutionContext();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<VmStep> Vm::Step() {
  const bool owns_execution =
      impl_ != nullptr && (impl_->state_ == VmState::kReady || impl_->state_ == VmState::kRow);
  try {
    if (impl_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "cannot step a moved-from VM"));
    }
    return impl_->Step();
  } catch (const std::bad_alloc&) {
    if (owns_execution && impl_ != nullptr) {
      impl_->FailForException();
    }
    return std::unexpected(Error::OutOfMemory());
  } catch (...) {
    if (owns_execution && impl_ != nullptr) {
      impl_->FailForException();
    }
    throw;
  }
}

Status Vm::Reset() {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(VmError(ErrorCode::kMisuse, "cannot reset a moved-from VM"));
    }
    return impl_->Reset();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
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
