#ifndef MODERN_SQLITE_LOWERING_PLAN_LOWERING_HPP_
#define MODERN_SQLITE_LOWERING_PLAN_LOWERING_HPP_

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"

namespace modern_sqlite {

enum class PlanLoweringErrorCode : std::uint8_t {
  kInvalidInput,
  kUnsupportedPlan,
  kResourceLimit,
  kInternalInvariant,
};

struct PlanLoweringError {
  PlanLoweringErrorCode code = PlanLoweringErrorCode::kInternalInvariant;
  std::optional<ProgramError> program_error{};
  std::string detail{};

  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view PlanLoweringErrorCodeName(PlanLoweringErrorCode code) noexcept;

using LowerPlanResult = std::expected<BytecodeProgram, PlanLoweringError>;

[[nodiscard]] LowerPlanResult LowerPlan(const PhysicalPlan& plan, ProgramLimits limits = {});
[[nodiscard]] LowerPlanResult LowerPlan(const PhysicalMutationPlan& plan,
                                        ProgramLimits limits = {});

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_LOWERING_PLAN_LOWERING_HPP_
