# ADR-0132: Physical Index Comparison Ownership

- Status: Accepted
- Date: 2026-10-21
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12, index-position prerequisite

## Current ownership gap

ADR-0131 supplies one native cold index move, but a physical cursor cannot
restore an encoded key without stable comparison metadata:

- the read facade owns copied column orders and codec options outside its
  registered physical `CursorCore`;
- `IndexBtreeWriter` owns another copied column array, while its physical
  `WritableCursor` receives columns/options separately on each operation;
- retained index append state keeps only a field count and may outlive the
  public writer that supplied those orders.

Do not solve this with an outer-object callback, a metadata registry, an
erased pointer, another comparator or a save method that cannot restore.

Native `KeyInfo` is immutable comparison data with explicit shared lifetime.
Its reference-counted allocation is in `select.c:1586-1628`, schema-index
construction in `build.c:5733-5774`, and opcode ownership in
`vdbeaux.c:1395`. `vdbe.c:4530-4618,4690-4765` gives real cursors the
metadata; `btree.c:4720-4783` retains that comparison reference. Native
cursor cleanup precedes opcode/P4 owner release (`vdbeaux.c:2761-2815`).
The database/connection pointer does not cross the Modern dependency DAG.

## Decision

### 1. One immutable comparison-info owner

Add `IndexComparisonInfo` beside the existing `IndexColumnOrder` and record
comparison contracts. It owns one immutable array of the existing typed
orders through standard `std::shared_ptr<const IndexColumnOrder[]>`.
Factory creation copies a validated, nonempty span once; copies and moves
share or transfer its existing storage without allocation. Referenced
collations remain borrowed and must outlive every comparison, as today.

Use one standard shared-array allocation rather than a separately allocated
owner plus vector. Do not add a general intrusive-pointer library, custom
allocator, flexible-array extension, byte/object lifetime trick or root
metadata map. Default construction of an empty comparison-info owner is
not a valid index-open input; moved-from handles expose an empty view and
are rejected at fallible index boundaries.

Give `IndexColumnOrder` the canonical default BINARY/ascending/NULL-first
value. This matches native `KeyInfo`'s zero-initialized collation/sort slots,
not an invalid placeholder or error fallback. It also permits portable
standard shared-array construction: the local libc++ filled-array probe
uses one allocation, whereas GNU16 instantiates a default-construction path
for the currently non-default-constructible element. Qualify both actual
standard libraries before relying on the new representation.

The factory retains current nonempty, direction and NULL-placement checks
and typed allocation errors. No comparison width, collation lifetime,
record encoding, prefix policy or admitted field limit changes.

### 2. Put comparison context at each physical index cursor

The read physical core retains the comparison-info handle and its current
header-derived `RecordCodecOptions`. Move the existing index comparison and
bounded-prefix helpers there; the public facade no longer has a second
column/options owner. Reuse those exact helpers and their scratch lifetimes.

Keep the public span-taking read entry point and its current validation and
I/O/error order. Validate/admit the root through the canonical loader before
the existing metadata-copy allocation, then construct/register the complete
physical context. Failed metadata/Pimpl creation must unwind the same pins,
registration and owners. Table opening acquires no index metadata.

The index write facade retains the same comparison-info type. Every real
writable index cursor receives that handle and current codec options at
open, including transient point operations and retained append cursors.
Table cursors carry no index context. Use explicit table/index physical-open
boundaries and migrate all production/unit/OOM callers; retain no index
overload that silently supplies missing metadata.

Writable seek/insert/delete implementations use that one opened context,
not separately supplied per-call columns or options. Retained append state
shares it so destroying or moving the public writer cannot leave a dangling
comparison domain. Remove the old standalone append field-count copy when
the canonical info supplies that same count.

Preserve write/root/statement authority, tree-kind and geometry admission,
lease/checkpoint ordering, bulk append, malformed-input results and cleanup.
Metadata must outlive every physical comparison and end after any borrowing
views; database registration remains in the existing physical cursor only.

### 3. Do not invent the following restore domain

This slice makes the already-used ordering context complete and durable at
physical cursors. It does not add unused native-field-count knobs or infer
restore identity from all stored payload fields.

Native packed-key restoration unpacks at most `nKeyField + 1` fields, not
necessarily `nAllField`. Executed controls show a non-unique persistent
index with key/all counts 2/2, but bounded top-N with key/all counts 1/4,
four encoded fields and two unpacked fields. The following encoded-key ADR
must finish those consumer domains and preserve the native decode/search
allocation, I/O and cleanup order. Do not enable index peer saving merely
because metadata is now available.

Schema destruction, global rollback/fault coordination, overflow page lists,
native Column, new SQL and later DAG nodes remain outside this source.

## Qualification before the next source

Start from the qualified, reviewed, committed and ledgered ADR-0131 source.
Characterize valid defaults and shared owner copying/moving, caller-span
destruction, writer destruction with retained append, and physical-context
use after input metadata expires. Reject empty/invalid metadata, wrong tree
or Pager ownership and stale authority through the existing typed results.

Use the existing allocation/OOM runners to prove one metadata allocation,
allocation-free sharing, cleanup/retry and preserved low-level cursor-open
counts. Keep the original 2,000-byte selective fault and 36/37 allocation
contracts, all old ceilings, goldens, scratch/error visibility and deadlines.
No per-row or per-move allocation is admitted.

Run focused characterization, complete freshly linked fast units, affected
isolated GNU/sanitizer/instrumented and quality checks, unchanged native/
matched controls and exact-tree GPT-5.6 Sol review. Commit and append the
complete slice ledger before encoded-key decoding/restoration production.
The related draft remains unready until real index positions and its final
integrated local/hosted gates are complete.
