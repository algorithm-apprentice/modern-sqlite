#include "modern_sqlite/syntax/parser.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace modern_sqlite {
namespace {

enum class NameClass : std::uint8_t {
  kId,
  kIds,
  kIdj,
  kNm,
};

struct ExpressionOptions {
  bool allow_wildcard = false;
};

struct InfixOperator {
  BinaryOperator op = BinaryOperator::kAdd;
  int binding = 0;
  std::size_t token_count = 1;
  bool adds_negation_level = false;
};

struct ParsedTableOptions {
  bool without_rowid = false;
  bool strict = false;
};

using TokenResult = std::expected<Token, ParseError>;
using NameResult = std::expected<QualifiedName, ParseError>;
using ExpressionResult = std::expected<ExpressionId, ParseError>;
using StatementResult = std::expected<Statement, ParseError>;

struct ParsedQueryCore {
  QueryCore core;
  std::vector<OrderingTerm> order_by{};
  std::optional<LimitClause> limit{};
};

struct ParsedCompoundOperator {
  CompoundOperator operation = CompoundOperator::kUnion;
  SourceSpan span{};
};

using QueryCoreResult = std::expected<ParsedQueryCore, ParseError>;
using CompoundOperatorResult = std::expected<ParsedCompoundOperator, ParseError>;

[[nodiscard]] SourceSpan MakeSpan(ByteOffset begin, ByteOffset end) noexcept {
  const auto span = SourceSpan::FromBounds(begin, end);
  assert(span.has_value());
  return *span;
}

[[nodiscard]] bool EqualsAsciiCaseInsensitive(std::string_view left,
                                              std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto left_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(left[index]));
    const auto right_byte = static_cast<std::uint8_t>(static_cast<unsigned char>(right[index]));
    if (SqliteToLower(left_byte) != SqliteToLower(right_byte)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool IsFallbackToIdentifier(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::kAbort:
    case TokenKind::kAction:
    case TokenKind::kAfter:
    case TokenKind::kAnalyze:
    case TokenKind::kAsc:
    case TokenKind::kAttach:
    case TokenKind::kBefore:
    case TokenKind::kBegin:
    case TokenKind::kBy:
    case TokenKind::kCascade:
    case TokenKind::kCast:
    case TokenKind::kColumnKeyword:
    case TokenKind::kConflict:
    case TokenKind::kDatabase:
    case TokenKind::kDeferred:
    case TokenKind::kDesc:
    case TokenKind::kDetach:
    case TokenKind::kDo:
    case TokenKind::kEach:
    case TokenKind::kEnd:
    case TokenKind::kExclusive:
    case TokenKind::kExplain:
    case TokenKind::kFail:
    case TokenKind::kFor:
    case TokenKind::kIgnore:
    case TokenKind::kImmediate:
    case TokenKind::kInitially:
    case TokenKind::kInstead:
    case TokenKind::kLikeKeyword:
    case TokenKind::kMatch:
    case TokenKind::kNo:
    case TokenKind::kPlan:
    case TokenKind::kQuery:
    case TokenKind::kKey:
    case TokenKind::kOf:
    case TokenKind::kOffset:
    case TokenKind::kPragma:
    case TokenKind::kRaise:
    case TokenKind::kRecursive:
    case TokenKind::kRelease:
    case TokenKind::kReplace:
    case TokenKind::kRestrict:
    case TokenKind::kRow:
    case TokenKind::kRows:
    case TokenKind::kRollback:
    case TokenKind::kSavepoint:
    case TokenKind::kTemp:
    case TokenKind::kTrigger:
    case TokenKind::kVacuum:
    case TokenKind::kView:
    case TokenKind::kVirtual:
    case TokenKind::kWith:
    case TokenKind::kWithout:
    case TokenKind::kNulls:
    case TokenKind::kFirst:
    case TokenKind::kLast:
    case TokenKind::kCurrent:
    case TokenKind::kFollowing:
    case TokenKind::kPartition:
    case TokenKind::kPreceding:
    case TokenKind::kRange:
    case TokenKind::kUnbounded:
    case TokenKind::kExclude:
    case TokenKind::kGroups:
    case TokenKind::kOthers:
    case TokenKind::kTies:
    case TokenKind::kGenerated:
    case TokenKind::kAlways:
    case TokenKind::kMaterialized:
    case TokenKind::kReindex:
    case TokenKind::kRename:
    case TokenKind::kCurrentTimeKeyword:
    case TokenKind::kIf:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool IsNameToken(TokenKind kind, NameClass name_class) noexcept {
  if (kind == TokenKind::kIdentifier || IsFallbackToIdentifier(kind)) {
    return true;
  }
  switch (name_class) {
    case NameClass::kId:
      return kind == TokenKind::kIndexed;
    case NameClass::kIds:
      return kind == TokenKind::kString;
    case NameClass::kIdj:
      return kind == TokenKind::kIndexed || kind == TokenKind::kJoinKeyword;
    case NameClass::kNm:
      return kind == TokenKind::kIndexed || kind == TokenKind::kJoinKeyword ||
             kind == TokenKind::kString;
  }
  return false;
}

[[nodiscard]] bool IsContextLookaheadIdentifier(TokenKind kind) noexcept {
  return kind == TokenKind::kIdentifier || kind == TokenKind::kString ||
         kind == TokenKind::kJoinKeyword || kind == TokenKind::kWindow ||
         kind == TokenKind::kOver || IsFallbackToIdentifier(kind);
}

[[nodiscard]] Token ReadRawSignificant(Lexer& lexer) noexcept {
  Token token = lexer.Next();
  while (IsTrivia(token.kind)) {
    token = lexer.Next();
  }
  return token;
}

class Parser final {
 public:
  Parser(Utf8View source, ByteOffset base_offset, ParseOptions options) noexcept
      : source_(source), base_offset_(base_offset), lexer_(source), options_(options) {}

  [[nodiscard]] ParseResult Parse() {
    StatementResult statement = ParseStatement();
    if (!statement.has_value()) {
      return std::unexpected(statement.error());
    }

    const Token terminator = Peek();
    std::size_t source_end = 0;
    ByteOffset next_offset;
    Token invariant_token = terminator;
    if (terminator.kind == TokenKind::kSemicolon) {
      const Token semicolon = Consume();
      source_end = semicolon.span.end().value();
      next_offset = AbsoluteOffset(semicolon.span.end());
      invariant_token = semicolon;
    } else if (terminator.kind == TokenKind::kEndOfInput) {
      source_end = StatementSpan(*statement).end().value();
      next_offset = AbsoluteOffset(terminator.span.begin());
    } else if (terminator.kind == TokenKind::kIllegal) {
      return std::unexpected(IllegalToken(terminator));
    } else {
      return std::unexpected(Unexpected(terminator, ParseExpectation::kEndOfStatement));
    }

    std::string owned_source{
        source_.bytes().substr(0, source_end),
    };
    auto tree =
        SyntaxTree::Create(std::move(owned_source), std::move(expressions_), std::move(*statement));
    if (!tree.has_value()) {
      return std::unexpected(ParseError{
          .code = ParseErrorCode::kInternalInvariant,
          .span = AbsoluteSpan(invariant_token.span),
          .actual = invariant_token.kind,
          .expected = ParseExpectation::kNone,
          .next_offset = next_offset,
      });
    }
    return ParseOutput{
        .tree = std::move(*tree),
        .next_offset = next_offset,
    };
  }

 private:
  static constexpr int kOrBinding = 10;
  static constexpr int kAndBinding = 20;
  static constexpr int kNotPrefixBinding = 30;
  static constexpr int kComparisonBinding = 40;
  static constexpr int kRelationalBinding = 50;
  static constexpr int kBitwiseBinding = 60;
  static constexpr int kAdditiveBinding = 70;
  static constexpr int kMultiplicativeBinding = 80;
  static constexpr int kConcatenateBinding = 90;
  static constexpr int kCollateBinding = 100;
  static constexpr int kUnaryBinding = 110;

  [[nodiscard]] ByteOffset AbsoluteOffset(ByteOffset local) const noexcept {
    return ByteOffset{base_offset_.value() + local.value()};
  }

  [[nodiscard]] SourceSpan AbsoluteSpan(SourceSpan local) const noexcept {
    return MakeSpan(AbsoluteOffset(local.begin()), AbsoluteOffset(local.end()));
  }

  [[nodiscard]] std::string_view Text(Token token) const noexcept {
    assert(token.span.end().value() <= source_.size_bytes());
    return std::string_view{source_.data() + token.span.begin().value(),
                            token.span.length().value()};
  }

  [[nodiscard]] ParseError IllegalToken(Token token) const noexcept {
    return ParseError{
        .code = ParseErrorCode::kIllegalToken,
        .span = AbsoluteSpan(token.span),
        .actual = TokenKind::kIllegal,
        .expected = ParseExpectation::kNone,
        .next_offset = AbsoluteOffset(token.span.begin()),
    };
  }

  [[nodiscard]] ParseError Unexpected(Token token, ParseExpectation expected) const noexcept {
    if (token.kind == TokenKind::kIllegal) {
      return IllegalToken(token);
    }
    const ByteOffset next =
        token.kind == TokenKind::kEndOfInput ? token.span.begin() : token.span.end();
    return ParseError{
        .code = ParseErrorCode::kUnexpectedToken,
        .span = AbsoluteSpan(token.span),
        .actual = token.kind,
        .expected = expected,
        .next_offset = AbsoluteOffset(next),
    };
  }

  [[nodiscard]] ParseError Unsupported(Token token) const noexcept {
    if (token.kind == TokenKind::kIllegal) {
      return IllegalToken(token);
    }
    const ByteOffset next =
        token.kind == TokenKind::kEndOfInput ? token.span.begin() : token.span.end();
    return ParseError{
        .code = ParseErrorCode::kUnsupportedSyntax,
        .span = AbsoluteSpan(token.span),
        .actual = token.kind,
        .expected = ParseExpectation::kNone,
        .next_offset = AbsoluteOffset(next),
    };
  }

  [[nodiscard]] ParseError ResourceLimit(Token token) const noexcept {
    const ByteOffset next =
        token.kind == TokenKind::kEndOfInput ? token.span.begin() : token.span.end();
    return ParseError{
        .code = ParseErrorCode::kResourceLimitExceeded,
        .span = AbsoluteSpan(token.span),
        .actual = token.kind,
        .expected = ParseExpectation::kNone,
        .next_offset = AbsoluteOffset(next),
    };
  }

  [[nodiscard]] ParseError ExpressionDepth(Token trigger) const noexcept {
    const std::size_t consumed_end =
        std::max(last_consumed_end_.value(), trigger.span.end().value());
    return ParseError{
        .code = ParseErrorCode::kExpressionDepthExceeded,
        .span = AbsoluteSpan(trigger.span),
        .actual = trigger.kind,
        .expected = ParseExpectation::kNone,
        .next_offset = AbsoluteOffset(ByteOffset{consumed_end}),
    };
  }

  [[nodiscard]] ParseError ParserDepth(Token token) const noexcept {
    if (token.kind == TokenKind::kIllegal) {
      return IllegalToken(token);
    }
    return ParseError{
        .code = ParseErrorCode::kParserDepthExceeded,
        .span = AbsoluteSpan(token.span),
        .actual = token.kind,
        .expected = ParseExpectation::kExpression,
        .next_offset = AbsoluteOffset(token.span.begin()),
    };
  }

  [[nodiscard]] Token Contextualize(Token token) const noexcept {
    if (token.kind == TokenKind::kWindow) {
      Lexer probe = lexer_;
      const Token name = ReadRawSignificant(probe);
      if (!IsContextLookaheadIdentifier(name.kind)) {
        token.kind = TokenKind::kIdentifier;
        return token;
      }
      if (ReadRawSignificant(probe).kind != TokenKind::kAs) {
        token.kind = TokenKind::kIdentifier;
      }
      return token;
    }
    if (token.kind == TokenKind::kOver) {
      Lexer probe = lexer_;
      const Token next = ReadRawSignificant(probe);
      if (!last_read_kind_.has_value() || *last_read_kind_ != TokenKind::kRightParenthesis ||
          (next.kind != TokenKind::kLeftParenthesis && !IsContextLookaheadIdentifier(next.kind))) {
        token.kind = TokenKind::kIdentifier;
      }
      return token;
    }
    if (token.kind == TokenKind::kFilter) {
      Lexer probe = lexer_;
      const Token next = ReadRawSignificant(probe);
      if (!last_read_kind_.has_value() || *last_read_kind_ != TokenKind::kRightParenthesis ||
          next.kind != TokenKind::kLeftParenthesis) {
        token.kind = TokenKind::kIdentifier;
      }
    }
    return token;
  }

  [[nodiscard]] Token ReadSignificant() noexcept {
    Token token = ReadRawSignificant(lexer_);
    token = Contextualize(token);
    last_read_kind_ = token.kind;
    return token;
  }

  [[nodiscard]] Token Peek(std::size_t index = 0) noexcept {
    assert(index < lookahead_.size());
    while (lookahead_count_ <= index) {
      lookahead_[lookahead_count_] = ReadSignificant();
      ++lookahead_count_;
    }
    return lookahead_[index];
  }

  [[nodiscard]] Token Consume() noexcept {
    const Token token = Peek();
    for (std::size_t index = 1; index < lookahead_count_; ++index) {
      lookahead_[index - 1U] = lookahead_[index];
    }
    --lookahead_count_;
    if (token.kind != TokenKind::kEndOfInput) {
      last_consumed_end_ =
          ByteOffset{std::max(last_consumed_end_.value(), token.span.end().value())};
    }
    return token;
  }

  [[nodiscard]] bool ConsumeIf(TokenKind kind) noexcept {
    if (Peek().kind != kind) {
      return false;
    }
    static_cast<void>(Consume());
    return true;
  }

  [[nodiscard]] TokenResult Expect(TokenKind kind, ParseExpectation expectation) {
    const Token token = Peek();
    if (token.kind != kind) {
      return std::unexpected(Unexpected(token, expectation));
    }
    return Consume();
  }

  [[nodiscard]] TokenResult ParseNameToken(NameClass name_class) {
    const Token token = Peek();
    if (!IsNameToken(token.kind, name_class)) {
      return std::unexpected(Unexpected(token, ParseExpectation::kName));
    }
    return Consume();
  }

  [[nodiscard]] NameResult ParseQualifiedName(std::size_t maximum_parts, NameClass name_class) {
    TokenResult first = ParseNameToken(name_class);
    if (!first.has_value()) {
      return std::unexpected(first.error());
    }
    std::vector<SourceSpan> parts;
    parts.push_back(first->span);
    ByteOffset end = first->span.end();
    while (parts.size() < maximum_parts && Peek().kind == TokenKind::kDot) {
      static_cast<void>(Consume());
      TokenResult part = ParseNameToken(name_class);
      if (!part.has_value()) {
        return std::unexpected(part.error());
      }
      end = part->span.end();
      parts.push_back(part->span);
    }
    return QualifiedName{
        .span = MakeSpan(first->span.begin(), end),
        .parts = std::move(parts),
    };
  }

  template <typename Payload>
  [[nodiscard]] ExpressionResult AppendExpression(SourceSpan span, Payload payload,
                                                  std::size_t depth, Token trigger) {
    if (depth > kMaximumExpressionConstructionDepth) {
      return std::unexpected(ExpressionDepth(trigger));
    }
    const ExpressionId id{expressions_.size()};
    expressions_.emplace_back();
    expressions_.back().span = span;
    expressions_.back().payload.template emplace<std::remove_cvref_t<Payload>>(std::move(payload));
    expression_depths_.push_back(depth);
    return id;
  }

  [[nodiscard]] std::size_t Depth(ExpressionId id) const noexcept {
    assert(id.value < expression_depths_.size());
    return expression_depths_[id.value];
  }

  [[nodiscard]] LiteralKind QuotedNumberKind(Token token) const noexcept {
    const std::string_view text = Text(token);
    if (text.size() >= 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      return LiteralKind::kInteger;
    }
    for (const char value : text) {
      if (value == '.' || value == 'e' || value == 'E') {
        return LiteralKind::kReal;
      }
    }
    return LiteralKind::kInteger;
  }

  [[nodiscard]] LiteralKind CurrentTimeKind(Token token) const noexcept {
    if (EqualsAsciiCaseInsensitive(Text(token), "current_date")) {
      return LiteralKind::kCurrentDate;
    }
    if (EqualsAsciiCaseInsensitive(Text(token), "current_time")) {
      return LiteralKind::kCurrentTime;
    }
    return LiteralKind::kCurrentTimestamp;
  }

  [[nodiscard]] std::optional<LiteralKind> LiteralKindFor(Token token) const noexcept {
    switch (token.kind) {
      case TokenKind::kNull:
        return LiteralKind::kNull;
      case TokenKind::kInteger:
        return LiteralKind::kInteger;
      case TokenKind::kFloat:
        return LiteralKind::kReal;
      case TokenKind::kString:
        return LiteralKind::kString;
      case TokenKind::kBlob:
        return LiteralKind::kBlob;
      case TokenKind::kQuotedNumber:
        return QuotedNumberKind(token);
      case TokenKind::kCurrentTimeKeyword:
        return CurrentTimeKind(token);
      default:
        return std::nullopt;
    }
  }

  [[nodiscard]] ExpressionResult ParseLiteral() {
    const Token token = Peek();
    const std::optional<LiteralKind> kind = LiteralKindFor(token);
    if (!kind.has_value()) {
      return std::unexpected(Unexpected(token, ParseExpectation::kExpression));
    }
    static_cast<void>(Consume());
    return AppendExpression(token.span,
                            LiteralExpression{
                                .kind = *kind,
                                .token = token.span,
                            },
                            1U, token);
  }

  [[nodiscard]] ExpressionResult ParseFunctionCall(std::size_t recursion_depth) {
    const Token name_token = Consume();
    const QualifiedName name{
        .span = name_token.span,
        .parts = {name_token.span},
    };
    TokenResult left_parenthesis =
        Expect(TokenKind::kLeftParenthesis, ParseExpectation::kExpression);
    if (!left_parenthesis.has_value()) {
      return std::unexpected(left_parenthesis.error());
    }

    bool distinct = false;
    bool quantified = false;
    if (ConsumeIf(TokenKind::kDistinct)) {
      distinct = true;
      quantified = true;
    } else if (ConsumeIf(TokenKind::kAll)) {
      quantified = true;
    }

    if (Peek().kind == TokenKind::kOrder && Peek(1).kind == TokenKind::kBy) {
      return std::unexpected(Unsupported(Peek()));
    }

    std::vector<ExpressionId> arguments;
    if (Peek().kind == TokenKind::kAsterisk && !quantified) {
      const Token asterisk = Consume();
      ExpressionResult wildcard = AppendExpression(asterisk.span,
                                                   WildcardExpression{
                                                       .asterisk = asterisk.span,
                                                   },
                                                   1U, asterisk);
      if (!wildcard.has_value()) {
        return std::unexpected(wildcard.error());
      }
      arguments.push_back(*wildcard);
    } else if (Peek().kind != TokenKind::kRightParenthesis) {
      while (true) {
        ExpressionResult argument = ParseExpression(0, ExpressionOptions{}, recursion_depth + 1U);
        if (!argument.has_value()) {
          return std::unexpected(argument.error());
        }
        arguments.push_back(*argument);
        if (!ConsumeIf(TokenKind::kComma)) {
          break;
        }
        if (Peek().kind == TokenKind::kRightParenthesis) {
          return std::unexpected(Unexpected(Peek(), ParseExpectation::kExpression));
        }
      }
    }

    if ((Peek().kind == TokenKind::kOrder && Peek(1).kind == TokenKind::kBy) ||
        Peek().kind == TokenKind::kFilter || Peek().kind == TokenKind::kOver) {
      return std::unexpected(Unsupported(Peek()));
    }
    TokenResult right_parenthesis =
        Expect(TokenKind::kRightParenthesis, ParseExpectation::kCommaOrRightParenthesis);
    if (!right_parenthesis.has_value()) {
      return std::unexpected(right_parenthesis.error());
    }

    std::size_t depth = 1U;
    for (const ExpressionId argument : arguments) {
      depth = std::max(depth, Depth(argument) + 1U);
    }
    return AppendExpression(MakeSpan(name_token.span.begin(), right_parenthesis->span.end()),
                            FunctionCallExpression{
                                .name = name,
                                .arguments = std::move(arguments),
                                .distinct = distinct,
                            },
                            depth, name_token);
  }

  [[nodiscard]] ExpressionResult ParseParenthesized(std::size_t recursion_depth) {
    const Token left_parenthesis = Peek();
    if (recursion_depth >= kMaximumParserRecursionDepth) {
      return std::unexpected(ParserDepth(left_parenthesis));
    }
    static_cast<void>(Consume());
    if (Peek().kind == TokenKind::kSelect) {
      return std::unexpected(Unsupported(Peek()));
    }
    ExpressionResult inner = ParseExpression(0, ExpressionOptions{}, recursion_depth + 1U);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    if (Peek().kind == TokenKind::kComma) {
      return std::unexpected(Unsupported(Peek()));
    }
    TokenResult right_parenthesis =
        Expect(TokenKind::kRightParenthesis, ParseExpectation::kRightParenthesis);
    if (!right_parenthesis.has_value()) {
      return std::unexpected(right_parenthesis.error());
    }
    return AppendExpression(MakeSpan(left_parenthesis.span.begin(), right_parenthesis->span.end()),
                            ParenthesizedExpression{.inner = *inner}, Depth(*inner),
                            left_parenthesis);
  }

  [[nodiscard]] ExpressionResult ParseQualifiedOrWildcard(ExpressionOptions options) {
    const Token first = Peek();
    if (options.allow_wildcard && Peek(1).kind == TokenKind::kDot &&
        Peek(2).kind == TokenKind::kAsterisk) {
      static_cast<void>(Consume());
      static_cast<void>(Consume());
      const Token asterisk = Consume();
      const QualifiedName qualifier{
          .span = first.span,
          .parts = {first.span},
      };
      return AppendExpression(MakeSpan(first.span.begin(), asterisk.span.end()),
                              WildcardExpression{
                                  .asterisk = asterisk.span,
                                  .qualifier = qualifier,
                              },
                              2U, first);
    }

    NameResult name = ParseQualifiedName(3U, NameClass::kNm);
    if (!name.has_value()) {
      return std::unexpected(name.error());
    }
    const Token trigger{
        .kind = first.kind,
        .span = first.span,
    };
    const std::size_t depth = name->parts.size();
    const SourceSpan span = name->span;
    return AppendExpression(span,
                            IdentifierExpression{
                                .name = std::move(*name),
                            },
                            depth, trigger);
  }

  [[nodiscard]] ExpressionResult ParsePrefix(ExpressionOptions options,
                                             std::size_t recursion_depth) {
    if (recursion_depth > kMaximumParserRecursionDepth) {
      return std::unexpected(ParserDepth(Peek()));
    }
    const Token token = Peek();
    if (token.kind == TokenKind::kIllegal) {
      return std::unexpected(IllegalToken(token));
    }

    if (token.kind == TokenKind::kCurrentTimeKeyword) {
      return ParseLiteral();
    }
    if (token.kind == TokenKind::kCase || token.kind == TokenKind::kExists ||
        token.kind == TokenKind::kSelect || token.kind == TokenKind::kValues ||
        ((token.kind == TokenKind::kCast || token.kind == TokenKind::kRaise) &&
         Peek(1).kind == TokenKind::kLeftParenthesis)) {
      return std::unexpected(Unsupported(token));
    }
    if (token.kind == TokenKind::kCast || token.kind == TokenKind::kRaise) {
      static_cast<void>(Consume());
      return std::unexpected(Unexpected(Peek(), ParseExpectation::kExpression));
    }
    if (IsNameToken(token.kind, NameClass::kIdj) && Peek(1).kind == TokenKind::kLeftParenthesis) {
      return ParseFunctionCall(recursion_depth);
    }
    if (IsNameToken(token.kind, NameClass::kNm) && Peek(1).kind == TokenKind::kDot) {
      return ParseQualifiedOrWildcard(options);
    }

    if (LiteralKindFor(token).has_value()) {
      return ParseLiteral();
    }
    if (token.kind == TokenKind::kVariable) {
      static_cast<void>(Consume());
      return AppendExpression(token.span, VariableExpression{.token = token.span}, 1U, token);
    }
    if (token.kind == TokenKind::kAsterisk && options.allow_wildcard) {
      static_cast<void>(Consume());
      return AppendExpression(token.span,
                              WildcardExpression{
                                  .asterisk = token.span,
                              },
                              1U, token);
    }
    if (token.kind == TokenKind::kLeftParenthesis) {
      return ParseParenthesized(recursion_depth);
    }
    if (token.kind == TokenKind::kPlus || token.kind == TokenKind::kMinus ||
        token.kind == TokenKind::kBitwiseNot || token.kind == TokenKind::kNot) {
      if (recursion_depth >= kMaximumParserRecursionDepth) {
        return std::unexpected(ParserDepth(token));
      }
      static_cast<void>(Consume());
      const int binding = token.kind == TokenKind::kNot ? kNotPrefixBinding : kUnaryBinding;
      ExpressionResult operand =
          ParseExpression(binding, ExpressionOptions{}, recursion_depth + 1U);
      if (!operand.has_value()) {
        return std::unexpected(operand.error());
      }
      UnaryOperator operation = UnaryOperator::kNot;
      if (token.kind == TokenKind::kPlus) {
        operation = UnaryOperator::kPositive;
      } else if (token.kind == TokenKind::kMinus) {
        operation = UnaryOperator::kNegative;
      } else if (token.kind == TokenKind::kBitwiseNot) {
        operation = UnaryOperator::kBitwiseNot;
      }
      const SourceSpan span = MakeSpan(token.span.begin(), expressions_[operand->value].span.end());
      return AppendExpression(span,
                              UnaryExpression{
                                  .op = operation,
                                  .operator_span = token.span,
                                  .operand = *operand,
                              },
                              Depth(*operand) + 1U, token);
    }
    if (IsNameToken(token.kind, NameClass::kIdj)) {
      static_cast<void>(Consume());
      const QualifiedName name{
          .span = token.span,
          .parts = {token.span},
      };
      return AppendExpression(token.span,
                              IdentifierExpression{
                                  .name = name,
                              },
                              1U, token);
    }
    return std::unexpected(Unexpected(token, ParseExpectation::kExpression));
  }

  [[nodiscard]] std::optional<InfixOperator> FindInfix() noexcept {
    const Token token = Peek();
    switch (token.kind) {
      case TokenKind::kOr:
        return InfixOperator{
            .op = BinaryOperator::kOr,
            .binding = kOrBinding,
        };
      case TokenKind::kAnd:
        return InfixOperator{
            .op = BinaryOperator::kAnd,
            .binding = kAndBinding,
        };
      case TokenKind::kEquality:
        return InfixOperator{
            .op = BinaryOperator::kEqual,
            .binding = kComparisonBinding,
        };
      case TokenKind::kNotEqual:
        return InfixOperator{
            .op = BinaryOperator::kNotEqual,
            .binding = kComparisonBinding,
        };
      case TokenKind::kIs:
        return InfixOperator{
            .op = Peek(1).kind == TokenKind::kNot ? BinaryOperator::kIsNot : BinaryOperator::kIs,
            .binding = kComparisonBinding,
            .token_count = Peek(1).kind == TokenKind::kNot ? 2U : 1U,
        };
      case TokenKind::kLessThan:
        return InfixOperator{
            .op = BinaryOperator::kLess,
            .binding = kRelationalBinding,
        };
      case TokenKind::kLessThanOrEqual:
        return InfixOperator{
            .op = BinaryOperator::kLessOrEqual,
            .binding = kRelationalBinding,
        };
      case TokenKind::kGreaterThan:
        return InfixOperator{
            .op = BinaryOperator::kGreater,
            .binding = kRelationalBinding,
        };
      case TokenKind::kGreaterThanOrEqual:
        return InfixOperator{
            .op = BinaryOperator::kGreaterOrEqual,
            .binding = kRelationalBinding,
        };
      case TokenKind::kBitwiseAnd:
        return InfixOperator{
            .op = BinaryOperator::kBitwiseAnd,
            .binding = kBitwiseBinding,
        };
      case TokenKind::kBitwiseOr:
        return InfixOperator{
            .op = BinaryOperator::kBitwiseOr,
            .binding = kBitwiseBinding,
        };
      case TokenKind::kLeftShift:
        return InfixOperator{
            .op = BinaryOperator::kLeftShift,
            .binding = kBitwiseBinding,
        };
      case TokenKind::kRightShift:
        return InfixOperator{
            .op = BinaryOperator::kRightShift,
            .binding = kBitwiseBinding,
        };
      case TokenKind::kPlus:
        return InfixOperator{
            .op = BinaryOperator::kAdd,
            .binding = kAdditiveBinding,
        };
      case TokenKind::kMinus:
        return InfixOperator{
            .op = BinaryOperator::kSubtract,
            .binding = kAdditiveBinding,
        };
      case TokenKind::kAsterisk:
        return InfixOperator{
            .op = BinaryOperator::kMultiply,
            .binding = kMultiplicativeBinding,
        };
      case TokenKind::kSlash:
        return InfixOperator{
            .op = BinaryOperator::kDivide,
            .binding = kMultiplicativeBinding,
        };
      case TokenKind::kRemainder:
        return InfixOperator{
            .op = BinaryOperator::kRemainder,
            .binding = kMultiplicativeBinding,
        };
      case TokenKind::kConcatenate:
        return InfixOperator{
            .op = BinaryOperator::kConcatenate,
            .binding = kConcatenateBinding,
        };
      case TokenKind::kMatch:
        return InfixOperator{
            .op = BinaryOperator::kMatch,
            .binding = kComparisonBinding,
        };
      case TokenKind::kLikeKeyword:
        if (EqualsAsciiCaseInsensitive(Text(token), "glob")) {
          return InfixOperator{
              .op = BinaryOperator::kGlob,
              .binding = kComparisonBinding,
          };
        }
        if (EqualsAsciiCaseInsensitive(Text(token), "regexp")) {
          return InfixOperator{
              .op = BinaryOperator::kRegexp,
              .binding = kComparisonBinding,
          };
        }
        return InfixOperator{
            .op = BinaryOperator::kLike,
            .binding = kComparisonBinding,
        };
      case TokenKind::kNot:
        if (Peek(1).kind == TokenKind::kMatch) {
          return InfixOperator{
              .op = BinaryOperator::kNotMatch,
              .binding = kComparisonBinding,
              .token_count = 2U,
              .adds_negation_level = true,
          };
        }
        if (Peek(1).kind == TokenKind::kLikeKeyword) {
          BinaryOperator operation = BinaryOperator::kNotLike;
          if (EqualsAsciiCaseInsensitive(Text(Peek(1)), "glob")) {
            operation = BinaryOperator::kNotGlob;
          } else if (EqualsAsciiCaseInsensitive(Text(Peek(1)), "regexp")) {
            operation = BinaryOperator::kNotRegexp;
          }
          return InfixOperator{
              .op = operation,
              .binding = kComparisonBinding,
              .token_count = 2U,
              .adds_negation_level = true,
          };
        }
        return std::nullopt;
      default:
        return std::nullopt;
    }
  }

  [[nodiscard]] bool IsUnsupportedInfix(TokenKind kind) const noexcept {
    switch (kind) {
      case TokenKind::kBetween:
      case TokenKind::kIn:
      case TokenKind::kIsNull:
      case TokenKind::kNotNull:
      case TokenKind::kPointer:
      case TokenKind::kEscape:
      case TokenKind::kOver:
      case TokenKind::kFilter:
      case TokenKind::kWindow:
        return true;
      default:
        return false;
    }
  }

  [[nodiscard]] ExpressionResult ParseExpression(int minimum_binding, ExpressionOptions options,
                                                 std::size_t recursion_depth) {
    if (recursion_depth > kMaximumParserRecursionDepth) {
      return std::unexpected(ParserDepth(Peek()));
    }
    ExpressionResult left = ParsePrefix(options, recursion_depth);
    if (!left.has_value()) {
      return std::unexpected(left.error());
    }

    while (true) {
      const Token token = Peek();
      if (token.kind == TokenKind::kIllegal) {
        return std::unexpected(IllegalToken(token));
      }
      if (token.kind == TokenKind::kCollate) {
        if (kCollateBinding < minimum_binding) {
          break;
        }
        const Token keyword = Consume();
        TokenResult collation = ParseNameToken(NameClass::kIds);
        if (!collation.has_value()) {
          return std::unexpected(collation.error());
        }
        const SourceSpan span =
            MakeSpan(expressions_[left->value].span.begin(), collation->span.end());
        ExpressionResult collated = AppendExpression(span,
                                                     CollateExpression{
                                                         .operand = *left,
                                                         .keyword = keyword.span,
                                                         .collation = collation->span,
                                                     },
                                                     Depth(*left) + 1U, keyword);
        if (!collated.has_value()) {
          return std::unexpected(collated.error());
        }
        left = *collated;
        continue;
      }
      if (token.kind == TokenKind::kIs &&
          (Peek(1).kind == TokenKind::kDistinct ||
           (Peek(1).kind == TokenKind::kNot && Peek(2).kind == TokenKind::kDistinct))) {
        const Token distinct = Peek(1).kind == TokenKind::kDistinct ? Peek(1) : Peek(2);
        return std::unexpected(Unsupported(distinct));
      }
      if (token.kind == TokenKind::kNot &&
          (Peek(1).kind == TokenKind::kBetween || Peek(1).kind == TokenKind::kIn ||
           Peek(1).kind == TokenKind::kNull)) {
        return std::unexpected(Unsupported(token));
      }
      if (IsUnsupportedInfix(token.kind)) {
        return std::unexpected(Unsupported(token));
      }

      const std::optional<InfixOperator> operation = FindInfix();
      if (!operation.has_value() || operation->binding < minimum_binding) {
        break;
      }
      if (recursion_depth >= kMaximumParserRecursionDepth) {
        return std::unexpected(ParserDepth(token));
      }

      const Token first_operator = Consume();
      Token last_operator = first_operator;
      for (std::size_t index = 1; index < operation->token_count; ++index) {
        last_operator = Consume();
      }
      ExpressionResult right =
          ParseExpression(operation->binding + 1, ExpressionOptions{}, recursion_depth + 1U);
      if (!right.has_value()) {
        return std::unexpected(right.error());
      }
      const SourceSpan expression_span =
          MakeSpan(expressions_[left->value].span.begin(), expressions_[right->value].span.end());
      const SourceSpan operator_span =
          MakeSpan(first_operator.span.begin(), last_operator.span.end());
      const std::size_t added_depth = operation->adds_negation_level ? 2U : 1U;
      ExpressionResult combined =
          AppendExpression(expression_span,
                           BinaryExpression{
                               .op = operation->op,
                               .operator_span = operator_span,
                               .left = *left,
                               .right = *right,
                           },
                           std::max(Depth(*left), Depth(*right)) + added_depth, first_operator);
      if (!combined.has_value()) {
        return std::unexpected(combined.error());
      }
      left = *combined;
    }
    return *left;
  }

  [[nodiscard]] ExpressionResult ParseGeneralExpression(bool allow_wildcard = false) {
    return ParseExpression(0,
                           ExpressionOptions{
                               .allow_wildcard = allow_wildcard,
                           },
                           1U);
  }

  [[nodiscard]] std::expected<std::optional<SourceSpan>, ParseError> ParseOptionalAlias() {
    if (ConsumeIf(TokenKind::kAs)) {
      TokenResult alias = ParseNameToken(NameClass::kNm);
      if (!alias.has_value()) {
        return std::unexpected(alias.error());
      }
      return std::optional<SourceSpan>{alias->span};
    }
    if (IsNameToken(Peek().kind, NameClass::kIds)) {
      return std::optional<SourceSpan>{Consume().span};
    }
    return std::optional<SourceSpan>{};
  }

  [[nodiscard]] std::expected<ResultColumn, ParseError> ParseResultColumn() {
    const bool wildcard = Peek().kind == TokenKind::kAsterisk ||
                          (IsNameToken(Peek().kind, NameClass::kNm) &&
                           Peek(1).kind == TokenKind::kDot && Peek(2).kind == TokenKind::kAsterisk);
    if (wildcard) {
      ExpressionResult expression = ParsePrefix(ExpressionOptions{.allow_wildcard = true}, 1U);
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      const SourceSpan span = expressions_[expression->value].span;
      return ResultColumn{
          .span = span,
          .expression = *expression,
          .alias = std::nullopt,
      };
    }

    ExpressionResult expression = ParseGeneralExpression();
    if (!expression.has_value()) {
      return std::unexpected(expression.error());
    }
    auto alias = ParseOptionalAlias();
    if (!alias.has_value()) {
      return std::unexpected(alias.error());
    }
    const SourceSpan expression_span = expressions_[expression->value].span;
    const ByteOffset end = alias->has_value() ? (*alias)->end() : expression_span.end();
    return ResultColumn{
        .span = MakeSpan(expression_span.begin(), end),
        .expression = *expression,
        .alias = *alias,
    };
  }

  [[nodiscard]] std::expected<TableSource, ParseError> ParseTableSource() {
    NameResult name = ParseQualifiedName(2U, NameClass::kNm);
    if (!name.has_value()) {
      return std::unexpected(name.error());
    }
    if (Peek().kind == TokenKind::kLeftParenthesis) {
      return std::unexpected(Unsupported(Peek()));
    }
    const SourceSpan name_span = name->span;
    auto alias = ParseOptionalAlias();
    if (!alias.has_value()) {
      return std::unexpected(alias.error());
    }
    const ByteOffset end = alias->has_value() ? (*alias)->end() : name_span.end();
    return TableSource{
        .span = MakeSpan(name_span.begin(), end),
        .name = std::move(*name),
        .alias = *alias,
    };
  }

  [[nodiscard]] std::expected<LimitClause, ParseError> ParseLimitClause() {
    const Token limit_keyword = Consume();
    ExpressionResult first = ParseGeneralExpression();
    if (!first.has_value()) {
      return std::unexpected(first.error());
    }

    LimitSyntax syntax = LimitSyntax::kLimitOnly;
    ExpressionId limit = *first;
    std::optional<ExpressionId> offset;
    ByteOffset end = expressions_[first->value].span.end();
    if (ConsumeIf(TokenKind::kOffset)) {
      ExpressionResult parsed_offset = ParseGeneralExpression();
      if (!parsed_offset.has_value()) {
        return std::unexpected(parsed_offset.error());
      }
      syntax = LimitSyntax::kOffsetKeyword;
      offset = *parsed_offset;
      end = expressions_[parsed_offset->value].span.end();
    } else if (ConsumeIf(TokenKind::kComma)) {
      ExpressionResult parsed_limit = ParseGeneralExpression();
      if (!parsed_limit.has_value()) {
        return std::unexpected(parsed_limit.error());
      }
      syntax = LimitSyntax::kComma;
      offset = *first;
      limit = *parsed_limit;
      end = expressions_[parsed_limit->value].span.end();
    }
    return LimitClause{
        .span = MakeSpan(limit_keyword.span.begin(), end),
        .limit = limit,
        .offset = offset,
        .syntax = syntax,
    };
  }

  [[nodiscard]] std::expected<NullOrder, ParseError> ParseNullOrder() {
    if (!ConsumeIf(TokenKind::kNulls)) {
      return NullOrder::kDefault;
    }
    if (ConsumeIf(TokenKind::kFirst)) {
      return NullOrder::kFirst;
    }
    if (ConsumeIf(TokenKind::kLast)) {
      return NullOrder::kLast;
    }
    return std::unexpected(Unexpected(Peek(), ParseExpectation::kExpression));
  }

  [[nodiscard]] std::expected<OrderingTerm, ParseError> ParseOrderingTerm() {
    const ByteOffset begin = Peek().span.begin();
    ExpressionResult expression = ParseGeneralExpression();
    if (!expression.has_value()) {
      return std::unexpected(expression.error());
    }
    const SortOrder order = ParseSortOrder();
    auto null_order = ParseNullOrder();
    if (!null_order.has_value()) {
      return std::unexpected(null_order.error());
    }
    return OrderingTerm{
        .span = MakeSpan(begin, last_consumed_end_),
        .expression = *expression,
        .order = order,
        .null_order = *null_order,
    };
  }

  [[nodiscard]] std::expected<std::vector<OrderingTerm>, ParseError> ParseOrderByClause() {
    static_cast<void>(Consume());
    TokenResult by = Expect(TokenKind::kBy, ParseExpectation::kExpression);
    if (!by.has_value()) {
      return std::unexpected(by.error());
    }
    if (options_.maximum_columns == 0U) {
      return std::unexpected(ResourceLimit(Peek()));
    }
    std::vector<OrderingTerm> terms;
    auto first = ParseOrderingTerm();
    if (!first.has_value()) {
      return std::unexpected(first.error());
    }
    terms.push_back(*first);
    while (ConsumeIf(TokenKind::kComma)) {
      if (terms.size() >= options_.maximum_columns) {
        return std::unexpected(ResourceLimit(Peek()));
      }
      auto term = ParseOrderingTerm();
      if (!term.has_value()) {
        return std::unexpected(term.error());
      }
      terms.push_back(*term);
    }
    return terms;
  }

  [[nodiscard]] QueryCoreResult ParseSimpleSelectCore() {
    const Token select_keyword = Consume();
    SelectQuantifier quantifier = SelectQuantifier::kDefault;
    if (ConsumeIf(TokenKind::kDistinct)) {
      quantifier = SelectQuantifier::kDistinct;
    } else if (ConsumeIf(TokenKind::kAll)) {
      quantifier = SelectQuantifier::kAll;
    }

    std::vector<ResultColumn> columns;
    if (options_.maximum_columns == 0) {
      return std::unexpected(ResourceLimit(Peek()));
    }
    auto first_column = ParseResultColumn();
    if (!first_column.has_value()) {
      return std::unexpected(first_column.error());
    }
    columns.push_back(*first_column);
    while (ConsumeIf(TokenKind::kComma)) {
      if (columns.size() >= options_.maximum_columns) {
        return std::unexpected(ResourceLimit(Peek()));
      }
      auto column = ParseResultColumn();
      if (!column.has_value()) {
        return std::unexpected(column.error());
      }
      columns.push_back(*column);
    }

    std::optional<TableSource> from;
    if (ConsumeIf(TokenKind::kFrom)) {
      if (Peek().kind == TokenKind::kLeftParenthesis) {
        return std::unexpected(Unsupported(Peek()));
      }
      auto source = ParseTableSource();
      if (!source.has_value()) {
        return std::unexpected(source.error());
      }
      from = std::move(*source);
      if (Peek().kind == TokenKind::kComma || Peek().kind == TokenKind::kJoin ||
          Peek().kind == TokenKind::kJoinKeyword || Peek().kind == TokenKind::kOn ||
          Peek().kind == TokenKind::kUsing || Peek().kind == TokenKind::kIndexed ||
          (Peek().kind == TokenKind::kNot && Peek(1).kind == TokenKind::kIndexed)) {
        return std::unexpected(Unsupported(Peek()));
      }
    }

    std::optional<ExpressionId> where;
    if (ConsumeIf(TokenKind::kWhere)) {
      ExpressionResult expression = ParseGeneralExpression();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      where = *expression;
    }

    if (Peek().kind == TokenKind::kGroup || Peek().kind == TokenKind::kHaving ||
        Peek().kind == TokenKind::kWindow) {
      return std::unexpected(Unsupported(Peek()));
    }

    const ByteOffset core_end = last_consumed_end_;
    std::vector<OrderingTerm> order_by;
    if (Peek().kind == TokenKind::kOrder) {
      auto parsed_order_by = ParseOrderByClause();
      if (!parsed_order_by.has_value()) {
        return std::unexpected(parsed_order_by.error());
      }
      order_by = std::move(*parsed_order_by);
    }

    std::optional<LimitClause> limit;
    if (Peek().kind == TokenKind::kLimit) {
      auto parsed_limit = ParseLimitClause();
      if (!parsed_limit.has_value()) {
        return std::unexpected(parsed_limit.error());
      }
      limit = *parsed_limit;
    }

    return ParsedQueryCore{
        .core =
            SelectCore{
                .span = MakeSpan(select_keyword.span.begin(), core_end),
                .quantifier = quantifier,
                .result_columns = std::move(columns),
                .from = std::move(from),
                .where = where,
            },
        .order_by = std::move(order_by),
        .limit = limit,
    };
  }

  [[nodiscard]] std::expected<std::vector<ExpressionId>, ParseError> ParseValuesRow() {
    TokenResult left = Expect(TokenKind::kLeftParenthesis, ParseExpectation::kExpression);
    if (!left.has_value()) {
      return std::unexpected(left.error());
    }
    if (options_.maximum_columns == 0U) {
      return std::unexpected(ResourceLimit(Peek()));
    }
    ExpressionResult first = ParseGeneralExpression();
    if (!first.has_value()) {
      return std::unexpected(first.error());
    }
    std::vector<ExpressionId> row{*first};
    while (ConsumeIf(TokenKind::kComma)) {
      if (row.size() >= options_.maximum_columns) {
        return std::unexpected(ResourceLimit(Peek()));
      }
      ExpressionResult expression = ParseGeneralExpression();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      row.push_back(*expression);
    }
    TokenResult right = Expect(TokenKind::kRightParenthesis, ParseExpectation::kRightParenthesis);
    if (!right.has_value()) {
      return std::unexpected(right.error());
    }
    return row;
  }

  [[nodiscard]] QueryCoreResult ParseValuesCore() {
    const Token values_keyword = Consume();
    auto first = ParseValuesRow();
    if (!first.has_value()) {
      return std::unexpected(first.error());
    }
    std::vector<std::vector<ExpressionId>> rows;
    rows.push_back(std::move(*first));
    while (ConsumeIf(TokenKind::kComma)) {
      auto row = ParseValuesRow();
      if (!row.has_value()) {
        return std::unexpected(row.error());
      }
      rows.push_back(std::move(*row));
    }
    return ParsedQueryCore{
        .core =
            ValuesCore{
                .span = MakeSpan(values_keyword.span.begin(), last_consumed_end_),
                .rows = std::move(rows),
            },
    };
  }

  [[nodiscard]] QueryCoreResult ParseQueryCore() {
    if (Peek().kind == TokenKind::kSelect) {
      return ParseSimpleSelectCore();
    }
    if (Peek().kind == TokenKind::kValues) {
      return ParseValuesCore();
    }
    return std::unexpected(Unexpected(Peek(), ParseExpectation::kStatement));
  }

  [[nodiscard]] static bool IsCompoundOperator(TokenKind kind) noexcept {
    return kind == TokenKind::kUnion || kind == TokenKind::kIntersect || kind == TokenKind::kExcept;
  }

  [[nodiscard]] CompoundOperatorResult ParseCompoundOperator() {
    const Token token = Consume();
    CompoundOperator operation = CompoundOperator::kUnion;
    ByteOffset end = token.span.end();
    if (token.kind == TokenKind::kUnion) {
      if (ConsumeIf(TokenKind::kAll)) {
        operation = CompoundOperator::kUnionAll;
        end = last_consumed_end_;
      }
    } else if (token.kind == TokenKind::kIntersect) {
      operation = CompoundOperator::kIntersect;
    } else if (token.kind == TokenKind::kExcept) {
      operation = CompoundOperator::kExcept;
    } else {
      return std::unexpected(Unexpected(token, ParseExpectation::kStatement));
    }
    return ParsedCompoundOperator{
        .operation = operation,
        .span = MakeSpan(token.span.begin(), end),
    };
  }

  [[nodiscard]] static SourceSpan QueryCoreSpan(const QueryCore& core) {
    return std::visit([](const auto& value) { return value.span; }, core);
  }

  [[nodiscard]] StatementResult ParseSelect() {
    if (options_.maximum_compound_terms == 0U) {
      return std::unexpected(ResourceLimit(Peek()));
    }
    QueryCoreResult first = ParseQueryCore();
    if (!first.has_value()) {
      return std::unexpected(first.error());
    }
    const ByteOffset begin = QueryCoreSpan(first->core).begin();
    std::vector<OrderingTerm> order_by = std::move(first->order_by);
    std::optional<LimitClause> limit = first->limit;
    QueryCore first_core = std::move(first->core);
    std::vector<CompoundTerm> compounds;

    while (IsCompoundOperator(Peek().kind)) {
      if (!order_by.empty() || limit.has_value()) {
        return std::unexpected(Unexpected(Peek(), ParseExpectation::kEndOfStatement));
      }
      if (compounds.size() >= options_.maximum_compound_terms - 1U) {
        return std::unexpected(ResourceLimit(Peek()));
      }
      auto operation = ParseCompoundOperator();
      if (!operation.has_value()) {
        return std::unexpected(operation.error());
      }
      QueryCoreResult core = ParseQueryCore();
      if (!core.has_value()) {
        return std::unexpected(core.error());
      }
      const SourceSpan core_span = QueryCoreSpan(core->core);
      compounds.push_back(CompoundTerm{
          .span = MakeSpan(operation->span.begin(), core_span.end()),
          .operation = operation->operation,
          .core = std::move(core->core),
      });
      order_by = std::move(core->order_by);
      limit = core->limit;
    }

    return Statement{SelectStatement{
        .span = MakeSpan(begin, last_consumed_end_),
        .first = std::move(first_core),
        .compounds = std::move(compounds),
        .order_by = std::move(order_by),
        .limit = limit,
    }};
  }

  [[nodiscard]] StatementResult ParseInsert() {
    const Token insert_keyword = Consume();
    if (Peek().kind == TokenKind::kOr) {
      return std::unexpected(Unsupported(Peek()));
    }
    TokenResult into = Expect(TokenKind::kInto, ParseExpectation::kStatement);
    if (!into.has_value()) {
      return std::unexpected(into.error());
    }
    NameResult table = ParseQualifiedName(2U, NameClass::kNm);
    if (!table.has_value()) {
      return std::unexpected(table.error());
    }
    if (Peek().kind == TokenKind::kAs) {
      return std::unexpected(Unsupported(Peek()));
    }

    std::vector<SourceSpan> columns;
    if (ConsumeIf(TokenKind::kLeftParenthesis)) {
      while (true) {
        if (columns.size() >= options_.maximum_columns) {
          return std::unexpected(ResourceLimit(Peek()));
        }
        TokenResult column = ParseNameToken(NameClass::kNm);
        if (!column.has_value()) {
          return std::unexpected(column.error());
        }
        columns.push_back(column->span);
        if (!ConsumeIf(TokenKind::kComma)) {
          break;
        }
      }
      TokenResult right =
          Expect(TokenKind::kRightParenthesis, ParseExpectation::kCommaOrRightParenthesis);
      if (!right.has_value()) {
        return std::unexpected(right.error());
      }
    }

    InsertSource source;
    if (Peek().kind == TokenKind::kDefault) {
      const Token default_keyword = Consume();
      TokenResult values_keyword = Expect(TokenKind::kValues, ParseExpectation::kStatement);
      if (!values_keyword.has_value()) {
        return std::unexpected(values_keyword.error());
      }
      source = InsertDefaultValuesSource{
          .span = MakeSpan(default_keyword.span.begin(), values_keyword->span.end()),
      };
    } else {
      if (Peek().kind == TokenKind::kSelect) {
        return std::unexpected(Unsupported(Peek()));
      }
      TokenResult values_keyword = Expect(TokenKind::kValues, ParseExpectation::kStatement);
      if (!values_keyword.has_value()) {
        return std::unexpected(values_keyword.error());
      }
      TokenResult left = Expect(TokenKind::kLeftParenthesis, ParseExpectation::kExpression);
      if (!left.has_value()) {
        return std::unexpected(left.error());
      }
      std::vector<ExpressionId> values;
      while (true) {
        if (values.size() >= options_.maximum_columns) {
          return std::unexpected(ResourceLimit(Peek()));
        }
        ExpressionResult value = ParseGeneralExpression();
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        values.push_back(*value);
        if (!ConsumeIf(TokenKind::kComma)) {
          break;
        }
      }
      TokenResult right =
          Expect(TokenKind::kRightParenthesis, ParseExpectation::kCommaOrRightParenthesis);
      if (!right.has_value()) {
        return std::unexpected(right.error());
      }
      if (Peek().kind == TokenKind::kComma || Peek().kind == TokenKind::kUnion ||
          Peek().kind == TokenKind::kIntersect || Peek().kind == TokenKind::kExcept) {
        return std::unexpected(Unsupported(Peek()));
      }
      source = InsertValuesSource{
          .span = MakeSpan(values_keyword->span.begin(), right->span.end()),
          .values = std::move(values),
      };
    }

    if (Peek().kind == TokenKind::kReturning || Peek().kind == TokenKind::kOn) {
      return std::unexpected(Unsupported(Peek()));
    }
    return Statement{InsertStatement{
        .span = MakeSpan(insert_keyword.span.begin(), last_consumed_end_),
        .table = std::move(*table),
        .columns = std::move(columns),
        .source = std::move(source),
    }};
  }

  [[nodiscard]] StatementResult ParseUpdate() {
    const Token update_keyword = Consume();
    if (Peek().kind == TokenKind::kOr) {
      return std::unexpected(Unsupported(Peek()));
    }
    NameResult table = ParseQualifiedName(2U, NameClass::kNm);
    if (!table.has_value()) {
      return std::unexpected(table.error());
    }
    if (Peek().kind == TokenKind::kAs || Peek().kind == TokenKind::kIndexed ||
        (Peek().kind == TokenKind::kNot && Peek(1).kind == TokenKind::kIndexed)) {
      return std::unexpected(Unsupported(Peek()));
    }
    TokenResult set_keyword = Expect(TokenKind::kSet, ParseExpectation::kStatement);
    if (!set_keyword.has_value()) {
      return std::unexpected(set_keyword.error());
    }

    std::vector<UpdateAssignment> assignments;
    while (true) {
      if (assignments.size() >= options_.maximum_columns) {
        return std::unexpected(ResourceLimit(Peek()));
      }
      if (Peek().kind == TokenKind::kLeftParenthesis) {
        return std::unexpected(Unsupported(Peek()));
      }
      TokenResult column = ParseNameToken(NameClass::kNm);
      if (!column.has_value()) {
        return std::unexpected(column.error());
      }
      TokenResult equal = Expect(TokenKind::kEquality, ParseExpectation::kExpression);
      if (!equal.has_value()) {
        return std::unexpected(equal.error());
      }
      ExpressionResult expression = ParseGeneralExpression();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      assignments.push_back(UpdateAssignment{
          .span = MakeSpan(column->span.begin(), expressions_[expression->value].span.end()),
          .column = column->span,
          .expression = *expression,
      });
      if (!ConsumeIf(TokenKind::kComma)) {
        break;
      }
    }

    std::optional<ExpressionId> where;
    if (ConsumeIf(TokenKind::kWhere)) {
      ExpressionResult expression = ParseGeneralExpression();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      where = *expression;
    }
    if (Peek().kind == TokenKind::kFrom || Peek().kind == TokenKind::kReturning ||
        Peek().kind == TokenKind::kOrder || Peek().kind == TokenKind::kLimit) {
      return std::unexpected(Unsupported(Peek()));
    }
    return Statement{UpdateStatement{
        .span = MakeSpan(update_keyword.span.begin(), last_consumed_end_),
        .table = std::move(*table),
        .assignments = std::move(assignments),
        .where = where,
    }};
  }

  [[nodiscard]] StatementResult ParseDelete() {
    const Token delete_keyword = Consume();
    TokenResult from = Expect(TokenKind::kFrom, ParseExpectation::kStatement);
    if (!from.has_value()) {
      return std::unexpected(from.error());
    }
    NameResult table = ParseQualifiedName(2U, NameClass::kNm);
    if (!table.has_value()) {
      return std::unexpected(table.error());
    }
    if (Peek().kind == TokenKind::kAs || Peek().kind == TokenKind::kIndexed ||
        (Peek().kind == TokenKind::kNot && Peek(1).kind == TokenKind::kIndexed)) {
      return std::unexpected(Unsupported(Peek()));
    }
    std::optional<ExpressionId> where;
    if (ConsumeIf(TokenKind::kWhere)) {
      ExpressionResult expression = ParseGeneralExpression();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      where = *expression;
    }
    if (Peek().kind == TokenKind::kReturning || Peek().kind == TokenKind::kOrder ||
        Peek().kind == TokenKind::kLimit) {
      return std::unexpected(Unsupported(Peek()));
    }
    return Statement{DeleteStatement{
        .span = MakeSpan(delete_keyword.span.begin(), last_consumed_end_),
        .table = std::move(*table),
        .where = where,
    }};
  }

  [[nodiscard]] StatementResult ParseBegin() {
    const Token begin_keyword = Consume();
    BeginTransactionMode mode = BeginTransactionMode::kDeferred;
    if (ConsumeIf(TokenKind::kDeferred)) {
      mode = BeginTransactionMode::kDeferred;
    } else if (ConsumeIf(TokenKind::kImmediate)) {
      mode = BeginTransactionMode::kImmediate;
    } else if (Peek().kind == TokenKind::kExclusive) {
      return std::unexpected(Unsupported(Peek()));
    }
    const bool transaction_keyword = ConsumeIf(TokenKind::kTransaction);
    if (transaction_keyword && IsNameToken(Peek().kind, NameClass::kNm)) {
      return std::unexpected(Unsupported(Peek()));
    }
    return Statement{BeginTransactionStatement{
        .span = MakeSpan(begin_keyword.span.begin(), last_consumed_end_),
        .mode = mode,
        .transaction_keyword = transaction_keyword,
    }};
  }

  [[nodiscard]] StatementResult ParseCommit() {
    const Token keyword = Consume();
    const bool transaction_keyword = ConsumeIf(TokenKind::kTransaction);
    if (transaction_keyword && IsNameToken(Peek().kind, NameClass::kNm)) {
      return std::unexpected(Unsupported(Peek()));
    }
    return Statement{CommitTransactionStatement{
        .span = MakeSpan(keyword.span.begin(), last_consumed_end_),
        .syntax = keyword.kind == TokenKind::kEnd ? CommitTransactionSyntax::kEnd
                                                  : CommitTransactionSyntax::kCommit,
        .transaction_keyword = transaction_keyword,
    }};
  }

  [[nodiscard]] StatementResult ParseRollback() {
    const Token rollback_keyword = Consume();
    const bool transaction_keyword = ConsumeIf(TokenKind::kTransaction);
    if (transaction_keyword && IsNameToken(Peek().kind, NameClass::kNm)) {
      return std::unexpected(Unsupported(Peek()));
    }
    if (!ConsumeIf(TokenKind::kTo)) {
      return Statement{RollbackTransactionStatement{
          .span = MakeSpan(rollback_keyword.span.begin(), last_consumed_end_),
          .transaction_keyword = transaction_keyword,
      }};
    }
    const bool savepoint_keyword = ConsumeIf(TokenKind::kSavepoint);
    TokenResult name = ParseNameToken(NameClass::kNm);
    if (!name.has_value()) {
      return std::unexpected(name.error());
    }
    if (Peek().kind == TokenKind::kDot) {
      return std::unexpected(Unsupported(Peek()));
    }
    return Statement{RollbackToSavepointStatement{
        .span = MakeSpan(rollback_keyword.span.begin(), last_consumed_end_),
        .name = name->span,
        .transaction_keyword = transaction_keyword,
        .savepoint_keyword = savepoint_keyword,
    }};
  }

  [[nodiscard]] StatementResult ParseSavepoint() {
    const Token savepoint_keyword = Consume();
    TokenResult name = ParseNameToken(NameClass::kNm);
    if (!name.has_value()) {
      return std::unexpected(name.error());
    }
    if (Peek().kind == TokenKind::kDot) {
      return std::unexpected(Unsupported(Peek()));
    }
    return Statement{SavepointStatement{
        .span = MakeSpan(savepoint_keyword.span.begin(), last_consumed_end_),
        .name = name->span,
    }};
  }

  [[nodiscard]] StatementResult ParseRelease() {
    const Token release_keyword = Consume();
    const bool savepoint_keyword = ConsumeIf(TokenKind::kSavepoint);
    TokenResult name = ParseNameToken(NameClass::kNm);
    if (!name.has_value()) {
      return std::unexpected(name.error());
    }
    if (Peek().kind == TokenKind::kDot) {
      return std::unexpected(Unsupported(Peek()));
    }
    return Statement{ReleaseSavepointStatement{
        .span = MakeSpan(release_keyword.span.begin(), last_consumed_end_),
        .name = name->span,
        .savepoint_keyword = savepoint_keyword,
    }};
  }

  [[nodiscard]] std::expected<bool, ParseError> ParseIfNotExists() {
    if (!ConsumeIf(TokenKind::kIf)) {
      return false;
    }
    TokenResult not_keyword = Expect(TokenKind::kNot, ParseExpectation::kStatement);
    if (!not_keyword.has_value()) {
      return std::unexpected(not_keyword.error());
    }
    TokenResult exists = Expect(TokenKind::kExists, ParseExpectation::kStatement);
    if (!exists.has_value()) {
      return std::unexpected(exists.error());
    }
    return true;
  }

  [[nodiscard]] std::expected<ConflictAction, ParseError> ParseConflictClause() {
    if (!ConsumeIf(TokenKind::kOn)) {
      return ConflictAction::kDefault;
    }
    TokenResult conflict = Expect(TokenKind::kConflict, ParseExpectation::kConstraint);
    if (!conflict.has_value()) {
      return std::unexpected(conflict.error());
    }
    const Token action = Peek();
    ConflictAction result = ConflictAction::kDefault;
    switch (action.kind) {
      case TokenKind::kRollback:
        result = ConflictAction::kRollback;
        break;
      case TokenKind::kAbort:
        result = ConflictAction::kAbort;
        break;
      case TokenKind::kFail:
        result = ConflictAction::kFail;
        break;
      case TokenKind::kIgnore:
        result = ConflictAction::kIgnore;
        break;
      case TokenKind::kReplace:
        result = ConflictAction::kReplace;
        break;
      default:
        return std::unexpected(Unexpected(action, ParseExpectation::kConstraint));
    }
    static_cast<void>(Consume());
    return result;
  }

  [[nodiscard]] SortOrder ParseSortOrder() noexcept {
    if (ConsumeIf(TokenKind::kAsc)) {
      return SortOrder::kAscending;
    }
    if (ConsumeIf(TokenKind::kDesc)) {
      return SortOrder::kDescending;
    }
    return SortOrder::kDefault;
  }

  [[nodiscard]] bool IsTypeNumber(TokenKind kind) const noexcept {
    return kind == TokenKind::kInteger || kind == TokenKind::kFloat;
  }

  [[nodiscard]] std::expected<ByteOffset, ParseError> ParseSignedTypeNumber() {
    static_cast<void>(ConsumeIf(TokenKind::kPlus) || ConsumeIf(TokenKind::kMinus));
    const Token number = Peek();
    if (!IsTypeNumber(number.kind)) {
      return std::unexpected(Unexpected(number, ParseExpectation::kExpression));
    }
    return Consume().span.end();
  }

  [[nodiscard]] std::expected<std::optional<SourceSpan>, ParseError> ParseTypeName() {
    if (!IsNameToken(Peek().kind, NameClass::kIds)) {
      return std::optional<SourceSpan>{};
    }
    const Token first = Consume();
    ByteOffset end = first.span.end();
    while (IsNameToken(Peek().kind, NameClass::kIds)) {
      end = Consume().span.end();
    }
    if (ConsumeIf(TokenKind::kLeftParenthesis)) {
      auto first_number = ParseSignedTypeNumber();
      if (!first_number.has_value()) {
        return std::unexpected(first_number.error());
      }
      if (ConsumeIf(TokenKind::kComma)) {
        auto second_number = ParseSignedTypeNumber();
        if (!second_number.has_value()) {
          return std::unexpected(second_number.error());
        }
      }
      TokenResult right_parenthesis =
          Expect(TokenKind::kRightParenthesis, ParseExpectation::kRightParenthesis);
      if (!right_parenthesis.has_value()) {
        return std::unexpected(right_parenthesis.error());
      }
      end = right_parenthesis->span.end();
    }
    return std::optional<SourceSpan>{
        MakeSpan(first.span.begin(), end),
    };
  }

  [[nodiscard]] bool IsDefaultTerm(TokenKind kind) const noexcept {
    switch (kind) {
      case TokenKind::kNull:
      case TokenKind::kInteger:
      case TokenKind::kFloat:
      case TokenKind::kString:
      case TokenKind::kBlob:
      case TokenKind::kQuotedNumber:
      case TokenKind::kCurrentTimeKeyword:
        return true;
      default:
        return false;
    }
  }

  [[nodiscard]] ExpressionResult ParseDefaultValue() {
    const Token token = Peek();
    if (token.kind == TokenKind::kLeftParenthesis) {
      return ParseParenthesized(1U);
    }
    if (token.kind == TokenKind::kPlus || token.kind == TokenKind::kMinus) {
      const Token operation = Consume();
      if (!IsDefaultTerm(Peek().kind)) {
        return std::unexpected(Unexpected(Peek(), ParseExpectation::kExpression));
      }
      ExpressionResult term = ParseLiteral();
      if (!term.has_value()) {
        return std::unexpected(term.error());
      }
      const UnaryOperator unary =
          operation.kind == TokenKind::kPlus ? UnaryOperator::kPositive : UnaryOperator::kNegative;
      return AppendExpression(
          MakeSpan(operation.span.begin(), expressions_[term->value].span.end()),
          UnaryExpression{
              .op = unary,
              .operator_span = operation.span,
              .operand = *term,
          },
          Depth(*term) + 1U, operation);
    }
    if (IsDefaultTerm(token.kind)) {
      return ParseLiteral();
    }
    if (IsNameToken(token.kind, NameClass::kId)) {
      static_cast<void>(Consume());
      LiteralKind kind = LiteralKind::kString;
      if (token.kind == TokenKind::kIdentifier && EqualsAsciiCaseInsensitive(Text(token), "true")) {
        kind = LiteralKind::kTrue;
      } else if (token.kind == TokenKind::kIdentifier &&
                 EqualsAsciiCaseInsensitive(Text(token), "false")) {
        kind = LiteralKind::kFalse;
      }
      return AppendExpression(token.span,
                              LiteralExpression{
                                  .kind = kind,
                                  .token = token.span,
                              },
                              1U, token);
    }
    return std::unexpected(Unexpected(token, ParseExpectation::kExpression));
  }

  [[nodiscard]] bool IsColumnConstraintStart(TokenKind kind) const noexcept {
    switch (kind) {
      case TokenKind::kConstraint:
      case TokenKind::kNull:
      case TokenKind::kNot:
      case TokenKind::kPrimary:
      case TokenKind::kUnique:
      case TokenKind::kCheck:
      case TokenKind::kDefault:
      case TokenKind::kCollate:
      case TokenKind::kReferences:
      case TokenKind::kGenerated:
      case TokenKind::kAs:
      case TokenKind::kForeign:
      case TokenKind::kDeferrable:
        return true;
      default:
        return false;
    }
  }

  [[nodiscard]] std::expected<ColumnConstraint, ParseError> ParseColumnConstraint() {
    const Token first = Peek();
    std::optional<SourceSpan> name;
    if (ConsumeIf(TokenKind::kConstraint)) {
      TokenResult parsed_name = ParseNameToken(NameClass::kNm);
      if (!parsed_name.has_value()) {
        return std::unexpected(parsed_name.error());
      }
      name = parsed_name->span;
    }
    const Token constraint = Peek();
    ColumnConstraintPayload payload;

    if ((constraint.kind == TokenKind::kNot && Peek(1).kind == TokenKind::kDeferrable) ||
        constraint.kind == TokenKind::kReferences || constraint.kind == TokenKind::kGenerated ||
        constraint.kind == TokenKind::kAs || constraint.kind == TokenKind::kForeign ||
        constraint.kind == TokenKind::kDeferrable) {
      return std::unexpected(Unsupported(constraint));
    }

    if (constraint.kind == TokenKind::kNull) {
      static_cast<void>(Consume());
      auto conflict = ParseConflictClause();
      if (!conflict.has_value()) {
        return std::unexpected(conflict.error());
      }
      payload = NullColumnConstraint{.conflict = *conflict};
    } else if (constraint.kind == TokenKind::kNot) {
      static_cast<void>(Consume());
      TokenResult null_keyword = Expect(TokenKind::kNull, ParseExpectation::kConstraint);
      if (!null_keyword.has_value()) {
        return std::unexpected(null_keyword.error());
      }
      auto conflict = ParseConflictClause();
      if (!conflict.has_value()) {
        return std::unexpected(conflict.error());
      }
      payload = NotNullColumnConstraint{.conflict = *conflict};
    } else if (constraint.kind == TokenKind::kPrimary) {
      static_cast<void>(Consume());
      TokenResult key = Expect(TokenKind::kKey, ParseExpectation::kConstraint);
      if (!key.has_value()) {
        return std::unexpected(key.error());
      }
      const SortOrder order = ParseSortOrder();
      auto conflict = ParseConflictClause();
      if (!conflict.has_value()) {
        return std::unexpected(conflict.error());
      }
      const bool autoincrement = ConsumeIf(TokenKind::kAutoincrement);
      payload = PrimaryKeyColumnConstraint{
          .order = order,
          .conflict = *conflict,
          .autoincrement = autoincrement,
      };
    } else if (constraint.kind == TokenKind::kUnique) {
      static_cast<void>(Consume());
      auto conflict = ParseConflictClause();
      if (!conflict.has_value()) {
        return std::unexpected(conflict.error());
      }
      payload = UniqueColumnConstraint{.conflict = *conflict};
    } else if (constraint.kind == TokenKind::kCheck) {
      static_cast<void>(Consume());
      TokenResult left_parenthesis =
          Expect(TokenKind::kLeftParenthesis, ParseExpectation::kExpression);
      if (!left_parenthesis.has_value()) {
        return std::unexpected(left_parenthesis.error());
      }
      ExpressionResult expression = ParseGeneralExpression();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      TokenResult right_parenthesis =
          Expect(TokenKind::kRightParenthesis, ParseExpectation::kRightParenthesis);
      if (!right_parenthesis.has_value()) {
        return std::unexpected(right_parenthesis.error());
      }
      payload = CheckColumnConstraint{.expression = *expression};
    } else if (constraint.kind == TokenKind::kDefault) {
      static_cast<void>(Consume());
      ExpressionResult expression = ParseDefaultValue();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      payload = DefaultColumnConstraint{.expression = *expression};
    } else if (constraint.kind == TokenKind::kCollate) {
      static_cast<void>(Consume());
      TokenResult collation = ParseNameToken(NameClass::kIds);
      if (!collation.has_value()) {
        return std::unexpected(collation.error());
      }
      payload = CollateColumnConstraint{.collation = collation->span};
    } else {
      return std::unexpected(Unexpected(constraint, ParseExpectation::kConstraint));
    }

    return ColumnConstraint{
        .span = MakeSpan(first.span.begin(), last_consumed_end_),
        .name = name,
        .payload = payload,
    };
  }

  [[nodiscard]] std::expected<ColumnDefinition, ParseError> ParseColumnDefinition() {
    TokenResult name = ParseNameToken(NameClass::kNm);
    if (!name.has_value()) {
      return std::unexpected(name.error());
    }
    auto type = ParseTypeName();
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    std::vector<ColumnConstraint> constraints;
    while (IsColumnConstraintStart(Peek().kind)) {
      auto constraint = ParseColumnConstraint();
      if (!constraint.has_value()) {
        return std::unexpected(constraint.error());
      }
      constraints.push_back(*constraint);
    }
    return ColumnDefinition{
        .span = MakeSpan(name->span.begin(), last_consumed_end_),
        .name = name->span,
        .type_name = *type,
        .constraints = std::move(constraints),
    };
  }

  [[nodiscard]] std::expected<IndexedTerm, ParseError> ParseIndexedTerm() {
    const Token first = Peek();
    ExpressionResult expression = ParseExpression(0, ExpressionOptions{}, 1U);
    if (!expression.has_value()) {
      return std::unexpected(expression.error());
    }
    std::optional<SourceSpan> collation;
    assert(expression->value + 1U == expressions_.size());
    const auto* outer_collation =
        std::get_if<CollateExpression>(&expressions_[expression->value].payload);
    if (outer_collation != nullptr) {
      const ExpressionId operand = outer_collation->operand;
      collation = outer_collation->collation;
      expressions_.pop_back();
      expression_depths_.pop_back();
      expression = operand;
    }
    const SortOrder order = ParseSortOrder();
    return IndexedTerm{
        .span = MakeSpan(first.span.begin(), last_consumed_end_),
        .expression = *expression,
        .collation = collation,
        .order = order,
    };
  }

  [[nodiscard]] std::expected<std::vector<IndexedTerm>, ParseError> ParseIndexedTerms() {
    std::vector<IndexedTerm> terms;
    if (options_.maximum_columns == 0) {
      return std::unexpected(ResourceLimit(Peek()));
    }
    auto first = ParseIndexedTerm();
    if (!first.has_value()) {
      return std::unexpected(first.error());
    }
    terms.push_back(*first);
    while (ConsumeIf(TokenKind::kComma)) {
      if (terms.size() >= options_.maximum_columns) {
        return std::unexpected(ResourceLimit(Peek()));
      }
      auto term = ParseIndexedTerm();
      if (!term.has_value()) {
        return std::unexpected(term.error());
      }
      terms.push_back(*term);
    }
    return terms;
  }

  [[nodiscard]] bool IsTableConstraintStart(TokenKind kind) const noexcept {
    return kind == TokenKind::kConstraint || kind == TokenKind::kPrimary ||
           kind == TokenKind::kUnique || kind == TokenKind::kCheck || kind == TokenKind::kForeign;
  }

  [[nodiscard]] std::expected<TableConstraint, ParseError> ParseTableConstraint() {
    const Token first = Peek();
    std::optional<SourceSpan> name;
    if (ConsumeIf(TokenKind::kConstraint)) {
      TokenResult parsed_name = ParseNameToken(NameClass::kNm);
      if (!parsed_name.has_value()) {
        return std::unexpected(parsed_name.error());
      }
      name = parsed_name->span;
    }
    const Token constraint = Peek();
    TableConstraintPayload payload;

    if (constraint.kind == TokenKind::kPrimary) {
      static_cast<void>(Consume());
      TokenResult key = Expect(TokenKind::kKey, ParseExpectation::kConstraint);
      if (!key.has_value()) {
        return std::unexpected(key.error());
      }
      TokenResult left_parenthesis =
          Expect(TokenKind::kLeftParenthesis, ParseExpectation::kExpression);
      if (!left_parenthesis.has_value()) {
        return std::unexpected(left_parenthesis.error());
      }
      auto terms = ParseIndexedTerms();
      if (!terms.has_value()) {
        return std::unexpected(terms.error());
      }
      const bool autoincrement = ConsumeIf(TokenKind::kAutoincrement);
      TokenResult right_parenthesis =
          Expect(TokenKind::kRightParenthesis, ParseExpectation::kCommaOrRightParenthesis);
      if (!right_parenthesis.has_value()) {
        return std::unexpected(right_parenthesis.error());
      }
      auto conflict = ParseConflictClause();
      if (!conflict.has_value()) {
        return std::unexpected(conflict.error());
      }
      payload = PrimaryKeyTableConstraint{
          .terms = std::move(*terms),
          .conflict = *conflict,
          .autoincrement = autoincrement,
      };
    } else if (constraint.kind == TokenKind::kUnique) {
      static_cast<void>(Consume());
      TokenResult left_parenthesis =
          Expect(TokenKind::kLeftParenthesis, ParseExpectation::kExpression);
      if (!left_parenthesis.has_value()) {
        return std::unexpected(left_parenthesis.error());
      }
      auto terms = ParseIndexedTerms();
      if (!terms.has_value()) {
        return std::unexpected(terms.error());
      }
      TokenResult right_parenthesis =
          Expect(TokenKind::kRightParenthesis, ParseExpectation::kCommaOrRightParenthesis);
      if (!right_parenthesis.has_value()) {
        return std::unexpected(right_parenthesis.error());
      }
      auto conflict = ParseConflictClause();
      if (!conflict.has_value()) {
        return std::unexpected(conflict.error());
      }
      payload = UniqueTableConstraint{
          .terms = std::move(*terms),
          .conflict = *conflict,
      };
    } else if (constraint.kind == TokenKind::kCheck) {
      static_cast<void>(Consume());
      TokenResult left_parenthesis =
          Expect(TokenKind::kLeftParenthesis, ParseExpectation::kExpression);
      if (!left_parenthesis.has_value()) {
        return std::unexpected(left_parenthesis.error());
      }
      ExpressionResult expression = ParseGeneralExpression();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      TokenResult right_parenthesis =
          Expect(TokenKind::kRightParenthesis, ParseExpectation::kRightParenthesis);
      if (!right_parenthesis.has_value()) {
        return std::unexpected(right_parenthesis.error());
      }
      auto ignored_conflict = ParseConflictClause();
      if (!ignored_conflict.has_value()) {
        return std::unexpected(ignored_conflict.error());
      }
      payload = CheckTableConstraint{.expression = *expression};
    } else if (constraint.kind == TokenKind::kForeign) {
      return std::unexpected(Unsupported(constraint));
    } else {
      return std::unexpected(Unexpected(constraint, ParseExpectation::kConstraint));
    }

    return TableConstraint{
        .span = MakeSpan(first.span.begin(), last_consumed_end_),
        .name = name,
        .payload = std::move(payload),
    };
  }

  [[nodiscard]] bool IsStrictToken(Token token) const noexcept {
    return IsNameToken(token.kind, NameClass::kNm) &&
           EqualsAsciiCaseInsensitive(Text(token), "strict");
  }

  [[nodiscard]] std::expected<ParsedTableOptions, ParseError> ParseTableOptions() {
    ParsedTableOptions options;
    bool require_option = ConsumeIf(TokenKind::kComma);
    while (true) {
      if (Peek().kind == TokenKind::kWithout) {
        static_cast<void>(Consume());
        const Token option = Peek();
        if (!IsNameToken(option.kind, NameClass::kNm)) {
          return std::unexpected(Unexpected(option, ParseExpectation::kTableOption));
        }
        static_cast<void>(Consume());
        if (!EqualsAsciiCaseInsensitive(Text(option), "rowid")) {
          return std::unexpected(Unexpected(option, ParseExpectation::kTableOption));
        }
        options.without_rowid = true;
      } else if (IsStrictToken(Peek())) {
        options.strict = true;
        static_cast<void>(Consume());
      } else if (require_option || IsNameToken(Peek().kind, NameClass::kNm)) {
        return std::unexpected(Unexpected(Peek(), ParseExpectation::kTableOption));
      } else {
        break;
      }

      if (!ConsumeIf(TokenKind::kComma)) {
        break;
      }
      require_option = true;
    }
    return options;
  }

  [[nodiscard]] StatementResult ParseCreateTable(Token create_keyword, bool temporary) {
    auto if_not_exists = ParseIfNotExists();
    if (!if_not_exists.has_value()) {
      return std::unexpected(if_not_exists.error());
    }
    NameResult name = ParseQualifiedName(2U, NameClass::kNm);
    if (!name.has_value()) {
      return std::unexpected(name.error());
    }
    if (Peek().kind == TokenKind::kAs) {
      return std::unexpected(Unsupported(Peek()));
    }
    TokenResult left_parenthesis =
        Expect(TokenKind::kLeftParenthesis, ParseExpectation::kColumnDefinition);
    if (!left_parenthesis.has_value()) {
      return std::unexpected(left_parenthesis.error());
    }
    if (IsTableConstraintStart(Peek().kind) || Peek().kind == TokenKind::kRightParenthesis) {
      return std::unexpected(Unexpected(Peek(), ParseExpectation::kColumnDefinition));
    }

    std::vector<ColumnDefinition> columns;
    if (options_.maximum_columns == 0) {
      return std::unexpected(ResourceLimit(Peek()));
    }
    auto first_column = ParseColumnDefinition();
    if (!first_column.has_value()) {
      return std::unexpected(first_column.error());
    }
    columns.push_back(std::move(*first_column));

    std::vector<TableConstraint> constraints;
    bool parsing_constraints = false;
    while (true) {
      if (parsing_constraints && IsTableConstraintStart(Peek().kind)) {
        auto constraint = ParseTableConstraint();
        if (!constraint.has_value()) {
          return std::unexpected(constraint.error());
        }
        constraints.push_back(std::move(*constraint));
        continue;
      }
      if (!ConsumeIf(TokenKind::kComma)) {
        break;
      }
      if (Peek().kind == TokenKind::kRightParenthesis || Peek().kind == TokenKind::kComma) {
        return std::unexpected(Unexpected(Peek(), parsing_constraints
                                                      ? ParseExpectation::kConstraint
                                                      : ParseExpectation::kColumnDefinition));
      }
      if (IsTableConstraintStart(Peek().kind)) {
        parsing_constraints = true;
        auto constraint = ParseTableConstraint();
        if (!constraint.has_value()) {
          return std::unexpected(constraint.error());
        }
        constraints.push_back(std::move(*constraint));
      } else if (parsing_constraints) {
        return std::unexpected(Unexpected(Peek(), ParseExpectation::kConstraint));
      } else {
        if (columns.size() >= options_.maximum_columns) {
          return std::unexpected(ResourceLimit(Peek()));
        }
        auto column = ParseColumnDefinition();
        if (!column.has_value()) {
          return std::unexpected(column.error());
        }
        columns.push_back(std::move(*column));
      }
    }

    TokenResult right_parenthesis =
        Expect(TokenKind::kRightParenthesis, ParseExpectation::kCommaOrRightParenthesis);
    if (!right_parenthesis.has_value()) {
      return std::unexpected(right_parenthesis.error());
    }
    auto options = ParseTableOptions();
    if (!options.has_value()) {
      return std::unexpected(options.error());
    }

    return Statement{CreateTableStatement{
        .span = MakeSpan(create_keyword.span.begin(), last_consumed_end_),
        .temporary = temporary,
        .if_not_exists = *if_not_exists,
        .name = std::move(*name),
        .columns = std::move(columns),
        .constraints = std::move(constraints),
        .without_rowid = options->without_rowid,
        .strict = options->strict,
    }};
  }

  [[nodiscard]] StatementResult ParseCreateIndex(Token create_keyword, bool unique) {
    auto if_not_exists = ParseIfNotExists();
    if (!if_not_exists.has_value()) {
      return std::unexpected(if_not_exists.error());
    }
    NameResult name = ParseQualifiedName(2U, NameClass::kNm);
    if (!name.has_value()) {
      return std::unexpected(name.error());
    }
    TokenResult on = Expect(TokenKind::kOn, ParseExpectation::kStatement);
    if (!on.has_value()) {
      return std::unexpected(on.error());
    }
    NameResult table = ParseQualifiedName(1U, NameClass::kNm);
    if (!table.has_value()) {
      return std::unexpected(table.error());
    }
    TokenResult left_parenthesis =
        Expect(TokenKind::kLeftParenthesis, ParseExpectation::kExpression);
    if (!left_parenthesis.has_value()) {
      return std::unexpected(left_parenthesis.error());
    }
    auto terms = ParseIndexedTerms();
    if (!terms.has_value()) {
      return std::unexpected(terms.error());
    }
    TokenResult right_parenthesis =
        Expect(TokenKind::kRightParenthesis, ParseExpectation::kCommaOrRightParenthesis);
    if (!right_parenthesis.has_value()) {
      return std::unexpected(right_parenthesis.error());
    }
    std::optional<ExpressionId> where;
    if (ConsumeIf(TokenKind::kWhere)) {
      ExpressionResult expression = ParseGeneralExpression();
      if (!expression.has_value()) {
        return std::unexpected(expression.error());
      }
      where = *expression;
    }
    return Statement{CreateIndexStatement{
        .span = MakeSpan(create_keyword.span.begin(), last_consumed_end_),
        .unique = unique,
        .if_not_exists = *if_not_exists,
        .name = std::move(*name),
        .table = std::move(*table),
        .terms = std::move(*terms),
        .where = where,
    }};
  }

  [[nodiscard]] StatementResult ParseAnalyze() {
    const Token keyword = Consume();
    std::optional<QualifiedName> target;
    if (Peek().kind != TokenKind::kSemicolon && Peek().kind != TokenKind::kEndOfInput) {
      NameResult name = ParseQualifiedName(2U, NameClass::kNm);
      if (!name.has_value()) {
        return std::unexpected(name.error());
      }
      target = std::move(*name);
    }
    return Statement{AnalyzeStatement{
        .span = MakeSpan(keyword.span.begin(), last_consumed_end_),
        .target = std::move(target),
    }};
  }

  [[nodiscard]] StatementResult ParseCreate() {
    const Token create_keyword = Consume();
    bool temporary = false;
    if (ConsumeIf(TokenKind::kTemp)) {
      temporary = true;
    }
    if (ConsumeIf(TokenKind::kTable)) {
      return ParseCreateTable(create_keyword, temporary);
    }
    if (temporary) {
      if (Peek().kind == TokenKind::kTrigger || Peek().kind == TokenKind::kView) {
        return std::unexpected(Unsupported(Peek()));
      }
      return std::unexpected(Unexpected(Peek(), ParseExpectation::kStatement));
    }

    bool unique = false;
    if (ConsumeIf(TokenKind::kUnique)) {
      unique = true;
    }
    if (ConsumeIf(TokenKind::kIndex)) {
      return ParseCreateIndex(create_keyword, unique);
    }
    if (!unique && (Peek().kind == TokenKind::kTrigger || Peek().kind == TokenKind::kView ||
                    Peek().kind == TokenKind::kVirtual)) {
      return std::unexpected(Unsupported(Peek()));
    }
    return std::unexpected(Unexpected(Peek(), ParseExpectation::kStatement));
  }

  [[nodiscard]] bool IsUnsupportedStatementStart(TokenKind kind) const noexcept {
    switch (kind) {
      case TokenKind::kWith:
      case TokenKind::kInsert:
      case TokenKind::kUpdate:
      case TokenKind::kDelete:
      case TokenKind::kReplace:
      case TokenKind::kBegin:
      case TokenKind::kCommit:
      case TokenKind::kRollback:
      case TokenKind::kSavepoint:
      case TokenKind::kRelease:
      case TokenKind::kPragma:
      case TokenKind::kAttach:
      case TokenKind::kDetach:
      case TokenKind::kVacuum:
      case TokenKind::kReindex:
      case TokenKind::kAlter:
      case TokenKind::kDrop:
        return true;
      default:
        return false;
    }
  }

  [[nodiscard]] StatementResult ParseStatement() {
    const Token token = Peek();
    if (token.kind == TokenKind::kIllegal) {
      return std::unexpected(IllegalToken(token));
    }
    if (token.kind == TokenKind::kSelect || token.kind == TokenKind::kValues) {
      return ParseSelect();
    }
    if (token.kind == TokenKind::kAnalyze) {
      return ParseAnalyze();
    }
    if (token.kind == TokenKind::kCreate) {
      return ParseCreate();
    }
    if (token.kind == TokenKind::kInsert) {
      return ParseInsert();
    }
    if (token.kind == TokenKind::kUpdate) {
      return ParseUpdate();
    }
    if (token.kind == TokenKind::kDelete) {
      return ParseDelete();
    }
    if (token.kind == TokenKind::kBegin) {
      return ParseBegin();
    }
    if (token.kind == TokenKind::kCommit || token.kind == TokenKind::kEnd) {
      return ParseCommit();
    }
    if (token.kind == TokenKind::kRollback) {
      return ParseRollback();
    }
    if (token.kind == TokenKind::kSavepoint) {
      return ParseSavepoint();
    }
    if (token.kind == TokenKind::kRelease) {
      return ParseRelease();
    }
    if (IsUnsupportedStatementStart(token.kind)) {
      return std::unexpected(Unsupported(token));
    }
    return std::unexpected(Unexpected(token, ParseExpectation::kStatement));
  }

  [[nodiscard]] static SourceSpan StatementSpan(const Statement& statement) {
    return std::visit([](const auto& value) { return value.span; }, statement);
  }

  Utf8View source_;
  ByteOffset base_offset_;
  Lexer lexer_;
  std::array<Token, 4> lookahead_{};
  std::size_t lookahead_count_ = 0;
  std::optional<TokenKind> last_read_kind_;
  ByteOffset last_consumed_end_;
  std::vector<Expression> expressions_;
  std::vector<std::size_t> expression_depths_;
  ParseOptions options_;
};

[[nodiscard]] ParseError PrefixIllegal(Token token) noexcept {
  return ParseError{
      .code = ParseErrorCode::kIllegalToken,
      .span = token.span,
      .actual = TokenKind::kIllegal,
      .expected = ParseExpectation::kNone,
      .next_offset = token.span.begin(),
  };
}

}  // namespace

ParseResult ParseOne(Utf8View source, ParseOptions options) {
  if (source.size_bytes() > options.maximum_source_bytes) {
    const SourceSpan span = MakeSpan(ByteOffset{0}, ByteOffset{source.size_bytes()});
    return std::unexpected(ParseError{
        .code = ParseErrorCode::kResourceLimitExceeded,
        .span = span,
        .actual = TokenKind::kEndOfInput,
        .expected = ParseExpectation::kNone,
        .next_offset = span.end(),
    });
  }
  Lexer lexer{source};
  while (true) {
    const Token token = ReadRawSignificant(lexer);
    if (token.kind == TokenKind::kIllegal) {
      return std::unexpected(PrefixIllegal(token));
    }
    if (token.kind == TokenKind::kEndOfInput) {
      return ParseOutput{
          .tree = std::nullopt,
          .next_offset = token.span.begin(),
      };
    }
    if (token.kind == TokenKind::kSemicolon) {
      continue;
    }

    const std::size_t statement_begin = token.span.begin().value();
    const Utf8View statement_source{
        source.bytes().substr(statement_begin),
    };
    return Parser{statement_source, token.span.begin(), options}.Parse();
  }
}

}  // namespace modern_sqlite
