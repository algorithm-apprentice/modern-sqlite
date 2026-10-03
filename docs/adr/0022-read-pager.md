# ADR-0022: Read-Only Pager

- Status: Accepted
- Date: 2026-10-03

## Context

The first pager node must open SQLite format-3 database files, hold a
consistent read transaction, validate the database header, fetch fixed-size
pages through the page cache, and invalidate stale cached pages between
transactions. It must not introduce rollback recovery, WAL, B-tree decoding,
or write policy before their dependency-graph nodes.

Pinned SQLite 3.54.0 separates the relevant behavior across pager and B-tree:

- `src/pager.c:3075-3140` reads one complete database page, accepts
  zero-filled short reads, and records bytes 24 through 39 from page 1.
- `src/pager.c:3333-3378` derives physical page count by rounding a nonempty
  file up to the configured page size.
- `src/pager.c:3821-3890` changes page size only when no page references are
  outstanding and resets the cache when the size changes.
- `src/pager.c:3951-3977` reads the initial database header and treats a
  short read as zero-filled data.
- `src/pager.c:5196-5314` defines when a rollback journal is hot.
- `src/pager.c:5316-5532` acquires SHARED, handles recovery, compares header
  bytes 24 through 39 on later transactions, clears stale cache entries, and
  computes the transaction page count.
- `src/pager.c:5597-5698` serves cache hits and reads misses by page number.
- `src/btree.c:3312-3462` validates the format-3 magic, read version, payload
  fractions, page size, reserved space, and header page count.
- `test/rdonly.test:39-75` proves that an unknown write-version byte still
  permits read-only access.

The existing Modern SQLite VFS supplies positioned reads, zero-filled short
reads, database locks, reserved-lock inspection, sidecar opens, and explicit
unlock errors. ADR-0021 supplies stable cached page frames and move-only pins.

Rollback-journal recovery and WAL are later nodes. Silently reading the main
database while a hot journal or current WAL may contain the authoritative
state would violate snapshot consistency. This pager must detect and reject
those states explicitly rather than return stale pages.

## Decision

Add `DatabaseHeader`, `ParseDatabaseHeader`, and `ReadPager` under
`modern_sqlite/pager`.

```cpp
class DatabaseHeader final {
 public:
  ByteCount page_size() const noexcept;
  ByteCount reserved_bytes() const noexcept;
  ByteCount usable_size() const noexcept;
  std::uint8_t write_version() const noexcept;
  std::uint8_t read_version() const noexcept;
  std::uint32_t file_change_counter() const noexcept;
  std::uint32_t header_page_count() const noexcept;
  PageNumber first_freelist_trunk() const noexcept;
  std::uint32_t freelist_page_count() const noexcept;
  std::uint32_t schema_cookie() const noexcept;
  std::uint32_t schema_format() const noexcept;
  std::int32_t suggested_cache_size() const noexcept;
  PageNumber largest_root_page() const noexcept;
  std::uint32_t text_encoding() const noexcept;
  std::uint32_t user_version() const noexcept;
  std::uint32_t incremental_vacuum() const noexcept;
  std::uint32_t application_id() const noexcept;
  std::uint32_t version_valid_for() const noexcept;
  std::uint32_t sqlite_version() const noexcept;
};

Result<DatabaseHeader> ParseDatabaseHeader(ByteView bytes);

struct ReadPagerOptions {
  ByteCount empty_database_page_size = ByteCount{4096};
  std::size_t cache_capacity_pages = 512;
};

class ReadPager final {
 public:
  static Result<std::unique_ptr<ReadPager>> Open(
      Vfs& vfs, std::string_view path, ReadPagerOptions options = {});

  Status BeginRead();
  Status EndRead();
  Result<ReadPagePin> ReadPage(PageNumber page_number);

  bool in_read_transaction() const noexcept;
  const DatabaseHeader* header() const noexcept;
  ByteCount page_size() const noexcept;
  std::uint32_t page_count() const noexcept;
  std::uint64_t data_version() const noexcept;
  std::string_view path() const noexcept;
};
```

The pager, its transaction methods, and all page pins are externally
serialized. `Vfs` must outlive the pager. The pager must outlive every returned
page pin. `ReadPagePin` owns an underlying cache pin but exposes only immutable
`PageFrame` access, so read-pager consumers cannot introduce dirty cache
entries.

### Open and ownership

- `Open` validates the configured empty-database page size as a power of two
  from 512 through 65536.
- It resolves and owns the full database path, derives the `-journal` and
  `-wal` sidecar paths once, and opens the main database read-only without
  creating it.
- Opening performs no database lock or page read.
- The pager owns the `File` and lazily creates its `PageCache` after a
  transaction determines the effective page size.
- The pager is non-copyable and non-movable.

### Header parsing

`ParseDatabaseHeader` requires at least the 100-byte SQLite database header
and validates the same structural fields used by pinned SQLite:

- exact `SQLite format 3\0` magic;
- page-size encoding, including encoded value 1 for 65536 bytes;
- a power-of-two page size from 512 through 65536;
- read version no greater than 2;
- payload fractions exactly 64, 32, and 32; and
- at least 480 usable bytes after the reserved region.

The write-version byte is retained without rejection. SQLite treats values
greater than its known versions as read-only, and this pager is already
read-only.

The parser loads the remaining standard header integers without assigning
catalog or B-tree policy to them. Text encoding, schema format, freelist
structure, auto-vacuum fields, and page-1 B-tree bytes are validated by their
future consumers.

Malformed structural fields return `kNotDatabase`. An otherwise valid header
whose trusted page count exceeds the physical file page count returns
`kCorruption` from transaction setup.

### Read transaction

`BeginRead`:

1. Rejects a nested transaction.
2. Retries cleanup of any SHARED lock retained after an earlier unlock error.
3. Acquires a SHARED database lock with no pager-level busy loop.
4. Reads the database file size.
5. Detects an unsupported hot rollback journal.
6. Reads the first 100 bytes of a nonempty database.
7. Accepts a zero-length file as an empty database only after rejecting a
   nonempty WAL sidecar.
8. Parses a nonempty file header and rejects read-version 2 because WAL is
   not yet implemented;
9. Rejects a nonempty `-wal` sidecar conservatively.
10. Computes the physical and effective page counts.
11. Invalidates or replaces the cache if the database identity or page size
    changed; and
12. Publishes the header and page count only after every step succeeds.

Lock contention and VFS errors propagate unchanged. If setup fails after
SHARED was acquired, the pager explicitly attempts to unlock. If that cleanup
also fails, the cleanup error is returned with the original setup failure in
its message, and the retained lock is retried before the next transaction.

`EndRead` rejects calls without an active transaction and returns `kBusy`
while any page pin remains. Unlock failure leaves the transaction active so
the caller can release resources and retry. Successful end clears the current
header and page count but retains clean cached pages for the next transaction.

The file destructor remains a best-effort final lock cleanup. Normal code uses
`EndRead` so unlock failures stay observable.

### Empty and nonempty databases

- After sidecar safety checks, a zero-length file is a valid empty database
  with page count zero, no `DatabaseHeader`, and the configured
  empty-database page size.
- A nonempty file shorter than 100 bytes fails as `kNotDatabase`.
- Physical page count is `ceil(file_size / page_size)`, matching SQLite's
  treatment of a partial final page.
- Header page count at offset 28 is trusted only when it is nonzero and the
  change counter at offset 24 equals the version-valid-for value at offset
  92. Otherwise the physical page count is used.
- A trusted header page count greater than the physical page count is
  corruption. A smaller trusted count intentionally hides trailing bytes,
  matching SQLite.
- Page counts that do not fit `std::uint32_t` return `kTooLarge`.

### Journal and WAL safety

The read-only pager implements only enough sidecar inspection to avoid stale
reads:

- A rollback journal is hot when the database is nonempty, the journal
  exists, no participant holds RESERVED or greater, and its first byte is
  nonzero.
- An active writer's journal is not hot because the RESERVED lock proves the
  main database still represents the committed snapshot visible to SHARED
  readers.
- Zero-length and zero-first-byte journals are ignored.
- A hot journal returns `kProtocol` with an explicit recovery-required
  message. It is never replayed, deleted, or modified by this node.
- Read-version 2 or a nonempty WAL sidecar returns `kProtocol`.

These checks are deliberately conservative. Super-journal false positives and
WAL validity parsing are resolved by their later format/recovery nodes.

### Cache identity and invalidation

The database identity is the 16-byte sequence at header offsets 24 through 39,
matching SQLite's pager comparison.

- The first successful transaction establishes the identity without
  incrementing `data_version`.
- After a later SHARED acquisition, a changed identity or transition between
  empty and nonempty invalidates every unpinned clean page and increments
  `data_version`.
- A page-size change replaces the complete cache and also increments
  `data_version`.
- `EndRead` permits no outstanding pins, so invalidation never discards a
  borrowed page.
- File size and effective page count are recomputed for every transaction even
  when the identity is unchanged.

The same vanishingly small missed-change probability as SQLite's 16-byte
comparison is accepted.

### Page fetches

`ReadPage` requires an active read transaction.

- Page zero, a page beyond the effective transaction page count, and the
  SQLite locking page at `0x40000000 / page_size + 1` return `kCorruption`.
- A cache hit returns a new move-only pin without file I/O or allocation.
- A miss allocates one exact-size `ByteBuffer`, performs one positioned read
  at `(page_number - 1) * page_size`, and moves the buffer into the cache.
- A short final-page read succeeds with the unread suffix already zero-filled
  by the VFS contract, matching SQLite.
- An I/O failure inserts nothing, so a later retry performs a fresh read.
- Read pages remain clean. This node never requests mutable bytes.

## Explicitly deferred behavior

- Rollback-journal playback, deletion, synchronization, and super-journals.
- WAL snapshots, WAL-index shared memory, checkpoints, and read-only WAL.
- Write transactions, dirty pages, spill/writeback, savepoints, and recovery.
- Busy handlers, locking-mode changes, exclusive transactions, and no-lock or
  immutable URI modes.
- Temporary, transient, and in-memory databases.
- Memory-mapped page fetches and direct overflow reads.
- Dynamic page size or cache capacity configuration.
- Encryption/codec hooks.
- B-tree page validation, freelist traversal, pointer maps, schema encoding,
  and catalog semantics.

## Validation plan

Tests cover:

- every supported page-size boundary, including 65536 encoding;
- exact header field decoding and unknown write-version acceptance;
- bad magic, truncated headers, invalid page sizes, invalid read versions,
  payload fractions, and insufficient usable space;
- empty files, partial final pages, trusted and untrusted header page counts,
  and impossible physical sizes;
- full-path resolution, read-only open, SHARED/None lock ordering, lock
  contention, unlock failure and retry, and pinned-page end rejection;
- hit/miss behavior, exact offsets, zero-filled short page reads, out-of-range
  and locking-page rejection, and retry after I/O failure;
- cache reuse across transactions and invalidation after bytes 24 through 39
  change;
- hot, active, zero-header, and missing rollback journals;
- WAL read-version and nonempty WAL-sidecar rejection for both empty and
  nonempty main databases;
- SQLite 3.54.0-created 512-, 4096-, and 65536-byte-page fixtures; and
- zero allocations for optimized cache-hit pager reads.

The optimized baseline records pager cache hits, cold positioned reads, and
transaction begin/end costs with allocation and I/O counts. Optimization
requires a repeatable integrated bottleneck.

## Consequences

- The next B-tree reader receives stable immutable page pins and one validated
  transaction snapshot without depending on journal or WAL implementations.
- Corrupt headers, stale sidecar states, lock conflicts, and cleanup failures
  remain explicit.
- Cached pages survive read transactions but are invalidated with SQLite's
  native change token after reacquiring SHARED.
- The initial read path is small, allocation-free on cache hits, and ready for
  matched SQLite compatibility and performance work after B-tree integration.
