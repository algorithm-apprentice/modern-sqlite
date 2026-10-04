#ifndef MODERN_SQLITE_LOWERING_READ_LOWERING_HPP_
#define MODERN_SQLITE_LOWERING_READ_LOWERING_HPP_

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"

namespace modern_sqlite {

enum class ReadLoweringErrorCode : std::uint8_t {
  kInvalidInput,
  kResourceLimit,
  kInternalInvariant,
};

struct ReadLoweringError {
  ReadLoweringErrorCode code = ReadLoweringErrorCode::kInternalInvariant;
  std::optional<ProgramError> program_error{};
  std::string detail{};

  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view ReadLoweringErrorCodeName(ReadLoweringErrorCode code) noexcept;

using LowerReadPlanResult = std::expected<BytecodeProgram, ReadLoweringError>;

[[nodiscard]] LowerReadPlanResult LowerReadPlan(const PhysicalPlan& plan,
                                                ProgramLimits limits = {});

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_LOWERING_READ_LOWERING_HPP_
