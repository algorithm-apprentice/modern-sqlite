# ADR-0052: SQLite-Aligned Index Planning, Maintenance, and Statistics

- Status: Accepted
- Date: 2026-10-08

## Context

The writable MVP now has:

- immutable catalog records that already preserve complete physical index
  terms, collations, sort order, uniqueness, partial predicates, roots, and
  `sqlite_stat1` values;
- read and write B-tree cursors for SQLite index trees;
- a deterministic physical planner with table scans and rowid equality;
- typed bytecode, VM cursor descriptors, and full scans of WITHOUT ROWID
  primary indexes;
- statement-atomic table DML and schema publication; and
- pinned result, format, crash, fuzz, sanitizer, and performance harnesses.

The active `implement-index-planning` node must integrate those foundations
without reintroducing SQLite's parser/planner/VDBE cycles. Its graph
deliverable includes index creation, index scans, covering scans, selectivity
estimates, and ANALYZE/statistics support.

Pinned SQLite 3.54.0 defines the relevant mechanisms in:

- `src/build.c:3851-3942`, where CREATE INDEX scans the table, generates full
  physical index records, sorts them, checks adjacent unique prefixes, and
  bulk-inserts the result;
- `src/build.c:4020-4558`, where index names, terms, collations, sort order,
  rowid or primary-key suffixes, covering metadata, the schema row, schema
  cookie, and index refill are constructed;
- `src/build.c:4631-4669`, where missing STAT1 prefix estimates default to
  10, 9, 8, 7, 6, and then 5 rows, with a complete unique key estimated at
  one row;
- `src/where.c:3223-3655`, where equality prefixes and at most one following
  range are enumerated recursively, STAT1 prefix estimates adjust output
  cardinality, unordered indexes reject ranges, and covering avoids table
  lookup cost;
- `src/where.c:4160-4305`, where full covering-index scans compete with table
  scans using estimated row widths;
- `src/wherecode.c:1881-2245`, where index constraints become one start seek,
  one end-of-range check per candidate, index-only reads, or an index-rowid
  table seek;
- `src/delete.c:890-1022`, where each old physical index key is assembled and
  deleted before the table row;
- `src/insert.c:1889-2820`, where rowid and unique-prefix conflicts are
  checked before index and table insertion;
- `src/update.c:1042-1114`, where UPDATE checks new constraints, deletes old
  index entries, and inserts new index entries with statement atomicity;
- `src/analyze.c:166-250`, where `sqlite_stat1` is created or cleared;
- `src/analyze.c:818-882`, where STAT1 values are emitted as total rows
  followed by rounded rows-per-distinct-prefix estimates; and
- `src/analyze.c:977-1510`, where indexes are scanned in physical order and
  ANALYZE database/table/index forms refresh planner statistics.

A pinned empirical audit agrees with those source paths:

- a covering equality query emits one index cursor with `SeekGE`, `IdxGT`,
  `Column`, and `Next`, without opening the table;
- a noncovering equality query uses the same bounded index range plus a table
  rowid lookup;
- `(a,b)` supports an equality prefix on `a` and lower/upper bounds on `b`;
- unary plus and a mismatched collation prevent constrained index use;
- a deliberately unselective STAT1 value makes a noncovering query fall back
  to a table scan while the covering query still uses the index; and
- UNIQUE permits multiple NULL prefixes but rejects a repeated non-NULL
  prefix.

Modern must preserve those observable and persistent invariants while keeping
the optimizer independently inspectable and the B-tree independent of bound
expressions, bytecode registers, and session state.

## Decision

### 1. Deliver one bounded first index surface

The first executable index surface is:

- ordinary rowid tables;
- existing explicit and automatic indexes whose key terms are direct table
  columns;
- BINARY, NOCASE, RTRIM, and registered collations;
- ASC and DESC physical key order;
- nonunique and UNIQUE indexes with default/ABORT conflict behavior;
- equality prefixes using `=` or `IS`;
- optional lower and upper bounds using `>`, `>=`, `<`, or `<=` on the next
  key term;
- covering and noncovering reads;
- INSERT, UPDATE, and DELETE maintenance for every supported index on the
  target table;
- explicit column-only `CREATE [UNIQUE] INDEX [IF NOT EXISTS]`; and
- `ANALYZE`, `ANALYZE table`, and `ANALYZE index` for the main schema.

The planner retains a table-scan fallback. It does not reject a read merely
because a table also owns an index outside this first planning surface.

Mutation is stricter. A table is writable through indexed DML only when every
owned index is maintainable by this node. A partial index, expression index,
unsupported collation, unsupported conflict action, or unsupported table
shape is rejected before persistent mutation.

The following remain explicit deferrals:

- expression-index matching or creation;
- partial-index implication, creation, or maintenance;
- secondary-index lookup for WITHOUT ROWID tables;
- automatic transient query indexes and skip-scan;
- OR, IN, LIKE-prefix, ORDER BY, joins, and multi-source planning;
- `INDEXED BY`, `NOT INDEXED`, REINDEX, and DROP INDEX;
- a shared external sorter and SQLite's sorter-based bulk index build; and
- STAT4 samples.

WITHOUT ROWID tables retain their existing primary-index full-scan behavior.
Their secondary indexes are loaded and may be analyzed, but are not selected
for queries or maintained by DML in this node.

### 2. Keep index eligibility in the physical optimizer

The binder continues to resolve table columns, affinity, collation, functions,
parameters, and source identity. It does not choose an index.

The optimizer examines every catalog index for the source table in stable
catalog order. A constrained candidate requires:

1. no partial predicate;
2. direct-column key terms for every selected prefix position;
3. a comparison whose indexed operand resolves to that column after the
   existing alias and COLLATE transparency rules;
4. a source-independent opposite operand;
5. an effective comparison collation equal to the index term collation under
   SQLite ASCII name folding; and
6. no unary-plus or other affinity-changing barrier around the indexed
   operand.

The optimizer chooses the longest leftmost equality prefix and then at most
one lower and one upper bound on the immediately following key term. It does
not skip an unconstrained leading key term.

Comparison orientation is normalized. `? < column` becomes a lower bound and
`? > column` becomes an upper bound. The physical plan retains:

- the selected `IndexId`;
- table and index roots;
- equality key expressions in physical term order;
- optional logical lower and upper expressions with inclusive flags;
- each selected term's affinity, collation, and sort order;
- the original conjunct positions needed for one-time key evaluation order;
- whether ordinary `=` must reject a NULL runtime key;
- whether the access is covering; and
- the residual predicates not enforced by the B-tree range.

Selected key expressions execute once, in original WHERE-conjunct order.
Their results are then copied into physical index-term order. This preserves
the current rowid-key occurrence contract without exposing expression
evaluation to the storage layer.

For a descending range term, lowering swaps the logical lower and upper roles
when constructing physical start and end keys. Traversal always follows the
index's physical forward order.

### 3. Define covering from the actual query dependency set

The optimizer walks the retained residual predicates and projection
expressions and records every source column or rowid occurrence.

An ordinary rowid index covers the query only when every required occurrence
maps to a physical index field:

- a direct column term covers that catalog column;
- the implicit final rowid term covers `rowid`, `_rowid_`, `oid`, and the
  INTEGER PRIMARY KEY alias; and
- an expression term does not implicitly cover the columns used to compute
  that expression.

Unreferenced table columns do not prevent a covering plan. Missing physical
fields prevent it even when the index has at least as many fields as the
table.

A noncovering plan uses two read cursors:

1. the index cursor produces bounded candidates and its final rowid field;
2. the table cursor performs an exact rowid seek before residual predicates
   or projection read table fields.

A covering plan opens only the index cursor. Lowering owns the mapping from
bound source-column IDs to physical record fields; bytecode descriptors do
not acquire catalog or optimizer types.

### 4. Extend costs with SQLite-shaped exact estimates

The existing table cardinality remains:

- the table STAT1 estimate when present; or
- 1,048,576 rows otherwise.

For an index with `E` equality key terms, estimated output rows are:

1. `rows_per_prefix[E]` when STAT1 supplied that position;
2. otherwise SQLite's defaults of 10, 9, 8, 7, 6, then 5 rows for successive
   equality-prefix lengths, capped by the estimated index cardinality; or
3. one row for a complete UNIQUE `=` key, and for a complete `IS` key only
   when `unique_not_null` is true.

An `IS NULL` term without a supplied STAT1 position doubles the corresponding
equality estimate, capped by the incoming prefix estimate. A range reduces
its incoming estimate to one quarter for one bound and one sixty-fourth for
two bounds, with zero preserved and a nonzero estimate clamped to at least
one.

`IndexStatistics::unordered` allows equality access but rejects range and
full ordered scans. `no_skip_scan` is retained but has no effect because
skip-scan is deferred.

Row-width defaults mirror SQLite's simple estimator:

- TEXT, BLOB, or NONE contributes five units;
- INTEGER, REAL, or NUMERIC contributes one unit;
- every physical rowid term contributes one unit, and a table without a rowid
  alias adds one rowid unit; and
- estimated bytes are four times the summed units.

STAT1 `average_row_size` replaces the derived width when present.

Published work units remain deterministic integers:

- table scan = table rows times table row width;
- index seek = `bit_width(max(index_rows, 1))`;
- index scan = estimated output rows times index row width;
- noncovering lookup additionally pays one table seek and table-row read per
  estimated output row; and
- covering access omits that table cost.

A full index scan is considered only when it is covering, ordered, nonpartial,
and estimated cheaper than the table scan. This reproduces the pinned choice
where an unselective noncovering predicate falls back to the table but the
same covering predicate can still use the index.

Candidates are published in stable order: Empty/SingleRow, table scan, rowid
lookup, then catalog-order index candidates. Lowest work wins, followed by
estimated output, covering over noncovering, longer constrained prefixes,
rowid lookup, and stable candidate order.

The ordered covering slice publishes and selects only index candidates that
need one index cursor. The following noncovering slice admits candidates that
also require a table lookup. This delivery boundary preserves the final cost
and ordering rules while ensuring that every selected plan is executable in
the PR that first publishes it.

Stable explain output extends the existing escaped canonical-identifier
format:

```text
SCAN "<table>" USING COVERING INDEX "<index>"
SEARCH "<table>" USING COVERING INDEX "<index>" ("<column>"=?)
SEARCH "<table>" USING COVERING INDEX "<index>" ("<column>" IS ?)
SEARCH "<table>" USING COVERING INDEX "<index>" ("<column>"=? AND "<column>">=? AND "<column>"<?)
SEARCH "<table>" USING INDEX "<index>" ("<column>"=?)
```

A full covering-index scan uses `SCAN`. Any constrained index access uses
`SEARCH`. Equality terms appear in physical index order, followed by the
logical lower and upper bounds on the range term. Explain preserves `IS`
versus `=`, and preserves inclusive versus exclusive range operators.
Descending physical order does not swap the logical operators shown to the
user. Table, index, and column names use the existing byte-stable identifier
escaping contract.

### 5. Add typed index-range bytecode without planner leakage

The bytecode layer gains typed equivalents of the pinned seek/range protocol:

- open an index-backed read cursor from existing descriptor metadata;
- seek a nonempty contiguous register key with equal, greater-or-equal,
  greater, less-or-equal, or less mode;
- compare the current index record with a prefix end key and branch when the
  cursor has crossed the inclusive or exclusive end;
- read the implicit rowid field for a noncovering rowid-table lookup; and
- perform an exact rowid-table seek that returns corruption when an index
  suffix is not an integer rowid or does not resolve to a table record; and
- advance the index cursor exactly once per candidate.

`IndexBtreeCursor` gains a current-record prefix comparison primitive that
uses the same `CompareIndexRecord` contract as seek. It accepts borrowed
`SqlValue` keys only for the call and retains no VM register knowledge.

The verifier tracks both cursors independently. It requires an index-backed
descriptor for index seek/range operations, initialized key registers, a
positioned index cursor for end checks and field reads, and a rowid-backed
table descriptor for the subsequent table seek.

Key affinity is applied before the seek. Ordinary comparison keys that become
NULL branch to the empty path; `IS NULL` remains a valid key. A start seek and
one end check at the top of each loop enforce selected predicates completely,
so those predicates are not redundantly evaluated as residual filters.

No B-tree type includes bound expressions, physical-plan nodes, bytecode
addresses, or session state.

### 6. Maintain supported indexes in the same statement

Mutation binding publishes immutable index-maintenance descriptors for every
supported index on the target table. Each descriptor copies:

- root page and `IndexId`;
- uniqueness and `unique_not_null`;
- key-term count;
- physical column/rowid sources;
- collations and sort order; and
- conflict metadata.

Index expression and partial-predicate evaluation are absent from the first
descriptor by construction.

Lowering opens one typed index write cursor per descriptor and constructs old
and new full physical keys from row snapshots. The persistent ordering is:

- INSERT: preflight rowid and all unique prefixes, insert index entries in
  descriptor order, then insert the table record;
- DELETE: delete every old index entry in descriptor order, then delete the
  table row; and
- UPDATE: preflight the new rowid and unique prefixes while allowing the
  current row's old identity, delete old index entries, insert new index
  entries, then replace or move the table row.

Index-key construction reads the physical table storage value before
result-facing REAL affinity is applied. This mirrors pinned
`sqlite3GenerateIndexKey()`, which removes the preceding REAL-affinity opcode
so an integral value stored compactly in the table remains integral in the
index record.

For a UNIQUE index, a key containing NULL never conflicts. Otherwise the VM
performs a prefix seek over `key_term_count` fields. INSERT conflicts on any
match. UPDATE ignores a match only when the matched physical rowid suffix is
the old rowid of the row being changed.

Default and ABORT failures return `kConstraint`, publish zero changes for the
failed statement, and use the existing statement rollback boundary. No
partially maintained table/index image is observable.

The bytecode write-cursor descriptor is storage-discriminated. Index
descriptors carry complete collation/sort metadata, key-term count,
uniqueness, and `unique_not_null`. Typed VM operations:

- preflight a new UPDATE rowid while allowing the current row's old rowid;
- preflight a non-NULL unique prefix, optionally ignoring one old rowid;
- insert one complete physical index key; and
- delete one complete physical index key, treating a missing old key as
  corruption.

The storage writer exposes the first physical rowid matching a nonempty index
prefix. It returns no match for absence and corruption when the matched
record has no integer rowid suffix.

An indexed scan UPDATE or DELETE does not hold the persistent table mutation
cursor while mutating secondary roots. It first collects qualifying original
rowids, then reopens each row and applies table plus index changes. Exact
rowid mutation remains direct. The one-pass cursor remains selected only for
index-free stable-rowid scans.

### 7. Execute simple CREATE INDEX through the immutable pipeline

Add `BoundCreateIndex`, a logical schema mutation, a physical CREATE INDEX
mutation, and lowering through typed bytecode.

The first accepted CREATE INDEX shape requires:

- the main schema;
- an ordinary rowid table;
- one or more direct table-column terms;
- registered collations;
- no partial predicate;
- no expression term;
- no explicit NULLS FIRST/LAST syntax;
- default conflict behavior; and
- the existing maximum column and object limits.

Name resolution, shared table/index namespace checks, `sqlite_` reservation,
`IF NOT EXISTS`, exact canonical SQL, collation derivation, sort-order
normalization, rowid suffix construction, uniqueness, and `unique_not_null`
follow the catalog loader's existing rules.

Execution:

1. creates an index B-tree root with complete comparison metadata;
2. inserts the `sqlite_schema` row with type `index`;
3. scans the table in rowid order;
4. constructs and point-inserts each full physical index record;
5. detects UNIQUE prefix conflicts before publication;
6. increments the schema cookie; and
7. loads and validates a candidate catalog before statement success.

SQLite uses a sorter and bulk cursor for step 4. Modern does not add an
index-only sorter before the later shared sorting node. The first
implementation uses the already reviewed reference-faithful index insertion
path, requires bounded per-row memory, and records its fixed-work performance.
This is an execution-strategy difference, not a private format or relaxed
constraint rule.

CREATE INDEX publishes zero user changes and no last-insert-rowid event.
Failure at any root, schema, population, uniqueness, cookie, or candidate-load
boundary rolls the whole statement back.

The bytecode descriptor for the newly allocated index is an index write
descriptor with a pending root rather than a fabricated page number.
`CreateIndexRootInstruction` creates the B-tree with that descriptor's
comparison metadata, publishes the real root page to the schema-record
register, and opens the typed index capability used by population. Ordinary
open-write instructions reject pending-root descriptors.

### 8. Implement STAT1-only ANALYZE as catalog refresh

Add immutable syntax and bound forms for:

- `ANALYZE`;
- `ANALYZE name`; and
- `ANALYZE schema.name`.

Name resolution follows pinned SQLite within the accepted single-schema
surface: bare `main` selects the main database, any other unqualified name
resolves an index before a table, and a qualified name must use `main`.

ANALYZE creates `sqlite_stat1(tbl,idx,stat)` when absent or clears the
selected database/table/index rows when present. It scans each selected
physical index in order and computes:

- total index rows `K`;
- the number of distinct values for every declared key prefix `D`;
- `ceil(K / D)` for each rows-per-prefix position; and
- SQLite's special normalization from 2 to 1 when `K*10 <= D*11`.

Prefix equality uses the index term collations, sort order, and SQLite NULL
equality used by the pinned analyzer. Expression and partial indexes can be
analyzed because the operation compares stored physical records and does not
reevaluate schema expressions. A partial index does not propagate slot zero
to the table estimate.

When a table has no selected nonpartial index, ANALYZE emits the direct table
row with NULL `idx` and the table count. STAT4 tables and samples are not
created.

ANALYZE publishes zero user changes. It increments the schema cookie only
when creating `sqlite_stat1`, but always advances the in-memory catalog
generation and reloads the immutable catalog before statement success.
Prepared statements therefore reprepare against the new statistics even when
the on-disk schema cookie is unchanged.

### 9. Preserve errors, ownership, and durability

Expected SQL failures use typed results:

- missing table/index/column/collation and unsupported index shapes map
  through existing binder protocol;
- duplicate index names and UNIQUE violations map to SQLite-compatible
  constraint/schema errors;
- malformed index pages or records remain `kCorruption`;
- changed schema identity remains `kSchemaChanged`;
- resource limits remain `kTooLarge`;
- allocation failure remains `kOutOfMemory`; and
- pager, journal, VFS, and sync errors propagate unchanged.

All schema, STAT1, table, and index writes use the existing rollback-journal
ordering. No index operation weakens synchronous mode, cache limits,
corruption checks, or candidate-catalog validation.

Index cursors and write handles are RAII-owned. A statement closes or destroys
all page pins before commit, rollback, catalog publication, or Pager
transaction end.

### 10. Deliver the oversized node in ordered reviewed slices

The node is implemented sequentially:

1. this ADR and pinned behavior audit;
2. a dedicated pre-index performance baseline using immutable indexed read
   workloads;
3. index prefix comparison plus typed seek/end-range bytecode;
4. physical index candidates, STAT1 costs, stable explain output, and covering
   lowering;
5. noncovering index-to-table lookup;
6. indexed DML maintenance and UNIQUE enforcement;
7. simple CREATE INDEX execution and catalog publication;
8. STAT1-only ANALYZE and catalog-generation refresh;
9. bidirectional compatibility, crash, model, fuzz, allocation, and
   malformed-input expansion; and
10. a regenerated post-index baseline and node-completion review.

Every slice is based on the merged previous slice. No work begins on
`implement-advanced-sql` until all slices and final evidence are merged.

## Verification

Focused tests cover:

- equality, `IS`, lower, upper, and two-sided ranges on ASC and DESC indexes;
- multi-column equality prefixes and one following range;
- NULL keys, affinity conversion, collation matches/mismatches, unary-plus
  barriers, duplicate constraints, and source-independent key expressions;
- table-scan fallback for missing prefixes, partial/expression indexes,
  unordered ranges, and unselective noncovering STAT1 estimates;
- covering and noncovering rows, exact storage classes, missing trailing
  table fields, overflow index records, and rowid aliases;
- candidate ordering, costs, selected predicates, residual order, explain
  text, move semantics, limits, OOM, and allocation budgets;
- INSERT/UPDATE/DELETE maintenance across multiple indexes, unchanged keys,
  rowid moves, scan mutation, NULL UNIQUE keys, non-NULL conflicts, rollback,
  and reopen;
- CREATE INDEX empty/nonempty tables, ASC/DESC, collations, UNIQUE,
  `IF NOT EXISTS`, schema SQL, cookie, root/freelist behavior, and every
  deterministic failure boundary;
- ANALYZE database/table/index forms, empty indexes, duplicate prefixes,
  NULLs, collations, descending terms, partial indexes, exact STAT1 text,
  catalog refresh, and repeated ANALYZE;
- SQLite-created/Modern-read, Modern-created/SQLite-read, and alternating
  indexed DML ownership with `PRAGMA integrity_check`;
- crash recovery at index root allocation, schema insertion, index population,
  index maintenance, STAT1 clear/insert, statement rollback, and commit; and
- bounded SQL/database fuzz inputs with indexes.

The performance contract uses a separate version-1 indexed workload matrix.
It records at least:

- equality hit and miss;
- multi-column equality;
- one- and two-sided ranges;
- covering and noncovering access;
- selective and deliberately unselective STAT1 cases;
- indexed INSERT/UPDATE/DELETE maintenance;
- CREATE INDEX population; and
- ANALYZE.

The pre-feature baseline records Modern's table-scan fallback against pinned
SQLite's selected index. The final baseline preserves the same corpus,
statistics, page/cache configuration, result digest, and durability settings.
Profiler or diagnostic builds cannot replace uninstrumented timing.

Node completion requires:

- every supported query and mutation result to match pinned SQLite;
- every index/table image to pass pinned `PRAGMA integrity_check`;
- all deterministic crash outcomes to be old-statement or complete-statement
  images;
- no unresolved design or code review finding;
- no regression in the existing read and write baseline contracts; and
- the indexed severe-regression guards accepted by the baseline ADR.

## Consequences

### Positive

- Existing catalog, record, collation, and B-tree contracts become one
  end-to-end index implementation rather than parallel metadata systems.
- Planner choices remain immutable and inspectable before bytecode.
- Covering plans avoid table opens and rowid seeks.
- STAT1 values have one exact meaning in loader, optimizer, and ANALYZE.
- Indexed DML becomes statement-atomic and interoperable with SQLite.
- Unsupported expression/partial maintenance remains explicit instead of
  silently corrupting an index.

### Negative

- Read lowering must manage two independently verified cursors.
- Indexed scan mutation temporarily uses the rowid-list path rather than the
  one-pass table cursor.
- Point-insert CREATE INDEX is slower than SQLite's sorter-based refill until
  the shared sorting infrastructure exists.
- The first planner deliberately omits skip-scan, OR, ORDER BY, and
  expression/partial matching.

## Rejected alternatives

### Choose indexes in the binder or lowering layer

Rejected because access-path selection belongs to the optimizer and must be
inspectable before bytecode emission.

### Treat every loaded index as executable

Rejected because expression and partial maintenance require schema-expression
evaluation and implication rules that are not accepted in this node.

### Use only the first available index

Rejected because it ignores leftmost-prefix legality, collation, covering,
STAT1 selectivity, and stable cost comparison.

### Retain selected predicates only as residual filters

Rejected because scanning past the matching prefix would be correct but could
turn a lookup into an unbounded tail scan. A typed end-range check is required.

### Reconstruct physical suffixes independently in each layer

Rejected because ADR-0029 already defines one canonical complete index shape.

### Add a private CREATE INDEX file format or unjournaled rebuild

Rejected because SQLite interoperability and rollback recovery are mandatory.

### Add a one-purpose in-memory sorter

Rejected because it duplicates the later shared sorter, introduces a separate
memory/spill policy, and is unnecessary for a correct bounded first build.

### Defer ANALYZE while consuming existing STAT1

Rejected because the graph node explicitly owns ANALYZE/statistics support and
the immutable catalog requires a reviewed refresh path after statistics
change.

## References

- `docs/adr/0015-sql-values-affinity-and-comparison.md`
- `docs/adr/0016-collation-contracts-and-builtins.md`
- `docs/adr/0018-record-codec.md`
- `docs/adr/0024-read-only-btree-cursors.md`
- `docs/adr/0029-immutable-catalog-model.md`
- `docs/adr/0030-sqlite-schema-catalog-loader.md`
- `docs/adr/0035-deterministic-basic-read-optimization.md`
- `docs/adr/0036-physical-read-plan-lowering.md`
- `docs/adr/0043-sqlite-compatible-btree-mutation.md`
- `docs/adr/0045-existing-module-reference-alignment.md`
- `docs/adr/0048-writable-sql-dml-ddl.md`
- `docs/adr/0049-writable-mvp-verification-harness.md`
- `src/build.c`
- `src/where.c`
- `src/wherecode.c`
- `src/insert.c`
- `src/update.c`
- `src/delete.c`
- `src/analyze.c`
