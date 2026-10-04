#ifndef MODERN_SQLITE_SYNTAX_TOKEN_TEXT_HPP_
#define MODERN_SQLITE_SYNTAX_TOKEN_TEXT_HPP_

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace modern_sqlite::internal {

enum class DequoteSqlTokenError {
  kUnterminated,
};

[[nodiscard]] inline std::expected<std::string, DequoteSqlTokenError> DequoteSqlToken(
    std::string_view token) {
  if (token.empty()) {
    return std::string{};
  }
  char closing = '\0';
  switch (token.front()) {
    case '\'':
    case '"':
    case '`':
      closing = token.front();
      break;
    case '[':
      closing = ']';
      break;
    default:
      return std::string{token};
  }
  if (token.size() < 2U || token.back() != closing) {
    return std::unexpected(DequoteSqlTokenError::kUnterminated);
  }

  std::string value;
  value.reserve(token.size() - 2U);
  for (std::size_t index = 1; index + 1U < token.size(); ++index) {
    if (token[index] == closing && index + 2U < token.size() && token[index + 1U] == closing) {
      value.push_back(closing);
      ++index;
    } else {
      value.push_back(token[index]);
    }
  }
  return value;
}

}  // namespace modern_sqlite::internal

#endif  // MODERN_SQLITE_SYNTAX_TOKEN_TEXT_HPP_
