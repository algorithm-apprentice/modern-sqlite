#include "modern_sqlite/syntax/lexer.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {
namespace {

enum class CharacterClass : std::uint8_t {
  kIllegal,
  kSpace,
  kMinus,
  kLeftParenthesis,
  kRightParenthesis,
  kSemicolon,
  kPlus,
  kAsterisk,
  kSlash,
  kRemainder,
  kEquality,
  kLessThan,
  kGreaterThan,
  kBang,
  kPipe,
  kComma,
  kBitwiseAnd,
  kBitwiseNot,
  kDot,
  kDigit,
  kQuoteString,
  kQuoteIdentifier,
  kVariableNumeric,
  kVariableAlpha,
  kKeyword,
  kIdentifier,
  kX,
  kBom,
};

[[nodiscard]] consteval std::array<CharacterClass, 256> BuildCharacterClasses() {
  std::array<CharacterClass, 256> classes{};
  classes.fill(CharacterClass::kIllegal);

  for (std::size_t value = 0x80; value < classes.size(); ++value) {
    classes[value] = CharacterClass::kIdentifier;
  }
  for (auto value = static_cast<std::uint8_t>('A'); value <= static_cast<std::uint8_t>('Z');
       ++value) {
    classes[value] = CharacterClass::kKeyword;
  }
  for (auto value = static_cast<std::uint8_t>('a'); value <= static_cast<std::uint8_t>('z');
       ++value) {
    classes[value] = CharacterClass::kKeyword;
  }
  for (auto value = static_cast<std::uint8_t>('0'); value <= static_cast<std::uint8_t>('9');
       ++value) {
    classes[value] = CharacterClass::kDigit;
  }

  classes[static_cast<std::uint8_t>('\t')] = CharacterClass::kSpace;
  classes[static_cast<std::uint8_t>('\n')] = CharacterClass::kSpace;
  classes[static_cast<std::uint8_t>('\f')] = CharacterClass::kSpace;
  classes[static_cast<std::uint8_t>('\r')] = CharacterClass::kSpace;
  classes[static_cast<std::uint8_t>(' ')] = CharacterClass::kSpace;
  classes[static_cast<std::uint8_t>('-')] = CharacterClass::kMinus;
  classes[static_cast<std::uint8_t>('(')] = CharacterClass::kLeftParenthesis;
  classes[static_cast<std::uint8_t>(')')] = CharacterClass::kRightParenthesis;
  classes[static_cast<std::uint8_t>(';')] = CharacterClass::kSemicolon;
  classes[static_cast<std::uint8_t>('+')] = CharacterClass::kPlus;
  classes[static_cast<std::uint8_t>('*')] = CharacterClass::kAsterisk;
  classes[static_cast<std::uint8_t>('/')] = CharacterClass::kSlash;
  classes[static_cast<std::uint8_t>('%')] = CharacterClass::kRemainder;
  classes[static_cast<std::uint8_t>('=')] = CharacterClass::kEquality;
  classes[static_cast<std::uint8_t>('<')] = CharacterClass::kLessThan;
  classes[static_cast<std::uint8_t>('>')] = CharacterClass::kGreaterThan;
  classes[static_cast<std::uint8_t>('!')] = CharacterClass::kBang;
  classes[static_cast<std::uint8_t>('|')] = CharacterClass::kPipe;
  classes[static_cast<std::uint8_t>(',')] = CharacterClass::kComma;
  classes[static_cast<std::uint8_t>('&')] = CharacterClass::kBitwiseAnd;
  classes[static_cast<std::uint8_t>('~')] = CharacterClass::kBitwiseNot;
  classes[static_cast<std::uint8_t>('.')] = CharacterClass::kDot;
  classes[static_cast<std::uint8_t>('\'')] = CharacterClass::kQuoteString;
  classes[static_cast<std::uint8_t>('"')] = CharacterClass::kQuoteIdentifier;
  classes[static_cast<std::uint8_t>('`')] = CharacterClass::kQuoteIdentifier;
  classes[static_cast<std::uint8_t>('[')] = CharacterClass::kQuoteIdentifier;
  classes[static_cast<std::uint8_t>('?')] = CharacterClass::kVariableNumeric;
  classes[static_cast<std::uint8_t>('$')] = CharacterClass::kVariableAlpha;
  classes[static_cast<std::uint8_t>('@')] = CharacterClass::kVariableAlpha;
  classes[static_cast<std::uint8_t>(':')] = CharacterClass::kVariableAlpha;
  classes[static_cast<std::uint8_t>('#')] = CharacterClass::kVariableAlpha;
  classes[static_cast<std::uint8_t>('_')] = CharacterClass::kIdentifier;
  classes[static_cast<std::uint8_t>('x')] = CharacterClass::kX;
  classes[static_cast<std::uint8_t>('X')] = CharacterClass::kX;
  classes[0xef] = CharacterClass::kBom;
  return classes;
}

inline constexpr auto kCharacterClasses = BuildCharacterClasses();

struct KeywordEntry {
  std::string_view text;
  TokenKind kind;
};

inline constexpr std::array kKeywords{
    KeywordEntry{.text = "ABORT", .kind = TokenKind::kAbort},
    KeywordEntry{.text = "ACTION", .kind = TokenKind::kAction},
    KeywordEntry{.text = "ADD", .kind = TokenKind::kAdd},
    KeywordEntry{.text = "AFTER", .kind = TokenKind::kAfter},
    KeywordEntry{.text = "ALL", .kind = TokenKind::kAll},
    KeywordEntry{.text = "ALTER", .kind = TokenKind::kAlter},
    KeywordEntry{.text = "ALWAYS", .kind = TokenKind::kAlways},
    KeywordEntry{.text = "ANALYZE", .kind = TokenKind::kAnalyze},
    KeywordEntry{.text = "AND", .kind = TokenKind::kAnd},
    KeywordEntry{.text = "AS", .kind = TokenKind::kAs},
    KeywordEntry{.text = "ASC", .kind = TokenKind::kAsc},
    KeywordEntry{.text = "ATTACH", .kind = TokenKind::kAttach},
    KeywordEntry{.text = "AUTOINCREMENT", .kind = TokenKind::kAutoincrement},
    KeywordEntry{.text = "BEFORE", .kind = TokenKind::kBefore},
    KeywordEntry{.text = "BEGIN", .kind = TokenKind::kBegin},
    KeywordEntry{.text = "BETWEEN", .kind = TokenKind::kBetween},
    KeywordEntry{.text = "BY", .kind = TokenKind::kBy},
    KeywordEntry{.text = "CASCADE", .kind = TokenKind::kCascade},
    KeywordEntry{.text = "CASE", .kind = TokenKind::kCase},
    KeywordEntry{.text = "CAST", .kind = TokenKind::kCast},
    KeywordEntry{.text = "CHECK", .kind = TokenKind::kCheck},
    KeywordEntry{.text = "COLLATE", .kind = TokenKind::kCollate},
    KeywordEntry{.text = "COLUMN", .kind = TokenKind::kColumnKeyword},
    KeywordEntry{.text = "COMMIT", .kind = TokenKind::kCommit},
    KeywordEntry{.text = "CONFLICT", .kind = TokenKind::kConflict},
    KeywordEntry{.text = "CONSTRAINT", .kind = TokenKind::kConstraint},
    KeywordEntry{.text = "CREATE", .kind = TokenKind::kCreate},
    KeywordEntry{.text = "CROSS", .kind = TokenKind::kJoinKeyword},
    KeywordEntry{.text = "CURRENT", .kind = TokenKind::kCurrent},
    KeywordEntry{.text = "CURRENT_DATE", .kind = TokenKind::kCurrentTimeKeyword},
    KeywordEntry{.text = "CURRENT_TIME", .kind = TokenKind::kCurrentTimeKeyword},
    KeywordEntry{.text = "CURRENT_TIMESTAMP", .kind = TokenKind::kCurrentTimeKeyword},
    KeywordEntry{.text = "DATABASE", .kind = TokenKind::kDatabase},
    KeywordEntry{.text = "DEFAULT", .kind = TokenKind::kDefault},
    KeywordEntry{.text = "DEFERRABLE", .kind = TokenKind::kDeferrable},
    KeywordEntry{.text = "DEFERRED", .kind = TokenKind::kDeferred},
    KeywordEntry{.text = "DELETE", .kind = TokenKind::kDelete},
    KeywordEntry{.text = "DESC", .kind = TokenKind::kDesc},
    KeywordEntry{.text = "DETACH", .kind = TokenKind::kDetach},
    KeywordEntry{.text = "DISTINCT", .kind = TokenKind::kDistinct},
    KeywordEntry{.text = "DO", .kind = TokenKind::kDo},
    KeywordEntry{.text = "DROP", .kind = TokenKind::kDrop},
    KeywordEntry{.text = "EACH", .kind = TokenKind::kEach},
    KeywordEntry{.text = "ELSE", .kind = TokenKind::kElse},
    KeywordEntry{.text = "END", .kind = TokenKind::kEnd},
    KeywordEntry{.text = "ESCAPE", .kind = TokenKind::kEscape},
    KeywordEntry{.text = "EXCEPT", .kind = TokenKind::kExcept},
    KeywordEntry{.text = "EXCLUDE", .kind = TokenKind::kExclude},
    KeywordEntry{.text = "EXCLUSIVE", .kind = TokenKind::kExclusive},
    KeywordEntry{.text = "EXISTS", .kind = TokenKind::kExists},
    KeywordEntry{.text = "EXPLAIN", .kind = TokenKind::kExplain},
    KeywordEntry{.text = "FAIL", .kind = TokenKind::kFail},
    KeywordEntry{.text = "FILTER", .kind = TokenKind::kFilter},
    KeywordEntry{.text = "FIRST", .kind = TokenKind::kFirst},
    KeywordEntry{.text = "FOLLOWING", .kind = TokenKind::kFollowing},
    KeywordEntry{.text = "FOR", .kind = TokenKind::kFor},
    KeywordEntry{.text = "FOREIGN", .kind = TokenKind::kForeign},
    KeywordEntry{.text = "FROM", .kind = TokenKind::kFrom},
    KeywordEntry{.text = "FULL", .kind = TokenKind::kJoinKeyword},
    KeywordEntry{.text = "GENERATED", .kind = TokenKind::kGenerated},
    KeywordEntry{.text = "GLOB", .kind = TokenKind::kLikeKeyword},
    KeywordEntry{.text = "GROUP", .kind = TokenKind::kGroup},
    KeywordEntry{.text = "GROUPS", .kind = TokenKind::kGroups},
    KeywordEntry{.text = "HAVING", .kind = TokenKind::kHaving},
    KeywordEntry{.text = "IF", .kind = TokenKind::kIf},
    KeywordEntry{.text = "IGNORE", .kind = TokenKind::kIgnore},
    KeywordEntry{.text = "IMMEDIATE", .kind = TokenKind::kImmediate},
    KeywordEntry{.text = "IN", .kind = TokenKind::kIn},
    KeywordEntry{.text = "INDEX", .kind = TokenKind::kIndex},
    KeywordEntry{.text = "INDEXED", .kind = TokenKind::kIndexed},
    KeywordEntry{.text = "INITIALLY", .kind = TokenKind::kInitially},
    KeywordEntry{.text = "INNER", .kind = TokenKind::kJoinKeyword},
    KeywordEntry{.text = "INSERT", .kind = TokenKind::kInsert},
    KeywordEntry{.text = "INSTEAD", .kind = TokenKind::kInstead},
    KeywordEntry{.text = "INTERSECT", .kind = TokenKind::kIntersect},
    KeywordEntry{.text = "INTO", .kind = TokenKind::kInto},
    KeywordEntry{.text = "IS", .kind = TokenKind::kIs},
    KeywordEntry{.text = "ISNULL", .kind = TokenKind::kIsNull},
    KeywordEntry{.text = "JOIN", .kind = TokenKind::kJoin},
    KeywordEntry{.text = "KEY", .kind = TokenKind::kKey},
    KeywordEntry{.text = "LAST", .kind = TokenKind::kLast},
    KeywordEntry{.text = "LEFT", .kind = TokenKind::kJoinKeyword},
    KeywordEntry{.text = "LIKE", .kind = TokenKind::kLikeKeyword},
    KeywordEntry{.text = "LIMIT", .kind = TokenKind::kLimit},
    KeywordEntry{.text = "MATCH", .kind = TokenKind::kMatch},
    KeywordEntry{.text = "MATERIALIZED", .kind = TokenKind::kMaterialized},
    KeywordEntry{.text = "NATURAL", .kind = TokenKind::kJoinKeyword},
    KeywordEntry{.text = "NO", .kind = TokenKind::kNo},
    KeywordEntry{.text = "NOT", .kind = TokenKind::kNot},
    KeywordEntry{.text = "NOTHING", .kind = TokenKind::kNothing},
    KeywordEntry{.text = "NOTNULL", .kind = TokenKind::kNotNull},
    KeywordEntry{.text = "NULL", .kind = TokenKind::kNull},
    KeywordEntry{.text = "NULLS", .kind = TokenKind::kNulls},
    KeywordEntry{.text = "OF", .kind = TokenKind::kOf},
    KeywordEntry{.text = "OFFSET", .kind = TokenKind::kOffset},
    KeywordEntry{.text = "ON", .kind = TokenKind::kOn},
    KeywordEntry{.text = "OR", .kind = TokenKind::kOr},
    KeywordEntry{.text = "ORDER", .kind = TokenKind::kOrder},
    KeywordEntry{.text = "OTHERS", .kind = TokenKind::kOthers},
    KeywordEntry{.text = "OUTER", .kind = TokenKind::kJoinKeyword},
    KeywordEntry{.text = "OVER", .kind = TokenKind::kOver},
    KeywordEntry{.text = "PARTITION", .kind = TokenKind::kPartition},
    KeywordEntry{.text = "PLAN", .kind = TokenKind::kPlan},
    KeywordEntry{.text = "PRAGMA", .kind = TokenKind::kPragma},
    KeywordEntry{.text = "PRECEDING", .kind = TokenKind::kPreceding},
    KeywordEntry{.text = "PRIMARY", .kind = TokenKind::kPrimary},
    KeywordEntry{.text = "QUERY", .kind = TokenKind::kQuery},
    KeywordEntry{.text = "RAISE", .kind = TokenKind::kRaise},
    KeywordEntry{.text = "RANGE", .kind = TokenKind::kRange},
    KeywordEntry{.text = "RECURSIVE", .kind = TokenKind::kRecursive},
    KeywordEntry{.text = "REFERENCES", .kind = TokenKind::kReferences},
    KeywordEntry{.text = "REGEXP", .kind = TokenKind::kLikeKeyword},
    KeywordEntry{.text = "REINDEX", .kind = TokenKind::kReindex},
    KeywordEntry{.text = "RELEASE", .kind = TokenKind::kRelease},
    KeywordEntry{.text = "RENAME", .kind = TokenKind::kRename},
    KeywordEntry{.text = "REPLACE", .kind = TokenKind::kReplace},
    KeywordEntry{.text = "RESTRICT", .kind = TokenKind::kRestrict},
    KeywordEntry{.text = "RETURNING", .kind = TokenKind::kReturning},
    KeywordEntry{.text = "RIGHT", .kind = TokenKind::kJoinKeyword},
    KeywordEntry{.text = "ROLLBACK", .kind = TokenKind::kRollback},
    KeywordEntry{.text = "ROW", .kind = TokenKind::kRow},
    KeywordEntry{.text = "ROWS", .kind = TokenKind::kRows},
    KeywordEntry{.text = "SAVEPOINT", .kind = TokenKind::kSavepoint},
    KeywordEntry{.text = "SELECT", .kind = TokenKind::kSelect},
    KeywordEntry{.text = "SET", .kind = TokenKind::kSet},
    KeywordEntry{.text = "TABLE", .kind = TokenKind::kTable},
    KeywordEntry{.text = "TEMP", .kind = TokenKind::kTemp},
    KeywordEntry{.text = "TEMPORARY", .kind = TokenKind::kTemp},
    KeywordEntry{.text = "THEN", .kind = TokenKind::kThen},
    KeywordEntry{.text = "TIES", .kind = TokenKind::kTies},
    KeywordEntry{.text = "TO", .kind = TokenKind::kTo},
    KeywordEntry{.text = "TRANSACTION", .kind = TokenKind::kTransaction},
    KeywordEntry{.text = "TRIGGER", .kind = TokenKind::kTrigger},
    KeywordEntry{.text = "UNBOUNDED", .kind = TokenKind::kUnbounded},
    KeywordEntry{.text = "UNION", .kind = TokenKind::kUnion},
    KeywordEntry{.text = "UNIQUE", .kind = TokenKind::kUnique},
    KeywordEntry{.text = "UPDATE", .kind = TokenKind::kUpdate},
    KeywordEntry{.text = "USING", .kind = TokenKind::kUsing},
    KeywordEntry{.text = "VACUUM", .kind = TokenKind::kVacuum},
    KeywordEntry{.text = "VALUES", .kind = TokenKind::kValues},
    KeywordEntry{.text = "VIEW", .kind = TokenKind::kView},
    KeywordEntry{.text = "VIRTUAL", .kind = TokenKind::kVirtual},
    KeywordEntry{.text = "WHEN", .kind = TokenKind::kWhen},
    KeywordEntry{.text = "WHERE", .kind = TokenKind::kWhere},
    KeywordEntry{.text = "WINDOW", .kind = TokenKind::kWindow},
    KeywordEntry{.text = "WITH", .kind = TokenKind::kWith},
    KeywordEntry{.text = "WITHOUT", .kind = TokenKind::kWithout},
};

constexpr std::size_t kKeywordHashSize = 127;

[[nodiscard]] constexpr std::size_t KeywordHash(std::string_view text) noexcept {
  const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(text.front())) | 0x20U;
  const auto last = static_cast<std::uint32_t>(static_cast<unsigned char>(text.back())) | 0x20U;
  return ((first * 4U) ^ (last * 3U) ^ static_cast<std::uint32_t>(text.size())) % kKeywordHashSize;
}

struct KeywordHashTable {
  std::array<std::uint8_t, kKeywordHashSize> heads{};
  std::array<std::uint8_t, kKeywords.size()> next{};
};

[[nodiscard]] consteval KeywordHashTable BuildKeywordHashTable() {
  KeywordHashTable table;
  for (std::size_t index = 0; index < kKeywords.size(); ++index) {
    const std::size_t bucket = KeywordHash(kKeywords[index].text);
    table.next[index] = table.heads[bucket];
    table.heads[bucket] = static_cast<std::uint8_t>(index + 1U);
  }
  return table;
}

inline constexpr auto kKeywordHashTable = BuildKeywordHashTable();

[[nodiscard]] constexpr bool KeywordEquals(std::string_view source,
                                           std::string_view keyword) noexcept {
  if (source.size() != keyword.size()) {
    return false;
  }
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto byte = static_cast<std::uint8_t>(static_cast<unsigned char>(source[index]));
    if (SqliteToUpper(byte) != static_cast<std::uint8_t>(keyword[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] TokenKind LookupKeyword(std::string_view source) noexcept {
  if (source.size() < 2U) {
    return TokenKind::kIdentifier;
  }
  std::uint8_t entry = kKeywordHashTable.heads[KeywordHash(source)];
  while (entry != 0U) {
    const auto index = static_cast<std::size_t>(entry - 1U);
    if (KeywordEquals(source, kKeywords[index].text)) {
      return kKeywords[index].kind;
    }
    entry = kKeywordHashTable.next[index];
  }
  return TokenKind::kIdentifier;
}

[[nodiscard]] Token MakeToken(TokenKind kind, std::size_t begin, std::size_t end) noexcept {
  const auto span = SourceSpan::FromBounds(ByteOffset{begin}, ByteOffset{end});
  assert(span.has_value());
  return Token{.kind = kind, .span = *span};
}

inline constexpr std::array<std::string_view, static_cast<std::size_t>(TokenKind::kWithout) + 1U>
    kTokenKindNames{
        "end_of_input",
        "illegal",
        "whitespace",
        "comment",
        "identifier",
        "string",
        "integer",
        "float",
        "blob",
        "variable",
        "quoted_number",
        "semicolon",
        "left_parenthesis",
        "right_parenthesis",
        "comma",
        "bitwise_and",
        "bitwise_not",
        "plus",
        "minus",
        "asterisk",
        "slash",
        "remainder",
        "concatenate",
        "pointer",
        "equality",
        "less_than_or_equal",
        "not_equal",
        "left_shift",
        "less_than",
        "greater_than_or_equal",
        "right_shift",
        "greater_than",
        "bitwise_or",
        "dot",
        "abort",
        "action",
        "add",
        "after",
        "all",
        "alter",
        "always",
        "analyze",
        "and",
        "as",
        "asc",
        "attach",
        "autoincrement",
        "before",
        "begin",
        "between",
        "by",
        "cascade",
        "case",
        "cast",
        "check",
        "collate",
        "column_keyword",
        "commit",
        "conflict",
        "constraint",
        "create",
        "current",
        "current_time_keyword",
        "database",
        "default",
        "deferrable",
        "deferred",
        "delete",
        "desc",
        "detach",
        "distinct",
        "do",
        "drop",
        "each",
        "else",
        "end",
        "escape",
        "except",
        "exclude",
        "exclusive",
        "exists",
        "explain",
        "fail",
        "filter",
        "first",
        "following",
        "for",
        "foreign",
        "from",
        "generated",
        "group",
        "groups",
        "having",
        "if",
        "ignore",
        "immediate",
        "in",
        "index",
        "indexed",
        "initially",
        "insert",
        "instead",
        "intersect",
        "into",
        "is",
        "is_null",
        "join",
        "join_keyword",
        "key",
        "last",
        "like_keyword",
        "limit",
        "match",
        "materialized",
        "no",
        "not",
        "not_null",
        "nothing",
        "null",
        "nulls",
        "of",
        "offset",
        "on",
        "or",
        "order",
        "others",
        "over",
        "partition",
        "plan",
        "pragma",
        "preceding",
        "primary",
        "query",
        "raise",
        "range",
        "recursive",
        "references",
        "reindex",
        "release",
        "rename",
        "replace",
        "restrict",
        "returning",
        "rollback",
        "row",
        "rows",
        "savepoint",
        "select",
        "set",
        "table",
        "temp",
        "then",
        "ties",
        "to",
        "transaction",
        "trigger",
        "unbounded",
        "union",
        "unique",
        "update",
        "using",
        "vacuum",
        "values",
        "view",
        "virtual",
        "when",
        "where",
        "window",
        "with",
        "without",
};

static_assert(kKeywords.size() == 147U);
static_assert(static_cast<std::size_t>(TokenKind::kWithout) <
              static_cast<std::size_t>(std::numeric_limits<std::uint8_t>::max()));

}  // namespace

Token Lexer::Next() noexcept {
  const std::size_t start = position_.value();
  const std::uint8_t first = ByteAt(start);
  if (first == 0U) {
    return MakeToken(TokenKind::kEndOfInput, start, start);
  }

  const auto emit = [this, start](TokenKind kind, std::size_t end) noexcept {
    position_ = ByteOffset{end};
    return MakeToken(kind, start, end);
  };
  const auto scan_identifier = [this, start, &emit]() noexcept {
    std::size_t end = start + 1U;
    while (IsSqliteIdentifierByte(ByteAt(end))) {
      ++end;
    }
    const std::string_view spelling{source_.data() + start, end - start};
    return emit(LookupKeyword(spelling), end);
  };

  switch (kCharacterClasses[first]) {
    case CharacterClass::kSpace: {
      std::size_t end = start + 1U;
      while (IsSqliteSpace(ByteAt(end))) {
        ++end;
      }
      return emit(TokenKind::kWhitespace, end);
    }
    case CharacterClass::kMinus:
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('-')) {
        std::size_t end = start + 2U;
        while (ByteAt(end) != 0U && ByteAt(end) != static_cast<std::uint8_t>('\n')) {
          ++end;
        }
        return emit(TokenKind::kComment, end);
      }
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('>')) {
        const std::size_t length = ByteAt(start + 2U) == static_cast<std::uint8_t>('>') ? 3U : 2U;
        return emit(TokenKind::kPointer, start + length);
      }
      return emit(TokenKind::kMinus, start + 1U);
    case CharacterClass::kLeftParenthesis:
      return emit(TokenKind::kLeftParenthesis, start + 1U);
    case CharacterClass::kRightParenthesis:
      return emit(TokenKind::kRightParenthesis, start + 1U);
    case CharacterClass::kSemicolon:
      return emit(TokenKind::kSemicolon, start + 1U);
    case CharacterClass::kPlus:
      return emit(TokenKind::kPlus, start + 1U);
    case CharacterClass::kAsterisk:
      return emit(TokenKind::kAsterisk, start + 1U);
    case CharacterClass::kSlash:
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('*') && ByteAt(start + 2U) != 0U) {
        std::size_t end = start + 2U;
        while (ByteAt(end) != 0U && (ByteAt(end) != static_cast<std::uint8_t>('*') ||
                                     ByteAt(end + 1U) != static_cast<std::uint8_t>('/'))) {
          ++end;
        }
        if (ByteAt(end) != 0U) {
          end += 2U;
        }
        return emit(TokenKind::kComment, end);
      }
      return emit(TokenKind::kSlash, start + 1U);
    case CharacterClass::kRemainder:
      return emit(TokenKind::kRemainder, start + 1U);
    case CharacterClass::kEquality:
      return emit(TokenKind::kEquality,
                  start + (ByteAt(start + 1U) == static_cast<std::uint8_t>('=') ? 2U : 1U));
    case CharacterClass::kLessThan:
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('=')) {
        return emit(TokenKind::kLessThanOrEqual, start + 2U);
      }
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('>')) {
        return emit(TokenKind::kNotEqual, start + 2U);
      }
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('<')) {
        return emit(TokenKind::kLeftShift, start + 2U);
      }
      return emit(TokenKind::kLessThan, start + 1U);
    case CharacterClass::kGreaterThan:
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('=')) {
        return emit(TokenKind::kGreaterThanOrEqual, start + 2U);
      }
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('>')) {
        return emit(TokenKind::kRightShift, start + 2U);
      }
      return emit(TokenKind::kGreaterThan, start + 1U);
    case CharacterClass::kBang:
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('=')) {
        return emit(TokenKind::kNotEqual, start + 2U);
      }
      return emit(TokenKind::kIllegal, start + 1U);
    case CharacterClass::kPipe:
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('|')) {
        return emit(TokenKind::kConcatenate, start + 2U);
      }
      return emit(TokenKind::kBitwiseOr, start + 1U);
    case CharacterClass::kComma:
      return emit(TokenKind::kComma, start + 1U);
    case CharacterClass::kBitwiseAnd:
      return emit(TokenKind::kBitwiseAnd, start + 1U);
    case CharacterClass::kBitwiseNot:
      return emit(TokenKind::kBitwiseNot, start + 1U);
    case CharacterClass::kDot:
      if (!IsSqliteDigit(ByteAt(start + 1U))) {
        return emit(TokenKind::kDot, start + 1U);
      }
      [[fallthrough]];
    case CharacterClass::kDigit: {
      TokenKind kind = TokenKind::kInteger;
      std::size_t end = start;
      if (first == static_cast<std::uint8_t>('0') &&
          (ByteAt(start + 1U) == static_cast<std::uint8_t>('x') ||
           ByteAt(start + 1U) == static_cast<std::uint8_t>('X')) &&
          IsSqliteHexDigit(ByteAt(start + 2U))) {
        end = start + 3U;
        while (true) {
          if (IsSqliteHexDigit(ByteAt(end))) {
            ++end;
          } else if (ByteAt(end) == static_cast<std::uint8_t>('_')) {
            kind = TokenKind::kQuotedNumber;
            ++end;
          } else {
            break;
          }
        }
      } else {
        while (true) {
          if (IsSqliteDigit(ByteAt(end))) {
            ++end;
          } else if (ByteAt(end) == static_cast<std::uint8_t>('_')) {
            kind = TokenKind::kQuotedNumber;
            ++end;
          } else {
            break;
          }
        }
        if (ByteAt(end) == static_cast<std::uint8_t>('.')) {
          if (kind == TokenKind::kInteger) {
            kind = TokenKind::kFloat;
          }
          ++end;
          while (true) {
            if (IsSqliteDigit(ByteAt(end))) {
              ++end;
            } else if (ByteAt(end) == static_cast<std::uint8_t>('_')) {
              kind = TokenKind::kQuotedNumber;
              ++end;
            } else {
              break;
            }
          }
        }
        if ((ByteAt(end) == static_cast<std::uint8_t>('e') ||
             ByteAt(end) == static_cast<std::uint8_t>('E')) &&
            (IsSqliteDigit(ByteAt(end + 1U)) ||
             ((ByteAt(end + 1U) == static_cast<std::uint8_t>('+') ||
               ByteAt(end + 1U) == static_cast<std::uint8_t>('-')) &&
              IsSqliteDigit(ByteAt(end + 2U))))) {
          if (kind == TokenKind::kInteger) {
            kind = TokenKind::kFloat;
          }
          end += 2U;
          while (true) {
            if (IsSqliteDigit(ByteAt(end))) {
              ++end;
            } else if (ByteAt(end) == static_cast<std::uint8_t>('_')) {
              kind = TokenKind::kQuotedNumber;
              ++end;
            } else {
              break;
            }
          }
        }
      }
      while (IsSqliteIdentifierByte(ByteAt(end))) {
        kind = TokenKind::kIllegal;
        ++end;
      }
      return emit(kind, end);
    }
    case CharacterClass::kQuoteString:
    case CharacterClass::kQuoteIdentifier: {
      const bool bracket = first == static_cast<std::uint8_t>('[');
      const std::uint8_t delimiter = bracket ? static_cast<std::uint8_t>(']') : first;
      std::size_t end = start + 1U;
      while (ByteAt(end) != 0U) {
        if (ByteAt(end) == delimiter) {
          ++end;
          if (!bracket && ByteAt(end) == delimiter) {
            ++end;
            continue;
          }
          const TokenKind kind = kCharacterClasses[first] == CharacterClass::kQuoteString
                                     ? TokenKind::kString
                                     : TokenKind::kIdentifier;
          return emit(kind, end);
        }
        ++end;
      }
      return emit(TokenKind::kIllegal, end);
    }
    case CharacterClass::kVariableNumeric: {
      std::size_t end = start + 1U;
      while (IsSqliteDigit(ByteAt(end))) {
        ++end;
      }
      return emit(TokenKind::kVariable, end);
    }
    case CharacterClass::kVariableAlpha: {
      std::size_t end = start + 1U;
      std::size_t identifier_bytes = 0;
      TokenKind kind = TokenKind::kVariable;
      while (ByteAt(end) != 0U) {
        const std::uint8_t byte = ByteAt(end);
        if (IsSqliteIdentifierByte(byte)) {
          ++identifier_bytes;
          ++end;
        } else if (byte == static_cast<std::uint8_t>('(') && identifier_bytes > 0U) {
          do {
            ++end;
          } while (ByteAt(end) != 0U && !IsSqliteSpace(ByteAt(end)) &&
                   ByteAt(end) != static_cast<std::uint8_t>(')'));
          if (ByteAt(end) == static_cast<std::uint8_t>(')')) {
            ++end;
          } else {
            kind = TokenKind::kIllegal;
          }
          break;
        } else if (byte == static_cast<std::uint8_t>(':') &&
                   ByteAt(end + 1U) == static_cast<std::uint8_t>(':')) {
          end += 2U;
        } else {
          break;
        }
      }
      if (identifier_bytes == 0U) {
        kind = TokenKind::kIllegal;
      }
      return emit(kind, end);
    }
    case CharacterClass::kX:
      if (ByteAt(start + 1U) == static_cast<std::uint8_t>('\'')) {
        std::size_t end = start + 2U;
        while (IsSqliteHexDigit(ByteAt(end))) {
          ++end;
        }
        TokenKind kind = TokenKind::kBlob;
        if (ByteAt(end) != static_cast<std::uint8_t>('\'') || ((end - start) % 2U) != 0U) {
          kind = TokenKind::kIllegal;
          while (ByteAt(end) != 0U && ByteAt(end) != static_cast<std::uint8_t>('\'')) {
            ++end;
          }
        }
        if (ByteAt(end) != 0U) {
          ++end;
        }
        return emit(kind, end);
      }
      return scan_identifier();
    case CharacterClass::kBom:
      if (ByteAt(start + 1U) == 0xbbU && ByteAt(start + 2U) == 0xbfU) {
        return emit(TokenKind::kWhitespace, start + 3U);
      }
      return scan_identifier();
    case CharacterClass::kKeyword:
    case CharacterClass::kIdentifier:
      return scan_identifier();
    case CharacterClass::kIllegal:
      return emit(TokenKind::kIllegal, start + 1U);
  }

  return emit(TokenKind::kIllegal, start + 1U);
}

std::string_view TokenKindName(TokenKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= kTokenKindNames.size()) {
    return "unknown";
  }
  return kTokenKindNames[index];
}

}  // namespace modern_sqlite
