# ADR-0117: SQLite Ephemeral Relation B-Tree

- Status: Accepted
- Date: 2026-10-19
- Module graph node: `implement-advanced-sql`
- Audit entry: `runtime-ephemeral-relation`

## Context

Modern's `EphemeralRelation` preserves SQLite-visible membership, duplicate,
replacement, delete, ordering, lookup-prefix, reset, and failure semantics.
Its memory representation is not SQLite-shaped.

Membership-purpose relations currently retain an ordered array of
`ByteBuffer` records. Insertion performs binary search, then shifts the
complete suffix. Descending or otherwise adverse high-cardinality input is
therefore quadratic. The committed DISTINCT/compound baseline records:

- `distinct-high-card-memory`: 26.546831x SQLite;
- `union-replace-file`: 17.579147x; and
- `union-keyed-relation-before-file`: 12.902891x.

File-backed and automatic-lookup relations already use the canonical
`IndexBtreeWriter` and `IndexBtreeCursor` through the statement-shared
relation arena. File-mode membership relations, however, still pay sorted
array admission until their fairness threshold is crossed.

Pinned SQLite 3.54.0 uses one transient B-tree for these operations:

- `vdbe.c:4650-4765` opens `OP_OpenEphemeral` and `OP_OpenAutoindex` with
  `BTREE_OMIT_JOURNAL | BTREE_SINGLE`;
- `vdbe.c:5480-5585` probes encoded index keys through `OP_Found` and
  `OP_NotFound`;
- `vdbe.c:6680-6860` inserts and deletes encoded index records;
- `btree.c:2540-2645` selects `BTREE_MEMORY` when `temp_store=MEMORY`; and
- `main.c:2707-2739` defines the file-versus-memory temporary policy.

SQLite's memory temporary database is a Pager/B-tree database whose dirty
pages remain in memory. It does not replace the B-tree with a sorted array.

ADR-0115 retains Modern's statement-shared physical relation arena as a
measured ownership alternative. This repair preserves that owner: logical
relations keep distinct roots, while the statement owns one relation Pager.
It changes the relation algorithm, not the accepted sharing policy.

This slice does not change:

- record encoding, comparison, affinity, collation, NULL, or representative
  replacement semantics;
- relation bytecode or VM lifecycle shape;
- automatic-index physical plans or probe widths;
- B-tree page format, balancing, cursor, or writer algorithms;
- file-mode root reuse and statement-wide poison behavior;
- `BoundedTopN`, sorter, CREATE INDEX, planner, or aggregate algorithms; or
- main-database, journal, lock, schema, or crash behavior.

## Decision

### 1. Add a true fileless ephemeral Pager mode

Add:

```cpp
[[nodiscard]] static Result<std::unique_ptr<Pager>>
Pager::OpenEphemeralMemory(Vfs& services, PagerOptions options = {});
```

The Pager still borrows the session VFS for narrow services such as random
bytes, but it opens no `File` and performs no VFS read, write, sync, truncate,
or lock operation.

The mode reuses the existing ephemeral write state:

- `BeginRead()` publishes an empty snapshot;
- `BeginWrite()` acquires no database lock;
- page writes require no rollback journal;
- commit, rollback, and savepoints remain unavailable because the Pager is
  destroyed with the statement arena; and
- arena destruction discards the complete cache.

Every allocated page remains dirty and resident. A cache miss for an existing
memory page is an internal invariant failure; there is no backing file from
which to reload it.

The cache capacity is also the hard logical page limit. Before allocating a
new page, reject a candidate above `cache_capacity_pages` with typed
`kTooLarge`. This prevents dirty-page overflow from silently exceeding the
memory temporary bound. The reserved locking page cannot be reached under
the existing 512 MiB maximum threshold and supported page sizes.

The in-memory Pager introduces no alternate page, cursor, or balancing
algorithm. It changes only where the canonical Pager retains ephemeral pages.

### 2. Select one relation-arena storage mode

`StatementState::EnsureRelationArena()` creates the same one-Pager,
one-`BtreeWriteSession` arena in both temporary modes:

- file mode uses the existing pathless delete-on-close transient database;
- memory mode uses `Pager::OpenEphemeralMemory()`.

Both use:

```text
threshold_pages = max(1, ceil(statement_budget / page_size))
file_cache_pages = max(1, min(main_cache_pages, threshold_pages))
memory_cache_pages = threshold_pages
```

File mode retains the existing soft cache target because clean/dirty pages can
write back to the transient file. Memory mode is non-purgeable and must not
turn the main database's cache target into a logical temporary-database size
limit. Its resident cache and hard database page limit therefore derive only
from `threshold_pages`.

Page one and each live relation root consume pages from the memory-mode shared
limit. Clearing a root returns child and overflow pages to the ephemeral
freelist; root retirement and reuse remain unchanged.

The memory arena:

- opens no temporary file;
- leaves `live_file_count()` and `file_acquisitions` at zero;
- reports the existing relation-cache diagnostics; and
- remains outside retained-owner accounting, as ADR-0058 already specifies
  for the separately bounded shared relation Pager cache.

### 3. Use the B-tree from the first accepted record

Remove the ordered `ByteBuffer[]` backend, its growth accounting, binary
search, suffix shifts, migration, and pressure-spill callback.

An empty logical relation remains physically lazy under ADR-0115. On the
first accepted insertion, acquire one relation root from the shared arena and
use it for the relation's complete lifetime. This applies to:

- membership relations in memory mode;
- membership relations in file mode; and
- lookup relations in both modes.

`EphemeralRelation::file_backed()` continues to describe physical file
ownership. It returns true only for a B-tree root in file temporary mode.
Internal dispatch uses a separate `has_btree()` predicate so a memory B-tree
is not mislabeled as file-backed.

All operations use existing canonical primitives:

- `ContainsEncoded()` for key membership;
- `InsertEncoded()` for a missing key;
- `ReplaceEncoded()` for replacement;
- `DeleteEncoded()` for erase;
- `IndexBtreeCursor::First()`/`Next()` for ordered drain; and
- `Seek()`/`CompareCurrent()` for repeatable lookup-prefix iteration.

No relation-specific tree, hash table, comparator, or executor is added.

Delivery update: ADR-0154 replaces the redundant membership/replace wrapper
sequence for ordinary replace-existing insertion with the canonical
`InsertOrReplaceEncoded()` outcome. One positioning result determines
insertion versus representative replacement. Keep-existing dispatch and
the saturated record-count guard retain their existing contracts.

### 4. Preserve representative and lifecycle semantics

The relation count changes only after successful B-tree publication.

- keep-existing returns `kDuplicate` without changing the stored record;
- replace-existing retains the new complete representative record;
- erase returns false for a missing key;
- rewind returns records in canonical key order;
- lookup seek returns the first equivalent complete record and advances only
  while the requested prefix remains equivalent;
- reset clears the existing root and keeps it available for reuse; and
- close retires the root through the accepted arena pool.

Empty membership and lookup handles remain file/cache lazy. `Contains()` and
`Erase()` return false without opening an arena; empty rewind exhausts; empty
sealed lookup seek misses.

### 5. Make B-tree mutation failure statement-fatal

Sorted-array admission could reject some memory-budget growth before
publishing a mutation. The B-tree may discover OOM, page-limit, or backend
failure after it has begun a multi-page mutation. Match SQLite's VDBE failure
scope:

- mark the relation arena failed;
- close the current relation;
- reject all sibling temporary capabilities; and
- destroy the arena at statement cleanup.

Do not offer relation-local retry after a failed root creation, insertion,
replacement, delete, clear, cursor read, or page-limit transition.

Opening the supporting file must also retain typed OOM cleanup across every
supported standard library. The POSIX VFS resolves an existing opened path
with dynamically sized `realpath()` storage and preserves the existing trusted
`TMPDIR`, `TMP`, `TEMP`, `TEMPDIR`, then `/tmp` temporary-directory search,
including rejection of the first selected variable when it is empty. The VFS
stages the selected template in an owned string before `mkstemp()` and installs
non-allocating descriptor and unlink guards immediately after file creation.
It does not use allocation-bearing `std::filesystem::path` canonicalization or
path mutation in these open operations. This keeps allocation failure inside
the existing typed Pager boundary, without depending on a standard-library
path implementation's partial-construction cleanup or leaving an orphaned
temporary file.

Expected pre-mutation validation errors remain nonmutating:

- invalid descriptor;
- invalid insert mode;
- malformed record;
- wrong record/key field count; and
- invalid lifecycle or lookup-prefix width.

Positioning an overflow record may retain one copied current record through
the relation's existing memory account. The shared relation cache remains
separately bounded and is not double-charged.

### 6. Freeze mechanism and performance contracts

Add focused tests for:

- memory-mode insertion creating relation-cache pages without any VFS
  temporary open or file acquisition;
- memory capacity remaining governed by the temporary threshold when the main
  Pager cache target is smaller, including a one-page main cache;
- file-mode membership using a B-tree from the first accepted record;
- multiple memory roots sharing one Pager/cache and retaining independent
  contents;
- the hard in-memory page limit below, at, and above the page-one/root
  minimum;
- descending high-cardinality insertion without an ordered-array backend;
- keep/replace/delete/contains equivalence in both storage modes;
- collation, numeric equality, NULL, overflow, prefix lookup, reset, and root
  reuse in both modes;
- memory-Pager cache-miss invariants;
- OOM at memory Pager creation, cache/page allocation, root creation, and
  every encoded B-tree operation;
- statement poisoning and cleanup after memory and file B-tree mutation
  failures; and
- unchanged many-root cache, pin, and shared-file bounds.

Generate a fresh DISTINCT/compound baseline against the committed immutable
baseline. Preserve every result, digest, workload, deadline, configuration,
and 40x guard. Record:

- `distinct-high-card-memory`;
- `union-replace-file`;
- `union-keyed-relation-before-file`;
- low-cardinality DISTINCT controls;
- EXCEPT/INTERSECT membership controls; and
- ordered compound consumers that reuse sorters or top-N.

The high-cardinality memory case must materially improve. Timing evidence
does not replace the focused diagnostic that memory mode uses the canonical
B-tree and opens no file.

Current-head aggregate diagnostics require zero retained-owner bytes for
relation-only DISTINCT state and unique ordered DISTINCT state. The immutable
aggregate baseline v1 keeps its original positive retained-owner expectation
under an explicit v1 validation scope because those artifacts predate this
repair. Neither contract accepts both mechanisms for the same source scope.

Run:

- Pager memory-mode unit/OOM/allocation tests;
- complete ephemeral relation semantic, malformed, OOM, allocation, cache,
  root-reuse, and failure suites;
- DISTINCT, compound, aggregate-DISTINCT, and automatic-index dependent
  suites;
- Debug, Release, ASan/UBSan, instrumentation, GCC 16 Debug/Release;
- read/write SQL and database fuzz;
- native compatibility/performance contracts;
- full production clang-tidy, pinned formatting, graph, and engineering
  discipline; and
- exact-tree review and hosted checks.

## Consequences

### Positive

- every relation mode uses SQLite's logarithmic ephemeral B-tree mechanism;
- high-cardinality adverse insertion no longer shifts an ordered array;
- memory mode performs no physical file I/O;
- relation semantics reuse one canonical writer/cursor implementation; and
- top-N can reuse the same repaired B-tree mechanism in ledger 7.

### Negative

- Pager gains a fileless ephemeral mode and a hard page limit;
- memory-mode minimum useful capacity becomes page-granular;
- relation data moves from retained-owner diagnostics into the separately
  bounded shared relation-cache diagnostics; and
- a memory B-tree mutation failure is statement-fatal rather than
  relation-locally retryable.

### Deferred

- B-tree-backed top-N admission/deletion;
- Bloom filters for automatic joins;
- sorter-backed CREATE INDEX;
- memory-Pager savepoints or persistence;
- TEMP schema databases; and
- WAL or multi-connection temporary storage.

## Implementation evidence

Design commit `caa6912`, core implementation commit `f0cfe4d`, POSIX OOM
cleanup commit `95139c9`, aggregate ownership-contract commit `3b37ce1`, and
const-only quality correction `e2b191a` implement this decision.

Memory and file relations now acquire a canonical index B-tree root on the
first accepted record. Memory mode uses a fileless ephemeral Pager whose
resident cache and hard page limit derive from the temporary threshold; file
mode uses the existing delete-on-close transient Pager. Both modes reuse
`IndexBtreeWriter` and `IndexBtreeCursor` for insert, replacement, erase,
membership, ordered drain, and prefix lookup. Empty handles remain lazy, root
reuse preserves fresh comparison metadata, and any mutation failure poisons
the statement arena.

Focused coverage includes:

- no VFS open or file acquisition for memory relations;
- a one-page main cache with a larger independent temporary threshold;
- page-one/root limits below, at, and above the minimum useful capacity;
- first-record B-tree ownership in both modes;
- representative replacement, collation, numeric, NULL, overflow, ordered
  rewind, prefix lookup, reset, and root reuse semantics;
- many positioned roots under one bounded cache;
- exhaustive memory-Pager and relation allocation failures;
- statement poisoning after page-limit and backend failures; and
- cross-standard-library session OOM cleanup without leaked descriptors or
  visible pathless temporary files.

Fresh exact-input DISTINCT/compound evidence generated from core
implementation revision `f0cfe4d` preserves every result, digest, workload,
configuration, deadline, and the committed 40x guard:

| Workload | Prior wall ratio | New wall ratio |
|---|---:|---:|
| high-cardinality memory DISTINCT | 26.546831 | 8.904458 |
| file UNION replacement | 17.579147 | 16.182973 |
| ordered UNION merge | 8.294706 | 6.721865 |
| long mixed compound | 2.156691 | 1.793925 |
| low-cardinality memory DISTINCT | 4.047993 | 9.522887 |
| collated memory DISTINCT | 4.293979 | 9.178163 |
| DISTINCT ORDER/LIMIT | 9.393792 | 17.236709 |
| keyed UNION before file pressure | 12.902891 | 13.949456 |

The high-cardinality adverse case materially improves after removing suffix
shifts. The low-cardinality controls retain the expected fixed B-tree page
cost, and the DISTINCT ORDER/LIMIT case remains assigned partly to ledger 7's
unrepaired top-N mechanism. Current aggregate diagnostics record zero
retained-owner bytes for relation-only DISTINCT state; immutable aggregate
baseline v1 validation retains its historical positive expectation under the
explicit v1 contract. The fresh evidence is retained as
`ephemeral-relation-distinct-compound-baseline-v1`. It is retained
implementation evidence rather than a committed baseline replacement or
final-head timing artifact; the later closing commits do not change the
relation algorithm, while `95139c9` does change POSIX temporary-path setup.

Fast-local passes 1,426 batched and 92 isolated tests. Isolated Debug,
Release, ASan/UBSan, and genuine GCC 16 Debug/Release each pass 1,560 tests;
instrumentation passes 1,577. Twenty-seven shared checks reconcile 121
configured instances, and 58 native compatibility/performance contracts
pass. Four fresh 20,000-run fuzz domains complete with seed 102. Full
production clang-tidy, pinned formatting, graph, engineering discipline, and
whitespace checks pass.

GPT-5.6 Sol design review corrected an initial proposal that would have
limited the memory temporary database by the main Pager cache target.
Implementation review approved the canonical B-tree reuse. Closing review
then found libstdc++ allocation-failure crashes in filesystem path mutation,
an orphan-file window after `mkstemp()`, lost temporary-directory environment
precedence, fixed-buffer `realpath()` narrowing, and historical aggregate
contract coupling. The final POSIX path stages, non-allocating cleanup guards,
trusted environment lookup, dynamic canonical path owner, isolated OOM
regression, and source-scoped aggregate contracts close those findings.

## References

- ADR-0056: SQLite-Aligned DISTINCT, VALUES, and Compound SELECT
- ADR-0058: Statement-Owned Temporary Storage Budgets
- ADR-0089: Automatic Covering Join Indexes
- ADR-0111: SQLite Mechanism Baseline and Implemented-Engine Audit
- ADR-0115: Retain Statement-Shared Temporary Arenas
- ADR-0116: SQLite Sorter Record Allocation
- SQLite 3.54.0 `src/vdbe.c`
- SQLite 3.54.0 `src/btree.c`
- SQLite 3.54.0 `src/pager.c`
- SQLite 3.54.0 `src/pcache1.c`
