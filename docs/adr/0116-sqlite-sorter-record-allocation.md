# ADR-0116: SQLite Sorter Record Allocation

- Status: Accepted
- Date: 2026-10-18
- Module graph node: `implement-advanced-sql`
- Audit entry: `runtime-sorter`

## Context

Modern's sorter ordering, stability, PMA format, extent publication, buffered
I/O, merge fan-in, reset, failure cleanup, and statement-wide resource
ownership are already accepted. Its input-record allocation is not
SQLite-shaped.

`RecordSorter::Insert(ByteBuffer)` currently retains:

- one `ByteBuffer` allocation containing the encoded record; and
- one separately allocated `Entry` node containing metadata and the linked
  list pointer.

The VM creates the `ByteBuffer` through `EncodeRecord()` before calling the
sorter. A 65,536-row in-memory ordering therefore records 131,182 Modern
allocations, approximately two retained allocations per input row. File-mode
ordering records about 402,700 allocations because the same record/node path
feeds every PMA plus merge working sets.

Pinned SQLite 3.54.0 uses two mode-specific owners in `src/vdbesort.c`:

- `SorterRecord` stores `nVal` and its next pointer/offset in one header, with
  the encoded record immediately following it
  (`vdbesort.c:178-190,432-462`);
- memory temp-store mode allocates one `SorterRecord || payload` block per
  record (`vdbesort.c:1989-2045`);
- file temp-store mode starts with one page-sized `SorterList.aMemory`, grows
  it geometrically up to `mxPmaSize`, and stores linked record offsets so
  reallocation cannot invalidate the list (`vdbesort.c:1154-1177,
  1989-2039`); and
- PMA sorting converts offsets to ordinary traversal only after admission is
  complete (`vdbesort.c:1571-1600,1725-1785`).

Modern already exposes `EncodedRecordSize()` and `EncodeRecordInto()`. It can
therefore encode directly into the final sorter owner instead of allocating a
temporary record first.

This slice changes only `RecordSorter` input ownership. It does not change:

- comparison, collation, direction, NULL placement, or stable tie semantics;
- PMA bytes, extent ownership, buffered I/O, merge fan-in, or merge levels;
- statement-wide memory/file ownership accepted by ADR-0115;
- `BoundedTopN` or `EphemeralRelation` representation;
- bytecode shape other than selecting the direct sorter insertion overload;
  or
- CREATE INDEX, planner, VM dispatch, or aggregate algorithms.

## Decision

### 1. Add direct value insertion

Add:

```cpp
[[nodiscard]] Status RecordSorter::Insert(
    std::span<const SqlValue> values);
```

The sorter uses its descriptor's `RecordCodecOptions`. It validates field
count, computes `EncodedRecordSize()`, reserves the final owner, calls
`EncodeRecordInto()`, parses the resulting `RecordView`, and publishes the
record only after every step succeeds.

The VM sorter-insert instruction calls this overload directly. The existing
`Insert(ByteBuffer)` remains for lower-layer callers and malformed-record
tests. It copies the supplied bytes into the canonical owner; it is not used
by the VM hot path.

### 2. Coallocate memory-mode metadata and payload

Memory temp-store mode owns one allocation per record:

```text
[Entry header][encoded record bytes]
```

The complete allocation is created with ordinary aligned `operator new`.
`Entry` is constructed at the front; record bytes immediately follow it.
The owner stores:

- encoded byte count;
- parsed `RecordView` borrowing the trailing bytes;
- charged allocation size; and
- one owning next pointer.

No flexible array, compiler extension, owning raw pointer, or undefined
pointer arithmetic is used. Allocation size and address arithmetic are
checked. Class-specific deletion destroys the header and releases the same
block.

Direct insertion performs exactly one retained allocation per accepted
record. On size, encode, parse, or allocation failure, no list link, count,
tail, current pointer, or accounting value changes.

### 3. Use one offset-linked bulk arena in file mode

File temp-store mode owns one `ByteBuffer` bulk arena per logical sorter.

The first accepted in-memory record first guarantees the exact aligned cell
size, then takes at most one geometric doubling step toward:

```text
min(sorter page size, fairness threshold, available statement headroom)
```

bytes. For optional arena growth, the fairness threshold is the current
statement budget divided by the number of registered memory owners, capped by
the per-sorter threshold. Exact admission may exceed that fair share; the
share only prevents one arena from opportunistically consuming memory needed
by sibling owners during its first sparse allocation. Later admission first
reserves the exact growth required for the pending cell, then opportunistically
doubles within the per-sorter threshold and remaining statement headroom.
Optional growth never spills a sibling, so an arena that grows beyond its
initial fair share consumes only otherwise-free headroom. Failure to reserve
optional growth is not an insertion failure.

Each record is encoded as:

```text
[next offset][record size][validated layout][record bytes][8-byte alignment pad]
```

Offsets use an all-ones sentinel for end-of-list. Headers are encoded through
the canonical coding helpers and require no aligned typed object inside byte
storage. The validated layout records the header offsets and field count
produced by the one admission-time `RecordView::Parse()`. A narrow record-codec
internal contract reconstructs a trusted `RecordView` over relocated bytes in
O(1) after checking the encoded size. The validated-layout access is not part
of the installed public API. Reallocation copies only the published prefix
and cannot invalidate links or force full-record reparsing.

The arena records three separate quantities:

- logical used bytes;
- writable bytes (`ByteBuffer::size()`); and
- charged bytes (`ByteBuffer::capacity()`).

Only writable bytes may receive records. Statement memory accounting charges
actual capacity, not logical payload.

### 4. Sort bulk records by offsets

File-mode in-memory sorting uses the existing bottom-up stable merge shape,
but each run head is a byte offset rather than an owning pointer.

Comparison:

1. validates the referenced header range;
2. rebinds the admission-validated layout to the current arena payload;
3. uses `CompareRecordPrefixes()` and the existing descriptor; and
4. selects the left input on equality to preserve stable order.

Merging rewrites only next-offset fields. It performs no allocation and never
moves payload bytes.

`Rewind()` retains the sorted bulk arena until reset/close. `current_record()`
rebinds the validated current payload, and `Next()` follows the stored offset.
This matches SQLite's retained `aMemory` lifetime.

### 5. Spill the canonical owner without an intermediate record

`SpillCurrentRun()` sorts the active owner and streams each record directly
to the existing `PmaWriter`.

- memory mode walks coallocated `Entry` blocks;
- file mode walks bulk offsets.

After complete PMA publication:

- release the complete memory list or bulk arena;
- retain only the existing `PmaRun` metadata charge; and
- preserve run ordinal, extent, count, and failure behavior.

An oversized single record in file mode uses the existing direct-PMA path.
The direct value overload may allocate one exact `ByteBuffer` only for this
bounded exceptional path before streaming it. Memory mode continues to return
`kTooLarge`.

### 6. Preserve accounting and failure ordering

Before allocation or arena growth, reserve the final retained size.

Add one internal scoped reservation that temporarily excludes the requesting
sorter from statement pressure selection. Bulk replacement never borrows the
old arena across a reservation that may spill that same owner.

If sibling spills cannot make the excluded reservation fit:

1. release the provisional reservation;
2. if an active run exists, spill it explicitly;
3. project exact-cell headroom again;
4. if it is still insufficient and multiple published runs exist, merge them
   to one run to reclaim metadata;
5. restart admission once from the already validated caller input; and
6. never publish the pending record twice.

If one published run remains and the exact pending cell still cannot fit, use
the existing direct-PMA path when its metadata can be preallocated; otherwise
return typed `kTooLarge`. A page-sized threshold must support repeated PMA
runs rather than dead-ending after the first spill.

Completed sibling or self spills remain published; failed insertion guarantees
only that the pending record is absent.

For bulk growth:

1. compute a requested writable size;
2. reserve the projected charge delta under the self-exclusion guard;
3. allocate the replacement buffer;
4. inspect its actual capacity;
5. reserve or release the reconciliation delta under the same guard;
6. copy the published prefix;
7. publish writable size, charged capacity, and the new buffer; and
8. retain the exact final charge.

If allocation or reconciliation fails, release every provisional reservation
and retain the old arena byte-for-byte.

Before sorting or writing a PMA, preflight the persistent `PmaRun` charge:

```text
max(0, sizeof(PmaRun) - active_owner_charge)
```

Reserve any deficit with self excluded and allocate one detached, unlinked
`PmaRun` shell before sorting, extent allocation, or file I/O. Attach the
completed extent and link the shell only after `PmaWriter::Finish()` succeeds.
On failure, destroy the shell and release its provisional charge.

Current-run spill may transfer the retained charge from the active arena/list
after success. Direct spill reserves the complete node charge. Intermediate
merge preallocates its output shell before I/O and transfers charge from the
consumed input nodes only after publication.

If metadata cannot fit after sibling pressure and state-aware self-pressure,
return typed `kTooLarge` before sorting, file I/O, extent publication, or
owner release. Thresholds below, equal to, and above `sizeof(PmaRun)` remain
valid configurations with explicit success/failure tests.

Reset, close, successful spill, and destruction release the exact charged
block/arena bytes. The existing statement-wide pressure selection and poison
rules remain unchanged.

### 7. Freeze allocation and behavior contracts

Add focused tests for:

- one memory-mode allocation per direct inserted record;
- logarithmic file-mode bulk-arena growth rather than per-record node
  allocation;
- exact threshold retention and spill-before-crossing behavior;
- requester-excluded growth when the sorter is the largest and oldest
  pressure candidate, followed by explicit self-spill and one retry;
- repeated multi-PMA admission with a threshold equal to the sorter page size;
- chained active-run spill plus published-run merge when the remaining
  headroom is between one cell and one cell plus `sizeof(PmaRun)`;
- requested writable size versus actual-capacity reconciliation;
- PMA metadata preflight below, at, and above `sizeof(PmaRun)`;
- detached `PmaRun` allocation failure before any extent allocation or file
  write for current-run, direct, and merge output paths;
- stable equal-key ordering through arena reallocation;
- one admission-time record validation and O(1) layout rebinding during
  comparison, including many non-key fields;
- malformed `ByteBuffer` insertion without mutation;
- OOM at memory-block allocation and every bulk growth point with retry or
  exact sorter closure according to the existing contract;
- accounting equal to memory-block totals or bulk capacity;
- spill publication releasing the owner only after complete PMA output;
- reset/close/destruction in writing, positioned, exhausted, and failed
  states; and
- unchanged one-run, multi-run, multi-level, buffered-I/O, and corruption
  suites.

Add one standalone allocation executable. For 65,536 small direct records:

- memory mode permits at most one record allocation per row plus bounded
  sorter/setup allocations; and
- file mode permits only geometric arena growth plus bounded sorter/setup
  allocations before PMA I/O.

Extend the native ordering diagnostic contract:

- `order-integer-memory` allocations must be below 70,000; and
- `order-minimal-spill-file` allocations must not exceed 270,000; and
- `order-multi-spill-file` allocations must not exceed 270,000.

The red baseline was 131,182 allocations for memory ordering and approximately
402,700 for file ordering. The source-derived green measurements are 65,655,
262,325, and 262,459 respectively; the fixed limits retain bounded diagnostic
headroom without weakening the allocation-shape contract.

### 8. Qualify dependent behavior without absorbing later repairs

Run:

- sorter unit, malformed, OOM, allocation, buffered-I/O, PMA, merge, reset,
  and statement-pressure suites;
- ORDER BY, aggregate-local ordering, grouping, and compound native
  diagnostics;
- Debug, Release, ASan/UBSan, instrumentation, GCC 16 Debug/Release;
- read/write SQL and database fuzz;
- full production clang-tidy, pinned formatting, graph, and engineering
  discipline; and
- exact-tree review and hosted checks.

Generate fresh ORDER BY and aggregate evidence against the immutable
baselines. Preserve every result, digest, workload, deadline, and guard.

This slice does not change the linked-list top-N or sorted-array relation
algorithms. Their gaps remain ledger 6 and 7.

## Consequences

### Positive

- memory mode retains one allocation per record instead of payload plus node;
- file mode amortizes in-memory admission through one SQLite-shaped bulk
  arena;
- VM insertion avoids an intermediate encoded `ByteBuffer`;
- statement accounting matches the actual retained owner; and
- dependent sorter consumers improve without changing ordering semantics.

### Negative

- RecordSorter gains two mode-specific in-memory list implementations;
- bulk headers use checked serialized offsets rather than typed node pointers;
- `Insert(ByteBuffer)` copies into canonical ownership; and
- OOM/failure tests must cover arena growth publication.

### Deferred

- sorter worker threads;
- top-N B-tree replacement;
- ephemeral relation B-tree replacement;
- CREATE INDEX bulk loading;
- vectorized VM encoding; and
- allocator integration beyond the statement budget.

## References

- ADR-0054: Spill-Capable Stable Record Sorter
- ADR-0058: Statement-Owned Temporary Storage Budgets
- ADR-0071: Buffered PMA I/O
- ADR-0111: SQLite Mechanism Baseline and Implemented-Engine Audit
- ADR-0115: Retain Statement-Shared Temporary Arenas
- SQLite 3.54.0 `src/vdbesort.c`
