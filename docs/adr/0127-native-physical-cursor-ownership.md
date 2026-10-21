# ADR-0127: Native Physical Cursor Ownership

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12, shared cursor prerequisite

## Native owner and current missing seam

ADR-0125 establishes `BtreeDatabase`; ADR-0126 source `54b38c1` returns
completed retained writes to immutable capabilities without dropping their
physical pins. The three native controls now agree on read admission after
a retained write, but Modern still returns `kBusy` when an actual peer must
be saved before mutation.

Pinned SQLite represents every physical read or write cursor in
`BtShared.pCursor`, not a separate per-role registry:

| Responsibility | Native source |
|---|---|
| Shared cursor owner, link, root, flags and phase | `btreeInt.h:425-429,525-631` |
| Open sets INVALID, marks same-root peers Multiple, prepends one cursor | `btree.c:4720-4795` |
| Close unlinks head or predecessor, then releases cursor resources | `btree.c:4860-4892` |
| Root-filtered save and the Multiple fast-path contract | `btree.c:792-856` |
| Close requires all cursors closed before Pager destruction | `btree.c:2951-3007` |

Modern currently has a read `CursorCore` with `valid_`/`terminal_` and a
`WritableCursor` with its own three-state enum. Neither physical cursor is
registered with the database owner. The per-transaction writer core and its
root/statement epochs remain authorities, not replacements for this list.

## Decision: one physical registration and current phase contract

### 1. Keep the owner in the B-tree layer

Add one private cursor-state contract under `src/storage/btree/`. Its
implementation belongs with the existing B-tree database owner, not Pager,
PageCache, VM or a global lookup.

`BtreeDatabase` owns only the head of an intrusive singly linked list. Every
physical cursor contains exactly one borrowed link/control record: database
identity, root, read/write role, same-root Multiple flag and current phase.
There is no list-node allocation, second cursor container, observer registry
or callback dispatch.

Read and writable cores use the same phase type for their current Valid,
Invalid and Fault states. New enumerators are descriptive and unprefixed.
Replace the read validity/terminal booleans and the old writable-state
definition rather than keeping synchronized duplicates. Preserve each
existing typed error: obsolete read snapshots still report `kSchemaChanged`,
and existing writable fault behavior is unchanged.

Do not add saved-key storage, unused restore methods, virtual hooks or future
state-machine scaffolding in this ownership-only slice.

### 2. Register all real physical cursors

Both read-cursor cores and every `WritableCursor` participate, including
transient point-operation cursors and retained table/append cursors. Public
write-authority handles are not physical cursor nodes.

Open validates the owner/root/pins before publishing a link. It marks every
existing same-root peer and the new node Multiple, then prepends the new node,
matching native ordering. An empty-database read cursor uses native's root-zero
registration sentinel while preserving its existing logical empty-snapshot
behavior. Unrelated roots are not marked.

Internal writable-cursor opening also takes `BtreeDatabase&` explicitly.
Reject a `MutationPageOwner` belonging to another Pager before acquiring a
root pin. Migrate every direct physical-cursor caller together; retain no
ownerless overload or optional owner fallback. Pager-only page-allocation,
formatting and balancing helpers still borrow Pager and do not acquire a
cursor registration merely for convenience.

### 3. Preserve list identity across C++ moves and cleanup

The control record is noncopyable. A physical cursor move transfers its
registration by replacing the old address in the same list position, retaining
root, role, phase and flags. It does not remove/reprepend the node, allocate,
perform I/O or reorder a future save traversal. The moved-from record is
detached and invalid.

Unlink searches the same singly linked list used by native close. Closing a
peer need not eagerly clear other Multiple flags: native permits the
conservative flag until a later root-filtered save proves no peer remains.

Read-core destruction unlinks before its pin-owning members are destroyed.
Writable-cursor destruction unlinks before its existing separate
`MutationPageOwner` releases the pins; this does not transfer page ownership
into another container. Failed construction and allocation unwind the same
registration through RAII. A second unlink of a detached record is harmless.

The database remains nonmovable and outlives every physical cursor. Assert an
empty cursor list before its Pager is destroyed, matching the existing native
close precondition. Do not silently close borrowed cursors from the database
destructor.

## Qualification and sequential delivery

This is a behavior-preserving ownership refactor from the qualified
ADR-0126 source. Start from that green baseline rather than manufacturing a
SQL failure.

Add real-cursor lifecycle controls for read/write membership, same versus
different roots, native ordering/Multiple flags, moved-from state, stable
public Pimpl moves, relocation of value-stored writable cursors, head/middle/
tail closure, failed Open/allocation cleanup and wrong-database rejection.
Use the existing test-peer pattern for private list observations; do not
expose a public cursor-count or registry API.

Preserve existing pin counts/lifetimes, read snapshot invalidation, root and
statement authority, normal traversal, malformed/OOM behavior, allocation
ceilings, bulk append and commit preconditions. The ownership bookkeeping
must not allocate per cursor, statement, row or move.

Run a complete default build, focused controls and the complete fast tier,
affected GNU/sanitizer and quality checks, then exact-tree implementation
review and commit-level ledger evidence before the dependent slice begins.
Continue within the related source batch in draft PR #236 under ADR-0106;
final full closing is against its exact final head.

## Explicit next behavior boundary

The next accepted design must implement actual root-filtered key saving,
release of every cursor page, restore/skip/fault phases, and all directly
dependent mutation, VM record-cache and rollback/savepoint callers. It must
preserve native pre-mutation I/O/OOM ordering through the canonical record
and page contracts. No callback registry or global generation may replace
that protocol.

This slice changes no mutation admission, seek algorithm, VM execution,
rollback policy, physical format, SQL feature or concurrency scope. The two
peer-saving controls remain explicitly unresolved until the real protocol.
The overflow page list, lazy Column, remaining ledger-12 attribution and
later work stay blocked.
