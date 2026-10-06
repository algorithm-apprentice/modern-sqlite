# ADR-0042: Rollback-Mode Writable Pager

**Status:** Accepted

## Context

ADR-0022 introduced `ReadPager` as the read-only owner of:

- the main database file;
- SHARED-lock acquisition and release;
- snapshot and database-header validation;
- page-size and logical-page-count discovery;
- immutable page-cache pins;
- cache identity invalidation; and
- rollback-journal and WAL sidecar detection.

ADR-0040 and ADR-0041 now provide the semantic journal coordinator and the
SQLite-compatible DELETE-mode rollback-journal backend needed to extend that
owner into a writable pager.

SQLite 3.54.0 concentrates the relevant rollback-mode behavior in
`src/pager.c`. Its essential writable-pager invariants are:

1. A write transaction starts from a valid read snapshot and acquires a
   RESERVED lock before it creates mutable state.
2. A page is not caller-modifiable until its transaction-original and required
   savepoint images have been captured.
3. When the effective journal sector is larger than a database page, all
   original pages sharing the sector are protected before any member may reach
   the database file.
4. The rollback journal is synchronized before the first database-page write
   and before each later spill epoch containing newly journaled pages.
5. Database writes require an EXCLUSIVE lock.
6. Commit updates page 1's change metadata, synchronizes the journal, writes
   dirty pages, synchronizes the database, and only then makes the journal
   non-hot.
7. Full rollback restores the transaction-start size and page images.
8. Savepoint rollback restores the savepoint size and page images without
   ending the outer write transaction.
9. Cache pressure may spill unpinned dirty pages, but never a page whose
   rollback image is not durably protected.
10. Hot-journal recovery obtains an EXCLUSIVE lock and completes before a read
    snapshot is exposed.
11. An unsafe playback or journal-finalization failure is persistent because
    cache and file state can no longer be trusted.

The existing abstractions deliberately divide these responsibilities:

- `PageCache` owns stable frames, pins, dirty state, clean eviction, dirty
  recency, and soft-capacity pressure.
- `JournalTransaction` owns transaction and savepoint image membership,
  journal synchronization state, physical-sector authorization, rollback
  playback ordering, and failed/error state.
- `RollbackJournal` owns SQLite-compatible journal bytes, publication,
  playback, and DELETE-mode finalization.
- The pager must own database locks, mutable-page authorization, database-file
  I/O, cache/file coherence, commit phases, and recovery targets.

The current `ReadPager` name and private ownership model are no longer an
appropriate long-term seam. Adding a separate writable pager that owns a
second file handle or cache would create incoherent snapshots. A wrapper with
privileged access to `ReadPager` would split one state machine across two
owners and make hot recovery, page pins, and B-tree integration unnecessarily
fragile.

The writable pager is performance-sensitive. The Modern LevelDB precedents
used by this project reinforce the following rules:

- cached lookup, pin release, dirty transitions, writeback selection, and
  commit traversal should not allocate;
- reusable storage should be preferred to per-record or per-page temporary
  allocation;
- durability boundaries must remain explicit and observable;
- the first unsafe I/O failure must not be hidden by success-shaped cleanup;
  and
- crash tests must enumerate filesystem mutation boundaries rather than test
  only a few hand-selected failures.

This node does not implement B-tree mutation, freelist policy, transaction
statement semantics, SQL mutation, multi-database atomic commit,
multi-connection busy retry policy, WAL, alternate rollback-journal modes, or
memory-mapped writable pages.

## Decision

### 1. Replace the read-only-named owner with one unified `Pager`

The public pager surface will move to:

- `include/modern_sqlite/pager/pager.hpp`;
- `src/pager/pager.cpp`; and
- `src/pager/write_pager.cpp`.

`ReadPager` will be renamed to `Pager`. Existing read consumers will be updated
to use `Pager`; there will not be a second cache-owning writable facade or a
compatibility alias.

The existing immutable read API and behavior remain available:

```cpp
struct PagerOptions {
  ByteCount empty_database_page_size{4096};
  std::size_t cache_capacity_pages = 512;
};

class Pager final {
 public:
  static Result<std::unique_ptr<Pager>> Open(
      Vfs& vfs, std::string_view path, PagerOptions options = {});

  Status BeginRead();
  Status EndRead();
  Status CleanupReadState();
  Status ValidatePageNumber(PageNumber page_number) const;
  Result<ReadPagePin> ReadPage(PageNumber page_number);
};
```

`Open()` continues to open the main database read-only, never creates a
missing database, rejects hot rollback journals, and preserves all ADR-0022
read-only behavior.

Writable construction is explicit:

```cpp
struct WritablePagerOptions {
  PagerOptions pager;
  RollbackJournalOptions journal;
};

class Pager final {
 public:
  static Result<std::unique_ptr<Pager>> OpenWritable(
      Vfs& vfs, std::string_view path,
      WritablePagerOptions options = {});
};
```

`OpenWritable()`:

1. validates pager and journal options before VFS use;
2. canonicalizes the path;
3. opens or creates the main database read-write without a read-only fallback;
4. obtains the main-file properties;
5. creates one reusable `RollbackJournal`; and
6. constructs the same cache/snapshot owner used by read-only operation.

The VFS must outlive the pager. The pager must outlive every page pin and
savepoint identifier it returns. Destruction closes owned handles but performs
no hidden synchronization, commit, rollback, recovery, deletion, or lock
cleanup.

### 2. Use one explicit pager state machine

The pager will expose:

```cpp
enum class PagerState : std::uint8_t {
  kOpen,
  kReader,
  kWriterLocked,
  kWriterCacheModified,
  kWriterDatabaseModified,
  kWriterFinished,
  kError,
};
```

The rollback-mode transitions are:

```text
kOpen -> kReader
    BeginRead() acquires SHARED and publishes a validated snapshot.

kReader -> kOpen
    EndRead() releases all database locks.

kReader -> kWriterLocked
    BeginWrite() acquires RESERVED and captures transaction-start metadata.

kWriterLocked -> kWriterCacheModified
    The first successful writable-page or image-size mutation changes cache
    state. The journal transaction may be created earlier by a savepoint.

kWriterCacheModified -> kWriterDatabaseModified
    Spill or commit obtains EXCLUSIVE, synchronizes the journal, authorizes a
    database mutation, and successfully writes or resizes database bytes.

kWriterDatabaseModified -> kWriterFinished
    Commit has synchronized all database content, or rollback has completed
    playback and journal finalization. Only post-finalization file-size and
    lock cleanup may remain.

kWriterFinished -> kReader
    Required physical truncation and lock downgrade complete.

kWriter* -> kReader
    A successful rollback restores the transaction-start state and downgrades
    to SHARED.

any active state -> kError
    Playback, journal finalization, or another failure that leaves cache/file
    coherence unknowable becomes persistent.
```

`Pager::state()` returns this state. `in_read_transaction()` remains true in
`kReader` and all writer states. `in_write_transaction()` is true from
`kWriterLocked` through `kWriterFinished`.

The pager tracks the actual database lock separately. State changes are
published only after their required lock or I/O operation succeeds.

### 3. Keep transaction start cheap and create the journal lazily

`BeginWrite()` requires:

- a writable pager;
- `kReader` state; and
- a successful upgrade from SHARED to RESERVED.

For a nonempty database, it first requires both database-header format bytes to
be rollback mode `1`:

- byte 18, the write version; and
- byte 19, the read version.

ADR-0022 deliberately permits a read-only pager to inspect unknown write
versions. That permissiveness does not extend to mutation. A WAL or unknown
write version is rejected with `ErrorCode::kProtocol` before RESERVED is
requested or any journal pathname is opened.

It records:

- the transaction-start logical page count;
- the transaction-start header and change token;
- the transaction-start page size; and
- the known physical file size.

It does not create the rollback journal. This preserves SQLite's cheap empty
write transaction and ensures a failed journal open cannot leave dirty cache
state.

The pager creates `JournalTransaction` on the first operation that needs
journal state:

- making a page writable;
- allocating a page;
- truncating the logical image; or
- creating a savepoint.

The transaction uses:

```cpp
JournalTransactionInfo{
    .page_size = page_size(),
    .sector_size =
        ResolveRollbackJournalSectorSize(main_file_properties),
    .original_page_count = transaction_start_page_count,
};
```

A savepoint-only transaction may therefore create an empty rollback journal.
If the outer transaction ends without a cache or logical-size mutation, the
pager discards that journal through rollback finalization and performs no
database write.

### 4. Make pages writable only after complete rollback capture

The pager will expose a move-only mutable pin:

```cpp
class WritePagePin final {
 public:
  const PageFrame& frame() const noexcept;
  MutableByteView mutable_bytes() noexcept;
};

class Pager final {
 public:
  Result<WritePagePin> WritePage(PageNumber page_number);
};
```

`WritePage()` requires an active write transaction and an existing logical
page. It performs:

1. cache-pressure maintenance from earlier unpinned dirty pages;
2. exclusive cache acquisition of the requested page;
3. lazy journal-transaction creation;
4. capture of every relevant page in the requested page's effective physical
   sector;
5. dirty transition of the requested frame; and
6. construction of `WritePagePin`.

Sector capture considers every transaction-start or current logical page
sharing the effective journal sector. Before acquiring a sector neighbor, the
pager asks the coordinator whether that page still needs a transaction or
savepoint image:

```cpp
bool JournalTransaction::NeedsCapture(PageNumber page_number) const noexcept;
```

Only needed images are fetched and passed to `CapturePage()`. This avoids
reloading already protected sector neighbors after clean-cache eviction while
still capturing a dirty target or neighbor for a savepoint created after its
earlier mutation.

The target frame is marked dirty only after every required capture succeeds.
`mutable_bytes()` is therefore infallible on a successfully returned
`WritePagePin`.

Writable acquisition is exclusive per page:

- it succeeds only when the target frame has no existing read or write pin;
- while `WritePagePin` lives, `ReadPage()` and `WritePage()` for the same page
  return `ErrorCode::kBusy`;
- a retained `ReadPagePin` prevents writable acquisition of that page; and
- unrelated pages may remain read-pinned.

The cache, not a caller convention, enforces this rule. An immutable
`ReadPagePin` can therefore never observe in-place mutation through an aliased
write pin.

An append, allocation, or journal error returns without granting mutable
access. If the coordinator enters `kFailed`, only rollback may continue the
write transaction.

### 5. Append zero-filled pages through the pager

The pager will expose:

```cpp
Result<WritePagePin> AllocatePage();
```

This is a physical pager primitive, not a freelist policy. It appends the next
logical page number and returns a zero-filled writable frame. If that number is
SQLite's reserved locking page, the pager skips it and appends the following
page, leaving the locking-page hole inaccessible.

Allocation order is:

1. choose and range-check the new page number;
2. allocate and exclusively insert a zero-filled clean frame while keeping it
   pinned;
3. create the journal transaction if necessary;
4. capture pre-existing members of the same physical sector and any
   savepoint-required image of the candidate;
5. mark the candidate dirty;
6. publish the new logical page count; and
7. return mutable access.

If setup fails, the logical page count is unchanged and the new frame is
discarded when possible. Any journal failure still requires transaction
rollback.

The future B-tree writer owns freelist interpretation. It will use
`WritePage()` for a reused freelist page and `AllocatePage()` only for physical
append.

### 6. Make logical shrink the final pre-commit image operation

The pager will expose:

```cpp
Status TruncateImage(std::uint32_t page_count);
```

`TruncateImage()` may only keep or reduce the current logical page count. It
never extends the image, and it rejects a transition from a nonempty image to
zero pages. A committed nonempty SQLite database retains page 1; callers that
want to abandon creation of a new database roll back instead of committing a
zero-page truncation.

SQLite permits `sqlite3PagerTruncateImage()` only immediately before commit:
after logical truncation, the transaction must commit or fully roll back and
must not continue writing. The Modern SQLite pager adopts the same rule.

Before `TruncateImage()`:

- every dirty tail page has already passed through `WritePage()` and therefore
  has its transaction-original image;
- the B-tree layer has made page 1 writable and stored the new nonzero logical
  page count at byte 28; and
- any retained page that will be written after the size decision is already
  dirty and sector-protected.

`TruncateImage()` verifies that cached page 1 is dirty and that byte 28 equals
the requested logical size. It also requires zero outstanding read or write
pins, including pins to pages retained by the smaller image. This prevents an
already issued mutable capability from changing the supposedly final image.
It then discards cached pages above the new size, publishes the logical page
count, and enters a final-image state in which only `Commit()` or full
`Rollback()` is legal. `WritePage()`, `AllocatePage()`, savepoint
creation/release/rollback, and another size change return `ErrorCode::kMisuse`.

The main database file is not physically shrunk while the rollback journal is
needed. Dirty pages above the logical size are excluded from spill and commit.
This matches SQLite's rollback-mode safety property: rollback never has to
reconstruct untouched tail bytes that were prematurely truncated.

On successful commit:

1. synchronized page 1 durably contains the nonzero logical size chosen by the
   B-tree layer;
2. phase one writes and synchronizes the logical database image;
3. journal finalization makes the commit durable; and
4. phase-two cleanup truncates an oversized physical file.

A post-finalization truncate failure is a commit-cleanup failure, not a
rollback opportunity. The synchronized page-1 size makes the logical commit
unambiguous even while old tail bytes remain. `Commit()` remains retryable in
`kWriterFinished` until the truncate and SHARED-lock downgrade succeed.

Full rollback and hot recovery do physically resize the file to the original
page count before restoring page records, as required by ADR-0040.

### 7. Keep cache policy in `PageCache` and durability policy in `Pager`

`PageCache` will gain these policy-neutral operations:

```cpp
Result<std::optional<PageCache::Pin>> LookupExclusive(
    PageNumber page_number);
Result<PageCache::Pin> InsertExclusive(
    PageNumber page_number, ByteBuffer bytes);
std::optional<PageWritebackRequest> writeback_candidate() const noexcept;
Status DiscardAfter(std::uint32_t page_count);
Status Clear();
```

- `LookupExclusive()` and `InsertExclusive()` create the sole pin permitted for
  a writable frame. Ordinary lookup rejects an exclusively pinned frame, and
  exclusive lookup rejects any already pinned frame.
- `writeback_candidate()` returns the oldest unpinned dirty page regardless of
  whether the cache is currently over capacity.
- `DiscardAfter()` atomically rejects a pinned tail, then removes all clean or
  dirty frames above the supplied logical page count.
- `Clear()` rejects any pinned frame, then removes every frame and resets cache
  recency and dirty counters.

These operations do not synchronize journals, write database files, choose
locks, or hide errors.

The existing `pressure()` result remains the trigger for automatic spill.
Before and after page acquisition, the pager repeatedly spills eligible dirty
pages while the cache exceeds its soft capacity. If all excess dirty pages are
pinned, the cache may remain above capacity until a later pager operation.
Pin destruction performs no I/O and cannot hide a spill failure.

### 8. Spill one page through the complete journal-before-database protocol

For each pressure-selected page, the pager:

1. upgrades RESERVED to EXCLUSIVE if necessary;
2. calls `JournalTransaction::SyncJournal()`;
3. calls `AuthorizeDatabaseWrite(page_number)`;
4. writes exactly one full page at its database offset;
5. reports a write failure through `ReportDatabaseFailure()`;
6. marks the frame clean only after the write succeeds; and
7. enters `kWriterDatabaseModified`.

The coordinator's sector authorization proves that every original page sharing
the physical sector was captured before the write. A later mutation after the
spill may append another journal cohort; the next spill or commit synchronizes
that cohort before another database write.

Writeback selection and traversal allocate no memory. The cache's dirty
recency order is the initial spill order. Page-number sorting and larger
contiguous write batches are deferred until the write-performance baseline
demonstrates that their benefit justifies additional bookkeeping.

### 9. Update page 1 once during a modified commit

Before the final journal synchronization, a modified nonempty transaction
makes page 1 writable and updates:

- byte 24: the big-endian change counter, incremented modulo 2^32;
- byte 92: `version_valid_for`, set to the same counter; and
- byte 96: the pinned SQLite version number `3,054,000`.

The original page-1 image is therefore journaled before these bytes change.
The B-tree layer, not the pager, owns the page-count field at byte 28 and all
other database-header semantics.

If `TruncateImage()` has fixed a smaller logical image, page 1 is already dirty
and protected. Commit acquires it exclusively and updates only the change
metadata; it does not perform new sector capture after truncation.

After commit, the pager reparses the committed page-1 header and updates its
in-memory header and change token without incrementing `data_version()`.
`data_version()` continues to represent snapshot identity changes observed
across read transactions, not the pager's own acknowledged commit.

### 10. Commit in two durability phases

`Commit()` requires an active write transaction and no outstanding page pins.

An unmodified transaction:

- finalizes an empty journal if a savepoint created one;
- downgrades RESERVED to SHARED; and
- returns to `kReader` without a database write or sync.

A modified transaction performs phase one:

1. validate that page 1 byte 28 equals the current logical page count when the
   image size changed;
2. update page 1 change metadata;
3. obtain EXCLUSIVE;
4. synchronize the rollback journal;
5. authorize and write every dirty page at or below the logical size;
6. synchronize the main database with normal strength;
7. call `MarkDatabaseSynced()`; and
8. enter `kWriterFinished`.

It then performs phase two:

1. call `JournalTransaction::Commit()` to delete the rollback journal;
2. truncate an oversized physical file to the committed logical size;
3. downgrade the database lock to SHARED;
4. discard completed write/savepoint state; and
5. return to `kReader`.

The journal is never finalized before the database sync succeeds.
`AuthorizeDatabaseResize()` is not called for logical shrink because no
database resize occurs while the transaction is active. Physical tail
truncation is post-commit cleanup after the synchronized header makes the
smaller logical image durable.

If journal synchronization, database write, or database sync fails, the
coordinator records the failure and the caller must roll back. If journal
finalization fails, the pager enters `kError`. If post-finalization truncation
or lock downgrade fails, the pager remains `kWriterFinished`; repeating
`Commit()` retries only the incomplete cleanup and never rewrites or
re-finalizes the transaction.

### 11. Roll back cache-only and database-modifying transactions differently

`Rollback()` requires an active write transaction and no outstanding page
pins.

If no journal transaction exists, rollback only restores transaction-start
metadata and downgrades RESERVED to SHARED.

Otherwise, the pager supplies a `JournalRecoveryTarget` to
`JournalTransaction::Rollback()`.

For a transaction that never modified database bytes:

- `PreparePlayback()` clears the cache;
- `ResizeDatabase()` restores the transaction-start logical size;
- `RestorePage()` performs no database write because the file still contains
  every original image;
- `SyncDatabase()` is a no-op; and
- completion restores the transaction-start header and change token.

For a transaction that may have modified database bytes:

- the existing EXCLUSIVE lock is retained;
- `ResizeDatabase()` physically restores the transaction-start file size;
- `RestorePage()` writes each yielded original image directly to the main
  database;
- `SyncDatabase()` synchronizes the restored database; and
- completion clears stale cache state and restores transaction-start metadata.

After journal rollback finalization, the pager downgrades to SHARED and returns
to `kReader`.

Playback, database-restore, database-sync, or rollback-finalization failure
enters `kError`. The pager does not claim a readable snapshot after a partial
rollback.

### 12. Preserve the outer transaction across savepoint operations

The pager will expose:

```cpp
Result<JournalSavepointId> CreateSavepoint();
Status ReleaseSavepoint(JournalSavepointId savepoint);
Status RollbackToSavepoint(JournalSavepointId savepoint);
```

Creating a savepoint lazily creates the journal transaction and records the
current logical page count through `JournalTransaction::CreateSavepoint()`.

Release delegates to the coordinator and leaves pager state, dirty pages, and
the outer write transaction intact.

Savepoint rollback requires no outstanding pins and uses a savepoint recovery
target:

- `ResizeDatabase()` restores the savepoint logical size and discards cached
  tail pages but does not physically shrink the database file.
- `RestorePage()` replaces a cached page image when present.
- Before the first database spill, restored cached pages remain dirty and no
  database bytes are written.
- After any database spill, restored pages are written to the database under
  the existing EXCLUSIVE lock and then marked clean. The durable main journal
  still protects transaction-original images.
- A restored page absent from cache is ignored before the first spill because
  an unspilled dirty page cannot have been evicted. After a spill it is written
  directly to the database.
- `CompletePlayback()` reparses page 1 when the restored image is nonempty and
  leaves the write transaction active.

After successful savepoint playback, the pager recomputes net modification
bookkeeping. In particular, when both the transaction-start and restored
images are empty, `transaction_modified` and the cached change-counter update
are cleared so a following commit takes the no-op journal-finalization path
without database I/O.

Nested savepoints, release, main-journal playback, and subjournal playback
remain coordinator/backend responsibilities.

Storage coordinators may report a post-mutation failure through the pager's
non-allocating coordinator-failure latch. The first code remains authoritative.
While latched, coordinator claiming, writable-page access, allocation,
truncation, commit, and savepoint creation/release return that code. Full
rollback remains legal. Rollback to a savepoint that already existed before
the failure also remains legal because new savepoints cannot be created while
latched; successful playback clears both the failure latch and coordinator
claim only after the saved image has been restored. A failed rollback retains
both.

Commit, full-rollback, and savepoint-rollback attempts also seal the active
write generation immediately before invalidating handles. If an attempt
fails, ordinary writable-page access, allocation, truncation, coordinator
claiming, and savepoint creation/release remain blocked. Commit retry, full
rollback, and rollback to a savepoint that predates the attempt remain legal.
Successful savepoint rollback clears the seal only after playback completes;
successful terminal cleanup clears it with the rest of the write state.

### 13. Recover a hot journal before publishing a writable read snapshot

Read-only `Open()` retains ADR-0022 behavior and returns
`ErrorCode::kProtocol` when it detects a hot rollback journal.

For `OpenWritable()`, `BeginRead()`:

1. acquires SHARED;
2. checks whether a nonempty database has a journal pathname, no RESERVED lock
   held by another process, and a nonzero first journal byte;
3. upgrades directly to EXCLUSIVE when recovery is required;
4. calls `RecoverHotJournal()` with a database recovery target;
5. physically restores size and page images;
6. synchronizes the database;
7. finalizes the journal;
8. downgrades to SHARED; and
9. reads and publishes the recovered snapshot.

There is no busy loop in this node. Lock contention is returned as
`ErrorCode::kBusy`; retry policy belongs to the later transaction/concurrency
layer. A failed SHARED-to-EXCLUSIVE recovery upgrade first releases the
tentative SHARED lock to NONE before returning `kBusy`. This prevents two
recoverers from retaining mutually blocking SHARED locks. If that cleanup
unlock fails, the pager exposes the retained-lock cleanup state and
`CleanupReadState()` retry contract instead of reporting ordinary contention.

Hot recovery clears all cache state before database restoration. A failed
recovery never exposes a read transaction and leaves the pager in `kError`.
The caller must destroy and reopen that pager after the external cause is
resolved.

### 14. Define pin and operation boundaries explicitly

Read and write pins are move-only. The following operations require zero
outstanding page pins:

- `EndRead()`;
- `Commit()`;
- `Rollback()`;
- `RollbackToSavepoint()`;
- `TruncateImage()`;
- full or hot recovery; and
- cache replacement or clearing.

`BeginWrite()`, `ReadPage()`, `WritePage()`, `AllocatePage()`, and savepoint
creation/release may coexist with unrelated read pins. Savepoint creation
requires every write pin to be released so later mutation cannot bypass
savepoint capture. Commit or rollback returns `ErrorCode::kBusy` instead of
invalidating a caller's live view.

Read and write pins for the same page never coexist. The cache returns
`ErrorCode::kBusy` before journal or dirty state changes.

No pin destructor performs I/O, allocation, synchronization, or error
reporting.

### 15. Preserve explicit failure and retry semantics

The pager classifies failures as follows:

- lock contention and live-pin conflicts return `kBusy` and leave state
  retryable;
- option, page-number, and state violations return `kMisuse` or `kCorruption`
  without changing durable state;
- allocation failure returns `kOutOfMemory`; if journal state was already
  changed, rollback is required;
- journal append/sync, database write, and database sync failures leave the
  write transaction failed and permit only rollback;
- a storage-coordinator post-mutation failure latches its first code in the
  pager, rejects commit and later mutation, and permits only full rollback or
  rollback to an already-active savepoint;
- a failed commit or rollback attempt seals the invalidated generation against
  all ordinary mutation until commit/rollback cleanup or successful rollback
  to an already-active savepoint;
- rollback playback and journal commit/rollback finalization failures enter
  persistent `kError`;
- post-commit physical truncation or SHARED-lock downgrade failures remain
  retryable in `kWriterFinished`; and
- read-transaction unlock failure retains the lock and preserves the existing
  `CleanupReadState()` retry contract.

While in `kError`, page acquisition, new transactions, commit, and ordinary
rollback return the stored error. Destruction performs only handle closure.
Reopening is the recovery boundary because the surviving hot journal is the
authoritative source of truth.

Public allocation boundaries translate `std::bad_alloc` to
`ErrorCode::kOutOfMemory`. No broad exception catch converts unknown failures
into success.

The B-tree coordinator claim is one such complete boundary: duplicate,
sealed-generation, persistent-error, and rollback-required error construction
cannot leak an allocation exception.

Pager write-state error construction uses the same non-throwing translation,
so truncation, commit, savepoint creation/release, and persistent-error cleanup
return `kOutOfMemory` rather than throwing while a transaction requires
cleanup.

### 16. Make journal finalization exception-safe

Journal deletion is an irreversible commit/rollback boundary and must not let
an allocation exception bypass terminal state transitions.

This node will harden the tightly coupled journal/VFS path:

1. POSIX deletion computes and owns both the target path and parent-directory
   path before `unlink()`.
2. No pathname allocation occurs after successful unlink and before optional
   directory synchronization.
3. `Vfs::Delete()` translates `std::bad_alloc` from a backend to
   `ErrorCode::kOutOfMemory`.
4. `RollbackJournal::Finalize()` catches any remaining allocation failure,
   marks the backend terminal, and returns an error after closing active
   handles.
5. `JournalTransaction::Commit()` and `Rollback()` catch allocation failures
   from finalization and enter `JournalTransactionState::kError`.
6. `RecoverHotJournal()` returns finalization OOM, and the pager enters
   `PagerState::kError`.

No allocation failure after journal deletion is reported as rollback-required.
Once deletion may have occurred, the only safe state is persistent error and
reopen.

### 17. Keep the steady write path allocation-conscious

The implementation will preserve these measurable properties:

- cache hits, pin release, dirty transitions, pressure inspection,
  writeback-candidate selection, page writes, page sync, and pin destruction
  allocate nothing;
- repeated writes to a cached page after its required transaction/savepoint
  captures do not reload or allocate already protected sector neighbors;
- spill and commit traversal use intrusive cache links and one page at a time;
- rollback and hot playback reuse the rollback backend's record buffer;
- cache clear and range discard allocate nothing; and
- OOM tests cover each setup/growth boundary that may allocate.

Initial cache insertion, first journal/set membership, savepoint growth, and
backend setup may allocate and return explicit OOM.

No speculative write batching, asynchronous I/O, mmap write path, or custom
allocator is introduced before the writable performance baseline.

## Test Strategy

Implementation will proceed red-first with focused tests in these groups.

### State and ownership

- read-only construction remains byte-for-byte behaviorally compatible;
- writable construction uses read-write/create flags and rejects fallback;
- rollback write/read versions are required before RESERVED acquisition;
- rollback-readable images with WAL or unknown write versions remain readable
  but are not writable;
- legal and illegal state transitions;
- RESERVED, EXCLUSIVE, SHARED, and NONE lock order;
- live-pin rejection and retry;
- same-page read/write and write/write pin exclusion;
- destructor and move/copy constraints; and
- empty write transactions perform no database write or sync.

### Writable pages and image size

- original image capture precedes mutable access;
- failed capture never grants mutable access;
- repeated writes and savepoint-after-write capture;
- large-sector cohort capture;
- repeated large-sector writes with a cache smaller than the sector cohort do
  not reload protected neighbors;
- zero-filled append and locking-page skip;
- final-image shrink with dirty, clean, absent, and pinned tail pages;
- retained-page read and write pins block final-image shrink without changing
  the logical size;
- page-1 size mismatch and nonempty-to-zero shrink rejection;
- write, allocation, savepoint, and second-shrink rejection after
  `TruncateImage()`;
- shrink followed by commit or full rollback; and
- page-number and offset overflow.

### Spill and commit ordering

- cache pressure selects the oldest unpinned dirty page;
- pinned dirty pages preserve the soft-capacity escape;
- journal synchronization precedes every database write epoch;
- newly captured records force a later synchronization epoch;
- EXCLUSIVE lock precedes database writes;
- page 1 change metadata is updated exactly once;
- dirty pages above the logical image are not written;
- database sync precedes journal deletion;
- a synchronized nonzero page-1 count makes shrink durable before deletion;
- post-commit shrink follows journal deletion; and
- cleanup retry never repeats phase-one writes.

### Rollback, savepoints, and recovery

- cache-only rollback performs no database write;
- spilled-page rollback restores bytes and original size;
- transaction-new pages disappear on rollback;
- savepoint rollback before and after spill;
- nested release and rollback;
- coordinator failure rejects late savepoint creation and commit, while
  successful rollback to a preexisting savepoint admits exactly one new
  coordinator;
- failed commit attempts reject direct page mutation and late savepoints while
  preserving commit retry and preexisting-savepoint rollback;
- page-1 header refresh;
- initialization rolled back to an initially empty image followed by no-op
  commit;
- hot recovery before snapshot publication;
- two-handle hot-recovery contention releases both tentative SHARED locks so a
  later retry can recover;
- stale/zero-header journals remain non-hot; and
- read-only pagers continue rejecting hot journals.

### Deterministic failure matrix

Every mutating or state-publishing boundary will have an injected failure:

- RESERVED and EXCLUSIVE acquisition;
- journal open, header write, record write, subjournal write, and sync;
- cache insertion and coordinator membership allocation;
- database page write, truncate, and sync;
- journal commit deletion and rollback deletion;
- allocation before deletion and after unlink but before directory sync;
- EXCLUSIVE-to-SHARED downgrade and final unlock; and
- recovery target prepare, resize, restore, sync, and completion.

Each test asserts the exact pager/coordinator state, legal retry operation,
lock retained, cache dirtiness, journal hotness, and durable database image.

### Crash and compatibility

A deterministic crash VFS will enumerate every journal/database mutation and
sync cut during:

- a one-page commit;
- a multi-page commit with spill;
- logical growth;
- nonzero logical shrink, including cuts immediately before and after journal
  deletion and before physical tail truncation; and
- rollback after a database write.

Reopening each durable image through the writable pager must yield either the
complete old database or the complete committed database, never a mixed image.

A pinned SQLite 3.54.0 fixture will be modified through page 1 using the pager.
SQLite will reopen the committed image and observe the expected compatible
header metadata. Rollback and hot-recovery fixtures will compare restored bytes
with the SQLite-produced original.

### Allocation and performance contracts

Allocation-observable tests will prove:

- sequential cached write-pin acquisition is allocation-free after capture;
- repeated large-sector writes do not allocate or reread protected neighbors;
- cache pressure inspection and successful spill are allocation-free;
- commit traversal is allocation-free after page 1 is resident and all
  journal membership is established; and
- allocation failures at construction, first cache insertion, first journal
  capture, savepoint growth, recovery setup, and every finalization boundary
  return `kOutOfMemory` with the required failed or terminal state.

Instrumentation tests will count page reads, page writes, cache hits/misses,
VFS calls, and allocations. A fixed pager microbenchmark will be added only as
an implementation diagnostic; the repository's durable write-performance
threshold remains the responsibility of Node 40.

## Consequences

### Positive

- One owner coordinates the file, lock, snapshot, cache, journal, and recovery
  state required by both read and write consumers.
- Existing B-tree readers can consume the same pager that later B-tree
  mutation uses.
- Mutable access is impossible before rollback capture succeeds.
- Journal-before-database ordering is enforced at every spill and commit.
- Logical shrink avoids journaling untouched tail bytes solely to support
  premature truncation.
- Cache mechanics stay reusable and independent of durability policy.
- Hot recovery becomes part of writable snapshot acquisition rather than a
  caller-side special case.
- The design exposes deterministic boundaries for crash, I/O-failure, OOM, and
  allocation tests.

### Negative

- Renaming `ReadPager` touches existing read consumers and historical source
  paths.
- The initial commit path writes dirty pages one at a time in dirty-recency
  order instead of batching sorted adjacent pages.
- A savepoint created before any mutation opens an otherwise unnecessary
  journal.
- Cache-only rollback still parses journal records even though it can discard
  the cache without writing the database.
- Post-finalization truncate failure reports an error even though the
  synchronized page-1 count means the logical commit is already durable.

### Deferred

- B-tree freelist allocation, balancing, overflow mutation, and header
  page-count maintenance;
- implicit/explicit transaction and statement-savepoint policy;
- multi-database super-journal coordination;
- busy handlers, lock retry, deadlock policy, and multi-process test policy;
- WAL, mmap writes, atomic-write optimization, and alternate journal modes;
- sorted/contiguous dirty-page batches, asynchronous writeback, and direct I/O;
- cache page rekeying unless B-tree balancing proves it necessary; and
- writable performance thresholds and regression budgets.
