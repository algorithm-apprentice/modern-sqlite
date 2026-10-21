# ADR-0133: Native Index Peer Positions

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Remaining executable gap

PR #238 sources ADR-0131 and ADR-0132 establish one cold index search and
physical comparison ownership. A positioned index reader still makes an
ordinary peer insertion return `kBusy`. Native saves the complete encoded
key, releases every path page and resumes at the inserted successor.

This source completes that index-position mechanism, not ledger 12 or node43.

| Native responsibility | Pinned SQLite source |
|---|---|
| Complete raw key, one buffer and seventeen zero padding bytes | `btree.c:713-791` |
| Existing list order, root filter, first-error stop and Multiple fast path | `btree.c:792-856` |
| Allocate/unpack before one nearest move; saved-key cleanup and skip state | `btree.c:865-1007`, `vdbeaux.c:4194-4256` |
| Native key/all domains and default collation/sort slots | `build.c:5733-5774`, `select.c:1586-1604,1653-1680` |
| Cold index move and comparison width | `btree.c:6068-6307`, `vdbeaux.c:4705-4915,5085-5130` |
| Explicit root movement versus restoration key lifetime | `btree.c:5586-5670` |
| Directional skip consumption and empty restoration | `btree.c:6366-6394,6456-6484` |
| Insert/delete/clear fences and insertion's own moved cursor | `btree.c:9460-9496,9550-9580,9880-9980,10322-10348` |
| Cursor close and moved VM cache/NULL handling | `btree.c:4860-4892`, `vdbeaux.c:3838-3860` |

Executed native controls distinguish these real domains:

| Consumer | Key fields | All metadata fields | Encoded fields | Restored fields |
|---|---:|---:|---:|---:|
| Ordinary / UNIQUE nullable rowid index | 2 | 2 | 2 | 2 |
| UNIQUE NOT NULL rowid index | 1 | 2 | 2 | 2 |
| WITHOUT ROWID primary key | 2 | 4 | 4 | 3 |
| WITHOUT ROWID secondary index | 3 | 3 | 3 | 3 |
| Bounded ordering, one key | 1 | 4 | 4 | 2 |
| Bounded ordering, two keys | 2 | 4 | 4 | 3 |
| DISTINCT / aggregate DISTINCT | 1 | 2 | 1 | 1 |
| Automatic covering index | 4 | 4 | 4 | 4 |
| Aggregate ordering | 2 | 4 | 3 | 3 |

The compound controls selected native sorter/merge programs rather than a
live index cursor; do not claim an executed B-tree restore for those controls.
Their comparison KeyInfo and the current membership relation still require
explicit domains. Decoding every stored field, or treating the existing
comparison span length as native `nKeyField`, is not correct.

## Decision

### 1. Make each opened comparison domain explicit

Extend the canonical `IndexComparisonInfo`, not a second owner. A typed
`IndexKeyShape` records native key/all counts. Retain the current operational
comparison-prefix width separately from the one owned array's all-field
width; default extra slots are native BINARY/ascending/NULL-first.
Validate nonempty input, supplied orders, key/all bounds and representable
native counts before the one allocation. Copies/moves still allocate nothing.
The existing `CopyOf(columns)` denotes the explicit complete-key domain
`key = all = columns.size()`, not a payload-width inference.

Replace ambiguous public span-taking index-open/create boundaries with the
already-owned comparison-info handle. All current production and unit/OOM
callers must construct the actual domain; retain no compatibility overload
that guesses a missing native shape. This is an intentional clarification of
the current MVP C++ contract, before C API compatibility, not a migration
layer. Encoded operations still support their documented unique comparison
prefix and extra payload when the explicit shape describes that domain.

Read bytecode carries the native key count; its existing full index metadata
and record count supply the all count. Lowering supplies schema
`unique_not_null ? key_term_count : physical_column_count`, including the
WITHOUT ROWID primary policy, and verification rejects missing/invalid
index counts. Existing write descriptors already supply the corresponding
uniqueness/key policy.

Membership roots use their logical key width and sufficient native extra
metadata, including the native spare slot. Automatic lookup roots use the
complete physical key, including covering fields and rowid. Top-N uses its
ORDER BY key count, sequence and the existing packed-payload wrapper from
ADRs 0055/0118: restoration includes the sequence but not the payload BLOB.
Its all count describes that accepted wrapper, not a guessed unwrapped
SQL result width. Retired-root reuse receives the new domain explicitly.

Share the same info from each root writer into its read cursors; do not copy
another column array or keep top-N's redundant backend column owner.
Metadata creation and physical opening are separate fallible boundaries:
complete factory-plus-index-open retains the original two-allocation bound,
while an already-owned-info open allocates only its Pimpl. Keep root/kind,
Pager, snapshot, transaction and statement-authority validation.

### 2. Translate the actual saved-key unpacking

Add one move-only `UnpackedIndexRecord` beside the existing record types.
It allocates one typed `RecordFieldView[]` array of `nKeyField + 1` entries,
then uses the canonical bounds-checked record parser/cursor. Text and BLOB
fields borrow the saved encoded buffer; no owned `SqlValue` conversion or
per-field allocation is allowed.

Expose only the fields actually decoded. Reject zero fields or a decoded
count above `nAllField`, and preserve typed malformed/OOM cleanup. Use the
existing field-comparison template through borrowed-field overloads of
`CompareIndexRecord`; add no second decoder or comparator. Restore equality
uses the existing equivalent-prefix/default-zero result.

Retain the allocation-before-unpack order. Standard C++ never overreads a
record, even though the saved buffer retains native padding. Existing complete
record/header and overflow-error visibility is not weakened to imitate an
unsafe C read. The unpacked array ends before its saved-byte owner is freed.
Ordinary existing value/encoded operations keep their current comparison
prefix and scratch behavior; the new allocated decoder serves real saved-key
restoration, not unrelated per-row work.

### 3. Extend the one physical position state machine

Generalize the current table-only names and narrow physical hooks in
`CursorState` to one rowid-or-encoded-key protocol. Store one typed saved-key
variant: signed rowid or owned padded `ByteBuffer`. Reuse the existing owner
list, phases, Multiple flag, signed skip and nearest kernels.

Capture raw index payload directly into its one destination buffer through
the current payload reader. Reuse/extract the writable payload loop rather
than reading into a second resizeable scratch and copying it again.
Invalidate metadata even on failure; release the full path only after a
successful key capture. Earlier list saves remain saved when a later peer
fails. Table capture still allocates nothing and reads no payload.

Restore sets Invalid before allocating/unpacking and moving once. Keep the
saved bytes on failure without automatic retry; free them only after a
successful move and after borrowed decoded fields end. Preserve signed
SkipNext semantics, including repeated saves and empty trees.

An internal cold root reset must not clear a saved buffer that its decoded
key currently borrows. Explicit First/Last/Seek/Clear discard obsolete saved
work, including work retained after a failed restore; restoration calls the
same root loader without that explicit-position cleanup. Do not add another
root traversal, generation or cursor registry.

### 4. Enable the real index fences and VM consumers

Index insertion saves peers before its own positioning. Index deletion saves
after required self-positioning and before the first write, including
positioned-largest deletion. Index Clear uses the same root-filtered fence.
Retained sorted append keeps its strict increasing-key precondition and
canonical invalid/moved-path root/rightmost reacquisition; insertion does not
restore an obsolete original row merely to insert a new one.

Preserve the existing mutation checkpoint/header-lease and partial-write
failure boundaries. Root Drop stays conservatively Busy while any same-root
physical cursor exists, including pinless saved cursors. Schema destruction
and global rollback/fault coordination remain following work.

Expose index HasMoved/HasPosition/RestorePosition through the same read
facts as tables. Extend the VM's existing moved-record restoration and
physical-neighbor dispatch to actual index cursors. Invalidate borrowed
record/field caches before restoration, publish deleted-original NULL rather
than a neighbor's stale field/rowid, and let Next/Previous consume the same
directional skip. Preserve virtual outer-join NULL suppression.

## Test-first and closing boundary

First record the real public index-peer Busy regression. Cover read and
writable peers, insert/replace/delete/clear, both skip signs, repeated saves,
empty trees, interior/cross-page/overflow keys, collation/NULL/descending
order, unrelated roots and complete pin release. Match native raw-byte
padding, actual key/all/unpack domains, borrowed field lifetimes and one
unpack allocation without per-field copies.

Exercise pre-mutation save OOM/I/O/corruption, partial list saves, restore
allocation/root-I/O failures with no retry, explicit-position cleanup and
saved-cursor moves/close. Include real callback-mutated covering/noncovering
index records and index rowid reads, deleted NULL continuation, and retained
append after peer activity.

Keep all allocation ceilings, the selective 2,000-byte fault, 36/37 lowering
contracts, interoperability, thresholds and original model deadlines.
Complete focused and freshly linked fast units, affected GNU/sanitizer/
instrumented and quality gates, exact-tree GPT-5.6 Sol review and fresh
native/matched evidence. Publish its complete source ledger before final
batch closing. Final exact-head integrated local/hosted gates and review
remain mandatory before squash merge and branch cleanup.
