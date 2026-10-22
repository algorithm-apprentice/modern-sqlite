# ADR-0137: Native Overflow Payload Acquisition

- Status: Accepted
- Date: 2026-10-22
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 12

## Executed baseline and prerequisite closure

PRs 235-239 establish canonical database/physical cursor ownership, real
table/index save/release/restore and global savepoint/rollback coordination.
The former PageCache-generation proposal was withdrawn before production.
It is not a prerequisite or an implementation option for this source.

The unchanged SQLite-created fixture, rowid 44, four-page caches and four
sixteen-byte tail reads still reproduce the remaining mechanism gap on
merged main `749504b`: Modern acquires and misses 24 pages, while native
records three cache hits and one miss. Bytes agree. This is fixed-work
evidence, not a claimed cross-engine timing ratio.

Executed native lifecycle controls additionally verify lazy allocation,
at-least-two-times capacity, invalidation without freeing the array on
save/movement, restore reuse, growth-OOM retaining the old allocation and
invalid flag, retry growth and no activation for a fully local payload.

| Responsibility | Pinned SQLite source |
|---|---|
| One cursor-owned page-number list and close cleanup | `btreeInt.h:527-558`, `btree.c:4878-4880` |
| Root/position/movement invalidation | `btree.c:573-584,788,5496,5558,5648,6120,6223,6273,6422,6512` |
| Two-times growth, active-slot zeroing, jump and lazy population | `btree.c:5155-5355` |
| Skipped-page pointer-map guess and ordinary link fallback | `btree.c:5037-5088` |
| Direct-read capability and dirty-cache/WAL admission | `pager.c:795-826` |
| Native four-byte output-prefix save/restore and direct read | `btree.c:5293-5335` |
| Nonmoving root creation and drop/clear | `btree.c:10098-10155,10324-10470` |

### Current root-lifetime boundary

Actual SQL Destroy rejects any other active reader VM, including an
unrelated table and non-autovacuum databases (`vdbe.c:7080-7122`). Four
executed native controls confirm LOCKED until that reader closes in both
vacuum modes. Modern SQL DROP/Destroy and writable auto-vacuum are not yet
admitted. Do not implement those deferred features merely to add the cache.

The existing public backend Drop retains ADR-0130's conservative same-root
cursor admission, including saved/pinless/faulted cursors, and root-handle
incarnation checks. There is no current production Drop caller. Temporary
roots are cleared and reused, not destroyed; their lease owns its physical
cursor and closes it before retirement. Clear and mutations use the real
same-root fence, while schema rollback trips all affected cursors.
Non-autovacuum creation moves no existing root/overflow pages. These current
contracts make cached IDs safe without claiming native SQL-destruction
parity. Future root relocation must translate native global invalidation
before admission.

## Decision: translate the complete read-acquisition cluster

### 1. One cursor-owned list and one payload walker

Put the native page-number list in the existing CursorState, shared by read
and writable physical roles. Use one `std::vector<PageNumber>`, a valid flag
and its existing move/destruction lifetime. Empty construction and moves
allocate nothing. Zero is the typed unknown-page sentinel.

On the first needed overflow access with an invalid list, compute the
checked ceiling overflow-page count. If it exceeds current capacity,
reserve at least twice that count before discarding old contents. Allocation
failure preserves the old allocation and invalid state. Reconstruct/zero
only the active count, retain high-water capacity, then mark valid. Capacity
is the portable C++ allocation contract, not native allocator usable-size
ABI. Local-only and zero-length requested reads allocate nothing.

Move the existing checked payload-copy work into one same-layer reader.
Both physical roles call it; do not keep equivalent read/write traversal
algorithms. Preserve the existing read-role copy-counter attribution rather
than silently changing diagnostic meaning during the refactor. All owned
raw bytes remain ByteBuffer; the new vector contains typed page IDs only.

Reuse cached IDs for the requested starting page and known skipped links.
Populate each visited ID before acquisition. Keep the requested-range and
complete-chain extents distinct: record-prefix comparison retains its
existing complete-chain malformed/I/O validation even when it copies only a
prefix. Preserve ADR-0103's consumed-link boundary; never follow or validate
an unused terminal word.

### 2. Invalidate at the actual native position boundaries

Reuse the same save/trip/release paths, not another observer or generation.
Invalidate the list on row/index movement, explicit positioning, clear and
root/path changes; keep its allocation. Metadata invalidation after every
save attempt clears validity even on failure. Trip/close release positions
through the current owner. Move transfers the list, flag and registration
without allocating; the moved-from record is empty.

Use the existing read index setter and one canonical writable index setter
for actual position changes. During index binary search, invalidate before
each candidate's overflow read and after comparison, matching native
`IndexMoveto`; one candidate cannot reuse another's list. Ordinary repeated
reads of an unchanged positioned entry retain it. Directional no-op skips
do not invent movement or another root walk.

### 3. Translate the skipped-link prerequisite, preserving strict decoding

Reuse PointerMapPageFor/IsPointerMapPage and the current pure map decoder.
The guess path is admitted only when the canonical database header enables
auto-vacuum (`largest_root_page != 0`, matching native metadata word 4);
geometry alone never establishes that mode. A non-auto-vacuum database
reads the current overflow link without probing a pointer-map page.

Start the candidate at checked `current + 1`. Skip pointer-map pages and
the geometry's locking page with checked arithmetic. Probe a map entry only
when the resulting candidate remains within the current snapshot page
count. If no representable in-snapshot candidate remains, read the current
overflow link instead. Infer that candidate only when its raw map type is
LaterOverflow and raw parent equals the current page. A valid mismatch
likewise reads the actual link; a map decode/I/O error propagates rather
than becoming a mismatch. Preserve native acquisition and error ordering.

The existing PointerMapView::entry also validates parent semantics more
strictly than native ptrmapGet. Extract one raw entry decoding operation
with bounds/type checks, shared by the existing strict entry and this exact
native comparison. Keep strict parent checks in the existing API and
inspector; do not duplicate parsing, weaken them or silently catch a malformed
decode as a normal mismatch.

### 4. Translate native direct overflow reads through Pager

Add one narrow fallible Pager operation that reports direct-read
eligibility and, when eligible, reads the requested leading-link-plus-data
span through its existing File. Use current SubpageRead capability, actual
open backing file and no-dirty-cache checks. WAL frames are not currently
admitted; do not invent WAL handling or bypass its existing admission.
Memory-only or ineligible paths use the existing Pager read.

The B-tree caller additionally requires zero intra-page offset and four
already-emitted bytes within its actual destination span. Save those four
bytes, perform the native overlapping read, obtain the next word only on a
successful read, and restore the saved bytes on every path. Prove span
bounds with indices before forming pointers; no out-of-array pointer
arithmetic, overread or user-output corruption is permitted.

An actual I/O/short-read error is explicit, never a success-shaped fallback.
Keep direct acquisition distinct from cache hit/miss accounting and record
actual physical reads through the existing counters. Prefix-only complete
validation cannot use a shortcut requiring output bytes it does not own.
This is one native acquisition mechanism, not a second file reader in the
format module.

## Ordered related-source delivery

Use one reviewable related batch under ADR-0106:

1. Shared payload walker, cursor-owned page list and real invalidation, from
   a freshly qualified green baseline and a focused cold/repeated-range red.
2. Pointer-map skipped-link and direct-read prerequisites through that same
   walker, developed test-first before final matched qualification.

The intermediate first source does not claim full payload parity. In
particular, native full warmup may directly read data pages that Modern
still caches until source 2; explain any warm-cache hit/miss difference
rather than misreporting a speedup or altering the frozen baseline.

Each source needs its own complete fast tier, affected failure/ownership/
allocation/isolated evidence, GPT-5.6 Sol review, commit and published ledger
before the suffix begins. Final integrated qualification uses the exact
combined head, all native controls, existing matched workloads and all nine
hosted checks before guarded squash and proof/cleanup.

## Observable and failure contracts

Keep all existing fixtures, workloads, seeds, copy/prefix allocation ceilings,
36/37 lowering caps and model deadlines. Existing copy tests already warm
the complete payload before measuring ownership allocation; retain that
one-result-allocation contract and add independent cold-list/growth OOM
windows instead of widening old limits.

Cover table/index and read/writable roles, overflow interior keys, forward/
reverse movement, seeks, saved/repeated SkipNext, peers, unrelated roots,
clear, schema/global rollback, root reuse, moves and close. Exercise
partial lists, grow OOM, skipped-link and data I/O, malformed consumed links,
unused terminal links, pointer-map guesses/mismatches/errors, direct-read
admission and exact output-prefix restoration on success and failure.
Local payload remains borrowed without allocation or I/O.

Source 2 explicitly tests non-auto-vacuum admission, pointer-map/locking-page
candidate skipping, checked arithmetic and end-of-snapshot fallback, in
addition to auto-vacuum matching and malformed-map controls.

Freshly reproduce the four-tail-read fixed work and bytes after both
sources. Qualify the original cold/warm controls and existing non-target
timings/counters without threshold changes. Lazy Column/result ownership,
remaining ledger-12 attribution, later audit ledgers, new SQL and nodes
44-48 remain separate ordered work.
