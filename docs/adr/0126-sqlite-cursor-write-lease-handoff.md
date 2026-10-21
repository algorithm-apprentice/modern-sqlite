# ADR-0126: SQLite Cursor Write-Lease Handoff

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12, shared cursor prerequisite

## Reference and reproduced gap

ADR-0125, merged in PR #235 as `6a7efcd`, establishes the database owner.
Native cursor save/reseek also requires the correct physical pin ownership:
an existing writable cursor must not exclude a reader merely because its
last operation wrote a page.

Pinned SQLite retains `MemPage`/Pager references in `BtCursor.pPage` and
`apPage` (`btreeInt.h:525-565`). `sqlite3BtreeInsert()` makes pages writable
when required, but the surviving cursor reference is not an exclusive
mutation borrow. `saveAllCursors()` later releases the other cursors' pages
before an actual mutation (`btree.c:694-856,9460-9474,9960-9980`).

A bounded local probe uses the pinned amalgamation and the same native
benchmark flags. After replacing the current row with
`BTREE_SAVEPOSITION`, opening a second reader succeeds and the writer remains
positioned. The current Modern equivalent returns `kBusy`: retained
`MutationPageOwner` slots keep `WritePagePin` after `ReplaceCurrent()`.
This is a reproduced ownership difference, not a timing claim.

The same probe confirms that a native table mutation saves both read and
write peers, releases their entire page stacks, enters `REQUIRESEEK`, and
subsequently returns the inserted successor. Modern currently returns
`kBusy` in both cases. This ADR addresses only the completed-write lease
handoff; it does not claim to implement that following save/restore protocol.

## Existing canonical mechanisms

- `PageCache::Pin` already owns the frame reference separately from its
  `exclusive_` mutation permission.
- `Pager::WritePage(ReadPagePin&&)` consumes a read capability, promotes the
  sole pin, and establishes journal-before-write eligibility.
- `MutationPageOwner` already owns one bounded slot array containing
  `ReadPagePin` or `WritePagePin`. Do not add another page container.
- `TableBtreeMutationCursor` and `IndexBtreeAppendCursor` already retain
  their physical cursor and page owner between calls.
- Mutable page objects and their temporary views finish inside the
  underlying cursor operation before its public retained-cursor call returns.

## Decision

### 1. Add the inverse typed capability transfer

Add `Result<ReadPagePin> Pager::ReadPage(WritePagePin&&)`, symmetric with the
existing consuming promotion overload. It transfers the same frame reference
and revokes exclusive mutation permission without releasing or looking up the
frame.

Use one private PageCache helper, accessible through the existing Pager
friendship. It validates ownership and exclusivity, clears the pin/entry
exclusive flags, and decrements the exclusive-pin count exactly once. Total
pins, frame address, dirty list, sync authorization, `needs_sync`,
`dont_write`, bytes, and cache recency are unchanged.

The successful transfer performs no allocation, I/O, journal capture,
pressure maintenance, page lookup, eviction, snapshot change, or generation
change. Re-promotion still requires the sole read pin and follows the
existing journal-before-write path.

Preserve the existing typed-error and consuming-rvalue conventions, including
moved-from and wrong-Pager errors. Do not turn an invalid pin into a valid
empty result. Any mutable view obtained from the consumed write pin expires
at the transfer; the resulting capability exposes only immutable bytes.
`MutableByteView` is a copyable span, so this expiry is a borrowing contract,
not mechanical revocation of existing copies. Call-site scoping must prove
that the operation's local mutable views are no longer usable.

### 2. Use it at completed retained-cursor operations

Add one bounded `MutationPageOwner` operation that converts its occupied write
slots to read slots through that same Pager transfer. Reuse the existing
array; do not track a second set of pages or copy their contents.
Preflight all occupied slots before converting the first one: no slot may
retain staged cells or their borrowed mutable views, and write capabilities
must be live and exclusively owned. In particular, require `staged_count == 0`
for every slot.

Apply it after successful `ReplaceCurrent()`, `DeleteAndNext()`, and
`AppendSorted()`, once the underlying operation's mutable page objects are
gone. Preserve the returned position, traversal state and every retained
physical pin. Propagate a handoff error explicitly; never report success.
Each retained boundary captures the existing `mutation_sequence()` checkpoint
before its operation. Pass that checkpoint to the shared handoff helper;
on any post-operation handoff error it must call the existing
`MarkRollbackRequiredAfter(error.code(), checkpoint)` before returning the
error. A reported failure after mutation must not become committable merely
because the caller destroys its cursor and releases the pins. Every partially
converted owner remains safely destructible. Cleanup for an operation that
already returned an error remains unchanged in this slice.

Do not demote a page while a mutable page object or view can still be used.
Do not weaken PageCache's exclusive-write or sole-read promotion rules.
Point-operation owners already destroy their pins at return and need no
second handoff path.

## Test-first qualification

Before production changes:

1. Add a focused Pager transfer test and observe the absent-overload failure.
2. Record the retained-writer/reader `kBusy` failure against native success.
3. Implement the smallest inverse transfer and its three real consumers.

Cover stable address/content, unchanged total pins and dirty/sync flags,
shared reading after transfer, rejection of promotion while a peer reads,
successful sole re-promotion, moved-from and wrong-owner errors, no new I/O
or allocation, and retained table/index cursor position and continuation.
Require reader admission after each of `ReplaceCurrent()`, `DeleteAndNext()`,
and `AppendSorted()`, including a multi-page retained path. Verify unchanged
total pins and the exact removal of every exclusive pin. Cover preflight and
post-mutation normalization failure, the existing rollback-required latch,
and rejection of commit after the cursor releases its pins. For continued
writes, close the temporary read peer first; actual peer save/reseek remains
the next mechanism.
Preserve the existing commit-with-live-pins precondition, temporary modes,
bulk append, rollback, malformed/OOM tests and allocation limits.

Run focused green and the complete freshly linked fast tier before review
and commit. Use native controls and changed GNU/sanitizer selections without
changing assertions, workloads or deadlines.

## Ordered delivery boundary

This is the first coherent source slice of the native cursor-coordination
chain. ADR-0106 permits batching two through four related, buildable source
commits in one draft pull request. Each following slice still needs its own
completed mapping, accepted design, focused/fast evidence and review before
implementation; this ADR does not preapprove its design.

After this accepted prefix, continue the actual root-filtered cursor list,
saved rowid/encoded key, pin release, restore/skip/fault handling, and
directly dependent VM cache and rollback callers. The final exact batch head
receives the actual-change closing plan, unchanged matched/native/model/fuzz
contracts, integrated review and all nine latest hosted successes before
guarded squash and cleanup.

No PageCache write epoch, cursor invalidation generation, callback registry,
compatibility facade, second executor, new SQL or persistent transition is
introduced. The native overflow cache and lazy Column remain later work;
ledger 12, node 43 and subsequent nodes remain incomplete.
