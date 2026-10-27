#ifndef MODERN_SQLITE_LOWERING_PLAN_LOWERING_HPP_
#define MODERN_SQLITE_LOWERING_PLAN_LOWERING_HPP_

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"

namespace modern_sqlite {

struct BindError;

enum class PlanLoweringErrorCode : std::uint8_t {
  kInvalidInput,
  kUnsupportedPlan,
  kResourceLimit,
  kInternalInvariant,
  LiteralMaterialization,
};

struct PlanLoweringError {
  PlanLoweringErrorCode code = PlanLoweringErrorCode::kInternalInvariant;
  std::variant<std::monostate, ProgramError, std::shared_ptr<const BindError>> cause{};
  std::string detail{};

  [[nodiscard]] const ProgramError* program_error() const noexcept {
    return std::get_if<ProgramError>(&cause);
  }
  [[nodiscard]] const BindError* bind_error() const noexcept {
    const auto* error = std::get_if<std::shared_ptr<const BindError>>(&cause);
    return error != nullptr ? error->get() : nullptr;
  }
  [[nodiscard]] ErrorCode base_error_code() const noexcept;
};

[[nodiscard]] std::string_view PlanLoweringErrorCodeName(PlanLoweringErrorCode code) noexcept;

using LowerPlanResult = std::expected<BytecodeProgram, PlanLoweringError>;

[[nodiscard]] LowerPlanResult LowerPlan(const PhysicalPlan& plan, ProgramLimits limits = {});
[[nodiscard]] LowerPlanResult LowerPlan(const PhysicalMutationPlan& plan,
                                        ProgramLimits limits = {});

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_LOWERING_PLAN_LOWERING_HPP_
