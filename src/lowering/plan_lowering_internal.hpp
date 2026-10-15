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
