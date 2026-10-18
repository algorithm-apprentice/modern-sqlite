# ADR-0115: Retain Statement-Shared Temporary Arenas

- Status: Accepted
- Date: 2026-10-18
- Module graph node: `implement-advanced-sql`
- Audit entry: `runtime-temporary-core`

## Context

ADR-0058 replaced one memory budget, Pager cache, and temporary file per
temporary capability with one statement-owned runtime:

- one retained-memory budget and spillable-owner registry;
- one relation arena with one ephemeral Pager, cache, transaction, and
  B-tree session;
- one logical root per relation or file-backed top-N;
- one sorter arena with one extent-managed PMA file; and
- statement-wide poison after temporary I/O, corruption, or failed pressure
  spill.

The ADR-0111 audit left this mechanism unresolved because pinned SQLite
3.54.0 uses different physical ownership:

- `vdbe.c:4685-4734` opens one `BTREE_SINGLE | BTREE_OMIT_JOURNAL`
  transient B-tree for each ephemeral cursor; and
- `vdbesort.c:308-336,1081-1177` allocates one `VdbeSorter` and its PMA file
  state for each sorter cursor.

The open question is not SQL semantics. Each Modern sorter, relation, and
top-N handle already has an independent descriptor, lifecycle, root or
extent set, position, sequence, and current-record state. The question is
whether sharing the physical Pager/cache/file and failure domain is an
acceptable measured alternative or whether it must be replaced before the
dependent sorter, ephemeral-relation, and top-N algorithm repairs.

The relevant observable boundaries are:

- query results, ordering, representative rows, and lifecycle errors;
- statement memory, cache, and file bounds as capability count grows;
- failure propagation and cleanup;
- reset/re-execution freshness;
- fixed work and matched timing; and
- absence of main-database, journal, lock, schema, or crash effects.

Temporary bytes are internal and delete-on-close. There is no persistent
format interoperability requirement.

## SQLite reference ownership

### Ephemeral B-trees

`OP_OpenEphemeral` and `OP_OpenAutoindex` allocate one VDBE cursor and call
`sqlite3BtreeOpen()` with a null path, `BTREE_SINGLE`, and
`BTREE_OMIT_JOURNAL`. Each cursor therefore owns a separate transient B-tree
and pager/cache domain. Reopening the same VDBE cursor may clear and reuse that
same B-tree.

### Sorters

Each sorter cursor owns one `VdbeSorter`. Its in-memory list, PMA readers,
subtasks, and `SorterFile` instances are cursor-local. The single-threaded
pinned profile still creates one owner per sorter cursor.

### Failure scope

The internal owners are cursor-local, but expected sorter/B-tree I/O, OOM,
corruption, and limit failures are statement-fatal:

- individual opcodes jump immediately to `abort_due_to_error`
  (`vdbe.c:987-990,9482-9520`);
- `sqlite3VdbeHalt()` runs before control returns; and
- VDBE teardown closes every cursor in the active frame
  (`vdbeaux.c:2805-2859,3326-3510`).

SQLite does not continue using sibling temporary cursors after one of these
failures.

## Modern ownership evidence

ADR-0058 retains cursor-local logical state while sharing only physical
resources.

### Relation arena

Each logical relation or file-backed top-N has a distinct B-tree root and
writer. The arena owns one Pager/cache/file and one B-tree session.

Existing focused evidence proves:

- independent keep/replace, membership, delete, seek, and iteration semantics
  in memory and file modes;
- a positioned row remains stable while a sibling root is created
  (`EphemeralRelation.PreservesPositionedRowsWhileAnotherSharedRootIsBuilt`);
- many file-mode relations use one lazy file and retain independent contents
  (`SharesOneLazyStatementArenaAcrossFileModeRelations`);
- closed roots are cleared and reused without growing shared overhead;
- root reuse with new collation metadata is correct; and
- a root-clear/write/read failure poisons the statement and makes an existing
  sibling reject further use
  (`DefersRetiredRootClearingAndPoisonsSharedArenaOnFailure`,
  `FileBackendPropagatesIoFailuresAndCleansUp`).

The relation cache capacity is one checked statement-wide soft target:

```text
max(1, min(main_cache_pages, ceil(statement_budget / page_size)))
```

Unpinned clean/dirty frames contract to that target. Live positioned cursors
may retain pinned paths, and one mutation may retain its bounded workspace.
The complete frame bound is:

```text
allocated relation-cache frames
  <= soft target
   + live positioned relation/top-N cursors * kMaximumBtreeDepth
   + kMaximumMutationPages
   + 1 in-progress fill
```

This is still materially smaller than one full cache target per root, but it
is not capability-count independent. Add relation-cache diagnostics for the
configured target, current/peak allocated frames, and current pins. A
multi-root test positions many roots simultaneously and verifies the formula.

Expose:

```cpp
std::size_t relation_cache_capacity_pages() const noexcept;
std::size_t relation_cache_page_count() const noexcept;
std::size_t relation_cache_allocated_pages() const noexcept;
std::size_t relation_cache_peak_allocated_pages() const noexcept;
std::size_t relation_cache_pin_count() const noexcept;
```

All return zero before the relation arena is created.

### Sorter arena

Each logical sorter owns independent in-memory records, stable sequence/run
ordinals, PMA extents, merge readers, and lifecycle state. The arena shares
only one pathless file and one checked extent allocator.

Existing focused evidence proves:

- two simultaneously spilled sorters open one file and keep independent runs
  (`SharesOneLazyStatementPmaFileAndReusesReleasedExtents`);
- extent split, lowest-offset fit, coalescing, tail reuse, interleaving, and
  nonoverlapping merge publication;
- stable ties and sorted results across one-run, multi-run, and multi-level
  merges;
- a positioned sibling is never selected for pressure spill;
- late read/write/corruption failures poison the statement, preserve
  publication ordering, and make a sibling reject further use
  (`PoisonsStatementAndDefersFileCleanupAfterSpillFailures`,
  `PoisonsStatementOnLateSharedPmaReadFailure`); and
- reset and close release all extents before the arena file is destroyed.

### Mixed ownership and statement bounds

The combined contract proves:

- zero files before the first spill/root;
- one live file for any number of relations/top-N handles;
- one live file for any number of sorters;
- exactly two live files when relation and sorter arenas coexist
  (`RecordSorter.CoexistsWithRelationArenaWithinTwoStatementFiles`);
- one retained-memory budget shared by every owner, with deterministic
  largest-owner pressure and ordinal tie-breaking;
- file-mode drain working sets remain bounded and included in peak
  diagnostics; and
- reset destroys the poisoned statement state and creates fresh arenas,
  ordinals, files, roots, and budgets.

## Matched evidence

The immutable aggregate baseline contains a statement with four simultaneous
file-mode aggregate modifier capabilities:

```text
wide-modifier-file
```

Lowering maps them to two ordered sorters and two keyed relations. Its Modern
diagnostic records:

- four aggregate-state opens, four finalizations, and four closes;
- 200,704 aggregate step calls over four 65,536-row probes;
- `live_files_peak = 2`;
- `file_acquisitions = 2`;
- `retained_bytes_peak = 2,097,147`;
- `working_set_bytes_peak = 2,101,243`;
- 282 sorter spill writes;
- the expected one-row, 639,215-byte result and digest
  `c9abc9b4135a16a2`; and
- a passing committed 40x guard.

The 6.321198x wall ratio is not evidence against shared ownership:

- the shared core performs two total file acquisitions across all temporary
  capabilities in the statement;
- no capability-count-proportional Pager cache or file is created;
- diagnostics record 545,693 Modern allocations; and
- ADR-0111 ledger 5 source and allocation evidence separately identifies one
  payload allocation plus one node allocation per memory record and the
  missing SQLite file-mode bulk arena.

The dependent sorter repair must address those allocation/layout mechanisms
without replacing the statement-wide physical ownership.

## Decision

### 1. Accept the shared core as a measured alternative

Retain ADR-0058's statement-shared physical arenas.

The canonical invariant is:

```text
logical capability ownership remains per cursor
physical memory/cache/file ownership is bounded per statement
```

This is not a second sorter, relation, queue, comparator, or executor. It is a
different canonical resource-owner mechanism below those algorithms,
authorized only by this ADR's matched and bounded-resource evidence.

The audit entry remains `unresolved` until the relation-cache diagnostics,
many-root bound test, and complete closure evidence pass review. It then
becomes `measured-alternative`.

### 2. Retain one shared relation Pager/cache

Every relation/top-N root remains independently addressable and clearable.
Page allocation and cache pressure may interleave across roots, but no root
may observe another root's keys, position, descriptor, or sequence.

The cache retains one statement-wide soft target and the explicit pinned-path
bound above. Do not restore one Pager/cache target per root merely to mirror
SQLite's internal object count.

### 3. Retain one shared extent-managed sorter file

Every sorter retains separate run metadata and owns nonoverlapping extents.
Publication remains atomic per run/merge. Do not restore one PMA file owner
per sorter cursor.

Ledger 5 may change record allocation and PMA buffering inside each logical
sorter, but it reuses the shared extent arena.

### 4. Retain statement-wide poison

Temporary I/O, corruption, failed publication, and failed pressure spill
remain fatal to the active VDBE statement's temporary capabilities. Once
poisoned:

- all siblings reject further operations;
- handles close without fallible cleanup;
- the VM aborts the statement; and
- reset destroys the entire temporary state.

This matches SQLite's intra-VDBE cursor cleanup and sibling-rejection boundary
even though SQLite's physical owners are cursor-local.

SQLite's connection/transaction reaction to `SQLITE_IOERR`, `SQLITE_NOMEM`,
and `SQLITE_FULL` may additionally roll back an explicit transaction.
Modern read-statement failures currently preserve that transaction. That
difference belongs to ledger 8 transaction admission/completion policy and is
not claimed as temporary-core equivalence here.

Expected non-poisoning errors remain local where ADR-0058 already specifies
them, including memory-mode `kTooLarge` before mutation and ordinary lifecycle
misuse.

### 5. Preserve the statement-wide budget and two-file bound

Do not add a capability-count multiplier, hidden per-root cache, or extra PMA
file. The public diagnostics remain the closure contract:

```text
retained_bytes <= bounded working-set rules
0 <= live_file_count <= 2
```

### 6. Keep dependent repairs separate

This resolution accepts ownership only. It does not accept:

- the sorter's per-record memory/node allocation or file-mode allocation
  policy;
- sorted-array ephemeral-relation insertion;
- linked-list memory-mode top-N;
- CREATE INDEX direct insertion instead of sorter/bulk append; or
- any planner/VM/function performance gap.

Those remain ledger 5 onward and must reuse the accepted shared core.

## Completion evidence

This is a documentation-only resolution. No production behavior changes.

Closure requires:

- SQLite source mapping for per-cursor ownership and statement-fatal errors;
- the ADR-0058 budget, file, root, extent, poison, reset, OOM, allocation, and
  lifecycle suites;
- relation-cache target/current/peak/pin diagnostics plus a many-positioned-
  root bound test;
- native/differential semantic coverage for every current consumer;
- the immutable wide-modifier matched evidence above;
- exact classification/accounting review; and
- graph, engineering-discipline, and documentation consistency checks.

## Consequences

### Positive

- capability count cannot multiply Pager caches, memory thresholds, or file
  descriptors;
- dependent algorithm repairs have one canonical temporary resource owner;
- failure cleanup remains uniform at the VM statement boundary; and
- the existing accepted SQL semantics and immutable baselines remain valid.

### Negative

- internal physical ownership does not mirror SQLite cursor for cursor;
- one temporary I/O failure poisons siblings earlier than their next direct
  access, although the statement would abort in SQLite as well; and
- shared cache pressure can change interleaving and timing between roots.

### Deferred

- sorter allocation/layout repair;
- SQLite ephemeral B-tree relation and top-N algorithms;
- worker sorter threads;
- TEMP schema persistence;
- attached-database temporary ownership; and
- shared-cache or multi-connection synchronization.

## References

- ADR-0053: SQLite-Aligned Advanced SQL Architecture
- ADR-0058: Statement-Owned Temporary Storage Budgets
- ADR-0111: SQLite Mechanism Baseline and Implemented-Engine Audit
- SQLite 3.54.0 `src/vdbe.c`
- SQLite 3.54.0 `src/vdbeaux.c`
- SQLite 3.54.0 `src/vdbesort.c`
