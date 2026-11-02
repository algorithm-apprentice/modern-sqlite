#ifndef MODERN_SQLITE_BINDER_NATIVE_TRUTH_HPP_
#define MODERN_SQLITE_BINDER_NATIVE_TRUTH_HPP_

#include <cassert>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

#include "modern_sqlite/base/compiler_continuation.hpp"
#include "modern_sqlite/binder/bound_select.hpp"

namespace modern_sqlite::binder_detail {

[[nodiscard]] inline const BoundBinaryExpression* LogicalOperandBinary(
    std::span<const BoundExpression> expressions, BoundExpressionId id) {
  assert(id.value() < expressions.size());
  while (const auto* alias =
             std::get_if<BoundAliasReferenceExpression>(&expressions[id.value()].payload)) {
    id = alias->target;
    assert(id.value() < expressions.size());
  }
  const auto* child = std::get_if<BoundBinaryExpression>(&expressions[id.value()].payload);
  return child != nullptr && (child->operation == BoundBinaryOperation::kLogicalAnd ||
                              child->operation == BoundBinaryOperation::kLogicalOr)
             ? child
             : nullptr;
}

template <typename TruthObserver>
[[nodiscard]] auto SelectLogicalOperand(std::span<const BoundExpression> expressions,
                                        const BoundBinaryExpression& binary,
                                        const TruthObserver& observe_truth)
    -> std::expected<
        std::optional<BoundExpressionId>,
        typename std::invoke_result_t<const TruthObserver&, BoundExpressionId>::error_type> {
  if (binary.operation != BoundBinaryOperation::kLogicalAnd &&
      binary.operation != BoundBinaryOperation::kLogicalOr) {
    return std::optional<BoundExpressionId>{};
  }
  enum class Phase : std::uint8_t { Right, Left, Observe };
  struct Frame {
    const BoundBinaryExpression* expression;
    Phase phase = Phase::Right;
    BoundExpressionId left{0};
    BoundExpressionId right{0};
  };
  compiler_detail::ContinuationStack<Frame> pending;
  Frame current{.expression = &binary};
  while (true) {
    if (current.phase == Phase::Right) {
      current.phase = Phase::Left;
      if (const auto* child = LogicalOperandBinary(expressions, current.expression->right);
          child != nullptr) {
        pending.Push(Frame{current});
        current = Frame{.expression = child};
        continue;
      }
      current.right = current.expression->right;
    }
    if (current.phase == Phase::Left) {
      current.phase = Phase::Observe;
      if (const auto* child = LogicalOperandBinary(expressions, current.expression->left);
          child != nullptr) {
        pending.Push(Frame{current});
        current = Frame{.expression = child};
        continue;
      }
      current.left = current.expression->left;
    }
    const auto left = observe_truth(current.left);
    const auto right = observe_truth(current.right);
    if (!left.has_value()) {
      return std::unexpected(left.error());
    }
    if (!right.has_value()) {
      return std::unexpected(right.error());
    }
    const bool is_and = current.expression->operation == BoundBinaryOperation::kLogicalAnd;
    std::optional<BoundExpressionId> selected;
    if (*left == BoundTruthHint::kAlwaysTrue || *right == BoundTruthHint::kAlwaysFalse) {
      selected = is_and ? current.right : current.left;
    } else if (*right == BoundTruthHint::kAlwaysTrue || *left == BoundTruthHint::kAlwaysFalse) {
      selected = is_and ? current.left : current.right;
    }
    if (pending.empty()) {
      return selected;
    }
    current = pending.Pop();
    if (current.phase == Phase::Left) {
      current.right = selected.value_or(current.expression->right);
    } else {
      current.left = selected.value_or(current.expression->left);
    }
  }
}

}  // namespace modern_sqlite::binder_detail

#endif  // MODERN_SQLITE_BINDER_NATIVE_TRUTH_HPP_
