#ifndef MODERN_SQLITE_SYNTAX_PARSER_HPP_
#define MODERN_SQLITE_SYNTAX_PARSER_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "modern_sqlite/syntax/ast.hpp"
#include "modern_sqlite/syntax/lexer.hpp"

namespace modern_sqlite {

enum class ParseErrorCode : std::uint8_t {
  kIllegalToken,
  kUnexpectedToken,
  kUnsupportedSyntax,
  kExpressionDepthExceeded,
  kParserDepthExceeded,
  kInternalInvariant,
};

enum class ParseExpectation : std::uint8_t {
  kNone,
  kStatement,
  kExpression,
  kName,
  kRightParenthesis,
  kCommaOrRightParenthesis,
  kEndOfStatement,
  kColumnDefinition,
  kConstraint,
  kTableOption,
};

struct ParseError {
  ParseErrorCode code = ParseErrorCode::kUnexpectedToken;
  SourceSpan span{};
  TokenKind actual = TokenKind::kEndOfInput;
  ParseExpectation expected = ParseExpectation::kNone;
  ByteOffset next_offset{};

  constexpr auto operator<=>(const ParseError&) const noexcept = default;
};

struct ParseOutput {
  std::optional<SyntaxTree> tree;
  ByteOffset next_offset;
};

using ParseResult = std::expected<ParseOutput, ParseError>;

inline constexpr std::size_t kMaximumExpressionConstructionDepth = 1000;
inline constexpr std::size_t kMaximumParserRecursionDepth = 512;

[[nodiscard]] constexpr std::string_view ParseErrorCodeName(ParseErrorCode code) noexcept {
  switch (code) {
    case ParseErrorCode::kIllegalToken:
      return "illegal_token";
    case ParseErrorCode::kUnexpectedToken:
      return "unexpected_token";
    case ParseErrorCode::kUnsupportedSyntax:
      return "unsupported_syntax";
    case ParseErrorCode::kExpressionDepthExceeded:
      return "expression_depth_exceeded";
    case ParseErrorCode::kParserDepthExceeded:
      return "parser_depth_exceeded";
    case ParseErrorCode::kInternalInvariant:
      return "internal_invariant";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view ParseExpectationName(
    ParseExpectation expectation) noexcept {
  switch (expectation) {
    case ParseExpectation::kNone:
      return "none";
    case ParseExpectation::kStatement:
      return "statement";
    case ParseExpectation::kExpression:
      return "expression";
    case ParseExpectation::kName:
      return "name";
    case ParseExpectation::kRightParenthesis:
      return "right_parenthesis";
    case ParseExpectation::kCommaOrRightParenthesis:
      return "comma_or_right_parenthesis";
    case ParseExpectation::kEndOfStatement:
      return "end_of_statement";
    case ParseExpectation::kColumnDefinition:
      return "column_definition";
    case ParseExpectation::kConstraint:
      return "constraint";
    case ParseExpectation::kTableOption:
      return "table_option";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view ParseErrorMessage(const ParseError& error) noexcept {
  switch (error.code) {
    case ParseErrorCode::kIllegalToken:
      return "unrecognized token";
    case ParseErrorCode::kUnexpectedToken:
      return error.actual == TokenKind::kEndOfInput ? "incomplete input" : "syntax error";
    case ParseErrorCode::kUnsupportedSyntax:
      return "unsupported syntax";
    case ParseErrorCode::kExpressionDepthExceeded:
      return "expression depth exceeded";
    case ParseErrorCode::kParserDepthExceeded:
      return "parser depth exceeded";
    case ParseErrorCode::kInternalInvariant:
      return "internal parser invariant failed";
  }
  return "unknown parser error";
}

[[nodiscard]] ParseResult ParseOne(Utf8View source);

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_SYNTAX_PARSER_HPP_
