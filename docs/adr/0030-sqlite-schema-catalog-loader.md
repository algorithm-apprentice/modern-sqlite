# ADR-0030: SQLite Schema Catalog Loader

- Status: Accepted
- Date: 2026-10-04

## Context

ADR-0029 defines immutable canonical catalog snapshots, but intentionally does
not interpret stored SQL. A loader must now bridge the physical database and
that model without recreating SQLite's recursive prepare-time architecture.

Pinned SQLite 3.54.0 defines the relevant behavior in:

- `src/prepare.c:96-203`, where `sqlite_schema` rows are visited in rowid
  order, callback values are treated as text, stored CREATE statements are
  reparsed, and blank-SQL rows attach roots to automatic indexes;
- `src/prepare.c:225-445`, where schema cookie, schema format, text encoding,
  page count, and the schema scan are read under one B-tree transaction;
- `src/build.c:1040-1120`, where nonblank schema rows are checked against the
  object type, object name, and owning-table name derived from SQL;
- `src/build.c:1549-1768`, where names and declared types are dequoted,
  duplicate columns are rejected, affinity is derived, and later DEFAULT,
  COLLATE, and NOT NULL constraints replace earlier metadata;
- `src/build.c:1882-1960`, where a single non-descending column or table
  primary key whose declared type is exactly `INTEGER` becomes the rowid
  alias;
- `src/build.c:2437-2555`, where WITHOUT ROWID conversion normalizes the
  primary key, creates a deferred primary index for a former rowid alias,
  extends the primary index into a covering record, and rewrites automatic
  secondary suffixes;
- `src/build.c:4020-4620`, where automatic constraints are folded, explicit
  indexes are resolved, collations and sort orders are derived, and physical
  table-key suffixes are appended;
- `src/callback.c:495-526`, where clearing a loaded schema advances an
  in-memory generation independently of the on-disk schema cookie; and
- `src/analyze.c:1523-1648` and `src/analyze.c:1942-2008`, where
  `sqlite_stat1` is decoded tolerantly and applied sequentially.

The existing read pager owns the stable file snapshot and transaction. The
existing table B-tree cursor already reads page-one records and overflow
payloads. The record codec decodes SQLite values, the parser owns exact DDL
source, and the immutable catalog model validates representation-level
invariants. The loader must compose those layers without depending on a
session, binder, planner, VM, function registry, or collation registry.

## Decision

Add the loader under:

```text
include/modern_sqlite/catalog/catalog_loader.hpp
src/catalog/catalog_loader.cpp
```

### Public API and transaction ownership

The public API is:

```cpp
struct CatalogLoadOptions {
  std::string schema_name = "main";
  std::uint64_t generation = 0;
  std::size_t maximum_schema_rows = 10'000'000;
  std::size_t maximum_schema_objects = 10'000'000;
  std::size_t maximum_sql_bytes = 1'000'000'000;
  std::size_t maximum_columns = 2'000;
};

[[nodiscard]] Result<CatalogSnapshotPtr> LoadCatalog(ReadPager& pager);
[[nodiscard]] Result<CatalogSnapshotPtr> LoadCatalog(
    ReadPager& pager, const CatalogLoadOptions& options);

[[nodiscard]] Result<bool> CatalogRequiresReload(
    const ReadPager& pager, const CatalogSnapshot& catalog);
```

Both operations require an active pager read transaction. They return
`kMisuse` otherwise. They never begin, end, commit, retry, or replace the
caller's transaction.

`LoadCatalog` reads one coherent pager snapshot. An empty database produces an
empty catalog with schema cookie zero. A nonempty snapshot requires a database
header. The snapshot version uses the header schema cookie and the
caller-supplied owner generation.

`CatalogRequiresReload` compares the current snapshot's schema cookie with the
catalog version. Ordinary data-version changes do not request a reload. The
future session owner increments `generation` whenever it publishes a
replacement, including a reload whose cookie equals an older snapshot.

### Shared header compatibility

Add shared lower-layer helpers under
`modern_sqlite/storage/database_format.hpp` for effective schema format and
text encoding. The catalog loader, B-tree cursors, and storage diagnostics all
use these helpers; no layer interprets the raw header fields independently.

Raw schema format zero maps to effective format one. Values one through four
map directly; larger values return `kNotDatabase`.

The main database accepts raw text-encoding values whose low two bits are zero
or one as UTF-8, matching SQLite. Effective UTF-16 values return `kProtocol`
until UTF-16 record text and SQL parsing are implemented.

Schema format controls physical descending order. Format four honors `DESC`.
Earlier formats normalize every index term to ascending, while the
`INTEGER PRIMARY KEY DESC` rowid-alias exception still follows the parsed
declaration.

Add `ReadPager::ValidatePageNumber(PageNumber)`. It requires an active
transaction and validates nonzero, snapshot range, and the page-size-dependent
reserved locking page. `ReadPage` and the catalog loader both use this single
check.

### Physical `sqlite_schema` scan

Page one is opened as a table B-tree and visited in rowid order. Each payload
must decode as a five-field SQLite record:

1. object type;
2. object name;
3. owning-table name;
4. root page;
5. SQL text or NULL.

The four required fields must be non-NULL. Like `sqlite3_exec`, every non-NULL
field is coerced to SQLite text. Text and BLOB bytes are preserved; numeric
values use the existing SQLite-compatible text cast. Callback strings are
then truncated at the first embedded NUL.

The root must be a complete unsigned decimal 32-bit value after truncation.
Every nonzero table or index root is validated through
`ReadPager::ValidatePageNumber`, so an out-of-snapshot or reserved locking
page fails before publication. The catalog model performs page-one,
maximum-page, sharing, and uniqueness validation.

The loader counts every physical schema row before interpretation and every
surviving derived schema object. The object count starts at one for the
synthetic `sqlite_schema` table and includes tables plus named, automatic, and
rowless primary indexes. Exceeding `maximum_schema_rows` or
`maximum_schema_objects` returns `kTooLarge`; the object default matches
`SQLITE_MAX_SCHEMA`.

### Supported persistent objects

This node accepts ordinary `CREATE TABLE` and `CREATE INDEX` rows supported by
ADR-0028. It explicitly rejects persistent views, triggers, virtual tables,
generated columns, foreign keys, and other unsupported stored DDL with
`kProtocol`. Malformed records, malformed supported SQL, row/SQL identity
mismatches, orphan indexes, and impossible roots return `kCorruption`.

For nonblank SQL, the callback text must begin with ASCII-case-insensitive
`CR`, and parsing must consume the complete text. A parser
`kUnsupportedSyntax` result maps to `kProtocol`; every other parse failure
maps to `kCorruption`.

Stored persistent CREATE statements must use unqualified object names. The
dequoted SQL-derived object type, object name, and owning-table name must
match their schema-row values with SQLite ASCII case folding. The canonical
catalog preserves spelling derived from SQL rather than schema-row casing.

NULL or empty SQL is reserved for automatic-index root rows. As in pinned
SQLite, such a row resolves only by its case-insensitive index name; its type
and owning-table columns are not revalidated. An unresolved name is an orphan
index and therefore corruption. Repeated blank rows update the same pending
index root sequentially, with final structural validation performed by the
catalog model.

### SQLite dequoting and canonical names

Identifiers, declared types, and collation names are copied from their owned
source spans and dequoted with SQLite rules:

- `"..."`, `'...'`, and `` `...` `` use doubled closing delimiters;
- `[...]` uses `]` as the closing delimiter; and
- an unquoted span is preserved exactly.

The loader never locale-folds or Unicode-normalizes names. Empty quoted names
remain valid. Qualified names in persistent SQL are rejected.

Before dequoting a declared type, the loader reproduces
`sqlite3AddColumn()`'s compatibility rule: if the raw type span ends in the
ASCII-case-insensitive token `ALWAYS`, remove it and surrounding SQLite
spaces, then remove a preceding trailing `GENERATED` token and spaces when
present. A span emptied by this normalization means no declared type. An
explicitly quoted empty type remains an engaged empty string.

### Table derivation

Columns remain in declaration order. For each column the loader derives:

- dequoted name and optional dequoted declared type;
- the last declared collation, defaulting to `BINARY`;
- the last NOT NULL conflict action;
- the last DEFAULT expression;
- an affinity-applied missing-record value when the DEFAULT has an
  ALTER-compatible literal form;
- primary-key participation; and
- every column CHECK expression in parse order.

Explicit `NULL` does not clear NOT NULL metadata. Table CHECK expressions are
appended after column checks in their parse order. STRICT, WITHOUT ROWID, and
AUTOINCREMENT flags come from the parsed table definition and primary-key
semantics.

There may be at most one declared primary key. A single-key primary
declaration becomes a rowid alias only when:

- the table is not WITHOUT ROWID;
- the target is a real column;
- the dequoted declared type is exactly `INTEGER` with SQLite ASCII folding;
  and
- the declared key order is not descending.

The alias retains its primary-key conflict action and may use AUTOINCREMENT.
Every other primary key is represented by an automatic primary-key index.

For a WITHOUT ROWID table, an INTEGER primary key that would otherwise have
been a rowid alias is converted into a primary index after all other
automatic constraints. This preserves SQLite's automatic-index ordinal
ordering.

Before classifying an indexed term, a direct single-quoted string literal,
optionally under the term's outer COLLATE, is treated as an identifier for
SQLite legacy compatibility. The retained syntax tree and source remain
unchanged.

### Automatic constraints and physical index shape

Column constraints are considered in column and constraint order, followed by
table constraints. Automatic PRIMARY KEY and UNIQUE constraints resolve only
to table columns. An expression term is corruption.

Equivalent automatic constraints have identical ordered columns and
case-insensitive collations; sort order is ignored. The later constraint is
folded into the first. Two different explicit non-default conflict actions
are corruption. Otherwise an explicit action replaces a default action, and
a folded primary key promotes the surviving index origin.

Surviving automatic indexes receive
`sqlite_autoindex_<table>_<ordinal>` names in construction order. Rowid-table
indexes append `BINARY ASC` rowid. WITHOUT ROWID processing:

- removes duplicate primary-key terms by column and collation while retaining
  the first term and ignoring sort order for duplicate detection;
- appends non-key table columns in declaration order with their column
  collation and ascending order;
- appends missing normalized primary-key terms to automatic secondary
  indexes using ascending order, preserving SQLite's historical behavior.

The table schema row supplies the WITHOUT ROWID primary-index root. Every
other automatic index remains pending until a blank-SQL schema row supplies
its root.

### Explicit index derivation

An explicit index must reference an already loaded table. Key expressions are
resolved as columns only when the outer expression, after parenthesis and
outer COLLATE removal, is a single matching column identifier. Other terms
retain a `SchemaExpression`.

The effective term collation is:

1. the indexed term's final explicit COLLATE;
2. an outer expression COLLATE;
3. the referenced column collation; or
4. `BINARY`.

Classification peels any number of source-preserving parenthesis and COLLATE
wrappers before deciding whether the underlying term is a column, a legacy
single-quoted column, or an expression. The original expression tree remains
unchanged.

Default sort order is ascending, subject to schema-format normalization.
Partial predicates retain their expression reference. Rowid-table indexes
append rowid. WITHOUT ROWID explicit indexes append missing primary-key terms
with the primary key's actual sort order.

Unknown function and collation names remain unresolved, as required by
ADR-0029.

### Schema-expression resolution and normalization

Before catalog records are built, the loader clones the parsed expression
arena and statement into a validated owned tree when normalization is needed.
Expression IDs remain stable.

A loader-owned context pass then:

- resolves every non-function identifier against the owning table using
  SQLite name folding, then recognizes visible `rowid`, `_rowid_`, and `oid`
  aliases in rowid-table CHECK and partial-index contexts;
- rejects missing, ambiguous, wildcard, and qualified column references in
  schema expressions;
- rejects variables in CHECK, index-key, and partial-index contexts;
- converts legacy variables in DEFAULT expressions to NULL literal nodes,
  matching SQLite schema initialization;
- rejects column references and wildcards in DEFAULT expressions;
- recursively validates function arguments;
- validates the arity and determinism requirements of functions known to the
  immutable core function registry in CHECK, index-key, and partial-index
  contexts; and
- retains unknown application-defined function names for later
  connection-owned resolution.

CHECK permits known nondeterministic scalar functions. Expression indexes and
partial predicates require known functions to be deterministic. DEFAULT
follows SQLite's constant-or-function shape within the parser's supported
expression subset but defers function lookup and arity, matching schema
initialization. Unresolved double-quoted identifiers do not fall back to
strings: the pinned DQS-DDL policy is disabled.

ADR-0036 narrows one exception to the otherwise non-evaluating loader. It
materializes literal DEFAULT forms that SQLite permits for an ALTER-added
column, then applies the derived column affinity. NULL, signed numeric, text,
blob, and unquoted TRUE/FALSE literals are supported. Quoted identifiers never
materialize as boolean literals. The immutable `SqlValue` is owned by a
nullable `std::shared_ptr<const SqlValue>` named
`CatalogColumn::missing_record_value`; shared immutable ownership keeps
catalog input aggregates copyable. The value is used only when a record is
physically shorter than the requested field. A default expression outside
that safe literal subset remains retained but has no materialized value, so
the later VM returns `ErrorCode::kGeneric` only if a short record actually
needs it. No function is invoked and no registry pointer is stored.

### `sqlite_stat1`

After schema rows and automatic roots are resolved, the loader looks up an
ordinary rowid table named `sqlite_stat1`. If absent, statistics remain
unset. If present, columns named `tbl`, `idx`, and `stat` are selected
case-insensitively from each table record in scan order. Missing trailing
record fields behave as NULL. A same-named WITHOUT ROWID table returns
`kProtocol` until index-tree table scanning is supported.

Non-NULL values are coerced to callback text and truncated at embedded NUL.
Rows with NULL table or stat text, or with an unknown table, are ignored.

Resolution and mutation follow ADR-0029 exactly:

- NULL `idx` selects direct table statistics;
- `idx == tbl` selects that table's primary-key index when one exists,
  including an ordinary rowid table's non-alias primary key; otherwise it
  selects direct table statistics;
- every other `idx` performs schema-wide index lookup, even when the index
  belongs to a different table;
- an unresolved index falls back to direct table statistics;
- duplicate rows mutate retained values in scan order;
- missing numeric positions retain prior supplied values;
- `unordered` and `noskipscan` reset for each index row;
- `sz=` uses SQLite's signed 32-bit prefix conversion, persists when omitted,
  and is clamped to at least two;
- unsigned numeric accumulation wraps at 64 bits;
- modifier recognition is case-sensitive and prefix-compatible with pinned
  SQLite; and
- a non-partial index propagates its retained slot zero, including absence,
  to the row-selected table estimate.

Malformed and noncanonical stat text is not catalog corruption. STAT4 remains
deferred.

### Error and allocation boundary

Operational pager, cursor, record, and I/O errors retain their existing error
codes. The loader maps persistent schema inconsistencies and catalog-model
validation failures to `kCorruption`. Recognized unsupported features map to
`kProtocol`. Resource limits map to `kTooLarge`. Allocation failure maps to
`kOutOfMemory`. The options-taking overload accepts a const reference and
copies it only inside this catch boundary; the no-options overload constructs
defaults there as well.

No partial snapshot is published. Temporary rows, syntax trees, maps, and
inputs are destroyed on failure.

### Parser resource limits

Extend `ParseOne` with an optional `ParseOptions` value:

```cpp
struct ParseOptions {
  std::size_t maximum_source_bytes = 1'000'000'000;
  std::size_t maximum_columns = 2'000;
};
```

The source limit is checked before tokenization. The column limit is checked
before appending SELECT result columns, CREATE TABLE columns, table-constraint
terms, or CREATE INDEX terms. A new parser
`kResourceLimitExceeded` diagnostic is deterministic and maps to
`kTooLarge` at the loader boundary. `CatalogLoadOptions` supplies these
values to the parser.

### Complexity and performance contract

Schema scanning, DDL derivation, automatic-root attachment, and STAT1
application use case-insensitive temporary maps and are expected O(rows +
objects + terms), excluding parser work and the catalog model's final
validation.

The loader is compared with pinned SQLite's internal schema initialization on
the same read-only database, warm file cache, schema, indexes, statistics,
compiler, and optimization level. The timed region excludes database open,
pager transaction setup, corpus creation, and correctness verification. It
includes page-one traversal, DDL parsing, automatic-root resolution,
statistics loading, and immutable snapshot publication.

The initial severe-regression gate requires the Modern SQLite median to be no
slower than 2.0 times pinned SQLite. Any failure requires profiling before
review; the threshold is not a parity claim.

Five reviewed Release runs on the pinned catalog fixture produced ratios from
1.73 to 1.84. The medians across runs were 26.62 us for Modern SQLite and
14.54 us for SQLite, a 1.83 ratio. Provenance and the full configuration are
stored in the session artifact `catalog_loader_benchmark-results.json`.

## Verification

Red-first tests cover:

- active-transaction requirements, empty databases, schema cookie,
  caller generation, and reload detection;
- schema formats zero through four, pre-format-four descending normalization,
  UTF-8-compatible raw encoding values, and rejected UTF-16;
- five-field schema-record decoding, callback coercion, embedded-NUL
  truncation, row limits, roots outside the snapshot, and overflow payloads;
- SQL/row identity, full parse consumption, unsupported persistent objects,
  malformed DDL, orphan explicit and automatic indexes, qualified names, SQL
  byte limits, derived-object limits, and column/index-term limits;
- quoted and empty names, exact declared types, affinity, collations,
  trailing `ALWAYS`/`GENERATED ALWAYS` type normalization, defaults, CHECK
  order, NULL/NOT NULL replacement, STRICT, and duplicate columns;
- rowid aliases, descending INTEGER primary keys, AUTOINCREMENT, folded
  constraints, conflict actions, and deferred WITHOUT ROWID alias conversion;
- rowid and WITHOUT ROWID physical index terms, duplicate primary-key
  normalization, legacy single-quoted column terms, automatic ascending
  suffixes, explicit descending suffixes, expression terms, and partial
  predicates;
- DEFAULT variable normalization and missing-record literal materialization,
  plus CHECK, index-expression, and partial-predicate identifier/context
  validation;
- automatic-root attachment in rowid order and rowless WITHOUT ROWID primary
  indexes;
- shared raw-header normalization and locking-page root rejection;
- tolerant sequential STAT1 decoding, duplicates, overflow, modifiers,
  unusual name resolution, missing values, and partial-index propagation;
- allocation failure and deterministic error codes; and
- a pinned SQLite 3.54.0 fixture plus the matched Release performance gate.

Project tests enforce that the loader header depends only on the catalog
model, pager, and base result contracts, and that the loader source does not
depend on diagnostics, binder, planner, VM, session, or public API layers.

## Consequences

- The catalog can be reconstructed directly from a stable read snapshot
  without invoking the future VM or binder.
- DDL interpretation has one owner: the catalog loader. The immutable model
  continues to validate canonical representation rather than replay SQL.
- Automatic-index roots and WITHOUT ROWID storage order are complete before
  publication.
- Schema change detection remains cookie-based while snapshot identity also
  includes an owner generation.
- Unsupported persistent features fail explicitly instead of producing
  opaque or partially usable catalog objects.
- The future session layer owns read-transaction policy, reload retries, and
  publication.
