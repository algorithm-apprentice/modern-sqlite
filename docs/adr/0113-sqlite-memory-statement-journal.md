# ADR-0113: SQLite In-Memory Statement Journal and Spill

- Status: Accepted
- Date: 2026-10-18
- Module graph node: `implement-advanced-sql`
- Audit entry: `storage-rollback-journal`

## Context

Modern SQLite's rollback-journal backend reproduces SQLite main-journal
headers, sectors, checksums, cohort publication, playback, savepoints, and
DELETE finalization. Its statement subjournal does not reproduce SQLite's
storage mechanism:

- the first savepoint page opens a pathless delete-on-close VFS file;
- every subjournal record is written to that file immediately; and
- `WriteSessionOptions::temporary_storage` does not reach Pager or Journal.

The matched write benchmark nevertheless reports Modern
`temp_store=memory`. In `insert-batch-explicit`, the current mechanism writes
305,372,100 subjournal bytes through 74,481 VFS writes. The workload is
7.058x SQLite in the matched profile.

Pinned SQLite 3.54.0 uses `src/memjournal.c`:

- `memjournal.c:28-80` stores journal bytes in chunked memory;
- `memjournal.c:86-127` reads across chunks;
- `memjournal.c:145-178` spills existing chunks atomically to a VFS file;
- `memjournal.c:187-250` appends in memory or spills before the triggering
  write;
- `memjournal.c:260-293` truncates and closes the memory representation; and
- `memjournal.c:353-392` selects immediate file, bounded memory then spill,
  or memory-only behavior from `nSpill`.

SQLite's default statement-journal spill threshold is 64 KiB
(`global.c:196-203`). `pager.c:4574-4585` forces memory-only subjournals when
the journal mode or connection temp-store policy is memory. Otherwise the
statement journal starts in memory and spills after the configured threshold.

Savepoint release truncates only an in-memory subjournal
(`pager.c:7106-7118`). A spilled file keeps its physical tail while the
logical record count rewinds; later appends overwrite from the logical end.

The main rollback journal is durability-critical. The statement subjournal is
not: it restores the statement/savepoint image inside an already active
transaction. It requires exact typed failure and rollback behavior, but no
sync or directory-durability operation.

This slice changes only subjournal storage and configuration. It does not
change:

- main-journal format, cohorts, synchronization, or recovery;
- savepoint membership and duplicate suppression;
- conditional statement-journal admission, which remains the later
  `storage-transaction` repair;
- Pager `NEED_SYNC`, durable-cohort authorization, or `DONT_WRITE`; or
- SQL PRAGMA support.

## Decision

### 1. Add one SQLite-shaped statement-journal storage owner

Add an internal `StatementJournal` owned by `RollbackJournal::Impl`.
It exposes only the operations the backend requires:

```cpp
class StatementJournal final {
 public:
  StatementJournal(
      Vfs& vfs, std::optional<ByteCount> spill_threshold) noexcept;

  [[nodiscard]] Status Append(ByteView bytes);
  [[nodiscard]] Status ReadAt(MutableByteView output, FileOffset offset);
  [[nodiscard]] Status Rewind(FileSize size);
  [[nodiscard]] bool in_memory() const noexcept;
  [[nodiscard]] FileSize size() const noexcept;
  void Close() noexcept;
};
```

The threshold maps to SQLite `nSpill`:

- `std::nullopt`: memory-only, equivalent to `nSpill < 0`;
- zero: immediate pathless file, equivalent to `nSpill == 0`; and
- positive: retain bytes in memory through that size, then spill before the
  first write whose logical end exceeds the threshold.

The default is 64 KiB. The storage is created lazily with the first
subjournal append.

### 2. Reproduce SQLite's mode-specific chunk geometry

For a positive spill threshold, use one in-memory chunk whose payload size is
the threshold. This matches `sqlite3JournalOpen()`, which sets
`nChunkSize = nSpill`.

For memory-only mode, use 1,024-byte conceptual chunk allocations with 1,016
payload bytes on the current 64-bit target, matching
`MEMJOURNAL_DFLT_FILECHUNKSIZE` and the one-pointer `FileChunk` header.
Encode the payload size as a named source-derived constant; do not use a magic
literal.

Use two mode-specific owners, matching SQLite:

- A positive spill threshold owns one `ByteBuffer` whose size is exactly the
  threshold. SQLite also uses one `nSpill`-sized memory chunk before creating
  the file.
- Memory-only mode owns a singly linked list of fixed chunks:

```cpp
struct MemoryChunk {
  std::unique_ptr<MemoryChunk> next;
  std::array<std::byte, kMemoryChunkPayloadBytes> bytes;
};
```

`kMemoryChunkPayloadBytes` is
`1024U - sizeof(std::unique_ptr<MemoryChunk>)`, reproducing SQLite's
1,024-byte conceptual `FileChunk` allocation without assuming a pointer width.
Require the complete chunk object to remain exactly 1,024 bytes on supported
targets.

Each memory-only allocation is therefore one independently owned fixed chunk.
There is no contiguous metadata allocation, container relocation, or
allocation proportional to total journal size. Append and read walk logical
chunk order. Truncation and close release linked suffixes iteratively so
database-sized journals cannot recurse through one destructor frame per
chunk.

### 3. Make memory append transactional

`Append()` is append-only at the current logical end.

Before copying bytes:

1. compute the checked new logical size;
2. in positive-threshold mode, allocate the one threshold buffer before the
   first append that remains within the threshold; an append that already
   crosses the threshold opens the file directly;
3. in memory-only mode, allocate every required linked chunk without changing
   the published logical size; and
4. on any allocation failure, unlink the newly allocated empty suffix and
   return typed out of memory.

After all storage exists, copy the bytes and publish the new logical size.
The copy itself cannot fail.

The rollback backend continues to build one complete
`page_number || page_image` record in its retained subjournal buffer and
appends that record once. The record count advances only after a complete
append.

### 4. Spill atomically to one pathless temporary file

When a positive-threshold append would cross the threshold:

1. open one pathless read-write, create, exclusive, delete-on-close
   `FileKind::kSubjournal`;
2. write every currently published memory chunk in logical order;
3. write the triggering bytes at the old logical end;
4. publish the file-backed state only after every write succeeds; and
5. then release the memory chunks.

If open or any copy/write fails:

- close and delete the provisional file through RAII;
- retain the complete memory representation and old logical size;
- do not publish the triggering bytes; and
- return the original typed VFS error.

This mirrors `memjrnlCreateFile()`, which restores the `MemJournal` object
when spill creation or copy fails.

After spill, appends and reads use positioned file I/O. The file is never
synced. Close/destruction is best effort under the existing VFS contract.

### 5. Rewind logical and physical state like SQLite

`ReleaseSavepoint(..., rewind_subjournal=true)` computes the byte position
from the retained record count.

- Memory mode calls `StatementJournal::Rewind()`, which drops later chunks,
  resets any read cursor/cache, and publishes the shorter logical size.
- File mode changes only the logical size. It does not issue `Truncate()`;
  later appends overwrite the retained tail.

An equal-size rewind only resets the read cursor and returns; it does not walk
the chunk list.

Savepoint rollback reads records only through the published logical size.
Stale file-tail bytes are unreachable.

Closing the outer transaction releases every chunk or the temporary file and
restores an empty storage owner.

### 6. Keep playback backend-independent

`RollbackSavepointPlayback` borrows `StatementJournal` rather than a raw
`File*`. Its record reader calls the same checked `ReadAt()` contract in
memory and file modes.

Playback order remains:

1. main-journal records at or after the savepoint main boundary; then
2. subjournal records at or after the savepoint record boundary.

Page validation, target start/finish, duplicate suppression, database-size
rules, and cleanup remain unchanged.

### 7. Thread temp-store policy without an upward dependency

Extend `RollbackJournalOptions` with:

```cpp
std::optional<ByteCount> statement_journal_spill_threshold{
    ByteCount{64U * 1024U}};
```

Journal and Pager do not include temporary-storage types.

`WriteSession::Open()` translates its existing
`TemporaryStorageOptions::mode`:

- `TemporaryStoreMode::kMemory` -> `std::nullopt`; and
- `TemporaryStoreMode::kFile` -> the SQLite default 64 KiB threshold.

Direct `Pager::OpenWritable()` callers retain the 64 KiB default and may set
zero or another positive threshold for deterministic tests.

This extends the meaning of the existing non-SQL session temp-store option to
all statement-temporary storage. It does not add PRAGMA parsing or connection-
global configuration.

### 8. Add a version-2 benchmark contract with truthful configuration

The write benchmark currently reports Modern `temp_store=memory` while opening
the default file-mode session.

Preserve the complete version-1 workload, validator, baseline directory, and
raw artifacts unchanged. Mark that baseline superseded for new comparisons;
do not reinterpret its profile identifiers.

Add a version-2 workload/report contract and baseline path:

- `tests/performance/write-workloads-v2.json`;
- workload/report semantics version 2 in
  `tools/write_performance_v2.py`; and
- `benchmarks/write-baseline-v2/`.

Version 2 keeps the SQL, fixtures, scales, digests, timing protocol, and
durability settings unchanged. It changes only the corrected Modern
configuration semantics:

- `engine-default` opens the default file-mode `WriteSession` and reports
  `temp_store=file`; and
- `matched-durable` opens `WriteSession` with
  `TemporaryStoreMode::kMemory`, matching the verified SQLite
  `PRAGMA temp_store=MEMORY`.

Verification sessions may use the same profile-specific option, although
temporary state must be gone before content verification.

Generate and commit the complete version-2 baseline with pinned provenance.
The version-1 validator continues to validate historical artifacts byte for
byte; version 2 becomes the authority for this and later write-performance
repairs. Because the current benchmark source intentionally diverges from the
version-1 input fingerprint, the version-2 runner provides an independent
historical-artifact mode that validates every recorded report and artifact
without rebinding the frozen source-input references to the current tree.

### 9. Freeze source-derived tests before implementation

Add red tests for:

- default positive-threshold appends remaining memory-only below 64 KiB;
- exact-threshold retention and first-byte-over-threshold spill;
- memory-only mode never opening a VFS file;
- zero threshold opening the file on the first append;
- reads spanning memory chunks and file offsets;
- in-memory rewind releasing later chunks;
- file-mode rewind retaining a physical tail but hiding it logically;
- append after rewind overwriting the logical tail;
- spill open failure and retained-buffer copy failure leaving only provisional
  output;
- triggering-write failure after successful copy retaining memory ownership
  and the old logical size;
- OOM at the positive-threshold buffer and each linked memory-only chunk with
  exact retry;
- savepoint rollback parity in memory and spilled modes;
- outer commit/rollback cleanup with no retained chunk or pathless file;
- WriteSession memory/file option propagation; and
- benchmark effective configuration matching the actual option.

The existing file-only subjournal test is expected to fail under the new
default until it explicitly requests threshold zero or asserts the
source-derived memory/spill behavior.

### 10. Qualify savepoint, crash, and performance behavior

After focused green and complete fast units, run:

- journal, rollback-journal, Pager, transaction, and writable-session OOM and
  allocation suites;
- savepoint, statement rollback, crash-cut, and exact recovered-image tests;
- Debug, Release, ASan/UBSan, and genuine GCC Debug/Release;
- native compatibility and matched write-performance contracts;
- write SQL/database fuzz plus read regression fuzz;
- full production clang-tidy, formatting, graph, and discipline checks; and
- exact-tree review and hosted gates.

The committed version-2 matched write baseline must preserve rows, changes,
transaction counts, final digests, integrity, main-journal/database
durability work, and deadlines. For `insert-batch-explicit` under matched
memory temp-store, subjournal VFS opens and writes must become zero. Timing
and allocation deltas are reported but do not replace exact storage-mode
assertions.

## Consequences

### Positive

- small and memory-configured statement journals avoid temporary file I/O;
- spill failure retains a complete rollback image;
- savepoint playback no longer depends on one physical backend;
- the matched write benchmark reports and uses the same temp-store mode; and
- historical version-1 evidence remains reproducible while version 2 carries
  the corrected profile semantics; and
- later conditional statement-journal admission can remove unnecessary
  journals without first repairing their storage.

### Negative

- rollback-journal code gains one chunked memory/file state machine;
- positive-threshold spill adds atomic copy and retry tests;
- direct Pager tests must select immediate file mode when they inspect VFS
  subjournal bytes; and
- the later transaction repair remains necessary to eliminate journals for
  single-write/non-aborting statements.

### Deferred

- main-journal MEMORY mode;
- SQL `PRAGMA temp_store`;
- connection-global `SQLITE_CONFIG_STMTJRNL_SPILL`;
- conditional `usesStmtJournal` admission;
- Pager durable-cohort authorization, `NEED_SYNC`, and `DONT_WRITE`; and
- WAL savepoint storage.

## References

- ADR-0040: Journal Contracts and Durable Ordering
- ADR-0041: SQLite-Compatible DELETE-Mode Rollback Journal
- ADR-0042: Rollback-Mode Writable Pager
- ADR-0047: Reference-Faithful Single-Database Transaction Coordination
- ADR-0050: Pinned Write Performance Baseline
- ADR-0054: SQLite-Aligned ORDER BY and External Sorter
- ADR-0111: SQLite Mechanism Baseline and Implemented-Engine Audit
- ADR-0112: SQLite Page-Cache Hash, Recycling, and Sync Eligibility
- SQLite 3.54.0 `src/global.c`
- SQLite 3.54.0 `src/memjournal.c`
- SQLite 3.54.0 `src/pager.c`
