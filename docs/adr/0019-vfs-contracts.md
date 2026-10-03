# ADR-0019: Virtual File System Contracts

- Status: Accepted
- Date: 2026-10-03

## Context

SQLite routes persistent I/O through `sqlite3_vfs` and
`sqlite3_io_methods`. The pager depends on positioned reads and writes,
truncation, synchronization, file size, database locks, path operations, and
device durability properties. WAL later adds shared-memory mappings and lock
ranges. Time, sleep, and randomness are also supplied by the VFS so tests can
control them.

The C ABI combines these operations with integer flags, caller-owned file
storage, generic file-control pointers, dynamic-library loading, and optional
method-table versions. Modern SQLite needs the same storage and durability
semantics without importing C ownership or untyped extension points.

The contract must also support deterministic fake files. Pager, journal, WAL,
busy-handler, and crash tests need to inject short reads, I/O failures, lock
conflicts, time, randomness, and durability events without using a real
filesystem.

Pinned SQLite references:

- `src/sqlite.h.in:603-733` defines open flags, device capabilities, lock
  levels, and synchronization modes.
- `src/sqlite.h.in:743-875` defines the file I/O and shared-memory method
  table and requires zero-filled short reads.
- `src/sqlite.h.in:1334-1611` defines VFS path, open, randomness, sleep,
  clock, access, and shared-memory lock contracts.
- `src/os.c:85-305` shows the core wrappers used by pager and WAL.
- `src/pager.c:1136-1223,2761-2810,4352-4427` shows lock transitions and how
  sector size, atomic-write, safe-append, sequential-write, and sync
  properties affect durability.
- `src/os_unix.c:5087-5104` shows that a non-extending shared-memory map may
  successfully return no region.

## Decision

- The VFS contract is a pure `platform` module. This node defines no POSIX
  syscalls, pager state, journal policy, page format, SQL behavior, or global
  VFS registry.
- `Vfs` and `File` use runtime polymorphism only at the operating-system
  boundary. Files are returned with unique ownership. A `Vfs` must outlive
  every file it opens.
- Public non-virtual front doors validate caller input and backend results
  before dispatching to protected implementation hooks. This keeps fake and
  POSIX implementations under one contract without repeating validation in
  every consumer.
- File offsets and file sizes are distinct unsigned 64-bit strong types.
  In-memory transfer lengths continue to use `ByteCount`.
- Positioned reads return the number of bytes obtained:
  - a complete read initializes the entire destination;
  - a short read is a successful result with a smaller byte count;
  - the public wrapper zero-fills the unread suffix before returning; and
  - non-short I/O failures return an error and leave destination contents
    unspecified.
- Positioned writes are all-or-error. Implementations retry interruptible or
  partial operating-system writes internally and never report success for a
  short write.
- Zero-length reads, writes, randomness requests, and sleeps complete without
  invoking the backend.
- Synchronization separates normal versus full barriers and data-only versus
  data-and-metadata flushing. File deletion separately states whether the
  containing directory must be synchronized before success is returned.
- Database locks preserve SQLite's ordered NONE, SHARED, RESERVED, PENDING,
  and EXCLUSIVE states:
  - lock requests may upgrade to SHARED, RESERVED, or EXCLUSIVE;
  - PENDING is an observable intermediate backend state and is never requested
    directly;
  - unlock requests may only downgrade to SHARED or NONE; and
  - reserved-lock inspection reports whether any participant holds
    RESERVED, PENDING, or EXCLUSIVE.
- File properties expose sector size and typed device capabilities matching
  SQLite's atomic-write sizes, safe append, sequential writes,
  undeletable-while-open behavior, powersafe overwrite, immutable media,
  batch atomic writes, and subpage reads. Atomic-size queries do not replace
  the caller's responsibility to satisfy the corresponding write alignment.
- Shared-memory support is attached to the database file:
  - regions are numbered, fixed-size borrowed mutable views;
  - a non-extending lookup may return no region without error;
  - mapped views remain valid until shared memory is unmapped or the file is
    destroyed;
  - lock ranges are limited to SQLite's eight slots, shared operations cover
    one slot, and exclusive operations may cover consecutive slots;
  - locks use explicit lock/unlock plus shared/exclusive modes; and
  - callers use the explicit barrier before relying on cross-process
    visibility.
- Open requests use typed file purpose, requested access, creation,
  exclusive-create, delete-on-close, read-only-fallback, and no-follow
  fields. A missing path is reserved for newly created, exclusive,
  delete-on-close temporary files. Returned access is explicit so a
  read-write request may intentionally fall back to read-only.
- Paths are borrowed only for the duration of a VFS call. Implementations
  copy any path state retained by an open file. Empty paths and embedded NUL
  bytes are rejected; temporary files use a missing path instead.
- `FullPath` returns an owned path. `Access` distinguishes existence,
  readability, and read-write directory capability.
- Randomness success fills the complete requested buffer. Sleep returns the
  actual duration and may not report less than requested. Current time uses a
  millisecond-resolution `std::chrono::system_clock` time point.
- Expected backend failures use `Result` and the existing error taxonomy.
  Invalid caller configuration is `kMisuse`, unrepresentable ranges are
  `kTooLarge`, malformed backend results are `kInternal`, lock contention is
  `kBusy` or `kLocked`, and operating-system failures retain the most
  specific available typed error.
- File destruction releases handles and locks but is not a durability
  boundary. Callers must observe explicit write, sync, truncate, delete, and
  unmap results.
- Memory-mapped file fetch/unfetch, dynamic-library loading, system-call
  replacement, generic `void*` file controls, global VFS registration, and
  batch-atomic-write commands are deferred until a consumer requires a typed
  contract.

## Consequences

- Pager and journal code can be tested against deterministic in-memory fakes
  before the POSIX backend exists.
- Short-read zero filling and option validation are centralized rather than
  trusted to every backend and caller.
- Durability-relevant file and directory barriers remain explicit and
  observable in tests.
- Shared-memory contracts are available for WAL without making WAL an early
  implementation dependency.
- Ordinary successful positioned I/O adds no allocation. Virtual dispatch is
  accepted at this syscall-dominated boundary and will be measured before
  optimization.
- Future C API compatibility will adapt these typed contracts to SQLite flags
  rather than exposing SQLite's untyped method tables inside the engine.
