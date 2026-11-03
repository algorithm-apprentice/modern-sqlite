#ifndef MODERN_SQLITE_CATALOG_DEFAULT_EXPRESSION_HPP_
#define MODERN_SQLITE_CATALOG_DEFAULT_EXPRESSION_HPP_

#include <optional>

#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/syntax/ast.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite::internal {

[[nodiscard]] inline std::optional<bool> DefaultBooleanValue(
    const SyntaxTree& tree, const IdentifierExpression& identifier) {
  if (identifier.name.parts.size() != 1U) {
    return std::nullopt;
  }
  const auto token = Slice(tree.source(), identifier.name.parts.front());
  if (!token.has_value()) {
    return std::nullopt;
  }
  if (CatalogNamesEqual(token->bytes(), "true")) {
    return true;
  }
  if (CatalogNamesEqual(token->bytes(), "false")) {
    return false;
  }
  return std::nullopt;
}

}  // namespace modern_sqlite::internal

#endif  // MODERN_SQLITE_CATALOG_DEFAULT_EXPRESSION_HPP_
