#ifndef MODERN_SQLITE_SYNTAX_LEXER_HPP_
#define MODERN_SQLITE_SYNTAX_LEXER_HPP_

#include <compare>
#include <cstdint>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

enum class TokenKind : std::uint8_t {
  kEndOfInput,
  kIllegal,
  kWhitespace,
  kComment,
  kIdentifier,
  kString,
  kInteger,
  kFloat,
  kBlob,
  kVariable,
  kQuotedNumber,
  kSemicolon,
  kLeftParenthesis,
  kRightParenthesis,
  kComma,
  kBitwiseAnd,
  kBitwiseNot,
  kPlus,
  kMinus,
  kAsterisk,
  kSlash,
  kRemainder,
  kConcatenate,
  kPointer,
  kEquality,
  kLessThanOrEqual,
  kNotEqual,
  kLeftShift,
  kLessThan,
  kGreaterThanOrEqual,
  kRightShift,
  kGreaterThan,
  kBitwiseOr,
  kDot,
  kAbort,
  kAction,
  kAdd,
  kAfter,
  kAll,
  kAlter,
  kAlways,
  kAnalyze,
  kAnd,
  kAs,
  kAsc,
  kAttach,
  kAutoincrement,
  kBefore,
  kBegin,
  kBetween,
  kBy,
  kCascade,
  kCase,
  kCast,
  kCheck,
  kCollate,
  kColumnKeyword,
  kCommit,
  kConflict,
  kConstraint,
  kCreate,
  kCurrent,
  kCurrentTimeKeyword,
  kDatabase,
  kDefault,
  kDeferrable,
  kDeferred,
  kDelete,
  kDesc,
  kDetach,
  kDistinct,
  kDo,
  kDrop,
  kEach,
  kElse,
  kEnd,
  kEscape,
  kExcept,
  kExclude,
  kExclusive,
  kExists,
  kExplain,
  kFail,
  kFilter,
  kFirst,
  kFollowing,
  kFor,
  kForeign,
  kFrom,
  kGenerated,
  kGroup,
  kGroups,
  kHaving,
  kIf,
  kIgnore,
  kImmediate,
  kIn,
  kIndex,
  kIndexed,
  kInitially,
  kInsert,
  kInstead,
  kIntersect,
  kInto,
  kIs,
  kIsNull,
  kJoin,
  kJoinKeyword,
  kKey,
  kLast,
  kLikeKeyword,
  kLimit,
  kMatch,
  kMaterialized,
  kNo,
  kNot,
  kNotNull,
  kNothing,
  kNull,
  kNulls,
  kOf,
  kOffset,
  kOn,
  kOr,
  kOrder,
  kOthers,
  kOver,
  kPartition,
  kPlan,
  kPragma,
  kPreceding,
  kPrimary,
  kQuery,
  kRaise,
  kRange,
  kRecursive,
  kReferences,
  kReindex,
  kRelease,
  kRename,
  kReplace,
  kRestrict,
  kReturning,
  kRollback,
  kRow,
  kRows,
  kSavepoint,
  kSelect,
  kSet,
  kTable,
  kTemp,
  kThen,
  kTies,
  kTo,
  kTransaction,
  kTrigger,
  kUnbounded,
  kUnion,
  kUnique,
  kUpdate,
  kUsing,
  kVacuum,
  kValues,
  kView,
  kVirtual,
  kWhen,
  kWhere,
  kWindow,
  kWith,
  kWithout,
};

struct Token {
  TokenKind kind = TokenKind::kEndOfInput;
  SourceSpan span;

  constexpr auto operator<=>(const Token&) const noexcept = default;
};

class Lexer final {
 public:
  explicit constexpr Lexer(Utf8View source) noexcept : source_(source) {}

  [[nodiscard]] Token Next() noexcept;

  [[nodiscard]] constexpr ByteOffset position() const noexcept { return position_; }
  [[nodiscard]] constexpr bool finished() const noexcept { return ByteAt(position_.value()) == 0U; }

 private:
  [[nodiscard]] constexpr std::uint8_t ByteAt(std::size_t offset) const noexcept {
    if (offset >= source_.size_bytes()) {
      return 0;
    }
    return static_cast<std::uint8_t>(static_cast<unsigned char>(source_.bytes()[offset]));
  }

  Utf8View source_;
  ByteOffset position_;
};

[[nodiscard]] constexpr bool IsTrivia(TokenKind kind) noexcept {
  return kind == TokenKind::kWhitespace || kind == TokenKind::kComment;
}

[[nodiscard]] constexpr bool IsKeyword(TokenKind kind) noexcept {
  return kind >= TokenKind::kAbort && kind <= TokenKind::kWithout;
}

[[nodiscard]] std::string_view TokenKindName(TokenKind kind) noexcept;

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_SYNTAX_LEXER_HPP_
