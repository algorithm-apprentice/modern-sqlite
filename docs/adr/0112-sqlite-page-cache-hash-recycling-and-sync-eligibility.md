# ADR-0112: SQLite Page-Cache Hash, Recycling, and Sync Eligibility

- Status: Accepted
- Date: 2026-10-18
- Module graph node: `implement-advanced-sql`
- Audit entry: `storage-page-cache`

## Context

ADR-0021 introduced a connection-local page cache with move-only pins,
intrusive clean/dirty recency, stable frame addresses, soft capacity, and an
advisory dirty-page writeback request. ADR-0044 later added in-place pin
promotion and page-number rekeying.

Those ownership contracts are valid, but the container and miss path do not
match SQLite's first mechanism:

- `PageCache` stores entries in `std::unordered_map`;
- clean eviction erases and destroys the entry and its page buffer;
- the next Pager miss allocates a new `ByteBuffer`, reads the page, then
  allocates/inserts a new map node; and
- dirty writeback selection starts at the oldest dirty entry without
  representing SQLite's `PGHDR_NEED_SYNC` or `PCache.pSynced` state.

Pinned SQLite 3.54.0 uses two cooperating layers:

- `src/pcache.c:23-43,203-255,403-528,551-658` owns dirty ordering,
  `PGHDR_NEED_SYNC`, the approximate `pSynced` pointer, pin/release, and
  stress-candidate selection; and
- `src/pcache1.c:117-210,537-641,876-1153` owns a cache-local power-of-two
  chained hash, an intrusive LRU of unpinned pages, compatible page-frame
  recycling, and allocation-free rekeying.

The SQLite fetch path obtains or recycles a frame before Pager reads the page.
A failed read discards the new cache identity; a later miss can reuse the
retained compatible frame. The ordinary steady-state pressure path therefore
does not allocate a page buffer and hash node for every miss.

The ADR-0111 audit records the observed consequence in the committed read
baseline:

- point-pressure cases perform 58,459 cache misses;
- Modern records 313,595 to 379,131 allocations for those runs; and
- matched read workloads remain 1.28x through 1.53x SQLite.

This slice closes the cache-owned mechanism only. ADR-0113 will add SQLite's
in-memory statement-journal prerequisite. A later Pager slice then publishes
and clears `NEED_SYNC`, honors sync-safe candidates, and adds
`PGHDR_DONT_WRITE`. Implementing those Pager transitions before the Journal
prerequisite would invalidate their evidence.

The first cache repair remains single-connection and externally serialized.
It does not add SQLite's global cache group, mutex modes, application-provided
page-cache memory, or public `sqlite3_pcache_methods2` compatibility.

## Decision

### 1. Replace `std::unordered_map` with SQLite's cache-local hash shape

`PageCache` owns a power-of-two array of intrusive bucket heads. Each stable
entry records:

- its active page number;
- one owned page buffer;
- pin and exclusive-pin state;
- hash-chain linkage;
- clean and dirty recency linkage;
- free-slot linkage;
- dirty, `needs_sync`, and list-membership flags; and
- one stable allocation identity.

Lookup masks the page number by `bucket_count - 1` and walks that bucket's
chain. The initial bucket count is 256, matching `pcache1ResizeHash()`.
When active entries reach the bucket count, the cache attempts to double and
rehash the bucket array.

Hash growth is a benign optimization. If bucket allocation fails, the
existing table remains valid and the operation continues with longer chains,
matching SQLite's benign resize behavior. Creating the cache still reports a
typed out-of-memory error if the required initial table cannot be allocated.

Entry addresses never change. Pins, clean/dirty links, fill capabilities, and
Pager borrows retain direct entry pointers.

### 2. Separate a Pager fill from published cache identity

Add one move-only private cache capability borrowed only by `Pager`:

```cpp
class PageCache::Fill final {
 public:
  Fill(Fill&&) noexcept;
  Fill& operator=(Fill&&) noexcept;
  ~Fill();

  [[nodiscard]] MutableByteView mutable_bytes() noexcept;
  [[nodiscard]] Result<Pin> Publish(bool exclusive);
};

[[nodiscard]] Result<Fill> PrepareFill(PageNumber page_number);
```

`PrepareFill()` requires a nonzero page number absent from the active hash.
It returns one unlinked entry whose page buffer is exactly the cache page
size. The entry is not visible to `Lookup()` until `Publish()`.

`Pager::AcquirePage()` changes its miss path to:

1. request a fill entry;
2. read directly into that entry's retained page buffer;
3. publish the entry under the requested page number; and
4. return the requested shared or exclusive pin.

If offset calculation or file read fails, `Fill` destruction returns the
entry to the cache's free-slot list. No partially read page becomes visible.
A recycled victim was clean, so losing its old identity after a failed read
does not lose dirty data or transaction state.

The existing complete-buffer `Insert()` and `InsertExclusive()` contracts
remain for direct cache tests and non-Pager callers. They use the same entry,
hash, recency, and capacity machinery, but may replace a recycled buffer with
the caller-owned buffer. The Pager hot path is the allocation-free fill path.

### 3. Recycle compatible clean frames at the soft-capacity boundary

When a fill needs a slot, choose in this order:

1. a retained free slot with a page-sized buffer;
2. the oldest unpinned clean entry when adding another active entry would
   exceed the configured soft capacity; or
3. a newly allocated stable entry and page buffer.

Recycling removes the victim from the clean list and hash, clears its
page-local state, retains its buffer, and gives it to the unpublished fill.

Pinned and dirty pages still allow the active cache to exceed capacity.
When the final pin releases a clean page, capacity enforcement moves oldest
clean victims to the reusable free-slot list only while total allocated slots
remain within the configured capacity. A slot allocated while pins or dirty
pages forced the cache above capacity is destroyed when that overflow
subsides. Memory therefore contracts to the configured capacity instead of
tracking the historical maximum pin/dirty pressure.

Capacity zero retains a clean frame only while pinned, as before, and destroys
the slot on final release. The allocation-free repeated-fill contract uses a
positive fixed capacity; zero capacity intentionally remains a no-retention
mode.

Explicit memory-reclamation operations remain distinct from ordinary
replacement:

- `Discard()` destroys the named unpinned entry and its buffer;
- `DiscardAfter()` validates pins atomically, then destroys every selected
  entry;
- `ReclaimClean()` destroys all unpinned clean entries and retained free
  slots; and
- `Clear()` requires no pins and destroys every active and free entry.

Identity-change invalidation therefore continues to return cache memory rather
than retaining bytes from an obsolete database snapshot.

### 4. Add cache-owned sync eligibility and `pSynced` selection

Each entry carries `needs_sync` independently from dirty and writable state,
matching SQLite's page-header invariant.

Add the narrow cache contracts needed by the later Pager repair:

```cpp
[[nodiscard]] Status Pin::MarkNeedsSync();
void PageCache::ClearSyncFlags() noexcept;

struct PageWritebackRequest {
  PageNumber page_number;
  bool journal_sync_required = false;
};
```

`MarkNeedsSync()` accepts any valid shared or exclusive pin from the owning
cache. It mutates cache metadata, not page bytes, and the existing external
serialization contract prevents a race with readers. This permits Pager to
mark every cached sector peer even when another read pin already exists. It
may mark a clean or dirty entry and is idempotent. `MarkClean()` clears
`needs_sync`. `ClearSyncFlags()` clears the flag from every dirty entry after
the Journal is durable.

The dirty list retains current oldest-to-newest ordering. `PageCache` also
maintains an approximate sync-safe pointer equivalent to `PCache.pSynced`.
Writeback selection:

1. searches from that pointer for the oldest unpinned dirty entry with
   `needs_sync == false`;
2. updates the approximate pointer during that search;
3. falls back to the oldest unpinned dirty entry when no sync-safe entry
   exists; and
4. reports whether the selected entry requires Journal synchronization.

Dirty insertion/removal, clean transitions, discard, permutation, and sync-flag
clearing update the pointer without allocation.

`needs_sync` is owned by the physical page number, not by the bytes moving
through a cache entry. Replace the one-entry rekey primitive with one bounded
atomic permutation operation used by `Pager::PermutePageNumbers()`:

```cpp
struct PageCacheRekey {
  Pin* pin;
  PageNumber final_page;
};

[[nodiscard]] Status PageCache::PermuteExclusiveForPager(
    std::span<const PageCacheRekey> pages);
```

The operation accepts the existing maximum-five distinct exclusive dirty
pins, validates that current and final page-number sets are the same, removes
all participant hash links, snapshots `needs_sync` by current physical page
number in fixed storage, publishes every final identity, restores each
destination page number's prior sync flag, and repairs the sync-safe pointer.
It performs no allocation and requires no locking-page sentinel.

Thus moving bytes from page 1 to page 2 retains page 2's old sync requirement
on physical page 2. The later Pager repair remains responsible for recreating
a source-page requirement when SQLite's move algorithm requires the original
physical page to remain protected.

This slice does not change Pager's current unconditional pre-spill
`SyncJournal()` call. It publishes and verifies the lower cache state needed
by the later Pager slice after ADR-0113. Until then, every production spill
remains conservatively synchronized.

The later Pager repair must also replace the Journal transaction's current
global `main_journal_dirty_` authorization gate. SQLite may spill a clear-
`NEED_SYNC` page from a durable cohort while a newer cohort remains
unsynchronized. The future seam therefore records per-page or per-cohort
durability, or spills before admitting the newer capture. Its crash matrix
must cover one durable cohort, one unsynchronized cohort, and a sync-safe
spill from the durable cohort.

### 5. Preserve pin, dirty, permutation, and failure invariants

The existing invariants remain:

- multiple read pins or one exclusive pin;
- sole-read-pin promotion preserves the frame and buffer address;
- bounded permutation preserves each entry, pin, recency, dirty state, and
  byte view while rotating page-number-owned sync state;
- clean lookup, pin release, state transitions, and writeback selection
  allocate nothing;
- dirty pages are never recycled or automatically discarded;
- no operation publishes a partially initialized entry;
- allocation failure never removes a dirty or pinned entry; and
- all expected failures use typed results.

Permutation removes and reinserts a bounded set of hash-chain nodes without
allocation. It no longer depends on standard-library node extraction, a
locking-page sentinel, or a non-portable assumption about rehash behavior.

Entry allocation occurs before any active-cache mutation. If either the entry
or page buffer cannot be allocated, the operation returns out of memory and
the previous cache remains unchanged.

### 6. Freeze source-derived tests before implementation

Add focused red tests for:

- colliding page numbers in the initial and grown power-of-two hash;
- benign hash-growth allocation failure with successful lookup/insertion;
- stable frame and byte addresses across hits, pin promotion, and permutation;
- oldest-clean frame recycling with the same page-buffer address;
- failed Pager read after victim selection publishing no new page;
- zero-capacity repeated misses reusing one slot;
- pinned/dirty soft-capacity overflow without unsafe recycling;
- `ReclaimClean`, `Discard`, `DiscardAfter`, and `Clear` actually releasing
  retained slots;
- `needs_sync` independent of dirty state;
- sync-safe dirty preference, pinned candidates, fallback to an unsynced
  dirty page, and `ClearSyncFlags()` pointer repair;
- sector-peer sync marking through an existing shared pin;
- mixed-flag two- and three-page permutations preserving sync state by
  physical page number;
- contraction back to the configured retained-slot bound after large
  pinned/dirty overflow, including zero-capacity no-retention behavior;
- OOM at initial buckets, first entry, first page buffer, and nonfatal hash
  growth; and
- unchanged cache hit/miss instrumentation.

Add one standalone allocation contract:

1. warm a cache to its fixed capacity through Pager reads;
2. cycle through more database pages than capacity;
3. verify zero C++ allocations in the repeated fill/recycle interval; and
4. verify exact rows/bytes and stable active-page capacity.

The old implementation is expected to fail this contract because every
pressure miss allocates a page buffer and map node.

### 7. Qualify the complete cache seam

After focused green and the complete fast unit tier, run:

- PageCache and Pager OOM/allocation suites;
- read Pager, B-tree cursor, storage diagnostic, and cache-pressure tests;
- isolated Debug, Release, ASan/UBSan, and genuine GCC Debug/Release;
- the immutable read fit/pressure native contracts;
- database fuzz and malformed-page coverage;
- full production clang-tidy and formatting; and
- exact-tree review and hosted checks.

Regenerate no benchmark baseline in this slice. Compare a fresh local matched
run against the committed immutable baseline and retain it as evidence. The
pressure cases must preserve correctness, page-read/cache-miss counts, and
deadlines while materially reducing Modern allocation counts. Timing is
reported but does not replace the exact allocation contract.

## Consequences

### Positive

- steady-state Pager misses reuse SQLite-style cache frames and page buffers;
- hash lookup, pin release, permutation, and clean replacement no longer depend on
  `std::unordered_map`;
- the later Pager repair receives the exact `needs_sync` and sync-safe
  candidate seam it requires;
- existing RAII pin and typed failure contracts remain; and
- pressure benchmarks gain a direct allocation explanation instead of a
  timing-only claim.

### Negative

- the cache owns manual intrusive hash, free, clean, and dirty links;
- a fill capability adds one internal state to the cache lifecycle;
- explicit shrink/invalidation must distinguish active entries from retained
  reusable slots; and
- the later Journal and Pager repairs remain required before write-pressure
  parity is complete.

### Deferred

- `PGHDR_DONT_WRITE`, Pager publication/clearing of `NEED_SYNC`, source-page
  sync recreation during page moves, and per-page/cohort Journal write
  authorization;
- in-memory statement journals and subjournal spill policy;
- global cache groups, shared-cache mutexes, cross-connection recycling, and
  application-provided page-cache memory;
- memory-pressure hooks outside the configured soft page capacity; and
- WAL-specific page-cache state.

## Implementation evidence

The implementation replaces `std::unordered_map` with one cache-local
power-of-two intrusive hash and stable heap-owned entries. Pager misses now
read directly into an unpublished fill capability. At capacity, the cache
rekeys and reuses the oldest clean frame and its existing page buffer; failed
reads return that slot without publishing partial bytes. Explicit discard,
snapshot invalidation, clear, and overflow contraction still release retained
memory.

The cache publishes `needs_sync`, sync-safe dirty selection, and bounded
page-number permutation. Sync state follows physical page numbers across two-
and three-page cycles, shared pins may mark sector peers, and the approximate
sync candidate is repaired after pin release, dirty recency changes, sync
clearing, removal, and permutation. Pager continues to synchronize
conservatively until the ordered Journal and Pager repairs.

The focused allocation regression failed before production changes with 65
allocations across 32 pressure reads after a two-page warmup. It now passes
with exactly zero allocations. Focused coverage also includes:

- hash collisions and benign growth failure;
- stable frame addresses across promotion and permutation;
- failed fills and zero-capacity no-retention;
- pinned/dirty overflow contraction;
- sync-safe preference, unsynced fallback, and shared-peer marking;
- page-number-owned sync flags through a three-page cycle; and
- typed OOM at cache creation, entry allocation, page-buffer allocation, and
  benign hash growth.

Fast-local passes 1,394 batched and 91 isolated tests. Isolated Debug,
Release, ASan/UBSan, and genuine GCC 16 Debug/Release each pass 1,524 tests;
instrumentation passes 1,541. Fifty-six native compatibility/performance
contracts pass, and 28 shared checks reconcile 168 configured instances.
Four fresh 20,000-run fuzz domains complete with seed 102. Full production
clang-tidy, pinned formatting, graph, engineering discipline, and whitespace
checks pass.

A fresh matched read baseline at implementation commit
`f128e5915c95b4bcdfe637ce9edfcf16ec54fa99` preserves every result,
cache-miss/page-read count, workload, fixture, guard, and deadline:

| Workload | Prior allocations | New allocations | Allocation delta | Prior wall ratio | New wall ratio |
|---|---:|---:|---:|---:|---:|
| point present, pressure | 379,131 | 262,228 | -116,903 | 1.321990 | 1.264210 |
| point missing, pressure | 313,595 | 196,692 | -116,903 | 1.321887 | 1.250459 |
| scan, pressure | 74,362 | 65,612 | -8,750 | 1.491876 | 1.479414 |

The fit workloads add 12-15 one-time setup allocations from the explicit
cache hash/owner arrays; their wall ratios change by -0.94%, -0.38%, and
+1.15%, within the existing run variation. The pressure allocation deltas
equal two removed allocations per miss plus the fixed setup difference.

GPT-5.6 Sol design review found and closed physical-page sync transfer,
shared-pin sector marking, future Journal authorization, and retained-overflow
requirements. Final code review found one stale sync-candidate case on newest
dirty-page release; its focused regression failed before the correction and
the complete frozen matrix passes afterward.

## References

- ADR-0008: SQLite Performance Parity Strategy
- ADR-0021: Page Cache
- ADR-0022: Read-Only Pager
- ADR-0042: Rollback-Mode Writable Pager
- ADR-0044: Reference-Faithful SQLite B-Tree Mutation
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0102: Change-Aware Verification
- ADR-0111: SQLite Mechanism Baseline and Implemented-Engine Audit
- SQLite 3.54.0 `src/pcache.c`
- SQLite 3.54.0 `src/pcache1.c`
