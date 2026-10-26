# ADR-0154: Native Encoded Index Upsert

- Status: Accepted
- Date: 2026-10-26
- Module graph node: `implement-advanced-sql`
- Prerequisite closure: ADR-0153 sanitizer qualification
- Amends: ADR-0117 encoded replace-existing dispatch

## Context

The unchanged
`ReadLowering.SharesTemporaryStorageAcrossAggregateModifiers` test exceeds
its ten-second sanitizer deadline on the hosted Linux runner even when
ordinary units have exclusive CTest scheduling. The exact rebuilt
`534a639` binary also fails the original local CTest deadline; an explicitly
diagnostic, bounded direct execution completes in 12.375 seconds, including
10.804 seconds of process CPU. That diagnostic is not passing qualification.

The test evaluates the same grouped DISTINCT/local-order query in memory
and file temporary modes. Its unique ordered DISTINCT invocation uses
replace-existing insertion for every accepted input, as ADR-0069 requires.
There is no evidence supporting a workload, deadline, sanitizer, or stack
reservation change.

The existing encoded mutation path performs redundant work:

1. `EphemeralRelation::Impl::InsertBtree` opens a writable cursor through
   `ContainsEncoded` and searches for the comparison prefix.
2. For an equivalent record, `ReplaceEncoded` opens another writable cursor
   and searches again to retain its public not-found contract.
3. `WritableCursor::InsertIndexRecord` validates the record, saves peers,
   searches again, and calls the existing `InsertIndexAt` replacement path.

A missing key instead performs the membership search and a second insertion
search. Each wrapper also constructs a complete operation-scoped mutation
owner/cursor. Sanitizer stack samples reach that cursor construction and
the existing record sorter; they do not establish path canonicalization as
the aggregate test's primary cost.

## Pinned source mapping and existing owners

The reference remains SQLite 3.54.0, Git
`cb547ab3e931c7766e24834af5ef6c4578863e3d`.

| Responsibility | Pinned SQLite | Existing Modern owner/mechanism |
|---|---|---|
| Aggregate-local ordered root and key metadata | `select.c:6784-6814`, `resetAccumulator` | ADR-0069 modifier lowering and statement-owned `EphemeralRelation` |
| Accepted row, encoded key, and direct index insertion | `select.c:6946-7014`, `updateAccumulator` | UniqueOrdered uses `kReplaceExisting` without changing expression evaluation |
| Ephemeral database/root/cursor lifetime | `vdbe.c:4686-4764`, `OP_OpenEphemeral` | ADR-0115/0117 statement-shared arena, `RelationRootLease`, lazy logical root |
| Borrowed encoded input and error propagation | `vdbe.c:6728-6753`, `OP_IdxInsert` | `ByteView` borrowed for one synchronous writer call |
| Save peers, seek once if necessary, insert or replace | `btree.c:9441-9603`, `sqlite3BtreeInsert` | `SavePeers`, `SeekIndexRecord`, `InsertIndexAt` |
| Equal-size overwrite, overflow cleanup, cell replacement and balancing | `btree.c:9580-9747` | Existing `OverwritePayload`, `FillIndexCell`, `InsertFormattedCell`, rollback checkpoints and cursor invalidation |
| Cursor and statement cleanup | `vdbeaux.c:2761-2812`, `sqlite3VdbeFreeCursorNN` and frame cleanup | Operation-scoped mutation owner, relation cursor clearing, lease retirement and statement-wide arena failure |

SQLite's unique ordered insertion does not emit a separate membership
probe before each `OP_IdxInsert`. The B-tree's one positioning result
already determines whether the encoded representative is inserted or
replaced. Its optional seek-result reuse is not needed to establish this
one-call invariant.

Inventory found no existing public encoded upsert outcome. `InsertEncoded`
requires a missing key, `ReplaceEncoded` requires an existing key, and the
retained index append cursor requires strictly increasing physical keys.
The internal `InsertIndexRecord` already supports native insert-or-replace
through its existing replace mode; only its successful outcome is discarded.
No new tree, comparator, workspace, retained cursor, cache, or container is
needed.

## Decision

Expose `Result<bool> IndexBtreeWriter::InsertOrReplaceEncoded(ByteView)`.
Successful `true` means a new prefix was inserted; successful `false` means
the equivalent prefix's complete encoded representative was replaced.
Expected failures remain typed results.

Change the existing internal `InsertIndexRecord` result to publish
`!seek.exact` only after `InsertIndexAt` succeeds. Preserve record and restore
metadata validation, peer-saving order, the one canonical search,
equal-size overwrite, overflow handling, balancing, rollback marking, and
failure cleanup. Adapt the existing status-only callers without changing
their public contracts. Share encoded insert-only/upsert admission through
one private writer helper rather than duplicating cursor construction.

Use this outcome directly for ordinary replace-existing relation insertion:
increment `record_count` only after a successful new-key publication and
return the existing inserted/replaced result. Preserve writing-state
admission, which excludes positioned drain views from mutation, and retain
statement-wide poison/cleanup on failure.

Keep the existing guarded path when the relation count is saturated. A
replacement remains admissible at the maximum count, while a missing key
must return `kTooLarge` before mutation. The public `ReplaceEncoded`
missing-key behavior remains unchanged. ADR-0155 separately extends the
same admission repair to keep-existing insertion.

This changes redundant operation volume, not SQL behavior or persistent
transition ordering. Allocation sweeps must still inject every observed
boundary of the new production path; fewer redundant cursor/search
operations are not permission to skip any remaining prefix. ADR-0153's
earlier fixture-only count-identity evidence remains scoped to that earlier
change, not asserted for this engine repair.

## Verification and scope

Before implementation, add and run a failing fixed-work regression through
the existing relation API: a one-record comparison prefix is searched once
for replacement and once for a new-key upsert, in both memory and file
modes. Count actual collation callbacks, not elapsed time or source text.
Assert representative bytes and relation counts as well.

Add writer outcome, malformed/metadata, moved/stale ownership, overflow,
replacement-size, and multi-page coverage using existing helpers. Extend
the existing exhaustive relation OOM runner for missing-key replace-mode
insertion and size-changing replacement; retain its original operations
and every observed fault prefix. Existing numeric, NULL, collation,
statement-poison, rollback, interoperability, model and crash tests remain
required controls.

Run the focused regression and complete fast unit tier, matched fixed-work
before/after diagnostics, affected isolated sanitizer tests at their
original ten-second deadlines, native MSVC qualification, production quality,
and the final exact-head integrated gates. A diagnostic extended run is
never passing execution evidence. Retain the fifteen-minute sanitizer job
cap, original workloads, allocation limits, assertions and fuzz campaigns.

This is a bounded node-43 prerequisite repair, not a new SQL feature or a
general index-cursor redesign. It does not change persistent encoding,
main-database transaction policy, SQL evaluation order, sorter algorithms,
or cross-operation cursor retention. Scalar IN query-RHS remains next only
after ADR-0153's final qualification and merge.
