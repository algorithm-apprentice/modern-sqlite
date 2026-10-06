# ADR-0044: Reference-Faithful SQLite B-Tree Mutation

**Status:** Accepted

## Context

ADR-0043 selected a custom point-mutation design built around owning node
images, independent two-way or three-way splits, persistent freelist ownership
hashes, and reusable polymorphic-allocation scratch.

Implementation and review exposed a repeated class of design errors:

- pager, savepoint, and B-tree failure state had been split across independent
  mechanisms;
- page ownership was enforced through new caches rather than SQLite's
  transaction and integrity-check mechanisms;
- delete balancing and root depth had been reconstructed from mutable paths;
- allocation-failure retry depended on standard-library allocator
  implementation behavior; and
- the custom split planner diverged materially from SQLite's proven
  three-sibling balancing state machine.

The first writable implementation must minimize algorithmic invention. The
pinned SQLite 3.54.0 source is therefore the implementation oracle, not only
the file-format oracle.

The relevant reference mechanisms are:

- `newDatabase()` at `src/btree.c:3540-3573`;
- transaction rollback and statement savepoints at
  `src/btree.c:4476-4635`;
- `allocateBtreePage()` at `src/btree.c:6546-6867`;
- `freePage2()` at `src/btree.c:6868-7004`;
- overflow release and cell construction at `src/btree.c:7005-7290`;
- page-local cell editing at `src/btree.c:7320-8050`;
- `CellArray`, `balance_nonroot()`, `balance_deeper()`, and `balance()` at
  `src/btree.c:7555-9300`;
- `sqlite3BtreeInsert()` at `src/btree.c:9441-9740`;
- `sqlite3BtreeDelete()` at `src/btree.c:9873-10060`;
- root creation, clear, and drop at `src/btree.c:10098-10480`; and
- integrity ownership checking at `src/btree.c:10694-11310`.

SQLite's source architecture cannot be copied directly:

- Modern SQLite has typed `Result` values instead of connection-global error
  flags;
- the pager and page cache expose move-only RAII pins;
- shared-cache cursor coordination, triggers, auto-vacuum, WAL, and the VDBE
  remain outside this node; and
- owning raw pointers are forbidden.

The algorithm, state transitions, persistent ordering, bounded constants, and
cleanup structure can still be reproduced faithfully through Modern C++ types.

## Decision

### 1. Supersede ADR-0043's custom mutation algorithm

ADR-0043 is superseded for implementation details.

The following ADR-0043 decisions are removed from the first version:

- independent split planning for only the overflowing page;
- the custom two-way/three-way split optimizer;
- persistent freelist ownership and root-liveness hash scans on every point
  operation;
- whole-tree clear preplanning;
- a session-wide polymorphic pool reused after allocation failure; and
- predecessor deletion through a second root search.

The public typed writer API remains valid:

- `BtreeWriteSession`;
- `TableBtreeWriter`;
- `IndexBtreeWriter`;
- point insert, replace, and delete;
- create, clear, and drop; and
- explicit rollback-required reporting.

These values become facades over an internal SQLite-style writable cursor and
page-balancing engine.

The replacement is developed on a fresh main-based branch. The superseded
custom writer is not copied, compiled, or exercised by the replacement PR.
Reusable pager/cache contracts and reference-faithful internal primitives may
be migrated after review, but their test fixtures must not depend on the
rejected writer. Historical ADR-0043 remains only as decision traceability.

### 2. Add an internal writable cursor with SQLite's fixed path model

Each mutation opens one internal writable cursor.

The cursor owns:

- the root page number and tree kind;
- a current page pin;
- at most `kMaximumBtreeDepth - 1` parent frames;
- one selected child index per parent;
- current cell metadata;
- fixed cursor state equivalent to SQLite's valid, invalid, require-seek, and
  fault states; and
- the immutable full tree depth measured during the initial seek.

The cursor path is the mutation path. Insert, delete, root deepening, sibling
balancing, and root collapse update that path directly. A consumed path is
never reconstructed from an independent search result.

The typed public writers do not expose cursor positioning. They create,
operate, and close the internal cursor inside one public call.

### 3. Give each mutation one bounded page owner

Every mutation owns touched page pins through one `MutationPageOwner`.

It contains:

- the cursor's current and parent frames;
- one optional page-1 frame;
- fixed arrays for the at-most-three old and five new balance siblings; and
- temporary overflow-page pins.

Each owned page also retains its fixed page-local staged-cell slots. Reopening
a mutable facade from a cursor frame therefore preserves staged ancestor
dividers, while releasing the owner slot clears those borrowed views.

The owner performs a bounded linear lookup by page number. It does not use a
hash table.

Helpers receive borrowed mutable-page facades from this owner:

- freelist allocation and release borrow the already-held page-1 facade;
- root operations borrow page 1 when it is both database header and B-tree
  root;
- `freePage2()` borrows the current/sibling facade when the released page is
  already held; and
- a new pin is acquired only when the operation owner does not already contain
  that page.

One page number has at most one owned pin in the operation. A sibling or child
alias to a cursor-path page is corruption. An unrelated external pin that
prevents exclusive access remains `kBusy`.

### 4. Add a pager pin-promotion seam

SQLite retains `MemPage` references and calls `sqlite3PagerWrite()` on the
same page object.

Modern SQLite will add a narrow equivalent:

```cpp
Result<WritePagePin> Pager::WritePage(ReadPagePin&& pin);
```

The operation:

1. validates the page and active write state;
2. verifies that the consumed pin is the only pin for that cache entry;
3. converts that same cache pin to exclusive in place without decrementing its
   reference count or enforcing cache capacity;
4. journals the original sector before mutable access;
5. marks it dirty; and
6. returns one `WritePagePin`.

Promotion fails with `kBusy` if another pin still references the page.
The cache entry, frame address, and borrowed byte views remain stable
throughout promotion. If journal capture fails, destruction of the local
exclusive pin releases the frame and no mutable view is published.

Cursor parent frames may remain read-pinned while a different page is
promoted. Before a parent itself is edited, its frame is consumed and promoted.

The page cache adds an allocation-free private operation that changes a sole
shared `Pin` to exclusive in place. It never releases and reacquires the entry,
so capacity-zero caches cannot evict the promoted frame.

### 5. Add allocation-free cache rekeying

Add an internal page-cache operation:

```cpp
Status PageCache::RekeyExclusiveForPager(Pin& pin, PageNumber new_page);
```

It requires:

- one exclusively pinned dirty frame;
- no existing cache entry for `new_page`;
- a nonzero target page number; and
- externally serialized access.

The cache uses C++17 unordered-map node extraction:

1. extract the existing map node;
2. change its key;
3. update `PageFrame::page_number`;
4. reinsert the same node; and
5. preserve the `Entry` address, pin object, dirty lists, and byte views.

No allocation occurs. The operation is private to `Pager`; callers cannot use
the locking page as a persistent cache identity.

Pager exposes one bounded permutation seam:

```cpp
struct PageNumberRekey {
  WritePagePin* pin;
  PageNumber final_page;
};

Status Pager::PermutePageNumbers(std::span<const PageNumberRekey> pages);
```

It validates that:

- the span contains at most five distinct exclusively pinned dirty frames;
- final page numbers are distinct, nonzero, non-locking ordinary page numbers;
- current and final page-number sets are the same permutation; and
- rollback-journal and every active-savepoint image has already been captured
  for each physical participant.

Pager then performs all cycles internally using the locking-page number as a
temporary cache-key sentinel. No caller observes or can persist the sentinel
identity. Journal membership remains keyed by physical page number and is not
moved. All final `PageFrame` and mutable-page page numbers are synchronized
before the function returns.

### 6. Model SQLite's mutable `MemPage` state explicitly

Add an internal mutable page facade over one page pin.

It stores validated derived metadata equivalent to SQLite `MemPage`:

- table/index and leaf/interior flags;
- page number and header offset;
- child-pointer size;
- local-payload limits;
- cell count, free-byte count, and content offsets;
- exactly four overflow-cell slots; and
- the insertion index for each staged overflow cell.

The staged overflow cells are transaction-local pointers into owned operation
buffers. They are never persistent page bytes and never survive the public
operation.

`insertCell()` behavior is reproduced:

- if the cell fits, journal the page, allocate page-local space, copy the cell,
  and insert its pointer;
- otherwise, copy when required and stage the cell in the fixed overflow
  slots; and
- parent overflow cells remain sorted, adjacent, and bounded by SQLite's
  invariant.

`dropCell()`, `freeSpace()`, `allocateSpace()`, `defragmentPage()`,
`editPage()`, and `rebuildPage()` follow the pinned algorithms and corruption
checks.

### 7. Use SQLite-style temporary memory lifetimes

The write session owns two retained standard buffers:

- one `page_size + 4` cell-formatting buffer corresponding to
  `BtShared.pTmpSpace`; and
- one page-sized pager rebuild buffer corresponding to
  `Pager.pTmpSpace`.

They are allocated when the first writable cursor is opened and remain owned
by the session.

Balancing uses separately typed operation-local storage:

```cpp
struct CellLocator {
  std::uint32_t offset;
  std::uint16_t size;
  std::uint8_t source_slot;
  std::uint8_t flags;
};
static_assert(sizeof(CellLocator) == 8);
```

`source_slot` identifies one pinned sibling or copied-divider scratch.
Staged-cell locators use a separate fixed source array that borrows the
operation-owned staged buffers without consuming divider scratch. `offset` is
relative to the selected stable byte buffer. `flags` distinguishes borrowed
page bytes, copied dividers, and borrowed staged cells.

The operation owns:

```text
max_cells * sizeof(CellLocator)
+ page_size
```

as a typed `std::vector<CellLocator>` plus one page-sized `ByteBuffer` for
divider copies. Cell sizes live in `CellLocator`; no C-style typed overlay is
used. The maximum-cell formula is recomputed from SQLite's `MX_CELL`, the
three-sibling bound, divider count, and four overflow slots, and the total must
remain no larger than seven pages.

An additional page-sized parent-overflow buffer may live until the next upward
`balance_nonroot()` call copies its staged divider cells. It is then released.

The retained `pTmpSpace` base allocation is exactly `page_size + 4` bytes. The
first four bytes are initialized and reserved for prepending an interior child
pointer; the exposed formatting view begins at byte four and never moves.

Pager rebuild scratch is exactly one page plus any decoder-required fixed
overrun padding documented by ADR-0023.

No general-purpose PMR pool or typed byte overlay is used. No allocator object
is reused after an allocation failure. Every operation-local allocation is
destroyed through one cleanup path before the error is returned. Every staged
overflow slot is cleared before its owning `pSpace` buffer is released.

### 8. Reproduce SQLite freelist mutation ordering

The first version does not cache the whole freelist in a hash set.

`allocateBtreePage()`:

1. validates header count and first-trunk consistency;
2. journals page 1 and decrements the free count;
3. walks at most the declared number of trunk pages;
4. reuses an empty trunk or one leaf entry;
5. journals every modified trunk or predecessor trunk;
6. otherwise appends a page and publishes the page count; and
7. skips the locking page.

`freePage2()`:

1. journals page 1 and increments the free count;
2. validates and reads the first trunk when present;
3. appends a leaf while below SQLite's historical `usable_size / 4 - 8`
   limit; or
4. journals the released page and installs it as the new first trunk.

The operation releases every page pin through one cleanup path.

Ordinary non-auto-vacuum `BTALLOC_ANY` is implemented first. Exact, less-than,
pointer-map, root relocation, and secure-delete branches remain deferred with
their source mapping documented.

### 9. Add the transaction-scoped `pHasContent` equivalent with a Bitvec

Implement a page-number Bitvec equivalent to SQLite's `src/bitvec.c` as part
of Pager's outer write-transaction state.

It records pages that:

- became freelist leaf pages in the current transaction; and
- may be reused before that transaction ends.

The bit controls whether freelist reuse may use a no-content pager
optimization or must read and journal the prior bytes.

Pager exposes narrow operations to mark a page as requiring original content
and to request reusable-page access. The B-tree layer supplies page numbers;
Pager owns and queries the Bitvec.

The Bitvec:

- supports page numbers through SQLite's maximum;
- uses fixed-size bitmap/hash nodes and recursive subdivision;
- returns `kOutOfMemory` without changing previously set bits;
- survives savepoint release and savepoint rollback; and
- is destroyed only after commit or full transaction rollback; and
- is not a general page-ownership index.

Bits are conservative outer-transaction history. A savepoint rollback may
remove later frees, but retaining those bits only disables the no-content
optimization. Clearing them could lose transaction-original bytes for pages
freed before the savepoint.

Because Pager owns the Bitvec, it survives B-tree session destruction,
generation invalidation, and savepoint rollback. It is reset only by
successful outer commit/full rollback cleanup or the next outer write
transaction initialization.

Until the pager no-content seam is implemented, the allocator conservatively
reads and journals reused pages. The Bitvec contract and tests are still added
with the B-tree node so the later optimization does not alter transaction
correctness.

### 10. Format cells through retained `pTmpSpace`

`fillInCell()` is reproduced:

1. encode the child prefix, payload length, and rowid or index record;
2. apply SQLite's exact local-payload formula;
3. pad no-overflow cells to four bytes;
4. write local payload into retained `pTmpSpace`;
5. allocate overflow pages one at a time;
6. publish each new page number only after allocation succeeds;
7. initialize every next pointer to zero before continuing; and
8. release the current overflow page on every exit.

Allocated overflow pages are not manually undone on error. The caller's
statement savepoint or full transaction rollback restores the operation.

`clearCellOverflow()` reads the next pointer before freeing the current page,
validates range and aliasing, and calls `freePage2()` one page at a time.

### 11. Reproduce `balance()` as the only structural mutation path

After local insert or delete:

1. stop when there are no staged overflow cells and free space is at most
   two-thirds of usable space;
2. call `balance_deeper()` for an overfull root;
3. use `balance_quick()` only for SQLite's exact right-edge table-leaf case;
4. otherwise call `balance_nonroot()` on the parent and up to three old
   siblings;
5. clear the processed page's overflow count;
6. release the child frame and move the cursor one level upward; and
7. continue until the root is valid or an error occurs.

`balance_nonroot()` reproduces:

- SQLite's neighbor selection with `NN = 1` and `NB = 3`;
- parent-divider removal before CellArray assembly;
- leaf-data and leaf-correction behavior;
- one CellArray ordered across old siblings and parent dividers;
- cached cell-size computation;
- the left-biased initial packing;
- the required right-to-left balancing adjustment;
- reuse of old pages before allocation of new pages;
- ascending page-number reassignment through the required pager rekey seam;
- divider reconstruction, including tiny leaf padding and canonical
  table-rowid varints (unlike `balance_quick()`'s raw rowid preservation);
- dependency-safe two-pass page editing;
- root shallowing; and
- freeing surplus old sibling pages.

### 12. Reproduce SQLite insert behavior

Insertion:

1. seeks with the writable cursor;
2. validates table/index type and exact duplicate policy;
3. when replacing a cell with the same logical payload size, overwrites local
   bytes and the existing overflow chain in place without allocation;
4. otherwise formats the new cell into retained `pTmpSpace`;
5. applies SQLite's later equal-local-cell-size direct copy optimization when
   legal;
6. otherwise clears old overflow, drops the replaced cell, and inserts or
   stages the new cell;
7. calls `balance()` only when staged overflow cells exist; and
8. clears staged overflow and invalidates the cursor on every failed balance.

Modern `kInsertOnly` returns `kConstraint` before mutation when the seek is
exact. `kReplace` follows the reference overwrite/drop path.

An overflow-backed same-size replacement retains the exact overflow page
numbers and freelist state.

### 13. Reproduce SQLite delete behavior

Deletion keeps the original cursor stack.

For an interior index entry:

1. record the original cell depth and index;
2. move the cursor to the predecessor leaf;
3. journal and clear the original interior cell;
4. copy the predecessor cell through retained `pTmpSpace`;
5. insert it into the original interior page with the preserved child page;
6. drop the predecessor leaf cell;
7. balance the predecessor leaf first; and
8. if propagation did not already repair the original level, restore the
   cursor to that level and balance it.

No second root search is used. Overflow ownership moves with the cell bytes,
matching the reference algorithm.

### 14. Reproduce root create, clear, and drop

Database initialization follows `newDatabase()`.

Ordinary root creation allocates one page and calls the modern equivalent of
`zeroPage()` with table-leaf or index-leaf flags.

Clear follows `clearDatabasePage()`:

- recurse with a depth value capped at 20;
- visit and clear child pages;
- release each cell's overflow chain;
- free non-root pages immediately;
- count table leaf rows and all index cells; and
- journal and zero the retained root.

Drop clears and then frees a non-page-1 root.

Modern root-handle incarnations remain as a facade safety mechanism, but they
use standard containers and do not alter the reference page algorithm.

### 15. Keep integrity ownership checking out of the point path

Point mutation performs the local corruption checks used by the reference
operation.

Global duplicate and orphan ownership is verified by:

- the storage diagnostics module;
- a dense one-bit-per-page integrity-check workspace;
- pinned SQLite `PRAGMA integrity_check`; and
- focused malformed-image tests.

The point path does not build a whole-database ownership hash before ordinary
mutation.

### 16. Let the transaction coordinator provide statement atomicity

B-tree functions:

- return the first typed error;
- release all pins and operation-local scratch;
- clear page-local overflow slots when required; and
- mark the pager transaction rollback-required after persistent mutation.

The mutation owner records the operation-start checkpoint. `balance()` uses
that checkpoint, rather than a checkpoint taken after the local edit, so a
later allocation or I/O failure cannot leave an earlier insert/delete change
committable.

They do not retry through a failed allocator arena.

Node 37 wraps statement execution in a pager savepoint, matching
`sqlite3BtreeBeginStmt()`. A statement error rolls back that savepoint. Until
Node 37 exists, direct Node 36 tests use explicit savepoints or full rollback
for every injected post-mutation failure.

Commit and full rollback refresh cursor generations, clear transaction Bitvec
state, and invalidate stale writable cursors. Savepoint rollback refreshes
cursor generations and invalidates stale writable cursors but preserves the
outer-transaction Bitvec.

### 17. Preserve strict Modern C++ ownership

Reference pointers become:

- move-only pager pins;
- fixed arrays of optional cursor frames;
- borrowed spans valid only while their source pins live;
- owned byte buffers for copied divider and staged overflow cells;
- standard vectors for retained buffers; and
- typed page numbers and checked sizes.

No owning raw pointer is introduced.

All comments and tests identify the pinned SQLite symbol being mirrored.

## Test Strategy

Implementation proceeds red-first in reference order.

### Pager and cursor seams

- consume-read-pin promotion to write pin;
- sole-pin and competing-pin behavior;
- capacity-zero cache promotion without eviction or file re-read;
- page-1 root and freelist header sharing one operation-owned pin;
- current/sibling page borrowing by freelist release;
- cursor stack at depths 1 and 20;
- write-promotion journal ordering;
- cursor invalidation after balance and rollback; and
- allocation-free three-step page rekey with stable pins and byte views.
- permutation rejection before journal/savepoint capture and locking-sentinel
  cleanup after every cycle;

### Temp space and cells

- first writable-cursor `pTmpSpace` allocation and OOM;
- exact cell header, local payload, and four-byte padding vectors;
- local and overflow-backed equal-payload overwrite with unchanged page
  allocation;
- overflow allocation and pointer publication cuts;
- clearCell overflow traversal and partial-failure rollback;
- fixed page overflow slots and ordering; and
- no temp pointer surviving operation teardown.

### Freelist and Bitvec

- SQLite trunk and leaf selection vectors;
- free-count ordering;
- historical `-8` trunk limit;
- append and locking-page skip;
- free-then-reuse in one transaction;
- free before savepoint, savepoint rollback, reuse, then full rollback to
  byte-identical original content;
- Bitvec bitmap, hash, subdivision, clear, and OOM; and
- rollback after every page-1, trunk, and reused-page boundary.

### Page editing

- allocateSpace/freeSpace and freeblock coalescing;
- insertCellFast and insertCell equivalence;
- editPage fast path and rebuild fallback;
- overlapping source corruption;
- tiny index divider handling; and
- page-1 header offset behavior.

### Balancing

- exact sibling selection for left, middle, and right children;
- CellArray ordering for all four page kinds;
- one through five output siblings;
- quick-balance append;
- root deepen and shallow;
- page-number ordering;
- dependency-safe edit order;
- deletion underflow and insertion overflow;
- 20-level maximum depth; and
- deterministic OOM/I/O failure at every cleanup label.

### Insert, delete, clear, and drop

- table insert-only and replacement;
- index duplicate rejection;
- same-size overwrite;
- interior predecessor transfer without a second search;
- leaf-first and interior-second delete balance;
- recursive clear counts;
- root reuse and stale handles; and
- explicit statement/full rollback restoration.

### Interoperability and differential structure

- SQLite-created/Modern-mutated and Modern-created/SQLite-mutated databases;
- page sizes 512 through 65536 and reserved bytes;
- `PRAGMA integrity_check` and freelist count;
- decoded page topology after selected operations;
- reference operation traces for quick, non-root, deeper, and shallower
  balance cases; and
- matched work counters before performance claims.

### Crash recovery

Direct Node 36 crash matrices use explicit pager transactions and forced spill.
They enumerate every journal/database mutation and sync cut for:

- local and overflow insert;
- quick balance;
- non-root three-sibling balance;
- root deepening and shallowing;
- interior predecessor delete;
- freelist reuse;
- clear; and
- drop.

Reopening through Modern SQLite and pinned SQLite must produce either the
complete old transaction or complete committed transaction. Both outcomes
must pass integrity checking and match the expected freelist.

Node 37 adds statement-savepoint crash behavior; it does not replace these
storage-transition tests.

## Consequences

### Positive

- The first writable implementation follows a production-proven algorithm.
- Source review, test vectors, and failure ordering remain traceable to pinned
  SQLite symbols.
- Allocator and scratch lifetimes are explicit and operation-bounded.
- Later performance comparisons are meaningful because the structural work is
  comparable.
- Refactoring starts from compatible behavior instead of a custom algorithm.

### Negative

- The implementation is closer to SQLite's complexity than ADR-0043.
- A mutable internal cursor and mutable page facade add more state than the
  custom owning-node design.
- Pager promotion and possibly page rekeying require reviewed lower-layer
  seams.
- Statement-level atomicity is not ergonomic until Node 37.

### Deferred

- auto-vacuum, pointer maps, root relocation, and incremental vacuum;
- shared-cache cursor preservation and concurrent writers;
- secure delete;
- WAL;
- bulk-load tuning beyond reference hints;
- custom modern balancing algorithms;
- generalized allocator arenas;
- persistent point-path ownership caches; and
- post-parity refactoring.

## References

- SQLite 3.54.0 `src/btree.c`
- SQLite 3.54.0 `src/btreeInt.h`
- SQLite 3.54.0 `src/bitvec.c`
- SQLite 3.54.0 `src/pager.c`
- ADR-0023: B-Tree Page Decoding
- ADR-0024: Read-Only B-Tree Cursors
- ADR-0040: Journal Contracts and Durable Ordering
- ADR-0041: SQLite-Compatible DELETE-Mode Rollback Journal
- ADR-0042: Rollback-Mode Writable Pager
- ADR-0043: SQLite-Compatible B-Tree Mutation
