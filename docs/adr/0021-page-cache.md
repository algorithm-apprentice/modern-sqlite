# ADR-0021: Page Cache

- Status: Accepted
- Date: 2026-10-03

## Context

The read pager needs a bounded in-memory map from database page numbers to
fixed-size page bytes. Later write support also needs explicit dirty state and
a way to identify pages that should be written back under memory pressure.
The cache must not know file I/O, rollback-journal, WAL, B-tree, or SQL policy.

SQLite splits this responsibility between `pcache.c` and `pcache1.c`:

- `src/pcache.h:16-130` defines page headers, reference-counted pinning,
  dirty state, and the stress callback boundary.
- `src/pcache.c:403-510` performs normal fetches and selects an unreferenced
  dirty page when a hard fetch needs pager assistance.
- `src/pcache.c:551-628` releases pins and maintains clean/dirty transitions.
- `src/pcache1.c:117-210` stores page-number hash entries and an intrusive LRU
  of unpinned pages.
- `src/pcache1.c:623-641` reclaims least-recently-used unpinned pages when a
  cache exceeds its configured target.
- `src/pcache1.c:876-1050` permits pinned pages to exceed the soft target,
  reuses unpinned pages, and keeps cache hits on a short path.
- `src/pcache1.c:1084-1119` either frees an unpinned page immediately under
  pressure or adds it to the reusable LRU.

SQLite's pager supplies the `pagerStress()` callback that may synchronize a
journal and write a dirty page. Modern SQLite cannot place that callback in the
cache because the cache is below journal and pager in the dependency graph.
It needs a data-only request that a future pager may accept, defer, or reject.

The sibling Modern LevelDB cache work demonstrates three relevant rules:

- Pin ownership should be move-only RAII, not shared ownership.
- Hot lookup and release paths should not allocate.
- Intrusive recency state avoids list-node allocation and keeps entry
  addresses stable while pinned.

The LevelDB cache's sharding, binary keys, value deleters, and concurrent
operations do not apply to one connection-local database page cache.

## Decision

Add `PageCache`, `PageFrame`, and `PageNumber` under
`modern_sqlite/storage/cache`.

```cpp
class PageNumber;
class PageFrame {
 public:
  PageNumber page_number() const noexcept;
  ByteView bytes() const noexcept;
  bool dirty() const noexcept;
};

struct PageCacheOptions {
  ByteCount page_size;
  std::size_t capacity_pages;
};

struct PageWritebackRequest {
  PageNumber page_number;
};

struct PageCachePressure {
  std::size_t excess_pages;
  std::optional<PageWritebackRequest> writeback;
};

class PageCache final {
 public:
  class Pin {
   public:
    const PageFrame& frame() const noexcept;
    const PageFrame* operator->() const noexcept;
    const PageFrame& operator*() const noexcept;
    Result<MutableByteView> mutable_bytes();
    void MarkDirty() noexcept;
    void MarkClean() noexcept;
  };

  static Result<std::unique_ptr<PageCache>> Create(PageCacheOptions options);

  Result<Pin> Insert(PageNumber page_number, ByteBuffer bytes);
  Result<std::optional<Pin>> Lookup(PageNumber page_number);
  Status Discard(PageNumber page_number);
  std::size_t ReclaimClean() noexcept;

  PageCachePressure pressure() const noexcept;
  ByteCount page_size() const noexcept;
  std::size_t capacity_pages() const noexcept;
  std::size_t page_count() const noexcept;
  std::size_t dirty_page_count() const noexcept;
  std::size_t pin_count() const noexcept;
};
```

The cache is non-copyable and non-movable. A `Pin` is move-only and borrows
its owning cache. The cache must outlive every pin, and external code must
serialize all cache and pin operations. This matches SQLite's common
connection-local cache mode without adding a mutex to every hit.

### Page numbers and frames

- `PageNumber` is a strong `std::uint32_t` value. Cache operations reject page
  zero; file-format maximum-page validation belongs to the pager.
- Every cache has one immutable nonzero page size.
- `Insert` accepts one `ByteBuffer` of exactly that size and moves it into the
  frame without copying page bytes.
- Duplicate insertion is misuse. Callers look up before loading or inserting.
- `PageFrame` exposes its page number, immutable bytes, and dirty state.
- Mutable bytes are available only through a pin after that pin has marked
  the frame dirty. Initial disk reads happen before insertion into a
  `ByteBuffer`, so cache misses never require an untracked mutable clean view.
- Frame references and byte views borrow their pin and must not outlive it.
  A mutable view must not be used after the pin marks the frame clean.

### Pin ownership

- Each successful insert or lookup returns one `Pin`.
- Additional lookups of a pinned page create additional pins.
- Releasing the last pin makes a clean page an eviction candidate or moves a
  dirty page to the newest end of the dirty recency list.
- Moving a pin transfers exactly one reference. Pin destruction performs no
  allocation.
- Cache destruction with outstanding pins violates the documented ownership
  precondition and is asserted in debug builds.

No `shared_ptr` participates in page ownership. Stable node-based hash entries
allow pins and intrusive lists to retain direct entry pointers across hash
rehashing.

### Clean-page replacement

The configured capacity is a soft page-count target:

- Unpinned clean pages form one intrusive least-recently-used list.
- A cache hit removes an unpinned clean page from that list while pinned.
- Releasing its final pin appends it as most recently used.
- Insert and final clean release evict oldest clean pages until the cache is
  at capacity or no clean page is reclaimable.
- Pinned or dirty pages may temporarily exceed capacity. This preserves
  forward progress when an operation needs multiple simultaneous page pins,
  matching SQLite's soft-limit behavior.
- Capacity zero still returns usable inserted pins but retains a clean page
  only while pinned.
- `ReclaimClean()` discards every unpinned clean page regardless of capacity
  and returns the number reclaimed.

The initial implementation uses `std::unordered_map<std::uint32_t, Entry>`
with an identity key and intrusive recency links. Lookup, pin release, dirty
transitions, and pressure inspection allocate nothing. A custom hash table or
bulk page allocator requires measured integrated-pager evidence before it can
replace this baseline.

### Dirty state and writeback requests

Dirty state is explicit and cache-local:

- `Pin::MarkDirty()` is idempotent and adds a newly dirty frame to the newest
  end of an intrusive dirty list.
- `Pin::mutable_bytes()` fails with `kMisuse` while the frame is clean.
- `Pin::MarkClean()` is idempotent and removes the frame from the dirty list.
- Dirty pages are never selected by automatic clean-page eviction.
- Dirty recency is refreshed when the last pin is released.

When the cache is over capacity, `pressure()` reports:

- the exact number of pages above the soft target; and
- the oldest unpinned dirty page as an optional `PageWritebackRequest`.

The request is advisory and deterministic. It contains no callback and does
not pin, write, clean, or discard the page. A future pager may look up the
page, perform the required journal/WAL ordering, write it, mark it clean, and
release it. If every excess page is pinned, the request is empty and the cache
remains temporarily over capacity.

The returned pressure value is a snapshot that remains meaningful only while
the caller continues to provide the required external serialization.

Journal-synchronization eligibility such as SQLite's `PGHDR_NEED_SYNC` is not
cache state. The future pager may defer a request that is not currently safe
to write.

### Explicit discard

`Discard(page_number)` is idempotent for an absent page and rejects a pinned
page with `kBusy`. It may remove an unpinned clean or dirty page. Its name is
deliberate: the caller is responsible for proving that dirty contents are
obsolete or otherwise safely handled before discarding them.

Automatic replacement never calls `Discard` and never loses dirty data.

### Error and allocation behavior

- Zero page numbers, zero configured page size, wrong-sized inserted buffers,
  duplicate insertion, and mutable access to a clean frame return `kMisuse`.
- Discarding a pinned frame returns `kBusy`.
- Ordinary misses return an empty optional, not an error.
- Standard allocation failure remains a C++ allocation failure; no cache
  mutation occurs before insertion succeeds.
- Successful lookup, pin movement/release, state transitions, pressure
  inspection, and clean eviction perform no allocation.

## Explicitly deferred behavior

- Page reads, writes, synchronization, and recovery.
- Rollback-journal, WAL, `NEED_SYNC`, writable, and do-not-write flags.
- Dynamic capacity or page-size changes.
- Global cache groups shared by multiple pagers.
- Cross-thread cache operations or internal locking.
- Custom hash tables, bulk page allocation, page-buffer recycling, and
  application-provided page-cache memory.
- Page rekeying, range truncation, dirty-page sorting, and transaction-wide
  dirty iteration until their pager/B-tree callers exist.
- Cache-policy plugins, alternative replacement policies, and public ABI
  compatibility with `sqlite3_pcache_methods2`.

## Validation plan

Tests cover:

- option, page-number, page-size, duplicate, and mutable-view validation;
- move-only pin ownership, multiple pins, and total pin accounting;
- hit, miss, insertion, and page-byte preservation;
- least-recently-used clean eviction and recency refresh;
- pinned pages exceeding capacity and reclamation after release;
- zero-capacity behavior;
- dirty-page exclusion from automatic eviction;
- deterministic oldest-unpinned writeback requests;
- clean/dirty transitions and mutable-byte access;
- explicit discard of clean and dirty pages, pinned discard rejection, and
  idempotent missing discard;
- reclaiming all and only unpinned clean pages;
- zero allocations for successful lookup, pin release, state transitions, and
  pressure inspection in the optimized baseline.

The performance baseline records cache-hit lookup/pin/release, dirty-state
transitions, and clean LRU turnover with allocation counts. Optimization waits
for a repeatable bottleneck.

## Consequences

- The read pager receives a small type-safe cache with no platform or format
  dependency.
- Later write support can track dirty pages and request writeback without
  creating an upward dependency from cache to pager or journal.
- Soft capacity preserves progress while pins or dirty data prevent
  reclamation.
- The first implementation has allocation-free hits and intrusive replacement
  state while remaining small enough to validate before integrated pager
  benchmarks exist.
