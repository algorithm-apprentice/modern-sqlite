#ifndef MODERN_SQLITE_SRC_LOWERING_PLAN_LOWERING_INTERNAL_HPP_
#define MODERN_SQLITE_SRC_LOWERING_PLAN_LOWERING_INTERNAL_HPP_

#include <cassert>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

struct IndexTraversal {
  const PhysicalIndexBound* start = nullptr;
  const PhysicalIndexBound* end = nullptr;
  bool has_start_key = false;
  bool has_end_key = false;
};

[[nodiscard]] inline IndexTraversal ResolveIndexTraversal(
    const PhysicalIndexScanNode& index) noexcept {
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
  return IndexTraversal{
      .start = start,
      .end = end,
      .has_start_key = !index.equalities.empty() || start != nullptr,
      .has_end_key = !index.equalities.empty() || end != nullptr,
  };
}

[[nodiscard]] inline ProgramResult<InstructionAddress> EmitIndexTraversalSeek(
    ProgramBuilder& builder, const PhysicalIndexScanNode& index, const IndexTraversal& traversal,
    CursorId cursor, RegisterId first, std::uint32_t count, Label missing) {
  const bool inclusive = traversal.start == nullptr || traversal.start->inclusive;
  const IndexSeekMode mode =
      index.direction == PhysicalScanDirection::kReverse
          ? (inclusive ? IndexSeekMode::kLessOrEqual : IndexSeekMode::kLess)
          : (inclusive ? IndexSeekMode::kGreaterOrEqual : IndexSeekMode::kGreater);
  return builder.EmitSeekIndex(cursor, first, count, missing, mode);
}

[[nodiscard]] inline ProgramResult<InstructionAddress> EmitIndexTraversalEndCheck(
    ProgramBuilder& builder, const PhysicalIndexScanNode& index, const IndexTraversal& traversal,
    CursorId cursor, RegisterId first, std::uint32_t count, Label end) {
  const bool inclusive = traversal.end == nullptr || traversal.end->inclusive;
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
