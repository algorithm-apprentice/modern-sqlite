# ADR-0028: Pure SQLite-Compatible SQL Parser

- Status: Accepted
- Date: 2026-10-04

## Context

The catalog loader, binder, statement preparation path, and later diagnostics
need a deterministic parser over the source-preserving lexer and immutable
syntax tree. The first parser increment must cover the syntax already modeled
by ADR-0027 without importing catalog lookup, semantic resolution, query
planning, bytecode generation, or mutable SQLite parser state.

Pinned SQLite 3.54.0 defines the relevant behavior in:

- `src/parse.y:1-170`, which configures Lemon, disables error recovery, and
  defines `near "<token>": syntax error` and `incomplete input` failures.
- `src/parse.y:268-340`, which defines fallback keywords, precedence,
  `id`/`ids`/`idj`, and `nm`.
- `src/parse.y:396-478`, which defines supported `CREATE TABLE` forms.
- `src/parse.y:600-979`, which defines `SELECT`, aliases, table sources, and
  normalized `LIMIT` forms.
- `src/parse.y:1070-1525`, which defines expressions and their precedence.
- `src/parse.y:1622-1637`, which defines `CREATE INDEX`.
- `src/tokenize.c:216-265`, which contextually classifies `WINDOW`, `OVER`,
  and `FILTER`.
- `src/tokenize.c:600-744`, which skips trivia, synthesizes an end semicolon,
  stops at the first prepared statement, and records the tail.
- `src/sqliteLimit.h:92-118`, which sets default expression and parser depth
  limits to 1000 and 2500.

The existing syntax tree intentionally represents only:

- simple `SELECT`;
- `CREATE TABLE` with the modeled column and table constraints; and
- `CREATE INDEX`, including expression and partial indexes.

SQLite's complete grammar is much larger. Silently accepting a prefix of a
join, compound query, common-table expression, generated column, foreign key,
subquery, window expression, or other unmodeled construct would create an
incorrect tree. Those forms must fail explicitly until their AST support is
designed.

The parser is a performance-sensitive DAG node. It must preserve SQLite's
token precedence and prepare-style tail behavior while avoiding Lemon tables,
SQLite C structures, recursive AST ownership, and runtime-value decoding.

## Decision

Add a pure parser under `modern_sqlite/syntax/parser.hpp`.

### Public contract

The public surface is:

```cpp
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
  SourceSpan span;
  TokenKind actual = TokenKind::kEndOfInput;
  ParseExpectation expected = ParseExpectation::kNone;
  ByteOffset next_offset;
};

struct ParseOutput {
  std::optional<SyntaxTree> tree;
  ByteOffset next_offset;
};

using ParseResult = std::expected<ParseOutput, ParseError>;

inline constexpr std::size_t kMaximumExpressionConstructionDepth = 1000;
inline constexpr std::size_t kMaximumParserRecursionDepth = 512;

[[nodiscard]] ParseResult ParseOne(Utf8View source);
[[nodiscard]] constexpr std::string_view ParseErrorCodeName(
    ParseErrorCode code) noexcept;
[[nodiscard]] constexpr std::string_view ParseExpectationName(
    ParseExpectation expectation) noexcept;
[[nodiscard]] constexpr std::string_view ParseErrorMessage(
    const ParseError& error) noexcept;
```

`ParseOutput` is move-only because a contained `SyntaxTree` is move-only.
`ParseOne` borrows the input only for the duration of the call. On success,
the tree owns its exact statement source and all syntax references are
source-relative spans into that owned string.

Parse errors are allocation-free values. Their spans are relative to the
caller's complete input, unlike successful AST spans, which are relative to
the owned statement slice. The caller may recover the exact offending bytes
from the input. `next_offset` is always an absolute byte offset and remains
available when parsing fails, matching the shape of SQLite's prepare API.
`ParseErrorMessage` returns stable category text:

- illegal token: `unrecognized token`;
- unexpected end: `incomplete input`;
- other unexpected token: `syntax error`;
- unsupported syntax: `unsupported syntax`;
- resource limits: `expression depth exceeded` or `parser depth exceeded`;
- invariant failure: `internal parser invariant failed`.

The typed code, expectation, actual token, and span are the compatibility
contract. They are normalized as follows:

| Error | `span` and `actual` | `expected` | `next_offset` |
|---|---|---|---|
| Illegal token | Raw illegal token | `kNone` | Start of the illegal token, because SQLite does not advance over it |
| Unexpected token | Effective parser token | Exact grammar expectation from the table below | End of a non-end token |
| Unexpected end | Empty end span and `kEndOfInput` | Exact grammar expectation from the table below | Logical end |
| Unsupported syntax | First effective token that proves the unsupported production | `kNone` | End of that token |
| Expression depth | Operator, function name, qualified-name start, wildcard qualifier, or `COLLATE` token that creates the rejected depth | `kNone` | Furthest consumed offset |
| Parser depth | Current effective token, not yet consumed | `kExpression` | Start of that token |
| Internal invariant | Terminator token, or logical end | `kNone` | Already determined statement tail |

Unexpected-token expectations are selected only from these grammar states:

| Grammar state | `expected` |
|---|---|
| First substantive token is not a supported or recognized unsupported statement start | `kStatement` |
| Expression prefix/primary is required | `kExpression` |
| A schema, object, column, alias, collation, or constraint name is required | `kName` |
| A production has consumed `(` and requires its matching `)` | `kRightParenthesis` |
| A nonempty comma-separated list has completed an item | `kCommaOrRightParenthesis` |
| A supported statement is complete but another significant token remains | `kEndOfStatement` |
| `CREATE TABLE (` or a column comma requires another column before table constraints begin | `kColumnDefinition` |
| `CONSTRAINT name` or an incomplete supported constraint requires a constraint production | `kConstraint` |
| A table-option comma or `WITHOUT` requires a valid option | `kTableOption` |

When more than one description could apply, the innermost active production
wins. For example, an invalid function argument start uses `kExpression`; a
missing function `)` after a valid final argument uses
`kCommaOrRightParenthesis`; and end-of-input while an already recognized
parenthesized expression still needs its delimiter uses
`kRightParenthesis`.

For example, `SELECT x ORDER BY y` reports `ORDER`, `kUnsupportedSyntax`,
`kNone`, and the byte after `ORDER`. A contextual `OVER` that remains a
keyword reports `kOver`; one reclassified as a name reports `kIdentifier`.
SQLite's rendered `near "<token>"` text is presentation policy for a later API
boundary.

Expected syntax and compatibility failures use `std::expected`; exceptions
are not normal parser control flow. Allocation failure continues to propagate
to the explicit statement-preparation API boundary required by ADR-0004.
An AST validation failure after successful parsing is a parser bug and maps
to `kInternalInvariant`, never to a success-shaped empty result.

### Prepare-style statement and tail behavior

One call behaves like one `sqlite3_prepare_v3` iteration, including a tail
offset on both success and failure:

1. Skip leading whitespace, comments, and any number of empty semicolon
   statements.
2. If logical end is reached, return no tree and `next_offset` at that end.
3. Parse exactly one substantive statement.
4. If a terminating semicolon exists, include it in the tree source and set
   `next_offset` immediately after it.
5. Without a semicolon, synthesize end-of-statement logically, exclude
   trailing trivia from the tree source, and set `next_offset` at logical end
   after that trivia.
6. Never tokenize or validate the next statement after a real terminating
   semicolon.

The first embedded NUL remains the logical end defined by ADR-0026. Bytes
after it are not parsed, and `next_offset` points at the NUL. Offsets are
absolute byte offsets into the caller's input.

The owned tree source:

- starts at the first substantive statement token;
- excludes leading trivia and empty statements;
- includes internal trivia and an optional terminating semicolon;
- excludes trailing trivia after an unterminated statement; and
- gives the root statement a span from offset zero through the final grammar
  token, excluding the semicolon.

The implementation first locates the substantive statement start, then
restarts the borrowing lexer over that suffix. This keeps every constructed
AST span statement-local without a rebasing pass. Only the accepted source
prefix is copied when `SyntaxTree::Create` is called.

### Token adapter and names

The parser skips both lexer trivia kinds. It stops at the first
`TokenKind::kIllegal` and reports `kIllegalToken`.

SQLite's contextual window keywords are applied before grammar decisions:

- `WINDOW` remains a keyword only when followed by a token accepted by the
  dedicated SQLite lookahead predicate below and then `AS`;
- `OVER` remains a keyword only after `)` and before `(` or an
  accepted lookahead token;
- `FILTER` remains a keyword only after `)` and before `(`;
- otherwise each is treated as an identifier.

The adapter may inspect future significant tokens by copying the trivially
copyable lexer cursor. It does not allocate or consume the parser's cursor.

Lemon fallback is not a global rewrite. A token is accepted as an identifier
only where the grammar requests an identifier. The parser uses the pinned
fallback list from `parse.y` and four independent token classes:

- `id`: identifier, `INDEXED`, or a fallback token;
- `ids`: identifier, string, or a fallback token, but not `INDEXED`;
- `idj`: identifier, `INDEXED`, grouped join keyword, or a fallback token;
- `nm`: `idj` or string.

The lookahead predicate used only by contextual `WINDOW` and `OVER` exactly
matches SQLite's private `getToken`: raw identifier, string, grouped join
keyword, raw `WINDOW`, raw `OVER`, or a fallback token. It intentionally
excludes `INDEXED` and `FILTER`.

This preserves cases where `LIKE`, `MATCH`, `CURRENT_DATE`, and other fallback
keywords are names in name positions but operators or literals in expression
positions. Qualified expression names contain one to three `nm` parts.
Function names contain exactly one `idj` part. Qualified wildcards contain
exactly one `nm` qualifier. The parser preserves raw quoting and case; it
never dequotes or normalizes names.

Expression-production priority is independent from fallback eligibility.
Current-time keywords are parsed as literals before an identifier production,
so forms such as `CURRENT_DATE()` and `CURRENT_DATE.x` are syntax errors.
`CAST` and `RAISE` likewise enter their dedicated SQLite productions before
fallback-name handling: bare uses are syntax errors, while their complete
currently unmodeled forms report `kUnsupportedSyntax`.

### Parsing strategy and limits

Statements and DDL productions use hand-written recursive descent.
Expressions use a Pratt parser that appends nodes directly to the existing
postorder expression arena. Child IDs are therefore always smaller than their
parent IDs, and no mutable pointer graph or later arena reorder is needed.

The parser retains a parallel temporary construction-depth vector. This is a
Modern SQLite resource contract with a limit of 1000, informed by SQLite's
default expression limit but not falsely presented as the height of SQLite's
different internal `Expr` graph. Leaf depth is one. Unary, binary, function,
and collation nodes add one to the maximum child depth. Parentheses preserve
depth. Qualified identifiers account for each SQLite `TK_DOT` level,
qualified wildcards account for their dot level, and negated LIKE-family
operators account for their additional logical-NOT level. Depth 1000 is
accepted and depth 1001 reports `kExpressionDepthExceeded`.

This policy intentionally does not reproduce SQLite's constant-folding
shortcuts or every internal height quirk. It gives the source-preserving AST a
deterministic linear resource bound while remaining close to the pinned
grammar's construction cost.

Recursive expression entry is guarded independently at 512 active calls.
This deliberately independent conservative limit exceeds SQLite's historical
fixed 100-entry parser stack without claiming equivalence to the current
2500-entry Lemon-stack limit. Entering level 513 reports
`kParserDepthExceeded`. The boundary is validated under ASan before merge.
AST ownership and destruction remain nonrecursive. A future iterative parser
may raise this limit without changing the AST.

The Pratt binding order, from lowest to highest, is exactly:

1. `OR`
2. `AND`
3. prefix `NOT`
4. `IS`, `IS NOT`, `MATCH`, `LIKE`, `GLOB`, `REGEXP`, equality, and
   inequality
5. `<`, `<=`, `>`, and `>=`
6. `&`, `|`, `<<`, and `>>`
7. `+` and `-`
8. `*`, `/`, and `%`
9. `||`
10. postfix `COLLATE`
11. prefix `~`, unary `+`, and unary `-`

Binary operators are left-associative. Prefix operators are right-associative.
`NOT LIKE`, `NOT GLOB`, `NOT REGEXP`, and `NOT MATCH` are compound infix
operators at the comparison level. The unsupported `ESCAPE`, `BETWEEN`, `IN`,
`ISNULL`, `NOTNULL`, `IS [NOT] DISTINCT FROM`, and JSON pointer productions
are detected rather than partially consumed.

### Expression surface

The parser constructs every expression variant accepted by ADR-0027:

- raw NULL, integer, real, quoted-number, string, blob, and current-time
  literals, plus DEFAULT-specific TRUE and FALSE literals;
- variables;
- one- to three-part identifiers;
- unqualified and one-part-qualified wildcards;
- unary and supported binary operators;
- zero-or-more-argument function calls, including `DISTINCT` and `*`;
- postfix `COLLATE`; and
- explicit parenthesized expressions.

Quoted-number spelling remains raw. A `0x` hexadecimal spelling remains an
integer regardless of hexadecimal digits such as `E`; otherwise its literal
kind is integer unless the separator-free spelling contains a decimal point
or exponent marker.
General unquoted `TRUE` and `FALSE` expressions remain
`IdentifierExpression` nodes. SQLite first resolves them as names and only
converts an unresolved name to a boolean value, so that conversion belongs to
the binder. Current-time spellings retain their three distinct literal kinds.

Function `ALL` is accepted as the default argument mode because SQLite
accepts it and the AST needs no semantic distinction from omission.
Function `DISTINCT` sets the existing boolean. Function-local `ORDER BY`,
`FILTER`, `OVER`, ordered-set syntax, subqueries, vector expressions, `CASE`,
`CAST`, `RAISE`, and all other unmodeled expressions are unsupported.
SQLite's separate bare-asterisk function production remains separate:
`f(*)` is accepted, while `f(ALL *)` and `f(DISTINCT *)` are syntax errors.

### SELECT surface

The supported form is:

```text
SELECT [ALL | DISTINCT] result-column [, ...]
  [FROM qualified-table-name [AS] alias]
  [WHERE expression]
  [LIMIT expression
    | LIMIT expression OFFSET expression
    | LIMIT offset-expression , limit-expression]
```

A result column is an expression with an optional explicit or bare
SQLite-compatible alias, or the complete terminal production `*` or
`name.*`. Wildcard result columns cannot take aliases or participate in
operators. The table source is zero or one named table with an optional
alias. Comma sources, joins, table-valued functions, subqueries, and
`INDEXED BY` are unsupported.

`GROUP BY`, `HAVING`, `WINDOW`, `VALUES`, compounds, CTEs, `ORDER BY`, and
other absent clauses report `kUnsupportedSyntax`.

LIMIT is normalized into the AST's semantic `limit` and optional `offset`
slots while retaining `LimitSyntax`. The source order remains exact:

- `LIMIT n` stores `n`;
- `LIMIT n OFFSET m` stores limit `n`, offset `m`; and
- `LIMIT m, n` stores limit `n`, offset `m`.

### CREATE TABLE surface

The parser accepts:

```text
CREATE [TEMP | TEMPORARY] TABLE [IF NOT EXISTS]
  [schema.]table (
    column-definition [, ...]
    [, table-constraint ...]
  )
  [,] [WITHOUT ROWID] [, STRICT]
```

The actual SQLite option grammar is followed: the first option may appear
directly or after one comma, and multiple options are comma-separated.

Column type names preserve one exact raw span. They accept SQLite `ids`
sequences and optional signed one- or two-number parenthesized suffixes.
The parser does not derive affinity.

The supported column constraints are:

- optional `CONSTRAINT name` prefixes;
- `NULL` and `NOT NULL`, with optional conflict clauses;
- `PRIMARY KEY`, sort order, conflict action, and `AUTOINCREMENT`;
- `UNIQUE`, with optional conflict action;
- `CHECK (expression)`;
- SQLite-shaped `DEFAULT` terms, signed terms, bare identifiers, or
  parenthesized expressions;
  and
- `COLLATE name`.

The DEFAULT mapping is production-specific:

- a raw literal or current-time `term` keeps its literal kind;
- unary `+` or `-` wraps exactly one `term`;
- `DEFAULT id` produces a string literal node over the raw identifier token,
  except unquoted `TRUE` and `FALSE`, which produce boolean literal nodes; and
- `DEFAULT (expr)` uses the general expression grammar and retains the
  explicit parenthesized node.

Bare DEFAULT identifiers therefore never become general
`IdentifierExpression` nodes.

The supported table constraints are:

- optional `CONSTRAINT name` prefixes;
- `PRIMARY KEY (indexed-term, ...)`, optional conflict action, and
  `AUTOINCREMENT`;
- `UNIQUE (indexed-term, ...)`, with optional conflict action; and
- `CHECK (expression)`, including SQLite's syntactically accepted conflict
  clause even though it has no AST semantic field.

Conflict actions are `ROLLBACK`, `ABORT`, `FAIL`, `IGNORE`, and `REPLACE`.
Indexed terms retain a trailing collation and sort order separately from the
expression. `CREATE TABLE ... AS SELECT`, generated columns, references,
foreign keys, and unmodeled constraints report `kUnsupportedSyntax`.
Only the final outer `COLLATE` is extracted into the indexed-term field;
nested collations remain expression nodes, preserving cases such as
`a COLLATE nocase + b` and repeated collations.

### CREATE INDEX surface

The parser accepts:

```text
CREATE [UNIQUE] INDEX [IF NOT EXISTS] [schema.]index
  ON table (indexed-term [, ...]) [WHERE expression]
```

The target table is intentionally unqualified, matching pinned SQLite
grammar. Each indexed term contains an expression plus optional trailing
`COLLATE` and `ASC`/`DESC`. A partial-index `WHERE` expression is preserved.

### Full-consumption and unsupported-syntax policy

Success requires consumption through a real semicolon or logical end. Any
significant token remaining after a supported statement is an error; no
prefix tree is returned.

Recognizable SQLite constructs that the current AST cannot represent use
`kUnsupportedSyntax`. Malformed syntax inside the supported grammar uses
`kUnexpectedToken` with the normalized expectation above. This distinction is
deterministic but is not a promise to recognize every valid future SQLite
production before that production is implemented. No unsupported construct is
rewritten into a different supported meaning.

There is no error recovery. The first error ends the call, matching SQLite's
parser configuration.

### Allocation and hot-path design

The implementation uses:

- the existing allocation-free lexer;
- a fixed-size significant-token lookahead buffer;
- direct postorder arena construction;
- one temporary expression-height byte or integer per expression;
- move-only statement and syntax-tree assembly; and
- one final source copy for a successful statement.

It does not build a token vector, concrete syntax tree, parser table, decoded
literal object, or diagnostic string. Lookahead never scans past a terminating
semicolon into the next statement.

### Verification

Development follows red-green-refactor. Before production implementation,
tests define:

- empty input, trivia, empty semicolons, multiple statements, absent
  semicolons, comments, embedded NUL, exact source ownership, and absolute
  tail offsets;
- every supported expression node, SQLite precedence and associativity,
  keyword fallback, contextual window keywords, aliases, and exact spans;
- simple SELECT, both LIMIT spellings, CREATE TABLE constraints and options,
  CREATE INDEX terms, and partial indexes;
- raw literal preservation, quoted identifiers, strings used as `nm`, and
  malformed lexer tokens;
- deterministic unexpected, incomplete, unsupported, limit, and invariant
  diagnostics;
- construction depth 1000/1001 and recursive parser depth 512/513
  boundaries;
- explicit rejection of every major unmodeled production named above;
- source-layer dependency isolation; and
- differential acceptance fixtures checked against pinned SQLite 3.54.0 for
  supported valid and invalid statements.

The checked fixture records the pinned Fossil and Git identities, the
generated `sqlite3.c` SHA-256, and the exact build-and-probe command used to
classify each `sqlite3_prepare_v3` outcome.

Because this DAG node is performance-sensitive, a Release fixed-work
benchmark parses and destroys the same schema-independent supported statement
corpus with Modern SQLite and pinned `sqlite3_prepare_v3`. The SQLite side
also performs semantic preparation and bytecode construction, so this is a
conservative end-to-end guard rather than a parser-only equivalence claim.
Statement count, return status, and tail offsets are verified outside the
timed loop. Modern SQLite's median lifecycle time must not exceed 1.5 times
SQLite's median without profiling and a reviewed exception.

## Deferred work

This node does not implement:

- catalog-aware name resolution or semantic validation;
- literal decoding or runtime values;
- joins, grouping, ordering, compounds, CTEs, subqueries, windows, VALUES, or
  table-valued functions;
- DML, transactions, pragmas, attach/detach, vacuum, triggers, views, or
  virtual tables;
- generated columns, foreign keys, references, or `CREATE TABLE AS SELECT`;
- complete SQLite diagnostic rendering or C API result-code mapping; or
- alternate SQLite compile-time grammar profiles.

Each deferred syntax family requires an AST decision before parser support.

## Consequences

- Later layers receive immutable trees with exact source spelling and no
  parser-owned lifetime.
- Prepare-style callers can iterate a multi-statement buffer using one
  absolute tail offset.
- SQLite keyword fallback and operator precedence remain explicit, reviewed
  data rather than incidental recursive-descent behavior.
- Unsupported valid SQLite syntax fails visibly instead of producing a
  misleading partial tree.
- Parser memory is linear in accepted syntax size, while tree destruction
  remains nonrecursive.
- The initial parser is intentionally narrower than SQLite 3.54.0, but every
  accepted form has a source-preserving AST representation and a tested
  compatibility rule.
