# ADR-0130: Native Table Peer Positions

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Required behavior and source mapping

ADRs 0125 through 0129 establish stable database/physical cursor ownership,
operation-scoped header references and one native cold nearest-rowid
primitive. The remaining executable table control is not an ownership-only
claim: a mutation with a positioned read or writable peer still returns
`kBusy`, while native succeeds, saves the rowid, drops every peer path page
and resumes at the inserted successor.

| Native responsibility | Pinned source |
|---|---|
| Rowid capture, whole-path release and unconditional metadata invalidation | `btree.c:694-791` |
| Root-filtered sequential saving and conservative Multiple fast path | `btree.c:792-856` |
| Restore once, signed nearest result and skip preservation | `btree.c:865-1007` |
| Forward/reverse no-op signs | `btree.c:6366-6394,6456-6484` |
| Insert saves peers before its own seek | `btree.c:9460-9496` |
| Delete saves peers before the first write | `btree.c:9880-9980` |
| Moved VM cursor clears record cache and publishes NULL for a deleted row | `vdbeaux.c:3838-3860`, `vdbe.c:3064-3110,3150-3165` |
| Native Next resumes despite a deleted-row NULL state | `vdbe.c:6653-6698` |

This slice implements table-rowid peer positions. Index encoded keys and
their metadata/ownership, global rollback/fault policy and schema destruction
are explicit following behavior, not success-shaped placeholders.

## Decision

### 1. One current table-position protocol

Extend the existing private `CursorState` with RequireSeek and SkipNext
phases, a saved rowid and signed skip direction. Reuse its one database list,
root, role and Multiple flag. Add a narrow same-layer physical table
interface implemented by the existing read and writable cores: obtain their
current table rowid, release their path pages/borrowed metadata, and perform
the canonical nearest table move. No function registry, erased `void*`,
second cursor container, page-copy executor or global epoch is admitted.

Table operations reject an index role through their existing typed
preconditions; they are not unimplemented generic index-restore hooks.
The protected base destructor becomes virtual when the concrete table
interface is added. The owner still borrows cursors and never deletes them.

Saving a Valid peer captures only its signed rowid, performs no allocation
or overflow-payload read, releases every frame including root, and enters
RequireSeek. Saving a SkipNext peer first normalizes its physical phase to
Valid while retaining the existing skip sign, matching native. Invalid peers
release residual path pins without manufacturing a key. Fault peers retain
their error behavior. End cached page/cell borrows before releasing pins;
invalidate position metadata even if capture fails.

The owner visits its existing list in native order, selects the same root,
excludes the mutator and stops on the first error. Earlier successful saves
remain saved. No mutation has occurred, so a save failure is not converted
into a partial-write success or an unnecessary rollback requirement.
When no same-root peer exists, clear the mutator's conservative Multiple
flag as native does.

### 2. Restore through the shared one-pass nearest primitive

RequireSeek restoration sets the physical phase Invalid before its one
nearest move. On success, retain or replace the skip sign exactly as native
does and enter SkipNext when appropriate. No automatic second root walk or
retry is added on failure; return the original typed error. Explicit First,
Last, Seek and ClearPosition discard obsolete saved-position work.

Forward movement consumes a positive SkipNext as a no-op; reverse movement
consumes a negative one as a no-op. Otherwise perform one existing physical
step. Empty restoration ends traversal. Reacquire root lazily through the
same canonical root loader/validation used by Open; do not duplicate kind,
geometry, virtual-root or snapshot checks.

Expose only the facts required by real callers: whether a physical cursor
has moved, restoration's same-original-row result, and whether it currently
has a physical neighbor (Valid or SkipNext). Preserve the existing
`valid()` and strict never-positioned movement preconditions. These facts
perform no I/O except explicit restoration.

### 3. Enable only the actual table mutation fences

Table insertion saves peers before its own seek. Table deletion saves peers
after the required self-positioning checks and before promotion/mutation.
Retained replacement and deletion restore their own saved position first;
never modify a neighboring row after the original row disappeared.
Complete that retained self-restoration at the public cursor boundary after
authority validation but **before entering `ApplyMutation()`**, its fresh
mutation checkpoint and ADR-0128's header-preownership snapshot. A restored
page-one path must therefore already be caller-owned when that lease starts.
If the original row is gone or restoration fails, return the typed result
without entering the mutation boundary. Do not restore a released path from
inside the guarded mutation lambda and let the header lease discard it.

Use the same fences for transient point cursors and retained writable peers.
Preserve existing authority/root/statement checks, journal ordering,
operation-header cleanup and checkpoint-based partial-mutation failures.
This does not independently redesign the mutator's existing one-pass
DeleteAndNext optimization.

Table Clear may use the same rowid save contract before mutation. Do not
silently enable Drop, schema-root destruction, index mutation or global
rollback through a table-only save method; their current error/admission
behavior remains until their own complete mechanism is accepted.

### 4. Keep VM logical rows and borrowed records consistent

Before a table field, rowid or cached record is read, check the actual moved
physical cursor, clear its borrowed record/field cache, restore once and
publish SQL NULL when the original row was deleted. Do not reuse decoded
fields whose pin was released by another cursor.

Replace the VM's single null-row boolean with one explicit logical-row
reason: ordinary, virtual NULL, or deleted-original-row NULL. Do not keep a
second synchronized boolean. This distinction preserves the current
virtual-row Next suppression while letting a deleted-row NULL resume from
the physical SkipNext neighbor. If no neighbor remains, end traversal
without turning ordinary invalid input into success.

Next/Previous clear logical deleted-row NULL after successfully positioning
on a real row. Preserve current binding/program ownership, callback-running
guards, completion, reset/error cleanup and outer-join NULL semantics.

## Test-first matrix and completion boundary

Record the real same-root Busy failures for read and writable peers before
production changes. Match native insertion, replacement and deletion traces,
forward/reverse gaps and signed extrema, repeated saves of SkipNext, empty
trees, unrelated roots, head/middle/tail list order and full root pin release.
Include a real page-one-root peer save followed by retained replacement and
deletion: the original path is restored before lease construction, remains
readable and pinned after the operation, and still enforces the existing
live-cursor commit precondition.

Test pre-mutation save errors, root reread/restore I/O and OOM, malformed
nearest data and explicit-position cleanup. Prove table saving does not copy
payloads or allocate, and no saved/moved byte view is accessed after release.
Exercise the VM cache and both rowid paths with a real callback mutation
between reads; deleted originals must produce NULL and then continue to the
correct successor, without breaking virtual NULL rows.

Keep all existing goldens, interoperability, allocation budgets and original
model deadlines. Complete freshly linked fast units, affected GNU/sanitizer/
instrumented and quality checks, unchanged-workload native/matched evidence,
exact-tree implementation review and slice ledger before closing the related
batch. Its final exact head requires complete integrated local/hosted gates.

This is the table peer-position slice, not full B-tree or ledger-12 parity.
Index keys, schema destruction, rollback/fault coordination, overflow page
lists, lazy Column, new SQL and later DAG nodes remain blocked work.
