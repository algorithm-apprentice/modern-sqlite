# ADR-0051: SQLite-Aligned One-Pass Scan Mutation

- Status: Accepted
- Date: 2026-10-08

## Context

The canonical Node 40 baseline is complete and valid, but the
`matched-durable` scan-mutation cases expose a repeatable gap:

- `delete-scan-implicit` records a 10.44x aggregate wall ratio, with all
  paired rounds between 10.24x and 10.87x;
- `update-scan-implicit` records a 9.54x aggregate wall ratio, with one paired
  round at 10.07x; and
- all output images, changes, row counts, digests, durability profiles, and
  final database fingerprints remain correct.

The delete diagnostic records 4,380 Modern database-page writes, 7,738
main-journal syncs, 1,168,839 B-tree comparisons, and 917,509 VM instructions.
Pinned SQLite records 73 cache writes and 196,616 VM steps for the same fixed
work.

This satisfies ADR-0008 optimization admission:

1. the deterministic workload and matched SQLite control are named;
2. three paired rounds reproduce the gap;
3. timing and fixed-work counters identify the hot path;
4. the pinned native mechanism is auditable locally;
5. this ADR records the selected mechanism;
6. the complete integrated path will be remeasured; and
7. writable correctness, crash, fuzz, interoperability, and non-target
   performance gates remain mandatory.

SQLite does not use the truncate/`OP_Clear` optimization for
`DELETE FROM kv WHERE k>=1`. `src/delete.c:496-528` requests a one-pass WHERE
plan, and `src/delete.c:731-859` preserves the data-cursor position with
`ONEPASS_MULTI` and `OPFLAG_SAVEPOSITION`.

Preparing `EXPLAIN DELETE FROM kv WHERE k>=1` through the pinned 3.54.0 shared
library yields `SeekGE`, `Delete` with P5 value `2`, and `Next`, with no
`Clear`. `src/sqliteInt.h:4126` defines `OPFLAG_SAVEPOSITION` as `0x02`;
`src/vdbe.c:6022-6027` and `src/vdbe.c:6097-6124` document and implement the
saved table-cursor position.

Modern currently lowers stable-rowid table-scan DELETE and UPDATE by:

1. reading and snapshotting one source row;
2. closing the read cursor;
3. mutating through a separate point writer;
4. reopening the read cursor; and
5. seeking strictly greater than the previous rowid.

That safety-first path was correct for the writable MVP, but it repeats B-tree
search and page-lifetime work for every row.

The existing `TableBtreeWriter::Clear()` is not a valid substitute. The
predicate `k>=1` is not universally true for SQLite rowids because negative
rowids are legal, and future residual predicates must continue to observe
row values.

## Decision

### 1. Add a persistent one-pass table mutation cursor

Add a narrow B-tree write-cursor contract for rowid tables that can:

- position by the existing rowid seek rules;
- expose the currently positioned immutable row snapshot;
- delete the current row while preserving the logical successor position;
- replace the current row when the rowid remains stable; and
- advance exactly once without reopening or re-seeking from the root.

The contract remains below the VM and depends only on existing Pager, B-tree,
record, value, and collation layers. It is not a general iterator framework or
an index-planning abstraction.

### 2. Mirror SQLite one-pass eligibility

The planner/lowering path may select one-pass mutation only when:

- the physical access is a rowid-table scan;
- DELETE keeps no row after a successful mutation;
- UPDATE preserves every target rowid;
- source values are snapshotted before mutation;
- residual expressions cannot observe a mutated later row; and
- no unsupported index, trigger, foreign-key, view, or virtual-table behavior
  is involved.

Exact-rowid mutation remains on the point path.

Rowid-changing UPDATE remains on the existing two-phase rowid-list path. A
moved row must never be revisited, and conflict detection must still occur
against the complete original target set.

### 3. Preserve durable ordering and statement atomicity

The optimization changes cursor lifetime and traversal only.

It does not change:

- rollback-journal record encoding;
- journal-before-database ordering;
- cache capacity or durability settings;
- implicit or explicit transaction boundaries;
- statement rollback mode;
- change counting or last-insert-rowid;
- corruption detection; or
- failure cleanup.

Every persistent cut introduced or reordered by the cursor implementation
requires deterministic fault-injection coverage. Existing crash and
interoperability matrices must remain green.

### 4. Do not pre-authorize pager spill redesign

The first implementation reproduces SQLite's one-pass cursor mechanism and
remeasures the complete SQL path.

The baseline also shows many Modern journal syncs, but this ADR does not
authorize speculative journal batching, cache growth, weaker sync, or a
second spill algorithm. If one-pass mutation does not satisfy the guard,
another measured profile and ADR amendment must identify the remaining
SQLite pager mechanism and its crash-ordering prerequisites.

### 5. Use the canonical write baseline as the acceptance gate

The node is complete only when:

- every Node 39 writable correctness, crash, fuzz, and interoperability gate
  passes;
- focused tests cover first, middle, last, empty, malformed, rollback, OOM,
  and injected-I/O failure behavior;
- rowid-moving UPDATE remains two-phase and correct;
- `update-scan-implicit` and `delete-scan-implicit` satisfy the matched 10x
  aggregate and every-paired-round wall/CPU guards;
- all other matched cases continue to satisfy the same guard; and
- a regenerated version-1 baseline records the new raw evidence without
  changing workload semantics or durability configuration.

## Consequences

### Positive

- The optimization follows the pinned SQLite mutation mechanism instead of
  inventing a benchmark-only shortcut.
- Stable-rowid scan mutation avoids one root seek and cursor reconstruction
  per row.
- The same primitive benefits DELETE and stable-rowid UPDATE.
- Negative rowids and residual predicates retain correct SQL semantics.

### Negative

- B-tree mutation cursor ownership and save-position behavior require new
  invariants across balancing and page invalidation.
- The implementation touches B-tree, VM, and lowering seams and therefore
  requires broad regression evidence.
- A separate pager optimization may still be required if one-pass traversal
  does not remove the measured durability cost.

## Rejected alternatives

### Raise the guard

Rejected because the baseline records a repeatable mechanism gap, not a
single noisy sample.

### Replace `k>=1` with unconditional DELETE

Rejected because it changes the immutable workload and would admit
`OP_Clear` semantics that are incorrect for negative rowids.

### Call `TableBtreeWriter::Clear()` when the fixture happens to use positive keys

Rejected because benchmark corpus facts are not SQL semantics.

### Increase the cache only for the benchmark

Rejected because the matched profile intentionally fixes both engines at 512
pages and production cache policy is a separate product decision.

### Batch or omit journal syncs immediately

Rejected until one-pass mutation is measured and any remaining pager gap is
traced to a specific pinned SQLite durability mechanism.
