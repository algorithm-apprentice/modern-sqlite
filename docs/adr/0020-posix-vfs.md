# ADR-0020: POSIX Virtual File System

- Status: Accepted
- Date: 2026-10-03

## Context

ADR-0019 defines the operating-system boundary required by the pager, rollback
journal, WAL, and deterministic tests. The first concrete backend must support
Linux and macOS without weakening SQLite's short-I/O, durability, database
locking, or shared-memory behavior.

The implementation cannot be a thin collection of independent file
descriptors. Traditional POSIX `fcntl(F_SETLK)` locks belong to a process, and
closing any descriptor for the same inode can release every lock held by that
process. Multiple Modern SQLite connections in one process therefore need
shared inode state, local lock accounting, and deferred descriptor close.

WAL shared memory has the same process-local complication. Connections share
one `-shm` descriptor and mapping set, while each connection retains its own
shared and exclusive lock masks. The first process to attach after all prior
users have exited must also clear stale shared memory before using it.

Pinned SQLite references:

- `src/os.h:85-166` defines database-lock semantics and the PENDING,
  RESERVED, and 510-byte SHARED lock ranges.
- `src/os_unix.c:1350-1575` explains robust close and deferred descriptors.
- `src/os_unix.c:1672-2289` implements reserved-lock inspection and the
  SHARED, RESERVED, PENDING, and EXCLUSIVE transition protocol.
- `src/os_unix.c:3435-3730` implements retrying positioned reads and exact
  writes.
- `src/os_unix.c:3760-4035` implements normal/full synchronization,
  directory synchronization, truncation, and file-size queries.
- `src/os_unix.c:4485-5550` implements WAL shared-memory files, DMS
  initialization, mappings, process-local lock accounting, barriers, and
  cleanup.
- `src/os_unix.c:6300-7218` implements file creation, read-only fallback,
  delete-on-close, deletion, access, canonical paths, randomness, sleep, and
  wall time.

Modern LevelDB references:

- `docs/adr/0011-filesystem-contracts.md` documents exact writes, interrupted
  syscall retry, close semantics, explicit directory synchronization, and
  process-local lock protection.
- `src/platform/posix_file_system.cc` supplies proven error mapping,
  close-on-exec, offset, sync, and directory-sync patterns.

## Decision

- Add public `PosixVfs`, a final implementation of `Vfs`. The backend remains
  an ordinary object rather than a global registration mechanism.
- Support Linux and macOS using `open`, `pread`, `pwrite`, `ftruncate`,
  `fstat`, `fsync`, `fdatasync`, `fcntl`, `unlink`, `mmap`, `munmap`,
  `nanosleep`, and related POSIX calls.
- Retain paths as native byte strings. Named opens resolve existing symlinks
  to a canonical absolute path after opening so inode aliases share lock and
  `-shm` state. Pathless temporary files use `mkstemp`, mode `0600`, and
  immediate unlink.

### Positioned I/O and file lifecycle

- Positioned reads retry `EINTR` and continue after positive partial reads
  until the destination is full, EOF is reached, or an error occurs. The
  public `File` wrapper performs the required short-read zero fill.
- Positioned writes retry `EINTR`, continue after positive partial writes,
  and fail explicitly on a zero-byte write with data remaining.
- Each syscall request is capped at `SSIZE_MAX`.
- Ranges and sizes that exceed signed POSIX `off_t` return `kTooLarge`.
- Read-only files reject writes, truncation, and write-lock upgrades with
  `kReadOnly` before issuing a syscall.
- File destruction releases mappings, locks, and descriptors on a best-effort
  basis. It does not imply synchronization.
- `O_CLOEXEC` is used when available and emulated with `FD_CLOEXEC`
  otherwise. `no_follow` maps to `O_NOFOLLOW`.
- Named delete-on-close files are unlinked immediately after opening, matching
  normal POSIX SQLite behavior while preserving access through the descriptor.

### Durability

- Normal data-only sync uses `fdatasync` on Linux. Normal metadata sync uses
  `fsync`. macOS uses `fsync` for normal synchronization.
- Full sync on macOS first attempts `F_FULLFSYNC` and falls back to `fsync`.
  Other supported systems use `fsync`.
- Newly created main journals, super-journals, and WAL files retain an open
  parent-directory descriptor. Their first successful file sync is followed
  by directory sync before success is returned.
- `Delete(path, kYes)` unlinks the file and then synchronizes its parent
  directory. Directory-open, sync, and close failures remain explicit.
- Main-journal and WAL creation attempts to inherit the corresponding
  database file's permission bits. Super-journals have no associated database
  path and use the normal creation mode. Delete-on-close files use mode
  `0600`; other new files use mode `0644`, subject to the process umask.

### Database locking

- A process-global registry is keyed by `st_dev` and `st_ino`, not pathname.
  It coordinates every `PosixVfs` instance in the process.
- Database locks use SQLite's fixed byte ranges:
  - PENDING at `0x40000000`;
  - RESERVED at `0x40000001`; and
  - the 510-byte SHARED range beginning at `0x40000002`.
- SHARED temporarily read-locks PENDING, read-locks the SHARED range, and then
  releases PENDING.
- RESERVED write-locks the RESERVED byte while retaining SHARED.
- Upgrading RESERVED to EXCLUSIVE first write-locks PENDING. If existing
  readers prevent EXCLUSIVE, the connection remains PENDING so no new readers
  can enter before a retry.
- EXCLUSIVE write-locks the complete SHARED range.
- Downgrades restore a SHARED read lock before releasing PENDING and RESERVED.
- Process-local shared counts and the single higher-lock owner prevent classic
  POSIX locks from falsely succeeding against sibling connections.
- Closing a descriptor while any sibling lock remains defers the close until
  the process releases its last lock for that inode.
- `HasReservedLock` combines process-local state with `F_GETLK`, because
  `F_GETLK` does not report locks held by the calling process.
- Reusing inherited VFS objects after `fork()` is outside this node's
  contract. Subprocess tests may use raw descriptors to probe parent-held
  locks without invoking the inherited VFS objects.

### Shared memory

- Main database files use a same-directory `<database>-shm` sidecar opened
  read-write with no-follow behavior.
- One process-wide shared-memory state exists per database inode. It owns the
  sidecar descriptor, mappings, aggregate lock counts, and DMS lock.
- The first process to attach obtains an exclusive lock on byte 128, truncates
  stale shared memory, and downgrades to a shared DMS lock. Later processes
  join the shared DMS lock.
- WAL lock bytes start at offset 120 and cover SQLite's eight slots.
  Per-connection masks and aggregate process counts prevent a sibling
  connection from bypassing an incompatible lock.
- Region extension allocates through the requested end before mapping, so a
  later page touch does not discover disk exhaustion as an unexpected
  `SIGBUS`.
- The first mapping fixes the region size for that database inode. A later
  request with a different region size is rejected as misuse.
- Mappings tolerate region offsets that are not system-page aligned by
  mapping from the preceding page boundary and returning the requested
  subview.
- Shared-memory barriers use a sequentially consistent C++ thread fence.
- Unmap releases the connection's locks. A delete request is applied after
  the final local connection detaches.
- Read-only shared-memory mappings are deferred because the current contract
  returns a mutable view and cannot simultaneously return SQLite's
  `READONLY_CANTINIT` signal.

### Other VFS services

- `FullPath` uses error-code-based weak canonicalization so a missing final
  component is allowed while existing symlinks are resolved.
- Access checks return `false` for ordinary missing or permission-denied
  results and reserve errors for unexpected operating-system failures.
- Randomness fills every requested byte from `getentropy` in bounded chunks.
- Sleep retries interruption and uses a steady-clock deadline so reported
  elapsed time is never less than requested.
- Current time is `system_clock` truncated to milliseconds.
- File properties report a 4096-byte sector and SQLite's default
  powersafe-overwrite plus subpage-read capabilities. Atomic-write
  capabilities are not claimed without platform evidence.
- Generic runtime syscall replacement remains deferred. Tests use real
  temporary files, subprocess lock probes, and the deterministic abstract VFS
  fakes from ADR-0019.

## Error mapping

- Lock contention maps to `kBusy`.
- Read-only mutations map to `kReadOnly`.
- Permission failures map to `kPermissionDenied`.
- `ENOSPC`, `EDQUOT`, and write-side `EFBIG` map to `kFull`.
- Unrepresentable POSIX offsets map to `kTooLarge`.
- Missing open targets map to `kCannotOpen`; missing delete targets map to
  `kNotFound`.
- Other syscall failures map to the most specific existing error and otherwise
  to `kIo`.

## Consequences

- Pager and journal code can use one production VFS on both supported
  platforms without depending on C method tables.
- Multiple in-process connections obey the same locking rules as separate
  processes despite classic POSIX lock ownership.
- Journal creation and deletion retain explicit crash-durability boundaries.
- WAL has compatible shared-memory mappings and lock slots before the WAL
  engine node is implemented.
- Successful positioned I/O performs no heap allocation after open.
- Network filesystem locking variants, read-only WAL shared memory,
  memory-mapped database-page fetch, batch atomic writes, and fork-safe live
  handle inheritance remain future decisions.
