# ADR-0029: Immutable Catalog Snapshots

- Status: Accepted
- Date: 2026-10-04

## Context

The catalog loader, binder, bytecode verifier, optimizer, and B-tree cursor
setup need a common schema representation. That representation must preserve
SQLite's object identity and physical index metadata without inheriting
SQLite's mutable, mutex-protected pointer graph.

Pinned SQLite 3.54.0 defines the relevant behavior in:

- `src/sqliteInt.h:1536-1547`, where `Schema` stores the schema cookie,
  mutable generation counter, and name-indexed object hashes;
- `src/sqliteInt.h:2284-2330`, where `Column` stores its name, type,
  affinity, constraints, collation metadata, and default-expression index;
- `src/sqliteInt.h:2464-2535`, where `Table` stores columns, linked indexes,
  root page, INTEGER PRIMARY KEY alias, flags, and row estimates;
- `src/sqliteInt.h:2829-2892`, where `Index` stores the owning table, root
  page, key and auxiliary terms, collations, sort orders, partial predicate,
  and `sqlite_stat1` estimates;
- `src/build.c:338-399` and `src/build.c:527-544`, where table and index
  lookup is SQLite-ASCII-case-insensitive and unqualified lookup searches
  TEMP, MAIN, then attached databases;
- `src/build.c:1549-1768`, where column names are dequoted, duplicate names
  are rejected case-insensitively, and declared-type spelling determines
  affinity;
- `src/build.c:1882-1960`, where a non-descending single-column declaration
  whose dequoted type is exactly `INTEGER` becomes the rowid alias instead of
  creating a separate primary-key index;
- `src/build.c:2437-2555`, where a WITHOUT ROWID table uses its primary-key
  index as the table B-tree and extends that index into a covering record;
- `src/build.c:4020-4620`, where automatic indexes are named, index
  expressions are resolved, the table key is appended to every index record,
  and non-WITHOUT-ROWID automatic-index schema rows are emitted with NULL SQL;
- `src/prepare.c:96-203`, where `sqlite_schema` rows are parsed and blank-SQL
  automatic-index rows attach root pages to indexes created while parsing the
  table definition;
- `src/callback.c:495-526`, where clearing a loaded schema increments its
  mutable generation counter; and
- `src/analyze.c:20-67` and `src/analyze.c:1523-1648`, where
  `sqlite_stat1` supplies exact row-count and per-prefix estimates plus
  `unordered`, `noskipscan`, and `sz=` metadata.

The immutable syntax tree from ADR-0027 is move-only and owns the exact source
for one parsed DDL statement. Catalog data derived from that syntax must not
retain borrowed source spans after the tree is destroyed or moved. Defaults,
CHECK constraints, expression indexes, and partial-index predicates also need
stable expression references for later compilation.

The current milestone opens one database and has no TEMP or ATTACH support.
Building a connection-wide multi-schema search path now would add policy that
belongs to the later session layer. Conversely, omitting physical index
suffixes would force the B-tree and optimizer layers to reconstruct catalog
semantics independently.

## Decision

Add the immutable catalog model under
`modern_sqlite/catalog/catalog.hpp`.

### Scope: one schema per snapshot

One `CatalogSnapshot` represents one named SQLite schema, initially `main`.
It owns tables, indexes, columns, parsed DDL definitions, lookup sidecars,
version metadata, and planner statistics for that schema.

TEMP-before-MAIN-before-attached search order is deliberately excluded. A
later session-owned catalog set may order multiple immutable snapshots without
changing their internal representation. Schema aliases such as
`sqlite_master` and `sqlite_schema` are likewise session or binder policy,
not magic entries in the single-schema lookup table.

Views, virtual tables, triggers, foreign keys, generated columns, STAT4
samples, schema mutation, TEMP objects, and attached schemas remain outside
this node. The next catalog-loader node rejects unsupported persistent schema
objects explicitly.

### Stable dense identities

The public model uses value IDs rather than internal pointers:

```cpp
struct TableId {
  std::size_t value = 0;
  constexpr auto operator<=>(const TableId&) const noexcept = default;
};

struct ColumnId {
  std::size_t value = 0;
  constexpr auto operator<=>(const ColumnId&) const noexcept = default;
};

struct IndexId {
  std::size_t value = 0;
  constexpr auto operator<=>(const IndexId&) const noexcept = default;
};

struct SchemaDefinitionId {
  std::size_t value = 0;
  constexpr auto operator<=>(const SchemaDefinitionId&) const noexcept =
      default;
};
```

Table, index, and definition IDs are dense indexes in snapshot construction
order. Column IDs are dense and table-local. Construction order is preserved
for deterministic diagnostics and inspection; lookup order is independent.

IDs are stable for the lifetime of their owning snapshot but are not portable
across snapshots, schema reloads, database files, or serialization. A caller
must retain the owning snapshot and must never apply an ID to a different
snapshot. Validated accessors assert this trusted internal invariant and do
not repeat recoverable bounds checks.

Root pages use a catalog-layer strong value:

```cpp
struct RootPageId {
  std::uint32_t value = 0;
  constexpr auto operator<=>(const RootPageId&) const noexcept = default;
};
```

The catalog does not depend upward on pager or B-tree handles. Opening a
cursor performs an explicit conversion from a validated `RootPageId` to the
storage-layer `PageNumber`.

### Shared immutable ownership

Validated construction returns:

```cpp
enum class CatalogValidationCode : std::uint8_t {
  kInvalidString,
  kDuplicateName,
  kInvalidReference,
  kDefinitionMismatch,
  kInvalidTableShape,
  kInvalidIndexShape,
  kInvalidRootPage,
  kInvalidStatistics,
  kUnownedDefinition,
};

enum class CatalogObjectKind : std::uint8_t {
  kSchema,
  kDefinition,
  kTable,
  kIndex,
};

enum class CatalogMemberKind : std::uint8_t {
  kNone,
  kColumn,
  kCheckExpression,
  kDefaultExpression,
  kIndexTerm,
  kPartialPredicate,
  kStatisticValue,
};

struct CatalogValidationLocation {
  CatalogObjectKind owner_kind;
  std::size_t owner_index;
  CatalogMemberKind member_kind = CatalogMemberKind::kNone;
  std::size_t member_index = 0;
};

struct CatalogValidationError {
  CatalogValidationCode code;
  CatalogValidationLocation location;
  std::string detail;
};

class CatalogSnapshot;
struct CatalogInput;

using CatalogSnapshotPtr = std::shared_ptr<const CatalogSnapshot>;
using CatalogSnapshotResult =
    std::expected<CatalogSnapshotPtr, CatalogValidationError>;

class CatalogSnapshot final {
 public:
  [[nodiscard]] static CatalogSnapshotResult Create(CatalogInput input);

  CatalogSnapshot(const CatalogSnapshot&) = delete;
  CatalogSnapshot& operator=(const CatalogSnapshot&) = delete;
  CatalogSnapshot(CatalogSnapshot&&) = delete;
  CatalogSnapshot& operator=(CatalogSnapshot&&) = delete;
};
```

There is no public constructor and no mutable accessor. The shared pointer is
the publication unit: a session may atomically replace its current pointer
while prepared statements retain the old snapshot. Old objects, strings,
syntax trees, IDs, spans, and expression references remain valid until the
last owner releases that snapshot.

Reference counting occurs only when ownership is copied, not during table,
column, index, definition, or expression lookup. Allocation failure is not
caught inside this layer and is handled at the later API boundary required by
ADR-0004.

### Version identity

Each snapshot stores:

```cpp
struct CatalogVersion {
  std::uint32_t schema_cookie = 0;
  std::uint64_t generation = 0;
  constexpr auto operator<=>(const CatalogVersion&) const noexcept = default;
};
```

`schema_cookie` is the on-disk value from database-header offset 40.
`generation` is assigned monotonically by the owner whenever it publishes a
new snapshot, including a reload whose cookie happens to equal an older
cookie. Equality of the pair identifies one published schema version.

Ordinary data changes do not require a new catalog version. A schema-cookie
change requires reloading before new statements are prepared. Existing
statements continue to own their old snapshot until session policy returns
`kSchemaChanged` or reparses them.

### Owned DDL definitions and expression references

`CatalogInput` transfers a vector of validated `SyntaxTree` values into the
snapshot. Every table references one CREATE TABLE definition. Every explicit
index references one CREATE INDEX definition. Every automatic PRIMARY KEY or
UNIQUE index references the CREATE TABLE definition that created it.

Catalog expressions use:

```cpp
struct SchemaExpression {
  SchemaDefinitionId definition;
  ExpressionId expression;
  constexpr auto operator<=>(const SchemaExpression&) const noexcept =
      default;
};
```

Column defaults, table CHECK constraints, expression-index terms, and partial
index predicates retain `SchemaExpression` values. The snapshot exposes the
owning tree and referenced expression through const accessors. No expression
node, source string, or token text is copied out of the syntax tree.

All definition and expression references are validated before publication.
Definitions may be shared by multiple catalog objects and expressions, but
every definition must be referenced by at least one table or index. The model
does not mutate or semantically annotate syntax trees.

### Canonical catalog records

Input names are already dequoted owned byte strings. The catalog preserves
their bytes and case exactly. Embedded NUL is rejected because SQLite catalog
names and type strings are NUL-terminated. Empty quoted names remain valid.

A catalog column stores:

- dequoted name;
- optional dequoted declared-type spelling;
- derived `TypeAffinity`;
- effective dequoted collation name, defaulting to `BINARY`;
- optional declared NOT NULL conflict action;
- optional effective NOT NULL conflict action;
- whether it participates in the declared primary key; and
- an optional default-expression reference; and
- a nullable `std::shared_ptr<const SqlValue>` named `missing_record_value`,
  used only when an older physical record predates an added column.

ADR-0036 adds `missing_record_value`. A non-null pointer may point to SQL NULL.
When a default-expression reference exists but the pointer is null, the
default remains syntactically valid but is not materialized by the current
catalog producer. Lowering preserves that distinction so complete records
remain readable and a genuinely short record fails explicitly instead of
silently substituting NULL.

The immutable shared value keeps aggregate catalog inputs copyable for
initializer-list construction without making `SqlValue` itself copyable.
Catalog publication adds at most one allocation per materialized column
default, which is acceptable for rare ALTER-added columns.

The affinity helper follows pinned SQLite's case-insensitive substring
priority:

1. any `INT` substring yields INTEGER;
2. otherwise `CHAR`, `CLOB`, or `TEXT` yields TEXT;
3. otherwise `BLOB`, or no declared type, yields BLOB;
4. otherwise `REAL`, `FLOA`, or `DOUB` yields REAL;
5. otherwise the result is NUMERIC.

The catalog keeps exact declared-type spelling separately from affinity.
Column derivation is table-context aware:

- a STRICT column must use exactly `ANY`, `BLOB`, `INT`, `INTEGER`, `REAL`,
  or `TEXT`, compared with SQLite ASCII case folding;
- a missing or custom declared type is invalid in a STRICT table;
- STRICT `ANY` has BLOB affinity rather than the ordinary NUMERIC result;
- a STRICT primary-key column that is not the rowid alias gains an implicit
  `NOT NULL ON CONFLICT ABORT` when no explicit NOT NULL exists; and
- every WITHOUT ROWID primary-key column gains the same implicit NOT NULL
  behavior, whether or not the table is STRICT.

The snapshot stores effective affinity and nullability needed by the read
binder, index comparator, metadata API, and later write path. Runtime
STRICT-value enforcement remains deferred to write and coercion code.

A catalog table stores:

- its definition, dequoted name, and root page;
- columns in declaration order;
- table CHECK-expression references in declaration order;
- an optional rowid-alias column;
- rowid PRIMARY KEY conflict behavior when applicable;
- WITHOUT ROWID, STRICT, and AUTOINCREMENT flags; and
- optional statistics.

Named constraint spelling and all other source details remain available from
the owned CREATE TABLE tree. AUTOINCREMENT requires a rowid alias. A WITHOUT
ROWID table cannot have a rowid alias.

### Complete physical index shape

An index stores its complete B-tree record order, not only the terms written
in SQL. This prevents the cursor, optimizer, and bytecode layers from
independently reconstructing SQLite's implicit suffix rules.

Index origin is one of:

- explicit CREATE INDEX;
- automatic UNIQUE constraint; or
- PRIMARY KEY.

Each index records:

- definition, name, owning table, root page, origin, uniqueness, and conflict
  behavior;
- `key_term_count`, the post-normalization value equivalent to SQLite
  `Index.nKeyCol`;
- derived `unique_not_null`, equivalent to SQLite `Index.uniqNotNull`;
- all physical terms in record order;
- an optional partial-index predicate; and
- optional statistics.

Each physical term contains a target, collation name, and effective sort
order. The target is exactly one of:

- a table-local `ColumnId`;
- a `SchemaExpression`; or
- the rowid marker.

Expression and rowid targets are explicit alternatives rather than sentinel
column numbers. Expression terms may appear only among the leading key terms.
The rowid marker may appear only as the final auxiliary term.

The normalized key prefix is the prefix described by `sqlite_stat1`. It
normally equals the SQL-visible index term list, except that a WITHOUT ROWID
primary key removes duplicate terms. The raw CREATE TABLE tree remains the
source of the written term list.

Physical record comparison uses only the normalized key prefix when
`unique_not_null` is true; otherwise it uses every stored term so duplicate
or NULL-containing keys remain ordered by their rowid or primary-key suffix.
WITHOUT ROWID primary-key payload columns are stored terms but never key
terms. Later optimizer rules may exploit an auxiliary primary-key suffix for
ordering or covering only when they name that behavior explicitly; it is not
silently part of normalized-key uniqueness.

For a rowid table, every stored index ends with the implicit rowid term. An
INTEGER PRIMARY KEY alias has no separate primary-key index. Any other
declared primary key is represented by one primary-key index.

For a WITHOUT ROWID table:

- exactly one primary-key index exists;
- the table and primary-key index share the same root page;
- the primary-key index contains its declared key terms followed by every
  remaining table column needed for the table record; and
- secondary indexes append the missing primary-key columns instead of a
  rowid.

Canonical normalization follows SQLite exactly:

- a repeated WITHOUT ROWID primary-key term is removed when an earlier term
  has the same `ColumnId` and an ASCII-case-insensitively equal collation;
  sort order does not participate, and the first term is retained;
- a secondary index omits an appended primary-key term under the same
  column-plus-collation identity test against its normalized key prefix;
- rowid suffixes use `BINARY` and ASC;
- primary-index payload columns are appended in table declaration order with
  each column's effective collation and ASC; and
- automatic UNIQUE/PRIMARY KEY constraints are equivalent when their ordered
  columns and collations match, even if sort orders differ. Equivalent
  constraints fold into one surviving index, merge compatible conflict
  policy as SQLite does, and a later PRIMARY KEY upgrades the surviving
  origin. Autoindex ordinals count only surviving indexes.

`unique_not_null` is derived with SQLite's construction-order semantics rather
than solely from final effective nullability:

- nonunique indexes and indexes with any expression key are false;
- an INTEGER PRIMARY KEY rowid-alias key is non-NULL even without a declared
  NOT NULL clause;
- an automatic UNIQUE or ordinary primary-key index uses declared NOT NULL
  state, including a NOT NULL constraint that appears later in the same
  column definition because SQLite retroactively promotes an existing
  single-column automatic index;
- STRICT and WITHOUT ROWID implicit primary-key NOT NULL processing occurs
  after automatic secondary indexes are built and does not retroactively
  promote them;
- an explicit CREATE INDEX is built later and therefore uses final effective
  nullability; and
- the WITHOUT ROWID primary index is forced true after conversion.

The snapshot stores the resulting boolean. Validation recomputes it from
index origin, rowid-alias identity, declared and effective nullability,
expression presence, and the WITHOUT ROWID primary special case rather than
trusting unconstrained input.

SQLite 3.54.0 preserves a historical ordering quirk when converting automatic
UNIQUE indexes for a WITHOUT ROWID table: appended descending primary-key
columns remain ASC and set SQLite's internal ascending-key-bug flag. Explicit
CREATE INDEX statements instead copy the primary-key term's effective sort
order. The catalog stores the actual effective order on every physical term;
the loader must reproduce this distinction without retaining a separate bug
flag.

Root-page resolution has three sources:

- a table's `sqlite_schema` row supplies the table root and also the
  WITHOUT ROWID primary-index root;
- a nonempty-SQL index row supplies an explicit index root; and
- a blank-SQL index row, whose SQL is NULL or empty, supplies an automatic
  UNIQUE or ordinary rowid-table primary-index root.

The WITHOUT ROWID primary index has an automatic
`sqlite_autoindex_<table>_<ordinal>` identity but no separate
`sqlite_schema` row. It still consumes its autoindex ordinal. Ordinals are
assigned in constraint-construction order after equivalent automatic
constraints are folded. For `sqlite_stat1`, an index name equal to its table
name identifies this rowless primary index.

All required roots must be resolved before `CatalogSnapshot::Create` is
called. The immutable model has no unresolved-root state and never publishes
a partially loaded schema.

Ordinary catalog root pages are in `[2, 0xfffffffe]`: page one belongs to
`sqlite_schema`, and `0xffffffff` exceeds SQLite's maximum page count. Table
roots are unique. Index roots are unique except for the required
table/primary-index sharing of a WITHOUT ROWID table.

### Statistics metadata

Catalog statistics preserve exact non-logarithmic values supplied by
`sqlite_stat1`:

```cpp
struct TableStatistics {
  bool has_stat1 = false;
  std::optional<std::uint64_t> estimated_rows;
  std::optional<std::uint64_t> average_row_size;
};

struct IndexStatistics {
  bool has_stat1 = false;
  std::vector<std::uint64_t> rows_per_prefix;
  std::optional<std::uint64_t> average_row_size;
  bool unordered = false;
  bool no_skip_scan = false;
};
```

For a K-key index, `rows_per_prefix` contains between zero and K+1 supplied
values: total index rows followed by the average rows sharing each successive
declared key prefix. Missing trailing values remain absent so the optimizer
can fill its explicit default estimates. `has_stat1` distinguishes no row
from a present row that supplies no usable integer. A partial index's first
value may differ from the table estimate and may be zero.

The loader follows SQLite's tolerant, sequential decoding:

- it visits at most K+1 positions while input remains;
- each position consumes leading decimal digits, records zero if there are no
  digits, and consumes at most one following ASCII space;
- a nonspace nonnumeric token therefore remains in place and fills every
  remaining visited position with zero;
- missing positions retain values supplied by an earlier duplicate row;
- extra integers and unknown trailing modifiers are ignored;
- every duplicate row resets `unordered` and `noskipscan` before applying its
  own modifiers;
- `sz=N` persists when later duplicate rows omit it and is clamped to at
  least 2; and
- noncanonical STAT1 text is not catalog corruption.

STAT1 rows are applied in `sqlite_stat1` table-scan order without sorting:

Before these steps, every non-NULL `tbl`, `idx`, and `stat` value is treated
as SQLite UTF-8 callback text and truncated at its first embedded NUL. The
NULL versus non-NULL distinction is retained after truncation.

1. A NULL table name, NULL stat text, or unknown truncated table name causes
   the row to be ignored.
2. A NULL index name selects direct table statistics.
3. An index name ASCII-case-insensitively equal to the table name calls
   primary-index lookup for that table. If no primary index exists, the row
   falls through to direct table statistics.
4. Any other index name uses schema-wide index lookup exactly as SQLite does;
   it is not required to belong to the named table. A missing index also
   falls through to direct table statistics.
5. A resolved index row mutates that index's statistics. If the resolved
   index is not partial, the table selected by the row's table name is marked
   as having STAT1 and is unconditionally assigned from the index's retained
   slot zero. A retained supplied value is copied exactly; if the index has
   never supplied slot zero, the table's `estimated_rows` is cleared so the
   optimizer uses its explicit default rather than preserving an earlier
   direct-table value.
6. A direct table-statistics row decodes one numeric slot and `sz=` metadata
   into that table, retaining a prior supplied value when the input ends
   before the slot is visited.

This propagation is required because ordinary ANALYZE omits the NULL-index
table row whenever the table has a non-partial index.

Numeric accumulation uses unsigned 64-bit wraparound. Modifier recognition is
case-sensitive and uses permissive prefixes equivalent to
`unordered*`, `noskipscan*`, and `sz=[0-9]*`. `sz=` then follows
`sqlite3Atoi` 32-bit parsing: invalid or out-of-range text becomes zero before
the minimum value of 2 is applied.

The model does not store SQLite `LogEst`, planner costs, default estimates, or
raw `sqlite_stat1` text. It retains the final exact supplied prefix and
recognized metadata after SQLite-compatible sequential application. The
optimizer later combines those values with explicit immutable defaults.

### Lookup semantics and representation

The snapshot exposes allocation-free:

```cpp
[[nodiscard]] std::optional<TableId> FindTable(
    std::string_view name) const noexcept;
[[nodiscard]] std::optional<IndexId> FindIndex(
    std::string_view name) const noexcept;
[[nodiscard]] std::optional<ColumnId> FindColumn(
    TableId table, std::string_view name) const noexcept;
```

Lookup uses SQLite's bytewise ASCII case folding through
`SqliteToLower`. Bytes at or above 0x80 compare unchanged. No Unicode
normalization, locale, allocation, temporary folded string, or collation
callback participates.

Immutable open-addressed sidecars hold IDs and use the owned object names for
hashing and equality. A maximum load factor of 0.5 provides expected O(1)
table, index, and per-table column lookup without duplicating owned name
strings. Object vectors retain construction order.

Table and index names share one schema namespace and must be unique
case-insensitively. Column names must be unique within their table.

### Validation boundary

`CatalogSnapshot::Create` performs all structural validation before
publication and returns a provenance-neutral error:

The factory validates:

- valid schema, object, column, type, and collation strings;
- case-insensitive namespace uniqueness;
- in-range table, column, index, definition, and expression IDs;
- definition statement kinds consistent with table, explicit-index, and
  automatic-index origins;
- nonempty table columns and index keys;
- valid rowid-alias and AUTOINCREMENT combinations;
- valid primary-key cardinality and WITHOUT ROWID ownership;
- complete rowid or primary-key index suffixes;
- valid root-page sharing and uniqueness;
- expression ownership by the expected DDL definition;
- valid statistic vector shape; and
- at least one catalog-object owner for every retained definition.

`CatalogInput` is canonical loader output, not raw schema input. The factory
does not replay identifier dequoting or reconstruct constraint positions from
the retained SQL. Exact table/column/index spelling, declared key order,
rowid-alias eligibility, and the selection of each DEFAULT, CHECK, index-term,
or partial-predicate expression are derived once by the catalog loader. The
factory then validates the representation-level invariants listed above,
including ownership, physical suffixes, cardinality, and cross-object
relationships. This keeps SQL interpretation in the loader instead of
embedding a second loader in the immutable model.

The model validates each invariant exactly once. A catalog loader maps a
validation failure caused by persistent schema content to
`ErrorCode::kCorruption` and includes the structured object location and
detail in its diagnostic. An internal builder or test helper maps the same
failure to `ErrorCode::kMisuse`. The model itself does not guess provenance
and callers do not duplicate validation.

Validation locations use `CatalogInput` construction coordinates. Schema
errors use owner kind `kSchema` and owner index zero. Definition, table, and
index errors use the corresponding top-level vector index. A column,
CHECK/default expression, index term/predicate, or statistic error uses its
top-level table or index as owner plus the matching `CatalogMemberKind` and
table-local/member-vector index. `kNone` uses member index zero. The detail
may name a conflicting earlier object, but provenance never changes the
coordinate system.

### Collation and function boundaries

The catalog stores effective dequoted collation names, not pointers into a
registry. Explicit spelling is preserved; an omitted collation becomes
`BINARY`. Unknown custom names remain in the snapshot because SQLite permits
schema loading before a connection supplies their implementations. A later
connection-owned resolution layer maps names to immutable `Collation`
instances when an index or expression is used and excludes an unusable index
without mutating the catalog.

Retained schema expressions likewise keep syntax-level function names rather
than `ScalarFunction*` values. The immutable model performs no function
arity, existence, or determinism resolution:

- DEFAULT constant-or-function rules are catalog-loader semantics;
- CHECK permits nondeterministic scalar functions under SQLite's rules;
- expression and partial indexes impose their own restricted-expression
  rules; and
- unknown application-defined functions are retained while loading an
  existing schema and make the affected expression unavailable until a
  connection resolves them.

Ordinary query-function resolution belongs to binding. Any connection-local
schema-expression resolution cache lives above the immutable model and must
include function- and collation-registry generation, or be fully invalidated
on every registry mutation. Prepared statements are invalidated when either
registry changes. The model never owns or mutates a `FunctionRegistry`.

Missing-collation index exclusion follows SQLite's sticky policy without
mutating the snapshot: after a connection attempts and fails to resolve an
index collation, its overlay marks that `(CatalogVersion, IndexId)` unusable
until a new catalog snapshot is published. Registering the missing collation
alone does not reactivate that index. Function resolution misses are
retryable after the function-registry generation changes.

Accordingly, the catalog-model DAG node depends directly on the immutable AST
and SQL-value affinity layers. Collation and function registries remain later
resolution dependencies rather than model ownership dependencies.

### Performance contract

Catalog construction may allocate and sort because it occurs once per schema
snapshot. Steady-state table, index, column, object, definition, and
expression access performs no allocation, string copying, or reference-count
operation.

The Node 22 Release benchmark will:

1. construct a deterministic schema with thousands of tables, columns, and
   indexes outside the timed region;
2. verify mixed hit and miss results before timing;
3. time repeated table-plus-column lookups with a deterministic query order;
4. compare equivalent preloaded, schema-qualified lookup work against pinned
   SQLite 3.54.0 through a benchmark-only C shim around
   `sqlite3FindTable` plus `sqlite3ColumnIndex`; and
5. require the Modern SQLite median to be no slower than the pinned internal
   lookup path.

The future session API may separately compare its metadata surface with
`sqlite3_table_column_metadata`, but that envelope comparison is not the
catalog lookup gate. The artifact records compiler, flags, platform, SQLite
provenance, medians, and ratio as required by ADR-0008.

## Verification

Red-first tests will cover:

- dense table, column, index, and definition identity;
- exact owned name, type, collation, source, and expression lifetime after
  the input is destroyed;
- old-snapshot lifetime after a newer version is published;
- schema-cookie and owner-generation equality;
- SQLite-ASCII-case-insensitive table, index, and column hit and miss lookup,
  including non-ASCII bytes and empty quoted names;
- duplicate table/index namespace and duplicate-column rejection;
- declared-type affinity priority against a pinned SQLite corpus;
- canonical INTEGER PRIMARY KEY rowid aliases, descending INTEGER primary-key
  indexes, ordinary primary-key indexes, and AUTOINCREMENT invariants;
- rowid-index suffixes;
- normalized primary-key terms, automatic-constraint folding, derived
  unique-nullability, and comparison length;
- WITHOUT ROWID primary and secondary index layout, root-page sharing, and
  automatic-versus-explicit descending suffix order;
- automatic-index definition ownership, duplicate folding, ordinal identity,
  three root sources, and rowless primary-index STAT1 mapping;
- STRICT type validation, ANY affinity, and implicit primary-key nullability;
- default, CHECK, expression-index, and partial-index expression references;
- unresolved function and collation names;
- tolerant, duplicate-aware table and index `sqlite_stat1` metadata,
  table-estimate propagation, unusual name resolution, overflow, and modifier
  parsing;
- invalid IDs, definitions, roots, terms, statistics, and ownership; and
- zero steady-state allocations and the Release lookup performance gate.

Project tests will enforce that the public catalog header depends only on
completed lower layers and does not include pager, B-tree, bytecode, binder,
planner, VM, session, or public API headers.

## Consequences

- Prepared statements can retain a coherent schema version without locks or
  defensive copying.
- Catalog objects use compact value IDs while preserving exact DDL syntax and
  expression ownership.
- Physical index layout has one canonical representation shared by later
  storage and optimizer code.
- Lookup is deterministic, allocation-free, and independent of locale.
- Catalog loading must finish automatic-index root resolution and structural
  validation before publishing a snapshot.
- Multi-schema search policy, writable schema mutation, and advanced schema
  objects remain explicit future decisions rather than hidden placeholders.
