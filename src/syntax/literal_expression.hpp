#ifndef MODERN_SQLITE_SYNTAX_LITERAL_EXPRESSION_HPP_
#define MODERN_SQLITE_SYNTAX_LITERAL_EXPRESSION_HPP_

#include <variant>

#include "modern_sqlite/syntax/ast.hpp"

namespace modern_sqlite::internal {

[[nodiscard]] inline const LiteralExpression* UnaryNumericLiteral(const SyntaxTree& tree,
                                                                  ExpressionId id) noexcept {
  while (true) {
    const Expression& expression = tree.expression(id);
    if (const auto* literal = std::get_if<LiteralExpression>(&expression.payload);
        literal != nullptr) {
      return literal;
    }
    if (const auto* parenthesized = std::get_if<ParenthesizedExpression>(&expression.payload);
        parenthesized != nullptr) {
      id = parenthesized->inner;
      continue;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.payload);
        unary != nullptr && unary->op == UnaryOperator::kPositive) {
      id = unary->operand;
      continue;
    }
    return nullptr;
  }
}

}  // namespace modern_sqlite::internal

#endif  // MODERN_SQLITE_SYNTAX_LITERAL_EXPRESSION_HPP_
