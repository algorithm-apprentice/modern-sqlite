# ADR-0076: Typed NULL-Row Cursor Publication

- Status: Accepted
- Date: 2026-10-14

## Context

Comma/CROSS products and their aggregate consumers are merged through PR #186.
The next LEFT join package needs to publish an unmatched right-hand source
without reading a nonexistent B-tree record or inventing projection-specific
NULL snapshots.

Pinned SQLite `src/vdbe.c:6350-6384` uses `OP_NullRow`: invalidate the cursor's
record cache and B-tree position, then expose NULL for column access.
`OP_Column` and rowid access honor that logical state. The source cursor remains
owned and can be repositioned for the next left prefix.

Modern already has one `RuntimeCursor`, one cached record representation,
typed field/rowid reads, and a definite cursor-state verifier. Its B-tree core
has one no-I/O position invalidator used by EOF and failed seeks. Reuse these
mechanisms rather than introduce a pseudo-table, second row buffer, or
join-specific value container.

## Decision

### 1. Add one typed publication operation

Add `NullRowInstruction { CursorId cursor; bool enabled = true; }`.

It operates only on an already-open ordinary read cursor, table or index.
Reject a descriptor ID that is used by `OpenMutationCursorInstruction` in the
same program. Current mutation traversal requires a physical row and must not
silently acquire virtual-row behavior. A write program may still use an
independent ordinary read cursor; this is not a statement-kind restriction.

Enabling publication clears physical position and cached decoded/owned record
state, then exposes one logical NULL row. Disabling publication clears that
flag and leaves the cursor open/unpositioned. Neither operation performs
filesystem I/O, allocates a record, closes/reopens the cursor, or changes the
database.

Unlike SQLite's additional unopened pseudo-cursor convenience, Modern rejects
closed use. LEFT join lowering already owns and opens every selected source.
No current requirement authorizes another cursor kind.

### 2. Define logical row availability precisely

The verifier's positioned state denotes an available logical row. That row
can be physical or explicitly published NULL. This preserves exact control
joins between matched and unmatched LEFT rows without weakening
closed-versus-open or unpositioned-versus-readable checks.

`ReadField` and rowid-backed fields return SQL NULL while publication is
enabled, before physical access or missing-field/default handling.
`ReadRowId` returns NULL for a rowid-capable table cursor; its existing
storage-kind structural restriction remains unchanged.

`Next` and `Previous` consume the virtual row as EOF, perform no physical
movement/I/O, clear publication, and leave the cursor unpositioned. Real-row
movement retains the existing behavior. Rewind, Last, and every seek clear
publication before positioning; failed positioning leaves ordinary
unpositioned state, never a stale virtual row.

The verifier still validates all cursor IDs, branch targets, field metadata,
register definitions, and storage-kind restrictions. Explicit disable sets
unpositioned state, so read-after-disable without repositioning is rejected.
No generic cursor-state union or optimistic read fallback is introduced.

### 3. Reuse the canonical position invalidator

Expose a narrow checked `ClearPosition()` on table/index read cursors, backed
by the existing `CursorCore` invalidation. A moved-from cursor returns misuse.
Clearing a live cursor is allocation-free and performs no I/O; it does not
revive a terminal snapshot/movement failure.

Runtime publication clears the same `RuntimeCursor` cache and uses this
operation. Reset, close, detach, halt, and error clear both publication and
owned physical resources through their existing cleanup.

### 4. Keep the foundation bounded

This PR adds no JOIN grammar, ON predicate, query plan, outer-row match flag,
or accepted SQL surface. The next reviewed LEFT join emitter consumes this
typed primitive and documents match-before-WHERE and NULL-extension order.

## Verification

Focused red bytecode/VM behavior precedes implementation. Cover:

- open table/index publication and NULL fields/rowid;
- suppression of cached records, rowid aliases, and missing-field defaults;
- physical and virtual branch joins with defined result registers;
- repeated publication, explicit disable, and reposition after a NULL row;
- Next/Previous virtual EOF and zero I/O/allocation;
- invalid ID, closed use, read-after-disable, and mutation-cursor rejection;
- reset/error/close ownership and snapshot rules; and
- moved-from and repeated B-tree position clearing.

Run existing read/mutation/bytecode ownership, allocation/OOM, compiler,
sanitizer, fuzz, quality, and frozen work contracts. Ordinary non-NULL-row
programs retain their allocation budgets and behavior.

Review design and implementation before committing and merge before the
executable LEFT join package. `implement-advanced-sql` remains active.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 5 prerequisite.

Contracts: ADR-0031, ADR-0032, ADR-0036, ADR-0046, ADR-0053, ADR-0073.
