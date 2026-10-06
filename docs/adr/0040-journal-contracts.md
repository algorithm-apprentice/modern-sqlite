# ADR-0040: Journal Contracts and Durable Ordering

- Status: Accepted
- Date: 2026-10-05
- Amended by: ADR-0045 replaces hash-set page membership and savepoint
  duplicate suppression with the shared SQLite-style PageBitvec.

## Context

ADR-0007 requires rollback journaling before writable pager and WAL work.
ADR-0019 supplies typed file, synchronization, deletion, and locking
primitives, while ADR-0021 deliberately keeps journal policy out of the page
cache. The `implement-journal-contracts` DAG node must now define the boundary
between a future writable pager and the concrete rollback-journal
implementation without performing real filesystem I/O itself.

Pinned SQLite 3.54.0 establishes the safety model:

- `src/pager.c:137-357` defines the rollback-pager states and requires journal
  synchronization before database modification and database synchronization
  before journal finalization.
- `doc/pager-invariants.txt:1-73` requires original page images for every page
  in an affected filesystem sector to be durable before overwrite, database
  writes to be page-aligned, rollback to restore the original database size,
  and the database to be synchronized before the journal is deleted,
  truncated, or invalidated.
- `src/pager.c:418-445` records, for each savepoint, the original page count,
  main-journal boundary, sub-journal boundary, and page-membership set.
- `src/pager.c:1039-1079,1849-1871,4589-4641` writes one sub-journal image for
  all applicable savepoints and updates membership only for pages that
  existed when each savepoint began.
- `src/pager.c:6050-6185` records the transaction-original page image before a
  page becomes writable and records a later savepoint image before the next
  mutation.
- `src/pager.c:6192-6264` journals every transaction-start page sharing a
  filesystem sector before one smaller database page in that sector is
  overwritten.
- `src/pager.c:6510-6799` performs commit in two phases: synchronize the
  journal, write and synchronize the database, then finalize the journal.
- `src/pager.c:2280-2410` skips pages beyond the playback size, suppresses
  duplicate savepoint replay, and treats an illegal page number or checksum
  mismatch as the beginning of an unusable crash tail rather than a fatal
  recovery error.
- `src/pager.c:2872-2995` restores the original database size and page images,
  stops safely at incomplete trailing content, and leaves failed playback
  recoverable from the still-present journal.
- `src/pager.c:4088-4112,5295-5421` synchronizes a hot journal before
  playback, invalidates cached state, restores the database while holding the
  required lock, and finalizes the journal only after successful recovery.

The concrete rollback-journal file format, DELETE-mode sidecar lifecycle,
checksums, torn-write parsing, and VFS integration belong to the next DAG
node. Writable page-cache integration, database locking, dirty-page spill,
and actual database writes belong to the writable-pager node. This node needs
contracts and executable ordering policy that both later nodes can use.

## Decision

Add journal contracts under `modern_sqlite/storage/journal`. The module has
three roles:

1. `JournalTransaction` validates transaction and savepoint state, tracks
   which page images have already been captured, and enforces commit order.
2. `JournalBackend` is the narrow semantic persistence boundary implemented
   by the future rollback-journal backend and by deterministic test fakes.
3. `JournalPlayback` and `JournalRecoveryTarget` separate journal decoding
   from database/cache restoration so recovery ordering is testable without
   real storage.

The contracts are externally serialized. They introduce no mutex, registry,
plugin discovery, SQL type, B-tree type, cache frame, or pager ownership.
The shared `PageNumber` value type is reused, but the journal module does not
depend on page-cache behavior.

### Page images and transaction metadata

```cpp
struct JournalTransactionInfo {
  ByteCount page_size;
  ByteCount sector_size;
  std::uint32_t original_page_count;
};

struct JournalPageImage {
  PageNumber page_number;
  ByteView bytes;
};

class JournalSavepointId;

struct JournalSavepoint {
  JournalSavepointId id;
  std::uint32_t original_page_count;
};
```

- Page size must be a power of two from 512 through 65536.
- Sector size must be a power of two from 32 through 65536. Since both sizes
  are powers of two, either one page spans complete sectors or one sector
  contains a fixed number of database pages.
- Page zero is invalid.
- The SQLite locking page at `0x40000000 / page_size + 1` is invalid for
  capture and playback. It is excluded from sector-cohort requirements.
- Every image must contain exactly one configured database page.
- Image bytes are borrowed only for the duration of the call that receives
  them. A backend must copy or persist all needed bytes before returning
  success.
- `original_page_count` is the logical database size at transaction or
  savepoint entry. A page above that value is restored by truncation and does
  not require a page image for that scope.
- Savepoint identifiers are monotonically assigned by one
  `JournalTransaction`. Callers may retain them as values, but operations
  reject identifiers that are not active in that transaction.

### Backend boundary

`JournalBackend` exposes only the operations required by the coordinator:

```cpp
class JournalBackend {
 public:
  virtual ~JournalBackend() = default;

 protected:
  virtual Status DoBegin(JournalTransactionInfo info) = 0;
  virtual Status DoAppendTransactionPage(JournalPageImage image) = 0;
  virtual Status DoAppendSavepointPage(JournalPageImage image) = 0;
  virtual Status DoCreateSavepoint(JournalSavepoint savepoint) = 0;
  virtual Status DoReleaseSavepoint(
      JournalSavepointId savepoint, bool rewind_subjournal) = 0;
  virtual Result<std::unique_ptr<JournalPlayback>>
  DoOpenSavepointPlayback(JournalSavepoint savepoint) = 0;
  virtual Status DoCompleteSavepointPlayback(
      JournalSavepoint savepoint) = 0;
  virtual Status DoSync() = 0;
  virtual Result<std::unique_ptr<JournalPlayback>>
  DoOpenTransactionPlayback() = 0;
  virtual Status DoPrepareHotRecovery() = 0;
  virtual Result<std::unique_ptr<JournalPlayback>>
  DoOpenHotPlayback() = 0;
  virtual Status DoFinalizeCommit() = 0;
  virtual Status DoFinalizeRollback() = 0;
};
```

The protected hooks are callable only through `JournalTransaction` and the
hot-recovery front door. This is runtime polymorphism at a persistence
boundary, not a general journal plugin framework.

`JournalTransaction::Begin(JournalBackend&, JournalTransactionInfo)` returns
unique ownership of a non-copyable, non-movable transaction that borrows the
backend. The backend must outlive the transaction. Playback cursors are
uniquely owned but may borrow backend file and buffer state, so they are
created and consumed synchronously. The coordinator destroys each cursor before
calling savepoint-completion or rollback-finalization hooks that may invalidate
borrowed backend state. A recovery target must outlive the call using it. No
cursor is exposed to the future pager.

The future rollback backend maps these semantic calls to main-journal and
sub-journal offsets. `DoPrepareHotRecovery()` performs the pre-playback
journal synchronization required after a crash. `DoFinalizeCommit()` and
`DoFinalizeRollback()` make the journal non-hot; for the writable MVP this
means DELETE-mode removal with the configured directory-durability policy.

Backend calls are all-or-error at the semantic boundary. A failed append may
leave an incomplete trailing record, but must not report success. The next
rollback or hot-recovery playback must ignore a trailing incomplete record
and must never expose it as a valid page image.

### Transaction page membership

`JournalTransaction` owns one transaction membership set and one membership
set per active savepoint.

Before every page mutation, the future writable pager calls
`CapturePage(image)` with the page's current bytes:

1. If the page existed at transaction entry and is absent from the
   transaction membership set, append it to the main journal.
2. After a successful main-journal append, mark the page present in the
   transaction set and in every applicable active savepoint set. The
   transaction-original image is also the savepoint-entry image when the page
   has not previously been mutated.
3. Otherwise, if at least one applicable savepoint has not captured the
   page, append one sub-journal record and mark it present in every
   applicable savepoint set. This includes a transaction-new page that
   existed when a later savepoint began.
4. Only then return success and make the page eligible for mutation.

One sub-journal image therefore serves all active savepoints that need the
same current page state, matching SQLite. The same mutation never writes both
a main-journal image and a redundant sub-journal image.

ADR-0045 supersedes the original hash-set membership baseline. Transaction
membership, savepoint membership, and savepoint-playback duplicate
suppression use the shared SQLite-style adaptive PageBitvec. The nested
savepoint stack remains a typed `std::vector`, matching SQLite's ordered
resizable savepoint array without importing its C ownership.

Allocation failure while publishing membership returns `kOutOfMemory` and
makes the transaction failed. A record may already have been appended, but
the database page has not yet been authorized for mutation; full rollback
remains safe and duplicate journal content is not exposed to the caller.

### Savepoint semantics

`CreateSavepoint(current_page_count)` records the current logical page count
and asks the backend to record its current main- and sub-journal boundaries.
Savepoint operations remain available across journal-sync and spill epochs
while the transaction is active.

Savepoints are nested:

- releasing a savepoint also releases every newer savepoint;
- rolling back to a savepoint releases every newer savepoint but retains the
  target savepoint;
- the target membership set remains after rollback so later mutations still
  refer to the same savepoint-entry image;
- pages above the savepoint's original page count are removed by playback
  truncation rather than page-image restoration; and
- identifiers carry opaque transaction provenance in addition to their local
  sequence number, so stale, foreign, or already released identifiers return
  `kMisuse` without calling the backend.

Each savepoint also starts eligible to rewind the logical subjournal record
count when it is released. If a later subjournal image is required first by an
older savepoint, every newer savepoint becomes ineligible because rewinding at
that newer boundary would discard an image still needed by the older
savepoint. Release passes this coordinator-owned decision to
`DoReleaseSavepoint(id, rewind_subjournal)`. This mirrors SQLite's
`bTruncateOnRelease` rule without requiring per-record backend metadata.

If main-journal records have been appended since the last journal sync,
savepoint rollback synchronizes the main journal before target playback.
This preserves recovery if target playback writes the database and a second
failure occurs. The sub-journal is not a crash-recovery artifact and does not
require a durability sync.

### Commit-order state machine

```cpp
enum class JournalTransactionState {
  kActive,
  kFailed,
  kFinished,
  kError,
};
```

`JournalTransaction::Begin()` calls `DoBegin()` and publishes a transaction
only after the backend succeeds. An active transaction also tracks three
ordering facts:

- `main_journal_dirty`: the backend has main-journal transaction metadata or
  page records not covered by a successful journal sync; this starts true
  because `DoBegin()` creates the initial transaction metadata;
- `database_may_be_modified`: a database write, truncate, or extension has
  been authorized and may have changed persistent bytes; and
- `database_synced`: all authorized database mutations completed and the
  database then completed the final sync required by pager durability policy.

The normal commit path is:

1. `CapturePage()` zero or more times. A successful main-journal append sets
   `main_journal_dirty` and clears `database_synced`. A sub-journal append
   does not affect main-journal durability.
2. `SyncJournal()` calls `DoSync()` when `main_journal_dirty` and then clears
   `main_journal_dirty`.
3. Before each database page write,
   `AuthorizeDatabaseWrite(page_number)` verifies that the journal is
   synced and that every transaction-start page sharing the physical sector
   is present in transaction membership. It then sets
   `database_may_be_modified` and clears `database_synced`. Savepoint
   membership is not required merely to spill a page that was dirtied before
   the savepoint and has not changed since; writing those current bytes
   preserves the savepoint-entry state.
4. Before a truncate or extension, the future pager performs the equivalent
   page-image proof for every page whose bytes may be needed by transaction
   rollback or any active savepoint, then calls
   `AuthorizeDatabaseResize(rollback_pages)`. The coordinator verifies each
   listed page against every applicable membership set and enforces the sync
   state, while the pager owns the format-aware proof that the list includes
   every tail page requiring exact restoration.
5. After all database mutations succeed and the final database sync required
   by pager policy completes, `MarkDatabaseSynced()` requires a clean main
   journal and a prior database authorization, then sets `database_synced`.
6. `Commit()` calls `DoFinalizeCommit()` and enters `kFinished`.

The physical sector cohort for a database page is computed from the page and
sector sizes in `JournalTransactionInfo`. If the sector is no larger than a
page, only that page is checked. If a sector contains multiple pages, every
page in that sector that existed at transaction entry must already be in the
main journal, including otherwise-unmodified neighbors and old pages sharing
a sector with a newly appended page. The reserved locking page is skipped
when enumerating the cohort.

Calling database-write authorization while `main_journal_dirty`, authorizing
an uncaptured sector cohort, or calling `Commit()` before database
synchronization returns `kMisuse` without backend I/O.

The future pager calls `ReportDatabaseFailure(error.code())` after any failed
database write, truncate, extension, or sync. This latches `kFailed`, clears
commit eligibility, and leaves only full rollback available while the pager
returns the original VFS error unchanged.

Page capture and savepoint work remain legal after earlier database writes.
If a new main-journal original image is appended, `main_journal_dirty`
becomes true again and no further database write is authorized until another
`SyncJournal()` succeeds. These repeated sync epochs provide the ordering
required for Node 35 dirty-page spill without assigning cache replacement
policy to this node.
Sub-journal-only capture does not force a main-journal sync and does not block
a database write whose protecting main-journal prefix is already durable.

### Playback and recovery

`JournalPlayback` yields a validated playback header and a sequence of
borrowed page images:

```cpp
enum class JournalPlaybackKind {
  kTransactionRollback,
  kSavepointRollback,
  kHotRecovery,
};

struct JournalPlaybackInfo {
  JournalPlaybackKind kind;
  ByteCount page_size;
  std::uint32_t original_page_count;
};

class JournalPlayback {
 public:
  virtual ~JournalPlayback() = default;
  virtual std::optional<JournalPlaybackInfo> info() const noexcept = 0;
  virtual Result<std::optional<JournalPageImage>> Next() = 0;
};

class JournalRecoveryTarget {
 public:
  virtual ~JournalRecoveryTarget() = default;
  virtual Status PreparePlayback(JournalPlaybackInfo info) = 0;
  virtual Status ResizeDatabase(std::uint32_t page_count) = 0;
  virtual Status RestorePage(JournalPageImage image) = 0;
  virtual Status SyncDatabase() = 0;
  virtual Status CompletePlayback(JournalPlaybackInfo info) = 0;
};
```

The page bytes returned by `Next()` remain valid until the next `Next()` call
or playback destruction. The concrete backend validates record boundaries
before yielding an image.

`info()` is empty when transaction or hot-recovery playback finds no usable
first journal header. That tolerated outcome performs no target preparation,
resize, or page restoration, but still permits rollback finalization so the
invalid journal does not remain hot. Savepoint playback must always have
metadata; an empty savepoint `info()` is an internal contract violation.

After a valid first header, an incomplete or unrecognizable later header or
record, illegal main-journal page number, or checksum mismatch terminates the
cursor successfully at the last valid record, matching SQLite's tolerated
crash-tail behavior. The concrete SQLite backend ignores later header geometry
fields, as specified by ADR-0041. Actual VFS read failures remain errors.
Savepoint records are generated and consumed by the same process; an invalid
savepoint record is an internal contract violation.

The shared playback driver performs operations in this order:

1. inspect the optional playback metadata and, when present, validate it;
2. call `PreparePlayback()` so the target can invalidate or detach stale
   cache state;
3. restore the original logical size with `ResizeDatabase()`;
4. consume records, first skip every image whose page number is greater than
   `original_page_count`, then call `RestorePage()` for each remaining
   transaction or hot-journal record and once per distinct remaining page
   number during savepoint playback;
5. for transaction rollback and hot recovery, call `SyncDatabase()`;
6. call `CompletePlayback()`; and
7. destroy the playback cursor;
8. only for full rollback or hot recovery, call
   `DoFinalizeRollback()`.

For savepoint rollback, cursor destruction occurs before
`DoCompleteSavepointPlayback()`. When metadata is absent for transaction
rollback or hot recovery, steps 2 through 6 are skipped, the cursor is
destroyed, and rollback finalization is still attempted.

Savepoint size filtering runs before duplicate tracking, so a shared
sub-journal image for a page created after an older savepoint cannot recreate
that page after the target has been truncated. Savepoint playback then
suppresses duplicate page numbers because its cursor may combine main- and
sub-journal records; backends must yield the savepoint-entry image first.
Transaction rollback and hot recovery stream their unique main-journal
records without an additional page-membership allocation, matching SQLite's
constant-memory playback path.

Savepoint playback retains the journal and omits the database sync because
the write transaction remains active and the durable main journal still
protects any database pages already written. The backend completes its
savepoint bookkeeping only after target playback succeeds. Successful
savepoint playback conservatively sets `database_may_be_modified`, clears
`database_synced`, and therefore requires another flush and pager-policy
database sync before commit, even if the target restored only cached pages.

`RecoverHotJournal(backend, target)` is independent of a live
`JournalTransaction`. It performs:

1. `DoPrepareHotRecovery()`;
2. `DoOpenHotPlayback()`;
3. full playback including database sync; and
4. `DoFinalizeRollback()`.

Live full `Rollback()` first calls `DoSync()` when the main journal is dirty,
then opens transaction playback. A sync or cursor-open failure occurs before
target preparation and therefore remains retryable in `kFailed`. This
pre-playback sync ensures a second crash during rollback can use the same
durable journal content.

Lock acquisition, lock downgrade, and cache ownership remain pager
responsibilities. The future pager must hold EXCLUSIVE before recovery and
must not expose reads until this function succeeds.

If playback metadata or an image that has already passed concrete-backend
decoding violates the semantic contract, including a yielded page zero or
reserved locking page, the driver returns `kInternal`.
Tolerated rollback-journal tail damage is converted to normal cursor
completion by the concrete backend and is not exposed as `kCorruption`.

### Error states

Invalid caller order or values return `kMisuse` and do not change state.
Backend, target, or allocation failures preserve their typed error.

- Capture, savepoint, or journal-sync failure enters `kFailed`. Only full
  `Rollback()` remains allowed.
- `ReportDatabaseFailure()` enters `kFailed`; `MarkDatabaseSynced()` and
  `Commit()` are rejected afterward.
- Savepoint playback failure enters `kFailed`, even if target restoration
  started. The target state may be inconsistent, but full transaction
  rollback remains the only permitted operation and restores from the main
  journal.
- Failure to open full-transaction playback before `PreparePlayback()` keeps
  the transaction in `kFailed` so full rollback may be retried.
- Once full-transaction playback has called `PreparePlayback()`, any cursor,
  target, database-sync, completion, or rollback-finalization failure enters
  persistent `kError`, because cache or database consistency is uncertain.
- Successful full rollback enters `kFinished`.
- Commit finalization failure and semantic backend inconsistency enter
  persistent `kError`.
- In `kFailed` and `kError`, the coordinator stores the first `ErrorCode`,
  not a copy of the owning `Error` string. The operation that observes a
  backend or target failure returns that original error by move. Later
  rejected operations synthesize a same-code state error and make no
  allocation guarantee.

`RecoverHotJournal()` returns any preparation, cursor, target, sync, or
finalization error without making the journal non-hot. The owning pager
enters its persistent error path and may retry recovery only after discarding
in-memory state and reacquiring the required locks.

Busy database-lock acquisition is outside this contract and remains
retryable pager behavior. It does not create a journal transaction and does
not poison journal state.

Destruction performs no hidden I/O. A caller abandons a failed or errored
object only after propagating the error and invalidating the owning pager
state. Any remaining durable journal is intentionally left available for hot
recovery.

### Scope

This node pins:

- rollback-style original-page capture;
- nested savepoint page membership;
- commit-phase ordering;
- transaction, savepoint, and hot-recovery playback boundaries;
- duplicate suppression during savepoint playback;
- typed failed and persistent-error states; and
- deterministic fake-backend and fake-target tests.

It does not implement:

- rollback-journal bytes, checksums, headers, sidecar paths, or file deletion;
- VFS reads, writes, locks, synchronization, or directory barriers;
- cache pins, dirty pages, page movement, spill, or database writes;
- cache replacement policy or selection of which dirty page to spill;
- writable-pager lock transitions;
- WAL frames, WAL savepoint data, checkpoints, or shared memory;
- super-journals or attached-database atomicity;
- PERSIST, TRUNCATE, MEMORY, OFF, or atomic-write optimizations; or
- transaction SQL semantics and connection-level savepoint names.

The first concrete backend is DELETE mode with SQLite's
`synchronous=FULL` publication protocol. ADR-0041 distinguishes that protocol
from the stronger VFS full-sync flag.

## Validation plan

Tests use a scripted fake backend, fake playback cursors, and a fake recovery
target. They cover:

- transaction option, page-number, image-size, state, and savepoint-id
  validation;
- exact main-journal and shared sub-journal capture decisions;
- no redundant sub-journal append on a page's first transaction mutation;
- sub-journal capture for a transaction-new page that existed at a later
  savepoint;
- sub-journal-only capture not dirtying the main-journal sync epoch;
- transaction and per-savepoint capture membership;
- nested release and rollback-to behavior;
- pages created after transaction or savepoint entry, including a page
  captured for a newer savepoint and skipped when rolling back an older one;
- exact journal-sync, database-mutation, database-sync, and finalization
  ordering;
- repeated capture, sync, and database-write epochs for deterministic spill;
- spilling a page dirtied before a savepoint without requiring a redundant
  savepoint image;
- sector sizes larger than page size, including an otherwise-unmodified
  neighboring page and a new page sharing a sector with old pages;
- sector cohorts adjacent to the reserved locking page without capturing or
  replaying that page;
- explicit database write, truncate, extension, and sync failure latching;
- recoverable backend failures followed by successful full rollback;
- retryable pre-playback full-rollback failure, rollback-only savepoint
  playback failure, and persistent post-mutation playback failure;
- transaction rollback, savepoint rollback, and hot-recovery event order;
- database synchronization before full rollback finalization;
- no database synchronization or journal finalization for savepoint rollback;
- a prior `MarkDatabaseSynced()`, successful rollback-to-savepoint, and
  rejection of immediate commit until another database sync is reported;
- duplicate savepoint playback records being applied once while transaction
  and hot-journal playback remain streaming;
- playback records above the target page count being skipped before
  duplicate tracking;
- incomplete records, invalid headers, illegal page numbers, and checksum
  failures terminating rollback-journal playback at the last valid record;
- an unusable first header causing no target operation but still permitting
  tolerated rollback finalization;
- invalid playback metadata and images;
- target failures leaving the journal unfinalized;
- incomplete-cursor and backend errors propagating unchanged; and
- no allocation for duplicate capture, successful state notification, and
  transaction or hot-journal playback after cursor setup.

The tests are written red-first. The complete fast unit tier, format checks,
sanitizers, and project-graph validation remain required. Real journal golden
vectors, torn-write tests, SQLite interoperability, and VFS crash boundaries
belong to the concrete rollback-journal node.

## Consequences

- The writable pager receives one explicit authorization boundary before
  every database write, including a check that the complete affected
  filesystem sector is rollback-safe, and cannot finalize a commit before
  database sync.
- The rollback backend can evolve its file layout implementation without
  exposing offsets, file handles, or checksum details to the pager.
- Savepoint membership and playback ordering are tested before real storage
  makes failures difficult to isolate.
- Hot recovery reuses the same target and playback contracts as live
  rollback while preserving its required pre-playback journal sync.
- Repeated journal-sync epochs support deterministic dirty-page spill without
  moving cache replacement policy into the journal layer.
- Page membership and savepoint duplicate suppression use SQLite's adaptive
  bit-vector mechanism through a shared typed storage primitive.
