# ADR-0055: SQLite-Aligned Ephemeral Pager and Spillable Top-N

- Status: Accepted
- Date: 2026-10-09

## Context

ADR-0054 requires ORDER BY with a syntactic LIMIT or OFFSET to use a
spill-capable bounded top-N relation. The current implementation has:

- typed top-N bytecode and verifier states;
- VM execution for an in-memory `BoundedTopN`;
- lazy payload evaluation after candidate admission;
- a session-owned temporary-storage factory;
- a rollback-journal writable Pager; and
- SQLite-compatible table and index B-tree mutation.

The in-memory relation deliberately returns `kTooLarge` when retained keys or
records exceed its configured threshold. That is not sufficient for a large
positive parameterized bound or a retained large payload. File-backed top-N
cannot be added by wrapping the current writable Pager:

- `Pager::writable()` currently means that a rollback journal exists;
- every page mutation starts a journal transaction and captures original
  sectors;
- ordinary writable Pager state acquires database locks, publishes a change
  counter, syncs durable files, and supports commit or rollback; and
- a pathless statement-owned relation has no durable image to recover and
  must not create a journal or sidecar.

Implementing a separate ad hoc disk tree in `temporary_storage` would duplicate
the accepted record comparator, B-tree balancing, overflow, freelist, page
cache, and failure behavior. It would violate ADR-0045's requirement that the
first storage implementation use SQLite-faithful mechanisms.

Pinned SQLite 3.54.0 supplies the relevant mechanism:

- `OP_OpenEphemeral` opens a pathless read-write, create, exclusive,
  delete-on-close transient database;
- `sqlite3BtreeOpen()` receives `BTREE_OMIT_JOURNAL | BTREE_SINGLE`;
- the temporary Pager acts as already exclusively locked, performs no lock or
  hot-journal protocol, and may spill dirty pages directly to its transient
  file;
- `pushOntoSorter()` keeps at most `LIMIT + OFFSET` records by comparing a
  candidate with the current largest record and deleting that record before a
  competitive insertion; and
- the ephemeral index record contains ORDER BY keys, an internal sequence
  field, and result data. The sequence makes equal ORDER BY keys stable without
  changing SQL-visible comparison.

The current Modern B-tree reader already permits an encoded index record to
contain more fields than its comparison metadata. The mutation API does not:
it accepts `SqlValue` keys and requires every encoded record field to be part
of the comparison key. A temporary queue needs the SQLite distinction between
key fields and payload fields.

## Decision

### 1. Add one discard-only ephemeral Pager mode

Add:

```cpp
static Result<std::unique_ptr<Pager>> Pager::OpenEphemeral(
    Vfs& vfs,
    PagerOptions options = {});
```

`OpenEphemeral()` opens one pathless file with:

```cpp
FileOpenOptions{
    .kind = FileKind::kTransientDatabase,
    .access = FileAccessMode::kReadWrite,
    .create = true,
    .exclusive_create = true,
    .delete_on_close = true,
}
```

The mode is explicit Pager state, not the accidental absence of a rollback
journal. Read-only, rollback-journal, and ephemeral Pagers therefore remain
distinguishable.

ADR-0042 remains authoritative for `Open()`, `OpenWritable()`, durable locking,
journal ordering, commit, rollback, and recovery. No ephemeral branch may
weaken or silently bypass those main-database contracts.

An ephemeral Pager:

- uses ordinary SQLite database pages, page cache frames, overflow pages,
  freelist pages, and B-tree geometry;
- begins from an empty snapshot with the configured page size;
- performs no `FullPath`, lock, unlock, sidecar access, hot-journal recovery,
  WAL check, rollback-journal creation, journal capture, directory sync, or
  database sync;
- enters one writable transaction after `BeginRead()` and `BeginWrite()`;
- allows one B-tree writer coordinator so the existing mutation code remains
  unchanged above Pager;
- writes dirty cache victims directly to the transient file under cache
  pressure;
- preserves the existing mutation checkpoints and marks the write attempt
  failed when an error occurs after mutation or transient-file publication;
- remains in that discard-only write transaction until destruction; and
- deletes the transient file when the final handle closes.

`Commit()`, `Rollback()`, savepoints, durable change-counter publication, and
transaction-coordinator ownership are not part of this mode. Callers reset a
logical temporary relation by clearing its B-tree and discard the complete
Pager on close or error. There is no durable prior image to restore.

Pager page mutation uses one shared internal preparation operation:

```text
rollback Pager:
  begin journal -> capture required sectors -> mark dirty

ephemeral Pager:
  mark dirty
```

This applies consistently to page promotion, allocation, and page-number
permutation. Cache-pressure writeback branches explicitly:

```text
rollback Pager:
  journal ordering -> database write

ephemeral Pager:
  transient database write
```

The ephemeral branch still preserves typed I/O and OOM failures. It does not
turn failures into success-shaped cleanup.

### 2. Bound the ephemeral cache from the existing threshold

The temporary-storage factory creates the ephemeral Pager with:

- the attached main Pager's current page size; and
- a cache capacity derived from the sorter threshold:

```text
threshold_pages = max(1, ceil(sorter_memory_threshold / page_size))
cache_pages = max(1, min(max(1, main_cache_pages), threshold_pages))
```

The threshold is a spill trigger, not an exact process-memory cap. A B-tree
mutation may temporarily exceed the target while its bounded working set is
pinned, and record/cursor scratch buffers remain separately accounted.

Memory mode creates no ephemeral Pager or file. Its current exact retained
record accounting and `kTooLarge` behavior remain unchanged.

### 3. Add encoded index-record mutation with a key prefix

Extend the index B-tree mutation contract with encoded-record insertion and
deletion. The comparison descriptor continues to contain only key fields,
while the encoded record may contain payload fields after that key prefix.

The internal writable cursor adds an encoded-record seek that:

1. parses the candidate with the canonical record codec;
2. requires at least as many fields as comparison columns;
3. compares stored and candidate records with
   `CompareRecordPrefixes()` and the existing `IndexColumnOrder` span;
4. returns the same insertion position and exact-prefix result as ordinary
   index seek; and
5. reuses existing balancing, overflow, freelist, and failure checkpoints.

The public writer adds narrowly typed operations equivalent to:

```cpp
Status InsertEncoded(ByteView record);
Result<bool> DeleteEncoded(ByteView record);
```

For these operations the configured comparison prefix is the unique key.
Duplicate prefixes are constraint failures. Persistent SQL index APIs retain
their existing exact-field `SqlValue` contract and behavior.

This is a B-tree capability, not a temporary-storage callback or VM type. It
keeps record comparison below both temporary storage and the VM.

### 4. Use two `BoundedTopN` backends selected by temp-store mode

`TemporaryStoreMode::kMemory` retains the existing sorted linked-list backend.

`TemporaryStoreMode::kFile` creates an ephemeral index B-tree when the top-N
relation opens. This matches SQLite's `OP_OpenEphemeral` lifecycle. The
pathless file handle may be opened immediately by the Modern VFS, but page
bytes are written only when cache pressure requires writeback.

The relation initializes SQLite page one and creates exactly one index root.
That root must be page two; any different root is an internal invariant
failure. No schema table entry or second temporary B-tree is created.

Both backends preserve the existing public lifecycle:

```text
open -> writing -> candidate-pending -> writing
                \-> positioned -> exhausted
any live state -> reset/closed
```

Both retain at most the runtime bound and use the same admission rule:

- if fewer than `bound` records are retained, accept the candidate;
- if full, compare only the SQL ORDER BY key prefix with the current largest
  retained record;
- reject a candidate that is greater than or equivalent to the largest key;
- otherwise delete the largest record before entering candidate-pending
  state; and
- require the following full record to match the pending candidate key.

Rejecting equivalent keys preserves the earlier retained rows, matching the
current in-memory behavior and SQLite's sequence-based stability.

### 5. Encode one stable ephemeral queue record

The file-backed relation stores:

```text
[ORDER BY key fields..., sequence, packed complete result record]
```

The comparison prefix is:

```text
[ORDER BY key fields..., sequence]
```

The original ORDER BY fields retain their collation, direction, and NULL
placement. The sequence field is:

- a nonnegative signed 64-bit integer;
- strictly increasing for each successful insertion;
- BINARY, ascending, and NULLS FIRST; and
- reset to zero when the relation is cleared.

Sequence exhaustion returns `kTooLarge` and closes the relation.

The final field is one BLOB containing the already encoded complete top-N
record. It is payload, not part of B-tree comparison. This mirrors SQLite's
packed-data sorter case and avoids inventing a second row format.

Insertion:

1. validates the complete record against the pending candidate;
2. materializes the key fields, next sequence, and owned packed-record BLOB;
3. encodes the ephemeral queue record once;
4. inserts it through encoded index mutation; and
5. clears pending state only after successful insertion.

Drain:

1. opens an ordinary index cursor on the ephemeral root;
2. scans from first to last;
3. copies the current ephemeral queue record;
4. parses the packed-record BLOB as the public current record; and
5. keeps the owning queue-record buffer alive until `Next()`, reset, close, or
   destruction.

No sequence field or wrapper BLOB is SQL-visible.

### 6. Clear on reset and discard on every failure

File-backed `Reset()` destroys any read cursor, clears the single ephemeral
index B-tree, resets count and sequence, and returns to writing state. It does
not commit, reopen, or publish a persistent image.

Any failure while:

- opening the transient file or Pager;
- initializing page one or the ephemeral root;
- reading, writing, allocating, balancing, or clearing B-tree pages;
- encoding or parsing the queue wrapper;
- inserting or deleting an encoded record; or
- opening, positioning, or advancing the drain cursor

closes the top-N relation and releases the delete-on-close file. VM halt,
reset, detach, exception, and statement destruction continue to use the same
top-N close path.

Best-effort destructor cleanup follows ADR-0019. Checked operations report the
original typed failure.

### 7. Keep runtime LIMIT lowering unchanged until storage is complete

The existing `PhysicalSortStrategy::kRuntimeLimit` remains explicitly
unsupported in lowering until:

1. ephemeral Pager behavior is implemented and reviewed;
2. encoded index-record mutation is implemented and reviewed; and
3. file-backed `BoundedTopN` passes its lifecycle, spill, failure, OOM, and
   allocation matrices.

Only then may lowering select:

- bounded top-N for a nonnegative checked `LIMIT + OFFSET`;
- the external sorter for negative LIMIT or signed bound overflow; and
- the existing zero-LIMIT pre-open short circuit.

The syntactic-LIMIT key-first evaluation schedule from ADR-0054 is unchanged.

### 8. Deliver the implementation in reviewed slices

Implementation is split into sequential pull requests:

1. ephemeral Pager mode, direct transient writeback, and Pager failure tests;
2. encoded index-record mutation with key-prefix comparison;
3. file-backed `BoundedTopN` plus memory/file parity, spill, reset, cleanup,
   OOM, and allocation tests;
4. runtime LIMIT/OFFSET lowering and VM/session differential coverage; and
5. the ORDER BY performance baseline required by ADR-0054.

No later slice begins before the prior pull request is reviewed and merged.

## Verification

The Pager slice requires deterministic tests for:

- exact pathless transient open flags;
- no lock, unlock, journal, sidecar, directory-sync, or file-sync calls;
- page initialization, allocation, promotion, permutation, cache-pressure
  writeback, readback, and delete-on-close cleanup;
- open, read, write, and allocation failures;
- no leaked pins or files after close and error; and
- unchanged rollback-journal Pager behavior.

Encoded index mutation requires:

- records with key prefixes and large payload suffixes;
- ascending/descending, NULL placement, BINARY, NOCASE, mixed storage classes,
  and equal-prefix cases;
- duplicate-prefix rejection;
- insertion, overflow, split, rebalance, deletion, and corruption cases;
- OOM and mutation-checkpoint failure matrices.

File-backed top-N requires parity with the memory backend for:

- zero, one, and large bounds;
- accepted, rejected, and evicted candidates;
- earlier equal-key retention and stable output;
- large payloads and multi-page overflow;
- reset during writing, positioned, and exhausted states;
- minimal and repeated transient-file writeback;
- every injected open, read, write, encoding, B-tree, and allocation failure;
- cleanup after check, insert, rewind, current-row, next, reset, halt,
  finalize, and destruction; and
- no retained row count above the runtime bound.

Runtime lowering requires pinned SQLite differential and model coverage for:

- LIMIT, OFFSET, comma LIMIT syntax, zero and negative LIMIT;
- checked `LIMIT + OFFSET` overflow;
- parameter rebinding and reset across top-N and external-sorter strategies;
- key-first admission and skipped payload side effects;
- table, rowid, covering-index, and noncovering-index access;
- in-memory and forced file-backed execution; and
- strict integer-conversion failures.

## Consequences

### Positive

- File-backed top-N reuses the accepted Pager, page cache, record codec, and
  B-tree mutation mechanisms.
- Temporary writes cannot create or recover a main-database journal.
- Equal-key stability and lazy payload evaluation remain explicit and
  testable.
- Large bounds and payloads are limited by transient storage rather than
  process memory.
- The encoded key-prefix mutation contract is reusable by later ephemeral
  queues, DISTINCT sets, automatic indexes, and materialization.

### Negative

- Pager gains a second write policy whose branches require dedicated failure
  coverage.
- File mode opens a pathless transient handle before actual page spill,
  whereas SQLite may defer the operating-system open until first write.
- The packed complete record is nested in one transient BLOB and is encoded
  again as part of the ephemeral queue record.
- Reset clears a B-tree instead of dropping one in-memory list.

## Rejected alternatives

### Keep returning `kTooLarge`

Rejected because valid large parameterized LIMIT queries must spill and
because payload evaluation behavior is already committed by ADR-0054.

### Add an unbounded vector, set, or heap

Rejected because database-controlled memory growth is not acceptable and a
custom comparator container would not provide spill, overflow pages, or VFS
failure semantics.

### Use the main rollback-journal Pager for the temporary file

Rejected because a delete-on-close scratch image has no durable prior state,
must not create sidecars, and must not participate in main-database locking or
recovery.

### Implement a second disk tree inside `temporary_storage`

Rejected because it would duplicate SQLite B-tree page layout, balancing,
overflow, freelist, comparison, and corruption behavior.

### Treat result payload fields as comparison keys

Rejected because payload must not affect SQL order. SQLite explicitly
distinguishes key fields from packed result data.

### Commit the ephemeral Pager between operations

Rejected because top-N is one statement-owned scratch transaction. Commit
ordering, sync, and recovery add work without observable durability.

### Migrate the current linked list only after threshold crossing

Rejected for file mode because SQLite opens one ephemeral ordering B-tree and
lets its page cache decide when bytes spill. Selecting the backend once at
open also avoids dual-backend migration after victim eviction.
