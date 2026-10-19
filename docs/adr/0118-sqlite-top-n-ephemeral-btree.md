# ADR-0118: SQLite Top-N Ephemeral B-Tree

- Status: Accepted
- Date: 2026-10-19
- Module graph node: `implement-advanced-sql`
- Audit entry: `runtime-topn`

## Context

Modern implements SQLite-visible ORDER BY with a positive bounded
`LIMIT + OFFSET` through `BoundedTopN`. The lowering and VM already preserve
the observable SQLite schedule:

- evaluate ORDER BY keys before non-key payload;
- reject noncompetitive rows before payload evaluation;
- evict the current largest retained row before evaluating an accepted
  replacement payload;
- retain earlier rows when a full queue receives an equivalent ORDER BY key;
- keep at most the runtime bound; and
- drain retained rows in canonical key order before applying OFFSET and LIMIT.

The storage mechanism remains split:

- memory temp-store keeps a sorted singly linked list of complete encoded
  records;
- file temp-store keeps an ephemeral index B-tree in the statement-shared
  relation arena; and
- both retain a separately owned pending key between admission and insertion.

The memory list scans to its tail to find and remove the largest record and
linearly scans again to insert every accepted row. Its adverse admission cost
is therefore O(bound) per source row. The file backend uses the B-tree but
copies the complete largest wrapper before every full-queue comparison and
eagerly copies/parses each current wrapper during drain, including rows skipped
only to satisfy OFFSET.

ADR-0117 now supplies the missing canonical mechanism in both temporary
storage modes:

- file mode uses the statement-shared pathless ephemeral Pager;
- memory mode uses the same Pager/B-tree with no `File` or VFS I/O;
- the relation cache is bounded independently from the main Pager cache;
- roots reuse `IndexBtreeWriter` and `IndexBtreeCursor`; and
- B-tree mutation and cursor failures have statement-wide failure scope.

Pinned SQLite 3.54.0 uses one ephemeral ordering B-tree for bounded ORDER BY:

- `select.c:742-903` evaluates keys, appends `OP_Sequence`, positions at the
  last entry when the queue is full, compares only the ORDER BY prefix through
  `OP_IdxLE`, deletes the current largest entry, then inserts the accepted
  record;
- `vdbe.c:5715-5726` publishes the increasing sequence;
- `vdbe.c:6058-6185` deletes the currently positioned entry;
- `vdbe.c:6411-6475` positions at the last entry;
- `vdbe.c:6728-6787` inserts the encoded index record; and
- `vdbe.c:6998-7065` compares the positioned index entry with a bounded key
  prefix.

SQLite selects memory or file temporary storage below the same B-tree
algorithm. It does not replace the memory queue with a linked list.

A fresh exact-source baseline at merge `8ce00a6` records:

| Workload | Wall ratio | Modern allocations |
|---|---:|---:|
| `order-topn-memory` | 6.410409x | 67,540 |
| `order-topn-file` | 8.229991x | 624,675 |

The file workload uses `LIMIT 64 OFFSET 60000`. Eager current-record copying
therefore materializes almost every skipped wrapper even though only 64 rows
are read as output.

This slice does not change:

- ORDER BY parsing, binding, aliases, ordinals, collations, direction, or NULL
  placement;
- physical strategy selection or runtime bound normalization;
- key-first expression evaluation and skipped-payload side effects;
- the external sorter or its PMA format;
- top-level LIMIT/OFFSET output semantics;
- the statement-shared relation arena accepted by ADR-0115;
- the queue wrapper's SQL-visible record fields; or
- persistent database, journal, locking, schema, or recovery behavior.

## Decision

### 1. Use one ephemeral B-tree backend in both temporary modes

Remove `BoundedTopN::Impl::Entry`, the sorted linked list, tail scans,
suffix-independent list insertion, and memory-mode retained-row accounting.

For every positive bound, `BoundedTopN::Create()` acquires one root from the
statement relation arena with comparison columns:

```text
[ORDER BY key metadata..., sequence ASC BINARY NULLS FIRST]
```

The selected statement temporary mode determines only the Pager backing:

- memory mode uses the fileless ephemeral Pager from ADR-0117;
- file mode uses the delete-on-close transient Pager; and
- both use the same root lease, writer, cursor, page format, balancing,
  overflow, freelist, and failure behavior.

A zero-bound direct capability acquires no root, always rejects candidates,
and rewinds exhausted. Production lowering already halts LIMIT zero before
`OpenTopN`, so this is an API boundary rather than a SQL execution difference.

Root pages remain owned by ADR-0115's statement-shared arena. The first root
may be page two; later or reused roots need not be. Close retires the root,
and reset clears it without reopening the Pager.

### 2. Preserve the stable queue record and candidate lifecycle

Retain ADR-0055's internal queue record:

```text
[ORDER BY key fields..., sequence, BLOB(complete visible top-N record)]
```

The comparison key is the ORDER BY fields plus sequence. The packed complete
record remains payload and is the only SQL-visible record returned by the
capability.

Sequence:

- starts at zero;
- increments only after successful insertion;
- orders equal accepted keys by arrival;
- is excluded from full-queue competitiveness comparison;
- resets to zero when the root is cleared; and
- returns typed `kTooLarge` and closes the capability on exhaustion.

Keep the public state machine:

```text
writing -> candidate-pending -> writing
writing -> positioned -> exhausted
any live state -> reset or closed
```

`CheckCandidate()` still owns the encoded key until `Insert()` publishes a
matching complete record. Pre-mutation shape or key mismatch remains
nonmutating and retryable.

### 3. Compare the current B-tree record through a borrowed encoded prefix

Add one canonical read-cursor primitive:

```cpp
Result<std::weak_ordering> CompareCurrentRecordPrefix(
    const RecordView& key,
    std::size_t key_field_count);
```

It returns the ordering of the current stored record relative to the supplied
encoded key over exactly `key_field_count` columns.

The implementation reuses the existing B-tree record-prefix reader and record
comparator:

- local records compare without allocation;
- overflow records read only the header and requested field payload prefix;
- malformed overflow chains retain complete-chain validation;
- the stored record may contain sequence and payload suffix fields; and
- the candidate must contain the complete requested key prefix.

Add the corresponding `RecordPrefixView` versus `RecordView`
`CompareRecordPrefixes()` overload rather than materializing owned
`SqlValue` keys or duplicating comparison logic.

### 4. Delete the positioned largest index entry directly

Add a generic index-writer operation:

```cpp
Result<bool> DeleteLast();
```

Its writable cursor:

1. resets to the root;
2. descends through each rightmost child;
3. reports false for an empty index;
4. positions on the last leaf record; and
5. reuses the existing index deletion, overflow cleanup, balancing,
   mutation-checkpoint, and rollback-required machinery.

This is the typed equivalent of SQLite's `OP_Last` followed by `OP_Delete`.
It avoids copying the largest payload merely to seek the same record again.
It is a lower B-tree primitive, not a top-N callback or alternate deletion
algorithm.

### 5. Reuse one admission cursor between rejected candidates

Every otherwise accepted candidate first preflights that the next signed
sequence value is representable.

When fewer than `bound` rows exist, admission then reserves the pending key
and does not inspect the B-tree.

When full:

1. open or reuse one index cursor;
2. position it at the last record;
3. compare the candidate with only the ORDER BY key prefix;
4. reject when the candidate is greater than or equivalent to the largest
   key, retaining the cursor for the next nonmutating check;
5. preflight sequence availability and reserve the complete pending-key
   ownership;
6. otherwise clear the read cursor, call `DeleteLast()`, decrement the count,
   and publish the already reserved pending candidate state with an explicit
   `pending_after_eviction` marker.

Equivalent keys are rejected only when full. Before the queue reaches its
bound, equal keys are inserted with increasing sequence values and drain in
arrival order.

The read cursor is destroyed before writer mutation. Any successful insertion
or reset invalidates prior admission position.

### 6. Materialize current output lazily

`Rewind()` and `Next()` position or advance the B-tree cursor without copying
the queue wrapper.

`current_record()` performs the first materialization for the current entry:

- if the complete queue record is local to its B-tree page, parse the wrapper
  and nested packed record directly from the pinned page;
- if it uses overflow pages, copy it once through `CopyPayload()`, charge the
  copy to the top-N memory account, and retain it until `Next()`, reset, close,
  or destruction;
- file mode may use the existing bounded working-set fallback after ordinary
  statement pressure;
- memory mode must remain within the ordinary temporary memory account; and
- repeated field reads of one current record reuse the same parsed view.

Rows advanced only to satisfy OFFSET therefore perform no wrapper copy or
record parse. This matches SQLite's positioned-cursor drain rather than eager
row materialization.

`BoundedTopN::current_record()` retains its public const signature. The PImpl
cache and owned scratch are internal mutable state, and the private PImpl
owner may close the capability after a lazy read failure without changing the
source or ABI contract. The returned `RecordView` lifetime remains unchanged.

### 7. Preserve statement accounting and failure scope

The top-N memory account owns only:

- the pending candidate key; and
- an overflow-backed copied current wrapper while positioned.

Retained queue rows live in the separately bounded relation Pager cache and
are not double-charged as retained owners, matching ADR-0117.

Failures before B-tree mutation preserve the existing local behavior:

- invalid descriptors, key shapes, record shapes, or key mismatch return typed
  errors without poisoning statement storage;
- pending-key budget rejection before eviction leaves the queue unchanged;
- zero bound rejects without allocation; and
- sequence exhaustion closes the capability.

Root, cursor, comparison-prefix, delete, insert, clear, page-limit, or
temporary backend failures mark the shared relation arena failed, close the
current top-N capability, and reject sibling temporary capabilities.

Successful insertion clears `pending_after_eviction`. A successful explicit
reset may also clear it after clearing the complete root. Close, destruction,
or move replacement while the marker remains set first marks statement
storage failed. This catches payload evaluation and outer complete-record
encoding failures that occur in the VM after `CheckCandidate()` evicts but
before `BoundedTopN::Insert()` is called. Direct capability users receive the
same behavior when they abandon and close a post-eviction pending candidate.

Any allocation, wrapper construction, or encoding failure inside `Insert()`
after `DeleteLast()` succeeds has the same statement-fatal scope because the
queue has already changed. Sequence exhaustion and pending-key budget
rejection are preflighted before eviction and remain nonmutating.

Reset clears the cursor, current wrapper, pending candidate, root contents,
count, and sequence in that order, then returns to writing. Destruction
retires the root through the existing no-throw lease.

### 8. Freeze mechanism and performance contracts

Add focused tests for:

- a positive memory-mode top-N allocating relation-cache pages while opening
  no VFS file;
- memory and file modes using the B-tree from the first accepted record;
- one-page memory capacity failing at root creation and the exact minimum
  succeeding;
- zero bound remaining root/file lazy;
- accepted, rejected, evicted, and equivalent candidates in both modes;
- full-queue pending-key budget rejection and sequence exhaustion preserving
  the complete victim and count before eviction;
- large key and payload overflow with prefix-only comparison;
- direct last-entry deletion through leaf and interior index pages;
- stable equal-key output through root splits and reuse;
- repeated rejected candidates reusing one admission cursor;
- OFFSET-style `Next()` calls performing no current-wrapper materialization;
- local current records remaining borrowed and overflow current records
  retaining one charged copy;
- reset during writing, candidate-pending, positioned, and exhausted states;
- root reuse with fresh ordering metadata;
- page-limit, cursor, comparison, deletion, insertion, clear, VFS, and
  allocation failures poisoning the shared arena where required; and
- post-eviction wrapper/encoding failure rejecting sibling capabilities and
  releasing the arena at statement cleanup;
- abandoned post-eviction pending candidates poisoning siblings on close,
  destruction, and move replacement, with successful full-root reset as the
  explicit recovery path;
- VM payload-evaluation and complete-record encoding failure after eviction
  exercising the same automatic pending-abandonment cleanup;
- dependent VM, session, aggregate, compound, and private-subroutine cleanup.

Generate a fresh ORDER BY baseline against both the committed immutable
baseline and the exact pre-repair artifact
`topn-order-before-baseline-v1`. Preserve every SQL string, result, digest,
configuration, deadline, and guard.

The repaired mechanism must:

- materially improve `order-topn-memory`;
- reduce `order-topn-file` allocation and skipped-row materialization work;
- retain zero file activity in memory mode;
- retain physical spill evidence in file mode;
- introduce no result or digest change; and
- keep unrelated sorter workloads within their existing allocation contracts.

Add current-head allocation or mechanism ceilings only from fresh,
cross-standard-library measurements. Do not weaken the immutable v1 baseline
or accept both linked-list and B-tree ownership at one source scope.

Run:

- B-tree cursor/writer focused, malformed, OOM, allocation, overflow, split,
  rebalance, and deletion tests;
- complete bounded top-N semantic, lifecycle, memory/file, OOM, allocation,
  cache, root-reuse, and failure suites;
- ORDER BY, aggregate HAVING/top-N, ordered compound, VM, lowering, session,
  and private-read dependent suites;
- Debug, Release, ASan/UBSan, instrumentation, GCC 16 Debug/Release;
- read/write SQL and database fuzz;
- native compatibility/performance contracts;
- full production clang-tidy, pinned formatting, graph, and engineering
  discipline; and
- exact-tree review and hosted checks.

## Consequences

### Positive

- both temporary modes use SQLite's logarithmic bounded-ordering mechanism;
- memory mode no longer scans or mutates a linked list per source row;
- full-queue rejection compares only the stored key prefix;
- eviction deletes the positioned maximum without copying its payload;
- OFFSET skips do not materialize discarded rows;
- queue pages share ADR-0115's bounded arena and ADR-0117's Pager policy; and
- no duplicate tree, heap, comparator, or spill implementation is added.

### Negative

- BoundedTopN creation with a positive bound now requires page one plus a
  relation root even in memory mode;
- the B-tree cursor gains an encoded-prefix comparison operation;
- the index writer gains direct last-entry deletion;
- current-record materialization may return typed OOM for an overflow wrapper;
  and
- very small bounds pay fixed B-tree page overhead instead of list-node
  overhead.

### Deferred

- changing the accepted packed-record BLOB wrapper to SQLite's direct payload
  field layout;
- a long-lived mutable B-tree cursor shared between admission and insertion;
- Bloom-filter admission for automatic indexes;
- vectorized key encoding;
- sorter worker threads; and
- top-N selection based on estimated cost rather than SQLite's current
  syntactic bounded-LIMIT rule.

## References

- ADR-0054: SQLite-Aligned ORDER BY and External Sorter
- ADR-0055: SQLite-Aligned Ephemeral Pager and Spillable Top-N
- ADR-0058: Statement-Owned Temporary Storage Budgets
- ADR-0115: Retain Statement-Shared Temporary Arenas
- ADR-0117: SQLite Ephemeral Relation B-Tree
- SQLite 3.54.0 `src/select.c`
- SQLite 3.54.0 `src/vdbe.c`
- SQLite 3.54.0 `src/btree.c`
