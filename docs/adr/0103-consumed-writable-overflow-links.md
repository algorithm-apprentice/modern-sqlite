# ADR-0103: Consumed Writable Overflow Links

- Status: Accepted
- Date: 2026-10-17
- Module graph node: `implement-advanced-sql`

## Context

The implemented-engine SQLite reference audit identifies B2: writable payload
access rejects an unused final overflow link that both pinned SQLite and
Modern's read cursor ignore. This is malformed-image point-operation parity,
not evidence that Modern produces invalid overflow chains.

Begin after PR #212, main commit
`356bbca5a51d6e3d539bb25fc7f6154e0d49d129`, accepted tree
`e120b0543f6f9f184ffb36e69b72e3522f023b76`. All nine first-head hosted checks
passed. The original unchanged sanitizer model passed in 6.45 seconds under
its ten-second deadline. ADR-0102's verification tools are now available;
their fast-local reports do not replace isolated qualification.

The existing canonical mechanisms are:

- `WritableCursor::ReadCellPayload` assembles owned scratch for every writable
  index-record comparison and table mutation-row read. Public consumers
  include encoded index lookup/insert/replace/delete and
  `TableBtreeMutationCursor::row()`.
- Read-only `CursorCore::ReadCellPayload` copies the requested bytes and parses
  a next link only if more bytes remain.
- Writable equal-size overwrite and `ClearCellOverflow` already consume a
  next link only when another logical overflow page is required.
- `MutationPageOwner` owns bounded page pins and provides page-sized frames;
  `BtreePageGeometry` bounds each borrowed payload span.
- `OverflowPageView::Parse` is the strict pure format decoder. It validates
  the link, including locking-page and maximum-page restrictions.
- Whole-database storage inspection separately checks overflow termination,
  page ownership, and reachability.

Pinned `src/btree.c:5338-5345` reads the raw next field, copies payload bytes,
and returns when the requested amount reaches zero. It does not validate or
follow that unused value. `sqlite3BtreeIndexMoveto` uses this payload path for
overflow-backed index records. The prior audit executed the counterexample:
native and Modern read access accept an invalid unused final link, Modern
writable lookup rejects it, and all reject the consumed intermediate-link
control.

The current writable reader instead invokes `OverflowPageView::Parse` before
copying every overflow chunk, including the last one. Thus identical logical
bytes acquire different read semantics depending on the owning cursor.

## Decision

### 1. Correct the existing writable payload reader

Keep `WritableCursor::ReadCellPayload` as the single writable implementation.
Reuse the existing read/overwrite rule:

1. Acquire the required overflow page through `MutationPageOwner`.
2. Borrow its exact page-sized bytes.
3. Copy at most the remaining logical payload bytes from the geometry-bounded
   payload region after the four-byte link.
4. Parse and retain the next link only if logical payload bytes still remain.
5. Release the operation-owned overflow pin on success and every error path.

Do not add another cursor, payload container, traversal algorithm, parser
mode, compatibility shim, or unchecked page reference. Reuse `ByteView`,
geometry, the existing documented resize-required scratch, and typed results.
No lower-layer API or persistent encoding changes.

### 2. Preserve the consumed-field boundary

An invalid, locking-page, out-of-snapshot, or cyclic unused final link is not
followed by point payload access. The returned record bytes must equal the
same image with a zero final link.

If more bytes remain, preserve existing corruption detection for missing,
invalid, out-of-snapshot, aliased, and prematurely terminating consumed links.
Preserve cursor fault publication and do not expose partially assembled
scratch after an error. Keep existing allocation and I/O errors explicit.

Do not weaken `OverflowPageView::Parse`, integrity inspection, allocator
ownership, overflow release, or mutation ordering. Whole-file inspection
continues to reject malformed overflow termination even when a point read
succeeds. This distinction is not a claim that the malformed image is valid.

### 3. Keep state, lifetime, and performance contracts

Fully local payload remains borrowed without new allocation or I/O.
Overflow payload retains its existing single scratch resize and bounded
page-by-page ownership. Do not add a second payload pass, pin cache, preload,
whole-file scan, or per-operation container.

The change only removes validation of a field that is not consumed. It
introduces no persistent transition and changes no journal, savepoint,
rollback, freelist, cell layout, mutation, or crash ordering. Existing
failure-injection and crash evidence remains mandatory.

No workload, fixture, oracle, seed, allocation ceiling, timing threshold,
process isolation requirement, or deadline is relaxed. Do not claim an engine
speedup from removing this check.

## Verification and delivery

Obtain GPT-5.6 Sol design approval before changing production code.
Develop the changed behavior test-first through the existing B-tree test
helpers and fixed VFS.

First observe a focused writable seek failure for unchanged logical bytes
with an invalid unused final link. Extend coverage through the same public
encoded index operations and table mutation-row access, including exact and
missing keys, one and multiple overflow pages, repeated access, pin cleanup,
and unchanged rollback state after read-only operations.

Use independent byte mutations for a zero link, `0xffffffff`, the locking
page, an out-of-snapshot page, and a cycle. Preserve controls in which a zero,
invalid, locking-page, or out-of-snapshot link is consumed before the payload
is complete and must fail. Do not add a new point-read cycle detector; retain
the existing ownership checks and compare cycle behavior with native. Retain
strict storage-inspection coverage of the same malformed termination.
Cover allocation and late I/O failures through the existing failure seams;
do not widen budgets or introduce a second failure harness.

Replay the corresponding point behavior through the exact pinned native
runtime, including the consumed-link control. Retain valid-image
interoperability in both directions, crash recovery, and existing native
performance contracts. No corpus or golden fixture is replaced.

After focused green, run the complete fast tier. Use ADR-0102's actual
change-aware plan: the writer path conservatively selects full qualification,
including read and writable SQL/database campaigns. Reconcile shared tests
before exclusion and keep exact coverage accounting instead of repeating
configuration-independent work.

Review implementation and exact-tree closure with GPT-5.6 Sol. Require all
nine exact-head hosted gates before guarded squash merge and verified named
branch cleanup. This correction does not complete node 43 or the other
remaining reference-audit findings.

## References

- ADR-0023: B-Tree Page Decoding
- ADR-0024: Read-Only B-Tree Cursors
- ADR-0044: Reference-Faithful SQLite B-Tree Mutation
- ADR-0051: SQLite-Aligned One-Pass Scan Mutation
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0102: Change-Aware Verification
- `docs/sqlite-reference-audit.md`: B2
