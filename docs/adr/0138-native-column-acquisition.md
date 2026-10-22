# ADR-0138: Native Column Acquisition

- Status: Accepted
- Date: 2026-10-22
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Executed evidence and existing owners

PR #240 closes overflow acquisition at merged `bf9b23c`, accepted tree
`425f8b9895b5a4e4245b9d1fbc60b47e8023820a`. Column acquisition is still eager:
`RuntimeCursor` owns a complete overflow record, validates its entire header
and decodes every expected field on the first field request. Invalidation
also resets every decoded field.

On the unchanged row-44 fixture, selecting `binary_key` has the same two
native cache misses as selecting only the rowid. Modern reads eight physical
pages instead of its rowid control's two. Values agree; six overflow reads
are unnecessary. This is fixed-work evidence, not an engine-wide timing claim.
With a 64-byte native value limit set after prepare, the small column still
succeeds while requesting the 3,000-byte BLOB returns TOOBIG.

Five native-created table/index controls confirm that repeated nonlocal
table BLOBs share storage only above 4,000 bytes; 3,999/4,000-byte values and
a 5,001-byte covering-index value do not. Twelve native record controls also
establish lazy error timing, missing defaults, zero-header handling, present
NULL serial codes 10/11 and legacy-file reads of codes 8/9. These malformed
point controls are not whole-file validity evidence.

Reuse the current owners: `RuntimeCursor`, its actual physical cursor,
`SqlValue` logical/backing separation, `SqlValueAccess::RetainBytes`,
`SetRecordField`, the pure record codec and
`CursorState::ReadPayloadRange`. Do not introduce another value array,
record/payload traversal, executor, page generation or compatibility layer.

| Responsibility | Pinned SQLite source |
|---|---|
| Cursor metadata allocation and type/offset words | `vdbe.c:265-323`, `vdbeInt.h:65-172` |
| Incremental Column header and missing-field state | `vdbe.c:3035-3245` |
| Local scalar/byte stores and nonlocal dispatch | `vdbe.c:3248-3382` |
| Bounded ordinary field/header acquisition | `vdbemem.c:1462-1516` |
| Saturating 32-bit varints and serial lengths | `util.c:1713-1742`, `vdbeaux.c:3980-4011` |
| Large table-value ownership and replacement | `vdbe.c:741-813`, `vdbemem.c:1273-1365`, `printf.c:1680-1735` |
| Physical payload identity | `btree.c:4975-4981` |
| Result-row and table-write cache counters | `vdbe.c:926,1811,5977,6143`, `vdbeaux.c:2628` |
| Actual moved-cursor and close cleanup | `vdbeaux.c:2764-2790,3838-3860` |

## Decision

### One consumed-field codec and one metadata owner

Factor canonical header, serial-length and scalar decoding rather than
duplicating `RecordCursor`'s numeric/TEXT/BLOB interpretation. Preserve
allocation-free strict `RecordView`, `RecordPrefixView`, comparison and
encoder contracts. Their whole-record/schema/reserved-code checks remain.

Add a pure consumed-column interface over that shared kernel. It performs
no I/O, affinity or default substitution. Follow native bounded varint
decoding, including 32-bit saturation; no native overread is copied.
Column consumption accepts the tested zero-header missing-field case,
distinguishes present NULL codes 10/11 from missing fields, and reads codes
8/9 as native Column does even in an older-format malformed image. This
supersedes ADR-0018 only for this consumer, not persistent encoding or strict
whole-record validation. Transient virtual-table no-change flags remain
unimplemented.

Replace `decoded_fields`, `record` and `owned_record` with one bounded
type/offset metadata array and native parsed-count/header-offset state.
Use 32-bit code/offset words, not a vector of large value views. Provision
it at the existing cursor-open boundary and retain capacity. Parse only
through the requested field; publish progress after native bounds and
end-of-header/body checks. Unparsed entries are never read. Invalidation
ends borrowed row views and marks metadata stale in constant time.

If the complete header fits on the pinned page, borrow it. Otherwise fetch
only the bounded complete header into temporary `ByteBuffer` storage and
release that scratch after parsing, as native does. Do not retain a second
record/header byte cache. Apply corruption checks when their fields or
header end are consumed, not by eagerly scanning unrequested fields.
Keep existing page/cell and index-position admission; this batch does not
claim all malformed-image admission parity in those lower contracts.

### Read the requested field into its actual destination

Use the same Column path for read table/index, retained mutation and
relation-backed cursor sources. Keep rowid, virtual/deleted NULL rows,
missing defaults and their affinity at the existing VM boundary.
Check actual moved/tripped cursor authority before any metadata/cache hit.
Expose only the missing mutation-cursor rowid/local-payload/range operations,
sharing its existing validation and the canonical payload walker.
The existing full `row()` operation remains available for mutations.

Local fields use the canonical decoder and `SetRecordField`. Nonlocal
numeric fields need at most eight stack bytes. Nonlocal TEXT/BLOB reads
fill the destination's retained `ByteBuffer` directly, without an
intermediate complete record or a second field copy. Factor the existing
retention primitive to support both alias-safe copying and direct fill,
with one growth/terminator policy. Private prepare/commit access keeps the
logical value NULL until successful I/O; errors never publish partial bytes.
Keep owned moves, public factory/Clone independence and static program/
parameter borrowing unchanged.

The VM value limit applies to the selected field, not the entire overflow
record or its independently bounded structural header. Preserve exact
typed I/O/OOM/corruption errors, consumed links and cleanup order.

Use the native nonzero VM row-cache counter, incremented on ResultRow,
plus per-cursor stale status. This is `Vdbe.cacheCtr` translated into its
owning VM, not the withdrawn Pager-wide generation proposal. Retain actual
position/NULL/restore invalidations; the counter does not replace them.

### One native large-value entry per eligible cursor

Only a nonlocal table TEXT/BLOB longer than 4,000 bytes uses this entry.
Indexes, relation keys, local fields and shorter values remain ordinary
stores. Match the native column, VM row-cache counter, step-local table-write
counter and physical payload-offset keys. Add the typed table payload-offset
accessor only in the source that consumes it; do not substitute rowid.
Reset the step counter only after acquiring execution ownership, never
on a rejected nested Step. Account for every table insert/update/delete path.

Use one immutable shared `ByteBuffer` owner for cached bytes and destination
values. Reserve the native three trailing zero bytes, replace the previous
cache reference before allocation and publish key metadata only after a
successful read. Keep one canonical `SqlValue`, not a shared-value side
registry. Nonstatic Copy and public Clone remain deep copies. Scalar stores
release external sharing while ordinary internal buffers retain capacity.
Measure layouts and derive accounting from canonical storage types.

Ordinary movement invalidates header metadata; cache keys decide value reuse.
Open/close/root-owner replacement and execution cleanup retire cache owners.
Preserve ADR-0130's explicit cache retirement on actual peer restoration.
The executed native self-modifying SELECT control rereads old cached bytes
above 4,000 bytes while stored bytes have changed; SQLite documents this
same-connection visibility as undefined. Do not turn that artifact into a
new Modern guarantee or weaken existing peer freshness. This is the bounded
existing-contract exception, not a global invalidation mechanism.
Held result values remain immutable and alive when a cursor entry retires.
Close cursor/cache capabilities before releasing registers on every exit.

## Ordered delivery and qualification

Use three ADR-0106 sources in one related batch:

1. Shared record-decoding refactor from the unchanged green baseline,
   preserving strict format APIs and existing allocation-free behavior.
2. Native lazy metadata and ordinary field acquisition, including the needed
   writable facades and canonical direct retained-buffer fill.
3. Native large-value sharing, physical identity and full lifetime controls.

Do not merge the intermediate ordinary source without the planned sharing
source: repeated large fields can otherwise cause avoidable rereads.
Each source requires its own focused red/green or green refactor baseline,
full fresh fast tier, failure/ownership coverage, GPT-5.6 Sol review, commit
and published ledger before its successor.

Reuse existing fixtures, typed builders and failure injectors. Cover normal
and repeated fields, partial headers, boundaries/defaults/NULL/NaN, malformed
requested versus unrequested suffixes, per-value limits, header/data I/O,
OOM, aliased buffers, 4,000/4,001 boundaries, table/index exclusion, movement,
ResultRow, saved/exact/deleted restoration, peer callbacks, write counters,
trips, owner replacement and halt/reset/error/move cleanup. Keep all current
ceilings, the 2,000-byte selective failure, 36/37 lowering contracts and
original model deadlines.

Run the actual final-head verification plan once, including native
interoperability, compiler/sanitizer/model/fuzz, canonical quality and fresh
matched fixed-work/allocation/timing evidence. Require published complete
ledger, integrated review and the current hosted policy before squash and
named cleanup. Do not repeat matching unchanged evidence after review only.

Type/length-only compiler hints, remaining VM/function attribution, new SQL,
UTF-16, later audit entries and nodes 44-48 remain separate. Ledger 12 and
node 43 remain open after this batch.

## References

- ADRs 0018, 0024, 0101, 0106, 0111, 0123, 0124, 0130, 0133 and 0137.
- [SQLite same-connection isolation](https://www.sqlite.org/isolation.html).
