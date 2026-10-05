# ADR-0043: SQLite-Compatible B-Tree Mutation

**Status:** Accepted

## Context

ADR-0023 provides allocation-free decoders for SQLite B-tree pages, cells,
overflow pages, freelist trunks, and pointer-map pages. ADR-0024 combines
those decoders with `Pager` to provide read-only table and index cursors.
ADR-0042 now provides rollback-mode writable pages, logical growth and
shrink, savepoints, spill, commit, rollback, and hot-journal recovery.

The next dependency-graph node must add the storage mutation required by
future transaction and DML/DDL layers:

- initialize an empty SQLite database image;
- create table and index roots;
- insert or replace table rows;
- insert index records;
- delete table and index entries;
- allocate, reuse, and free ordinary and overflow pages;
- split, merge, redistribute, deepen, and collapse B-trees;
- clear a tree while retaining its root; and
- drop a non-schema root.

Pinned SQLite 3.54.0 defines the relevant behavior across `src/btree.c` and
`src/btreeInt.h`:

- `src/btree.c:3540-3573` initializes page 1 and the database header.
- `src/btree.c:6546-7004` allocates and frees pages through the database
  freelist.
- `src/btree.c:7106-7290` encodes local and overflow-backed cells.
- `src/btree.c:7320-7530` edits page cells and defragments or rebuilds pages.
- `src/btree.c:8017-9230` deepens roots and balances sibling pages.
- `src/btree.c:9441-9735` inserts or replaces table and index entries.
- `src/btree.c:9873-10060` deletes leaf and interior entries and rebalances.
- `src/btree.c:10098-10480` creates, clears, and drops roots.
- `src/btree.c:10892-11190` checks page coverage, key ordering, overflow
  ownership, child depth, and page reachability.

The pinned implementation also includes shared-cache cursor preservation,
auto-vacuum page relocation, pointer maps, secure-delete modes, incremental
vacuum, bulk-load hints, and multiple insertion fast paths. Those policies do
not belong to this node. Modern SQLite is externally serialized, has no
connection-level transaction coordinator yet, and intentionally implements
ordinary rollback-mode databases before auto-vacuum and WAL.

The current module boundaries require the B-tree layer to own persistent
cell, overflow, freelist, balancing, and root policy while leaving page
capture, dirty state, spill, locking, and durability to `Pager`. The B-tree
writer must not reach into page-cache or journal internals.

This node is performance-sensitive. The applicable Modern LevelDB precedents
are:

- implement the smallest complete mechanism needed by current callers;
- preserve source-format and behavioral compatibility rather than copying
  reference implementation structure;
- establish validation at external and decoded-format boundaries, then trust
  documented internal invariants;
- reserve or grow reusable scratch before irreversible mutation;
- avoid whole-tree rebuilds for ordinary point mutation; and
- measure the completed successful path instead of justifying repeated work
  as defensive safety.

## Decision

### 1. Add one transaction-scoped write session with typed root handles

Add the public mutation surface to
`modern_sqlite/storage/btree/writer.hpp`:

```cpp
enum class BtreeInsertMode : std::uint8_t {
  kInsertOnly,
  kReplace,
};

struct BtreeDatabaseOptions {
  ByteCount reserved_bytes{0};
  DatabaseSchemaFormat schema_format = DatabaseSchemaFormat::kFour;
  DatabaseTextEncoding text_encoding = DatabaseTextEncoding::kUtf8;
};

class BtreeWriteSession final {
 public:
  static Result<BtreeWriteSession> Open(Pager& pager);

  bool requires_rollback() const noexcept;

  Status InitializeDatabase(BtreeDatabaseOptions options = {});
  Result<TableBtreeWriter> CreateTableBtree();
  Result<IndexBtreeWriter> CreateIndexBtree(
      std::span<const IndexColumnOrder> columns);
  Result<TableBtreeWriter> OpenTableBtree(PageNumber root_page);
  Result<IndexBtreeWriter> OpenIndexBtree(
      PageNumber root_page,
      std::span<const IndexColumnOrder> columns);
};

class TableBtreeWriter final {
 public:
  PageNumber root_page() const noexcept;
  bool requires_rollback() const noexcept;

  Status Insert(
      std::int64_t rowid, ByteView payload,
      BtreeInsertMode mode = BtreeInsertMode::kInsertOnly);
  Result<bool> Delete(std::int64_t rowid);
  Result<std::uint64_t> Clear();
  Status Drop();
};

class IndexBtreeWriter final {
 public:
  PageNumber root_page() const noexcept;
  bool requires_rollback() const noexcept;

  Status Insert(std::span<const SqlValue> values);
  Result<bool> Delete(std::span<const SqlValue> values);
  Result<std::uint64_t> Clear();
  Status Drop();
};
```

`BtreeWriteSession`, `TableBtreeWriter`, and `IndexBtreeWriter` are move-only
PImpl values. The root handles share ownership of the session's internal
mutation core and scratch while keeping table-rowid and index-record
operations unrepresentable on the wrong tree kind.

Exactly one B-tree write session may be opened for each admitted pager write
transaction generation. The future transaction coordinator owns that session
and hands out typed root handles. Opening a session requires an active pager
write transaction. Opening an existing root additionally requires a nonzero
root, a valid database header, and a root page of the expected kind.

ADR-0042's pager contract is extended with a monotonic
`write_transaction_generation()` accessor and a non-allocating
`ClaimWriteCoordinator()` operation. The claim succeeds at most once for each
generation that the pager explicitly admits. It is an independent latch, not
a property inferred from the generation value, and remains held across
generation changes caused by commit or rollback attempts even if every
session wrapper and root handle is destroyed. A second claim while latched
returns `kLocked`.
`BtreeWriteSession::Open()` completes all fallible validation and scratch
setup first, then claims the current generation as its final non-throwing
step. This makes the first session core the only B-tree mutation authority for
the transaction; destroying it requires successfully restoring a savepoint or
ending the write transaction and beginning another before a new session can
be opened.

The pager increments the generation, invalidating every existing session and
handle:

- when a write transaction begins;
- before a commit attempt enters any write phase;
- before a full rollback begins; and
- before savepoint rollback begins.

The generation changes even when a commit or rollback attempt later fails,
because pages or logical image state may already have changed. Savepoint
creation and release do not change it. Every session and root-handle operation
requires the captured generation to match and the pager still to be in the
same active write transaction. A mismatch is terminal `kSchemaChanged`.
Sessions and handles are never reused after commit, full rollback, or
savepoint rollback; callers open a new session after starting or restoring
the desired write transaction.

Generation invalidation is deliberately separate from coordinator-claim
release. A failed commit, full rollback, or savepoint rollback keeps the claim
and rollback-required state latched even though the old handles are invalid.
No new session can mutate the still-active transaction; the caller may only
retry pager commit/rollback cleanup. A successful commit or full rollback ends
the write transaction, and the next successful `BeginWrite()` clears the old
claim while advancing the generation again. A successful savepoint rollback
clears the claim only after playback and image restoration complete, allowing
one new session in the restored transaction generation.

The uniquely claimed shared session owns one rollback-required state. A
post-mutation failure through any handle prevents mutation through every
handle in that transaction, and the persistent pager claim prevents callers
from bypassing that state by destroying and reopening the session.
Create and drop also maintain per-page root-incarnation counters. A handle
captures its root's incarnation, and any successful drop invalidates all
other handles to that root before the page can be reused for a new tree.
This prevents a stale handle from mutating a different tree that later
occupies the same page number.

This node does not expose writable cursor positioning. Future VM operations
need point insert, replace, and delete, not a second public navigation API.
The existing read cursors remain the read and verification boundary.

### 2. Initialize only the ordinary rollback-mode database format

`BtreeWriteSession::InitializeDatabase()` requires:

- an active write transaction;
- a pager with zero logical pages;
- a valid pager page size;
- reserved bytes no greater than 255;
- usable size at least 480 bytes;
- schema format 1 through 4; and
- UTF-8 text encoding.

It allocates page 1, writes the 100-byte SQLite database header, and
initializes page 1 as an empty table leaf. The header publishes one logical
page, rollback read/write versions, the standard 64/32/32 payload fractions,
the requested schema format and text encoding, and an empty freelist.
The pager owns the change counter, version-valid-for value, and SQLite version
update at commit.

Existing databases are writable only when:

- the pager header and page geometry are valid;
- rollback read/write versions are active;
- `largest_root_page == 0`;
- `incremental_vacuum == 0`; and
- the database text encoding is UTF-8; and
- page 1 is a table B-tree root.

The auto-vacuum metadata checks exclude auto-vacuum and incremental-vacuum
databases. The text-encoding check excludes UTF-16 databases until record
encoding and index comparison support them. This node does not write pointer
maps, relocate roots, or transcode record text. Rejecting those images with
`kProtocol` before any page mutation is safer than silently producing an
inconsistent file.

Reserved-byte databases remain supported. B-tree content, overflow payload,
freelist trunks, and free-space calculations use the header's usable size;
the reserved suffix is never used for cells.

### 3. Keep page mutation decoded, validated, and locally rebuilt

The existing borrowed page and cell decoders remain the format-validation
boundary. The writer adds internal owning images for cells and mutable node
plans, but does not add a second public page format.

Before changing a page, the writer:

1. reads and parses the page;
2. validates every cell that the operation will move or replace;
3. copies the required raw cell encodings into reusable scratch;
4. computes the complete target layout and verifies that it fits; and
5. only then requests mutable page pins.

Touched pages are rebuilt into one reusable page-sized buffer and copied to
the writable page. Rebuilding:

- preserves the page-1 database header;
- writes the exact SQLite leaf or interior page type;
- emits a zero freeblock head and zero fragmented-byte count;
- writes sorted cell pointers;
- packs cell bodies from the end of usable space downward;
- writes the right-most child for interior pages;
- uses zero to encode a 65536-byte content offset; and
- leaves the reserved suffix outside the B-tree image.

This deliberately defragments each touched page. It avoids persistent
freeblock-edit complexity, produces complete page coverage accepted by
SQLite's integrity checker, and bounds work to the path and siblings involved
in one mutation.

### 4. Reuse SQLite's cell and overflow placement exactly

Table-leaf cells encode payload length, signed rowid, local payload bytes, and
an optional first-overflow pointer. Table-interior cells encode only a left
child and rowid separator. Index cells encode a record payload, with a left
child prefix on interior pages.

Local payload size uses ADR-0023's exact SQLite formula:

```text
K = M + ((P - M) % (U - 4))
local = K when K <= X, otherwise M
```

where `U` is usable size, `M` is minimum local payload, and `X` is the
page-kind maximum. A no-overflow cell is padded to SQLite's four-byte minimum.
Payloads above `0x7fffffff` return `kTooLarge`.

Overflow pages are allocated before the referencing cell is published. Every
overflow page stores the next-page number followed by up to `U - 4` payload
bytes; the final next pointer is zero. Existing overflow chains move between
leaf, interior, sibling, and parent cells by moving their local cell encoding,
not by copying the logical payload.

Replacing or deleting a cell frees exactly the number of overflow pages
implied by its logical and local payload sizes. Each next pointer is read and
validated before its page is returned to the freelist. Premature termination,
out-of-range references, locking-page references, or impossible chain length
is `kCorruption`.

### 5. Centralize ordinary freelist allocation and release

One internal allocator owns page-1 offsets 28, 32, and 36 and the freelist
trunk format.

Allocation follows pinned SQLite's ordinary `BTALLOC_ANY` path:

1. validate the header freelist count and first-trunk relationship;
2. when the first trunk has leaves, remove one leaf pointer;
3. when the first trunk has no leaves, remove and reuse the trunk itself;
4. otherwise reject append with `kTooLarge` when the next page would exceed
   SQLite's maximum page number `0xfffffffe`;
5. otherwise append through `Pager::AllocatePage()`; and
6. publish the pager's new logical page count in page 1.

Freeing a page:

1. increments the page-1 freelist count;
2. appends the page to the first trunk when it has fewer than
   `usable_size / 4 - 8` leaves; or
3. initializes the freed page as a new first trunk pointing to the previous
   first trunk.

The `-8` capacity preserves SQLite's long-standing backward-compatible trunk
limit. Reads still accept up to the format maximum `usable_size / 4 - 2`.

Page 1 and the locking page are never placed on the freelist. References must
be within the current logical image. This node deliberately does not implement
nearby/exact allocation, tail truncation, secure delete, pointer-map updates,
or SQLite's no-content bitset. Reused pages are read and journaled through
`Pager::WritePage()` before overwrite. That costs an avoidable read in some
cases but preserves rollback correctness without speculative allocator state.

### 6. Search without retaining pins across mutation

Mutation search stores a fixed path of at most `kMaximumBtreeDepth` frames.
Each frame contains only a page number and selected child slot. Page pins are
released before a page on that path is requested writable, so read pins never
alias write pins.

Table search binary-searches signed rowids and always descends to a leaf.
An exact row may be replaced only in `kReplace` mode; `kInsertOnly` returns
`kConstraint`.

Index writers copy and validate `IndexColumnOrder` metadata at open. Insert
and delete accept typed values, encode one SQLite record with the database
schema format, and compare existing records through the same collation,
direction, and null-placement contracts as `IndexBtreeCursor`. The value
count must equal the copied column count. An exact duplicate returns
`kConstraint`; deleting a missing record returns `false`.

Overflow-backed index records use a reusable payload buffer. Before growing
that buffer, the writer applies the same current-page-count feasibility bound
as the read cursor.

### 7. Split overfull pages bottom-up

An insertion or replacement first rebuilds its target page. If the page fits,
no structural page is touched.

An overfull non-root page is split into the original left page and one newly
allocated right page. The split boundary minimizes used-byte imbalance while
keeping both pages nonempty and valid.

- A table leaf retains every row in a child. The parent divider is the
  greatest rowid retained on the left page.
- An index leaf promotes one complete record out of the children.
- A table or index interior page promotes one divider and partitions its
  child pointers around that divider.

The divider is inserted into the parent at the recorded child slot. The left
page keeps the original page number, the new right page becomes the following
child, and an overflowing parent is split in the same way. This continues to
the root.

An overfull root keeps its page number. Two child pages are allocated, the
root's logical contents are divided between them, and the root is rebuilt as
an interior page with one divider. This is the Modern equivalent of SQLite's
deepen-then-balance path.

Insertion does not redistribute across already valid siblings. Byte-balanced
half-page splits provide bounded `O(log N)` mutation without importing
SQLite's three-sibling and bulk-load optimization machinery. Sibling
redistribution remains part of deletion, where it is required to prevent
empty or excessively sparse pages.

### 8. Merge or redistribute sparse pages after deletion

Deletion rebalances when a non-root page is empty or more than two-thirds
free, matching SQLite's balance trigger.

The writer selects one adjacent sibling, copies both sibling images and their
parent divider into scratch, and computes one of two outcomes:

- merge into one page and free the right page when the combined image fits;
  or
- redistribute into two byte-balanced pages and replace the parent divider.

For table leaves, the old parent divider is not row data and the replacement
is recomputed from the left page's greatest rowid. For index leaves and all
interior pages, the parent divider participates in the ordered cell stream and
one new divider is promoted.

Sparse-parent repair proceeds bottom-up. A root with no cells and one child is
collapsed by copying the child into the root and freeing the child. If page 1
cannot hold the child because of its 100-byte database header, page 1 remains
SQLite's valid zero-cell virtual table root pointing to that child.

Deleting an index record stored on an interior page uses SQLite's predecessor
rule while explicitly transferring overflow ownership:

1. copy and decode the greatest record from the entry's left subtree before
   mutation;
2. replace the interior entry with that predecessor while preserving the
   original entry's left-child pointer;
3. transfer the predecessor overflow chain to the replacement and free only
   the removed original entry's overflow chain;
4. if the larger replacement overflows its interior page, split that page
   upward before proceeding;
5. seek the installed predecessor, descend through its left child, and select
   the right-most equal leaf occurrence;
6. remove that leaf occurrence without freeing the transferred overflow
   chain; and
7. balance the leaf depth and then any still-sparse affected ancestor depths.

The temporary duplicate exists only inside the externally serialized
operation. The installed occurrence remains an interior entry even when its
page splits or the entry is promoted, so its left subtree still identifies
the leaf occurrence being removed. Every failure after step 2 requires pager
rollback. This preserves SQLite's predecessor semantics without retaining
stale page pins or requiring an unserializable overfull page image.

Table interior separators are upper bounds, not logical rows. A
non-structural delete may leave a separator larger than the new maximum of its
left subtree; that remains valid because it is still smaller than every row
in the following subtree. Structural merge and redistribution recompute
affected separators.

### 9. Clear and drop iteratively

`Clear()` performs a bounded-depth post-order traversal:

- validate every visited page kind and depth;
- free every cell's overflow chain;
- free every non-root child page; and
- rebuild the retained root as an empty leaf of the same tree kind.

It returns the number of logical entries removed. Table counts include only
leaf rows; index counts include leaf and interior records.

`Drop()` first clears the tree, then returns its root page to the freelist.
Dropping page 1 is `kMisuse`. Successful drop makes the writer terminal.
Ordinary non-auto-vacuum mode never relocates another root and therefore has
no `piMoved` result.

Traversal uses the 20-frame SQLite depth limit and never recursively consumes
the C++ call stack. Duplicate child references, cycles, page-kind changes, or
out-of-range references are corruption. A corruption found after mutation
requires transaction rollback.

### 10. Make mutation failure state explicit

Public allocation boundaries translate `std::bad_alloc` to
`ErrorCode::kOutOfMemory`.

Each operation distinguishes pre-mutation failures from failures after the
first persistent page or allocator change.

- `kConstraint`, missing delete results, invalid arguments, failed searches,
  and scratch-growth OOM before mutation leave the writer retryable.
- Once page allocation, overflow creation, freelist mutation, or page rebuild
  begins, any later error marks the shared session and every handle
  `requires_rollback()`.
- A rollback-required session or handle returns its first error from every
  later mutating call.
- The caller must roll back the pager transaction or an enclosing pager
  savepoint and open a new writer.

This node does not create an implicit savepoint around every point operation.
Statement atomicity and automatic savepoint policy belong to the next
transaction-coordinator node. Tests use explicit pager rollback to prove that
every injected mid-mutation failure restores the original image.

No destructor performs I/O, allocation, page mutation, or rollback.

### 11. Keep scratch reusable and mutation work bounded

The shared writer core owns reusable:

- page-rebuild bytes;
- copied-cell bytes;
- cell descriptors;
- encoded index-record bytes;
- overflow/index payload bytes; and
- split/redistribution plans.

Every operation grows the exact required scratch before its first persistent
mutation. Later page rebuild and balancing steps perform no heap allocation.
Scratch capacity is retained across operations.

`compared_overflow_page_visits` counts every overflow page read while
materializing existing index records selected by binary search, including a
repeat visit to the same page. `mutated_overflow_page_visits` counts every
overflow page allocated, written, traversed, transferred, or freed for the
inserted, replaced, or deleted cell. Table rowid search has no comparison
overflow term.

The measurable initial contracts are:

- cached local table insert, replace, and delete allocate zero after scratch
  warm-up when no split or overflow allocation is required;
- moving an existing overflow-backed cell during balance does not copy or
  allocate its logical payload;
- point insert/delete performs
  `O(depth + compared_overflow_page_visits + mutated_overflow_page_visits)`
  page visits, plus at most one B-tree sibling and the required freelist
  metadata per deletion level;
- no point operation scans or rebuilds the whole tree;
- freelist reuse precedes physical append; and
- every comparator, page read/write, split, merge, overflow page, and
  freelist page is observable through existing instrumentation and VFS
  counters.

SQLite's quick right-edge split, three-sibling redistribution, nearby page
allocation, direct same-size overwrite, and bulk-load hints are deferred until
the write-performance baseline identifies a material gap.

## Test Strategy

Implementation proceeds red-first in the following groups.

### Database, root, and freelist format

- empty-database initialization at every supported page size;
- reserved-byte geometry;
- rejection of nonempty initialization, invalid metadata, auto-vacuum, and
  UTF-16LE/UTF-16BE mutation before any page change;
- table and index root creation from append and freelist reuse;
- SQLite-compatible freelist trunk growth, trunk reuse, and count updates;
- append rejection at SQLite's `0xfffffffe` maximum page number;
- clear retains an empty root;
- drop frees non-page-1 roots and rejects page 1; and
- corruption in freelist counts, trunks, leaves, and page references.

Session-lifetime tests cover generation invalidation after commit, full
rollback, and savepoint rollback; shared rollback-required state across
handles; rejection of simultaneous or reopened sessions in one generation;
inability to bypass a post-mutation failure by destroying the session; and
stale-handle rejection after drop followed by same-page root reuse.
Failed commit, full-rollback, and savepoint-rollback attempts must retain the
coordinator claim and reject a new session; successful savepoint rollback and
the next write transaction must admit exactly one new session.

### Table mutation model

- insert-only and replace semantics;
- signed rowid ordering, including minimum and maximum values;
- front, middle, and append insertion;
- page split, recursive interior split, and root deepening;
- local and multi-page overflow payloads at every placement boundary;
- delete missing, first, middle, last, and interior-boundary rows;
- sibling redistribution, merge, recursive parent repair, and root collapse;
- clear and root reuse; and
- randomized operation sequences compared with `std::map`.

Every model checkpoint reopens `TableBtreeCursor`, verifies ordered rowids and
payload bytes, and checks freelist/page-count invariants.

### Index mutation model

- ascending and descending columns;
- null placement;
- binary, no-case, and RTRIM collations;
- integer, real, text, blob, and null fields;
- exact duplicate rejection;
- leaf and interior insertion/deletion;
- predecessor replacement of interior entries;
- local and overflow-backed records;
- overflow-backed records selected as binary-search comparison pivots;
- recursive split, redistribution, merge, and collapse; and
- randomized operation sequences compared with a reference ordered model.

Every checkpoint reopens `IndexBtreeCursor` and verifies exact record order
and payload bytes.

### Failure, OOM, and allocation

- failure before and after every page allocation, overflow write, freelist
  change, page rebuild, split, merge, clear, and drop boundary;
- exact retryable versus rollback-required writer state;
- full pager rollback restores the byte-identical original image;
- exhaustive allocation failure at session/root-handle open, scratch growth,
  record encoding, payload copy, split planning, clear traversal setup, and
  root creation;
- zero-allocation warmed local table insert/replace/delete; and
- no logical-payload allocation when balance moves an existing overflow cell.

### SQLite interoperability

A conditional pinned-SQLite 3.54.0 executable will:

1. create rollback-mode databases, rowid tables, and indexes through SQLite
   at multiple page sizes;
2. obtain their root pages from `sqlite_schema`;
3. mutate both trees through Modern SQLite;
4. commit and reopen through SQLite;
5. verify rows and index-backed queries; and
6. require `PRAGMA integrity_check` and `PRAGMA freelist_count` to match the
   expected model.

Separate cases cover overflow payloads, enough inserts for multi-level trees,
deletes that merge pages, clear, freelist reuse, and rollback after injected
failure.

A second interoperability family begins with an empty pager and uses only
Modern SQLite to:

1. initialize page 1;
2. create table and index roots;
3. encode and insert the corresponding `sqlite_schema` rows into page 1;
4. populate table records and index records;
5. commit the generated database; and
6. open and modify it through pinned SQLite.

The schema-table records describe a rowid table and a conventional index, so
SQLite can reach every Modern-created root. Pinned SQLite must query both the
table and index successfully, modify them, and report `PRAGMA integrity_check
= 'ok'`. This covers Modern header, schema-root, ordinary-root, table-cell,
index-cell, overflow, and freelist encoders without accepting unreachable
test-only pages.

## Consequences

### Positive

- Future transaction and DML/DDL layers receive typed point-mutation
  primitives without importing page layout or journal policy.
- Generated table, index, overflow, and freelist bytes remain readable by
  SQLite.
- Mutation stays local and logarithmic instead of rebuilding whole trees.
- Page rebuilding removes persistent fragmentation complexity from the first
  writable implementation.
- Split and deletion balancing preserve equal leaf depth and nonempty
  non-root pages.
- Explicit rollback-required state prevents continued mutation after a
  partially applied storage operation.
- Reusable scratch makes allocation and OOM boundaries deterministic.

### Negative

- The mutation core is substantially more complex than the read cursor.
- Index interior deletion requires two searches.
- Rebuilding touched pages copies up to one page even for a small local edit.
- Split-only insertion can produce lower occupancy than SQLite's
  three-sibling balancing.
- Reused freelist pages incur conservative read/journal work.
- Auto-vacuum databases remain read-only.

### Deferred

- auto-vacuum, incremental vacuum, pointer maps, root relocation, and tail
  truncation;
- secure-delete and fast-secure-delete modes;
- shared-cache cursor preservation and concurrent writers;
- writable cursor positioning and incremental-blob mutation;
- UTF-16 record encoding, index comparison, and mutation;
- nearby/exact freelist allocation and no-content tracking;
- SQLite quick-balance, three-sibling insert redistribution, bulk-load hints,
  and in-place same-size overwrite;
- statement-level automatic savepoints; and
- SQL schema, DML, trigger, foreign-key, and index-maintenance policy.

## References

- SQLite 3.54.0 `src/btree.c`
- SQLite 3.54.0 `src/btreeInt.h`
- SQLite database file format
- `docs/adr/0023-btree-page-decoding.md`
- `docs/adr/0024-read-only-btree-cursors.md`
- `docs/adr/0042-rollback-mode-writable-pager.md`
- Modern LevelDB ADR-0010, Need-Driven Simplicity
- Modern LevelDB ADR-0060, LevelDB Write-Path Parity
