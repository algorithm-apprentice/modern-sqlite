# ADR-0041: SQLite-Compatible DELETE-Mode Rollback Journal

**Status:** Accepted

## Context

ADR-0040 defines the semantic journal coordinator used by the future writable
pager. The coordinator owns transaction membership, sector protection,
savepoint semantics, write authorization, commit ordering, rollback ordering,
and failure-state transitions. It deliberately delegates persistence to
`JournalBackend`.

The first production `JournalBackend` must implement SQLite 3.54.0's
DELETE-mode rollback-journal format and durability protocol. The implementation
must be byte-compatible with SQLite journals, tolerate the same recoverable
torn tails, preserve the ordering enforced by ADR-0040, and remain reusable by
the writable pager without acquiring pager responsibilities.

The relevant SQLite behavior is concentrated in `src/pager.c`:

- journal-header construction and alignment;
- page-record checksums and serialization;
- journal publication and synchronization;
- multi-header playback;
- subjournal and savepoint playback;
- hot-journal preparation and recovery;
- super-journal trailer recognition; and
- DELETE-mode finalization.

The following format and protocol constraints are compatibility requirements:

1. A journal header starts on a journal-sector boundary and occupies one full
   journal sector.
2. Header and record integers use big-endian byte order.
3. A page record contains a four-byte page number, one complete database page,
   and a four-byte checksum.
4. Ordinary durable journals initially hide the header magic and record count.
   Synchronization publishes both values only after the record bytes are
   durable.
5. Full synchronization uses SQLite's two-phase publication sequence on
   devices that do not advertise sequential writes.
6. Devices that advertise safe append may publish an initial
   `0xffffffff` record count and derive the count from file size during
   playback.
7. Repeated synchronization epochs produce independently checksummed,
   sector-aligned cohorts.
8. The first valid header supplies the original database size used by rollback.
9. Later torn headers, torn records, invalid page numbers, locking-page records,
   and checksum mismatches terminate playback at the last valid record.
10. Actual VFS failures remain errors and must not be converted into successful
    end-of-playback.
11. The legacy zero page-size field uses a caller-supplied fallback page size.
12. A valid super-journal trailer whose referenced super-journal no longer
    exists means the transaction committed and page playback must not occur.
13. The subjournal is a pathless delete-on-close temporary file containing only
    a page number and page image per record.
14. DELETE finalization closes the journal and removes its pathname.

The existing VFS already provides the required operations:

- canonical path resolution;
- typed main-journal and subjournal opens;
- exact positioned reads and writes;
- truncation and file-size queries;
- normal or full file synchronization, including data-only hints;
- explicit directory-synchronized deletion;
- random bytes;
- file existence checks;
- sector-size and device-characteristic queries; and
- delete-on-close temporary files.

The VFS does not yet expose the pathname-length capability used by SQLite when
validating a super-journal trailer. This node will add that small capability
and implement the POSIX value used by the pinned SQLite VFS.

The POSIX VFS synchronizes a newly created main journal's parent directory on
the first successful journal-file synchronization. It also reports a 4096-byte
physical sector and the powersafe-overwrite device characteristic. SQLite uses
an effective 512-byte journal sector on powersafe-overwrite devices. The
coordinator and concrete backend must use the same effective sector so that
sector protection and on-disk header alignment agree.

This node does not own database locking, database-file writes, cache
invalidation, or lock downgrade. Those remain writable-pager responsibilities.
It also does not add the PERSIST, TRUNCATE, MEMORY, OFF, or WAL journal modes.

## Decision

### 1. Add one reusable concrete backend

The storage layer will expose a final `RollbackJournal` implementation in:

- `include/modern_sqlite/storage/journal/rollback_journal.hpp`; and
- `src/storage/journal/rollback_journal.cpp`.

Its public construction surface will be:

```cpp
struct RollbackJournalOptions {
  ByteCount legacy_page_size{4096};
  DirectorySync delete_directory_sync{DirectorySync::kNo};
};

class RollbackJournal final : public JournalBackend {
 public:
  static Result<std::unique_ptr<RollbackJournal>> Create(
      Vfs& vfs, std::string_view database_path,
      FileProperties database_properties,
      RollbackJournalOptions options = {});

  ~RollbackJournal() override;

  [[nodiscard]] std::string_view database_path() const noexcept;
  [[nodiscard]] std::string_view journal_path() const noexcept;
};
```

`Create()` canonicalizes the database pathname through the supplied VFS and
derives the sidecar name by appending `-journal`. The backend borrows the VFS
and stores the main database file's properties supplied by the pager. It owns
its canonical path strings and open journal files. The VFS must outlive the
backend. A `JournalTransaction` and any playback cursor must not outlive the
backend.

Publication policy is derived from `database_properties`, not from the opened
journal handle. SQLite queries the main database handle for powersafe
overwrite, safe append, sequential writes, and sector size, and a VFS may
report different characteristics for different file kinds.

The backend is reusable after successful commit, rollback, or hot-recovery
finalization. Destruction closes owned file handles but performs no
synchronization, playback, or deletion.

`legacy_page_size` is validated at construction and must be a power of two from
512 through 65536. It is used only when parsing a legacy journal header whose
page-size field is zero.

`delete_directory_sync` distinguishes SQLite's ordinary FULL policy from the
stronger EXTRA deletion policy:

- `DirectorySync::kNo` is the default and matches SQLite FULL;
- `DirectorySync::kYes` synchronizes the directory entry after deletion and
  provides the behavior required by SQLite EXTRA.

No general synchronous-mode option is introduced in this node. The initial
policy is SQLite `synchronous=FULL`: a two-phase journal-publication protocol
using normal VFS synchronization strength. SQLite's separate `fullfsync`
option, `synchronous=NORMAL`, and `synchronous=OFF` remain deferred until a
pager configuration surface requires them.

### 2. Resolve one effective journal-sector size

The rollback-journal API will expose:

```cpp
Result<ByteCount> ResolveRollbackJournalSectorSize(
    const FileProperties& properties);
```

The function applies SQLite's journal-sector rules to VFS properties:

- a powersafe-overwrite device uses an effective 512-byte sector;
- a reported size below 32 becomes 512;
- a reported size above 65536 becomes 65536; and
- any other reported size is retained.

SQLite's VFS contract expects an ordinary sector size to be a power of two, but
its pager does not independently enforce that expectation. ADR-0040 does
enforce it because the coordinator uses power-of-two sector cohorts.
Consequently, this resolver returns `ErrorCode::kInternal` if the normalized
non-powersafe value is not a power of two. This does not reject the production
POSIX VFS or a conforming SQLite VFS, and it prevents the coordinator and
on-disk format from using inconsistent geometry.

The writable pager will use this function on the main database file properties
before constructing `JournalTransactionInfo`. `RollbackJournal::DoBegin()`
independently resolves the properties retained at construction and rejects a
mismatching `JournalTransactionInfo::sector_size` before writing the first
header.

This clarifies ADR-0040: `JournalTransactionInfo::sector_size` is the effective
rollback-journal sector used for both coordinator protection and journal-header
alignment, not necessarily the raw physical-sector value returned by the VFS.

### 3. Keep format constants private except for tests through behavior

The backend will use these SQLite-compatible constants:

- eight-byte magic: `d9 d5 05 f9 20 a1 63 d7`;
- header prefix size: 28 bytes;
- page-record overhead: 8 bytes;
- subjournal-record overhead: 4 bytes;
- minimum journal sector: 32 bytes;
- maximum journal sector: 65536 bytes; and
- reserved locking page:
  `0x40000000 / page_size + 1`.

The implementation will use the existing big-endian coding helpers and
`ComputeRollbackJournalChecksum()`. It will not publish a second format API or
duplicate checksum logic.

### 4. Write a zero-magic initial header

`DoBegin()` will:

1. require no active transaction or recovery;
2. open the main journal read-write with create enabled;
3. truncate the file to zero;
4. validate the effective sector derived from the retained database-file
   properties;
5. obtain a four-byte checksum seed from `Vfs::RandomBytes()`;
6. write one full sector containing the initial header; and
7. initialize the first in-memory cohort.

For an ordinary device, bytes 0 through 11 are initially zero. The checksum
seed is written at offset 12, the original database page count at offset 16,
the effective sector size at offset 20, and the page size at offset 24.

For a safe-append device, the initial header contains the real magic and
`0xffffffff` record count from the beginning. This is safe because complete
records are appended in order and playback derives their count from the file
size.

The remainder of the sector is zero padding. Header construction uses one
reusable buffer sized to the effective sector.

### 5. Append one complete page record per VFS write

`DoAppendMainPage()` writes:

1. the four-byte big-endian page number;
2. exactly `page_size` bytes of original page content; and
3. the four-byte big-endian checksum seeded by the current cohort.

The backend assembles the record in one reusable buffer and issues one exact
positioned write. The cohort record count and next offset advance only after
the complete write succeeds.

If the preceding cohort was already published on a non-safe-append device, the
first later append lazily creates a new sector-aligned zero-magic header with a
fresh checksum seed. Laziness is equivalent to SQLite's eager next-header
creation for recovery, while avoiding an empty on-disk header when no later
record is appended.

Safe-append journals retain a single `0xffffffff` cohort and continue appending
records after repeated synchronization calls.

### 6. Publish ordinary cohorts with SQLite FULL ordering

`DoSync()` will first identify the aligned offset at which a later cohort could
begin. If the old journal tail contains SQLite journal magic there, the backend
overwrites the first magic byte with zero before publishing the current
cohort.

For an ordinary non-sequential device, `DoSync()` performs:

1. a normal-strength synchronization of the zero-magic header and complete
   page records;
2. one 12-byte write of the real magic and exact big-endian record count; and
3. a second normal-strength synchronization.

The first successful file synchronization also makes a newly created journal
directory entry durable through the POSIX VFS contract.

For a safe-append device, no header rewrite is required. The backend performs
the post-record normal-strength synchronization when the device is not
sequential.

For a sequential device, SQLite's device contract supplies the ordering
guarantee and the backend omits the explicit journal synchronization calls.

SQLite's `synchronous=FULL` and VFS `SQLITE_SYNC_FULL` are separate concepts.
The former selects the two-phase protocol above. The latter maps to Modern
SQLite's `SyncMode::kFull` and, on macOS, `F_FULLFSYNC`; it is used only by a
future explicit `fullfsync` option. Under that future option, both journal
syncs use `SyncMode::kFull` and the second carries the data-only hint.

A successful synchronization marks the cohort published. A later append on an
ordinary device starts another aligned cohort; it never overwrites records
needed by full transaction rollback.

### 7. Represent main-journal cohorts, not per-page offsets

The backend will retain compact metadata per main-journal cohort:

- header offset;
- first record offset;
- checksum seed;
- record count; and
- publication state.

It will not retain one offset or one allocation per page. Main-journal playback
calculates record offsets from cohort metadata and fixed record size.

This keeps successful page capture free of per-page metadata allocation beyond
the coordinator membership already required by ADR-0040. A new cohort may
allocate one small metadata entry, and savepoint creation may allocate one
boundary entry. Such failures occur before database mutation and propagate
normally.

### 8. Store subjournal records in a delete-on-close temporary file

The subjournal is opened lazily with:

- no pathname;
- read-write access;
- create enabled;
- exclusive creation;
- delete-on-close;
- no main-journal durability synchronization.

Each subjournal record contains a four-byte big-endian page number followed by
one page image. A record is written in one positioned write. The backend tracks
only the number of complete subjournal records.

Main-journal records are never truncated or repurposed by savepoint rollback.
They remain available for a later full transaction rollback.

### 9. Store savepoint boundaries as append positions

For each `JournalSavepoint`, the backend records:

- the current main-journal append offset; and
- the current complete subjournal-record count.

Savepoint playback yields:

1. complete main-journal records whose offsets are at or after the recorded
   main-journal boundary; then
2. complete subjournal records whose indexes are at or after the recorded
   subjournal boundary.

The coordinator remains responsible for savepoint-size filtering and duplicate
suppression. The backend emits the images in stable append order and allocates
only one page buffer for the cursor.

Completing a savepoint retains the named savepoint and drops newer boundaries.
Releasing a savepoint drops the named savepoint and all newer boundaries.

The coordinator also mirrors SQLite's `bTruncateOnRelease` rule. Every
savepoint starts eligible to rewind the logical subjournal count to its
boundary. When a subjournal image is first required by savepoint index `i`,
all newer savepoints become ineligible because that image may also be required
by the older savepoint. `DoReleaseSavepoint()` receives the target identity and
the coordinator's rewind decision. When rewind is allowed, the backend resets
the logical subjournal-record count to the released savepoint's boundary so
later appends overwrite unused records. Physical truncation of the file-backed
temporary subjournal is optional.

The backend validates the full opaque savepoint identity supplied by ADR-0040.

Malformed or incomplete live savepoint records indicate an internal backend
contract violation and return `ErrorCode::kInternal`; they are not treated as a
recoverable crash tail.

### 10. Use one streaming parser for transaction and hot playback

Transaction and hot-recovery cursors share a streaming parser. Cursor creation
reads only enough state to determine:

- whether a usable first header exists;
- the first header's `JournalPlaybackInfo`;
- whether a valid super-journal trailer suppresses playback; and
- the reusable record-buffer size.

`Next()` then walks cohorts and records without materializing all pages or
allocating per record.

The parser accepts:

- exact nonzero record counts;
- `0xffffffff`, deriving complete records from the remaining file size;
- legacy zero page size through `legacy_page_size`; and
- multiple sector-aligned headers.

The first valid header fixes `original_page_count` and `page_size` for the
playback cursor. It also fixes the sector size used to align every later
header. Matching SQLite, later headers contribute magic, record count,
checksum seed, and their otherwise ignored original-size field; bytes 20
through 27 are not revalidated and do not change playback geometry.

For transaction playback only, a final unpublished cohort with `nRec == 0` may
derive complete records from its remaining bytes. Hot recovery never applies
that live-process inference because zero magic or zero count can represent an
unpublished crash tail.

An unusable first header produces a cursor with no playback metadata. This lets
the ADR-0040 coordinator finalize a zeroed or otherwise non-hot journal without
modifying database pages.

After a valid first header, each of the following ends playback successfully at
the last complete valid record:

- a short later header;
- invalid later magic;
- a short page record;
- page number zero;
- a reserved locking-page record;
- checksum mismatch; or
- arithmetic that would move beyond the representable file range.

The parser does not hide VFS read or file-size errors. Those propagate through
`JournalPlayback::Next()`.

Pages above the first header's original database size may still be emitted;
ADR-0040 performs the authoritative playback-size filter before applying an
image.

### 11. Recognize super-journal trailers without writing them

Before exposing hot or transaction playback, the parser inspects the end of the
main journal for SQLite's super-journal trailer:

1. four leading bytes written as the reserved master-journal page number;
2. filename bytes;
3. big-endian filename length;
4. big-endian checksum of the filename bytes; and
5. standard journal magic.

Recognition mirrors SQLite's `readSuperJournal()` rather than validating every
written field. The parser:

- obtains the VFS maximum pathname length;
- requires a nonzero declared length no greater than that capability and no
  greater than the available file prefix;
- verifies the standard magic;
- verifies the checksum using each filename byte as a signed 8-bit value; and
- verifies the pinned SQLite `-mj` plus generated hexadecimal suffix naming
  pattern.

The leading reserved page-number word is not part of SQLite recognition and is
therefore not validated. Pattern matching and the subsequent access check use
the NUL-terminated interpretation used by SQLite, while the checksum covers
all declared filename bytes.

The parser then asks the VFS whether the validated pathname exists.

If the referenced super-journal is absent, the main journal is no longer hot:
the cursor exposes no playback metadata and finalization deletes the stale
main journal. If the referenced super-journal exists, ordinary page playback
continues. Access-check errors propagate.

This node does not write super-journal trailers or coordinate attached
databases.

The VFS contract will gain a non-failing maximum-pathname query. The POSIX VFS
returns 512 bytes, matching the pinned SQLite Unix VFS. Test VFS
implementations return explicit deterministic values.

### 12. Destroy playback cursors before backend completion

The ADR-0040 coordinator will reset a successful playback cursor immediately
after `ApplyPlayback()` and before calling:

- `DoCompleteSavepointPlayback()` for savepoint rollback; or
- `DoFinalizeRollback()` for transaction rollback and hot recovery.

This makes the borrowing rule enforceable: a concrete cursor may borrow an
open file and cohort metadata, and finalization may close that file and clear
that metadata only after cursor destruction.

### 13. Prepare hot recovery before parsing

`DoPrepareHotRecovery()` will:

1. require an inactive backend;
2. open the existing main journal read-write without creating it; and
3. synchronize the journal with normal VFS strength before any playback read.

This preserves SQLite's rule that a hot journal is made durable before recovery
can modify the database.

The writable pager remains responsible for proving all hot-journal
preconditions before invoking the backend:

- the pager initially holds SHARED;
- the journal pathname exists;
- no participant holds RESERVED or greater;
- the database is nonempty;
- the journal's first byte is nonzero; and
- the pager upgrades directly to EXCLUSIVE.

The pager also owns cache invalidation, database-page restoration,
database-file synchronization, and lock downgrade. The backend does not inspect
or manipulate database locks.

### 14. Finalize by closing and deleting

Commit, full rollback, and hot recovery share DELETE-mode finalization:

1. close the main-journal file;
2. close the delete-on-close subjournal, if any;
3. delete the journal pathname using the configured directory-sync policy; and
4. clear reusable transaction metadata only after successful deletion.

The current `File` abstraction closes through destruction and cannot report a
close result. Closing is therefore best-effort. A delete failure propagates
and leaves the ADR-0040 coordinator in its required persistent error state.
The backend does not report success-shaped fallbacks and does not silently
switch journal modes.

`Vfs::Delete()` may unlink the pathname and then fail while synchronizing or
closing the parent directory, so pathname existence is indeterminate after a
delete error. The failed backend instance is terminal and is not reused for a
new transaction.

### 15. Preserve typed failures and allocation guarantees

VFS, path, randomness, synchronization, truncation, deletion, and access-check
errors retain their existing typed `Error` values.

Public factory methods and cursor factories catch `std::bad_alloc`, including
exceptions from delegated VFS or backend setup, and return
`ErrorCode::kOutOfMemory`. Invalid caller configuration returns
`ErrorCode::kMisuse`. Impossible live metadata or malformed VFS properties
return `ErrorCode::kInternal`.

After successful setup:

- one ordinary main-page append performs no dynamic allocation;
- one subjournal-page append performs no dynamic allocation after the
  subjournal buffer and file have been created;
- transaction and hot playback allocate one reusable page-record buffer and no
  per-page state; and
- savepoint playback allocates one reusable page buffer and leaves duplicate
  suppression to ADR-0040.

### 16. Validate format, ordering, crash behavior, and interoperability

Tests will be written red-first and cover:

1. exact header bytes for 512-, 4096-, and 65536-byte database pages;
2. zero-magic initial headers and magic/count publication;
3. exact page-record bytes and SQLite checksums;
4. sector alignment and repeated cohorts;
5. SQLite-FULL protocol event order using two normal-strength syncs;
6. safe-append and sequential device branches;
7. effective sector resolution from database properties, including powersafe
   overwrite;
8. subjournal bytes and nested savepoint playback;
9. legacy zero page size and `0xffffffff` record counts;
10. multi-header playback;
11. torn headers, torn records, invalid pages, locking pages, and bad
    checksums;
12. exact super-journal recognition, pathname bounds, and present/absent
    behavior;
13. VFS read, write, sync, access, and delete failure propagation;
14. deterministic crash snapshots before and after every journal publication
    boundary;
15. real POSIX-VFS journal creation, recovery, and DELETE finalization;
16. cursor destruction before savepoint completion and rollback finalization;
17. allocation-free steady-state append and streaming playback; and
18. exhaustive allocation-failure behavior for setup paths.

Committed golden vectors will record their SQLite 3.54.0 provenance. When the
pinned SQLite amalgamation is supplied to the benchmark build, a regeneration
contract will verify that the vectors still match the pinned reference rather
than relying on the host SQLite installation.

## Consequences

### Positive

- Modern SQLite writes and reads the established SQLite rollback-journal
  format.
- The ADR-0040 ordering contract receives a concrete durable backend without
  taking pager ownership.
- Full rollback and hot recovery stream in constant coordinator memory.
- Savepoints remain efficient without weakening later full rollback.
- Device characteristics retain SQLite's safe-append, sequential-write, and
  powersafe-overwrite behavior.
- Super-journal read support avoids undoing a committed external
  multi-database transaction.
- Directory durability is explicit and can represent SQLite FULL or EXTRA
  deletion semantics.
- Deterministic crash tests pin the publication boundary instead of relying on
  timing-sensitive process crashes.

### Negative

- The backend contains a nontrivial state machine for live writing, savepoint
  playback, transaction playback, and hot recovery.
- Multi-header and super-journal compatibility add parser branches before
  Modern SQLite writes those forms itself.
- SQLite-FULL publication can require two file-sync operations per
  non-safe-append publication epoch.
- Cohort and savepoint metadata may allocate before database mutation.
- The first implementation supports one journal mode and one content-sync
  policy.

### Deferred

- Writable-pager locking and database-file mutation.
- NORMAL, OFF, and configurable synchronous policies.
- PERSIST, TRUNCATE, MEMORY, and exclusive-lock journal finalization.
- Super-journal creation and attached-database atomic commit.
- WAL and WAL-index formats.
- Shared-cache and concurrent-writer policy.
- Pager-level hot-journal detection and lock choreography.

## Alternatives Considered

### 1. Invent a Modern SQLite journal format

Rejected because the project requires SQLite compatibility and recovery
equivalence. A private format would also prevent SQLite from recovering a
Modern SQLite database and vice versa.

### 2. Put journal serialization directly in the writable pager

Rejected because ADR-0040 already separates semantic ordering from persistence.
Combining them would make crash ordering harder to test and would couple the
pager to DELETE-mode details.

### 3. Keep one in-memory offset for every page record

Rejected because record offsets are derivable from compact cohort metadata.
Per-page offsets would duplicate coordinator membership, increase allocation
pressure, and weaken the streaming recovery goal.

### 4. Truncate main-journal records after savepoint rollback

Rejected because those records may still be required for a later full
transaction rollback. Savepoint rollback changes logical page state, not the
transaction-start recovery history.

### 5. Treat every malformed journal as corruption

Rejected because SQLite deliberately accepts valid prefixes after torn writes.
Only real VFS errors and impossible live-process state remain errors.

### 6. Ignore super-journal trailers until attached databases exist

Rejected because Modern SQLite may open a database last written by SQLite. If a
deleted super-journal already committed that transaction, replaying the main
journal would undo committed data.

### 7. Always synchronize the directory when deleting

Rejected as the default because it would silently implement SQLite EXTRA and
add a commit-path synchronization beyond SQLite FULL. The explicit option keeps
the stronger policy available.

### 8. Add all SQLite journal modes now

Rejected because DELETE mode is the current topological node and the smallest
writable-pager foundation. Other modes would add configuration and lifecycle
states before a caller requires them.

## References

- SQLite 3.54.0 `src/pager.c`, rollback-journal header, record, sync, playback,
  savepoint, and hot-recovery paths.
- `docs/adr/0007-rollback-journal-before-wal.md`
- `docs/adr/0019-vfs-contracts.md`
- `docs/adr/0020-posix-vfs.md`
- `docs/adr/0040-journal-contracts.md`
- `include/modern_sqlite/base/coding.hpp`
- `include/modern_sqlite/platform/vfs.hpp`
- `include/modern_sqlite/storage/journal/journal.hpp`
- `project/module-graph.json`, node 34
