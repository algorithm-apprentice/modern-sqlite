# ADR-0027: Immutable Source-Preserving Syntax Trees

- Status: Accepted
- Date: 2026-10-04

## Context

The parser must produce syntax that can outlive its caller's SQL buffer and be
consumed independently by catalog loading, binding, planning, and later DML
work. The syntax layer must preserve exact literal spelling and byte locations
without acquiring catalog objects, runtime values, cursor numbers, registers,
or bytecode state.

Pinned SQLite 3.54.0 deliberately combines those concerns:

- `src/sqliteInt.h:3014-3142` defines `Expr` with a parser token opcode and
  source offset alongside dequoted token storage, resolved table and column
  numbers, aggregate state, VDBE registers, table pointers, window state, and
  subroutine addresses.
- `src/sqliteInt.h:3215-3278` defines mutable `ExprList` entries that later
  gain aliases, ordering metadata, resolved column numbers, register numbers,
  and planner flags.
- `src/sqliteInt.h:3349-3460` defines `SrcItem` and `SrcList` with parsed
  names alongside `Table*`, cursor IDs, used-column masks, index choices,
  subquery execution state, and join rewrites.
- `src/sqliteInt.h:3625-3650` defines `Select` with syntax clauses alongside
  estimated rows and LIMIT/OFFSET VM registers.
- `src/parse.y:651-870` constructs and then mutates those objects while parsing
  `SELECT`, result columns, sources, aliases, and clauses.
- `src/parse.y:1080-1250` dequotes literals and identifiers during expression
  construction and assigns variable or register numbers.
- `src/parse.y:1626-1675` sends parsed `CREATE INDEX` syntax directly into
  catalog mutation.

SQLite bounds expression depth to 1000 by default and parser-stack depth to
2500 in `src/sqliteLimit.h:93-117`. Those limits protect parsing and later
recursive algorithms, but a syntax object's destructor should not itself
depend on recursion depth.

The current read-only milestone needs:

- simple `SELECT` projection, one table source, predicates, functions, and
  LIMIT/OFFSET;
- `CREATE TABLE` and `CREATE INDEX` syntax sufficient for loading the initial
  `sqlite_schema` corpus; and
- extension points for later DML, joins, subqueries, aggregates, and schema
  features without importing those features prematurely.

## Decision

Add immutable syntax data under `modern_sqlite/syntax/ast.hpp`.

### Tree ownership and source lifetime

`SyntaxTree` owns one `std::string` containing exactly one nonempty statement
slice. The slice begins at the first nontrivia token, ends after the optional
terminating semicolon when one is present, and otherwise ends after the final
grammar token. Leading and trailing trivia outside those boundaries is not
retained. Interior trivia, including trivia before a terminator, remains
recoverable even though trivia does not become AST nodes. The bytes may
contain malformed UTF-8 and are never normalized.

Every name, token, clause, and node location is a checked half-open
`SourceSpan` relative to the beginning of that owned slice. The root statement
span starts at offset zero and ends after the final grammar token, excluding
the terminating semicolon.

The parser will either lex a statement-local view or subtract the absolute
slice-begin offset from every retained lexer span before finalizing the tree.
Its parse result separately reports the absolute input offset at which the
next prepare operation should resume. Multi-statement tail bytes are therefore
not retained inside the tree, and a second statement never carries offsets
from the caller's complete input buffer.

Literal, variable, identifier, operator, collation, type-name, and alias
spans retain their original quoting, case, numeric separators, and delimiter
spelling. AST construction performs no dequoting, numeric conversion, UTF-8
validation, name folding, or runtime-value construction.

`SyntaxTree` is move-only. It exposes its source, root statement, and
expressions only through const views and references. Node records are mutable
construction values before `SyntaxTree::Create` consumes them, but no mutable
tree access is exposed after successful creation.

Every borrowed `Utf8View`, `std::span`, reference, or pointer obtained from a
tree is invalidated when that tree is moved from, move-assigned, or destroyed.
Copied `ExpressionId` and `SourceSpan` values remain usable with the move
destination because arena order and source-relative offsets do not change,
but consumers must reacquire all views and references from the destination.

### Arena ownership and expression IDs

Expressions are stored by value in one contiguous vector and refer to children
through:

```cpp
struct ExpressionId {
  std::size_t value = 0;

  constexpr auto operator<=>(const ExpressionId&) const noexcept = default;
};
```

The vector is postorder: every child ID must be strictly smaller than its
parent's ID. The root statement owns every top-level expression reference.
`SyntaxTree::Create` rejects out-of-range, forward, cyclic, unreferenced, or
multiply referenced expression IDs. This produces a true owned tree while
keeping all destruction iterative through container destructors.

There are no owning raw pointers or recursive owning smart pointers. Moving or
destroying a tree does not recurse through expression depth, so adversarially
deep construction cannot overflow the C++ call stack during cleanup.

### Expression model

Every `Expression` contains a full syntax span and one payload from:

- `LiteralExpression` with a `LiteralKind` and exact token span;
- `VariableExpression` with its exact token span;
- `IdentifierExpression` with a one-to-three-part `QualifiedName`;
- `WildcardExpression` with the `*` span and an optional qualifier;
- `UnaryExpression` with a typed operator, operator span, and operand ID;
- `BinaryExpression` with a typed operator, operator span, and left/right IDs;
- `FunctionCallExpression` with an exact name, ordered argument IDs, and the
  presence of `DISTINCT`;
- `CollateExpression` with an operand ID, the `COLLATE` keyword span, and the
  collation-name span; or
- `ParenthesizedExpression` with its inner expression ID.

The initial unary operators are positive, negative, bitwise-not, and logical
not. Initial binary operators cover logical conjunction/disjunction,
comparisons including `IS` and `IS NOT`, arithmetic, bit operations,
concatenation, and the `LIKE`, `GLOB`, `REGEXP`, and `MATCH` families.

Literal kinds distinguish NULL, integer, floating-point, string, blob,
`CURRENT_DATE`, `CURRENT_TIME`, `CURRENT_TIMESTAMP`, TRUE, and FALSE while
retaining the raw token span. Variables remain unnumbered syntax.

`QualifiedName` owns an ordered vector of one to three identifier spans plus a
full span. It does not store dequoted text or resolved schema, table, or column
IDs. Identifier expressions may use one to three parts. SQLite's grammar
restricts function names and wildcard qualifiers to exactly one part, and the
validator enforces those context-specific limits.

### Initial statement model

The root `Statement` is a variant of:

- `SelectStatement`;
- `CreateTableStatement`; or
- `CreateIndexStatement`.

`SelectStatement` contains:

- default, ALL, or DISTINCT quantification;
- one or more result columns with expression IDs and optional alias spans;
- zero or one named table source with an optional alias;
- an optional WHERE expression; and
- an optional LIMIT with normalized limit and offset expression IDs plus the
  original clause span and syntax form.

Joins, subqueries, compound SELECT, GROUP BY, HAVING, ORDER BY, windows, CTEs,
and VALUES are deliberately absent from this node.

The shared `ConflictAction` enumeration distinguishes default, ROLLBACK,
ABORT, FAIL, IGNORE, and REPLACE behavior. The shared `IndexedTerm` record
contains its full span, expression ID, optional collation-name span, and ASC,
DESC, or default sort order. It is used by table key constraints and
`CREATE INDEX`, preserving key order and layout without resolving names or
collations.

`CreateTableStatement` contains:

- TEMP and IF NOT EXISTS flags;
- a one- or two-part object name;
- one or more column definitions;
- column type names as optional raw spans;
- named or unnamed column constraints:
  - PRIMARY KEY with sort order, conflict action, and AUTOINCREMENT;
  - NULL, NOT NULL, and UNIQUE with conflict actions;
  - CHECK and DEFAULT with expression IDs; and
  - COLLATE with an exact collation-name span;
- named or unnamed table constraints:
  - PRIMARY KEY and UNIQUE with ordered `IndexedTerm` values and conflict
    actions, with PRIMARY KEY also preserving AUTOINCREMENT; and
  - CHECK with an expression ID; and
- WITHOUT ROWID and STRICT flags.

`CreateIndexStatement` contains:

- UNIQUE and IF NOT EXISTS flags;
- the optionally schema-qualified index name and unqualified table name;
- one or more ordered `IndexedTerm` values; and
- an optional partial-index WHERE expression.

Foreign keys, generated columns, CREATE TABLE AS SELECT, virtual tables,
views, triggers, and schema mutations are deferred. The parser will reject
unsupported forms explicitly rather than storing partially interpreted
syntax.

### Validation contract

The construction entry point is:

```cpp
class SyntaxTree final {
 public:
  [[nodiscard]] static Result<SyntaxTree> Create(
      std::string source,
      std::vector<Expression> expressions,
      Statement statement);

  SyntaxTree(const SyntaxTree&) = delete;
  SyntaxTree& operator=(const SyntaxTree&) = delete;
  SyntaxTree(SyntaxTree&&) noexcept = default;
  SyntaxTree& operator=(SyntaxTree&&) noexcept = default;

  [[nodiscard]] Utf8View source() const noexcept;
  [[nodiscard]] const Statement& statement() const noexcept;
  [[nodiscard]] std::span<const Expression> expressions() const noexcept;
  [[nodiscard]] const Expression& expression(ExpressionId id) const noexcept;
};
```

`Create` performs an iterative validation pass before moving storage into the
tree:

- every span is nonempty where syntax is required and lies within the owned
  source;
- composite spans contain their token, name, clause, and child spans;
- qualified names contain one to three ordered nonempty parts;
- each expression child is an earlier valid node;
- collations contain a prior operand and ordered `COLLATE` and name spans;
- unary and binary expression spans exactly cover their operators and
  operands;
- LIMIT and OFFSET expression order agrees with the retained comma or OFFSET
  syntax;
- each expression is referenced exactly once by either a parent expression or
  the root statement;
- statement-specific required lists are nonempty; and
- optional payload fields agree with their variant kind.

Invalid construction data returns `ErrorCode::kMisuse`; it represents an
internal parser or test-builder defect, not user SQL syntax. Allocation failure
is not caught in the AST layer and is converted by the later parser/API
boundary according to ADR-0004.

Trusted consumers may use `expression(id)` after construction; it asserts the
validated ID invariant and performs no repeated recoverable validation.

### Immutability boundaries

Syntax contains no:

- `SqlValue`, affinity, or converted literal value;
- collation or function-registry pointer;
- catalog object, resolved identifier, root page, or schema version;
- logical or physical plan annotation;
- VM register, cursor, label, instruction, or result destination; or
- mutable rewrite flag.

Binding and planning produce separate representations instead of annotating
syntax nodes in place.

### Verification

Red-first tests will cover:

- exact preservation and slicing of malformed UTF-8, quoted identifiers,
  strings, blobs, and numeric separators;
- rebasing a second statement's spans from a larger input containing leading
  trivia, an earlier statement, and a terminator;
- every expression and initial statement payload;
- move-only ownership, ID and source-span value stability after moves, and
  explicit reacquisition of all borrowed views;
- rejection of invalid spans, qualified names, forward references, duplicate
  ownership, unreferenced nodes, and invalid statement shapes;
- parent/child span containment;
- absence of runtime, catalog, cursor, register, and bytecode fields from the
  public syntax contract; and
- construction and destruction of at least 100,000 nested parenthesized
  expressions without recursive cleanup.

The AST node is not performance-sensitive in the project DAG, so it requires
no native timing comparison. Tests still require linear validation and
constant-stack destruction.

## Consequences

- Parsed statements own all source bytes needed by diagnostics and later
  compilation stages.
- Exact syntax remains available without storing copied token strings on every
  node.
- A flat postorder arena gives compact ownership, stable IDs, iterative
  validation, and depth-independent destruction.
- Catalog loading and query compilation share one pure syntax vocabulary
  without inheriting SQLite's mutable parser/runtime structures.
- Initial statement variants intentionally cover the read milestone and
  schema loading; later SQL features extend the variants in reviewed DAG
  nodes rather than pre-building the full SQLite grammar now.
