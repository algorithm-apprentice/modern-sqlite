#include "modern_sqlite/syntax/ast.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<SyntaxTree>);
static_assert(!std::is_copy_assignable_v<SyntaxTree>);
static_assert(std::is_nothrow_move_constructible_v<SyntaxTree>);
static_assert(std::is_nothrow_move_assignable_v<SyntaxTree>);

[[nodiscard]] SourceSpan Span(std::size_t begin, std::size_t end) {
  const auto span = SourceSpan::FromBounds(ByteOffset{begin}, ByteOffset{end});
  if (!span.has_value()) {
    throw std::logic_error{"invalid test source span"};
  }
  return *span;
}

[[nodiscard]] SourceSpan FindSpan(std::string_view source, std::string_view text,
                                  std::size_t offset = 0) {
  const std::size_t begin = source.find(text, offset);
  if (begin == std::string_view::npos) {
    throw std::logic_error{"test text not found"};
  }
  return Span(begin, begin + text.size());
}

[[nodiscard]] QualifiedName Name(SourceSpan span, std::initializer_list<SourceSpan> parts) {
  return QualifiedName{
      .span = span,
      .parts = std::vector<SourceSpan>{parts},
  };
}

template <typename Enum>
[[nodiscard]] constexpr Enum InvalidEnumValue(std::uint8_t value) noexcept {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<Enum>(value);
}

[[nodiscard]] SelectStatement SingleResultSelect(SourceSpan statement_span, SourceSpan result_span,
                                                 ExpressionId expression) {
  return SelectStatement{
      .span = statement_span,
      .quantifier = SelectQuantifier::kDefault,
      .result_columns =
          {
              ResultColumn{
                  .span = result_span,
                  .expression = expression,
              },
          },
  };
}

void ExpectMisuse(const Result<SyntaxTree>& result) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, result.error().code());
  EXPECT_FALSE(result.error().message().empty());
}

TEST(SyntaxTree, OwnsExactRawSourceAndPreservesTokenSpellings) {
  std::string expected{"SELECT "};
  const std::size_t identifier_begin = expected.size();
  expected.push_back('"');
  expected.push_back(static_cast<char>(0xFF));
  expected.push_back('"');
  const std::size_t identifier_end = expected.size();
  expected.append(", 'a''b', 1_000, x'00fF';");

  const SourceSpan statement_span = Span(0, expected.size() - 1U);
  const SourceSpan identifier_span = Span(identifier_begin, identifier_end);
  const SourceSpan string_span = FindSpan(expected, "'a''b'");
  const SourceSpan integer_span = FindSpan(expected, "1_000");
  const SourceSpan blob_span = FindSpan(expected, "x'00fF'");

  auto result = [&]() {
    std::string source = expected;
    std::vector<Expression> expressions{
        Expression{
            .span = identifier_span,
            .payload =
                IdentifierExpression{
                    .name = Name(identifier_span, {identifier_span}),
                },
        },
        Expression{
            .span = string_span,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kString,
                    .token = string_span,
                },
        },
        Expression{
            .span = integer_span,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = integer_span,
                },
        },
        Expression{
            .span = blob_span,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kBlob,
                    .token = blob_span,
                },
        },
    };
    Statement statement = SelectStatement{
        .span = statement_span,
        .quantifier = SelectQuantifier::kDefault,
        .result_columns =
            {
                ResultColumn{.span = identifier_span, .expression = ExpressionId{0}},
                ResultColumn{.span = string_span, .expression = ExpressionId{1}},
                ResultColumn{.span = integer_span, .expression = ExpressionId{2}},
                ResultColumn{.span = blob_span, .expression = ExpressionId{3}},
            },
    };
    return SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement));
  }();

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  EXPECT_EQ(expected, result->source().bytes());
  ASSERT_EQ(4U, result->expressions().size());

  const auto raw_identifier = Slice(result->source(), identifier_span);
  const auto raw_string = Slice(result->source(), string_span);
  const auto raw_integer = Slice(result->source(), integer_span);
  const auto raw_blob = Slice(result->source(), blob_span);
  ASSERT_TRUE(raw_identifier.has_value());
  ASSERT_TRUE(raw_string.has_value());
  ASSERT_TRUE(raw_integer.has_value());
  ASSERT_TRUE(raw_blob.has_value());
  EXPECT_EQ(std::string_view{expected}.substr(identifier_begin, identifier_span.length().value()),
            raw_identifier->bytes());
  EXPECT_EQ("'a''b'", raw_string->bytes());
  EXPECT_EQ("1_000", raw_integer->bytes());
  EXPECT_EQ("x'00fF'", raw_blob->bytes());
}

TEST(SyntaxTree, UsesStatementLocalSpansForLaterStatements) {
  std::string input{" -- first\nSELECT 0; /* gap */ SELECT '"};
  input.push_back(static_cast<char>(0xFE));
  input.append("'; SELECT 2");

  const std::size_t statement_begin = input.find("SELECT '");
  ASSERT_NE(std::string::npos, statement_begin);
  const std::size_t statement_end = input.find(';', statement_begin) + 1U;
  ASSERT_NE(0U, statement_end);
  const std::size_t absolute_literal_begin = input.find('\'', statement_begin);
  const std::size_t absolute_literal_end = input.find('\'', absolute_literal_begin + 1U) + 1U;

  std::string source = input.substr(statement_begin, statement_end - statement_begin);
  const SourceSpan literal_span =
      Span(absolute_literal_begin - statement_begin, absolute_literal_end - statement_begin);
  const SourceSpan statement_span = Span(0, source.size() - 1U);
  std::vector<Expression> expressions{
      Expression{
          .span = literal_span,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kString,
                  .token = literal_span,
              },
      },
  };

  auto result =
      SyntaxTree::Create(std::move(source), std::move(expressions),
                         SingleResultSelect(statement_span, literal_span, ExpressionId{0}));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  EXPECT_EQ(input.substr(statement_begin, statement_end - statement_begin),
            result->source().bytes());
  const auto literal = Slice(result->source(), literal_span);
  ASSERT_TRUE(literal.has_value());
  EXPECT_EQ(3U, literal->size_bytes());
  EXPECT_EQ(static_cast<unsigned char>(0xFE), static_cast<unsigned char>(literal->bytes()[1]));
}

TEST(SyntaxTree, RepresentsEveryLiteralKindWithoutDecoding) {
  struct Case {
    LiteralKind kind;
    std::string_view token;
  };
  constexpr std::array cases{
      Case{.kind = LiteralKind::kNull, .token = "NULL"},
      Case{.kind = LiteralKind::kInteger, .token = "0x7f"},
      Case{.kind = LiteralKind::kReal, .token = "1_2.5e+2"},
      Case{.kind = LiteralKind::kString, .token = "'text'"},
      Case{.kind = LiteralKind::kBlob, .token = "x'CAFE'"},
      Case{.kind = LiteralKind::kCurrentDate, .token = "CURRENT_DATE"},
      Case{.kind = LiteralKind::kCurrentTime, .token = "CURRENT_TIME"},
      Case{.kind = LiteralKind::kCurrentTimestamp, .token = "CURRENT_TIMESTAMP"},
      Case{.kind = LiteralKind::kTrue, .token = "TRUE"},
      Case{.kind = LiteralKind::kFalse, .token = "FALSE"},
  };

  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.token);
    std::string source = "SELECT ";
    source.append(test_case.token);
    const SourceSpan token = Span(7, source.size());
    std::vector<Expression> expressions{
        Expression{
            .span = token,
            .payload =
                LiteralExpression{
                    .kind = test_case.kind,
                    .token = token,
                },
        },
    };
    auto result = SyntaxTree::Create(
        std::move(source), std::move(expressions),
        SingleResultSelect(Span(0, token.end().value()), token, ExpressionId{0}));

    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    const auto& literal = std::get<LiteralExpression>(result->expression(ExpressionId{0}).payload);
    EXPECT_EQ(test_case.kind, literal.kind);
    const auto text = Slice(result->source(), literal.token);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(test_case.token, text->bytes());
  }
}

TEST(SyntaxTree, RepresentsTypedExpressionPayloadsInPostorder) {
  const std::string source =
      "SELECT -1 + ?1, (fn(DISTINCT col) COLLATE nocase) AS result, table.* "
      "FROM main.items AS i;";
  const SourceSpan statement_span = Span(0, source.size() - 1U);
  const SourceSpan literal = FindSpan(source, "1");
  const SourceSpan variable = FindSpan(source, "?1");
  const SourceSpan identifier = FindSpan(source, "col");
  const SourceSpan wildcard = FindSpan(source, "table.*");
  const SourceSpan qualifier = FindSpan(source, "table");
  const SourceSpan asterisk = Span(wildcard.end().value() - 1U, wildcard.end().value());
  const SourceSpan unary = FindSpan(source, "-1");
  const SourceSpan binary = FindSpan(source, "-1 + ?1");
  const SourceSpan function = FindSpan(source, "fn(DISTINCT col)");
  const SourceSpan function_name = FindSpan(source, "fn");
  const SourceSpan collate = FindSpan(source, "fn(DISTINCT col) COLLATE nocase");
  const SourceSpan collate_keyword = FindSpan(source, "COLLATE");
  const SourceSpan collation = FindSpan(source, "nocase");
  const SourceSpan parenthesized = FindSpan(source, "(fn(DISTINCT col) COLLATE nocase)");
  const SourceSpan second_result = FindSpan(source, "(fn(DISTINCT col) COLLATE nocase) AS result");
  const SourceSpan alias = FindSpan(source, "result");
  const SourceSpan source_clause = FindSpan(source, "main.items AS i");
  const SourceSpan source_name = FindSpan(source, "main.items");
  const SourceSpan main_part = FindSpan(source, "main");
  const SourceSpan items_part = FindSpan(source, "items");
  const SourceSpan source_alias_suffix = FindSpan(source, " AS i", source_name.end().value());
  const SourceSpan source_alias =
      Span(source_alias_suffix.end().value() - 1U, source_alias_suffix.end().value());

  std::vector<Expression> expressions{
      Expression{
          .span = literal,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = literal,
              },
      },
      Expression{
          .span = variable,
          .payload = VariableExpression{.token = variable},
      },
      Expression{
          .span = identifier,
          .payload =
              IdentifierExpression{
                  .name = Name(identifier, {identifier}),
              },
      },
      Expression{
          .span = wildcard,
          .payload =
              WildcardExpression{
                  .asterisk = asterisk,
                  .qualifier = Name(qualifier, {qualifier}),
              },
      },
      Expression{
          .span = unary,
          .payload =
              UnaryExpression{
                  .op = UnaryOperator::kNegative,
                  .operator_span = Span(unary.begin().value(), unary.begin().value() + 1U),
                  .operand = ExpressionId{0},
              },
      },
      Expression{
          .span = binary,
          .payload =
              BinaryExpression{
                  .op = BinaryOperator::kAdd,
                  .operator_span = FindSpan(source, "+"),
                  .left = ExpressionId{4},
                  .right = ExpressionId{1},
              },
      },
      Expression{
          .span = function,
          .payload =
              FunctionCallExpression{
                  .name = Name(function_name, {function_name}),
                  .arguments = {ExpressionId{2}},
                  .distinct = true,
              },
      },
      Expression{
          .span = collate,
          .payload =
              CollateExpression{
                  .operand = ExpressionId{6},
                  .keyword = collate_keyword,
                  .collation = collation,
              },
      },
      Expression{
          .span = parenthesized,
          .payload = ParenthesizedExpression{.inner = ExpressionId{7}},
      },
  };
  Statement statement = SelectStatement{
      .span = statement_span,
      .quantifier = SelectQuantifier::kDefault,
      .result_columns =
          {
              ResultColumn{.span = binary, .expression = ExpressionId{5}},
              ResultColumn{
                  .span = second_result,
                  .expression = ExpressionId{8},
                  .alias = alias,
              },
              ResultColumn{.span = wildcard, .expression = ExpressionId{3}},
          },
      .from =
          TableSource{
              .span = source_clause,
              .name = Name(source_name, {main_part, items_part}),
              .alias = source_alias,
          },
  };

  auto result = SyntaxTree::Create(source, std::move(expressions), std::move(statement));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  EXPECT_TRUE(
      std::holds_alternative<LiteralExpression>(result->expression(ExpressionId{0}).payload));
  EXPECT_TRUE(
      std::holds_alternative<VariableExpression>(result->expression(ExpressionId{1}).payload));
  EXPECT_TRUE(
      std::holds_alternative<IdentifierExpression>(result->expression(ExpressionId{2}).payload));
  EXPECT_TRUE(
      std::holds_alternative<WildcardExpression>(result->expression(ExpressionId{3}).payload));
  EXPECT_TRUE(std::holds_alternative<UnaryExpression>(result->expression(ExpressionId{4}).payload));
  EXPECT_TRUE(
      std::holds_alternative<BinaryExpression>(result->expression(ExpressionId{5}).payload));
  const auto& call = std::get<FunctionCallExpression>(result->expression(ExpressionId{6}).payload);
  EXPECT_TRUE(call.distinct);
  ASSERT_EQ(1U, call.arguments.size());
  EXPECT_EQ(ExpressionId{2}, call.arguments.front());
  EXPECT_EQ(ExpressionId{6},
            std::get<CollateExpression>(result->expression(ExpressionId{7}).payload).operand);
  EXPECT_EQ(ExpressionId{7},
            std::get<ParenthesizedExpression>(result->expression(ExpressionId{8}).payload).inner);
}

TEST(SyntaxTree, RepresentsSelectClausesAndNormalizesLimitOperands) {
  const std::string source =
      "SELECT DISTINCT a AS x FROM main.t AS source WHERE a > 0 LIMIT 10 OFFSET 2;";
  const SourceSpan statement_span = Span(0, source.size() - 1U);
  const SourceSpan result_identifier = FindSpan(source, "a");
  const SourceSpan result_column = FindSpan(source, "a AS x");
  const SourceSpan result_alias = FindSpan(source, "x");
  const SourceSpan table_source = FindSpan(source, "main.t AS source");
  const SourceSpan table_name = FindSpan(source, "main.t");
  const SourceSpan main_part = FindSpan(source, "main");
  const SourceSpan table_part = FindSpan(source, "t", table_name.begin().value());
  const SourceSpan table_alias = FindSpan(source, "source");
  const SourceSpan where_expression = FindSpan(source, "a > 0");
  const SourceSpan where_identifier = FindSpan(source, "a", where_expression.begin().value());
  const SourceSpan where_literal = FindSpan(source, "0", where_expression.begin().value());
  const SourceSpan limit_value = FindSpan(source, "10");
  const SourceSpan offset_value = FindSpan(source, "2");
  const SourceSpan limit_clause = FindSpan(source, "LIMIT 10 OFFSET 2");

  std::vector<Expression> expressions{
      Expression{
          .span = result_identifier,
          .payload =
              IdentifierExpression{
                  .name = Name(result_identifier, {result_identifier}),
              },
      },
      Expression{
          .span = where_identifier,
          .payload =
              IdentifierExpression{
                  .name = Name(where_identifier, {where_identifier}),
              },
      },
      Expression{
          .span = where_literal,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = where_literal,
              },
      },
      Expression{
          .span = where_expression,
          .payload =
              BinaryExpression{
                  .op = BinaryOperator::kGreater,
                  .operator_span = FindSpan(source, ">"),
                  .left = ExpressionId{1},
                  .right = ExpressionId{2},
              },
      },
      Expression{
          .span = limit_value,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = limit_value,
              },
      },
      Expression{
          .span = offset_value,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = offset_value,
              },
      },
  };
  Statement statement = SelectStatement{
      .span = statement_span,
      .quantifier = SelectQuantifier::kDistinct,
      .result_columns =
          {
              ResultColumn{
                  .span = result_column,
                  .expression = ExpressionId{0},
                  .alias = result_alias,
              },
          },
      .from =
          TableSource{
              .span = table_source,
              .name = Name(table_name, {main_part, table_part}),
              .alias = table_alias,
          },
      .where = ExpressionId{3},
      .limit =
          LimitClause{
              .span = limit_clause,
              .limit = ExpressionId{4},
              .offset = ExpressionId{5},
              .syntax = LimitSyntax::kOffsetKeyword,
          },
  };

  auto result = SyntaxTree::Create(source, std::move(expressions), std::move(statement));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  const auto& select = std::get<SelectStatement>(result->statement());
  EXPECT_EQ(SelectQuantifier::kDistinct, select.quantifier);
  ASSERT_TRUE(select.from.has_value());
  EXPECT_EQ(table_alias, select.from->alias);
  EXPECT_EQ(ExpressionId{3}, select.where);
  ASSERT_TRUE(select.limit.has_value());
  EXPECT_EQ(ExpressionId{4}, select.limit->limit);
  EXPECT_EQ(ExpressionId{5}, select.limit->offset);
  EXPECT_EQ(LimitSyntax::kOffsetKeyword, select.limit->syntax);
}

TEST(SyntaxTree, PreservesCommaLimitSyntaxWithNormalizedOperands) {
  const std::string source = "SELECT 1 LIMIT 2, 3";
  const SourceSpan result_value = FindSpan(source, "1");
  const SourceSpan source_offset = FindSpan(source, "2");
  const SourceSpan normalized_limit = FindSpan(source, "3");
  std::vector<Expression> expressions{
      Expression{
          .span = result_value,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = result_value,
              },
      },
      Expression{
          .span = source_offset,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = source_offset,
              },
      },
      Expression{
          .span = normalized_limit,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = normalized_limit,
              },
      },
  };
  Statement statement = SelectStatement{
      .span = Span(0, source.size()),
      .result_columns =
          {
              ResultColumn{.span = result_value, .expression = ExpressionId{0}},
          },
      .limit =
          LimitClause{
              .span = FindSpan(source, "LIMIT 2, 3"),
              .limit = ExpressionId{2},
              .offset = ExpressionId{1},
              .syntax = LimitSyntax::kComma,
          },
  };

  auto result = SyntaxTree::Create(source, std::move(expressions), std::move(statement));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  const auto& select = std::get<SelectStatement>(result->statement());
  ASSERT_TRUE(select.limit.has_value());
  const auto& limit = *select.limit;
  EXPECT_EQ(ExpressionId{2}, limit.limit);
  EXPECT_EQ(ExpressionId{1}, limit.offset);
  EXPECT_EQ(LimitSyntax::kComma, limit.syntax);
}

TEST(SyntaxTree, RepresentsCreateTableColumnConstraints) {
  const std::string source =
      "CREATE TABLE IF NOT EXISTS main.items ("
      "id INTEGER CONSTRAINT pk PRIMARY KEY ASC ON CONFLICT REPLACE AUTOINCREMENT, "
      "name TEXT NOT NULL ON CONFLICT FAIL UNIQUE ON CONFLICT IGNORE COLLATE nocase "
      "DEFAULT 'x', "
      "nickname TEXT NULL ON CONFLICT ABORT, "
      "score REAL CHECK (score > 0), "
      "CONSTRAINT uq UNIQUE (name COLLATE nocase DESC) ON CONFLICT ABORT, "
      "CHECK (id > 0)"
      ") STRICT;";
  const SourceSpan statement_span = Span(0, source.size() - 1U);
  const SourceSpan table_name = FindSpan(source, "main.items");
  const SourceSpan main_part = FindSpan(source, "main");
  const SourceSpan items_part = FindSpan(source, "items");
  const SourceSpan id_name = FindSpan(source, "id");
  const SourceSpan id_type = FindSpan(source, "INTEGER");
  const SourceSpan primary_key =
      FindSpan(source, "CONSTRAINT pk PRIMARY KEY ASC ON CONFLICT REPLACE AUTOINCREMENT");
  const SourceSpan pk_name = FindSpan(source, "pk");
  const SourceSpan name_column =
      FindSpan(source,
               "name TEXT NOT NULL ON CONFLICT FAIL UNIQUE ON CONFLICT IGNORE COLLATE nocase "
               "DEFAULT 'x'");
  const SourceSpan name_name = FindSpan(source, "name");
  const SourceSpan name_type = FindSpan(source, "TEXT");
  const SourceSpan not_null = FindSpan(source, "NOT NULL ON CONFLICT FAIL");
  const SourceSpan unique_column = FindSpan(source, "UNIQUE ON CONFLICT IGNORE");
  const SourceSpan collate_column = FindSpan(source, "COLLATE nocase");
  const SourceSpan first_nocase = FindSpan(source, "nocase");
  const SourceSpan default_column = FindSpan(source, "DEFAULT 'x'");
  const SourceSpan default_literal = FindSpan(source, "'x'");
  const SourceSpan nickname_column = FindSpan(source, "nickname TEXT NULL ON CONFLICT ABORT");
  const SourceSpan nickname_name = FindSpan(source, "nickname");
  const SourceSpan nickname_type = FindSpan(source, "TEXT", name_type.end().value());
  const SourceSpan null_constraint = FindSpan(source, "NULL ON CONFLICT ABORT");
  const SourceSpan score_column = FindSpan(source, "score REAL CHECK (score > 0)");
  const SourceSpan score_name = FindSpan(source, "score");
  const SourceSpan score_type = FindSpan(source, "REAL");
  const SourceSpan score_check = FindSpan(source, "CHECK (score > 0)");
  const SourceSpan score_check_identifier = FindSpan(source, "score", score_name.end().value());
  const SourceSpan score_zero = FindSpan(source, "0", score_check_identifier.end().value());
  const SourceSpan score_expression = FindSpan(source, "score > 0");
  const SourceSpan table_unique =
      FindSpan(source, "CONSTRAINT uq UNIQUE (name COLLATE nocase DESC) ON CONFLICT ABORT");
  const SourceSpan uq_name = FindSpan(source, "uq");
  const SourceSpan unique_name_expression = FindSpan(source, "name", table_unique.begin().value());
  const SourceSpan second_nocase = FindSpan(source, "nocase", first_nocase.end().value());
  const SourceSpan unique_term = FindSpan(source, "name COLLATE nocase DESC");
  const SourceSpan table_check = FindSpan(source, "CHECK (id > 0)");
  const SourceSpan check_id = FindSpan(source, "id", table_check.begin().value());
  const SourceSpan check_zero = FindSpan(source, "0", check_id.end().value());
  const SourceSpan check_expression = FindSpan(source, "id > 0");

  std::vector<Expression> expressions{
      Expression{
          .span = default_literal,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kString,
                  .token = default_literal,
              },
      },
      Expression{
          .span = score_check_identifier,
          .payload =
              IdentifierExpression{
                  .name = Name(score_check_identifier, {score_check_identifier}),
              },
      },
      Expression{
          .span = score_zero,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = score_zero,
              },
      },
      Expression{
          .span = score_expression,
          .payload =
              BinaryExpression{
                  .op = BinaryOperator::kGreater,
                  .operator_span = FindSpan(source, ">", score_check_identifier.end().value()),
                  .left = ExpressionId{1},
                  .right = ExpressionId{2},
              },
      },
      Expression{
          .span = unique_name_expression,
          .payload =
              IdentifierExpression{
                  .name = Name(unique_name_expression, {unique_name_expression}),
              },
      },
      Expression{
          .span = check_id,
          .payload =
              IdentifierExpression{
                  .name = Name(check_id, {check_id}),
              },
      },
      Expression{
          .span = check_zero,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = check_zero,
              },
      },
      Expression{
          .span = check_expression,
          .payload =
              BinaryExpression{
                  .op = BinaryOperator::kGreater,
                  .operator_span = FindSpan(source, ">", check_id.end().value()),
                  .left = ExpressionId{5},
                  .right = ExpressionId{6},
              },
      },
  };
  Statement statement = CreateTableStatement{
      .span = statement_span,
      .temporary = false,
      .if_not_exists = true,
      .name = Name(table_name, {main_part, items_part}),
      .columns =
          {
              ColumnDefinition{
                  .span = FindSpan(
                      source,
                      "id INTEGER CONSTRAINT pk PRIMARY KEY ASC ON CONFLICT REPLACE AUTOINCREMENT"),
                  .name = id_name,
                  .type_name = id_type,
                  .constraints =
                      {
                          ColumnConstraint{
                              .span = primary_key,
                              .name = pk_name,
                              .payload =
                                  PrimaryKeyColumnConstraint{
                                      .order = SortOrder::kAscending,
                                      .conflict = ConflictAction::kReplace,
                                      .autoincrement = true,
                                  },
                          },
                      },
              },
              ColumnDefinition{
                  .span = name_column,
                  .name = name_name,
                  .type_name = name_type,
                  .constraints =
                      {
                          ColumnConstraint{
                              .span = not_null,
                              .payload =
                                  NotNullColumnConstraint{
                                      .conflict = ConflictAction::kFail,
                                  },
                          },
                          ColumnConstraint{
                              .span = unique_column,
                              .payload =
                                  UniqueColumnConstraint{
                                      .conflict = ConflictAction::kIgnore,
                                  },
                          },
                          ColumnConstraint{
                              .span = collate_column,
                              .payload =
                                  CollateColumnConstraint{
                                      .collation = first_nocase,
                                  },
                          },
                          ColumnConstraint{
                              .span = default_column,
                              .payload =
                                  DefaultColumnConstraint{
                                      .expression = ExpressionId{0},
                                  },
                          },
                      },
              },
              ColumnDefinition{
                  .span = nickname_column,
                  .name = nickname_name,
                  .type_name = nickname_type,
                  .constraints =
                      {
                          ColumnConstraint{
                              .span = null_constraint,
                              .payload =
                                  NullColumnConstraint{
                                      .conflict = ConflictAction::kAbort,
                                  },
                          },
                      },
              },
              ColumnDefinition{
                  .span = score_column,
                  .name = score_name,
                  .type_name = score_type,
                  .constraints =
                      {
                          ColumnConstraint{
                              .span = score_check,
                              .payload =
                                  CheckColumnConstraint{
                                      .expression = ExpressionId{3},
                                  },
                          },
                      },
              },
          },
      .constraints =
          {
              TableConstraint{
                  .span = table_unique,
                  .name = uq_name,
                  .payload =
                      UniqueTableConstraint{
                          .terms =
                              {
                                  IndexedTerm{
                                      .span = unique_term,
                                      .expression = ExpressionId{4},
                                      .collation = second_nocase,
                                      .order = SortOrder::kDescending,
                                  },
                              },
                          .conflict = ConflictAction::kAbort,
                      },
              },
              TableConstraint{
                  .span = table_check,
                  .payload =
                      CheckTableConstraint{
                          .expression = ExpressionId{7},
                      },
              },
          },
      .strict = true,
  };

  auto result = SyntaxTree::Create(source, std::move(expressions), std::move(statement));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  const auto& table = std::get<CreateTableStatement>(result->statement());
  EXPECT_FALSE(table.temporary);
  EXPECT_TRUE(table.if_not_exists);
  EXPECT_TRUE(table.strict);
  EXPECT_FALSE(table.without_rowid);
  ASSERT_EQ(4U, table.columns.size());
  const auto& key = std::get<PrimaryKeyColumnConstraint>(table.columns[0].constraints[0].payload);
  EXPECT_EQ(SortOrder::kAscending, key.order);
  EXPECT_EQ(ConflictAction::kReplace, key.conflict);
  EXPECT_TRUE(key.autoincrement);
  EXPECT_EQ(ConflictAction::kFail,
            std::get<NotNullColumnConstraint>(table.columns[1].constraints[0].payload).conflict);
  EXPECT_EQ(ConflictAction::kIgnore,
            std::get<UniqueColumnConstraint>(table.columns[1].constraints[1].payload).conflict);
  EXPECT_EQ(first_nocase,
            std::get<CollateColumnConstraint>(table.columns[1].constraints[2].payload).collation);
  EXPECT_EQ(ExpressionId{0},
            std::get<DefaultColumnConstraint>(table.columns[1].constraints[3].payload).expression);
  EXPECT_EQ(ConflictAction::kAbort,
            std::get<NullColumnConstraint>(table.columns[2].constraints[0].payload).conflict);
  ASSERT_EQ(2U, table.constraints.size());
  const auto& unique = std::get<UniqueTableConstraint>(table.constraints.front().payload);
  ASSERT_EQ(1U, unique.terms.size());
  EXPECT_EQ(SortOrder::kDescending, unique.terms.front().order);
  EXPECT_EQ(ConflictAction::kAbort, unique.conflict);
}

TEST(SyntaxTree, RepresentsCompositePrimaryKeysAndWithoutRowid) {
  const std::string source =
      "CREATE TABLE t (a TEXT, b TEXT, CONSTRAINT pk PRIMARY KEY "
      "(a COLLATE nocase DESC, b ASC) ON CONFLICT ROLLBACK) WITHOUT ROWID;";
  const SourceSpan statement_span = Span(0, source.size() - 1U);
  const SourceSpan table_name = FindSpan(source, "t");
  const SourceSpan first_column = FindSpan(source, "a TEXT");
  const SourceSpan first_name = FindSpan(source, "a");
  const SourceSpan first_type = FindSpan(source, "TEXT");
  const SourceSpan second_column = FindSpan(source, "b TEXT");
  const SourceSpan second_name = FindSpan(source, "b", first_name.end().value());
  const SourceSpan second_type = FindSpan(source, "TEXT", first_type.end().value());
  const SourceSpan constraint = FindSpan(
      source, "CONSTRAINT pk PRIMARY KEY (a COLLATE nocase DESC, b ASC) ON CONFLICT ROLLBACK");
  const SourceSpan constraint_name = FindSpan(source, "pk");
  const SourceSpan first_term = FindSpan(source, "a COLLATE nocase DESC");
  const SourceSpan first_expression =
      Span(first_term.begin().value(), first_term.begin().value() + 1U);
  const SourceSpan collation = FindSpan(source, "nocase");
  const SourceSpan second_term = FindSpan(source, "b ASC");
  const SourceSpan second_expression =
      Span(second_term.begin().value(), second_term.begin().value() + 1U);

  std::vector<Expression> expressions{
      Expression{
          .span = first_expression,
          .payload =
              IdentifierExpression{
                  .name = Name(first_expression, {first_expression}),
              },
      },
      Expression{
          .span = second_expression,
          .payload =
              IdentifierExpression{
                  .name = Name(second_expression, {second_expression}),
              },
      },
  };
  Statement statement = CreateTableStatement{
      .span = statement_span,
      .name = Name(table_name, {table_name}),
      .columns =
          {
              ColumnDefinition{
                  .span = first_column,
                  .name = first_name,
                  .type_name = first_type,
              },
              ColumnDefinition{
                  .span = second_column,
                  .name = second_name,
                  .type_name = second_type,
              },
          },
      .constraints =
          {
              TableConstraint{
                  .span = constraint,
                  .name = constraint_name,
                  .payload =
                      PrimaryKeyTableConstraint{
                          .terms =
                              {
                                  IndexedTerm{
                                      .span = first_term,
                                      .expression = ExpressionId{0},
                                      .collation = collation,
                                      .order = SortOrder::kDescending,
                                  },
                                  IndexedTerm{
                                      .span = second_term,
                                      .expression = ExpressionId{1},
                                      .order = SortOrder::kAscending,
                                  },
                              },
                          .conflict = ConflictAction::kRollback,
                      },
              },
          },
      .without_rowid = true,
  };

  auto result = SyntaxTree::Create(source, std::move(expressions), std::move(statement));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  const auto& table = std::get<CreateTableStatement>(result->statement());
  EXPECT_TRUE(table.without_rowid);
  const auto& primary_key = std::get<PrimaryKeyTableConstraint>(table.constraints.front().payload);
  ASSERT_EQ(2U, primary_key.terms.size());
  EXPECT_EQ(SortOrder::kDescending, primary_key.terms[0].order);
  EXPECT_EQ(SortOrder::kAscending, primary_key.terms[1].order);
  EXPECT_EQ(ConflictAction::kRollback, primary_key.conflict);
  EXPECT_FALSE(primary_key.autoincrement);
}

TEST(SyntaxTree, PreservesTablePrimaryKeyAutoincrement) {
  const std::string source =
      "CREATE TABLE t (id INTEGER, PRIMARY KEY (id AUTOINCREMENT) ON CONFLICT REPLACE);";
  const SourceSpan statement_span = Span(0, source.size() - 1U);
  const SourceSpan table_name = FindSpan(source, "t");
  const SourceSpan column = FindSpan(source, "id INTEGER");
  const SourceSpan column_name = FindSpan(source, "id");
  const SourceSpan column_type = FindSpan(source, "INTEGER");
  const SourceSpan constraint =
      FindSpan(source, "PRIMARY KEY (id AUTOINCREMENT) ON CONFLICT REPLACE");
  const SourceSpan key_expression = FindSpan(source, "id", constraint.begin().value());

  std::vector<Expression> expressions{
      Expression{
          .span = key_expression,
          .payload =
              IdentifierExpression{
                  .name = Name(key_expression, {key_expression}),
              },
      },
  };
  Statement statement = CreateTableStatement{
      .span = statement_span,
      .name = Name(table_name, {table_name}),
      .columns =
          {
              ColumnDefinition{
                  .span = column,
                  .name = column_name,
                  .type_name = column_type,
              },
          },
      .constraints =
          {
              TableConstraint{
                  .span = constraint,
                  .payload =
                      PrimaryKeyTableConstraint{
                          .terms =
                              {
                                  IndexedTerm{
                                      .span = key_expression,
                                      .expression = ExpressionId{0},
                                  },
                              },
                          .conflict = ConflictAction::kReplace,
                          .autoincrement = true,
                      },
              },
          },
  };

  auto result = SyntaxTree::Create(source, std::move(expressions), std::move(statement));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  const auto& table = std::get<CreateTableStatement>(result->statement());
  const auto& primary_key = std::get<PrimaryKeyTableConstraint>(table.constraints.front().payload);
  EXPECT_TRUE(primary_key.autoincrement);
  EXPECT_EQ(ConflictAction::kReplace, primary_key.conflict);
}

TEST(SyntaxTree, RepresentsCreateIndexTermsAndPartialPredicate) {
  const std::string source =
      "CREATE UNIQUE INDEX IF NOT EXISTS main.idx ON items "
      "(name COLLATE nocase DESC, lower(name) ASC) WHERE id > 0;";
  const SourceSpan statement_span = Span(0, source.size() - 1U);
  const SourceSpan index_name = FindSpan(source, "main.idx");
  const SourceSpan main_part = FindSpan(source, "main");
  const SourceSpan index_part = FindSpan(source, "idx");
  const SourceSpan table_name = FindSpan(source, "items");
  const SourceSpan first_term = FindSpan(source, "name COLLATE nocase DESC");
  const SourceSpan first_name = Span(first_term.begin().value(), first_term.begin().value() + 4U);
  const SourceSpan collation = FindSpan(source, "nocase");
  const SourceSpan second_term = FindSpan(source, "lower(name) ASC");
  const SourceSpan function = FindSpan(source, "lower(name)");
  const SourceSpan function_name = FindSpan(source, "lower");
  const SourceSpan argument = FindSpan(source, "name", function_name.end().value());
  const SourceSpan where = FindSpan(source, "id > 0");
  const SourceSpan where_id = FindSpan(source, "id", where.begin().value());
  const SourceSpan where_zero = FindSpan(source, "0", where.begin().value());

  std::vector<Expression> expressions{
      Expression{
          .span = first_name,
          .payload =
              IdentifierExpression{
                  .name = Name(first_name, {first_name}),
              },
      },
      Expression{
          .span = argument,
          .payload =
              IdentifierExpression{
                  .name = Name(argument, {argument}),
              },
      },
      Expression{
          .span = function,
          .payload =
              FunctionCallExpression{
                  .name = Name(function_name, {function_name}),
                  .arguments = {ExpressionId{1}},
              },
      },
      Expression{
          .span = where_id,
          .payload =
              IdentifierExpression{
                  .name = Name(where_id, {where_id}),
              },
      },
      Expression{
          .span = where_zero,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = where_zero,
              },
      },
      Expression{
          .span = where,
          .payload =
              BinaryExpression{
                  .op = BinaryOperator::kGreater,
                  .operator_span = FindSpan(source, ">", where.begin().value()),
                  .left = ExpressionId{3},
                  .right = ExpressionId{4},
              },
      },
  };
  Statement statement = CreateIndexStatement{
      .span = statement_span,
      .unique = true,
      .if_not_exists = true,
      .name = Name(index_name, {main_part, index_part}),
      .table = Name(table_name, {table_name}),
      .terms =
          {
              IndexedTerm{
                  .span = first_term,
                  .expression = ExpressionId{0},
                  .collation = collation,
                  .order = SortOrder::kDescending,
              },
              IndexedTerm{
                  .span = second_term,
                  .expression = ExpressionId{2},
                  .order = SortOrder::kAscending,
              },
          },
      .where = ExpressionId{5},
  };

  auto result = SyntaxTree::Create(source, std::move(expressions), std::move(statement));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  const auto& index = std::get<CreateIndexStatement>(result->statement());
  EXPECT_TRUE(index.unique);
  EXPECT_TRUE(index.if_not_exists);
  ASSERT_EQ(2U, index.terms.size());
  EXPECT_EQ(collation, index.terms[0].collation);
  EXPECT_EQ(ExpressionId{5}, index.where);
}

TEST(SyntaxTree, MovePreservesIdsAndSpansButRequiresReacquiredViews) {
  std::string source = "SELECT 1";
  const SourceSpan statement_span = Span(0, source.size());
  const SourceSpan literal_span = Span(7, source.size());
  const ExpressionId root{0};
  std::vector<Expression> expressions{
      Expression{
          .span = literal_span,
          .payload =
              LiteralExpression{
                  .kind = LiteralKind::kInteger,
                  .token = literal_span,
              },
      },
  };
  auto result = SyntaxTree::Create(std::move(source), std::move(expressions),
                                   SingleResultSelect(statement_span, literal_span, root));
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  const SyntaxTree moved = std::move(*result);

  EXPECT_EQ("SELECT 1", moved.source().bytes());
  EXPECT_EQ(literal_span, moved.expression(root).span);
  ASSERT_EQ(1U, moved.expressions().size());
  const auto text = Slice(moved.source(), literal_span);
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ("1", text->bytes());
}

TEST(SyntaxTree, RejectsInvalidSpansAndQualifiedNames) {
  {
    std::string source = "SELECT 1";
    const SourceSpan literal = Span(7, source.size() + 1U);
    std::vector<Expression> expressions{
        Expression{
            .span = literal,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = literal,
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 8), Span(7, 8), ExpressionId{0})));
  }
  {
    std::string source = "SELECT a";
    const SourceSpan identifier = Span(7, 8);
    std::vector<Expression> expressions{
        Expression{
            .span = identifier,
            .payload =
                IdentifierExpression{
                    .name =
                        QualifiedName{
                            .span = identifier,
                            .parts = {},
                        },
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 8), identifier, ExpressionId{0})));
  }
  {
    std::string source = "SELECT a.b.c.d";
    const SourceSpan identifier = Span(7, source.size());
    std::vector<Expression> expressions{
        Expression{
            .span = identifier,
            .payload =
                IdentifierExpression{
                    .name = Name(identifier, {Span(7, 8), Span(9, 10), Span(11, 12), Span(13, 14)}),
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 14), identifier, ExpressionId{0})));
  }
  {
    std::string source = " SELECT 1";
    const SourceSpan literal = Span(8, 9);
    std::vector<Expression> expressions{
        Expression{
            .span = literal,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = literal,
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(1, 9), literal, ExpressionId{0})));
  }
}

TEST(SyntaxTree, RejectsForwardDuplicateAndUnreferencedExpressionOwnership) {
  {
    std::string source = "SELECT (1)";
    const SourceSpan parent = Span(7, 10);
    std::vector<Expression> expressions{
        Expression{
            .span = parent,
            .payload = ParenthesizedExpression{.inner = ExpressionId{99}},
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 10), parent, ExpressionId{0})));
  }
  {
    std::string source = "SELECT (1)";
    const SourceSpan parent = Span(7, 10);
    std::vector<Expression> expressions{
        Expression{
            .span = parent,
            .payload = ParenthesizedExpression{.inner = ExpressionId{1}},
        },
        Expression{
            .span = Span(8, 9),
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = Span(8, 9),
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 10), parent, ExpressionId{0})));
  }
  {
    std::string source = "SELECT -1, 1";
    std::vector<Expression> expressions{
        Expression{
            .span = Span(8, 9),
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = Span(8, 9),
                },
        },
        Expression{
            .span = Span(7, 9),
            .payload =
                UnaryExpression{
                    .op = UnaryOperator::kNegative,
                    .operator_span = Span(7, 8),
                    .operand = ExpressionId{0},
                },
        },
    };
    Statement statement = SelectStatement{
        .span = Span(0, source.size()),
        .result_columns =
            {
                ResultColumn{.span = Span(7, 9), .expression = ExpressionId{1}},
                ResultColumn{.span = Span(11, 12), .expression = ExpressionId{0}},
            },
    };
    ExpectMisuse(
        SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement)));
  }
  {
    std::string source = "SELECT 2";
    std::vector<Expression> expressions{
        Expression{
            .span = Span(7, 8),
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = Span(7, 8),
                },
        },
        Expression{
            .span = Span(7, 8),
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = Span(7, 8),
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 8), Span(7, 8), ExpressionId{1})));
  }
}

TEST(SyntaxTree, RejectsChildAndComponentSpansOutsideTheirOwners) {
  {
    std::string source = "SELECT (1)";
    std::vector<Expression> expressions{
        Expression{
            .span = Span(0, source.size()),
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = Span(8, 9),
                },
        },
        Expression{
            .span = Span(7, 10),
            .payload = ParenthesizedExpression{.inner = ExpressionId{0}},
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 10), Span(7, 10), ExpressionId{1})));
  }
  {
    std::string source = "SELECT 1";
    const SourceSpan literal = Span(7, 8);
    std::vector<Expression> expressions{
        Expression{
            .span = literal,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = literal,
                },
        },
    };
    Statement statement = SelectStatement{
        .span = Span(0, 8),
        .result_columns =
            {
                ResultColumn{
                    .span = Span(0, 6),
                    .expression = ExpressionId{0},
                },
            },
    };
    ExpectMisuse(
        SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement)));
  }
  {
    std::string source = "SELECT -1";
    std::vector<Expression> expressions{
        Expression{
            .span = Span(8, 9),
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = Span(8, 9),
                },
        },
        Expression{
            .span = Span(0, 9),
            .payload =
                UnaryExpression{
                    .op = UnaryOperator::kNegative,
                    .operator_span = Span(7, 8),
                    .operand = ExpressionId{0},
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 9), Span(0, 9), ExpressionId{1})));
  }
  {
    std::string source = "SELECT 1 + 2";
    std::vector<Expression> expressions{
        Expression{
            .span = Span(7, 8),
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = Span(7, 8),
                },
        },
        Expression{
            .span = Span(11, 12),
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = Span(11, 12),
                },
        },
        Expression{
            .span = Span(0, 12),
            .payload =
                BinaryExpression{
                    .op = BinaryOperator::kAdd,
                    .operator_span = Span(9, 10),
                    .left = ExpressionId{0},
                    .right = ExpressionId{1},
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 12), Span(0, 12), ExpressionId{2})));
  }
  {
    const std::string source = "CREATE TABLE t(a TEXT)";
    const SourceSpan table_name = FindSpan(source, "t");
    const SourceSpan column = FindSpan(source, "a TEXT");
    const SourceSpan actual_name = FindSpan(source, "a", column.begin().value());
    const SourceSpan actual_type = FindSpan(source, "TEXT");
    Statement statement = CreateTableStatement{
        .span = Span(0, source.size()),
        .name = Name(table_name, {table_name}),
        .columns =
            {
                ColumnDefinition{
                    .span = column,
                    .name = actual_type,
                    .type_name = actual_name,
                },
            },
    };
    ExpectMisuse(SyntaxTree::Create(source, {}, std::move(statement)));
  }
}

TEST(SyntaxTree, RejectsQualifiedFormsForbiddenBySQLiteGrammar) {
  {
    std::string source = "SELECT main.f(1)";
    const SourceSpan argument = FindSpan(source, "1");
    const SourceSpan function = FindSpan(source, "main.f(1)");
    const SourceSpan function_name = FindSpan(source, "main.f");
    std::vector<Expression> expressions{
        Expression{
            .span = argument,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = argument,
                },
        },
        Expression{
            .span = function,
            .payload =
                FunctionCallExpression{
                    .name = Name(function_name, {FindSpan(source, "main"), FindSpan(source, "f")}),
                    .arguments = {ExpressionId{0}},
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 16), function, ExpressionId{1})));
  }
  {
    std::string source = "SELECT main.t.*";
    const SourceSpan wildcard = FindSpan(source, "main.t.*");
    const SourceSpan qualifier = FindSpan(source, "main.t");
    std::vector<Expression> expressions{
        Expression{
            .span = wildcard,
            .payload =
                WildcardExpression{
                    .asterisk = Span(wildcard.end().value() - 1U, wildcard.end().value()),
                    .qualifier =
                        Name(qualifier, {FindSpan(source, "main"),
                                         FindSpan(source, "t", qualifier.begin().value())}),
                },
        },
    };
    ExpectMisuse(SyntaxTree::Create(std::move(source), std::move(expressions),
                                    SingleResultSelect(Span(0, 15), wildcard, ExpressionId{0})));
  }
  {
    const std::string source = "CREATE INDEX i ON main.t(a)";
    const SourceSpan index_name = FindSpan(source, "i");
    const SourceSpan table_name = FindSpan(source, "main.t");
    const SourceSpan term = FindSpan(source, "a", table_name.end().value());
    std::vector<Expression> expressions{
        Expression{
            .span = term,
            .payload =
                IdentifierExpression{
                    .name = Name(term, {term}),
                },
        },
    };
    Statement statement = CreateIndexStatement{
        .span = Span(0, source.size()),
        .name = Name(index_name, {index_name}),
        .table = Name(table_name, {FindSpan(source, "main"),
                                   FindSpan(source, "t", table_name.begin().value())}),
        .terms =
            {
                IndexedTerm{
                    .span = term,
                    .expression = ExpressionId{0},
                },
            },
    };
    ExpectMisuse(SyntaxTree::Create(source, std::move(expressions), std::move(statement)));
  }
}

TEST(SyntaxTree, RejectsInvalidStatementShapes) {
  {
    Statement statement = SelectStatement{
        .span = Span(0, 8),
        .result_columns = {},
    };
    ExpectMisuse(SyntaxTree::Create("SELECT 1", {}, std::move(statement)));
  }
  {
    Statement statement = CreateTableStatement{
        .span = Span(0, 16),
        .name = Name(Span(13, 14), {Span(13, 14)}),
        .columns = {},
    };
    ExpectMisuse(SyntaxTree::Create("CREATE TABLE t()", {}, std::move(statement)));
  }
  {
    Statement statement = CreateIndexStatement{
        .span = Span(0, 21),
        .name = Name(Span(13, 14), {Span(13, 14)}),
        .table = Name(Span(18, 19), {Span(18, 19)}),
        .terms = {},
    };
    ExpectMisuse(SyntaxTree::Create("CREATE INDEX i ON t()", {}, std::move(statement)));
  }
  {
    std::string source = "SELECT 1 LIMIT 1";
    const SourceSpan first = FindSpan(source, "1");
    const SourceSpan limit = FindSpan(source, "1", first.end().value());
    std::vector<Expression> expressions{
        Expression{
            .span = first,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = first,
                },
        },
        Expression{
            .span = limit,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = limit,
                },
        },
    };
    Statement statement = SelectStatement{
        .span = Span(0, source.size()),
        .result_columns =
            {
                ResultColumn{.span = first, .expression = ExpressionId{0}},
            },
        .limit =
            LimitClause{
                .span = FindSpan(source, "LIMIT 1"),
                .limit = ExpressionId{1},
                .syntax = LimitSyntax::kOffsetKeyword,
            },
    };
    ExpectMisuse(
        SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement)));
  }
}

TEST(SyntaxTree, RejectsInvalidDmlAndTransactionStatementShapes) {
  {
    const std::string source = "INSERT INTO t VALUES()";
    const SourceSpan table = FindSpan(source, "t");
    Statement statement = InsertStatement{
        .span = Span(0, source.size()),
        .table = Name(table, {table}),
        .columns = {},
        .source =
            InsertValuesSource{
                .span = FindSpan(source, "VALUES()"),
                .values = {},
            },
    };
    ExpectMisuse(SyntaxTree::Create(source, {}, std::move(statement)));
  }
  {
    const std::string source = "INSERT INTO t VALUES(1,2)";
    const SourceSpan table = FindSpan(source, "t");
    const SourceSpan first = FindSpan(source, "1");
    const SourceSpan second = FindSpan(source, "2");
    std::vector<Expression> expressions{
        Expression{
            .span = first,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = first,
                },
        },
        Expression{
            .span = second,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = second,
                },
        },
    };
    Statement statement = InsertStatement{
        .span = Span(0, source.size()),
        .table = Name(table, {table}),
        .columns = {},
        .source =
            InsertValuesSource{
                .span = FindSpan(source, "VALUES(1,2)"),
                .values = {ExpressionId{1}, ExpressionId{0}},
            },
    };
    ExpectMisuse(SyntaxTree::Create(source, std::move(expressions), std::move(statement)));
  }
  {
    const std::string source = "UPDATE t SET";
    const SourceSpan table = FindSpan(source, "t");
    Statement statement = UpdateStatement{
        .span = Span(0, source.size()),
        .table = Name(table, {table}),
        .assignments = {},
    };
    ExpectMisuse(SyntaxTree::Create(source, {}, std::move(statement)));
  }
  {
    const std::string source = "UPDATE t WHERE 1 SET a=2";
    const SourceSpan table = FindSpan(source, "t");
    const SourceSpan predicate = FindSpan(source, "1");
    const SourceSpan value = FindSpan(source, "2");
    std::vector<Expression> expressions{
        Expression{
            .span = predicate,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = predicate,
                },
        },
        Expression{
            .span = value,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = value,
                },
        },
    };
    Statement statement = UpdateStatement{
        .span = Span(0, source.size()),
        .table = Name(table, {table}),
        .assignments =
            {
                UpdateAssignment{
                    .span = FindSpan(source, "a=2"),
                    .column = FindSpan(source, "a"),
                    .expression = ExpressionId{1},
                },
            },
        .where = ExpressionId{0},
    };
    ExpectMisuse(SyntaxTree::Create(source, std::move(expressions), std::move(statement)));
  }
  {
    const std::string source = "WHERE 1 DELETE FROM t";
    const SourceSpan table = FindSpan(source, "t");
    const SourceSpan predicate = FindSpan(source, "1");
    std::vector<Expression> expressions{
        Expression{
            .span = predicate,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = predicate,
                },
        },
    };
    Statement statement = DeleteStatement{
        .span = Span(0, source.size()),
        .table = Name(table, {table}),
        .where = ExpressionId{0},
    };
    ExpectMisuse(SyntaxTree::Create(source, std::move(expressions), std::move(statement)));
  }
  {
    Statement statement = BeginTransactionStatement{
        .span = Span(0, 5),
        .mode = InvalidEnumValue<BeginTransactionMode>(255U),
    };
    ExpectMisuse(SyntaxTree::Create("BEGIN", {}, std::move(statement)));
  }
  {
    Statement statement = CommitTransactionStatement{
        .span = Span(0, 6),
        .syntax = InvalidEnumValue<CommitTransactionSyntax>(255U),
    };
    ExpectMisuse(SyntaxTree::Create("COMMIT", {}, std::move(statement)));
  }
  {
    Statement statement = SavepointStatement{
        .span = Span(0, 11),
        .name = Span(0, 1),
    };
    ExpectMisuse(SyntaxTree::Create("s SAVEPOINT", {}, std::move(statement)));
  }
}

TEST(SyntaxTree, RejectsLimitOperandsThatDisagreeWithRetainedSyntax) {
  {
    std::string source = "SELECT 1 LIMIT 2, 3";
    const SourceSpan result_value = FindSpan(source, "1");
    const SourceSpan source_offset = FindSpan(source, "2");
    const SourceSpan normalized_limit = FindSpan(source, "3");
    std::vector<Expression> expressions{
        Expression{
            .span = result_value,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = result_value,
                },
        },
        Expression{
            .span = source_offset,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = source_offset,
                },
        },
        Expression{
            .span = normalized_limit,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = normalized_limit,
                },
        },
    };
    Statement statement = SelectStatement{
        .span = Span(0, source.size()),
        .result_columns =
            {
                ResultColumn{.span = result_value, .expression = ExpressionId{0}},
            },
        .limit =
            LimitClause{
                .span = FindSpan(source, "LIMIT 2, 3"),
                .limit = ExpressionId{1},
                .offset = ExpressionId{2},
                .syntax = LimitSyntax::kComma,
            },
    };
    ExpectMisuse(
        SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement)));
  }
  {
    std::string source = "SELECT 1 LIMIT 3 OFFSET 2";
    const SourceSpan result_value = FindSpan(source, "1");
    const SourceSpan source_limit = FindSpan(source, "3");
    const SourceSpan source_offset = FindSpan(source, "2");
    std::vector<Expression> expressions{
        Expression{
            .span = result_value,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = result_value,
                },
        },
        Expression{
            .span = source_limit,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = source_limit,
                },
        },
        Expression{
            .span = source_offset,
            .payload =
                LiteralExpression{
                    .kind = LiteralKind::kInteger,
                    .token = source_offset,
                },
        },
    };
    Statement statement = SelectStatement{
        .span = Span(0, source.size()),
        .result_columns =
            {
                ResultColumn{.span = result_value, .expression = ExpressionId{0}},
            },
        .limit =
            LimitClause{
                .span = FindSpan(source, "LIMIT 3 OFFSET 2"),
                .limit = ExpressionId{2},
                .offset = ExpressionId{1},
                .syntax = LimitSyntax::kOffsetKeyword,
            },
    };
    ExpectMisuse(
        SyntaxTree::Create(std::move(source), std::move(expressions), std::move(statement)));
  }
}

TEST(SyntaxTree, BuildsAndDestroysVeryDeepTreesWithoutRecursion) {
  constexpr std::size_t kDepth = 100'000;
  std::string source(kDepth, '(');
  source.push_back('1');
  source.append(kDepth, ')');

  std::vector<Expression> expressions(kDepth + 1U);
  const SourceSpan literal = Span(kDepth, kDepth + 1U);
  expressions[0].span = literal;
  expressions[0].payload.emplace<LiteralExpression>(LiteralExpression{
      .kind = LiteralKind::kInteger,
      .token = literal,
  });
  for (std::size_t level = 1; level <= kDepth; ++level) {
    expressions[level].span = Span(kDepth - level, kDepth + 1U + level);
    expressions[level].payload.emplace<ParenthesizedExpression>(
        ParenthesizedExpression{.inner = ExpressionId{level - 1U}});
  }

  const SourceSpan statement_span = Span(0, source.size());
  auto result =
      SyntaxTree::Create(std::move(source), std::move(expressions),
                         SingleResultSelect(statement_span, statement_span, ExpressionId{kDepth}));

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  EXPECT_EQ(kDepth + 1U, result->expressions().size());
  EXPECT_EQ(statement_span, result->expression(ExpressionId{kDepth}).span);
}

}  // namespace
}  // namespace modern_sqlite
