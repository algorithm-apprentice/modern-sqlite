# ADR-0026: SQLite-Compatible SQL Lexer

- Status: Accepted
- Date: 2026-10-04

## Context

The parser, catalog loader, statement preparation path, and later diagnostics
need a shared SQL token stream with exact source locations. SQLite tokenizes
SQL as bytes rather than Unicode code points, preserves malformed UTF-8 inside
identifiers, and treats malformed lexical forms as tokens instead of throwing
or allocating diagnostic objects.

Pinned SQLite 3.54.0 defines the relevant behavior in:

- `src/tokenize.c:12-118`, which defines the 256-entry first-byte dispatch
  table and its deliberate differences from general character classes.
- `src/tokenize.c:190-265`, which performs parser-loop lookaround for the
  context-sensitive `WINDOW`, `OVER`, and `FILTER` keywords.
- `src/tokenize.c:273-594`, which implements `sqlite3GetToken`.
- `src/tokenize.c:600-709`, which filters trivia, synthesizes a final
  semicolon, applies contextual keyword rewriting, and handles illegal tokens
  before invoking the Lemon parser.
- `src/parse.y:263-320`, which defines parser fallback behavior separately
  from lexical classification.
- `src/parse.y:2128-2169`, which declares scanner-only and synthetic token
  kinds.
- `tool/mkkeywordhash.c`, which defines the feature-gated keyword vocabulary
  and generates the compact keyword hash used by the pinned build.

Several behaviors are easy to lose in a conventional C++ lexer:

- High-bit bytes are identifier bytes even when they are not valid UTF-8.
- A UTF-8 BOM is whitespace, including away from offset zero.
- The first-byte dispatch table rejects a vertical tab that SQLite's
  subsequent whitespace loop can consume after another whitespace byte.
- An exact `/*` at end of input is slash followed by asterisk, while a longer
  unterminated block comment is a comment through end of input.
- Numeric separators produce SQLite's `QNUMBER` token, while an identifier
  byte following any numeric form makes the complete sequence illegal.
- `x'...'` blob literals, Tcl-style `$name::part(suffix)` variables, quoted
  identifiers, and malformed delimiters have SQLite-specific boundaries.
- The C scanner uses NUL as its sentinel even when a caller supplied a bounded
  byte count.

The lexer node must preserve those observable boundaries without importing
SQLite's Lemon token numbers or coupling the pure scanner to parser policy.

## Decision

Add a pure lexer under `modern_sqlite/syntax/lexer.hpp`.

### Public contract

The public surface is:

```cpp
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
  // Punctuation, operators, grouped keyword classes, and keyword tokens.
};

struct Token {
  TokenKind kind = TokenKind::kEndOfInput;
  SourceSpan span;

  constexpr auto operator<=>(const Token&) const noexcept = default;
};

class Lexer final {
 public:
  explicit constexpr Lexer(Utf8View source) noexcept;

  [[nodiscard]] Token Next() noexcept;
  [[nodiscard]] constexpr ByteOffset position() const noexcept;
  [[nodiscard]] constexpr bool finished() const noexcept;
};

[[nodiscard]] constexpr bool IsTrivia(TokenKind kind) noexcept;
[[nodiscard]] constexpr bool IsKeyword(TokenKind kind) noexcept;
[[nodiscard]] std::string_view TokenKindName(TokenKind kind) noexcept;
```

`Lexer` borrows its `Utf8View`; the source must outlive the lexer. Tokens retain
only half-open byte spans, never pointers or owning strings. Callers recover
the exact lexeme with the existing checked `Slice` operation.

`Next` is total and allocation-free. Lexical defects are `kIllegal` tokens, not
`Result` failures, because scanning bounded resident memory has no operational
failure path. Every non-end token consumes at least one byte. Repeated calls
after logical end return the same zero-length `kEndOfInput` token.
`position()` is the first unconsumed byte offset. `finished()` is true whenever
that offset is a physical end or NUL, including immediately after constructing
a lexer over empty input.

Stable token names are lowercase snake case without the `k` prefix. They are
intended for tests and diagnostics, but SQLite's numeric Lemon token values
are explicitly not part of the compatibility contract. An invalid
`TokenKind` underlying value maps to `unknown` rather than causing undefined
behavior in diagnostics.

The vocabulary has one enumerator for each raw scanner kind. Ordinary keyword
enumerators are the Pascal-case keyword spelling with a `k` prefix and
underscores removed, such as `kSavepoint`, `kNotNull`, and `kCurrent`.
Exceptions are the shared or clearer kinds `kAutoincrement`,
`kColumnKeyword`, `kCurrentTimeKeyword`, `kJoinKeyword`, `kLikeKeyword`, and
`kTemp`. The non-keyword operator vocabulary is:

```text
kSemicolon, kLeftParenthesis, kRightParenthesis, kComma,
kBitwiseAnd, kBitwiseNot, kPlus, kMinus, kAsterisk, kSlash,
kRemainder, kConcatenate, kPointer, kEquality, kLessThanOrEqual,
kNotEqual, kLeftShift, kLessThan, kGreaterThanOrEqual, kRightShift,
kGreaterThan, kBitwiseOr, kDot
```

### Raw token stream

The lexer exposes the raw scanner stream:

- Whitespace and comments are returned as distinct trivia tokens.
- `WINDOW`, `OVER`, and `FILTER` are returned as keyword tokens whenever their
  spelling matches.
- The parser layer will skip trivia and apply SQLite's surrounding-token rules
  when those three keywords need reclassification as identifiers.
- Lemon fallback-to-identifier behavior remains parser policy.
- End-of-input semicolon synthesis remains parser-driver policy.
- `TRUE` and `FALSE` remain identifiers, matching the scanner; later
  expression construction may recognize them as literals.

Keeping those policies out of the lexer makes one pass deterministic from
bytes alone and lets future parser tests exercise the contextual rules
directly.

### Bounded input and NUL

The source is an explicit byte view, but the first embedded NUL is the logical
end of SQL, matching SQLite's parser-facing behavior. At a NUL or the physical
end of the view, `Next` returns `kEndOfInput` with an empty span at that
offset. Bytes after the first NUL are not tokenized, and `position()` remains
at the NUL so a statement API can report the unconsumed suffix.

Internal lookahead is bounds-safe and treats reads at or beyond the physical
end as the SQLite NUL sentinel. No scanner path reads beyond the supplied
view.

### Compatibility profile

The lexer matches the pinned build's default feature set:

- Blob literals and Tcl-compatible variable suffixes are enabled.
- Window-function keywords are enabled.
- All 147 active keyword spellings generated from `mkkeywordhash.c` are
  recognized case-insensitively.
- Feature aliases retain one token kind:
  - `TEMP` and `TEMPORARY`;
  - `CROSS`, `FULL`, `INNER`, `LEFT`, `NATURAL`, `OUTER`, and `RIGHT`;
  - `GLOB`, `LIKE`, and `REGEXP`;
  - `CURRENT_DATE`, `CURRENT_TIME`, and `CURRENT_TIMESTAMP`.
- `AUTOINCREMENT` and `COLUMN` use dedicated descriptive token kinds rather
  than SQLite's abbreviated internal names.
- `=` and `==` share `kEquality`, `!=` and `<>` share `kNotEqual`, and `->`
  and `->>` share `kPointer`, matching the raw scanner.
- `WITHIN` remains an identifier because the pinned build does not enable
  `SQLITE_ENABLE_ORDERED_SET_AGGREGATES`.

The scanner preserves raw source spelling. It does not dequote strings or
identifiers, remove numeric separators, parse numbers, decode blobs, normalize
case, validate UTF-8, or construct runtime values.

### Malformed input

Token boundaries match `sqlite3GetToken`:

- Unknown ASCII bytes and a lone `!` are one-byte illegal tokens.
- Unterminated strings and quoted identifiers are illegal through logical end.
- Unterminated block comments consume through logical end except for the exact
  two-byte `/*` boundary behavior defined by SQLite.
- Malformed or odd-length blob literals are one illegal token.
- A numeric literal followed by any SQLite identifier byte is one illegal
  token.
- Numeric forms containing `_` use `kQuotedNumber`, corresponding to SQLite's
  `TK_QNUMBER`; later syntax processing removes separators and determines
  integer versus floating-point semantics.
- A variable prefix without a valid name is illegal with SQLite's exact
  consumed length.

Illegal tokens do not stop the lexer. The parser may stop at the first one,
while standalone tools and tests may continue to inspect later bytes.

### Lookup and hot-path design

The implementation uses:

- A compile-time 256-entry first-byte dispatch table matching
  `tokenize.c`, separate from the general SQLite character-class helpers.
- The compact hash topology generated by pinned `mkkeywordhash.c`, with modern
  `TokenKind` values replacing Lemon codes.
- Integer byte offsets and bounded sentinel-aware lookahead.
- A single forward pass with no source copies, UTF-8 decoding, locale calls,
  heap allocation, exceptions, or virtual dispatch.

The generated keyword data remains private to the implementation. Tests
independently enumerate every active spelling and alias so a table update
cannot silently change the public classification.

### Verification

Development follows red-green-refactor.

Before production implementation, tests will define:

- Every punctuation and operator boundary.
- Whitespace, BOM, line comments, block comments, and their end boundaries.
- Quoted strings and identifiers, doubled delimiters, and unterminated forms.
- Decimal, hexadecimal, fractional, exponent, separator, and illegal numeric
  forms.
- Blob literals and all variable syntaxes.
- All 147 default keyword spellings, representative ASCII case variants,
  aliases, `TRUE`, `FALSE`, and `WITHIN`.
- High-bit and malformed UTF-8 identifier bytes.
- Embedded NUL behavior, repeated end tokens, exact spans, and contiguous byte
  coverage up to logical end.
- A deterministic corpus whose expected kinds and lengths are generated by an
  authenticated SQLite 3.54.0 `sqlite3GetToken` oracle.
- A dedicated allocation test proving repeated iteration performs no dynamic
  allocation after fixture setup.

The checked-in differential fixture records the SQLite version, source
identity, build options, corpus encoding, and generation command.

Because this DAG node is performance-sensitive, a Release fixed-work
benchmark scans the same deterministic corpus with Modern SQLite and pinned
`sqlite3GetToken`. It verifies token count, consumed bytes, and a token-kind
checksum outside the timed loop, and reports median nanoseconds per byte and
throughput. A slowdown greater than 1.5 times the pinned scanner requires
profiling and correction or an explicit reviewed exception before merge.

### Deferred work

This node does not implement:

- Contextual window-keyword rewriting.
- Lemon fallback rules or a parser token adapter.
- Final semicolon synthesis or multi-statement tail policy beyond exposing the
  current byte position.
- Literal decoding, identifier dequoting, numeric conversion, or AST nodes.
- Parser diagnostics, error recovery, or syntax limits.
- Non-default SQLite keyword feature sets.

Those behaviors belong to the parser, AST, or later compatibility nodes.

## Consequences

- Parser code receives a stable, precise, byte-preserving token vocabulary
  without depending on SQLite's generated numeric IDs.
- Trivia-preserving tools can use the same stream as a trivia-skipping parser.
- Malformed UTF-8 and malformed SQL cannot trigger out-of-bounds reads or
  allocation failures in the lexer.
- Matching SQLite's NUL sentinel means suffix bytes after an embedded NUL are
  intentionally outside the SQL token stream.
- The keyword vocabulary and scanner quirks are pinned and testable, while
  parser-only context remains in the correct dependency layer.
- Supporting a different SQLite compile-time feature profile requires an
  explicit compatibility decision and regenerated differential evidence.
