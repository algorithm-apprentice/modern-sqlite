#include "modern_sqlite/syntax/lexer.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {
namespace {

static_assert(std::is_trivially_copyable_v<Token>);
static_assert(std::is_trivially_copyable_v<Lexer>);

struct ExpectedToken {
  std::string name;
  std::size_t offset = 0;
  std::size_t length = 0;
};

struct LexerVector {
  std::string name;
  std::string source;
  std::vector<ExpectedToken> tokens;
};

[[nodiscard]] constexpr std::uint8_t HexDigit(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(value - 'a' + 10);
  }
  return static_cast<std::uint8_t>(value - 'A' + 10);
}

[[nodiscard]] std::string DecodeHex(std::string_view value) {
  if (value == "-") {
    return {};
  }
  if ((value.size() % 2U) != 0U) {
    throw std::runtime_error{"odd lexer fixture hex length"};
  }

  std::string bytes;
  bytes.reserve(value.size() / 2U);
  for (std::size_t index = 0; index < value.size(); index += 2U) {
    const auto high = static_cast<std::uint32_t>(HexDigit(value[index]));
    const auto low = static_cast<std::uint32_t>(HexDigit(value[index + 1U]));
    bytes.push_back(static_cast<char>((high << 4U) | low));
  }
  return bytes;
}

[[nodiscard]] std::size_t ParseSize(std::string_view value) {
  std::size_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::runtime_error{"invalid lexer fixture size"};
  }
  return result;
}

[[nodiscard]] std::vector<ExpectedToken> ParseExpectedTokens(std::string_view value) {
  std::vector<ExpectedToken> tokens;
  std::size_t offset = 0;
  while (offset < value.size()) {
    const std::size_t comma = value.find(',', offset);
    const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
    const std::string_view encoded = value.substr(offset, end - offset);
    const std::size_t at = encoded.find('@');
    const std::size_t colon = encoded.find(':', at + 1U);
    if (at == std::string_view::npos || colon == std::string_view::npos) {
      throw std::runtime_error{"invalid lexer fixture token"};
    }
    tokens.push_back(ExpectedToken{
        .name = std::string{encoded.substr(0, at)},
        .offset = ParseSize(encoded.substr(at + 1U, colon - at - 1U)),
        .length = ParseSize(encoded.substr(colon + 1U)),
    });
    offset = end + static_cast<std::size_t>(comma != std::string_view::npos);
  }
  return tokens;
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "lexer" / "sqlite-3.54.0-tokenizer.tsv";
}

[[nodiscard]] std::vector<LexerVector> ReadVectors() {
  std::ifstream input{FixturePath()};
  if (!input) {
    throw std::runtime_error{"unable to open lexer fixture"};
  }

  std::vector<LexerVector> vectors;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t first_tab = line.find('\t');
    const std::size_t second_tab = line.find('\t', first_tab + 1U);
    if (first_tab == std::string::npos || second_tab == std::string::npos) {
      throw std::runtime_error{"invalid lexer fixture row"};
    }
    vectors.push_back(LexerVector{
        .name = line.substr(0, first_tab),
        .source =
            DecodeHex(std::string_view{line}.substr(first_tab + 1U, second_tab - first_tab - 1U)),
        .tokens = ParseExpectedTokens(std::string_view{line}.substr(second_tab + 1U)),
    });
  }
  return vectors;
}

TEST(Lexer, MatchesPinnedSqliteRawTokenVectors) {
  const std::vector<LexerVector> vectors = ReadVectors();
  ASSERT_EQ(348U, vectors.size());

  for (const LexerVector& vector : vectors) {
    SCOPED_TRACE(vector.name);
    Lexer lexer{Utf8View{vector.source}};
    std::size_t previous_end = 0;

    for (const ExpectedToken& expected : vector.tokens) {
      const Token actual = lexer.Next();
      EXPECT_EQ(expected.name, TokenKindName(actual.kind));
      EXPECT_EQ(expected.offset, actual.span.begin().value());
      EXPECT_EQ(expected.length, actual.span.length().value());
      EXPECT_EQ(expected.offset + expected.length, actual.span.end().value());
      EXPECT_EQ(expected.offset, previous_end);
      EXPECT_EQ(actual.span.end(), lexer.position());

      if (actual.kind == TokenKind::kEndOfInput) {
        EXPECT_TRUE(actual.span.empty());
        EXPECT_TRUE(lexer.finished());
        EXPECT_EQ(actual, lexer.Next());
      } else {
        EXPECT_FALSE(actual.span.empty());
      }
      previous_end = actual.span.end().value();
    }
  }
}

TEST(Lexer, ReportsPositionAndFinishedStateAtPhysicalAndNullEnds) {
  Lexer empty{Utf8View{}};
  EXPECT_EQ(0U, empty.position().value());
  EXPECT_TRUE(empty.finished());
  EXPECT_EQ(TokenKind::kEndOfInput, empty.Next().kind);

  Lexer physical_end{Utf8View{"abc"}};
  EXPECT_FALSE(physical_end.finished());
  const Token identifier = physical_end.Next();
  EXPECT_EQ(TokenKind::kIdentifier, identifier.kind);
  EXPECT_EQ(3U, physical_end.position().value());
  EXPECT_TRUE(physical_end.finished());
  EXPECT_EQ(TokenKind::kEndOfInput, physical_end.Next().kind);

  const std::string with_null{"a\0tail", 6};
  Lexer logical_end{Utf8View{with_null}};
  EXPECT_EQ(TokenKind::kIdentifier, logical_end.Next().kind);
  EXPECT_EQ(1U, logical_end.position().value());
  EXPECT_TRUE(logical_end.finished());
  const Token end = logical_end.Next();
  EXPECT_EQ(TokenKind::kEndOfInput, end.kind);
  EXPECT_EQ(1U, end.span.begin().value());
  EXPECT_EQ(1U, end.span.end().value());
}

TEST(Lexer, ExposesRawTriviaAndKeywordClassification) {
  Lexer lexer{Utf8View{" \t/*x*/ SELECT WINDOW OVER FILTER true WITHIN"}};
  const std::vector<TokenKind> expected{
      TokenKind::kWhitespace, TokenKind::kComment,    TokenKind::kWhitespace,
      TokenKind::kSelect,     TokenKind::kWhitespace, TokenKind::kWindow,
      TokenKind::kWhitespace, TokenKind::kOver,       TokenKind::kWhitespace,
      TokenKind::kFilter,     TokenKind::kWhitespace, TokenKind::kIdentifier,
      TokenKind::kWhitespace, TokenKind::kIdentifier, TokenKind::kEndOfInput,
  };

  for (const TokenKind kind : expected) {
    const Token token = lexer.Next();
    ASSERT_EQ(kind, token.kind);
    EXPECT_EQ(kind == TokenKind::kWhitespace || kind == TokenKind::kComment, IsTrivia(kind));
  }

  EXPECT_TRUE(IsKeyword(TokenKind::kSelect));
  EXPECT_TRUE(IsKeyword(TokenKind::kWindow));
  EXPECT_TRUE(IsKeyword(TokenKind::kOver));
  EXPECT_TRUE(IsKeyword(TokenKind::kFilter));
  EXPECT_FALSE(IsKeyword(TokenKind::kIdentifier));
  EXPECT_FALSE(IsKeyword(TokenKind::kString));
  EXPECT_FALSE(IsKeyword(TokenKind::kEndOfInput));
}

TEST(Lexer, ReturnsStableNamesForEveryUnderlyingValue) {
  for (std::uint16_t value = 0; value <= std::numeric_limits<std::uint8_t>::max(); ++value) {
    const auto kind = static_cast<TokenKind>(value);
    EXPECT_FALSE(TokenKindName(kind).empty());
  }
  EXPECT_EQ("unknown", TokenKindName(static_cast<TokenKind>(255)));
}

}  // namespace
}  // namespace modern_sqlite
