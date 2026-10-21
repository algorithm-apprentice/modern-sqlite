# ADR-0134: Native Savepoint Cursor Fences

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Executed prerequisite and source map

PR #238 closes actual table/index peer saving, not global rollback
coordination. Both coordinator savepoint-rollback paths still call Pager
directly. Positioned cursors retain page pins, so playback returns `kBusy`
instead of saving their keys before touching the journal.

| Responsibility | Pinned SQLite source |
|---|---|
| One list, root-zero global selection, first-error stop and partial saves | `btree.c:792-856` |
| Rowid/raw index key capture, metadata invalidation and complete path release | `btree.c:713-791` |
| B-tree savepoint saves every cursor before Pager playback | `btree.c:4648-4675` |
| Anonymous statement rollback consumes that B-tree boundary | `vdbeaux.c:3228-3260` |
| Named rollback adds a separate cursor-trip policy before that boundary | `vdbe.c:4080-4098` |
| Pager playback retains the transaction and matched savepoint | `pager.c:7083-7157` |

The local pinned native control executes a real savepoint rollback with three
read cursors and one `BTREE_WRCSR=4` cursor across two table roots and one
index root. All four paths go from one retained page to zero, enter
RequireSeek, and restore a deleted row with a positive directional skip.
Table Next returns rowid 3 rather than skipping that successor. The index
retains its complete encoded key until restoration. This is executed
`sqlite3BtreeSavepoint()` behavior, not a claim that named SQL rollback and
anonymous statement completion have identical cursor-trip policy.

## Decision

### 1. Reuse the existing global sentinel and physical protocol

Extend `BtreeDatabase::SaveCursors()` so root zero selects every registered
physical cursor, as native does. Nonzero selection, list order, the excluded
cursor, Multiple fast path, saved-key ownership, metadata invalidation,
first-error behavior and partial-save cleanup remain unchanged.

Use the existing `CursorState::SavePosition()` for table/index and read/write
roles. Invalid cursors release residual paths without inventing keys.
Already-saved and faulted cursors retain their phase. No second cursor list,
generation, key decoder, traversal or callback mechanism is introduced.

### 2. Put the real playback fence in the B-tree owner

Add only `BtreeDatabase::RollbackToSavepoint(JournalSavepointId)`. During a
write transaction it saves all cursor positions before invoking the existing
Pager operation. A save failure returns immediately: no journal playback,
write-generation advance or automatic retry has occurred. Earlier successful
saves remain saved, and the failing/later cursors retain native partial-list
state. A subsequent Pager failure likewise does not undo successful saves.

Outside a write transaction, delegate the original Pager rejection without
moving read cursors. Keep Pager's savepoint identity, ephemeral-mode,
transaction-state, raw-pin, journal and error validation. Ordinary borrowed
page pins are not cursor nodes and still enforce Pager's `kBusy` precondition;
do not forcibly invalidate external capabilities.

Route both current coordinator rollback-to-savepoint calls through this
boundary. Retain named-savepoint lookup/retention, anonymous release,
work counters, error-class-aware automatic rollback and terminal cleanup in
the coordinator. The owner does not parse SQL or acquire statement policy.

This narrowly extends ADR-0125's Pager-borrowing seam: operations that require
the B-tree cursor protocol belong to its canonical owner, not a complete
second Pager facade. Existing read snapshot and write-generation/statement
authority remain authoritative. Saved physical positions do not revive
expired write capabilities.

## Test-first and evidence

First expose the root-zero omission and a real coordinator playback returning
`kBusy` with live readers. Cover distinct roots, table/index read and writable
paths, already-saved and invalid cursors, complete pin release, original and
deleted-key restoration and unchanged nonzero-root filtering.

Exercise a malformed/save-allocation failure after an earlier successful save.
Prove first-error ordering, unchanged journal writes and generation, retained
later pins and no playback/retry. Also retain raw-pin rejection and the
existing journal/playback fault, savepoint and transaction models.

Run the focused red/green and freshly linked complete fast unit tier, affected
isolated sanitizer/GNU/instrumented and quality selections, native
interoperability/crash contracts and unchanged matched workloads. Review and
publish this coherent source's ledger before the next dependent source.
Final exact-head integrated gates, review and all nine hosted checks remain
mandatory for the delivery batch.

## Explicit continuation boundary

This source supplies the native B-tree savepoint fence. It does not yet
translate `sqlite3BtreeTripAllCursors()`, full rollback, named rollback's
write-only versus schema-wide fault policy, root destruction or retained
fault codes. Those are the next sequential coordination source, not implicit
behavior of saving a key. Overflow lists, lazy Column, ledger 12, later audit
ledgers, new SQL and nodes 44-48 remain open.
