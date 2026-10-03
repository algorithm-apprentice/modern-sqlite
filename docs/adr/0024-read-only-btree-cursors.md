# ADR-0024: Read-Only B-Tree Cursors

- Status: Accepted
- Date: 2026-10-03

## Context

ADR-0023 provides allocation-free borrowed decoders for individual SQLite
B-tree pages and cells. The next dependency-graph node must combine those
decoders with the read-only pager, record codec, and collation layer to expose
ordered table and index traversal, directional seeks, and payload access.

Pinned SQLite 3.54.0 defines the relevant behavior across `src/btreeInt.h`,
`src/btree.c`, and `src/vdbe.c`:

- `src/btreeInt.h:490-607` stores a cursor path of pinned pages and parent
  child indexes, with `BTCURSOR_MAX_DEPTH` equal to 20.
- `src/btree.c:2409-2440` rejects page references beyond the current snapshot.
- `src/btree.c:5486-5676` validates root and child page kinds, accepts an empty
  leaf root, and permits an empty interior virtual root only on page 1.
- `src/btree.c:5678-5820` descends to the first and last leaf entries.
- `src/btree.c:5825-5980` binary-searches signed rowids while treating table
  interior cells only as separators.
- `src/btree.c:6060-6300` binary-searches complete encoded index records on
  leaf and interior pages, assembling overflow-backed keys before comparison.
- `src/btree.c:6340-6520` advances and retreats across page boundaries,
  returning index interior cells as entries while skipping table separators.
- `src/btree.c:5155-5473` exposes local payload bytes and reads requested
  ranges through overflow chains.
- `src/vdbe.c:4940-5155` converts a nearby B-tree search result into exact
  `SeekGE`, `SeekGT`, `SeekLE`, and `SeekLT` behavior.

The existing `ReadPager` requires an active externally serialized read
transaction and returns move-only immutable page pins. `BtreePageView` borrows
the bytes held by those pins. `CompareIndexRecord` already implements
SQLite-compatible record-prefix comparison with per-column collations, sort
directions, and null placement.

The cursor API must preserve those ownership boundaries, avoid heap work on
ordinary movement through already cached pages, and keep corruption distinct
from caller misuse. It must not import mutation, balancing, schema, VM, or
whole-database integrity policy into this node.

## Decision

Add typed read-only cursors and payload access under
`modern_sqlite/storage/btree/cursor.hpp`.

```cpp
inline constexpr std::size_t kMaximumBtreeDepth = 20;

enum class BtreeSeekMode : std::uint8_t {
  kEqual,
  kGreaterOrEqual,
  kGreater,
  kLessOrEqual,
  kLess,
};

class BtreePayloadView final {
 public:
  ByteCount size() const noexcept;
  ByteView local_bytes() const noexcept;
  bool is_fully_local() const noexcept;
};

class TableBtreeCursor final {
 public:
  static Result<TableBtreeCursor> Open(
      ReadPager& pager, PageNumber root_page);

  TableBtreeCursor(TableBtreeCursor&&) noexcept;
  TableBtreeCursor& operator=(TableBtreeCursor&&) noexcept;

  bool valid() const noexcept;
  Result<bool> First();
  Result<bool> Last();
  Result<bool> Next();
  Result<bool> Previous();
  Result<bool> Seek(std::int64_t rowid, BtreeSeekMode mode);

  Result<std::int64_t> rowid() const;
  Result<BtreePayloadView> payload() const;
  Status ReadPayload(ByteOffset offset, MutableByteView destination);
  Result<ByteBuffer> CopyPayload();
};

class IndexBtreeCursor final {
 public:
  static Result<IndexBtreeCursor> Open(
      ReadPager& pager, PageNumber root_page,
      std::span<const IndexColumnOrder> columns);

  IndexBtreeCursor(IndexBtreeCursor&&) noexcept;
  IndexBtreeCursor& operator=(IndexBtreeCursor&&) noexcept;

  bool valid() const noexcept;
  Result<bool> First();
  Result<bool> Last();
  Result<bool> Next();
  Result<bool> Previous();
  Result<bool> Seek(
      std::span<const SqlValue> key, BtreeSeekMode mode);

  Result<BtreePayloadView> payload() const;
  Status ReadPayload(ByteOffset offset, MutableByteView destination);
  Result<ByteBuffer> CopyPayload();
};
```

Both cursor types are non-copyable and movable. They share one internal
traversal implementation, but the public types keep rowid-only and
record-key-only operations unrepresentable on the wrong tree kind.

`IndexBtreeCursor::Open` copies the small array of `IndexColumnOrder` values.
The referenced collations must outlive the cursor, but the caller's span and
container need not. Search keys are borrowed only for the duration of
`Seek`.

For a nonempty index tree, `Open` also snapshots the database header's schema
format and text encoding. Schema formats 1 through 4 map to the corresponding
`RecordCodecOptions`, which every index-record parse uses. A schema format
outside that range or a text encoding outside SQLite's values 1 through 3 is
`kNotDatabase`. UTF-16LE and UTF-16BE encodings return `kProtocol` until the
text layer provides their comparison adapters; this node supports only UTF-8
encoding 1.

### Transaction and ownership contract

Opening a cursor requires an active `ReadPager` transaction and a nonzero root
page. The cursor captures `ReadPager::data_version()` and rejects later use
with:

- `kMisuse` when no read transaction is active; or
- `kSchemaChanged` when the pager reports a different snapshot identity.

For a nonempty database, `Open` reads, parses, validates, and retains the root
page pin. Every descended page is retained in a fixed-capacity path of at most
20 frames. Each frame contains one move-only page pin and the selected cell or
child slot. Retaining the root and path pins keeps borrowed page bytes stable
and causes `ReadPager::EndRead()` to return `kBusy` while the cursor is alive.

An empty database is accepted only as an empty table B-tree rooted at page 1.
It has no page to pin, geometry, or valid position, so the captured
`data_version` protects later use across transaction boundaries.

`BtreePayloadView::local_bytes()` borrows the current cell's pinned page. It
remains valid only until the next positioning operation, cursor move
construction, cursor move-assignment, or cursor destruction. Payload reads do
not reposition the cursor.

The pager and cursors remain externally serialized. A cursor must not outlive
its pager, and referenced collations must not outlive an index cursor.

Path frame zero is the retained root, and the 20-frame limit includes that
root. Every transition to an invalid position--an empty tree, unsuccessful
seek, end-of-tree movement, or traversal error--releases all descendant frames
while retaining the nonempty database's root frame. A moved-from cursor
reports `valid() == false`, and its fallible operations return `kMisuse`.
`kSchemaChanged` is terminal for that cursor and requires a new `Open`; only
same-snapshot traversal errors may be retried with `First`, `Last`, or `Seek`.

### Root, child, and depth validation

`Open` checks `page_count()` before requiring a header. When the count is
zero, only `TableBtreeCursor::Open(pager, PageNumber{1})` succeeds; it captures
the pager and `data_version` but stores no page geometry or pin and can never
become positioned while that snapshot remains empty. Other nonzero roots and
all index cursors return `kCorruption`; root zero remains `kMisuse`.

For a nonempty database, the pager header must be present and supplies the
page and usable sizes for `BtreePageGeometry`. `Open` then validates the
requested root against the current snapshot.

- A table cursor requires a table B-tree root.
- An index cursor requires an index B-tree root.
- A zero-cell leaf root is an empty tree.
- A zero-cell interior root is valid only on page 1 and represents SQLite's
  virtual root with one right-most child.
- Every non-root child must contain at least one cell and have the same
  table-versus-index kind as the root.
- Every child reference, and every overflow reference that an operation must
  follow, is resolved through `ReadPager`, so zero, locking-page, and
  beyond-snapshot references are corruption.
- Descending beyond 20 simultaneously pinned B-tree pages is corruption.

The cursor does not prove global tree invariants. Equal leaf depth, parent
uniqueness, separator ordering, duplicate page references, pointer-map
agreement, and unreachable pages remain the diagnostics layer's
responsibility.

### Positioning and movement

`First`, `Last`, and successful `Seek` return `true` and leave the cursor
valid. An empty tree or a directional seek with no qualifying entry returns
`false` and leaves the cursor invalid. `Next` and `Previous` require a valid
position; crossing the final or first entry returns `false` and invalidates
the position. Calling either movement method while already invalid is
`kMisuse`. `First`, `Last`, or `Seek` may always be used to reposition a
cursor.

Any traversal or seek error invalidates the current position and releases all
path pins except the retained root pin. Except after terminal
`kSchemaChanged`, the cursor can be reused by calling `First`, `Last`, or
`Seek`. Payload-read errors do not alter the current position.

Movement follows pinned SQLite:

- Every `First`, `Last`, and `Seek` first resets to the root.
- A zero-cell leaf root is empty. A zero-cell interior root is accepted only
  on page 1; the operation first descends through its right-most child,
  validates that child as nonempty and of the same tree kind, and then
  continues normally.
- After root normalization, `First` repeatedly enters the left child of cell
  zero until reaching a leaf.
- After root normalization, `Last` repeatedly follows the right-most child
  and selects the final leaf cell.
- Table traversal returns only leaf cells and skips every interior separator.
- Index traversal returns interior cells in-order between their left and
  right subtrees.
- Moving forward from an interior index cell enters the following child
  subtree and descends left-most.
- Moving backward from an interior index cell enters that cell's left child
  and descends right-most.

Page opening and ordinary same-page movement perform no heap allocation.
Cache misses retain the pager's existing one-page allocation behavior.

### Table seeks

Table seeks binary-search signed 64-bit rowids. An exact match on interior
cell `i` descends through that cell's left child, child slot `i`, because
table interior cells are separators rather than rows. The right-most child is
selected only when the insertion slot equals `cell_count()`. The raw search
finishes on an exact leaf row or a neighboring leaf row, after which the
cursor applies the requested direction:

- `kEqual`: only an exact rowid succeeds;
- `kGreaterOrEqual`: the first rowid not less than the key;
- `kGreater`: the first rowid greater than the key;
- `kLessOrEqual`: the last rowid not greater than the key; and
- `kLess`: the last rowid less than the key.

No numeric-affinity conversion occurs in this layer. VM or expression code
must convert SQL values to a rowid before calling the table cursor.

### Index seeks

Index seeks binary-search both leaf and interior cells. Every compared payload
is parsed as one complete SQLite record and passed to `CompareIndexRecord`.
Malformed records and records with fewer fields than the search key are
corruption.

The search key must be nonempty and no longer than the copied comparison
metadata. It may be a prefix of the stored record. Prefix-equivalent records
use SQLite's directional default ordering:

- `kGreaterOrEqual` and `kLess` treat a prefix-equivalent record as greater
  than the key;
- `kGreater` and `kLessOrEqual` treat it as less than the key; and
- `kEqual` treats it as equivalent and succeeds on any matching entry.

Those defaults make greater-or-equal find the first matching prefix,
greater skip all matching prefixes, less-or-equal find the last matching
prefix, and less exclude all matching prefixes. Exact full-record searches
follow the same rules.

A fully local index record is compared directly from the pinned page. An
overflow-backed record is copied into a cursor-owned reusable scratch buffer
before parsing and comparison. The buffer grows only when necessary and is
reused across comparisons and seeks.

Before growing that buffer, the cursor computes the exact number of overflow
pages implied by the logical and local payload sizes:

`required = 1 + (remaining - 1) / (usable_size - 4)`.

If `required` exceeds `ReadPager::page_count()`, the payload is impossible in
the current snapshot and the seek returns `kCorruption` without attempting
the payload-sized allocation. This is only a necessary size bound; it does
not prove page uniqueness, ownership, acyclicity, or a zero unused final
link.

### Payload access

`payload()` returns the logical payload size and the bytes stored locally on
the current B-tree page. It allocates nothing. `is_fully_local()` is true when
the local byte count equals the logical size.

`ReadPayload` copies exactly the requested range into caller-owned memory:

1. let `N` be the logical payload size and `O = offset.value()`, then reject
   the range when `O > N` or `destination.size() > N - O`;
2. copy any overlapping local bytes;
3. follow overflow links from the cell's first overflow page;
4. copy at most `usable_size - 4` bytes from each overflow page; and
5. return `kCorruption` if the chain ends before the requested range.

Thus `O == N` is valid only for an empty destination. Invalid ranges are
rejected before copying or I/O and leave the destination unchanged.

Only overflow references that must actually be followed are decoded and
resolved through `ReadPager`. After the requested bytes have been copied from
an overflow page, the operation returns without validating or resolving that
page's next-link value. If additional bytes remain, a zero next link is
premature termination and therefore `kCorruption`.

Before traversing overflow pages, the cursor computes how many pages are
required to reach the end of the requested range. If that count exceeds the
snapshot page count, the range is impossible and returns `kCorruption`
without traversal. Intermediate pages use `OverflowPageView` to decode the
next link. The final page needed by the request copies its payload bytes
directly from the exact-size pager page so an unused next link is not decoded.

Like pinned SQLite's payload reader, `ReadPayload` does not require the final
visited overflow page's unused next pointer to be zero. Extra overflow links,
cycles that do not prevent the requested range, pointer-map agreement, and
chain ownership are deferred to diagnostics. An I/O or corruption error after
copying begins may leave a copied prefix in the destination; no rollback is
promised. Cursor position and borrowed local payload remain unchanged.

`CopyPayload` allocates one exact-size `ByteBuffer`, calls `ReadPayload` for
the full range, and returns owned contiguous bytes. Local-only callers that
need zero-copy record parsing use `payload().local_bytes()` instead.

Before that allocation, `CopyPayload` applies the same required-overflow-page
bound as index comparison and returns `kCorruption` if the declared payload
cannot fit in the current snapshot.

The cursor does not cache overflow page numbers in this node. Repeated
out-of-order partial reads retraverse links from the first overflow page.
SQLite-style overflow-link caching and direct file reads require measured
benefit and belong to a later performance change.

### Error contract

- Invalid cursor state, inactive transactions, zero roots, empty index
  metadata, empty search keys, and wrong API use are `kMisuse`.
- Invalid payload ranges are `kOutOfRange`.
- Changed pager snapshot identity is `kSchemaChanged`.
- Invalid schema-format or text-encoding header values are `kNotDatabase`;
  valid but unsupported UTF-16 encodings are `kProtocol`.
- Malformed page bytes, wrong tree kinds, empty non-root pages, excessive
  depth, malformed records, impossible complete payload sizes, premature
  overflow termination, and invalid followed page references are
  `kCorruption`.
- Pager I/O, cache, lock, and allocation failures propagate unchanged.

No broad exception handling, silent fallback, or success-shaped corruption
result is introduced.

## Explicitly deferred behavior

- Inserts, deletes, updates, balancing, page allocation, and free-space use.
- Cursor save/restore across writes or concurrent schema changes.
- Rollback-journal recovery and WAL snapshots.
- VM affinity conversion and opcode execution.
- Catalog, schema, WITHOUT ROWID, and index-metadata construction.
- UTF-16 record-text decoding, transcoding, and comparison adapters.
- Streaming record decoding across noncontiguous overflow pages.
- Overflow page-number caches and direct overflow file reads.
- Whole-tree ordering, equal-depth, parent uniqueness, pointer-map, freelist,
  reachability, and overflow ownership diagnostics.
- Shared-cache table locks, busy handlers, and cross-connection cursor
  coordination.

## Validation plan

Tests use pinned SQLite 3.54.0 fixtures and focused malformed-page mutations to
cover:

- empty, single-page, virtual-root, and multi-level table trees;
- negative, zero, sparse, boundary, and overflow-backed rowids;
- forward and reverse table traversal across leaf boundaries;
- all five table seek modes at exact matches, gaps, and both ends;
- multi-level index traversal that proves interior cells are returned;
- duplicate and prefix index seeks under BINARY, NOCASE, and RTRIM
  collations, ascending and descending fields, and null placement;
- local and overflow-backed index comparisons;
- local borrowed payloads, partial cross-boundary reads, complete copies,
  range rejection, premature overflow termination, and beyond-snapshot
  references;
- impossible declared payload sizes rejected before payload-sized allocation;
- impossible far-offset payload reads rejected before overflow traversal and
  unused final overflow links left uninterpreted;
- wrong root or child kinds, empty non-root pages, excessive depth, malformed
  records, invalid schema formats, unsupported UTF-16 encodings, inactive
  transactions, snapshot changes, moved-from cursors, and pinned-page
  `EndRead` rejection; and
- deterministic mutation smoke tests under sanitizers.

Optimized baselines record cached same-leaf iteration, cross-page iteration,
table seeks, local index seeks, overflow index seeks, local payload access,
and complete overflow copies. Each baseline records allocations, pager reads,
and comparisons. Optimization follows measured repeatable bottlenecks rather
than speculative complexity.

## Consequences

- Higher layers receive type-safe ordered table and index access without
  depending on SQLite's VM or mutable B-tree implementation.
- Fixed path storage and retained pins make cursor ownership explicit and keep
  ordinary traversal allocation-free.
- Directional seeks expose the operation higher layers need instead of leaking
  SQLite's nearby-entry-plus-comparison protocol.
- Local payloads remain zero-copy, while overflow reads have explicit caller
  ownership and bounded range semantics.
- Global integrity guarantees remain isolated from the hot read path and can
  be added by the later diagnostics node.
