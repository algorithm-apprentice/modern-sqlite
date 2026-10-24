#ifndef MODERN_SQLITE_SRC_LOWERING_PLAN_LOWERING_INTERNAL_HPP_
#define MODERN_SQLITE_SRC_LOWERING_PLAN_LOWERING_INTERNAL_HPP_

#include <cassert>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/lowering/plan_lowering.hpp"

namespace modern_sqlite::lowering_internal {

inline constexpr std::string_view kEmitJumpFailureDetail = "unable to emit a bytecode jump";
inline constexpr std::string_view kEmitConditionalJumpFailureDetail =
    "unable to emit a conditional bytecode jump";

enum class AggregateExpressionSource : std::uint8_t {
  AccessRow,
  SortedRow,
  Representative,
};

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

[[nodiscard]] inline PlanLoweringError InternalFailure(std::string detail) {
  return PlanLoweringError{
      .code = PlanLoweringErrorCode::kInternalInvariant,
      .detail = std::move(detail),
  };
}

[[nodiscard]] inline PlanLoweringError UnsupportedFailure(std::string detail) {
  return PlanLoweringError{
      .code = PlanLoweringErrorCode::kUnsupportedPlan,
      .detail = std::move(detail),
  };
}

[[nodiscard]] inline PlanLoweringError ProgramFailure(ProgramError error, std::string detail) {
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

enum class IndexKeyPhase : std::uint8_t {
  start,
  end,
};

struct IndexTraversalBound {
  const PhysicalRangeBound* expression = nullptr;
  bool implicit_null = false;

  [[nodiscard]] bool has_range_term() const noexcept {
    return expression != nullptr || implicit_null;
  }

  [[nodiscard]] bool inclusive() const noexcept {
    return expression != nullptr ? expression->inclusive : !implicit_null;
  }
};

struct IndexTraversal {
  IndexTraversalBound start;
  IndexTraversalBound end;
  bool has_start_key = false;
  bool has_end_key = false;
};

struct RowIdTraversal {
  const PhysicalRangeBound* start = nullptr;
  const PhysicalRangeBound* end = nullptr;
  RowIdSeekMode start_mode = RowIdSeekMode::kEqual;
  SqlComparison end_comparison = SqlComparison::kEqual;
  bool reverse = false;
};

[[nodiscard]] inline LoweringResult<RowIdTraversal> ResolveRowIdTraversal(
    const PhysicalRowIdRangeNode& range) {
  if (!range.lower.has_value() && !range.upper.has_value()) {
    return std::unexpected(InternalFailure("rowid range has no bound"));
  }
  RowIdTraversal traversal;
  traversal.reverse = range.direction == PhysicalScanDirection::kReverse;
  if (!traversal.reverse) {
    traversal.start = range.lower.has_value() ? &*range.lower : nullptr;
    traversal.end = range.upper.has_value() ? &*range.upper : nullptr;
    if (traversal.start != nullptr) {
      traversal.start_mode =
          traversal.start->inclusive ? RowIdSeekMode::kGreaterOrEqual : RowIdSeekMode::kGreater;
    }
    if (traversal.end != nullptr) {
      traversal.end_comparison =
          traversal.end->inclusive ? SqlComparison::kGreater : SqlComparison::kGreaterEqual;
    }
  } else {
    traversal.start = range.upper.has_value() ? &*range.upper : nullptr;
    traversal.end = range.lower.has_value() ? &*range.lower : nullptr;
    if (traversal.start != nullptr) {
      traversal.start_mode =
          traversal.start->inclusive ? RowIdSeekMode::kLessOrEqual : RowIdSeekMode::kLess;
    }
    if (traversal.end != nullptr) {
      traversal.end_comparison =
          traversal.end->inclusive ? SqlComparison::kLess : SqlComparison::kLessEqual;
    }
  }
  return traversal;
}

[[nodiscard]] inline LoweringResult<void> EmitRowIdTraversalEndCheck(
    ProgramBuilder& builder, const RowIdTraversal& traversal, CursorId cursor, RegisterId end_key,
    RegisterId rowid, RegisterId comparison, SymbolId binary, Label end) {
  assert(traversal.end != nullptr);
  constexpr std::string_view kFailure = "unable to append a bytecode instruction";
  auto read = ConvertProgramResult(
      builder.Append(ReadRowIdInstruction{.cursor = cursor, .output = rowid}), kFailure);
  if (!read.has_value()) {
    return std::unexpected(std::move(read.error()));
  }
  auto compared = ConvertProgramResult(builder.Append(CompareInstruction{
                                           .comparison = traversal.end_comparison,
                                           .affinity = TypeAffinity::kNumeric,
                                           .collation = binary,
                                           .left = rowid,
                                           .right = end_key,
                                           .output = comparison,
                                       }),
                                       kFailure);
  if (!compared.has_value()) {
    return std::unexpected(std::move(compared.error()));
  }
  auto jumped =
      ConvertProgramResult(builder.EmitJumpIf(comparison, JumpCondition::kIfTrueOrNull, end),
                           kEmitConditionalJumpFailureDetail);
  if (!jumped.has_value()) {
    return std::unexpected(std::move(jumped.error()));
  }
  return {};
}

[[nodiscard]] inline LoweringResult<IndexTraversal> ResolveIndexTraversal(
    const PhysicalIndexScanNode& index, const CatalogSnapshot& catalog) {
  IndexTraversalBound forward_start;
  IndexTraversalBound forward_end;
  if (index.range.has_value()) {
    if (index.table.value >= catalog.tables().size()) {
      return std::unexpected(InternalFailure("index range table ID is out of range"));
    }
    const CatalogTable& table = catalog.table(index.table);
    if (index.range->column.value >= table.columns.size()) {
      return std::unexpected(InternalFailure("index range column ID is out of range"));
    }
    const IndexTraversalBound lower{
        .expression = index.range->lower.has_value() ? &*index.range->lower : nullptr,
        .implicit_null =
            !index.range->lower.has_value() && index.range->upper.has_value() &&
            table.rowid_alias != index.range->column &&
            !table.columns[index.range->column.value].effective_not_null_conflict.has_value(),
    };
    const IndexTraversalBound upper{
        .expression = index.range->upper.has_value() ? &*index.range->upper : nullptr,
    };
    if (index.range->order == SortOrder::kDescending) {
      forward_start = upper;
      forward_end = lower;
    } else {
      forward_start = lower;
      forward_end = upper;
    }
  }
  const bool reverse = index.direction == PhysicalScanDirection::kReverse;
  const IndexTraversalBound start = reverse ? forward_end : forward_start;
  const IndexTraversalBound end = reverse ? forward_start : forward_end;
  return IndexTraversal{
      .start = start,
      .end = end,
      .has_start_key = !index.equalities.empty() || start.has_range_term(),
      .has_end_key = !index.equalities.empty() || end.has_range_term(),
  };
}

[[nodiscard]] inline ProgramResult<InstructionAddress> EmitIndexTraversalSeek(
    ProgramBuilder& builder, const PhysicalIndexScanNode& index, const IndexTraversal& traversal,
    CursorId cursor, RegisterId first, std::uint32_t count, Label missing) {
  const bool inclusive = traversal.start.inclusive();
  const IndexSeekMode mode =
      index.direction == PhysicalScanDirection::kReverse
          ? (inclusive ? IndexSeekMode::kLessOrEqual : IndexSeekMode::kLess)
          : (inclusive ? IndexSeekMode::kGreaterOrEqual : IndexSeekMode::kGreater);
  return builder.EmitSeekIndex(cursor, first, count, missing, mode);
}

[[nodiscard]] inline ProgramResult<InstructionAddress> EmitIndexTraversalEndCheck(
    ProgramBuilder& builder, const PhysicalIndexScanNode& index, const IndexTraversal& traversal,
    CursorId cursor, RegisterId first, std::uint32_t count, Label end) {
  const bool inclusive = traversal.end.inclusive();
  const IndexRangeEndMode mode =
      index.direction == PhysicalScanDirection::kReverse
          ? (inclusive ? IndexRangeEndMode::kReverseInclusive
                       : IndexRangeEndMode::kReverseExclusive)
          : (inclusive ? IndexRangeEndMode::kInclusive : IndexRangeEndMode::kExclusive);
  return builder.EmitCheckIndexRange(cursor, first, count, end, mode);
}

[[nodiscard]] inline LoweringResult<void> EmitIndexTableLookup(ProgramBuilder& builder,
                                                               CursorId index_cursor,
                                                               CursorId table_cursor,
                                                               RegisterId rowid,
                                                               bool open_table = false) {
  constexpr std::string_view kFailure = "unable to append a bytecode instruction";
  auto read = ConvertProgramResult(builder.Append(ReadFieldInstruction{
                                       .cursor = index_cursor,
                                       .field = CursorFieldId{0},
                                       .output = rowid,
                                   }),
                                   kFailure);
  if (!read.has_value()) {
    return std::unexpected(std::move(read.error()));
  }
  if (open_table) {
    auto opened = ConvertProgramResult(
        builder.Append(OpenReadCursorInstruction{.cursor = table_cursor}), kFailure);
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
  }
  auto sought = ConvertProgramResult(builder.Append(SeekTableRowIdInstruction{
                                         .cursor = table_cursor,
                                         .key = rowid,
                                     }),
                                     kFailure);
  if (!sought.has_value()) {
    return std::unexpected(std::move(sought.error()));
  }
  return {};
}

}  // namespace modern_sqlite::lowering_internal

#endif  // MODERN_SQLITE_SRC_LOWERING_PLAN_LOWERING_INTERNAL_HPP_
