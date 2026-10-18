# ADR-0114: SQLite Pager Sync Eligibility and DONT_WRITE

- Status: Accepted
- Date: 2026-10-18
- Module graph node: `implement-advanced-sql`
- Audit entry: `storage-pager`

## Context

ADR-0042 established the rollback-mode writable Pager, ADR-0044 added
SQLite-shaped B-tree mutation and transaction-scoped `pHasContent`, ADR-0112
added cache-owned `needs_sync` and `pSynced`-style selection, and ADR-0113
added the in-memory statement-journal prerequisite.

The remaining Pager writeback mechanism is still conservative:

- every pressure spill calls `JournalTransaction::SyncJournal()`;
- `JournalTransaction::AuthorizeDatabaseWrite()` rejects every database write
  while any later journal record is unsynchronized;
- Pager never publishes `needs_sync` when a rollback image is appended;
- successful journal sync never clears the cache flags;
- freed B-tree leaf pages remain ordinary dirty pages and are written back
  even though their bytes are unreachable; and
- the bounded page-number permutation preserves cache flags, but Pager has no
  end-to-end test proving SQLite's physical-page sync invariant across
  capture, spill, savepoint rollback, and reuse.

The committed version-2 matched write diagnostics expose the consequence:

- `update-scan-implicit` performs 4,371 database writes and 7,722 journal sync
  calls and is 6.969554x SQLite;
- `delete-scan-implicit` performs 4,380 database writes and 7,738 journal sync
  calls and is 8.519555x SQLite; and
- SQLite performs only 73 cache writes for the matched delete scan.

Pinned SQLite 3.54.0 uses one cooperating mechanism:

- `pager.c:6043-6094` sets `PGHDR_NEED_SYNC` when a transaction-original page
  is appended to the rollback journal;
- `pager.c:6110-6294` captures every page in a large physical sector and
  propagates `PGHDR_NEED_SYNC` across its cached peers;
- `pager.c:4336-4450` synchronizes the active journal cohort and clears every
  cache sync flag;
- `pcache.c:25-43,445-470` maintains `pSynced` so pressure prefers an
  unreferenced dirty page whose rollback image is already durable;
- `pager.c:4663-4728` synchronizes only when the selected page still requires
  it, then writes the page;
- `pager.c:2420-2495` permits savepoint playback to touch the database only
  when the transaction-original image is known durable;
- `pager.c:4483-4558,6330-6357` keeps `PGHDR_DONT_WRITE` pages dirty for
  rollback bookkeeping but skips their database write; and
- `pager.c:7234-7360` preserves sync requirements by physical page number
  across page moves and recreates the source requirement when an arbitrary
  move removes that cache identity.

Modern already captures an entire physical sector before returning a writable
target, and its B-tree move operation is a bounded permutation whose source
and target page-number sets are identical. This ADR completes the missing
state publication and writeback decisions without adding another cache,
journal, or write executor.

This slice does not change:

- rollback-journal bytes, publication, recovery, or statement-journal
  storage;
- transaction/savepoint admission;
- B-tree balancing or freelist format;
- WAL behavior;
- synchronous-mode configuration;
- mmap or shared-cache behavior; or
- SQL `PRAGMA` support.

## Decision

### 1. Complete the cache-owned page flags

Extend each `PageCache` entry with `dont_write`, independent from
`needs_sync` and dirty state.

Add:

```cpp
[[nodiscard]] Status PageCache::Pin::MarkDontWrite();
[[nodiscard]] bool PageCache::Pin::dont_write() const noexcept;
```

`MarkDontWrite()` requires an exclusive dirty pin. It sets `dont_write` but
does not clear `needs_sync`. `MarkDirty()` always clears `dont_write`, even
when the page was already dirty, matching `sqlite3PcacheMakeDirty()`.
`MarkClean()`, discard, fill reuse, and cache destruction clear both flags.
Mutable access while `dont_write` is set returns typed misuse; allocation
failure while constructing that error is translated to typed out of memory.

`dont_write` follows the cached bytes during a page-number permutation.
`needs_sync` remains owned by the physical page number as specified by
ADR-0112.

Add one Pager-only dirty transition that may operate through a shared pin.
It is used only for unchanged physical-sector peers whose transaction-original
bytes have just been journaled. This reproduces `pagerWriteLargeSector()`,
which makes newly protected peers dirty so their `NEED_SYNC` state cannot be
lost through clean-frame recycling. It does not grant mutable byte access.

### 2. Distinguish main-journal capture from savepoint-only capture

Add:

```cpp
[[nodiscard]] bool JournalTransaction::NeedsMainJournalCapture(
    PageNumber page_number) const noexcept;
[[nodiscard]] bool JournalTransaction::journal_ever_synced() const noexcept;
```

`NeedsMainJournalCapture()` is true only when the page belongs to the
transaction-start image and has not entered the main rollback journal.
It is independent from savepoint subjournal membership.

`journal_ever_synced()` becomes true after the first successful
`DoSync()` and remains true for the transaction. Later main-journal appends
make the active cohort dirty without erasing the fact that the initial header
and every prior published cohort are durable.

Every direct sync path, including savepoint and full rollback, publishes the
same successful-sync bookkeeping.

The existing `main_journal_dirty()` continues to describe only whether the
current tail requires publication.

### 3. Publish `needs_sync` with the rollback image

`Pager::CaptureSector()` computes the physical sector before any append.

For each transaction-original page that needs a main-journal capture:

1. acquire the target or peer page;
2. mark an unchanged peer dirty through the Pager-only transition;
3. mark the page `needs_sync`;
4. append its complete image; and
5. retain the existing savepoint capture behavior.

Before and after the required appends, Pager scans the cached physical-sector
peers. The sector requires synchronization when any peer already has
`needs_sync`, any member needs a new main-journal capture, or the target is a
new page before the first successful journal sync. After capture, every
currently cached peer is marked `needs_sync` when that combined condition is
true.

These inspections and flag updates use Pager-only, non-pinning cache
operations. They do not change recency and cannot conflict with another live
exclusive pin for a distinct page in the sector. A cache miss still uses the
ordinary read pin long enough to capture the missing image.

This rescan is mandatory even when every page already has main-journal
membership. A clean older peer may lose its local flag through cache
recycling, but an unsynchronized dirty peer from the newer cohort remains in
the cache. Refetching and writing the older peer observes the newer peer's
flag and restores the sector-wide requirement before authorization.

A page beyond the transaction-start image has no page record. It is marked
`needs_sync` only before the first successful journal sync, matching SQLite's
`PAGER_WRITER_CACHEMOD` rule. Once the original database size is durably
published, later appended pages do not require another sync merely because a
newer unrelated cohort is dirty.

Publishing the flag never allocates and cannot fail. If journal append or
membership publication fails, the transaction enters the existing failed
state and no database spill is admitted.

### 4. Carry per-page durability into Journal authorization

Add the explicit internal contract:

```cpp
enum class RollbackImageDurability : std::uint8_t {
  Unsynchronized,
  Durable,
};

[[nodiscard]] Status JournalTransaction::AuthorizeDatabaseWrite(
    PageNumber page_number,
    RollbackImageDurability durability);
```

The cache flag is the canonical per-page durability state. Pager passes
`Durable` only after observing `needs_sync == false`.

Journal authorization still verifies:

- an active transaction;
- a valid page number; and
- main-journal membership for every transaction-original page in the same
  physical sector.

An `Unsynchronized` request is always rejected. A `Durable` request is
allowed even when the current journal tail is dirty because the selected page
and its physical sector belong to an earlier published cohort. This removes
the current incorrect transaction-global gate without adding a second page
map.

Resize authorization remains transaction-global because physical truncation
occurs only after the complete required journal sync.

### 5. Synchronize only the selected pressure cohort

Add one Pager helper that:

1. obtains the EXCLUSIVE database lock;
2. calls `JournalTransaction::SyncJournal()`;
3. clears sync flags from dirty cache entries only after success, matching
   `sqlite3PcacheClearSyncFlags()`; and
4. leaves every flag intact on failure.

Pressure writeback uses the existing `PageWritebackRequest`:

1. select the `pSynced`-preferred unpinned dirty page;
2. look it up exclusively;
3. synchronize only when `journal_sync_required` or the live pin still has
   `needs_sync`;
4. require the resulting pin to be sync-safe;
5. skip the database write when `dont_write` is set;
6. otherwise authorize the durable page and issue the positioned write; and
7. mark the page clean.

If an older sync-safe page is selected while a newer cohort is dirty, Pager
writes the older page without synchronizing the newer cohort. When no
sync-safe candidate exists, the fallback page causes one cohort sync, which
clears all current flags and repairs the approximate candidate.

Commit uses the same sync helper before flushing. Savepoint rollback calls it
before opening playback so subjournal bytes can never reach the database
while the transaction-original image remains unsynchronized. Full rollback
retains its existing main-journal playback path and clears/replaces the cache.

After dirty-page flush and before database sync, commit compares the physical
file size with the logical image size. If a skipped `dont_write` page is the
new final page, Pager authorizes an empty growth resize and extends the file
before sync. This reproduces `pager.c:6720-6730`; commit never leaves a header
that names pages beyond end-of-file.

### 6. Preserve physical sync state through Modern page permutation

Modern's page move is not SQLite's arbitrary one-way
`sqlite3PagerMovepage()`. It is one bounded atomic permutation whose current
and final physical page-number sets are identical.

ADR-0112 already snapshots and restores `needs_sync` by physical page number.
ADR-0114 keeps that rule and adds end-to-end Pager tests:

- a sync-required physical source remains sync-required after two- and
  three-page permutations;
- a destination retains its pre-move sync requirement;
- a later journal sync clears every moved physical requirement; and
- `dont_write` follows the bytes and is cleared by the next writable
  transition.

Because no physical source identity disappears, Modern does not need
SQLite's temporary source-page recreation branch. Adding arbitrary one-way
move semantics remains out of scope.

### 7. Add SQLite's freed-page `DONT_WRITE` seam

Add:

```cpp
[[nodiscard]] Status Pager::MarkPageDontWrite(WritePagePin& pin);
[[nodiscard]] Status Pager::MarkPageDontWrite(PageNumber page_number);
[[nodiscard]] Status Pager::WritePage(WritePagePin& pin);
```

Both marking operations require a regular rollback-mode write transaction and
are no-ops when any savepoint is active, matching SQLite's
statement-rollback restriction. The pin overload requires a dirty exclusive
pin owned by that Pager. The page-number overload performs a non-pinning cache
lookup and marks the page only when it is currently cached and dirty; clean or
uncached freed pages already require no write.

Setting `dont_write` makes mutable access through that pin return typed misuse
and makes page permutation reject the pin. This is Modern's equivalent of
SQLite clearing `PGHDR_WRITEABLE`. A later mutation must re-enter Pager through
`WritePage(WritePagePin&)`; that operation repeats the ordinary write
authorization, clears `dont_write` through the dirty transition, and restores
mutable access without changing the pin's frame or ownership.

`MutationPageOwner` exposes a narrow forwarding operation. In
`FreeBtreePage()`, when the freed page is appended as a freelist leaf rather
than rewritten as a new trunk:

1. update and journal the existing trunk;
2. set the transaction-scoped `pHasContent` bit;
3. mark the cached dirty freed page `dont_write` through a non-pinning Pager
   lookup; and
4. preserve existing cleanup and rollback-required behavior.

Clean or uncached pages need no flag. A page rewritten as a freelist trunk
must still be written and is never marked. If another live write pin owns the
cached page, the entry flag immediately revokes its mutable access.
`MutationPageOwner::Promote()` re-enters Pager when an existing write pin is
`dont_write`, so later reuse cannot bypass the writable transition.

Writeback of a `dont_write` page still performs a required journal sync before
cleaning it. It does not authorize or issue a database write and does not
increment the page-write instrumentation counter.

### 8. Freeze source-derived correctness and fixed-work tests

Add red tests for:

- main capture setting `needs_sync` and successful sync clearing it;
- sync failure retaining every flag;
- a durable older page being authorized while a newer cohort remains dirty;
- an unsynchronized selected page being rejected;
- `pSynced` pressure spilling multiple older pages without another journal
  sync;
- fallback to a sync-required page causing exactly one cohort publication;
- large-sector capture marking every peer and preserving the sector
  authorization invariant;
- savepoint rollback synchronizing before any database restore;
- physical-page sync flags across two- and three-page permutations;
- `dont_write` surviving sync, suppressing one database write, and clearing
  on reuse;
- mutable access and permutation failing while the same pin is
  `dont_write`;
- savepoints disabling `dont_write`;
- freelist-leaf insertion marking a cached dirty page while new-trunk
  insertion does not;
- commit, full rollback, savepoint rollback, OOM, I/O failure, and crash-cut
  behavior with one durable and one unsynchronized cohort; and
- an EOF freelist leaf skipped by `DONT_WRITE` still producing an exact-size
  database that reopens successfully; and
- no new allocation in selection, flag publication, sync clearing, skipped
  writeback, or permutation.

Extend the version-2 diagnostic contract with bounded fixed work:

- matched update scan performs at most 32 main-journal sync calls; and
- matched delete scan performs at most 128 database-page writes and at most
  32 main-journal sync calls.

These limits are intentionally above the source-derived cache/cohort work and
far below the current 7,722/7,738 sync calls and 4,380 delete writes.

Timing remains evidence, not the correctness oracle.

### 9. Qualify the complete durability seam

After focused green and the complete fast unit tier, run:

- PageCache, Journal, Pager, transaction, B-tree writer, and writable-session
  OOM/allocation suites;
- savepoint, pressure-spill, physical-sector, page-move, freelist, and crash
  matrices;
- Debug, Release, ASan/UBSan, instrumentation, and genuine GCC 16
  Debug/Release;
- native write compatibility and the complete version-2 diagnostic matrix;
- read/write SQL and database fuzz;
- full production clang-tidy, pinned formatting, graph, and engineering
  discipline checks; and
- exact-tree review and hosted checks.

Run a fresh complete version-2 write baseline against the committed immutable
baseline. Preserve every logical result, database metadata, integrity result,
durability counter family, deadline, and matched 10x guard. `DONT_WRITE`
intentionally permits unreachable freelist-leaf bytes to differ physically;
record any resulting database fingerprint change and verify SQLite
interoperability rather than requiring byte identity. Record fixed-work and
timing deltas as implementation evidence without replacing the immutable
baseline.

## Consequences

### Positive

- pressure spills use SQLite's page-local durability state instead of a
  transaction-global approximation;
- `pSynced` can write durable older pages while a newer cohort remains dirty;
- sector and page-move safety become explicit cache invariants;
- freed leaf bytes no longer generate pointless database writes when no
  savepoint is active;
- public SQL delete scans gain the same cached-page optimization without a
  second B-tree ownership container;
- savepoint playback retains the main-journal durability guard; and
- the next transaction-admission repair starts from SQLite-faithful Pager
  storage work.

### Negative

- PageCache gains one additional flag and one Pager-only dirty transition;
- `WritePagePin::mutable_bytes()` becomes a typed-result contract so
  `dont_write` can revoke mutable access;
- Journal authorization accepts an explicit durability fact supplied by its
  sole Pager owner;
- physical-sector capture may dirty unchanged peer pages; and
- B-tree free-page ownership gains one finalization call before pin release.

### Deferred

- conditional `usesStmtJournal` admission;
- write-combining or batched database-page I/O;
- alternate rollback-journal modes and main-journal memory mode;
- arbitrary one-way page moves;
- secure-delete behavior;
- WAL writeback and checkpoint sync state; and
- shared-cache and multi-connection synchronization.

## References

- ADR-0040: Journal Contracts and Durable Ordering
- ADR-0041: SQLite-Compatible DELETE-Mode Rollback Journal
- ADR-0042: Rollback-Mode Writable Pager
- ADR-0044: Reference-Faithful B-Tree Mutation
- ADR-0111: SQLite Mechanism Baseline and Implemented-Engine Audit
- ADR-0112: SQLite Page-Cache Hash, Recycling, and Sync Eligibility
- ADR-0113: SQLite In-Memory Statement Journal and Spill
- SQLite 3.54.0 `src/pager.c`
- SQLite 3.54.0 `src/pcache.c`
- SQLite 3.54.0 `src/btree.c`
