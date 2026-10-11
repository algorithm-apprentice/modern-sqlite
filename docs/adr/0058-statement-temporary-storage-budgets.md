# ADR-0058: Statement-Owned Temporary Storage Budgets

- Status: Accepted
- Date: 2026-10-11

## Context

ADR-0057 requires a bounded statement-owned temporary-storage foundation
before aggregate syntax or execution begins. The current implementation is
bounded per capability, not per statement:

- `TemporaryStorageFactory` is session owned and directly creates every
  sorter, top-N relation, and keyed relation;
- each `RecordSorter` retains up to the full sorter threshold and owns its own
  spill file;
- each multi-level sorter merge may open a replacement spill file;
- file-mode `BoundedTopN` eagerly owns one ephemeral Pager, cache, file, and
  page-two index root;
- file-mode `EphemeralRelation` eagerly owns another ephemeral Pager, cache,
  file, and page-two index root; and
- memory-mode relations use an ordered vector without retained-byte
  accounting.

One capability is bounded, but a valid wide statement may open hundreds or
thousands of capabilities. The current design can therefore multiply:

- retained heap memory by capability count;
- ephemeral Pager caches by relation/top-N count; and
- live file descriptors by sorter/relation/top-N count.

That becomes observable before aggregate execution:

- wide DISTINCT/compound queries can exhaust file descriptors;
- later aggregate-local DISTINCT and ordering can exceed process memory before
  any one capability reaches its own threshold;
- one file or cache per capability makes the aggregate-term limit infeasible;
- reset and halt must coordinate shared poisoned storage; and
- changing the benchmark runner or immutable baseline evidence is not an
  acceptable workaround.

The existing contracts that must remain valid are:

- ADR-0054 stable sorter ordering, PMA merge, and key-before-payload schedules;
- ADR-0055 ephemeral Pager, encoded index records, top-N admission, and
  candidate-pending lifecycle;
- ADR-0056 keyed relation comparison and representative semantics;
- zero-LIMIT short-circuiting before temporary resource creation;
- typed OOM, `kTooLarge`, I/O, corruption, and misuse errors;
- no main-database journal, lock, sync, schema, or transaction effects; and
- cleanup on halt, error, reset, detach, finalize, and destruction.

This ADR changes ownership and resource accounting only. It adds no SQL
syntax, aggregate state, planner node, bytecode instruction, or persistent
format.

## Decision

### 1. Separate session configuration from statement runtime

`TemporaryStorageFactory` remains session owned. It borrows the session VFS
and main Pager and stores immutable `TemporaryStorageOptions`.

Add a move-only `StatementTemporaryStorage`. It borrows the factory and owns
all mutable scratch state for one VM execution:

```text
TemporaryStorageFactory
  -> immutable options and main Pager/VFS identity

StatementTemporaryStorage
  -> retained-memory accounting
  -> spillable-owner registry
  -> shared relation arena
  -> shared sorter arena
  -> live-file accounting
```

The VM creates statement storage lazily on the first temporary open. Merely
attaching an execution context or executing a zero-LIMIT short circuit creates
no statement storage, Pager, or file.

Sorters, relations, and top-N handles borrow `StatementTemporaryStorage`.
Every handle must close before statement storage is destroyed. The VM enforces
this order explicitly:

1. close cursors and all temporary capabilities;
2. destroy statement temporary storage; and
3. detach the execution context.

The same order applies to halt, error, reset, finalize, move assignment, and
destruction. Member declaration order is not the only lifetime guarantee.

### 2. Reuse the existing threshold as the version-1 statement budget

Do not add a new public setting in this version.

The effective value returned by
`TemporaryStorageFactory::sorter_memory_threshold()` becomes:

- the statement-wide retained heap-memory budget;
- the default per-sorter fairness threshold;
- the default per-relation migration threshold; and
- the cache-size input for the shared relation arena.

This preserves every existing benchmark configuration and workload identity.
A manifest that pins `sorter_memory_threshold` fully determines the new
resource behavior.

Memory mode and file mode use the same retained-memory budget. The difference
is whether registered owners may spill:

- memory mode returns `kTooLarge` when no retained owner can release enough
  memory; and
- file mode spills through the shared arenas before returning `kTooLarge`.

The threshold remains between one byte and 512 MiB. The existing derived
default from page size and main cache remains unchanged.

### 3. Account retained allocations, not encoded payload alone

Add one `TemporaryMemoryBudget` inside statement storage.

Account every allocation retained across VM instructions:

- `ByteBuffer::capacity()`;
- sorter and top-N entry nodes;
- relation vector capacity;
- run and extent metadata capacity;
- merge-reader record buffers and heap capacity;
- candidate-pending key/record buffers;
- copied current records retained until `Next()` or reset; and
- any future aggregate-local temporary owner.

Do not charge:

- stack storage;
- short-lived encoding scratch released within one instruction;
- VM registers, parameters, or ordinary result values;
- the main Pager cache; or
- the shared relation Pager cache, which has its own bound below.

Container growth is explicit. Before `reserve()`, node allocation, or owned
buffer retention, reserve the projected allocation-sized delta. After the
operation, reconcile with the actual capacity. On allocation failure, release
the reservation and return `OutOfMemory`.

All arithmetic is checked. Overflow returns `kTooLarge`.

The budget is a retained-memory bound, not an exact RSS cap. One operation may
temporarily hold encoding scratch and allocator metadata outside the account,
as ADR-0055 already permits for bounded B-tree working sets. If one record is
larger than the budget in file mode, do not retain it in memory: create the
required arena and stream/insert it directly. Memory mode returns `kTooLarge`.
When file-backed data is later positioned, the required drain working set may
exceed the nominal budget after ordinary pressure spilling has reclaimed
everything possible. This is one current keyed-relation or top-N queue record,
or at most one current sorter record per merge input. These buffers and fixed
merge arrays remain fully included in retained/peak diagnostics; the bounded
working-set reservation is not available in memory mode or for ordinary owner
growth.
Drain working sets remain bounded by merge fan-in, `maximum_value_bytes`, and
the existing lowering limits on simultaneously positioned temporary streams.

Expose diagnostics:

```cpp
ByteCount retained_bytes() const noexcept;
ByteCount peak_retained_bytes() const noexcept;
ByteCount overhead_bytes() const noexcept;
```

Add `EphemeralRelation::memory_usage()`. Capability `memory_usage()` accessors
report owner-charged retained bytes. Registration tables, free-extent
metadata, and other shared-arena allocations report separately as statement
overhead:

```text
sum(capability memory_usage) = retained_bytes
```

### 4. Register spillable owners through scoped borrowing

File-mode sorters and memory-first file-mode relations register as spillable
owners. Registration is move-only RAII and borrows the owner; the registration
cannot outlive it.

Each registered owner exposes internally:

```text
reclaimable_bytes()
spill_for_pressure()
registration_ordinal()
```

No callback owns a capability. No raw pointer is owning.

When a reservation would exceed the statement budget:

1. select the registered writable owner with the greatest reclaimable bytes;
2. break ties by the lowest registration ordinal;
3. invoke one pressure spill;
4. require the owner to release its charged memory through the ordinary
   accounting API; and
5. repeat until the reservation fits or no owner can reclaim memory.

An owner already inside a pressure callback is ineligible, preventing
reentrant selection. An owner may spill itself.

If spilling fails, mark statement temporary storage failed and return the
original typed error. The VM aborts the statement and performs uniform
cleanup. No other temporary capability may continue after the failure.

Per-capability thresholds remain fairness triggers: an owner spills itself
when it exceeds its threshold even if the statement still has capacity.

### 5. Bound physical temporary files to two lazy arenas

Version 1 permits at most two simultaneously live temporary files:

1. one transient database for keyed relations and file-mode top-N; and
2. one shared temporary PMA file for all external sorters.

Both are opened lazily. Live-file accounting reserves the corresponding slot
before `Vfs::Open` or `Pager::OpenEphemeral` and releases it on open failure or
arena destruction.

Expose:

```cpp
std::size_t live_file_count() const noexcept;
std::size_t peak_live_file_count() const noexcept;
```

The invariant is:

```text
0 <= live_file_count <= 2
```

No capability directly opens a VFS file after this migration.

### 6. Use one shared relation Pager and one root per logical relation

The relation arena lazily creates one ephemeral Pager with:

- the main Pager's current page size; and
- cache capacity derived from the existing threshold:

```text
threshold_pages = max(1, ceil(sorter_memory_threshold / page_size))
cache_pages = max(1, min(max(1, main_cache_pages), threshold_pages))
```

This is one query-wide cache bound, not one cache per relation.

The arena begins one read/write transaction, initializes page one once, and
keeps one `BtreeWriteSession`. Each spilled keyed relation or file-mode top-N
receives a separate index root and writer from that session.

Page two is required only for the first root in a fresh arena. Later roots are
ordinary valid root pages.

File-mode keyed relations start in the existing ordered memory
representation. They migrate when their fairness threshold or statement
pressure requires spill:

1. create a relation root;
2. insert retained records in comparator order with their keep/replace
   semantics;
3. admit the triggering record; and
4. release memory only after successful insertion.

File-mode top-N keeps ADR-0055's eager file-backend semantics: opening the
top-N creates its root immediately, but through the shared relation arena.
Candidate-pending buffers still use retained-memory accounting.

`Reset()` clears one root and preserves its sequence/root ownership.
`Close()` performs no fallible I/O: it releases cursors and moves the root
lease, including its current writer/comparison columns, into an arena-owned
retired pool. The next root acquisition clears one retired root through that
old writer, then reopens the same empty root with the new comparison columns.
Clearing returns child and overflow pages to the ephemeral freelist. If
clearing or reopening fails, the shared Pager is poisoned and the statement
aborts. This bounds root allocation by peak simultaneously live relation/top-N
handles rather than looped open/close count.

Any failure after ephemeral B-tree mutation may poison the shared Pager. That
failure is statement-wide. Close every dependent capability and destroy the
arena; do not promise savepoint-style recovery or continued sibling-root use.

### 7. Use one shared extent-managed PMA file

The sorter arena lazily opens one pathless delete-on-close temporary file.

Every sorter run is an owned extent:

```text
[begin, end, sorter ordinal, run ordinal]
```

The arena maintains checked, coalescing free extents plus a file high-water
offset. Allocation chooses the lowest-offset free extent that fits and appends
only when none fits, making offsets and fault schedules deterministic. Sorters
write and read only through arena methods.

Spilling a run:

1. sorts the in-memory run;
2. allocates one extent sized for the encoded records;
3. writes the run;
4. publishes the run metadata only after complete success; and
5. releases retained entries.

Merge fan-in remains 16. A merge:

1. allocates a nonoverlapping output extent whose maximum size is the sum of
   input extents;
2. reads inputs and writes output in the same shared file;
3. publishes the output run only after complete success; and
4. releases/coalesces input extents.

Reset/close releases a sorter's extents to the arena. If the free tail reaches
the high-water offset, trim logical high-water; physical truncation is optional
and never required for correctness.

Any PMA read/write corruption or I/O failure marks statement temporary storage
failed and aborts the statement.

Stable comparison, sequence/run ordinals, record encoding, key collations,
directions, NULL placement, and merge output remain ADR-0054 compatible.

### 8. Preserve capability APIs while changing their creator

`RecordSorter`, `BoundedTopN`, and `EphemeralRelation` retain their SQL-facing
lifecycles and typed operations.

Creation moves from:

```text
TemporaryStorageFactory::Create*
```

to:

```text
StatementTemporaryStorage::Create*
```

Migration is delivered in four implementation PRs:

1. **budget core**: add statement storage, accounting, registration, file
   leases, diagnostics, and direct unit tests without changing consumers;
2. **relation arena**: migrate keyed relations and VM relation creation;
3. **sorter arena**: migrate sorters and VM sorter creation; and
4. **top-N ownership**: migrate top-N and remove the obsolete factory creation
   and direct temporary-file APIs.

Each capability type has exactly one creator at every merged point. When a
type migrates, its factory method is removed in the same PR. The factory may
continue creating only capability types whose migration PR has not begun.
PR 4 removes the final direct creation and temporary-file methods. No
same-capability compatibility shim is retained.

### 9. Preserve reset and error phase

Opening statement storage, an arena, or a file remains lazy, preserving:

- zero-LIMIT pre-open behavior;
- prepare-versus-step error phase;
- skipped arm and skipped payload evaluation; and
- statements that never use temporary storage.

VM halt and successful completion close capabilities before deleting arenas.
VM error preserves the primary execution error; cleanup is best effort under
the existing destructor rules.

`Reset()` after success or error destroys the old statement storage. The next
execution creates fresh budgets, files, Pager state, roots, extents, ordinals,
and peak counters.

Automatic reprepare also destroys old statement storage before replacing the
VM program.

### 10. Amend ADR-0055 narrowly

ADR-0055 remains authoritative for:

- ephemeral Pager behavior;
- encoded key-prefix mutation;
- top-N admission and stable sequence;
- packed record format;
- candidate-pending lifecycle; and
- runtime LIMIT lowering.

This ADR supersedes only:

- one Pager/file/cache per file-mode top-N;
- the unconditional page-two root invariant;
- direct top-N ownership of Pager/session/writer; and
- direct temporary-file creation by each sorter.

No top-N SQL behavior changes.

## Verification

### Budget core

- exact reserve/release and peak accounting;
- checked overflow;
- allocation-capacity reconciliation;
- deterministic largest/oldest spill selection;
- reentrancy exclusion;
- memory-mode `kTooLarge`;
- spill-error poisoning;
- move/unregister lifetime; and
- zero live files before arena use.

### Shared relation arena

- no file before first migration/eager top-N root;
- one file and one Pager cache across many roots;
- first root page two and valid later roots;
- keep/replace/membership/delete parity before and after migration;
- threshold and global-pressure migration;
- clear/reset and freelist reuse;
- retired-root reuse across repeated open/close loops;
- multiple simultaneously spilled relations;
- top-N-independent relation behavior;
- poisoned-Pager statement abort;
- open/read/write/allocation failure cleanup; and
- retained/peak bytes and live-file diagnostics.

### Shared sorter arena

- many sorters sharing one file;
- one-run, multi-run, multi-level, and interleaved sorter extents;
- extent split, reuse, coalescing, and checked offsets;
- stable equal-key ordering;
- pressure-driven spill of another sorter;
- reset/close while other sorters remain live;
- fan-in reader memory accounting;
- read/write/short-I/O/corruption/OOM failures;
- no live handle above two with the relation arena also open; and
- unchanged ORDER BY benchmark CLI work and result digests.

### Shared top-N ownership

- memory/file parity for all existing top-N cases;
- multiple top-N roots in one arena;
- eager root but lazy arena file creation;
- candidate-pending accounting and cleanup;
- first and later root invariants;
- reset/close while relations remain live;
- compound per-arm top-N execution;
- all ORDER BY and DISTINCT/compound differential schedules; and
- unchanged committed workload and fixture identities.

Every implementation PR runs its focused tests and the complete fast unit
tier. PR 4 additionally runs Debug, Release, ASan/UBSan, GCC, clang-tidy,
model, fuzz, public session, and all existing performance-contract smoke and
baseline validation.

## Explicit deferrals

This ADR does not add:

- aggregate syntax or state;
- a public temp-store PRAGMA;
- a new user-visible threshold;
- durable temporary schemas;
- cross-statement scratch reuse;
- concurrent access to one statement arena;
- parallel sorter workers;
- shared cache across sessions;
- disk-space quotas; or
- recoverable savepoints inside a poisoned ephemeral Pager.

Those require their own accepted requirements.

## Consequences

### Positive

- Statement memory and file usage no longer multiply by temporary capability
  count.
- Existing sorter, relation, and top-N semantics remain reusable.
- Wide aggregate queries become feasible before aggregate execution is added.
- Benchmark configurations remain unchanged because the existing threshold
  fully determines version-1 budgets.
- One poisoned shared arena has an explicit statement-wide failure outcome.

### Negative

- Temporary-storage ownership moves from three independent implementations
  into one coordinated statement runtime.
- The PMA file needs an extent allocator and shared failure domain.
- Relation/top-N root lifetime is longer than individual handle lifetime.
- Four sequential implementation PRs are required before aggregate syntax.

## Rejected alternatives

### Raise the per-program temporary-capability limit

Rejected because count limits do not bound memory, cache, or file descriptors.

### Lower every per-capability threshold

Rejected because the product of capability count and threshold remains
unbounded and small single-capability queries regress unnecessarily.

### Keep one file per sorter and only bound relations

Rejected because aggregate-local ORDER BY can open as many sorters as
aggregate invocations.

### Use one B-tree for both relations and sorted runs

Rejected because it replaces ADR-0054's PMA merge mechanism without a measured
requirement and changes stable sorter failure/performance behavior.

### Add new public budget settings now

Rejected because the existing sorter threshold and cache settings already
provide deterministic version-1 inputs. New knobs are deferred until a real
configuration requirement exists.

### Preserve sibling capabilities after a poisoned arena mutation

Rejected because the ephemeral Pager has no rollback/savepoint contract.
Continuing would create success-shaped behavior over invalid scratch state.

## References

- ADR-0003: Layered Dependency Architecture
- ADR-0004: Errors, Ownership, and Runtime Boundaries
- ADR-0008: SQLite Performance Parity Strategy
- ADR-0019: Virtual File System Contracts
- ADR-0021: Page Cache
- ADR-0043: SQLite-Compatible B-Tree Mutation
- ADR-0045: Existing-Module SQLite Reference Alignment
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0054: SQLite-Aligned ORDER BY and External Sorter
- ADR-0055: SQLite-Aligned Ephemeral Pager and Spillable Top-N
- ADR-0056: SQLite-Aligned DISTINCT, VALUES, and Compound SELECT
- ADR-0057: SQLite-Aligned Aggregate Roadmap
