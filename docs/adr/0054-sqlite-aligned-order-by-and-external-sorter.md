# ADR-0054: SQLite-Aligned ORDER BY and External Sorter

- Status: Accepted
- Date: 2026-10-09

## Context

ADR-0053 makes ORDER BY and the shared spill-capable sorter the first
production slice of `implement-advanced-sql`.

The current SELECT pipeline supports:

- zero or one table source;
- WHERE;
- result projection;
- LIMIT and OFFSET;
- table, rowid, and ordinary-index access; and
- forward and reverse table/index B-tree cursor movement.

The parser currently rejects ORDER BY. The binder, logical plan, physical
plan, bytecode, and VM have no ordering contracts. The session owns the main
Pager/VFS lifetime, while the VM receives only execution-scoped capabilities.

ORDER BY cannot be implemented as `std::ranges::sort()` over all result rows:

- row count and payload size are controlled by the database;
- valid queries must spill according to the configured temporary-storage
  policy instead of exhausting process memory;
- collation, affinity, ASC/DESC, NULLS FIRST/LAST, and record storage classes
  must match SQLite;
- aliases and result ordinals must reuse the selected result expression;
- LIMIT/OFFSET is applied while draining ordered rows, not before sorting;
- reset, errors, and statement destruction must remove every temporary file;
  and
- later DISTINCT, GROUP BY, compounds, CTEs, automatic indexes, and windows
  must reuse the same temporary-record foundation.

Pinned SQLite 3.54.0 defines the relevant behavior in:

- `src/parse.y:921-956`, where ORDER BY terms carry expression, ASC/DESC, and
  NULLS FIRST/LAST syntax;
- `src/resolve.c:1508-1604` and `src/resolve.c:1826-1881`, where aliases,
  result ordinals, ordinary expressions, expression identity, and collations
  are resolved;
- `src/select.c:720-903`, where result payload and ORDER BY keys are assembled
  into one sorter record;
- `src/select.c:1740-1950`, where sorted payload fields are read and emitted;
- `src/select.c:8330-8388`, where the sorter or ephemeral ordering cursor is
  selected and ORDER BY metadata is published;
- `src/select.c:9036-9046`, where the sorted tail is drained after source
  processing;
- `src/vdbesort.c:1-150`, where in-memory runs spill to temporary
  packed-memory arrays and are merged incrementally;
- `src/vdbesort.c:1081-1171`, where PMA thresholds derive from page and cache
  configuration;
- `src/vdbesort.c:1944-2027`, where records are accepted and memory thresholds
  trigger a spill;
- `src/vdbesort.c:2517-2805`, where fan-in-16 merge trees and rewind are
  constructed; and
- `src/vdbeaux.c:4343-4353` and `src/vdbeaux.c:4884-4892`, where record
  comparison applies NULL placement and descending order.

SQLite may avoid sorting when an access path already supplies the complete
requested order. That is an optimization and follows a correctness baseline.
SQLite's bounded top-N path for LIMIT/OFFSET is not optional for this slice:
it changes which non-key result expressions execute and is therefore
observable through application-defined functions.

## Decision

### 1. Scope the first executable surface narrowly

This slice accepts ORDER BY only on the already supported simple SELECT
shape:

```sql
SELECT [ALL] result-column [, ...]
[FROM one-table [AS alias]]
[WHERE expression]
ORDER BY ordering-term [, ...]
[LIMIT expression [OFFSET expression]
 | LIMIT offset-expression , limit-expression]
```

An ordering term is:

```sql
expression [COLLATE name] [ASC | DESC] [NULLS FIRST | NULLS LAST]
```

The existing expression grammar remains authoritative. DISTINCT, joins,
GROUP BY, aggregates, compounds, subqueries, CTEs, windows, and VALUES remain
unsupported until their ordered ADR-0053 slices.

The first correctness implementation always uses an ordering capability for
an ORDER BY query. Without a positive, non-overflowing normalized runtime
LIMIT bound it uses the shared external sorter. With such a bound it uses the
shared temporary-storage module's bounded ordering relation, matching
SQLite's top-N behavior. LIMIT zero retains the existing pre-open halt.
Index-order delivery and all other sort elimination are a measured follow-up
within the ORDER BY slice.

### 2. Extend the immutable syntax tree

Add:

```cpp
enum class NullOrder : std::uint8_t {
  kDefault,
  kFirst,
  kLast,
};

struct OrderingTerm {
  SourceSpan span;
  ExpressionId expression;
  SortOrder order = SortOrder::kDefault;
  NullOrder null_order = NullOrder::kDefault;
};
```

`SelectStatement` owns `std::vector<OrderingTerm> order_by` between WHERE and
LIMIT. COLLATE remains an ordinary `CollateExpression`, preserving exact
source structure and existing expression-depth accounting.

Parsing:

- requires at least one term after ORDER BY;
- applies `maximum_columns` to ORDER BY term count, matching SQLite's column
  limit category;
- reports exact expression, collation-name, comma/end, and NULLS
  FIRST/LAST errors;
- retains statement-local spans and prepare tail behavior; and
- continues to reject ORDER BY on unsupported compound or nested forms rather
  than returning a partial tree.

### 3. Resolve aliases, ordinals, expressions, and collations like SQLite

Add:

```cpp
struct BoundOrderingTerm {
  BoundExpressionId expression;
  BoundCollationId collation;
  SortOrder order = SortOrder::kAscending;
  IndexNullPlacement null_placement = IndexNullPlacement::kFirst;
  std::optional<std::size_t> result_column{};
};

enum class SortOutputFieldKind : std::uint8_t {
  kKey,
  kPayload,
};

struct SortOutputField {
  SortOutputFieldKind kind = SortOutputFieldKind::kPayload;
  std::uint32_t field_index = 0;
};
```

Binding processes result columns before ORDER BY terms.

For each term:

1. A simple identifier matching a result alias resolves to the leftmost
   matching result column before ordinary source lookup.
2. A positive integer constant from 1 through the result count resolves as a
   1-based result ordinal.
3. An integer ordinal outside that range reports the SQLite-compatible
   out-of-range error.
4. Otherwise the term binds as an ordinary expression with source columns
   and result aliases visible under SQLite's ORDER BY rules.
5. An expression structurally identical to one or more result expressions
   records the rightmost matching result-column index so lowering may reuse
   the materialized value.

Unary plus is not an ordinal marker. `ORDER BY +1` is an ordinary constant
expression. Parentheses and an outer COLLATE preserve SQLite's transparent
alias/ordinal replacement rules.

The effective collation is:

1. explicit COLLATE on the ORDER BY expression;
2. the resolved result expression's collation when the term references a
   result alias, ordinal, or identical result expression;
3. the ordering expression's natural collation; or
4. BINARY.

Effective defaults are:

| Direction | Default NULL placement |
|---|---|
| ASC/default | NULLS FIRST |
| DESC | NULLS LAST |

Explicit NULLS FIRST/LAST overrides the default. Duplicate ORDER BY terms are
retained in source order.

### 4. Map output fields onto sort keys and payload fields

ORDER BY changes the physical evaluation pipeline.

Binding and physical planning publish one immutable output-field mapping. Each
visible result column is sourced from either:

- an ORDER BY key field when that result expression is the selected alias,
  ordinal, or rightmost identical-expression match; or
- one separate payload field.

Matching result expressions are omitted from the payload. This avoids
duplicating large values such as `SELECT blob AS x ORDER BY x` and matches
SQLite's sorter record shape.

For a full external sort, every row that passes guards and WHERE:

1. without syntactic LIMIT, evaluates non-key result expressions in
   result-column order;
2. evaluates ORDER BY keys in ORDER BY term order;
3. with syntactic LIMIT, evaluates non-key result expressions only after the
   key passes ordering admission, even when the runtime LIMIT is negative or
   its combined bound overflows to unbounded;
4. encodes one record containing all ORDER BY keys followed by only the
   required payload fields; and
5. inserts that record into the sorter.

For a positive, non-overflowing normalized `LIMIT + OFFSET` bound, the bounded
ordering relation holds at most that many rows:

1. evaluate ORDER BY keys;
2. reject the row immediately when the bounded relation is full and the new
   key cannot enter the retained prefix;
3. when full, evict the current largest record before payload evaluation;
4. reserve the accepted key as the pending candidate;
5. evaluate non-key payload expressions only for the retained row; and
6. insert the retained record, consuming the pending candidate.

After source exhaustion:

1. rewind the sorter;
2. skip OFFSET rows;
3. reconstruct visible output columns through the key/payload mapping;
4. publish rows until LIMIT or sorter exhaustion; and
5. halt through ordinary VM-owned cleanup.

Visible expressions are not reevaluated while draining. Key-backed outputs
read the key field and payload-backed outputs read their payload field. With
LIMIT, non-key expressions for rejected rows are never evaluated. Exact
evaluation order and counts are differential compatibility requirements.

When multiple ORDER BY terms reference the same visible result column, each
term is still evaluated in source order and the rightmost matching key field
becomes the output mapping for that result column, matching SQLite's repeated
mapping overwrite.

Rows equal on every ORDER BY key have SQL-unspecified relative order. Tests
that assert an exact order must include a complete deterministic key. The
sorter must not invent a rowid or sequence tie-breaker as observable SQL
semantics.

### 5. Add a lower-layer temporary-storage factory

Add a `temporary_storage` module as accepted by ADR-0053.

Session state owns the VFS, immutable temporary-storage configuration, and
one `TemporaryStorageFactory`. VM execution context borrows the factory while
the session state is alive. Pager exposes only the current main-database page
size and cache-page facts needed to derive the default threshold. The factory:

- borrows the session-owned VFS and owns configuration;
- can create pathless, exclusive, delete-on-close temporary files;
- is valid only while the attached Pager/session capability is valid;
- performs no SQL, catalog, planner, transaction, or bytecode work; and
- returns typed errors for allocation, pathless open (including backend
  unlink), read, and write failures.

`ReadSessionOptions` and `WriteSessionOptions` add a non-SQL temporary-storage
configuration used by embeddings, tests, and benchmarks:

- `TemporaryStoreMode::kFile` is the default, matching the pinned build's
  default temp-store policy;
- `TemporaryStoreMode::kMemory` disables spill files while retaining
  allocation limits; and
- an optional sorter threshold override is accepted for deterministic tests
  and benchmark fixtures.

PRAGMA temp_store remains deferred. The path and VFS overloads accept the same
options so custom VFS tests do not bypass configuration.

Read-only and writable sessions use the same factory. Temporary sorter files do not participate in the main database rollback
journal, schema cookie, transaction image, or crash recovery. Pathless files
are unlinked by VFS open and closed by RAII. Close/destructor cleanup is
best-effort under ADR-0019 and does not invent a checked-close error.

### 6. Implement one reference-faithful single-threaded sorter

Add a move-only `RecordSorter` with lifecycle:

```text
open -> writing -> rewound/positioned -> exhausted
  \        \             \               \
   +--------+-------------+---------------+-> reset/closed
```

Its immutable descriptor contains:

- total field count;
- ordered key field count;
- each key's collation, direction, and NULL placement;
- record codec options; and
- memory/PMA configuration.

The API accepts owned encoded records and exposes a borrowed current-record
view valid until next, reset, close, or destruction.

The first implementation mirrors SQLite's single-threaded sorter:

1. store owned records in a statement-local in-memory list;
2. account for record bytes and node overhead with checked arithmetic;
3. before inserting a record that would cross the threshold, merge-sort and
   spill the current nonempty list;
4. write one PMA as repeated `varint(record_size), record_bytes` entries to a
   pathless delete-on-close temporary file;
5. insert the triggering record into a new nonempty memory list;
6. at rewind, sort and spill or directly consume the remaining memory run;
7. once any spill occurs, treat two PMAs as the minimal spilled case;
8. merge up to 16 runs with a tournament tree;
9. build additional fan-in-16 merge levels when required; and
10. incrementally read records without materializing the complete merged
   output.

No worker threads are added. The ordinary build and pinned SQLite profile are
single-threaded.

The default spill threshold derives from the main database page size and
configured cache pages and is capped at 512 MiB, matching SQLite's overflow
bound. Tests may inject a smaller threshold to deterministically exercise
spill and multi-level merge paths. `temp_store=MEMORY` disables file spill
but still enforces the configured allocation/value limits.

The transient PMA bytes are not a persistent compatibility format. The
lifecycle, bounds, comparison, spill point, fan-in, incremental merge, and
failure cleanup are reference-faithful.

The bounded top-N relation is also spill capable. It uses a statement-owned
temporary keyed B-tree through the same factory and temp-store policy, with
ORDER BY keys plus an internal sequence field as its key and mapped output
payload as data. Its retained row count never exceeds the runtime bound, but
large positive parameterized bounds and large payloads may exceed memory and
therefore use temporary pager pages. Memory mode remains subject to the same
allocation/value limits. Deterministic tests cover in-memory and file-backed
top-N relations.

### 7. Reuse the canonical record comparator

Sorter records use the SQLite record codec. Comparison reads only the first
`key_field_count` fields and uses the descriptor's:

- collation;
- ASC/DESC direction; and
- effective NULL placement.

NULL compares before non-NULL before direction/null-placement inversion.
Storage classes and numeric equality use the existing SQLite-compatible
record comparison contract. Malformed sorter records are internal invariant
failures if produced by verified bytecode and corruption-style typed failures
if read back from a temporary file.

The sorter does not duplicate collation implementations, SQL affinity, text
encoding, or numeric comparison.

The record-codec module adds one allocation-free borrowed primitive:

```cpp
Result<std::weak_ordering> CompareRecordPrefixes(
    const RecordView& left,
    const RecordView& right,
    std::span<const IndexColumnOrder> columns);
```

It validates both records, compares exactly `columns.size()` fields, and
returns typed malformed-record errors. `CompareIndexRecord()` and the sorter
share one borrowed-field comparison helper so index seek and record/record
ordering cannot drift. The sorter never materializes owned `SqlValue` objects
during O(N log N) comparisons.

### 8. Add typed sorter bytecode

Program metadata owns sorter descriptors. Add strong `SorterId` values and:

- `OpenSorterInstruction`;
- `InsertSorterInstruction`;
- `RewindSorterInstruction`;
- `ReadSorterFieldInstruction`;
- `NextSorterInstruction`;
- `ResetSorterInstruction`; and
- `CloseSorterInstruction`.

Bounded LIMIT/OFFSET ordering additionally owns `TopNId` descriptors and:

- `OpenTopNInstruction`;
- `CheckTopNInstruction`, which consumes initialized key registers and
  branches before payload evaluation when the candidate is rejected and,
  when full, removes the current largest row before accepting the candidate;
- `InsertTopNInstruction`, which consumes the complete retained record and
  the pending-candidate state and inserts without any additional eviction;
- rewind/current-field/next/reset/close operations with the same read
  semantics as the external sorter.

`InsertSorterInstruction` consumes one contiguous initialized register range,
encodes one complete record, and transfers ownership to the sorter.

The verifier tracks sorter and top-N states independently from persistent
B-tree cursors:

- closed before open;
- writing after open/reset;
- positioned or exhausted after rewind;
- current-field reads only while positioned;
- insert only while writing;
- next only while positioned;
- reset from writing, positioned, or exhausted; and
- identical sorter states at control-flow joins.

For top-N, a successful check creates one pending-candidate state after any
required victim eviction. It must be consumed by exactly one insert before
another check, rewind, reset, close, or control-flow join. A rejected check
creates no pending state. If deferred payload evaluation fails, ordinary VM
error cleanup discards the scratch relation; no oversized intermediate
relation or output row is observable.

All register ranges, field indices, key counts, descriptors, and targets are
validated before execution. Halt/error cleanup closes every sorter.

### 9. Extend logical and physical plans without leaking runtime state

Add:

```cpp
enum class OrderEvaluationSchedule : std::uint8_t {
  kPayloadThenKeys,
  kKeysThenAdmissionThenPayload,
};

struct LogicalOrderNode {
  LogicalNodeId input;
  std::vector<BoundOrderingTerm> terms;
  std::vector<BoundExpressionId> payload_expressions;
  std::vector<SortOutputField> output_fields;
  OrderEvaluationSchedule schedule =
      OrderEvaluationSchedule::kPayloadThenKeys;
};
```

The logical shape for a sorted query is:

```text
Output
  Limit
    Order
      Filter/Guard
        Access
```

Order explicitly owns key evaluation, admission, and retained payload
evaluation. Without syntactic LIMIT it uses `kPayloadThenKeys`. With
syntactic LIMIT it uses `kKeysThenAdmissionThenPayload`, regardless of the
runtime LIMIT value. Limit drains the ordered stream. Output selects/reorders
mapped fields without evaluating SQL expressions. The plan therefore
represents SQLite's lazy retained-row projection rather than hiding it solely
in lowering.

Add:

```cpp
struct PhysicalSortNode {
  PhysicalNodeId input;
  std::vector<BoundOrderingTerm> terms;
  std::vector<BoundExpressionId> payload_expressions;
  std::vector<SortOutputField> output_fields;
  OrderEvaluationSchedule schedule =
      OrderEvaluationSchedule::kPayloadThenKeys;
  PhysicalSortStrategy strategy = PhysicalSortStrategy::kExternal;
  bool input_order_satisfied = false;
};
```

The first correctness plan selects a runtime LIMIT strategy when LIMIT or
OFFSET is present and `kExternal` otherwise. Lowering evaluates the existing
strict LIMIT/OFFSET registers before source access: zero halts, a negative
LIMIT selects the external sorter, and a nonnegative checked
`LIMIT + OFFSET` selects bounded top-N. Signed addition overflow selects the
unbounded external sorter, matching SQLite's `-1` combined bound. The
syntactic-LIMIT evaluation schedule remains key-first even for negative or
overflowed runtime bounds. It sets
`input_order_satisfied=false` for every first-version ORDER BY plan. A
measured follow-up may prove complete
ordering from:

- rowid table order, forward or reverse;
- an index prefix after equality-constrained terms;
- compatible collation, direction, and NULL placement; and
- a complete order, not a partial prefix.

Partial/block sorting remains disabled until its own baseline and reviewed
optimizer extension.

The future `input_order_satisfied` path has a separate evaluation contract:
it skips ORDER-only expressions, applies OFFSET before ordinary projection,
and evaluates visible result expressions once in result-column order. The
sorter-backed key-first and rightmost-key mapping rules do not apply when the
ORDER BY clause is fully eliminated.

### 10. Lower one scan phase and one drain phase

Lowering:

1. evaluates LIMIT/OFFSET and opens the selected external-sorter or
   bounded-top-N capability;
2. runs the existing access/guard/filter loop;
3. without syntactic LIMIT, evaluates payload fields and then ordering keys;
4. with syntactic LIMIT, evaluates ordering keys first;
5. for bounded top-N, rejects a noncompetitive row before non-key payload
   evaluation;
6. for any syntactic-LIMIT query, evaluates required payload fields only
   after admission, including negative/overflowed bounds that use the
   external sorter;
7. inserts the mapped encoded record;
8. closes or exhausts the source loop without retaining page pins;
9. rewinds the ordering capability;
10. applies OFFSET and LIMIT counters while reconstructing mapped output
   fields;
11. emits result rows; and
12. closes through explicit or halt-owned cleanup with verifier-compatible
   states.

LIMIT zero retains existing pre-open short-circuit behavior and does not open
the table or sorter. Runtime LIMIT/OFFSET conversion remains the existing
strict `MustBeIntegerInstruction` behavior.

An empty input rewinds to the empty target and returns no rows. Errors while
building or draining the sorter flow through the normal statement error path.
Read-only statements perform no main-database mutation.

### 11. Preserve inspectable planning and explain output

`ExplainPhysicalPlan()` adds one stable Sort line containing:

- term count;
- normalized direction and NULL placement;
- collation names; and
- whether the order is sorter-backed or access-path-satisfied.

Sorter descriptors and verifier metrics are inspectable through bytecode
tests. Temporary filenames and host paths are never exposed as SQL behavior
or stable explain text.

### 12. Test red-green in ordered increments

Implementation is split into:

1. syntax and AST;
2. binding and result-alias/ordinal resolution;
3. logical and physical sort nodes;
4. in-memory `RecordSorter`;
5. PMA spill and incremental merge;
6. typed bytecode and verifier;
7. VM execution and session temporary factory;
8. plan lowering;
9. pinned SQLite differential/session coverage;
10. failure, OOM, allocation, model, and fuzz coverage;
11. correctness-complete performance baseline; and
12. measured index-order elimination only if the baseline justifies it.

Each increment has one focused failing test before production behavior.

### 13. Require exact correctness and failure evidence

Coverage includes:

- ASC, DESC, default direction, NULLS FIRST, and NULLS LAST;
- BINARY and NOCASE plus registered custom collations;
- integer, real, text, blob, NULL, mixed storage classes, and equal keys;
- aliases, ordinals, parenthesized ordinals, unary plus, duplicate aliases,
  rightmost duplicate ORDER BY output mapping, source-column precedence, and
  ORDER BY expressions absent from projection;
- one row, empty input, multiple pages, overflow payloads, and WITHOUT ROWID
  scans;
- WHERE plus ORDER BY;
- LIMIT, OFFSET, comma LIMIT syntax, LIMIT zero, negative LIMIT,
  LIMIT-plus-OFFSET overflow, parameterized/reset strategy changes, and
  runtime type mismatch;
- non-deterministic result/ordering expressions and exact evaluation counts;
- statement reset and repeated execution with different bindings;
- in-memory, minimal two-PMA spill, multiple-PMA, and multi-level fan-in-16
  merge;
- in-memory and file-backed bounded top-N with large parameterized bounds and
  large payloads;
- malformed PMA length, truncation, and invalid record bytes;
- every pathless-open/unlink, read, write, and allocation failure;
- cleanup after insertion, rewind, current-row, next, reset, halt, error,
  finalize, and destruction;
- no leaked file, descriptor, page pin, sorter record, or allocation; and
- unchanged main-database bytes for read-only execution.

The public differential matrix compares rows, order, storage classes,
prepare/step error phase, reset behavior, and limits against pinned SQLite.
Fuzz input includes bounded ORDER BY syntax and bounded database images.

### 14. Establish a separate ORDER BY performance contract

The existing read and index baselines remain immutable.

A new version-1 ORDER BY workload records at least:

- in-memory integer ordering;
- mixed-type/collated ordering;
- descending and explicit NULL placement;
- noncovering payload ordering;
- deterministic minimal spill with two PMAs;
- deterministic spill with multiple PMAs;
- LIMIT/OFFSET after full sort;
- bounded top-N in memory and after temporary B-tree spill;
- index-compatible ordering before elimination; and
- the same index-compatible ordering after the measured optimizer follow-up.

Configuration pins page size, cache pages, temp-store mode, sorter threshold,
merge fan-in, mmap, corpus, SQL, result digest, and fixed work. Modern and
SQLite use matched settings. Every repetition reaches the configured timing
floor.

The correctness implementation, including bounded top-N behavior, is measured
before optimization. Index-order elimination, partial sorting, and
sorter-reference payload fetches are admitted only after fixed-work
diagnostics or profiling identify their specific gap. The accepted
severe-regression guard is 10x wall and CPU in aggregate and every paired
round.

## Explicit deferrals

This slice does not implement:

- DISTINCT, GROUP BY, aggregates, compounds, VALUES, joins, subqueries, CTEs,
  windows, views, triggers, or advanced DML;
- aggregate-local ORDER BY;
- ORDER BY on UPDATE or DELETE;
- partial/block sorting;
- sorter references that defer payload fetches;
- parallel sorter workers;
- hash or merge joins; or
- persistent TEMP schema objects.

Each deferred behavior remains assigned to ADR-0053.

## Consequences

### Positive

- ORDER BY reaches the public session through the canonical immutable
  pipeline.
- Visible output is reconstructed from mapped key/payload fields without
  reevaluation during sorter drain.
- Database-sized results are bounded by spill rather than process memory.
- Later advanced SQL slices receive one reusable sorter and temporary factory.
- Main-database bytes and transaction semantics remain untouched by read-only
  sorting.

### Negative

- The first correct plan sorts even when an existing index could provide the
  order.
- Spill support adds a new VFS-backed failure surface before later SQL
  features consume it.
- Sorter record payload duplicates projected values until a measured
  sorter-reference optimization is justified.
- Equal-key output order remains intentionally unspecified.

## Rejected alternatives

### Sort a vector of `SqlValue` rows

Rejected because valid query cardinality is unbounded, it duplicates value
comparison and ownership, and it cannot exercise SQLite-style spill failures.

### Add ORDER BY syntax before the sorter contract

Rejected because accepted syntax must reach observable execution in one
vertical slice and unsupported execution must not masquerade as success.

### Reevaluate projection after sorting

Rejected because aliases and non-deterministic expressions would execute more
times than SQLite and source cursors may no longer be positioned.

### Use the main database B-tree as scratch

Rejected because read-only ORDER BY must not modify the main database,
journal, schema cookie, or crash image.

### Implement index-order elimination first

Rejected because correctness and a fixed-work baseline must precede
optimization, and the shared sorter is required by later slices regardless.

### Defer top-N sorting

Rejected because pinned SQLite may avoid evaluating non-key projection
expressions for rows rejected from a bounded LIMIT/OFFSET ordering relation.
That behavior is observable through application-defined functions and belongs
to the correctness contract, not a later performance-only optimization.

## References

- ADR-0003: Layered Dependency Architecture
- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0005: Test-Driven Development
- ADR-0008: SQLite Performance Parity Strategy
- ADR-0018: SQLite Record Codec
- ADR-0019: Virtual File System Contracts
- ADR-0028: Pure SQLite-Compatible SQL Parser
- ADR-0031: Typed Immutable Bytecode Programs
- ADR-0033: Immutable Read-Only SELECT Binding
- ADR-0034: Immutable Logical SELECT Plans
- ADR-0035: Deterministic Basic Read Optimization
- ADR-0036: Physical Read Plan Lowering
- ADR-0037: RAII Read Session API
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0052: SQLite-Aligned Index Planning, Maintenance, and Statistics
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
