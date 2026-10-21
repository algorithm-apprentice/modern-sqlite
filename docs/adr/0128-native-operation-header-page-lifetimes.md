# ADR-0128: Native Operation Header-Page Lifetimes

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12, cursor-position prerequisite

## Native responsibility and reproduced difference

PR #236, merged as `90e9129`, closes typed write-lease handoff and physical
cursor registration, not actual position saving. Mapping that next protocol
revealed a separate ownership seam that root-filtered cursor saving must not
hide.

Native `allocateBtreePage()` and `freePage2()` borrow `BtShared.pPage1`
(`btree.c:6546-6568,6808-6832,6868-6900`). Page one is shared
database-header state, not an extra page of every cursor's `apPage` path.
A cursor on one table therefore does not block allocation for another table
or need its position saved merely because an operation updated the header.

A bounded direct native control replaces a retained table row with an
8,000-byte BLOB, then inserts an overflow row into a different table.
The insert succeeds; the retained cursor remains VALID with one path page
and its Multiple flag is false. Modern returns `kBusy`, retaining two pins.
Destroying that cursor releases both and immediately makes the same
unrelated insert succeed without rollback.

The mapped cause is the operation's page-one slot remaining in the retained
`MutationPageOwner` after the write. ADR-0126 correctly demotes its mutation
capability, but that extra read reference still prevents the next operation
from obtaining the sole page-one write lease.

This is ownership/state evidence, not a matched timing or I/O comparison.
Do not save or invalidate unrelated cursors to work around it.

## Decision: scope the existing temporary page lease

Reuse the canonical `TemporaryOwnedPage` RAII mechanism already used by
freelist-trunk, clear and balancing paths. Move its one definition to the
existing private B-tree contract so retained-operation boundaries can use it;
do not add another cleanup class, page set, callback or registry.

Before a retained operation begins, record whether its page owner already
owns page one. The temporary lease releases only a page-one reference newly
acquired by this operation. Existing caller-owned references, including a
cursor whose own root/path is page one, remain owned by that caller.

Scope that lease around the same three canonical completed-operation
boundaries as ADR-0126: `ReplaceCurrent()`, `DeleteAndNext()` and
`AppendSorted()`. Keep the header reference for the entire underlying
operation, rather than releasing/reacquiring it on every overflow-page
allocation. All internal mutable page objects and views finish before the
lease is destroyed.

The lease must unwind on success, a returned error and a foreign exception.
It performs only the existing owner-slot release, with no I/O, allocation or
new error result. It does not clear dirty/sync state, release file locks,
change a transaction generation, move a physical cursor or alter its phase.
Expected mutation failures retain their primary error and existing
checkpoint-based rollback-required behavior.

Successful write-capability handoff and its all-slot staging preflight still
run before the temporary header reference expires. Do not release staged
page borrows early to turn an invalid handoff into success.

This supersedes only preservation of a newly acquired, operation-local
header reference across retained calls in ADR-0126. That ADR's typed transfer
still preserves its input physical reference and total pins; genuine cursor
path pins and returned position remain retained.

## Qualification

Develop test-first against the reproduced unrelated-root `kBusy` failure.
Use the real retained cursor and overflow allocation, not an isolated mock
that merely drops a pin.

Cover table replacement, positioned deletion/freeing and multi-page index
append boundaries, including successive operations while the cursor stays
alive. Assert unrelated-root allocation succeeds without moving or saving
that cursor, while actual same-root peer mutation remains the separately
unimplemented protocol. Check root-page-one and deliberately preowned header
references are not released, and preserve the live-cursor commit
precondition.

Keep existing malformed/OOM/late-I/O, partial-write failure, temporary-store,
bulk-append and ownership controls. Verify no new allocation, copying or
unnecessary per-overflow-page header acquisition, and no change to durable
journal ordering or thresholds. Compare fresh unchanged native/matched
controls and disclose any counter or timing difference.

Complete the freshly linked fast tier, focused GNU/sanitizer and quality
selection, implementation review and slice ledger before the next dependent
slice. This starts the next ordered cursor-position batch; do not merge a
claim of real peer save/reseek from this header-lifetime correction alone.

## Next behavioral boundary

Continue the native one-traversal nearest-key primitive and real table peer
save/release/restore/skip behavior, including the directly dependent VM
record-cache, rowid and deleted-row NULL handling, under their own completed
mapping and accepted designs. Do not substitute GE-then-Last reseeks, whole-
database invalidation or a second execution/record mechanism.

Index-key metadata/restoration, schema/root destruction and rollback/fault
coordination keep explicit later boundaries. The native overflow page list,
lazy Column, remaining ledger-12 attribution, new SQL and later DAG nodes
remain incomplete.
