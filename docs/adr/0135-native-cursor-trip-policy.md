# ADR-0135: Native Cursor Trip Policy

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Native mechanism and executed controls

ADR-0134 supplies global saving before savepoint playback. Saving alone cannot
implement named rollback: native faults write cursors, and a schema-changing
transaction must fault read cursors too, before root pages can disappear.

| Responsibility | Pinned SQLite source |
|---|---|
| Write-only saving, all-cursor faulting and failure escalation | `btree.c:4486-4528` |
| Clear saved keys before faulting; release every path | `btree.c:700-709,853-863,4501-4525` |
| Repeated restore and explicit movement retain the fault code | `btree.c:901-1007,5586-5627` |
| Named rollback trips before savepoint playback, including read-only transactions | `vdbe.c:4080-4103` |
| A schema-cookie attempt marks schema change before its error is handled | `vdbe.c:4403-4421` |
| Named schema rollback retains the flag; terminal commit clears it | `vdbe.c:4100-4103`, `build.c:675-677` |
| Full rollback and error-class-dependent trip versus save callers | `btree.c:4552-4596`, `main.c:1516-1552`, `vdbeaux.c:3326-3486` |

Six local native controls execute direct write-only/all trip, freeing an
already-saved index key, an index-save allocation failure, named data/schema
rollback and named rollback without a write transaction/Pager savepoint.
Write-only mode saves all three readers and faults the real flag-4 writer.
All mode faults all four. Every path is released. A read-save OOM escalates
to all four faults with code 7, including an earlier saved reader and an
already-tripped writer. Ordinary rollback faults retain extended code 516,
`SQLITE_ABORT_ROLLBACK`, across repeated restore and First attempts.

The schema control retains the connection flag after named rollback. The
read-only control still saves all three readers despite having no writable
Pager savepoint. These are policy differences from anonymous statement
rollback, which continues to use ADR-0134 without an added trip.

## Decision

### 1. Reuse one position payload and the existing physical list

Extend the existing rowid/raw-key variant with one typed trip-code
alternative; rename that shared payload `CursorPositionData`. It owns a saved
key or a native SQLite fault code, never both. No extra cursor container,
error registry, generation or allocation is introduced.

Tripping replaces the old payload, freeing any saved raw key, resets the
directional skip, sets Fault and releases all borrowed metadata/path pages.
Moves preserve the registration and payload; the detached moved-from payload
is cleared. Explicit positioning must not erase a trip code before checking
the terminal fault.

`DiscardSavedPosition()` clears ordinary rowid/raw-key work only: when the
payload is a trip code it leaves both that code and Fault unchanged. Do not
use this operation to clean a detached moved-from object or destroy a cursor;
those ownership-ending paths explicitly clear their payload instead.
`ClearPosition()` retains its existing allocation-free, I/O-free successful
clear contract for an attached cursor, but cannot revive a fault or erase its
cause, as required by ADR-0076. `CountEntries()`, First/Last, seeks and
restoration must subsequently return the same trip code rather than a legacy
snapshot/corruption error.

Keep ordinary pre-existing read-snapshot and writable-operation fault
behavior. One shared `CheckTripped()` converts an actual trip code through
the existing extended `Error::FromSqliteCode` contract. Expected allocation
failure during error construction remains typed OOM, never an exception.
Read and writable checks consume it, including retained table/append
validation before stale statement/generation authority can mask the native
trip. Non-tripped authority validation keeps its original ordering and
errors; this does not revive an expired capability.

### 2. Translate native trip selection and failure ordering

Expose only `BtreeDatabase::TripCursors(BtreeCursorTripMode)` with the two
descriptive modes `WriteOnly` and `All`. Reject an invalid mode before moving
any cursor. Tripping itself does not require a write transaction.

WriteOnly walks the existing list in native order. Read cursors use the
canonical SavePosition protocol; write cursors are faulted with extended
abort-rollback code. Already-saved/invalid/faulted readers retain their
native state while residual paths are released. All mode saves no keys and
faults every cursor without allocation or I/O.

If saving one reader fails, fault every cursor with that error's exact SQLite
code, discarding earlier saved keys and overwriting earlier write-only
faults, then return the original typed error. Do not continue saving, retry,
play back a journal or mutate database pages from this primitive.

### 3. Put named rollback policy in the existing coordinator

Track one transaction-local schema-change flag in CoordinatorState, matching
native connection intent, not a snapshot generation. Set it when an
authorized schema-cookie increment is attempted, before handling that
operation's result. The current admitted schema writers all use this
canonical operation; add no second observer or schema detector.

After existing name/activity validation, named RollbackTo trips WriteOnly
when the flag is clear, otherwise All. Do this even when the matched named
savepoint has not materialized in Pager. Only then perform ADR-0134 playback,
if any, and retain the matched savepoint. Preserve the existing
error-class-aware full-rollback fallback on a trip/playback failure.

Keep the schema flag set across named rollback, including rollback that
removes its schema changes. Clear it at the existing completed terminal
logical reset. Anonymous statement rollback and all current transaction,
counter, catalog and cleanup ownership otherwise remain unchanged.

## Qualification and explicit boundary

First reproduce named schema rollback leaving live readers resumable rather
than faulted. Cover read/write table/index roles, already-saved and invalid
paths, moves, close, repeated access/explicit movement, extended code
retention and error priority without I/O or accidental raw-key retention.
For both table and index cursors, call ClearPosition after a trip and prove
zero allocation/I/O, retained Fault/cause, then repeated CountEntries,
First/Last, seek and RestorePosition errors with the exact stored code.
Test schema-flag terminal reset, sticky repeated named rollback, failed
cookie attempts and the no-Pager-savepoint named path.

Inject save OOM/I/O after an earlier successful reader save and writer trip.
Prove all-cursor escalation, exact stored cause, complete pin/key cleanup and
no playback. Retain all old authority, allocation, model and corruption
contracts, complete fast units, affected isolated/compiler/quality and fresh
native/matched evidence. Review and publish this source before its suffix.

This source implements the trip primitive and named policy, not full
transaction rollback. The latter still needs its distinct native
`tripCode == OK` versus abort callers, Pager-error precedence and completed
cleanup mapped through the current typed coordinator. It is the following
sequential source. Root destruction, overflow caching, lazy Column, ledger
12, later audit ledgers and later DAG nodes remain open.
