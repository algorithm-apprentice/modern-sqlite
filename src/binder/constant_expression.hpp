#ifndef MODERN_SQLITE_BINDER_CONSTANT_EXPRESSION_HPP_
#define MODERN_SQLITE_BINDER_CONSTANT_EXPRESSION_HPP_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

#include "modern_sqlite/base/compiler_continuation.hpp"
#include "modern_sqlite/binder/bound_select.hpp"

namespace modern_sqlite::binder_detail {

enum class ConstantExpressionMode : std::uint8_t { Value, WithoutTruthTests, BinaryGroup };

[[nodiscard]] inline ExpressionWalkControl ConstantExpressionControl(
    const BoundExpression& expression, std::span<const BoundScalarFunction> functions,
    ConstantExpressionMode mode) {
  using Control = ExpressionWalkControl;
  return std::visit(
      [functions, mode](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BoundLiteralExpression> ||
                      std::is_same_v<Payload, BoundLiteralTokenExpression> ||
                      std::is_same_v<Payload, BoundParameterExpression>) {
          return Control::Prune;
        } else if constexpr (std::is_same_v<Payload, BoundUnaryExpression> ||
                             std::is_same_v<Payload, BoundLikelihoodExpression> ||
                             std::is_same_v<Payload, BoundCollateExpression> ||
                             std::is_same_v<Payload, BoundBinaryExpression> ||
                             std::is_same_v<Payload, BoundAliasReferenceExpression> ||
                             std::is_same_v<Payload, BoundInExpression> ||
                             std::is_same_v<Payload, BoundCoalesceExpression> ||
                             std::is_same_v<Payload, BoundConditionalExpression>) {
          return Control::Continue;
        } else if constexpr (std::is_same_v<Payload, BoundComparisonExpression>) {
          return mode != ConstantExpressionMode::WithoutTruthTests ||
                         (payload.comparison != SqlComparison::kIs &&
                          payload.comparison != SqlComparison::kIsNot)
                     ? Control::Continue
                     : Control::Abort;
        } else if constexpr (std::is_same_v<Payload, BoundTruthTestExpression>) {
          return mode == ConstantExpressionMode::WithoutTruthTests ? Control::Abort
                                                                   : Control::Continue;
        } else if constexpr (std::is_same_v<Payload, BoundScalarCallExpression>) {
          assert(payload.function.value() < functions.size());
          return functions[payload.function.value()].deterministic ? Control::Continue
                                                                   : Control::Abort;
        } else {
          return Control::Abort;
        }
      },
      expression.payload);
}

template <typename Observer>
[[nodiscard]] bool InspectConstantExpression(std::span<const BoundExpression> expressions,
                                             std::span<const BoundScalarFunction> functions,
                                             BoundExpressionId id, ConstantExpressionMode mode,
                                             const Observer& observe) {
  using Control = ExpressionWalkControl;
  struct Frame {
    BoundExpressionId expression;
    std::size_t next_child = 0;
  };
  compiler_detail::ContinuationStack<Frame> pending;
  Frame current{.expression = id};
  while (true) {
    assert(current.expression.value() < expressions.size());
    const BoundExpression& expression = expressions[current.expression.value()];
    auto control = Control::Continue;
    if (current.next_child == 0U) {
      control = observe(current.expression);
      if (control == Control::Continue) {
        control = ConstantExpressionControl(expression, functions, mode);
      }
    }
    if (control == Control::Abort) {
      return false;
    }
    const auto child = control == Control::Prune
                           ? std::nullopt
                           : OrdinaryExpressionChild(expression, current.next_child);
    if (child.has_value()) {
      ++current.next_child;
      if (OrdinaryExpressionChild(expression, current.next_child).has_value()) {
        pending.Push(Frame{current});
      }
      current = Frame{.expression = *child};
    } else if (pending.empty()) {
      return true;
    } else {
      current = pending.Pop();
    }
  }
}

}  // namespace modern_sqlite::binder_detail

#endif  // MODERN_SQLITE_BINDER_CONSTANT_EXPRESSION_HPP_
