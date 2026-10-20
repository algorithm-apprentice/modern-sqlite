# ADR-0122: SQLite Streaming STAT1 Accumulation

- Status: Accepted
- Date: 2026-10-20
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 11 (`write-analyze`)

## Decision

Translate the pinned unlimited STAT1 analyzer, including its prerequisite
page count and distinctness-test shortcuts. Replace per-row `CopyPayload()`,
`DecodeRecord()`, and owned-value replacement with the existing bounded
record-prefix reader and retained field buffers. No second analyzer,
comparator, payload walker, value container, or statistics format is added.

| Responsibility | Pinned source |
|---|---|
| `StatAccum`, zeroed counters, initialization and destruction | `analyze.c:265-460` |
| First-row and changed-suffix `anDLt`/row-count updates | `analyze.c:695-790` |
| STAT1 text and near-unique normalization | `analyze.c:814-895`; `sqliteInt.h:904-909` |
| Physical count, tested width, previous fields, UNIQUE shortcuts and scan | `analyze.c:1059-1270` |
| Publication and direct table count | `analyze.c:1270-1455` |
| Exact page-oriented count and Count dispatch | `btree.c:10547-10616`; `vdbe.c:3940-3957` |
| Borrowed/persistent field reads and reusable buffers | `vdbe.c:3035-3378`; `vdbemem.c:310-324,1447-1514` |

ADR-0052 remains the SQL, scope, schema-refresh and transaction contract.
Its generic exact-distinct description is refined by these native policies;
STAT1 is an estimate, not permission to replace SQLite's shortcuts with
another full-key distinct algorithm. Already readable expression/partial
indexes and WITHOUT ROWID primary/secondary indexes remain analyzable.
No new CREATE INDEX shape, STAT4 sampling, analysis limit, grammar, catalog
format, or planner mechanism is admitted.

## Canonical prerequisites

Add `CountEntries()` to both read cursor facades through one `CursorCore`
implementation. Translate SQLite's page walk using the existing bounded
frame stack, child/parent movement, snapshot checks and page validation.
Count only leaf cells for table trees and both interior and leaf cells for
index trees. Do not inspect or copy record/overflow payloads. Empty trees
return zero; completion invalidates the logical row position and requires
an explicit subsequent `First()`. Failure invalidates position and propagates
the existing typed error. No additional traversal stack, visited set or owner.

Expose a synchronous borrowed index-record prefix read, using the current
comparison reader rather than copying its algorithm. It accepts a nonzero
field bound, caller-owned `ByteBuffer` scratch and an explicit maximum
complete-record size. Fully local prefixes borrow the current page;
nonlocal prefixes borrow that scratch. Views end before cursor movement or
scratch modification, including reuse without reallocation. No prefix/payload
cache is added to the cursor.

Factor only the shared overflow-prefix acquisition from
`CompareCellPrefix()`; preserve its local complete-record comparison path,
comparison counters, complete-field shortage handling, snapshot/feasibility
checks, complete required overflow traversal and error precedence from
ADR-0088. Keep the existing range/full-copy APIs unchanged. Feasibility is
checked before allocation, and the caller's whole-record value limit is an
early explicit allocation guard. No error selects a full-copy fallback.

Extend `RecordFieldView` with a copy-into-caller-buffer operation that returns
a view of the retained value. Scalar copies allocate nothing. TEXT/BLOB
copies reuse canonical high-water `ByteBuffer` storage and expose only the
current logical bytes, including empty values. Grow only when necessary;
allocation failure is explicit and cannot leave a published dangling view.
Expose the existing field comparator through the codec, not a second
comparison implementation. The codec still performs no Pager/VFS I/O.

## Retained STAT1 state

One instruction-local accumulator owns the native unsigned row count and
`anDLt` array; its physical column count includes the rowid/PK fields except
for WITHOUT ROWID primary storage. One fixed vector of previous-field slots
contains only `RecordFieldView` plus canonical buffer ownership. Initialize
its complete shape before scanning and never resize populated borrowed slots.

Propagate existing catalog uniqueness/storage facts through `BoundAnalyzeIndex`
and a verified typed bytecode policy; do not pass catalog objects to the VM:

| Policy | Accumulator columns | Compared previous fields |
|---|---|---|
| Ordinary or nullable UNIQUE secondary index | Complete physical index width | Physical width minus one |
| UNIQUE NOT NULL secondary index | Complete physical index width | Logical key width minus one |
| WITHOUT ROWID primary storage | Logical primary-key width | Logical primary-key width minus one |

For a single-column nullable UNIQUE index with one compared field, a non-NULL
previous value bypasses subsequent comparisons and previous-field replacement,
as in native code. The row's change boundary remains zero. In particular,
`NULL,NULL,1` under a UNIQUE DESC index produces native `3 1`, not the generic
distinct result `3 2`. A UNIQUE ASC control produces `3 2`.

Run exact page count before accumulator initialization and `First()`, matching
native Count/stat_init/Rewind ordering. On the first row, retain tested fields
and increment only the row count. Subsequently compare borrowed fields in
native order until the first difference, replace only that changed suffix,
and update `anDLt` from that boundary before incrementing the row count.
Do not replace an equivalent leading value merely because a later field
changes: custom-collation arguments and INTEGER/REAL representation must retain
the native previous-register intent.

Retain the existing Modern structural, minimum-logical-field, overflow and
whole-record value checks even when a native UNIQUE shortcut omits field
comparisons. Request at least one prefix field for this validation; do not
weaken ADR-0088 by using a zero-field prefix. Validation adds no collation
callbacks or owned SQL decode. Working storage is bounded by key arity,
maximum observed field sizes and the current bounded prefix, never row count.

Produce the existing STAT1 string with the native ceiling/near-unique rules.
Keep empty ordinary indexes/tables omitted and emit zero entries only for the
existing partial-index case. Direct table statistics use the same page count.
All local state and pins end before existing STAT1 insertion, catalog refresh,
statement completion or rollback. Existing generation/cookie/change semantics,
metadata/value admission, typed errors and corruption detection stay intact.

## Delivery and evidence

Deliver three ordered, buildable slices in one ADR-0106 batch: canonical cursor
count/prefix-read prerequisites; reusable borrowed-field retention/comparison;
then STAT1 policy metadata, accumulator and VM integration. Each needs focused
red/green, a complete default-ALL development build before complete fast units,
implementation review and append-only evidence before its successor. Important
design/code remain parent-authored; fresh GPT-5.6 Sol reviewers only review.

Use existing cursor, codec, binder/lowerer, VM/session, allocation/OOM, crash,
native compatibility and performance helpers. Add small controls for table vs
index interior counts, page-one/empty roots, malformed links/depth, I/O/snapshot
failures, pin cleanup, local/overflow prefix lifetime, high-water field reuse,
changed-suffix callback arguments, NULL/direction/UNIQUE policies, imported
WITHOUT ROWID/partial cases, and original resource contracts. Do not add a
large corpus or weaken the existing 37-allocation ANALYZE lowering contract,
other allocation ceilings, workloads, thresholds or deadlines.

The frozen fifteen-case before run is `bulk-create-index-final-v1` at
`0794a1d`; all 1,954 non-Markdown inputs equal merged prerequisite `9d26aae`.
ANALYZE records 25,074 allocations and 2.87x SQLite wall time. Six small
independent native controls are frozen separately. Capture a fresh matched
after run, run native contracts early and close the actual final exact-head
plan, required hosted gates and integrated review before merge. Ledger 12 and
new SQL features remain blocked until this repair closes.

## Cursor prerequisite slice evidence

The focused count control first fails because `CountEntries()` is absent.
The implemented shared page walk and borrowed prefix reader pass all 32
affected cursor/prefix/allocation cases. A complete default development build
precedes the passing 1,483 ordinary plus 92 isolated fast cases. Warm table
and index counts allocate zero; an overflow prefix allocates once and reuses
caller scratch without further allocation. Exhaustive allocation failures
remain typed and leak-free. Deterministic read failures prove that page
counting does not read overflow payloads, and existing full-chain comparison
failure/format precedence remains unchanged. At this slice boundary, the
retained-field and STAT1 VM integration slices had not begun.

## Retained-field prerequisite slice evidence

The focused field-retention control first fails because `CopyTo()` is absent.
The canonical copy-into-buffer operation and exposed shared comparator pass
81 affected codec/golden/allocation cases. A complete default development build
precedes the passing 1,486 ordinary plus 92 isolated fast cases. Tests retain
only logical bytes across large-to-small, scalar, empty TEXT/BLOB and embedded
NUL transitions, outlive the source record, and preserve exact mixed numeric
and collation semantics. Allocation failure leaves old storage intact; cold
byte retention allocates once and 1,024 warm copies/comparisons allocate zero.
At this slice boundary, STAT1 VM integration had not begun.

## STAT1 integration slice evidence

The focused nullable UNIQUE DESC control first returns `3 2` instead of
native `3 1`. The translated policy, count prepass and accumulator pass that
control, the ASC `3 2` counterpart and the zero-compared-field UNIQUE NOT NULL
case. A fixed custom-collation trace proves that equivalent leading spelling
survives later-field changes and that native UNIQUE/primary compared widths
omit the final key comparison. All 14 affected cases pass in Debug, genuine
GNU 16 and ASan/UBSan. The extended existing VM injector sweeps every Step
allocation across retained TEXT growth and overflow prefixes; each failure
and the whole-record value limit release pins and preserve the original
database bytes. The original 37-allocation lowering contract is unchanged.
A complete default development build precedes the passing 1,490 ordinary plus
92 isolated fast cases. Integrated native, matched performance and final-head
closing gates remain outstanding.
