# ADR-0023: B-Tree Page Decoding

- Status: Accepted
- Date: 2026-10-03

## Context

The read-only B-tree cursor needs a pure boundary for decoding SQLite database
pages without importing pager ownership, file I/O, transaction state,
collations, or cursor traversal. The boundary must cover B-tree headers and
cells, local-versus-overflow payload placement, overflow pages, freelist trunk
pages, and auto-vacuum pointer-map pages.

Pinned SQLite 3.54.0 defines the relevant persistent format across
`src/btreeInt.h`, `src/btree.c`, and the SQLite database-file-format
specification:

- `src/btreeInt.h:106-218` defines B-tree page regions, headers, cell layouts,
  overflow chains, and freelist trunks.
- `src/btree.c:1205-1252` defines the format-critical local-payload formula.
- `src/btree.c:1269-1590` decodes the four cell layouts and their encoded
  sizes.
- `src/btree.c:2055-2111` accepts exactly page-type bytes `0x02`, `0x05`,
  `0x0a`, and `0x0d`.
- `src/btree.c:2118-2202` validates freeblock ordering, sizes, content
  offsets, fragments, and total free space.
- `src/btree.c:2248-2294` initializes page-header metadata and bounds the cell
  pointer array.
- `src/btree.c:3458-3479` derives the minimum and maximum local payload sizes
  from usable page size.
- `src/btree.c:1063-1174` locates and decodes five-byte pointer-map entries.
- `src/btree.c:6636-6656` accepts at most `usable_size / 4 - 2` freelist leaf
  pointers on a trunk page.

The existing coding module already provides SQLite-compatible varint decoding
and big-endian integers. The page cache and pager provide ownership and exact
page-sized immutable bytes, but the decoder must not depend on either module's
state or perform I/O.

## Decision

Add pure decoders under `modern_sqlite/storage/btree/page.hpp`. Extract the
existing strong `PageNumber` into
`modern_sqlite/storage/page_number.hpp` so cache, pager, and B-tree format code
share one page-number type without coupling the decoder to the page cache.

```cpp
class BtreePageGeometry final {
 public:
  static Result<BtreePageGeometry> Create(
      ByteCount page_size, ByteCount usable_size);

  ByteCount page_size() const noexcept;
  ByteCount usable_size() const noexcept;
  ByteCount minimum_local_payload() const noexcept;
  ByteCount maximum_index_local_payload() const noexcept;
  ByteCount maximum_table_leaf_local_payload() const noexcept;
  ByteCount overflow_payload_capacity() const noexcept;
  PageNumber locking_page() const noexcept;
};

enum class BtreePageType : std::uint8_t {
  kInteriorIndex = 0x02,
  kInteriorTable = 0x05,
  kLeafIndex = 0x0a,
  kLeafTable = 0x0d,
};

class BtreeCellView final {
 public:
  std::optional<PageNumber> left_child() const noexcept;
  std::optional<std::int64_t> rowid() const noexcept;
  ByteCount payload_size() const noexcept;
  ByteView local_payload() const noexcept;
  std::optional<PageNumber> first_overflow_page() const noexcept;
  ByteCount encoded_size() const noexcept;
};

class BtreePageFreeSpace final {
 public:
  ByteCount total() const noexcept;
  BtreeFreeblockCursor freeblocks() const noexcept;
};

class BtreePageView final {
 public:
  static Result<BtreePageView> Parse(
      ByteView page, PageNumber page_number, BtreePageGeometry geometry);

  PageNumber page_number() const noexcept;
  BtreePageType type() const noexcept;
  bool is_leaf() const noexcept;
  bool is_table() const noexcept;
  std::size_t cell_count() const noexcept;
  ByteCount cell_content_offset() const noexcept;
  ByteCount fragmented_free_bytes() const noexcept;
  std::optional<PageNumber> rightmost_child() const noexcept;
  Result<ByteCount> cell_offset(std::size_t index) const;
  Result<BtreeCellView> cell(std::size_t index) const;
  Result<BtreePageFreeSpace> AnalyzeFreeSpace() const;
};

class OverflowPageView final {
 public:
  static Result<OverflowPageView> Parse(
      ByteView page, BtreePageGeometry geometry);

  std::optional<PageNumber> next_page() const noexcept;
  ByteView payload() const noexcept;
};

class FreelistTrunkView final {
 public:
  static Result<FreelistTrunkView> Parse(
      ByteView page, BtreePageGeometry geometry);

  std::optional<PageNumber> next_trunk() const noexcept;
  std::size_t leaf_count() const noexcept;
  Result<PageNumber> leaf_page(std::size_t index) const;
};

enum class PointerMapType : std::uint8_t {
  kRootPage = 1,
  kFreePage = 2,
  kFirstOverflow = 3,
  kLaterOverflow = 4,
  kBtreeChild = 5,
};

struct PointerMapEntry {
  PointerMapType type;
  std::optional<PageNumber> parent;
};

Result<PageNumber> PointerMapPageFor(
    PageNumber target, BtreePageGeometry geometry);
Result<bool> IsPointerMapPage(
    PageNumber page_number, BtreePageGeometry geometry);

class PointerMapView final {
 public:
  static Result<PointerMapView> Parse(
      ByteView page, PageNumber page_number, BtreePageGeometry geometry);

  Result<PointerMapEntry> entry(PageNumber target) const;
};
```

All view types borrow immutable bytes. The page bytes must outlive the view and
every byte span derived from it. Views are cheap value types and perform no
heap allocation on successful parsing or access.

### Geometry

`BtreePageGeometry::Create` validates:

- page size is a power of two from 512 through 65536;
- usable size is between 480 and page size;
- the reserved region, `page_size - usable_size`, fits SQLite's one-byte
  database-header field and is therefore no larger than 255 bytes; and
- the derived payload thresholds fit the persistent-format ranges.

It computes the exact SQLite formulas, with `U` equal to usable size:

- minimum local payload: `((U - 12) * 32 / 255) - 23`;
- maximum local index payload: `((U - 12) * 64 / 255) - 23`;
- maximum local table-leaf payload: `U - 35`; and
- overflow payload capacity: `U - 4`.

For payload size `P` larger than its page-kind maximum `X`, the local amount is
`K = M + ((P - M) % (U - 4))` when `K <= X`, and `M` otherwise. This formula
is file-format compatibility behavior and is not simplified.

The geometry also exposes SQLite's locking page,
`0x40000000 / page_size + 1`.

### B-tree page validation

`BtreePageView::Parse` requires one exact page-sized buffer. Page 1 begins its
B-tree header at byte 100 and must be a table B-tree page; every other page
begins at byte zero.

Parsing validates:

- a usable, nonzero, non-locking page number no greater than
  `0xfffffffe`;
- one of the four defined page-type bytes;
- complete 8-byte leaf or 12-byte interior headers;
- a nonzero valid right-most child for interior pages;
- a fragment count no greater than 60;
- the 65536-byte zero encoding for the cell-content offset;
- a complete cell-pointer array that does not cross the content area;
- SQLite's maximum cell count, `(page_size - 8) / 6`;
- and the empty-page content-offset invariant.

Page parsing is constant-time. It validates only header metadata, the total
cell-pointer-array extent, and the maximum cell count. An individual cell
pointer and body are validated when that cell is requested.

### Cell decoding

Cell access returns `kOutOfRange` for an invalid cell index and `kCorruption`
for malformed bytes.

The decoder supports:

- interior table cells: left child plus signed 64-bit rowid;
- leaf table cells: payload size, signed rowid, local payload, and optional
  first overflow page;
- interior index cells: left child, key payload, and optional first overflow
  page; and
- leaf index cells: key payload and optional first overflow page.

SQLite-compatible overlong varints remain accepted. Truncated varints,
payload sizes above `0x7fffffff`, cell bodies beyond usable bytes, zero or
reserved page references, and missing overflow pointers are corruption.

Cells expose only the payload bytes stored locally. They do not fetch or join
overflow pages and do not parse record fields. A no-overflow leaf cell whose
logical encoding is shorter than four bytes still reports SQLite's minimum
four-byte cell size.

### Free space and freeblocks

`AnalyzeFreeSpace()` is a separate linear operation for diagnostics, mutation
planning, and integrity checks. Read cursors do not pay this cost when opening
a page. The analysis validates that:

- a nonzero first freeblock begins at or after the cell-content offset;
- every freeblock is wholly inside the usable region and at least four bytes
  long;
- freeblocks are strictly increasing, non-overlapping, and separated as
  SQLite requires; and
- the total free-space computation neither underflows nor exceeds the usable
  page.

The returned total is:

- the unallocated region between the cell-pointer array and content area;
- plus every validated freeblock size;
- plus fragmented free bytes.

`BtreeFreeblockCursor` iterates the already validated freeblock chain without
allocation or additional failure paths. Detecting overlap between a cell body
and a freeblock, duplicate cell offsets, or key-order violations belongs to
the later diagnostics/integrity layer because it requires correlating all
cell extents or interpreting keys.

### Overflow pages

`OverflowPageView` decodes the next-page pointer from the first four bytes and
exposes the remaining `usable_size - 4` bytes. A zero next pointer marks the
final page. The caller knows the remaining logical payload length and truncates
the final page accordingly.

The decoder validates a nonzero next-page reference when one is present, but
does not follow the chain, detect cycles, or compare the chain length with a
cell payload. Those operations require pager access and belong to the
read-only B-tree cursor or diagnostics layer.

### Freelist trunks

`FreelistTrunkView` decodes the next trunk pointer, leaf count, and leaf page
numbers. It accepts up to `usable_size / 4 - 2` leaves, matching modern
SQLite's read behavior. The encoder-side historical convention that leaves
the final six slots unused is not imposed on reads.

Zero leaf page numbers and invalid database page references are corruption.
Cross-trunk count reconciliation, duplicate detection, cycles, and agreement
with database-header freelist metadata are deferred to diagnostics because
they require multiple pages.

### Pointer maps

Pointer-map placement uses:

- `entries_per_map = usable_size / 5`;
- `pages_per_map = entries_per_map + 1`; and
- the pinned SQLite page-number formula, including moving a map page forward
  by one page if it would occupy the locking page.

Each five-byte entry contains a type and parent page. Types 1 through 5 are
accepted. Root and free-page entries require a zero parent; overflow and B-tree
child entries require a valid nonzero parent. Page 1, the locking page, and a
pointer-map page itself have no pointer-map entry.

The decoder does not decide whether auto-vacuum is enabled. Callers use the
database header's largest-root-page field to decide whether pointer maps
should exist.

### Error and ownership contract

- Invalid caller geometry, wrong buffer size, or page zero is `kMisuse`.
- Out-of-range cell, freelist leaf, or pointer-map target access is
  `kOutOfRange`.
- Malformed external bytes and invalid on-disk page references are
  `kCorruption`.
- Successful parsing and access allocate zero times.
- No decoder owns bytes, performs I/O, retains pager pins, or catches broad
  exceptions.

## Explicitly deferred behavior

- Pager integration and overflow-chain reads.
- Table and index cursor traversal, seek, ordering, and duplicate handling.
- Full-record assembly and record decoding across overflow pages.
- Tree depth, parent/child, separator-key, and leaf-depth validation.
- Complete page-occupancy maps and cell/freeblock overlap detection.
- Freelist totals, duplicate references, cycles, and unreachable pages.
- Auto-vacuum root ordering and whole-database pointer-map verification.
- Page mutation, allocation, balancing, defragmentation, and secure delete.
- Database text-encoding and schema-format policy.

## Validation plan

Tests cover:

- geometry boundaries for 512 through 65536-byte pages and reserved regions,
  including the 255/256-byte reserved-region boundary;
- all four B-tree page kinds on ordinary pages and page 1;
- every cell layout, signed rowid varints, minimum four-byte cells, and exact
  local-payload boundaries;
- overflow transitions where `K` equals, precedes, and exceeds `X`;
- truncated varints, payload overflow, invalid child/overflow page numbers,
  malformed cell offsets, and out-of-range indexes;
- empty pages, 65536 content-offset encoding, exact maximum and
  maximum-plus-one cell counts, maximum fragments, a first freeblock in the
  unallocated region, freeblock ordering, overlap, size, and total-free-space
  boundaries;
- overflow-page next pointers and usable payload spans;
- maximum freelist trunk occupancy and malformed leaf pointers;
- pointer-map page placement, locking-page displacement, all five entry types,
  and invalid parent/type combinations;
- pinned SQLite 3.54.0 page fixtures for table/index interior/leaf pages,
  overflow, freelist, and pointer maps; and
- deterministic mutation/fuzz loops under sanitizers for every decoder.

The optimized baseline records page-header parsing and representative local
and overflow cell decoding, including allocation and fixed-work counts.
Optimization requires a repeatable bottleneck before the cursor layer is
integrated.

## Consequences

- The next B-tree cursor node can traverse typed, bounds-checked page and cell
  views while retaining pager pins for ownership.
- The diagnostics node can reuse the same decoders and add cross-page
  integrity checks without duplicating persistent-format logic.
- Hot successful decodes remain allocation-free and perform no I/O.
- Corrupt external bytes are rejected before unchecked access, while expensive
  whole-tree validation remains outside the cursor hot path.
