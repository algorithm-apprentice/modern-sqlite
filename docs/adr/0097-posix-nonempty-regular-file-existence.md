# ADR-0097: POSIX Nonempty Regular-File Existence

- Status: Accepted
- Date: 2026-10-16

## Context

The implemented-engine reference audit's D3 reproduction identifies a
recovery difference in the production POSIX backend. Modern currently uses
`access(F_OK)` for `FileAccessQuery::kExists`. Pinned SQLite's Unix VFS uses
`stat` and reports a regular file present only when its size is positive.
It still reports existing directories and other nonregular files present.

This distinction is not a generic filesystem convenience. ADR-0041 admits
recovery of SQLite-created individual journals with super-journal trailers,
even though attached-database writing remains deferred. A valid trailer
whose named super-journal is absent suppresses rollback. Treating an empty
regular placeholder as present can instead restore the old database image.

An independent reproduction constructs journal bytes over two SQLite-created
database images and compares production POSIX recovery with the pinned
default native VFS. A nonempty super-journal restores row `[1]` in both
engines. A missing super-journal retains `[1,2]` in both. For an empty
super-journal, native retains `[1,2]`, while Modern restores `[1]`.
Native reads the final Modern-produced image in every case.

The existing canonical mechanisms are:

- `Vfs::Access`, which validates paths and queries before backend dispatch;
- `PosixVfs::DoAccess`, with interrupted-call retry and shared access-error
  classification;
- `RollbackJournal`'s existing validated trailer and VFS access check;
- Pager's existing journal/WAL presence checks and recovery publication;
- temporary-directory, positioned-I/O, cold-journal, and custom-memory-VFS
  test helpers; and
- the inherited golden journal fixtures, native contracts, crash tests,
  compiler/sanitizer matrix, and fixed-work controls.

Custom VFS implementations need not use the Unix existence policy. In
particular, the existing memory-VFS test intentionally reports an empty
super-journal present and permits rollback. Preserve that contract.

## Decision

### 1. Correct only the production POSIX existence query

For `FileAccessQuery::kExists`, call `stat`, following symlinks as native
does. On success, return:

```text
not a regular file OR file size is positive
```

A newly created or truncated zero-size regular file is therefore absent for
this query even while its directory entry and open handle remain valid.
Nonempty and sparse regular files remain present. A directory or FIFO is
not rejected merely because its reported size is zero. A symlink takes its
target's classification; a dangling symlink is absent.

This is a backend query policy, not deletion, truncation, or an assertion
that a file can be opened. Readability and read-write queries continue to
use `access(R_OK)` and `access(R_OK | W_OK)`, including for empty files.
The public `Vfs` contract and custom backends remain unchanged.

### 2. Reuse the existing retry and explicit-error path

Keep one retry/error-classification path in `PosixVfs::DoAccess`. Select
`stat` for existence and `access` for the other two queries. Retry `EINTR`.
Retain the existing false results for `ENOENT`, `ENOTDIR`, `EACCES`, and
`EROFS`. Unexpected failures retain the canonical `SystemError` mapping
and identify the syscall that failed.

Pinned native Unix access returns a false existence result for any failed
`stat`. Modern deliberately retains ADR-0020's stronger explicit reporting
of unexpected operating-system errors, such as a symlink loop. Do not
broaden ordinary negative results or hide these errors as part of this
correction.

The implementation adds no owning container, allocation, cache, file handle,
recovery state, format encoder, or syscall replacement mechanism.

### 3. Leave recovery ownership and ordering unchanged

Do not add a super-journal-specific size check to journal or Pager code.
Their existing call to the supplied VFS determines presence. The POSIX
correction consequently covers both direct existence queries and all three
current production consumers: journal presence, WAL presence, and validated
super-journal presence.

For a cold journal with a valid trailer, recovery remains:

1. inspect the individual journal through the existing hot-recovery path;
2. validate its super-journal metadata, including ADR-0095's exact reads;
3. consult the supplied VFS;
4. suppress page playback if that VFS reports the super-journal absent;
5. otherwise perform the existing before-image playback; and
6. finalize the individual journal and publish the snapshot only through the
   existing successful recovery path.

The super-journal itself is not modified or deleted by this correction.
All metadata-read, playback, synchronization, finalization, and snapshot
failure handling remains canonical. Attached databases, WAL execution,
multi-connection policy, and later DAG nodes are not admitted.

## Verification and delivery

This is one prerequisite correctness slice of `implement-advanced-sql`,
after PR #205's computed-rowid correction, PR #204's metadata correction,
and PR #203's behavior-preserving binder foundation. It does not begin
ADR-0092's expression-copy behavior or complete node 43.

Obtain GPT-5.6 Sol design approval before changing production code.
First add and observe a focused platform failure for empty regular-file
existence, with readable/writable and one-byte/truncation controls. Then
observe a cold-journal/Pager seam failure for an empty super-journal while
missing and nonempty controls retain their existing outcomes.

Correct only the canonical POSIX access implementation. Run the focused
tests and complete fast unit tier. Cover nonregular paths, symlink targets,
dangling links, missing and non-directory components, permission denial,
and unexpected access errors. Retain the custom-VFS empty-file playback
control and all existing journal, Pager, allocation, and crash evidence.
Use the real POSIX backend rather than changing a fake to simulate the fix.

Repeat the independent native missing/empty/nonempty image comparison
against the corrected library. Preserve the old failing evidence and
record final native-readable rows and integrity. Run the complete local
compiler/sanitizer matrix and the existing 68 isolated native
compatibility/performance contracts. Preserve all inherited workloads,
fixtures, profiles, fixed-work properties, allocation ceilings, thresholds,
timeouts, compiler flags, and resource controls.

This is not a performance optimization. Both old and new existence queries
issue one pathname syscall; no speedup is claimed. Existing performance
contracts, not a new benchmark or weakened budget, qualify non-target
behavior.

Require GPT-5.6 Sol implementation and exact-tree review plus all nine
exact-head hosted checks before normal squash merge. Verify the merged main
tree and clean only the confirmed merged branch.

### Focused development evidence

Both expected failures were observed against unchanged production at main
`4094f14a40940ded922daab559882af88e7e5992`: empty/truncated regular files
reported present, and the real-POSIX cold-journal seam restored byte `02`
instead of retaining the written byte `7f` for an empty super-journal.
Missing/nonempty masters and individual-journal finalization controls passed.

The canonical POSIX correction passes 24 focused tests and the complete
1,464-case Debug suite. Strengthened controls also verify exact whole
recovered images, file sizes, unchanged master presence, and the named
unexpected-error syscall. The existing custom-VFS empty-master test remains
byte-identical.

The independent pinned-native image comparison now matches all three
outcomes: `[1]` for a nonempty master and `[1,2]` for missing or empty
masters. Each final Modern image is byte-identical to its native control;
all six final images pass native `integrity_check`. These focused results
do not replace the remaining complete-matrix, static, exact-tree review, or
hosted delivery requirements above.

## References

- ADR-0019: Virtual File System Contracts
- ADR-0020: POSIX Virtual File System
- ADR-0041: SQLite-Compatible DELETE-Mode Rollback Journal
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0095: Exact Rollback-Journal Metadata Reads
- [Implemented-engine reference audit](../sqlite-reference-audit.md), D3
- Pinned SQLite `src/os_unix.c:6867-6898`
