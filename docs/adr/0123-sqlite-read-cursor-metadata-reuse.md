# ADR-0123: SQLite Read-Cursor Metadata Reuse

- Status: Accepted
- Date: 2026-10-20
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12 (`execution-vm`), shared cursor prerequisite

## Evidence and scope

At merged source `4a39f10`, three unchanged, matched controls reproduce
Modern/SQLite wall ratios of 5.856023x for unselective covering-index access,
6.518114x for global functions and 4.325938x for integer memory sorting.
Existing benchmark `profile` commands replay identical work for both engines.
Exclusive samples identify repeated page/cell parsing, record decoding,
value operations and sorter comparison; they do not justify replacing the
typed instruction representation with a speculative compact IR or executor.
Modern instructions occupy 28 bytes versus the pinned native 24-byte layout.

The shared cursor discards page headers already validated by `Open` and
`DescendChild`, reparses them in `CurrentPage`, and decodes the same current
cell separately for rowid, payload and prefix consumers. Native SQLite
retains both forms of metadata:

| Responsibility | Pinned source |
|---|---|
| Initialized page header and page admission | `btree.c:2248-2295,2338-2443`, `MemPage.isInit` |
| Current cell metadata and payload callers | `btree.c:4921-4928,5155-5473`, `BtCursor.info` |
| Child/root/parent and seek invalidation | `btree.c:5486-5676,5825-6298` |
| Next/previous invalidation | `btree.c:6417-6427,6509-6520` |
| VM column/cache and row suspension | `vdbe.c:1806-1840,3035-3378,6530-6705` |
| Canonical current Modern owners | `CursorCore`, `BtreePageView`, `BtreeCellView`, immutable `ReadPagePin` |

This batch repairs that concrete metadata mechanism, not the entire VM gap.
VM column/value ownership and remaining execution costs still require source
mapping and profiles before ledger 12 can close. Ledger 13, new SQL and later
DAG nodes remain blocked. No opcode, register representation, comparator,
record format, payload cache or alternative execution path is introduced.

## Decision

Retain one optional validated `BtreePageView` for the current frame and one
optional `BtreeCellView` for its current positioned cell, inside the existing
shared `CursorCore`. Both are borrowed metadata over its immutable pin.
Neither belongs in every reserved path slot or in a new lookup container.

`PageCache::LookupExclusive` and read-pin promotion exclude other pins, and
mutable bytes require an exclusive dirty pin. The image cannot change while
this cursor retains its read pin. A writer may change a released child;
the next descent must acquire, parse and validate that child again.

Keep all admission and corruption checks in their current order. Seed the
page entry from the existing successful root/child parse. On a page-entry
miss, use the same `BtreePageView::Parse`. Publish a cell entry only after
the current position is valid and canonical `BtreePageView::cell` succeeds.
Do not publish binary-search candidates or memoize errors.

The private accessors return typed results holding borrowed nonnull const
pointers, rather than transporting large page/cell objects on every hit.
No reference survives its entry's replacement or invalidation.

Invalidate the cell entry before every positioning/movement attempt and
before changing a selected cell or child slot. Clear both entries before a
frame is replaced, pushed or popped; install a newly admitted child header
only after the parent reference's last use. A still-pinned parent is parsed
on the next page-entry miss. Clearing position retains only the existing
root pin and may retain that root's page entry, never a cell entry.
Snapshot failure, errors and moves preserve existing terminal/retry rules.
Payload-read errors still leave the physical position unchanged.

Declare metadata after the frame array so borrowed views end before pins.
Retain the fixed 20-frame path and existing allocation sites. The archived
per-frame header proposal expanded the core beyond the unchanged 2,000-byte
lazy-value allocation boundary; do not repeat that layout or raise any
resource ceiling. Measure the actual current-core and entry sizes.

Cell offsets, varints, payload lengths and overflow references still use the
canonical decoder. Reuse is valid only for the same immutable positioned
cell. Payload bytes are not retained, and every payload request keeps its
snapshot, range, feasibility, I/O and full-chain/error-precedence checks.
ADR-0122 page counting still counts pages, not payloads, and ends
unpositioned; adapt its traversal to the same borrowed page-entry lifetime.

## Development and qualification

Deliver two ordered ADR-0106 source slices: current-page metadata and its
fixed-work evidence; then current-cell metadata and its invalidation evidence.
Review and commit the accepted design before either implementation.
The previously preserved compact page-view candidate is prior art only:
adapt its bounded mechanism to current sources without applying or dropping
unrelated stashes, and do not reuse its historical qualification.

Reuse the existing opted-in, thread-local `ProbeCounterCollection` for two
typed read-work metrics: page-header parse attempts and cell-decode attempts.
They require a distinct collection kind and compile out of ordinary builds.
Keep the ten existing benchmark counter names, schemas and frozen workloads
unchanged. Scope metric tests around cursor work, not setup/catalog loading.

Start from the green, source-identical completed qualification of `c552e9f`;
all 1,954 non-Markdown entries equal `4a39f10`. Add characterization before
the refactor: pinned root/child exclusion, descendant release, changed-child
header/cell rejection, same-snapshot retry and moves. Fixed-work controls
must first show repeated header/cell decoding, then one decode for repeated
access to the same admitted frame/position. Check directional movement,
seeks, count, EOF, empty roots, failures, overflow and pin cleanup through
existing module tests. No large corpus or artificial behavioral failure.

Each slice requires focused/complete-fast evidence after a full default
development build and GPT-5.6 Sol implementation review. Close the actual
combined verification plan once at the final exact batch source, including
native interoperability, original sanitizer models, compiler/fuzz/resource
gates, fresh unchanged matched controls and isolated timings. Preserve
counts, thresholds, deadlines and the selective allocation boundary.
Publish the complete durable ledger and obtain integrated exact-head review
and all nine hosted successes before guarded squash merge and cleanup.
An incremental gain does not establish engine-wide parity or close ledger 12.

## Current-page slice evidence

The new scoped read-work interface is initially absent. After its observer
is installed, the unchanged cursor performs 13 header parses for two page
admissions in the focused repeated-access control. Reusing the admitted
current-frame view reduces that count to two. The ownership characterization
passes before and after the refactor, including writable exclusion, released
child changes, malformed header/cell rejection, count, retry and move.

The complete default development build precedes 1,491 ordinary plus 93
isolated passing fast cases. All 78 affected cases pass in Debug, genuine
GNU 16 and ASan/UBSan; 35 instrumented cases pass after relinking the complete
instrumented tree, including its standalone allocation runner. Scoped probe
isolation and disabled argument/link elimination remain covered.

Frames remain 32 bytes, the page view is 128 bytes and the core is 1,048
bytes, below the unchanged 2,000-byte lazy-value failure boundary. There is
no additional allocation site. Private core move assignment is deleted to
avoid replacing pins before their borrowed metadata; public facade moves
still transfer the existing implementation owner.

Current-cell implementation and combined native/performance/fuzz/hosted
closure remain outstanding.
