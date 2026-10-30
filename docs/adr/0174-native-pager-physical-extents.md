# ADR-0174: Native Pager Physical Extents and Shared Scratch

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #272 and ADR-0173

## Completed inventory and bounded finding

PR #272 delivers native savepoint cache replay, journal publication and
writeback readiness as main `d7d0869`, tree
`8673d9294baab9bec2c60432967ff179314ba94b`. Its actual-main CI and full
production analysis pass. The independent integrated review and append-only
delivery evidence close only ADR-0173's scope.

The four matched native MSVC Release controls retained from that exact tree
already demonstrate the next difference. During ordinary nested-savepoint
rollback, the native pressure control keeps its 4,304,896-byte main file,
whereas Modern shrinks its 4,087,808-byte main file to 8,192 bytes. Native
performs no truncate and Modern performs one. Both engines finish with the
same independently verified rows, integrity, two pages and no sidecars.
Different pre-rollback extents are not a claim of cache-pressure parity.
These are executed fixed-work observations, not timing measurements.

The remaining full-rollback equal-size truncate, commit growth through
`Truncate`, and logical-flag-based committed cleanup are source-derived
findings. Their runtime reds belong to the ordered implementation below;
they are not reported as already executed experiments.

### Completed pinned source map

| Native owner or operation | SQLite 3.54.0 source | Required translation |
|---|---|---|
| Extent validity and purpose | `pager.c:546-595` | Keep logical image, transaction-original size and transaction-local writeback extent distinct. |
| Write admission | `pager.c:5990-6045` | Initialize the writeback extent from the admitted logical page count, not a new filesystem query. |
| Ordinary and playback page writes | `pager.c:2467-2483,4527-4547` | Raise the estimate after the write attempt, including a failed or partial write. |
| Physical resize helper | `pager.c:2705-2759` | Query actual bytes; equal is no-op, shrink truncates, full-page growth writes one zero page at the final page offset. |
| Full/hot rollback size | `pager.c:2872-2980` | Apply that helper before restoring records, using writeback-ready or hot-recovery state. Do not trust the estimate instead of querying actual bytes. |
| Full/hot rollback synchronization | `pager.c:3029-3051` | Synchronize according to the same writeback-ready/hot proof, not whether resize changed bytes. |
| Savepoint size | `pager.c:3425-3558,7083-7152` | Reset logical size and change-counter readiness, without physical resize or extent reset. |
| Pre-sync committed growth | `pager.c:6717-6733` | Call the helper only when logical size exceeds the estimate, with the native locking-page adjustment. |
| Committed shrink | `pager.c:2193-2202` | After journal finalization, while EXCLUSIVE remains held, resize only when the estimate exceeds logical size. |
| Scratch geometry and allocation failure | `pager.c:3820-3880,5074-5100` | Retain one page-size-plus-eight-byte buffer; publish replacement geometry only after candidate setup succeeds. |
| Shared B-tree scratch | `pager.c:3883-3892`; `btree.c:1714-1718,7688-7692,7921-7925` | Reuse that same temporary page for page reconstruction rather than retain another page-sized owner. |
| Distinct cell-encoding scratch | `btree.c:2898-2945,4775-4780` | Keep the separate B-tree cell-encoding role; it is not the Pager temporary-page owner. |
| Close and failed open | `pager.c:4230-4290,5090-5100` | Release owned scratch. Preserve Modern's separately accepted explicit, no-hidden-I/O destruction contract. |

The helper updates the estimate only after successful differing-size work.
When actual size already equals the target, native leaves the estimate
unchanged. A smaller gap than one complete page performs no extending write,
but successful differing-size handling still publishes the requested
estimate. Ordinary/page-playback write attempts have a different error rule:
they raise the estimate even when the write fails. Preserve both rules.

### Existing canonical owners

`Pager` owns the file, cache, logical sizes, write phase and final cleanup.
`ApplySnapshot` and `RecoveryTarget::PreparePlayback` currently create
replacement caches independently. Neither owns retained temporary page
bytes. `image_size_changed_` describes a logical image change; resetting it
after a savepoint cannot prove that spilled physical growth disappeared.

The completed inventory also finds an existing equivalent whole-page
buffer: `BtreeWriteWorkspace::rebuild_scratch_`. The only production factory
is `OpenWriterCore`; its workspace belongs to the single admitted
`BtreeWriterCore`. Defragmentation, page rebuilding and cell editing use that
buffer synchronously. Its separate cell-encoding buffer has a different
lifetime and remains necessary. Adding a second Pager page buffer while
retaining this owned reconstruction buffer would not translate native
scratch ownership. This ADR changes only page-reconstruction ownership under
ADR-0044, not the remaining cell, record, balancing or cursor mechanisms.

Old root handles can retain their core after rollback, geometry replacement
or Pager destruction. Existing generation, active-statement and Pager
lifetime checks must reject them before any workspace bytes are accessed.
The single-coordinator claim remains the last fallible-setup boundary.

Journal cursor record buffers keep their existing bounded decoding and
borrowed-image lifetimes. They are not Pager-geometry scratch and cannot
supply it during open recovery or commit without reversing the dependency
DAG. No journal buffer or persistent-format ownership moves upward.

## Decision

### 1. Retain one canonical temporary page in Pager

Own one `ByteBuffer` of page size plus eight zeroed padding bytes in Pager.
Keep the existing lazy, snapshot-established geometry boundary. Factor the
two current replacement-cache sites through one private page-size operation:
prepare the candidate buffer and cache, then publish both without a further
fallible step. A failed candidate leaves the old cache, scratch, geometry and
snapshot identity unchanged. Same geometry reuses the allocation.

The canonical ByteBuffer constructor initializes bytes; do not introduce an
uninitialized allocator, stack page array, byte vector, pool or raw owner.
The scratch view used for a page is exactly one page, never the padding.
Normal destruction releases the buffer through RAII without filesystem work.

Change `BtreeWriteWorkspace` to borrow reconstruction bytes through
`MutableByteView` and retain only its distinct cell-encoding owner. Its one
factory takes an explicit exact-sized, nonaliasing reconstruction view.
Direct unit callers provide their own scoped `ByteBuffer`; there is no
allocating fallback or compatibility overload.

Use the existing Pager friendship with `BtreeWriteSession` to pass the narrow
borrow into `OpenWriterCore`; do not add a public scratch API, upper-layer
include or general resource provider. Validate writer state before using
the view. The workspace borrow expires on geometry replacement or Pager
destruction; stale cores must fail their existing lifetime/generation checks
before accessing it. No scratch-derived view survives a B-tree operation.

### 2. Track the native writeback extent independently

Add one private transaction-local page-count estimate. Initialize it from
`current_page_count_` after successful write admission. Raise it immediately
after ordinary and playback database-page write attempts, before returning a
write error. Physical-helper success updates it only as specified by the
native rules above. Reset it with completed write state.

Do not reset it during savepoint rollback or infer it from
`image_size_changed_`, journal dirtiness, a successful-write boolean or the
current filesystem length. The existing logical flag remains responsible
only for logical-image/header validation.

### 3. Use one physical-size helper

One private Pager helper owns actual-size comparison and physical resize.
Its callers already prove an open file, valid geometry and EXCLUSIVE
ownership in hot recovery, writeback-ready state or committed cleanup.

For valid input:

1. Compute the bounded target byte size and query actual file size.
2. Equal size returns success without truncate or write.
3. Larger actual size performs one shrinking `Truncate`.
4. Growth by at least one full page clears the retained temporary page and
   writes exactly that page at `target_bytes - page_size`.
5. A smaller partial-page gap performs neither extending truncate nor write.
6. Publish the differing-size estimate only after success.

Propagate size, write and truncate errors through the existing typed
boundaries. Preserve partial-write rollback and the still-present journal.
Do not add a VFS operation or optional size-hint API to implement this path.
Existing unsupported modes and corruption checks remain unchanged.

Full/hot playback invokes the helper before records when its existing target
proof permits database I/O. Derive database synchronization from that same
proof; remove the target's redundant actual-write boolean rather than make
an equal-size resize accidentally skip required rollback synchronization.

Commit uses the extent comparison, not the logical-change flag, to select
pre-sync growth and post-finalization shrink. Retain existing journal resize
authorization and error reporting. Apply the native locking-page adjustment
to the pre-sync target. Finalized shrink remains before SHARED downgrade.
A failure there retains `kWriterFinished` and retries only accepted explicit
cleanup, never page writes, synchronization, journal publication or rollback.

### 4. Keep savepoint reset logical

After savepoint playback validation, set the logical page count, reset change
counter readiness and discard cached tail pages. Perform no file-size query,
truncate or growth there, and retain the writeback extent.

Commit then cleans previously spilled growth even when rollback restored the
transaction-original logical size. Retain the accepted raw-Pager
empty-initialization no-op contract; a returned-to-empty image must still
clean any spilled physical extent before unlocking. Do not infer native SQL
empty-database behavior from this raw-Pager control: native B-tree savepoint
completion also calls `newDatabase` (`btree.c:4648-4675`).

## Ordered delivery and evidence

Use three coherent sources in one normal ADR-0106 batch:

1. Move the reconstruction owner into Pager and share the borrowed page with
   the existing writer workspace. In the existing allocation owner, prepare
   caller backing outside the measured region and specify no separately
   allocated reconstruction-page block during workspace creation, with a
   positive cell-encoding-buffer control. Observe the original redundant
   page allocation as a runtime failure before changing the factory.
   Record byte-block ownership separately from total heap calls: compiler
   container bookkeeping must not become a hardcoded one-allocation total.
   Preserve the positive total baseline and exhaust every actual allocation
   ordinal. Cover explicit borrow identity/size, page geometry replacement,
   all geometry-allocation failures, retry, stale handles and no hidden I/O.
2. Add the extent estimate and canonical physical helper for full/hot
   rollback and both commit phases. Begin with a real writeback-ready,
   equal-size full rollback that must not consume an armed truncate failure.
   Cover skipped final-page growth through one exact zero-page write,
   partial-page bounds, locking-page selection, allocation-free reuse,
   size/partial-write failures and complete existing crash campaigns.
3. Remove the existing savepoint physical shrink. Begin with spilled growth
   whose logical rollback must retain physical bytes and an armed size error.
   Cover repeated/nested rollback, return to transaction-original or empty
   size, successful final shrink, finalized cleanup failure/retry, exact
   final images and every mutating crash boundary.

There is one bounded staging exception in source 2: retain the existing
savepoint-only direct shrinking call, updating the new estimate after its
success, until source 3 removes that branch. That legacy operation is not
equivalent to general resize: routing it through the new helper would add
premature growth. Do not add a compatibility mode or another helper to
bridge the two sources. No intermediate source claims savepoint parity.

Each source requires focused runtime green, complete fast units,
implementation review and append-only exact-tree evidence before its
successor. Keep each source independently buildable. If this prerequisite
cluster exceeds the normal reviewable bound, split at a qualified prefix
rather than accumulate unrelated work.

At the final exact batch head, execute the change-aware native, sanitizer,
interoperability, model, crash, production analysis, discipline, format and
required hosted union. Repeat the four existing matched native MSVC public
controls unchanged: cache-only journal writes/syncs remain zero, and pressure
savepoint rollback performs no physical truncate in either engine. Verify
all final rows, integrity, geometry and sidecars with the pinned oracle, and
remove only hash-verified fixture files.

Keep all workloads, allocation ceilings, corruption assertions, deadlines,
native stack and compiler/sanitizer policies. Original failed runs remain
evidence. This closes only the named physical-size/scratch mechanism, not
ordinary data admission, preparation costs, the Python-only tool finding,
native benchmark clock portability, node 43 or later SQL features.

## Native full-TU quality clarification

The first source's native focused controls, complete 2,174-case fast tier
and 2,209-case preset pass before full native analysis reports nine
use-after-move diagnostics on the unchanged writable-cursor move constructor.
Exporting main `d7d0869`'s original implementation, internal owning header and
Pager header reproduces all nine with the same native compiler arguments and
project checks. The snapshot also retains the original sibling-header lookup;
the earlier incomplete include setup is not diagnostic evidence.

Make the base-subobject move explicit through `static_cast<CursorState&>`
before moving it. The constructor still moves that same base and then the
same independent derived members in the same order. No allocation, cursor
registration, address, lifetime or moved-from behavior changes, and no
const-cast, warning suppression or analyzer-policy exception is introduced.
This is a local green-baseline refactor, not a new storage mechanism.
Requalify the new exact source before implementation acceptance or the next
source; the earlier runtime reports remain the refactor baseline.
