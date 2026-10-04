# ADR-0033: Immutable Read-Only SELECT Binding

- Status: Accepted
- Date: 2026-10-04

## Context

The parser produces immutable, source-preserving syntax trees, while the
catalog, function registry, and collation modules expose immutable semantic
metadata. The next dependency-graph node must resolve parsed read-only
`SELECT` statements into a representation that later logical planning,
optimization, and lowering can consume without repeating textual name
lookup.

Pinned SQLite 3.54.0 provides the behavioral reference:

- `src/resolve.c:276-804` resolves one-, two-, and three-part column names,
  applies table aliases, gives real columns precedence over hidden rowid
  names, falls back to result aliases in selected contexts, and reports
  missing or ambiguous names;
- `src/resolve.c:934-953` accepts a likelihood only when its second argument
  is a REAL literal from 0.0 through 1.0;
- `src/resolve.c:1073-1229` resolves scalar functions, validates arity,
  distinguishes aggregate use, records collation use, and recognizes
  planner-hint functions;
- `src/expr.c:45-94` defines expression affinity propagation;
- `src/expr.c:248-309` defines natural and explicit collation propagation;
- `src/expr.c:339-443` selects comparison affinity and collation;
- `src/expr.c:1320-1401` assigns variable numbers and enforces the 32,766
  default variable limit;
- `src/select.c:411-528` expands `*` and `table.*` in table declaration
  order;
- `src/select.c:1838-1908` derives result-column names from aliases, direct
  columns, and source text;
- `src/callback.c:298-337` gives exact function arity precedence over
  variable arity;
- `src/func.c:3399-3550` registers `likely`, `unlikely`, `likelihood`,
  `ifnull`, `coalesce`, `iif`, and `if` as special built-ins;
- `src/sqliteInt.h:1271-1291` defines `sqlite_master` and `sqlite_schema` as
  legacy and preferred names for the schema table; and
- `src/sqliteLimit.h:136-138` defines 32,766 as the default maximum variable
  number.

Behavioral probes against the pinned C API additionally establish that:

- a table alias hides the original table name, including when a schema
  qualifier is present;
- `WHERE` may reference an earlier result alias, but a real source column
  wins and duplicate aliases select the first result;
- result aliases are not visible to other result expressions or to `LIMIT`
  and `OFFSET`;
- unresolved unquoted `TRUE` and `FALSE` become integer literals unless a
  source column shadows them;
- unresolved double-quoted identifiers become string literals when DQS-DML
  is enabled, which is the pinned library default;
- `rowid`, `_rowid_`, and `oid` bind to the hidden integer key only when the
  same spelling is not a declared column;
- all three hidden rowid spellings publish the result name `rowid`, declared
  type `INTEGER`, and INTEGER affinity;
- the main schema table exposes `type TEXT`, `name TEXT`, `tbl_name TEXT`,
  `rootpage INT`, and `sql TEXT` on root page 1;
- ordinary schema-table column qualification accepts both `sqlite_schema`
  and `sqlite_master`, while unaliased qualified-wildcard expansion follows
  SQLite's internal legacy name and accepts `sqlite_master.*`;
- `coalesce` requires at least two arguments, `ifnull` exactly two, and
  `iif`/`if` at least two;
- two-argument `iif`/`if` return NULL when their condition is false;
- `likely` and `unlikely` require one argument, while `likelihood` requires
  exactly two, accepts parentheses around its REAL-literal probability, and
  rejects integer, unary-plus, collated, or computed probability
  expressions;
- a parenthesized direct column retains its declared type, while unary plus,
  explicit `COLLATE`, concatenation, and other computed expressions do not;
- direct and parenthesized `NULL` on the right of `IS` or `IS NOT` use
  dedicated null-test semantics without consuming the left operand's natural
  collation, while aliases, unary plus, and an explicit right-side `COLLATE`
  remain ordinary comparisons; and
- `?`, `?NNN`, and named variables share one 1-based SQLite variable
  namespace, with holes contributing to the published parameter count.

The current syntax tree intentionally supports only zero or one table source.
It has no joins, subqueries, grouping, ordering, compound queries, or window
syntax. The bytecode and VM milestone likewise excludes aggregate and pattern
execution. The binder must remain the smallest implementation that serves
this read-only grammar rather than introducing speculative multi-source
planner structures.

## Decision

Add immutable binder contracts under:

- `include/modern_sqlite/binder/bound_select.hpp`; and
- `src/binder/bound_select.cpp`.

The binder accepts one moved `SyntaxTree`, one retained immutable
`CatalogSnapshotPtr`, one synchronous binding environment, and explicit
limits. It returns either one complete move-only `BoundSelect` or one typed
`BindError`. No partially bound result is observable.

### Public ownership boundary

The public shape is:

```cpp
template <typename Tag>
class BoundId final {
 public:
  explicit constexpr BoundId(std::uint32_t value) noexcept;
  constexpr std::uint32_t value() const noexcept;
  constexpr auto operator<=>(const BoundId&) const noexcept = default;
};

using BoundExpressionId = BoundId<struct BoundExpressionIdTag>;
using BoundSourceColumnId = BoundId<struct BoundSourceColumnIdTag>;
using BoundParameterId = BoundId<struct BoundParameterIdTag>;
using BoundCollationId = BoundId<struct BoundCollationIdTag>;
using BoundFunctionId = BoundId<struct BoundFunctionIdTag>;

struct BindOptions {
  std::size_t maximum_parameters = 32'766;
  std::size_t maximum_result_columns = 2'000;
  std::size_t maximum_function_arguments = 1'000;
  bool enable_double_quoted_strings = true;
};

class BindEnvironment final {
 public:
  BindEnvironment(
      const FunctionRegistry& functions,
      std::span<const Collation* const> collations,
      std::uint64_t registration_generation) noexcept;

  static BindEnvironment Core() noexcept;
};

std::expected<BoundSelect, BindError> BindSelectStatement(
    SyntaxTree tree,
    CatalogSnapshotPtr catalog,
    BindEnvironment environment = BindEnvironment::Core(),
    BindOptions options = {});
```

Binding consumes the moved syntax tree, copies its source buffer into the
successful bound statement, and discards the AST containers before returning.
The source buffer keeps source spans inspectable without exposing syntax to
planning or retaining two complete expression graphs. `BoundSelect` also
keeps a shared reference to the catalog snapshot, which makes every borrowed
catalog name, declared type, and stable object ID valid for the bound
statement's lifetime.

`BoundSelect` is non-copyable and nothrow movable. Its implementation is
hidden behind unique ownership so that later binder internals can evolve
without exposing mutable containers. It provides const accessors for:

- the retained SQL source and catalog;
- the required function/collation registration generation;
- the optional bound table source;
- source-column descriptors;
- owned collation, function, and parameter metadata;
- the immutable bound-expression arena;
- expanded result columns;
- the optional predicate and limit expressions; and
- the total parameter count.

Moving a `BoundSelect` does not invalidate its own semantic references.
Result names, result declared types, SQL source, parameter names, selected
function names, and collation names are owned strings. Catalog string views
refer only to the retained non-movable catalog snapshot.

The environment is needed only during the synchronous bind call. The binder
copies selected function metadata and names, stores collation identities as
owned names, and retains the supplied registration generation. It retains no
`ScalarFunction*`, `Collation*`, registry span, or environment pointer. Later
lowering emits names, and the VM performs the runtime resolution required by
ADR-0031 and ADR-0032.

Pointer-lifetime independence does not permit semantic environment changes.
A connection increments one monotonically increasing registration generation
whenever a function or collation is added, removed, or replaced. The prepared
statement carries the generation captured by `BoundSelect` through planning
and lowering and expires before execution if the connection generation no
longer matches. While a generation remains unchanged, descriptor semantics
and comparator behavior for each registered name are immutable. The core
environment uses generation zero because its process-wide registrations
never change.

`BindEnvironment::Core()` uses `CoreFunctionRegistry()`, a function-local
static pointer array containing the process-wide `BINARY`, `NOCASE`, and
`RTRIM` collations, and generation zero. Function-local initialization keeps
the environment safe when requested by another translation unit's static
initializer. A null catalog, null collation pointer, missing `BINARY`
collation, invalid option limit, non-SELECT statement, or invalid syntax-tree
invariant is misuse. Allocation failure propagates as `std::bad_alloc`; the
binder does not translate it into a semantic error.

### Bound source and columns

The current grammar produces at most one source:

```cpp
enum class BoundSourceKind : std::uint8_t {
  kCatalogTable,
  kSchemaTable,
};

struct BoundTableSource {
  BoundSourceKind kind;
  std::optional<TableId> table;
  SourceSpan span;
};

struct BoundSourceColumn {
  std::string_view name;
  std::optional<std::string_view> declared_type;
  TypeAffinity affinity;
  std::string_view collation_name;
  std::optional<ColumnId> catalog_column;
};
```

`BoundSourceColumnId` is a dense zero-based index into the source-column
array. Catalog-backed columns retain their catalog `ColumnId`. Synthetic
schema columns have no catalog column ID. A later logical plan can therefore
distinguish physical catalog columns, schema-table fields, and hidden rowid
without pointers into parser objects.

Bound symbol tables own every downstream identity selected from the
synchronous environment:

```cpp
struct BoundCollation {
  std::string name;
};

struct BoundScalarFunction {
  std::string name;
  bool deterministic;
  bool uses_collation;
};
```

`BoundCollationId` and `BoundFunctionId` index those tables. Collation names
are interned with SQLite ASCII case folding. Function names are copied using
the selected descriptor's canonical spelling.

A normal table source resolves through the catalog's indexed ASCII-folded
lookup. A two-part table name must use the active catalog's schema name.
An explicit table alias becomes the only effective table qualifier.

`sqlite_schema` and `sqlite_master` resolve to one synthetic main-schema
source before normal table lookup. It has:

| Ordinal | Name | Declared type | Affinity | Collation |
|---:|---|---|---|---|
| 0 | `type` | `TEXT` | TEXT | BINARY |
| 1 | `name` | `TEXT` | TEXT | BINARY |
| 2 | `tbl_name` | `TEXT` | TEXT | BINARY |
| 3 | `rootpage` | `INT` | INTEGER | BINARY |
| 4 | `sql` | `TEXT` | TEXT | BINARY |

The logical plan retains this as a logical schema-table scan. Physical
planning maps it to the page-1 table B-tree, and lowering emits that root-page
descriptor. The synthetic source behaves as a rowid table. Without an
explicit alias, ordinary column qualification accepts both schema-table names
for SQLite compatibility. Qualified wildcard expansion accepts SQLite's
internal legacy qualifier `sqlite_master`. An explicit alias hides both
built-in names.

No TEMP or attached schema is invented by this node. Session-level schema
selection is introduced only when the session owns multiple catalog
snapshots.

### Bound expressions

The bound arena contains a compact tagged union with these alternatives:

- materialized `SqlValue` literal;
- source column;
- hidden rowid;
- zero-based parameter;
- unary operation;
- non-comparison binary operation;
- comparison with selected affinity and collation;
- SQLite truth test;
- resolved result-alias reference;
- resolved eager scalar call;
- lazy coalesce;
- lazy conditional;
- planner likelihood hint; and
- explicit collation wrapper.

The binder defines its own unary and binary enums and does not include or
reuse bytecode instruction types. Comparison nodes may reuse the
runtime-level `SqlComparison` enum because it is part of SQL value semantics,
not VM representation.

Each `BoundExpression` carries:

```cpp
struct BoundExpressionProperties {
  TypeAffinity affinity;
  std::optional<BoundCollationId> collation;
  bool has_explicit_collation;
};

struct BoundExpression {
  SourceSpan span;
  BoundExpressionProperties properties;
  BoundExpressionPayload payload;
};
```

ADR-0036 adds one bound expression hint needed to preserve SQLite's narrow
scalar AND/OR dead-side simplification:

```cpp
enum class BoundTruthHint : std::uint8_t {
  kNone,
  kAlwaysFalse,
  kAlwaysTrue,
};

struct BoundExpressionProperties {
  TypeAffinity affinity;
  std::optional<BoundCollationId> collation;
  bool has_explicit_collation;
  BoundTruthHint truth_hint;
};
```

The hint is not general constant folding. It is set only for unresolved
unquoted TRUE/FALSE or a direct underscore-free decimal/hexadecimal INTEGER
token whose materialized value is nonnegative and no greater than
`INT32_MAX`, matching SQLite's signed-32-bit literal test. Zero is always
false; other eligible values are always true. High-bit hexadecimal tokens
retain their signed 64-bit SQL value but do not receive a hint.

The binder also records the exact parser-generated constants for a non-NULL
literal tested with `IS NULL` or `IS NOT NULL`. SQLite marks those expressions
always false or always true before scalar AND/OR code generation. The
eligibility matrix follows the parser rather than general value evaluation:
INTEGER, REAL, TEXT, and BLOB literals qualify, including the parser's
permitted unary numeric wrappers; a NULL left operand, column, function,
COLLATE wrapper, or other computed expression does not.

Parentheses remain transparent because they bind to the same expression.
Alias references copy the target properties and therefore the hint. COLLATE
explicitly clears the hint because SQLite's COLLATE node does not propagate
the parser truth flags. Unary plus/minus, likelihood, REAL, NULL, TEXT, BLOB,
underscored integer, and larger integer expressions otherwise retain
`kNone`.

An absent property collation means that the expression has no defined
collation. `BoundCollationId` indexes an owned case-insensitive name pool; an
entry may remain unresolved when no operation consumes it. Comparison and
eager-function payloads separately contain their selected, successfully
resolved invocation collation ID, falling back to `BINARY`. This distinction
preserves SQLite's difference between an expression's natural collation and
the collation chosen for one operation.

Parentheses are semantically transparent and bind directly to their child ID.
Result metadata still examines the syntax wrapper so a parenthesized direct
column retains direct-column metadata.

Each result-alias occurrence creates a `BoundAliasReferenceExpression`
containing the already bound target expression ID and the alias occurrence
span. It copies the target's semantic properties but is evaluated
independently every time it is referenced, matching SQLite's duplicated
alias-expression behavior. A later optimizer may common such evaluations
only after proving determinism; shared target identity is not permission to
memoize a nondeterministic expression.

Result expressions are bound from left to right without alias visibility.
Only after all result expressions are bound is the first occurrence of each
ASCII-folded alias published to the `WHERE` scope. This prevents cycles and
matches SQLite's first-alias rule.

`IS TRUE`, `IS FALSE`, `IS NOT TRUE`, and `IS NOT FALSE` use a dedicated
`BoundTruthTestExpression` whenever the right operand resolves to an unquoted
TRUE/FALSE literal, including a result alias whose root is that literal.
Transparent parentheses and `COLLATE` wrappers do not prevent recognition.
Planner-hint function calls are not transparent at this resolution point,
matching pinned SQLite. A truth test evaluates its left operand with
`EvaluateSqlTruth`, always returns integer 0 or 1, and does not apply
comparison affinity or collation. Explicit collation provenance may still
propagate through the expression for a later enclosing consumer.

### Literal materialization

Binding validates public-AST literal kind/spelling coherence, then materializes
literals into immutable `SqlValue` objects so that the optimizer and lowering
layers do not reparse source text. The conversion follows pinned SQLite
behavior:

- NULL produces SQL NULL;
- unquoted fallback TRUE and FALSE produce integer 1 and 0;
- decimal integer tokens ignore embedded underscores;
- decimal integers outside signed 64-bit range become REAL;
- hexadecimal integer tokens ignore embedded underscores and leading zeroes,
  then use SQLite's signed 64-bit two's-complement interpretation for at most
  16 significant digits;
- unary minus applied through transparent parentheses and unary plus to
  decimal magnitude `9223372036854775808`, independent of leading zeroes or
  embedded underscores, produces the minimum signed integer rather than a
  rounded REAL;
- REAL tokens ignore embedded underscores and use the existing
  SQLite-compatible floating parser;
- string literals remove their delimiters and collapse doubled delimiters;
  and
- blob literals decode each validated hexadecimal byte into owned storage.

Current-date, current-time, and current-timestamp literals are rejected as
unsupported because the current environment has no connection clock or
statement-time contract.

An oversized hexadecimal literal is valid syntax but invalid SQL value
materialization. It returns the user-facing diagnostic
`hex literal too big: <token>` at the literal or enclosing unary-minus span.
Other malformed literal shapes reaching this boundary indicate a parser or
AST invariant violation. No invalid literal is silently converted to NULL or
zero.

### Name resolution

Identifiers contain one, two, or three parts:

- `column`;
- `table-or-alias.column`; or
- `schema.table-or-alias.column`.

All schema, table, alias, column, function, and collation matching uses
SQLite's ASCII-only case folding. Quoted identifiers are dequoted before
matching but retain the same folding rules.

Resolution for an unqualified identifier is:

1. a declared source column;
2. a hidden rowid spelling not shadowed by that exact declared spelling;
3. the first visible result alias, when the binding context allows aliases;
4. unquoted `TRUE` or `FALSE`; and
5. an unresolved double-quoted string literal when DQS-DML is enabled.

Qualified identifiers consider only the qualified source and never result
aliases, boolean fallback, or DQS fallback. A real column always wins over a
same-named result alias. A declared `rowid`, `_rowid_`, or `oid` shadows only
that spelling; the remaining hidden spellings continue to expose the rowid.
WITHOUT ROWID tables expose no hidden rowid.

The current single-source grammar cannot construct a genuine multi-table
ambiguity. `BindErrorCode::kAmbiguousColumn` is nevertheless reserved so the
public diagnostic category remains stable when joins are added. Current
differential tests cover all reachable ambiguity-adjacent cases: source
column versus alias, duplicate aliases, rowid shadowing, schema-table aliases,
and hidden original table names.

The `WHERE` expression binds with source columns and result aliases visible.
`LIMIT` and `OFFSET` bind in an empty name scope: parameters, literals, and
scalar expressions are valid, but source columns and result aliases are not.

### Wildcard expansion

Wildcards are valid only as top-level result columns.

- `*` requires a table source and expands all declared source columns in
  declaration order.
- `table.*` requires the effective source qualifier.
- Hidden rowid names are never added by wildcard expansion.
- A table alias hides the original table name.
- Expansion creates ordinary bound column expressions and ordinary
  `BoundResultColumn` entries; downstream layers never handle wildcard nodes.
- The result-column limit is checked during expansion with overflow-safe
  arithmetic.

A wildcard without a source reports `no tables specified`. A mismatched
qualified wildcard reports `no such table: <qualifier>`. A wildcard in a
scalar-expression position is rejected. Aggregate `count(*)` remains outside
the read MVP rather than receiving partial scalar semantics.

### Result metadata

Every expanded result contains:

```cpp
struct BoundResultColumn {
  BoundExpressionId expression;
  std::string name;
  std::optional<std::string> declared_type;
  TypeAffinity affinity;
};
```

The name is:

1. the dequoted explicit alias, when present;
2. the catalog column's canonical name for a direct or parenthesized direct
   column;
3. `rowid` for any direct or parenthesized hidden-rowid spelling; or
4. the exact source bytes of the result expression.

A direct or parenthesized catalog column copies its declared type. A direct
or parenthesized hidden rowid uses `INTEGER`. Other expressions, including
explicit `COLLATE` and unary plus, have no declared type. The result affinity
is always the bound expression's affinity and is independent of whether a
declared type is published.

### Affinity and collation

Expression affinity follows SQLite:

- a source column uses its catalog affinity;
- hidden rowid uses INTEGER;
- parentheses and explicit `COLLATE` preserve operand affinity;
- unary plus removes affinity;
- every other unary, binary, comparison, function, parameter, and literal
  expression has NONE affinity.

Comparison affinity is selected once during binding:

- if both operands have affinity and either is NUMERIC, INTEGER, or REAL, use
  NUMERIC;
- if both operands have affinity and neither is numeric-family, use BLOB;
- if only one operand has affinity, use that affinity; and
- otherwise use NONE.

A natural collation originates from a direct source column and survives
parentheses or unary plus. An explicit `COLLATE` wrapper replaces that
identity and marks it explicit. Explicit collation markings propagate through
enclosing operators and calls even when natural column collation would
otherwise be lost.

Binary comparison collation precedence is:

1. explicit collation propagated from the left operand;
2. explicit collation propagated from the right operand;
3. the left operand's natural collation;
4. the right operand's natural collation; and
5. BINARY.

Direct or parenthesized NULL on the right of `IS` or `IS NOT` is a null test,
not a text comparison. It records NONE affinity and BINARY in the uniform
comparison payload without resolving either operand's natural collation.
Aliases, unary plus, and explicit right-side `COLLATE` wrappers are not
transparent to this recognition.

An eager scalar function that requires collation receives the first argument
with any defined collation, otherwise BINARY. Functions that do not inspect
collation still carry BINARY for a uniform lowering contract. The function
result itself propagates only an explicit argument collation, not a natural
column collation.

Binding interns natural and explicit collation names without resolving every
occurrence. This is required for SQLite compatibility: selecting a column
whose declared collation is unavailable, or selecting an explicitly collated
standalone expression, does not itself require the comparison implementation.
The binder resolves a collation only when a comparison or collation-sensitive
function selects it for execution. The payload then stores the owned
canonical name ID. A missing selected descriptor reports
`no such collation sequence: <name>`; BINARY is never substituted for an
unknown named collation.

### Functions and lazy forms

Ordinary scalar functions resolve against `FunctionRegistry`. Exact arity
outranks minimum arity. Function names are ASCII-case-insensitive. A
successful match is copied into a bound function table containing its owned
name, determinism, and collation-use metadata; no callback pointer is
retained. The binder distinguishes:

- no registered function with that name;
- a registered scalar name with invalid arity; and
- a known aggregate-only form that is unsupported by the read MVP.

`DISTINCT` on an ordinary scalar call is accepted and ignored, matching
SQLite. Aggregate calls, including `count`, `sum`, `avg`, one-argument
`min`/`max`, and wildcard function arguments, return a typed unsupported
feature error. Pattern operators `LIKE`, `GLOB`, `REGEXP`, and `MATCH` are
also rejected rather than rewritten into partially supported function calls.

These built-ins require lazy or planner semantics when no matching
environment function overrides them:

| Name | Arity | Bound form |
|---|---:|---|
| `coalesce` | at least 2 | lazy first-non-NULL |
| `ifnull` | exactly 2 | lazy first-non-NULL |
| `iif`, `if` | at least 2 | lazy condition/value pairs plus optional default |
| `likely` | exactly 1 | identity value, probability 0.9375 |
| `unlikely` | exactly 1 | identity value, probability 0.0625 |
| `likelihood` | exactly 2 | identity value plus validated REAL-literal probability |

The binder first attempts ordinary registry resolution for the exact call.
This matches SQLite's application-defined-function precedence. Only an
unmatched call is considered for the built-in special forms above. A custom
descriptor with matching name and arity therefore binds as an eager scalar
call; a nonmatching custom overload does not hide a valid special built-in.

The binder does not eagerly evaluate any lazy branch. It binds all branch
expressions for names and types, then preserves their ordered IDs for later
control-flow lowering. For an odd number of `iif`/`if` arguments the final
argument is the default; for an even number there is no default and a
no-match result is NULL.

### Parameters

Bound parameter IDs are zero-based, while SQL source numbering is one-based.
Before semantic expression binding, the binder collects every variable token
and assigns the complete namespace in ascending source-offset order. This
lexical prepass is required because normalized semantic fields may not retain
token order; for example, `LIMIT offset, count` stores the count before the
offset. Assignment follows SQLite:

- `?` uses one plus the current maximum variable number;
- `?NNN` uses explicit number NNN and may create holes;
- repeated `:name`, `@name`, and `$name` tokens reuse the exact same number;
- different prefixes are different names;
- a new named parameter uses one plus the current maximum; and
- `?NNN` must be in the inclusive range 1 through the configured maximum.

The published parameter count is the greatest assigned SQL variable number,
not the number of distinct tokens. `BoundSelect` publishes one metadata entry
per slot:

```cpp
struct BoundParameter {
  std::optional<std::string> name;
};
```

Anonymous `?` slots and holes have no name. Named parameters and explicit
`?NNN` parameters retain the first exact source spelling assigned to that
slot; later equivalent explicit numbers do not replace it. This metadata is
carried through lowering so the prepared-statement session can support named
lookup without reparsing SQL.

### SELECT scope and unsupported syntax

The binder accepts:

- default or explicit `ALL`;
- one or more explicit result expressions;
- zero or one table source;
- optional `WHERE`;
- optional `LIMIT` and `OFFSET`; and
- the expression forms listed above.

It rejects:

- `SELECT DISTINCT`;
- aggregates and wildcard scalar arguments;
- pattern operators;
- current-time literals; and
- any future AST alternative for joins, grouping, ordering, compounds,
  subqueries, windows, writes, schema mutation, or transactions until the
  corresponding dependency-graph node exists.

Unsupported syntax returns an explicit typed error. It never disappears,
binds as NULL, or reaches lowering in an invalid variant.

### Errors

`BindError` owns a stable detail string and points to the most specific source
span available. Its code distinguishes at least:

- misuse or invalid syntax-tree invariant;
- unsupported feature;
- missing table;
- missing column;
- ambiguous column;
- wildcard without a table;
- missing function;
- invalid function arity;
- missing collation;
- invalid literal;
- invalid variable number;
- parameter limit exceeded;
- function-argument limit exceeded; and
- result-column limit exceeded.

Semantic lookup and unsupported-feature failures map to `ErrorCode::kGeneric`.
Configured SQL limits map to `ErrorCode::kTooLarge`. Misuse and internal
invariants map to `ErrorCode::kMisuse` or `ErrorCode::kInternal` as
appropriate.

The diagnostic text follows pinned SQLite wording where the current error
model can represent it, including:

- `no such table: <name>`;
- `no such column: <name>`;
- `ambiguous column name: <name>`;
- `no tables specified`;
- `no such function: <name>`;
- `wrong number of arguments to function <name>()`;
- `no such collation sequence: <name>`;
- `hex literal too big: <token>`;
- `variable number must be between ?1 and ?<limit>`; and
- `too many SQL variables`.

### Dependency boundary

The binder may include:

- base result and byte primitives;
- text and source spans;
- SQL values;
- collations and scalar-function descriptors;
- syntax trees; and
- immutable catalog snapshots.

It must not include bytecode, VM, pager, B-tree, VFS, transaction, session,
API, diagnostics, logical-plan, optimizer, or lowering headers. Bound IDs are
independent of AST IDs, catalog IDs, bytecode IDs, registers, cursors, and
instruction addresses.

### Performance contract

Binding is performance-sensitive but remains an explicit compile-time
boundary:

- table and column lookup use catalog sidecars rather than schema scans;
- source and expression vectors reserve from syntax counts before binding;
- wildcard expansion visits each declared column once;
- each syntax expression occurrence is bound once, while alias-reference nodes
  point to already bound targets without authorizing runtime memoization;
- function lookup and every operation-required collation lookup complete
  during binding and are not repeated by planning or lowering;
- unquoted one-, two-, and three-part identifiers use fixed-capacity borrowed
  source views, so repeated ordinary name binding does not allocate temporary
  name containers; quoted parts allocate only when dequoting is required;
- named-variable lookup is exact and does not fold or normalize bytes; and
- ordinary successful binding performs no exception translation or logging.

A Release benchmark artifact compares a deterministic parse-and-bind workload
with pinned SQLite prepare on the same SQL and schema. It reports simple
table/column lookup, wildcard expansion, expression binding, parameter
assignment, wall time, and owned-allocation counts separately. Because the
native control includes parsing and VDBE generation while this node ends at
binding, the first gate is a severe-regression guard and report-shape check,
not a parity claim. Any measured hot-path optimization must follow ADR-0008.

### Verification

Red-first unit and differential tests cover:

- catalog and schema-table source resolution;
- table aliases and schema-qualified aliases;
- direct, qualified, missing, and shadowed columns;
- rowid spellings, declared shadows, INTEGER PRIMARY KEY aliases, and WITHOUT
  ROWID tables;
- result aliases, duplicate aliases, alias/source precedence, and scope
  exclusions;
- wildcard expansion, qualifiers, ordering, aliases, and limits;
- unquoted TRUE/FALSE, quoted-identifier exclusion, and configurable DQS
  fallback;
- SQLite truth tests, including columns named TRUE/FALSE, parenthesized and
  collated booleans, alias-derived booleans, and nontransparent likelihood
  calls;
- exact result names, declared types, and affinities;
- comparison affinity and collation precedence;
- direct `IS NULL` and `IS NOT NULL` behavior with unavailable natural
  collations, plus nontransparent alias, unary-plus, and explicit-collation
  wrappers;
- explicit and natural collation propagation, unavailable-but-unconsumed
  collations, and failures only when an operation selects a missing
  collation;
- eager scalar resolution, exact-versus-minimum arity, DISTINCT, lazy forms,
  application-defined overrides, 2/3/4/5-argument conditional parity,
  aggregate rejection, parenthesized likelihood probabilities, and
  likelihood validation;
- literal materialization at signed integer, transparent signed-minimum,
  leading-zero signed-minimum, hexadecimal, REAL, string, and blob
  boundaries, plus malformed public-AST literal rejection;
- literal truth-hint boundaries for TRUE/FALSE, direct signed-32-bit unsigned
  tokens, high-bit hexadecimal values, underscores, large integers, COLLATE,
  and unary wrappers;
- anonymous, explicit, named, repeated, sparse, source-order, first-spelling,
  and over-limit variables;
- `WHERE`, `LIMIT`, and `OFFSET` scope;
- every supported unary and binary expression;
- unsupported operators and SELECT forms;
- moved-from ownership, synchronous pointer independence, registration
  generation invalidation, discarded AST storage, and invalid input;
- deterministic allocation failure at representative ownership boundaries;
- deterministic one-name versus repeated-name allocation scaling and
  allocation-free immutable reads; and
- binder layering and Release benchmark report validation.

## Consequences

- Planning receives immutable semantic input and does not repeat identifier,
  selected-function, selected-collation, parameter, wildcard, or literal
  work.
- The binder retains SQL source and catalog ownership but copies all
  connection-level function and collation metadata required downstream.
- Function or collation registration replacement invalidates prepared
  semantics through one explicit connection generation rather than dangling
  pointers or silent same-name behavior changes.
- The initial implementation remains single-source and read-only without
  prebuilding join scopes or mutating parser nodes.
- Schema-table compatibility and DQS-DML policy are explicit rather than
  hidden in session or parser behavior.
- Later joins can add multiple source scopes while preserving bound IDs,
  expression metadata, and the existing ambiguous-column error category.
- Later aggregates, patterns, current-time functions, and advanced SELECT
  syntax require reviewed extensions rather than accidental partial support.
