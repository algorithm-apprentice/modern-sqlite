#include "modern_sqlite/syntax/parser.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/syntax/ast.hpp"
#include "modern_sqlite/syntax/lexer.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<ParseOutput>);
static_assert(!std::is_copy_assignable_v<ParseOutput>);
static_assert(std::is_nothrow_move_constructible_v<ParseOutput>);
static_assert(std::is_nothrow_move_assignable_v<ParseOutput>);
static_assert(kMaximumExpressionConstructionDepth == 1000U);
static_assert(kMaximumParserRecursionDepth == 512U);

[[nodiscard]] ParseOutput ParseOrThrow(std::string_view sql) {
  ParseResult result = ParseOne(Utf8View{sql});
  if (!result.has_value()) {
    throw std::runtime_error{std::string{ParseErrorMessage(result.error())}};
  }
  return std::move(*result);
}

[[nodiscard]] const SyntaxTree& RequiredTree(const ParseOutput& output) {
  if (!output.tree.has_value()) {
    throw std::runtime_error{"parser returned no syntax tree"};
  }
  return *output.tree;
}

[[nodiscard]] std::string_view SpanText(Utf8View source, SourceSpan span) {
  const auto text = Slice(source, span);
  if (!text.has_value()) {
    throw std::runtime_error{"invalid test span"};
  }
  return text->bytes();
}

[[nodiscard]] std::string_view SpanText(const SyntaxTree& tree, SourceSpan span) {
  return SpanText(tree.source(), span);
}

[[nodiscard]] ExpressionId FindExpression(const SyntaxTree& tree, std::string_view text) {
  for (std::size_t index = 0; index < tree.expressions().size(); ++index) {
    if (SpanText(tree, tree.expressions()[index].span) == text) {
      return ExpressionId{index};
    }
  }
  throw std::runtime_error{"expression text not found"};
}

[[nodiscard]] const SelectStatement& Select(const SyntaxTree& tree) {
  return std::get<SelectStatement>(tree.statement());
}

[[nodiscard]] const CreateTableStatement& CreateTable(const SyntaxTree& tree) {
  return std::get<CreateTableStatement>(tree.statement());
}

[[nodiscard]] const CreateIndexStatement& CreateIndex(const SyntaxTree& tree) {
  return std::get<CreateIndexStatement>(tree.statement());
}

[[nodiscard]] const Expression& ResultExpression(const SyntaxTree& tree,
                                                 std::size_t result_index = 0) {
  return tree.expression(Select(tree).result_columns.at(result_index).expression);
}

template <typename T>
[[nodiscard]] const T& RequiredOptional(const std::optional<T>& value) {
  if (!value.has_value()) {
    throw std::runtime_error{"required optional test value is absent"};
  }
  return value.value();
}

struct ParserCorpusCase {
  std::string name;
  std::string sqlite_outcome;
  std::string modern_outcome;
  std::string sql;
};

[[nodiscard]] std::filesystem::path ParserFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "parser" / "sqlite-3.54.0-parser.tsv";
}

[[nodiscard]] std::vector<ParserCorpusCase> ReadParserCorpus() {
  std::ifstream input{ParserFixturePath()};
  if (!input) {
    throw std::runtime_error{"unable to open parser fixture"};
  }
  std::vector<ParserCorpusCase> cases;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t first_tab = line.find('\t');
    const std::size_t second_tab = line.find('\t', first_tab + 1U);
    const std::size_t third_tab = line.find('\t', second_tab + 1U);
    if (first_tab == std::string::npos || second_tab == std::string::npos ||
        third_tab == std::string::npos) {
      throw std::runtime_error{"invalid parser fixture row"};
    }
    cases.push_back(ParserCorpusCase{
        .name = line.substr(0, first_tab),
        .sqlite_outcome = line.substr(first_tab + 1U, second_tab - first_tab - 1U),
        .modern_outcome = line.substr(second_tab + 1U, third_tab - second_tab - 1U),
        .sql = line.substr(third_tab + 1U),
    });
  }
  return cases;
}

TEST(ParserApi, ProvidesStableNamesAndMessages) {
  EXPECT_EQ("illegal_token", ParseErrorCodeName(ParseErrorCode::kIllegalToken));
  EXPECT_EQ("unexpected_token", ParseErrorCodeName(ParseErrorCode::kUnexpectedToken));
  EXPECT_EQ("unsupported_syntax", ParseErrorCodeName(ParseErrorCode::kUnsupportedSyntax));
  EXPECT_EQ("expression_depth_exceeded",
            ParseErrorCodeName(ParseErrorCode::kExpressionDepthExceeded));
  EXPECT_EQ("parser_depth_exceeded", ParseErrorCodeName(ParseErrorCode::kParserDepthExceeded));
  EXPECT_EQ("internal_invariant", ParseErrorCodeName(ParseErrorCode::kInternalInvariant));
  EXPECT_EQ("unknown",
            ParseErrorCodeName(static_cast<ParseErrorCode>(255)));  // NOLINT

  EXPECT_EQ("statement", ParseExpectationName(ParseExpectation::kStatement));
  EXPECT_EQ("expression", ParseExpectationName(ParseExpectation::kExpression));
  EXPECT_EQ("comma_or_right_parenthesis",
            ParseExpectationName(ParseExpectation::kCommaOrRightParenthesis));
  EXPECT_EQ("unknown",
            ParseExpectationName(static_cast<ParseExpectation>(255)));  // NOLINT

  EXPECT_EQ("unrecognized token",
            ParseErrorMessage(ParseError{.code = ParseErrorCode::kIllegalToken}));
  EXPECT_EQ("incomplete input", ParseErrorMessage(ParseError{
                                    .code = ParseErrorCode::kUnexpectedToken,
                                    .actual = TokenKind::kEndOfInput,
                                }));
  EXPECT_EQ("syntax error", ParseErrorMessage(ParseError{
                                .code = ParseErrorCode::kUnexpectedToken,
                                .actual = TokenKind::kSelect,
                            }));
  EXPECT_EQ("unsupported syntax",
            ParseErrorMessage(ParseError{.code = ParseErrorCode::kUnsupportedSyntax}));
  EXPECT_EQ("expression depth exceeded",
            ParseErrorMessage(ParseError{.code = ParseErrorCode::kExpressionDepthExceeded}));
  EXPECT_EQ("parser depth exceeded",
            ParseErrorMessage(ParseError{.code = ParseErrorCode::kParserDepthExceeded}));
  EXPECT_EQ("internal parser invariant failed",
            ParseErrorMessage(ParseError{.code = ParseErrorCode::kInternalInvariant}));
}

TEST(Parser, MatchesPinnedSqliteDifferentialCorpusPolicy) {
  const std::vector<ParserCorpusCase> cases = ReadParserCorpus();
  ASSERT_EQ(57U, cases.size());

  for (const ParserCorpusCase& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    EXPECT_TRUE(test_case.sqlite_outcome == "ok" || test_case.sqlite_outcome == "semantic_error" ||
                test_case.sqlite_outcome == "syntax_error");
    ParseResult result = ParseOne(Utf8View{test_case.sql});
    if (test_case.modern_outcome == "ok") {
      ASSERT_TRUE(result.has_value()) << ParseErrorMessage(result.error());
      EXPECT_TRUE(result->tree.has_value());
    } else {
      ASSERT_FALSE(result.has_value());
      ParseErrorCode expected = ParseErrorCode::kUnexpectedToken;
      if (test_case.modern_outcome == "unsupported") {
        expected = ParseErrorCode::kUnsupportedSyntax;
      } else if (test_case.modern_outcome == "illegal") {
        expected = ParseErrorCode::kIllegalToken;
      } else {
        ASSERT_EQ("unexpected", test_case.modern_outcome);
      }
      EXPECT_EQ(expected, result.error().code);
    }
  }
}

TEST(Parser, UsesPrepareStyleStatementSlicesAndTailOffsets) {
  const std::string input = " \n;/* empty */; SELECT 1 ; -- gap\n SELECT 2";
  const std::size_t statement_begin = input.find("SELECT 1");
  const std::size_t semicolon = input.find(';', statement_begin);

  const ParseOutput output = ParseOrThrow(input);
  ASSERT_TRUE(output.tree.has_value());
  const SyntaxTree& first_tree = RequiredTree(output);
  EXPECT_EQ(semicolon + 1U, output.next_offset.value());
  EXPECT_EQ(input.substr(statement_begin, semicolon + 1U - statement_begin),
            first_tree.source().bytes());

  const SelectStatement& select = Select(first_tree);
  EXPECT_EQ("SELECT 1", SpanText(first_tree, select.span));
  ASSERT_EQ(1U, select.result_columns.size());
  EXPECT_EQ("1", SpanText(first_tree, select.result_columns.front().span));

  const ParseOutput second =
      ParseOrThrow(std::string_view{input}.substr(output.next_offset.value()));
  ASSERT_TRUE(second.tree.has_value());
  EXPECT_EQ("SELECT 2", RequiredTree(second).source().bytes());
  EXPECT_EQ(input.size() - output.next_offset.value(), second.next_offset.value());
}

TEST(Parser, ReturnsNoTreeForTriviaAndEmptyStatements) {
  const std::string input = " \n; /* one */ ; -- two\n ";
  const ParseOutput output = ParseOrThrow(input);
  EXPECT_FALSE(output.tree.has_value());
  EXPECT_EQ(input.size(), output.next_offset.value());

  const ParseOutput empty = ParseOrThrow({});
  EXPECT_FALSE(empty.tree.has_value());
  EXPECT_EQ(0U, empty.next_offset.value());
}

TEST(Parser, StopsAtEmbeddedNullAndExcludesTrailingTriviaWithoutSemicolon) {
  std::string input{"  SELECT 1  /* trailing */"};
  input.push_back('\0');
  input.append("SELECT 2");
  const std::size_t logical_end = input.find('\0');

  const ParseOutput output = ParseOrThrow(std::string_view{input.data(), input.size()});
  ASSERT_TRUE(output.tree.has_value());
  const SyntaxTree& tree = RequiredTree(output);
  EXPECT_EQ("SELECT 1", tree.source().bytes());
  EXPECT_EQ(logical_end, output.next_offset.value());
  EXPECT_EQ("SELECT 1", SpanText(tree, Select(tree).span));
}

TEST(Parser, DoesNotTokenizePastATerminatingSemicolon) {
  const std::string input = "SELECT 1; SELECT @";
  const ParseOutput output = ParseOrThrow(input);
  ASSERT_TRUE(output.tree.has_value());
  EXPECT_EQ("SELECT 1;", RequiredTree(output).source().bytes());
  EXPECT_EQ(input.find(';') + 1U, output.next_offset.value());
}

TEST(Parser, BuildsAllInitialPrimaryExpressionKindsWithoutDecoding) {
  const std::string sql =
      "SELECT NULL, 1, 1_2.5, 'x', x'CAFE', CURRENT_DATE, ?1, true, "
      "main.t.c, fn(DISTINCT a), tab.*;";
  const ParseOutput output = ParseOrThrow(sql);
  const SyntaxTree& tree = RequiredTree(output);
  ASSERT_EQ(11U, Select(tree).result_columns.size());

  const auto& null_literal =
      std::get<LiteralExpression>(tree.expression(FindExpression(tree, "NULL")).payload);
  const auto& integer =
      std::get<LiteralExpression>(tree.expression(FindExpression(tree, "1")).payload);
  const auto& real =
      std::get<LiteralExpression>(tree.expression(FindExpression(tree, "1_2.5")).payload);
  const auto& string =
      std::get<LiteralExpression>(tree.expression(FindExpression(tree, "'x'")).payload);
  const auto& blob =
      std::get<LiteralExpression>(tree.expression(FindExpression(tree, "x'CAFE'")).payload);
  const auto& current =
      std::get<LiteralExpression>(tree.expression(FindExpression(tree, "CURRENT_DATE")).payload);
  EXPECT_EQ(LiteralKind::kNull, null_literal.kind);
  EXPECT_EQ(LiteralKind::kInteger, integer.kind);
  EXPECT_EQ(LiteralKind::kReal, real.kind);
  EXPECT_EQ(LiteralKind::kString, string.kind);
  EXPECT_EQ(LiteralKind::kBlob, blob.kind);
  EXPECT_EQ(LiteralKind::kCurrentDate, current.kind);

  EXPECT_TRUE(std::holds_alternative<VariableExpression>(
      tree.expression(FindExpression(tree, "?1")).payload));
  EXPECT_TRUE(std::holds_alternative<IdentifierExpression>(
      tree.expression(FindExpression(tree, "true")).payload));

  const auto& qualified =
      std::get<IdentifierExpression>(tree.expression(FindExpression(tree, "main.t.c")).payload);
  ASSERT_EQ(3U, qualified.name.parts.size());
  EXPECT_EQ("main", SpanText(tree, qualified.name.parts[0]));
  EXPECT_EQ("t", SpanText(tree, qualified.name.parts[1]));
  EXPECT_EQ("c", SpanText(tree, qualified.name.parts[2]));

  const ExpressionId call_id = FindExpression(tree, "fn(DISTINCT a)");
  const auto& call = std::get<FunctionCallExpression>(tree.expression(call_id).payload);
  EXPECT_TRUE(call.distinct);
  ASSERT_EQ(1U, call.arguments.size());
  EXPECT_LT(call.arguments.front().value, call_id.value);

  const auto& wildcard =
      std::get<WildcardExpression>(tree.expression(FindExpression(tree, "tab.*")).payload);
  ASSERT_TRUE(wildcard.qualifier.has_value());
  const QualifiedName& qualifier = RequiredOptional(wildcard.qualifier);
  ASSERT_EQ(1U, qualifier.parts.size());
  EXPECT_EQ("tab", SpanText(tree, qualifier.parts.front()));
}

TEST(Parser, ClassifiesHexadecimalQuotedNumbersAsIntegers) {
  const ParseOutput output = ParseOrThrow("SELECT 0xDE_AD, 1_2.5E+1");
  const SyntaxTree& tree = RequiredTree(output);
  const auto& hexadecimal = std::get<LiteralExpression>(ResultExpression(tree, 0).payload);
  const auto& decimal = std::get<LiteralExpression>(ResultExpression(tree, 1).payload);
  EXPECT_EQ(LiteralKind::kInteger, hexadecimal.kind);
  EXPECT_EQ(LiteralKind::kReal, decimal.kind);
}

TEST(Parser, AppliesPinnedPrecedenceAndAssociativity) {
  const ParseOutput output = ParseOrThrow("SELECT NOT a = b OR c AND d + e * f");
  const SyntaxTree& tree = RequiredTree(output);
  const auto& root = std::get<BinaryExpression>(ResultExpression(tree).payload);
  EXPECT_EQ(BinaryOperator::kOr, root.op);

  const auto& logical_not = std::get<UnaryExpression>(tree.expression(root.left).payload);
  EXPECT_EQ(UnaryOperator::kNot, logical_not.op);
  const auto& equality = std::get<BinaryExpression>(tree.expression(logical_not.operand).payload);
  EXPECT_EQ(BinaryOperator::kEqual, equality.op);

  const auto& logical_and = std::get<BinaryExpression>(tree.expression(root.right).payload);
  EXPECT_EQ(BinaryOperator::kAnd, logical_and.op);
  const auto& addition = std::get<BinaryExpression>(tree.expression(logical_and.right).payload);
  EXPECT_EQ(BinaryOperator::kAdd, addition.op);
  const auto& multiplication = std::get<BinaryExpression>(tree.expression(addition.right).payload);
  EXPECT_EQ(BinaryOperator::kMultiply, multiplication.op);

  const ParseOutput left_associative = ParseOrThrow("SELECT 10 - 3 - 2");
  const SyntaxTree& left_tree = RequiredTree(left_associative);
  const auto& outer = std::get<BinaryExpression>(ResultExpression(left_tree).payload);
  EXPECT_EQ(BinaryOperator::kSubtract, outer.op);
  EXPECT_TRUE(std::holds_alternative<BinaryExpression>(left_tree.expression(outer.left).payload));
  EXPECT_TRUE(std::holds_alternative<LiteralExpression>(left_tree.expression(outer.right).payload));
}

TEST(Parser, MapsEverySupportedOperator) {
  struct BinaryCase {
    std::string_view spelling;
    BinaryOperator expected;
  };
  constexpr std::array binary_cases{
      BinaryCase{.spelling = "OR", .expected = BinaryOperator::kOr},
      BinaryCase{.spelling = "AND", .expected = BinaryOperator::kAnd},
      BinaryCase{.spelling = "<", .expected = BinaryOperator::kLess},
      BinaryCase{.spelling = "<=", .expected = BinaryOperator::kLessOrEqual},
      BinaryCase{.spelling = ">", .expected = BinaryOperator::kGreater},
      BinaryCase{.spelling = ">=", .expected = BinaryOperator::kGreaterOrEqual},
      BinaryCase{.spelling = "=", .expected = BinaryOperator::kEqual},
      BinaryCase{.spelling = "!=", .expected = BinaryOperator::kNotEqual},
      BinaryCase{.spelling = "IS", .expected = BinaryOperator::kIs},
      BinaryCase{.spelling = "IS NOT", .expected = BinaryOperator::kIsNot},
      BinaryCase{.spelling = "+", .expected = BinaryOperator::kAdd},
      BinaryCase{.spelling = "-", .expected = BinaryOperator::kSubtract},
      BinaryCase{.spelling = "*", .expected = BinaryOperator::kMultiply},
      BinaryCase{.spelling = "/", .expected = BinaryOperator::kDivide},
      BinaryCase{.spelling = "%", .expected = BinaryOperator::kRemainder},
      BinaryCase{.spelling = "<<", .expected = BinaryOperator::kLeftShift},
      BinaryCase{.spelling = ">>", .expected = BinaryOperator::kRightShift},
      BinaryCase{.spelling = "&", .expected = BinaryOperator::kBitwiseAnd},
      BinaryCase{.spelling = "|", .expected = BinaryOperator::kBitwiseOr},
      BinaryCase{.spelling = "||", .expected = BinaryOperator::kConcatenate},
      BinaryCase{.spelling = "LIKE", .expected = BinaryOperator::kLike},
      BinaryCase{.spelling = "NOT LIKE", .expected = BinaryOperator::kNotLike},
      BinaryCase{.spelling = "GLOB", .expected = BinaryOperator::kGlob},
      BinaryCase{.spelling = "NOT GLOB", .expected = BinaryOperator::kNotGlob},
      BinaryCase{.spelling = "REGEXP", .expected = BinaryOperator::kRegexp},
      BinaryCase{.spelling = "NOT REGEXP", .expected = BinaryOperator::kNotRegexp},
      BinaryCase{.spelling = "MATCH", .expected = BinaryOperator::kMatch},
      BinaryCase{.spelling = "NOT MATCH", .expected = BinaryOperator::kNotMatch},
  };

  for (const BinaryCase& test_case : binary_cases) {
    SCOPED_TRACE(test_case.spelling);
    const std::string sql = "SELECT a " + std::string{test_case.spelling} + " b";
    const ParseOutput output = ParseOrThrow(sql);
    const auto& binary = std::get<BinaryExpression>(ResultExpression(RequiredTree(output)).payload);
    EXPECT_EQ(test_case.expected, binary.op);
    EXPECT_EQ(test_case.spelling, SpanText(RequiredTree(output), binary.operator_span));
  }

  struct UnaryCase {
    std::string_view spelling;
    UnaryOperator expected;
  };
  constexpr std::array unary_cases{
      UnaryCase{.spelling = "+", .expected = UnaryOperator::kPositive},
      UnaryCase{.spelling = "-", .expected = UnaryOperator::kNegative},
      UnaryCase{.spelling = "~", .expected = UnaryOperator::kBitwiseNot},
      UnaryCase{.spelling = "NOT ", .expected = UnaryOperator::kNot},
  };
  for (const UnaryCase& test_case : unary_cases) {
    SCOPED_TRACE(test_case.spelling);
    const std::string sql = "SELECT " + std::string{test_case.spelling} + "a";
    const ParseOutput output = ParseOrThrow(sql);
    const auto& unary = std::get<UnaryExpression>(ResultExpression(RequiredTree(output)).payload);
    EXPECT_EQ(test_case.expected, unary.op);
  }
}

TEST(Parser, ParsesFunctionsCollationAndParentheses) {
  const ParseOutput output =
      ParseOrThrow("SELECT f(), count(*), f(ALL a), f(DISTINCT a), (a COLLATE nocase)");
  const SyntaxTree& tree = RequiredTree(output);

  const auto& empty =
      std::get<FunctionCallExpression>(tree.expression(FindExpression(tree, "f()")).payload);
  EXPECT_TRUE(empty.arguments.empty());
  EXPECT_FALSE(empty.distinct);

  const auto& star =
      std::get<FunctionCallExpression>(tree.expression(FindExpression(tree, "count(*)")).payload);
  ASSERT_EQ(1U, star.arguments.size());
  EXPECT_TRUE(
      std::holds_alternative<WildcardExpression>(tree.expression(star.arguments[0]).payload));

  const auto& all =
      std::get<FunctionCallExpression>(tree.expression(FindExpression(tree, "f(ALL a)")).payload);
  EXPECT_FALSE(all.distinct);
  const auto& distinct = std::get<FunctionCallExpression>(
      tree.expression(FindExpression(tree, "f(DISTINCT a)")).payload);
  EXPECT_TRUE(distinct.distinct);

  const auto& parenthesized = std::get<ParenthesizedExpression>(
      tree.expression(FindExpression(tree, "(a COLLATE nocase)")).payload);
  const auto& collate = std::get<CollateExpression>(tree.expression(parenthesized.inner).payload);
  EXPECT_EQ("COLLATE", SpanText(tree, collate.keyword));
  EXPECT_EQ("nocase", SpanText(tree, collate.collation));
}

TEST(Parser, RejectsAQuantifiedFunctionWildcard) {
  ParseResult result = ParseOne(Utf8View{"SELECT f(ALL *)"});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, result.error().code);
  EXPECT_EQ(TokenKind::kAsterisk, result.error().actual);
  EXPECT_EQ(ParseExpectation::kExpression, result.error().expected);

  ParseResult malformed_order = ParseOne(Utf8View{"SELECT f(ORDER)"});
  ASSERT_FALSE(malformed_order.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, malformed_order.error().code);
  EXPECT_EQ(TokenKind::kOrder, malformed_order.error().actual);
}

TEST(Parser, ParsesSelectClausesAndNormalizesLimitForms) {
  const ParseOutput output = ParseOrThrow(
      "SELECT DISTINCT a AS x, b y FROM main.t AS source "
      "WHERE a > 0 LIMIT 10 OFFSET 2;");
  const SyntaxTree& tree = RequiredTree(output);
  const SelectStatement& select = Select(tree);
  EXPECT_EQ(SelectQuantifier::kDistinct, select.quantifier);
  ASSERT_EQ(2U, select.result_columns.size());
  ASSERT_TRUE(select.result_columns[0].alias.has_value());
  ASSERT_TRUE(select.result_columns[1].alias.has_value());
  EXPECT_EQ("x", SpanText(tree, RequiredOptional(select.result_columns[0].alias)));
  EXPECT_EQ("y", SpanText(tree, RequiredOptional(select.result_columns[1].alias)));
  ASSERT_TRUE(select.from.has_value());
  const TableSource& source = RequiredOptional(select.from);
  ASSERT_EQ(2U, source.name.parts.size());
  EXPECT_EQ("source", SpanText(tree, RequiredOptional(source.alias)));
  ASSERT_TRUE(select.where.has_value());
  EXPECT_EQ("a > 0", SpanText(tree, tree.expression(RequiredOptional(select.where)).span));
  ASSERT_TRUE(select.limit.has_value());
  const LimitClause& limit = RequiredOptional(select.limit);
  EXPECT_EQ(LimitSyntax::kOffsetKeyword, limit.syntax);
  EXPECT_EQ("10", SpanText(tree, tree.expression(limit.limit).span));
  EXPECT_EQ("2", SpanText(tree, tree.expression(RequiredOptional(limit.offset)).span));

  const ParseOutput comma_output = ParseOrThrow("SELECT ALL 1 LIMIT 2, 3");
  const SyntaxTree& comma_tree = RequiredTree(comma_output);
  const LimitClause& comma_limit = RequiredOptional(Select(comma_tree).limit);
  EXPECT_EQ(SelectQuantifier::kAll, Select(comma_tree).quantifier);
  EXPECT_EQ(LimitSyntax::kComma, comma_limit.syntax);
  EXPECT_EQ("3", SpanText(comma_tree, comma_tree.expression(comma_limit.limit).span));
  EXPECT_EQ("2",
            SpanText(comma_tree, comma_tree.expression(RequiredOptional(comma_limit.offset)).span));
}

TEST(Parser, TreatsWildcardsAsCompleteResultColumns) {
  struct Case {
    std::string_view sql;
    TokenKind actual;
  };
  constexpr std::array cases{
      Case{.sql = "SELECT * + 1", .actual = TokenKind::kPlus},
      Case{.sql = "SELECT * AS x", .actual = TokenKind::kAs},
      Case{.sql = "SELECT t.* x FROM t", .actual = TokenKind::kIdentifier},
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    ParseResult result = ParseOne(Utf8View{test_case.sql});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ParseErrorCode::kUnexpectedToken, result.error().code);
    EXPECT_EQ(test_case.actual, result.error().actual);
    EXPECT_EQ(ParseExpectation::kEndOfStatement, result.error().expected);
  }
}

TEST(Parser, AppliesExactIdentifierClassesAndContextualWindowKeywords) {
  const ParseOutput output =
      ParseOrThrow("SELECT abort, indexed, left, window, over, filter, CURRENT_DATE");
  const SyntaxTree& tree = RequiredTree(output);
  ASSERT_EQ(7U, Select(tree).result_columns.size());
  for (std::size_t index = 0; index < 6U; ++index) {
    EXPECT_TRUE(
        std::holds_alternative<IdentifierExpression>(ResultExpression(tree, index).payload));
  }
  EXPECT_TRUE(std::holds_alternative<LiteralExpression>(ResultExpression(tree, 6U).payload));

  ParseResult indexed_alias = ParseOne(Utf8View{"SELECT 1 INDEXED"});
  ASSERT_FALSE(indexed_alias.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, indexed_alias.error().code);
  EXPECT_EQ(TokenKind::kIndexed, indexed_alias.error().actual);
  EXPECT_EQ(ParseExpectation::kEndOfStatement, indexed_alias.error().expected);
  EXPECT_EQ(16U, indexed_alias.error().next_offset.value());

  ParseResult indexed_type = ParseOne(Utf8View{"CREATE TABLE t(a INDEXED)"});
  ASSERT_FALSE(indexed_type.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, indexed_type.error().code);
  EXPECT_EQ(TokenKind::kIndexed, indexed_type.error().actual);
  EXPECT_EQ(ParseExpectation::kCommaOrRightParenthesis, indexed_type.error().expected);

  struct ContextualCase {
    std::string_view sql;
    TokenKind token;
  };
  constexpr std::array contextual_cases{
      ContextualCase{.sql = "SELECT sum(x) OVER ()", .token = TokenKind::kOver},
      ContextualCase{.sql = "SELECT 1 WINDOW w AS ()", .token = TokenKind::kWindow},
      ContextualCase{
          .sql = "SELECT f(x) FILTER (WHERE x)",
          .token = TokenKind::kFilter,
      },
  };
  for (const ContextualCase& test_case : contextual_cases) {
    SCOPED_TRACE(test_case.sql);
    ParseResult result = ParseOne(Utf8View{test_case.sql});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ParseErrorCode::kUnsupportedSyntax, result.error().code);
    EXPECT_EQ(test_case.token, result.error().actual);
    EXPECT_EQ(ParseExpectation::kNone, result.error().expected);
  }
}

TEST(Parser, DistinguishesExplicitAndBareAliasAndTypeNameClasses) {
  const ParseOutput aliases = ParseOrThrow("SELECT 1 AS left, 2 'two'");
  const SyntaxTree& tree = RequiredTree(aliases);
  ASSERT_EQ(2U, Select(tree).result_columns.size());
  EXPECT_EQ("left", SpanText(tree, RequiredOptional(Select(tree).result_columns[0].alias)));
  EXPECT_EQ("'two'", SpanText(tree, RequiredOptional(Select(tree).result_columns[1].alias)));

  ParseResult bare_join_alias = ParseOne(Utf8View{"SELECT 1 left"});
  ASSERT_FALSE(bare_join_alias.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, bare_join_alias.error().code);
  EXPECT_EQ(TokenKind::kJoinKeyword, bare_join_alias.error().actual);
  EXPECT_EQ(ParseExpectation::kEndOfStatement, bare_join_alias.error().expected);

  ParseResult join_keyword_type = ParseOne(Utf8View{"CREATE TABLE t(a left)"});
  ASSERT_FALSE(join_keyword_type.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, join_keyword_type.error().code);
  EXPECT_EQ(TokenKind::kJoinKeyword, join_keyword_type.error().actual);
  EXPECT_EQ(ParseExpectation::kCommaOrRightParenthesis, join_keyword_type.error().expected);
}

TEST(Parser, GivesSpecialExpressionKeywordsPriorityOverFallbackNames) {
  ParseResult current_time_call = ParseOne(Utf8View{"SELECT current_date()"});
  ASSERT_FALSE(current_time_call.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, current_time_call.error().code);
  EXPECT_EQ(TokenKind::kLeftParenthesis, current_time_call.error().actual);
  EXPECT_EQ(ParseExpectation::kEndOfStatement, current_time_call.error().expected);

  ParseResult qualified_current = ParseOne(Utf8View{"SELECT CURRENT_DATE.x"});
  ASSERT_FALSE(qualified_current.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, qualified_current.error().code);
  EXPECT_EQ(TokenKind::kDot, qualified_current.error().actual);

  ParseResult cast_name = ParseOne(Utf8View{"SELECT cast;"});
  ASSERT_FALSE(cast_name.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, cast_name.error().code);
  EXPECT_EQ(TokenKind::kSemicolon, cast_name.error().actual);
  EXPECT_EQ(ParseExpectation::kExpression, cast_name.error().expected);

  ParseResult raise_name = ParseOne(Utf8View{"SELECT raise;"});
  ASSERT_FALSE(raise_name.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, raise_name.error().code);
  EXPECT_EQ(TokenKind::kSemicolon, raise_name.error().actual);
}

TEST(Parser, MapsDefaultProductionsWithoutPrematureNameResolution) {
  const ParseOutput output = ParseOrThrow(
      "CREATE TABLE t("
      "a DEFAULT true, b DEFAULT false, c DEFAULT word, "
      "d DEFAULT -1, e DEFAULT (1 + 2))");
  const SyntaxTree& tree = RequiredTree(output);
  const CreateTableStatement& table = CreateTable(tree);
  ASSERT_EQ(5U, table.columns.size());

  const auto default_expression = [&](std::size_t column_index) {
    const auto& constraint =
        std::get<DefaultColumnConstraint>(table.columns[column_index].constraints.at(0).payload);
    return constraint.expression;
  };

  const auto& true_literal =
      std::get<LiteralExpression>(tree.expression(default_expression(0)).payload);
  const auto& false_literal =
      std::get<LiteralExpression>(tree.expression(default_expression(1)).payload);
  const auto& word_literal =
      std::get<LiteralExpression>(tree.expression(default_expression(2)).payload);
  EXPECT_EQ(LiteralKind::kTrue, true_literal.kind);
  EXPECT_EQ(LiteralKind::kFalse, false_literal.kind);
  EXPECT_EQ(LiteralKind::kString, word_literal.kind);
  EXPECT_EQ("word", SpanText(tree, word_literal.token));

  const auto& negative = std::get<UnaryExpression>(tree.expression(default_expression(3)).payload);
  EXPECT_EQ(UnaryOperator::kNegative, negative.op);
  const auto& parenthesized =
      std::get<ParenthesizedExpression>(tree.expression(default_expression(4)).payload);
  EXPECT_TRUE(
      std::holds_alternative<BinaryExpression>(tree.expression(parenthesized.inner).payload));
}

TEST(Parser, ParsesCreateTableConstraintsTypesAndOptions) {
  const std::string sql =
      "CREATE TEMP TABLE IF NOT EXISTS main.t ("
      "id INTEGER CONSTRAINT pk PRIMARY KEY DESC ON CONFLICT REPLACE AUTOINCREMENT, "
      "name VARCHAR(10, 2) NULL ON CONFLICT ABORT "
      "NOT NULL ON CONFLICT FAIL UNIQUE ON CONFLICT IGNORE "
      "COLLATE nocase CHECK (name != ''), "
      "CONSTRAINT table_pk PRIMARY KEY (id AUTOINCREMENT) ON CONFLICT ROLLBACK, "
      "CONSTRAINT uq UNIQUE (name COLLATE binary DESC) ON CONFLICT FAIL, "
      "CHECK (id > 0) ON CONFLICT IGNORE"
      ") WITHOUT ROWID, STRICT;";
  const ParseOutput output = ParseOrThrow(sql);
  const SyntaxTree& tree = RequiredTree(output);
  const CreateTableStatement& table = CreateTable(tree);
  EXPECT_TRUE(table.temporary);
  EXPECT_TRUE(table.if_not_exists);
  EXPECT_TRUE(table.without_rowid);
  EXPECT_TRUE(table.strict);
  ASSERT_EQ(2U, table.name.parts.size());
  ASSERT_EQ(2U, table.columns.size());
  EXPECT_EQ("INTEGER", SpanText(tree, RequiredOptional(table.columns[0].type_name)));
  EXPECT_EQ("VARCHAR(10, 2)", SpanText(tree, RequiredOptional(table.columns[1].type_name)));

  ASSERT_EQ(1U, table.columns[0].constraints.size());
  const ColumnConstraint& primary_constraint = table.columns[0].constraints[0];
  EXPECT_EQ("pk", SpanText(tree, RequiredOptional(primary_constraint.name)));
  const auto& primary = std::get<PrimaryKeyColumnConstraint>(primary_constraint.payload);
  EXPECT_EQ(SortOrder::kDescending, primary.order);
  EXPECT_EQ(ConflictAction::kReplace, primary.conflict);
  EXPECT_TRUE(primary.autoincrement);

  ASSERT_EQ(5U, table.columns[1].constraints.size());
  EXPECT_EQ(ConflictAction::kAbort,
            std::get<NullColumnConstraint>(table.columns[1].constraints[0].payload).conflict);
  EXPECT_EQ(ConflictAction::kFail,
            std::get<NotNullColumnConstraint>(table.columns[1].constraints[1].payload).conflict);
  EXPECT_EQ(ConflictAction::kIgnore,
            std::get<UniqueColumnConstraint>(table.columns[1].constraints[2].payload).conflict);
  EXPECT_EQ(
      "nocase",
      SpanText(
          tree,
          std::get<CollateColumnConstraint>(table.columns[1].constraints[3].payload).collation));
  EXPECT_TRUE(
      std::holds_alternative<CheckColumnConstraint>(table.columns[1].constraints[4].payload));

  ASSERT_EQ(3U, table.constraints.size());
  const auto& table_primary = std::get<PrimaryKeyTableConstraint>(table.constraints[0].payload);
  EXPECT_TRUE(table_primary.autoincrement);
  EXPECT_EQ(ConflictAction::kRollback, table_primary.conflict);
  const auto& unique = std::get<UniqueTableConstraint>(table.constraints[1].payload);
  ASSERT_EQ(1U, unique.terms.size());
  EXPECT_EQ("binary", SpanText(tree, RequiredOptional(unique.terms[0].collation)));
  EXPECT_EQ(SortOrder::kDescending, unique.terms[0].order);
  EXPECT_EQ(ConflictAction::kFail, unique.conflict);
  EXPECT_TRUE(std::holds_alternative<CheckTableConstraint>(table.constraints[2].payload));
}

TEST(Parser, ParsesCreateIndexTermsAndPartialPredicate) {
  const ParseOutput output = ParseOrThrow(
      "CREATE UNIQUE INDEX IF NOT EXISTS main.idx ON items "
      "(lower(name) COLLATE nocase DESC, id + 1 ASC) WHERE active = true;");
  const SyntaxTree& tree = RequiredTree(output);
  const CreateIndexStatement& index = CreateIndex(tree);
  EXPECT_TRUE(index.unique);
  EXPECT_TRUE(index.if_not_exists);
  ASSERT_EQ(2U, index.name.parts.size());
  ASSERT_EQ(1U, index.table.parts.size());
  ASSERT_EQ(2U, index.terms.size());
  EXPECT_EQ("lower(name)", SpanText(tree, tree.expression(index.terms[0].expression).span));
  EXPECT_EQ("nocase", SpanText(tree, RequiredOptional(index.terms[0].collation)));
  EXPECT_EQ(SortOrder::kDescending, index.terms[0].order);
  EXPECT_EQ("id + 1", SpanText(tree, tree.expression(index.terms[1].expression).span));
  EXPECT_EQ(SortOrder::kAscending, index.terms[1].order);
  ASSERT_TRUE(index.where.has_value());
  const ExpressionId where = RequiredOptional(index.where);
  EXPECT_EQ("active = true", SpanText(tree, tree.expression(where).span));
  const auto& predicate = std::get<BinaryExpression>(tree.expression(where).payload);
  EXPECT_TRUE(
      std::holds_alternative<IdentifierExpression>(tree.expression(predicate.right).payload));
}

TEST(Parser, ExtractsOnlyTheFinalIndexedTermCollation) {
  const ParseOutput output = ParseOrThrow(
      "CREATE INDEX idx ON t("
      "a COLLATE nocase + b, "
      "a COLLATE nocase COLLATE binary)");
  const SyntaxTree& tree = RequiredTree(output);
  const CreateIndexStatement& index = CreateIndex(tree);
  ASSERT_EQ(2U, index.terms.size());

  EXPECT_FALSE(index.terms[0].collation.has_value());
  const auto& addition =
      std::get<BinaryExpression>(tree.expression(index.terms[0].expression).payload);
  EXPECT_TRUE(std::holds_alternative<CollateExpression>(tree.expression(addition.left).payload));

  EXPECT_EQ("binary", SpanText(tree, RequiredOptional(index.terms[1].collation)));
  const auto& inner_collation =
      std::get<CollateExpression>(tree.expression(index.terms[1].expression).payload);
  EXPECT_EQ("nocase", SpanText(tree, inner_collation.collation));
}

TEST(Parser, ReportsDeterministicLexicalAndSyntaxErrorsWithTails) {
  const std::string illegal_sql = "SELECT @ trailing";
  ParseResult illegal = ParseOne(Utf8View{illegal_sql});
  ASSERT_FALSE(illegal.has_value());
  EXPECT_EQ(ParseErrorCode::kIllegalToken, illegal.error().code);
  EXPECT_EQ(TokenKind::kIllegal, illegal.error().actual);
  EXPECT_EQ(ParseExpectation::kNone, illegal.error().expected);
  EXPECT_EQ("@", SpanText(Utf8View{illegal_sql}, illegal.error().span));
  EXPECT_EQ(7U, illegal.error().next_offset.value());

  const std::string unexpected_sql = "SELECT 1 SELECT 2";
  ParseResult unexpected = ParseOne(Utf8View{unexpected_sql});
  ASSERT_FALSE(unexpected.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, unexpected.error().code);
  EXPECT_EQ(TokenKind::kSelect, unexpected.error().actual);
  EXPECT_EQ(ParseExpectation::kEndOfStatement, unexpected.error().expected);
  EXPECT_EQ("SELECT", SpanText(Utf8View{unexpected_sql}, unexpected.error().span));
  EXPECT_EQ(15U, unexpected.error().next_offset.value());

  const std::string incomplete_sql = "SELECT (1";
  ParseResult incomplete = ParseOne(Utf8View{incomplete_sql});
  ASSERT_FALSE(incomplete.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, incomplete.error().code);
  EXPECT_EQ(TokenKind::kEndOfInput, incomplete.error().actual);
  EXPECT_EQ(ParseExpectation::kRightParenthesis, incomplete.error().expected);
  EXPECT_TRUE(incomplete.error().span.empty());
  EXPECT_EQ(incomplete_sql.size(), incomplete.error().next_offset.value());
  EXPECT_EQ("incomplete input", ParseErrorMessage(incomplete.error()));

  const std::string prefixed_sql = " ; /* empty */ ; SELECT 1 SELECT 2";
  ParseResult prefixed = ParseOne(Utf8View{prefixed_sql});
  ASSERT_FALSE(prefixed.has_value());
  const std::size_t second_select = prefixed_sql.rfind("SELECT");
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, prefixed.error().code);
  EXPECT_EQ(TokenKind::kSelect, prefixed.error().actual);
  EXPECT_EQ(second_select, prefixed.error().span.begin().value());
  EXPECT_EQ(second_select + std::string_view{"SELECT"}.size(), prefixed.error().span.end().value());
  EXPECT_EQ(prefixed.error().span.end(), prefixed.error().next_offset);

  ParseResult incomplete_create = ParseOne(Utf8View{"CREATE;"});
  ASSERT_FALSE(incomplete_create.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, incomplete_create.error().code);
  EXPECT_EQ(TokenKind::kSemicolon, incomplete_create.error().actual);

  ParseResult invalid_ddl_tail = ParseOne(Utf8View{"CREATE TABLE t(a) ORDER BY a"});
  ASSERT_FALSE(invalid_ddl_tail.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, invalid_ddl_tail.error().code);
  EXPECT_EQ(TokenKind::kOrder, invalid_ddl_tail.error().actual);
  EXPECT_EQ(ParseExpectation::kEndOfStatement, invalid_ddl_tail.error().expected);

  ParseResult missing_table_option = ParseOne(Utf8View{"CREATE TABLE t(a) WITHOUT;"});
  ASSERT_FALSE(missing_table_option.has_value());
  EXPECT_EQ(ParseErrorCode::kUnexpectedToken, missing_table_option.error().code);
  EXPECT_EQ(TokenKind::kSemicolon, missing_table_option.error().actual);
  EXPECT_EQ(ParseExpectation::kTableOption, missing_table_option.error().expected);
}

TEST(Parser, RejectsRecognizedButUnmodeledSqliteSyntax) {
  struct Case {
    std::string_view sql;
    TokenKind first_unsupported;
  };
  constexpr std::array cases{
      Case{.sql = "SELECT 1 ORDER BY 1", .first_unsupported = TokenKind::kOrder},
      Case{.sql = "SELECT 1 GROUP BY 1", .first_unsupported = TokenKind::kGroup},
      Case{.sql = "SELECT 1 UNION SELECT 2", .first_unsupported = TokenKind::kUnion},
      Case{.sql = "SELECT * FROM a JOIN b", .first_unsupported = TokenKind::kJoin},
      Case{.sql = "SELECT * FROM f(1)", .first_unsupported = TokenKind::kLeftParenthesis},
      Case{
          .sql = "SELECT * FROM (SELECT 1)",
          .first_unsupported = TokenKind::kLeftParenthesis,
      },
      Case{
          .sql = "SELECT * FROM t INDEXED BY idx",
          .first_unsupported = TokenKind::kIndexed,
      },
      Case{
          .sql = "WITH x AS (SELECT 1) SELECT 1",
          .first_unsupported = TokenKind::kWith,
      },
      Case{.sql = "CREATE TABLE t AS SELECT 1", .first_unsupported = TokenKind::kAs},
      Case{
          .sql = "CREATE TABLE t(a REFERENCES p)",
          .first_unsupported = TokenKind::kReferences,
      },
      Case{.sql = "CREATE TABLE t(a AS (1))", .first_unsupported = TokenKind::kAs},
      Case{
          .sql = "SELECT CASE WHEN 1 THEN 2 END",
          .first_unsupported = TokenKind::kCase,
      },
      Case{
          .sql = "SELECT 1 BETWEEN 0 AND 2",
          .first_unsupported = TokenKind::kBetween,
      },
      Case{.sql = "SELECT 1 IN (1)", .first_unsupported = TokenKind::kIn},
      Case{.sql = "SELECT CAST(1 AS TEXT)", .first_unsupported = TokenKind::kCast},
      Case{
          .sql = "SELECT f(ORDER BY 1)",
          .first_unsupported = TokenKind::kOrder,
      },
      Case{
          .sql = "SELECT f(ALL ORDER BY 1)",
          .first_unsupported = TokenKind::kOrder,
      },
      Case{
          .sql = "SELECT f(DISTINCT ORDER BY 1)",
          .first_unsupported = TokenKind::kOrder,
      },
      Case{
          .sql = "CREATE TABLE t(a DEFERRABLE)",
          .first_unsupported = TokenKind::kDeferrable,
      },
      Case{
          .sql = "CREATE TABLE t(a NOT DEFERRABLE)",
          .first_unsupported = TokenKind::kNot,
      },
      Case{.sql = "CREATE VIEW v AS SELECT 1", .first_unsupported = TokenKind::kView},
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.sql);
    ParseResult result = ParseOne(Utf8View{test_case.sql});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ParseErrorCode::kUnsupportedSyntax, result.error().code);
    EXPECT_EQ(test_case.first_unsupported, result.error().actual);
    EXPECT_EQ(ParseExpectation::kNone, result.error().expected);
    EXPECT_EQ(result.error().span.end(), result.error().next_offset);
  }
}

TEST(Parser, EnforcesConstructionDepthWithoutRecursiveDestruction) {
  const auto make_addition = [](std::size_t terms) {
    std::string sql = "SELECT 1";
    for (std::size_t index = 1; index < terms; ++index) {
      sql.append("+1");
    }
    return sql;
  };

  const std::string accepted_sql = make_addition(kMaximumExpressionConstructionDepth);
  const ParseOutput accepted = ParseOrThrow(accepted_sql);
  EXPECT_EQ((2U * kMaximumExpressionConstructionDepth) - 1U,
            RequiredTree(accepted).expressions().size());

  const std::string rejected_sql = make_addition(kMaximumExpressionConstructionDepth + 1U);
  ParseResult rejected = ParseOne(Utf8View{rejected_sql});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ParseErrorCode::kExpressionDepthExceeded, rejected.error().code);
  EXPECT_EQ(TokenKind::kPlus, rejected.error().actual);
  EXPECT_EQ(ParseExpectation::kNone, rejected.error().expected);
}

TEST(Parser, EnforcesConservativeRecursiveParserDepth) {
  const auto make_parentheses = [](std::size_t nesting) {
    std::string sql = "SELECT ";
    sql.append(nesting, '(');
    sql.push_back('1');
    sql.append(nesting, ')');
    return sql;
  };

  const ParseOutput accepted = ParseOrThrow(make_parentheses(kMaximumParserRecursionDepth - 1U));
  EXPECT_TRUE(accepted.tree.has_value());

  const std::string rejected_sql = make_parentheses(kMaximumParserRecursionDepth);
  ParseResult rejected = ParseOne(Utf8View{rejected_sql});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ParseErrorCode::kParserDepthExceeded, rejected.error().code);
  EXPECT_EQ(TokenKind::kLeftParenthesis, rejected.error().actual);
  EXPECT_EQ(ParseExpectation::kExpression, rejected.error().expected);
}

}  // namespace
}  // namespace modern_sqlite
