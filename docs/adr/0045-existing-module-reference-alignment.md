# ADR-0045: Existing-Module SQLite Reference Alignment

- Status: Accepted
- Date: 2026-10-06

## Context

ADR-0001 established a ground-up C++23 implementation rather than a
line-by-line C translation. ADR-0002 made SQLite's persistent formats,
transaction behavior, observable SQL semantics, and bidirectional
interoperability the compatibility contract. ADR-0003 then removed SQLite's
source-level dependency cycles.

That policy is correct, but it is not precise enough for compatibility-critical
mechanisms. Node 36 initially implemented a custom B-tree mutation algorithm
that matched the file format while diverging from SQLite's balancing,
ownership, scratch-lifetime, and failure-state machinery. Review showed that
format compatibility alone does not guarantee that independently designed
storage mechanisms will compose safely.

The same question therefore applies to the completed prerequisites: which
parts must reproduce SQLite's mechanism, and which parts should retain a
different internal design?

Blindly translating every completed module is not a safe answer:

- SQLite's parser actions mutate schema and emit VDBE work.
- The B-tree depends on VDBE record internals.
- Pager, cache, WAL, and connection state share concrete mutable structures.
- Recreating those boundaries would invalidate the accepted dependency DAG.

Ignoring internal mechanisms is also unsafe where mutation order, lifetime,
or failure state determines durability and interoperability.

The local pinned SQLite 3.54.0 checkout identified by the project manifest is
the only source oracle for this alignment work. Repository decisions do not
depend on a workstation-specific absolute path.

## Decision

### 1. Use three explicit alignment levels

Every module is assigned one of three alignment levels.

#### Level A: reference-faithful mechanism

Persistent-format and durability-critical code reproduces SQLite's:

- constants and bounded data structures;
- state transitions;
- mutation and synchronization order;
- ownership and scratch lifetimes;
- corruption checks;
- allocation and I/O failure cleanup; and
- recovery behavior.

The implementation uses Modern C++ types and the accepted dependency graph,
but algorithmic invention is not allowed in the first compatible version.

Level A applies to:

- binary database and journal coding;
- record encoding and comparison;
- VFS durability and locking;
- rollback-journal publication and playback;
- pager transaction state;
- B-tree page format, cursors, freelist, overflow, balancing, and root
  lifecycle; and
- WAL when that node begins.

#### Level B: observable SQLite semantics

SQL-facing modules preserve SQLite's supported observable behavior but do not
reproduce its source architecture.

Required evidence includes:

- differential rows and storage classes;
- primary error codes and failure phase;
- statement lifecycle and binding behavior;
- catalog interpretation;
- selected plan behavior where it is observable or performance-critical; and
- explicit unsupported-feature boundaries.

Level B applies to:

- text, values, affinity, collations, and functions;
- lexer, parser, and immutable syntax;
- catalog, binder, logical plan, optimizer, and lowering;
- typed bytecode and VM execution; and
- session and later public APIs.

#### Level C: Modern implementation infrastructure

Infrastructure with no persistent or externally observable identity may use a
different implementation when it preserves the contracts required by Levels A
and B.

Level C includes:

- typed errors and `Result`;
- RAII ownership wrappers;
- instrumentation;
- container implementation details;
- immutable AST and plan storage;
- internal identifiers; and
- test and benchmark tooling.

Level C is not permission to invent a second storage algorithm. A container or
helper moves to Level A when its behavior controls SQLite mutation order,
boundedness, recovery, or failure semantics.

### 2. Preserve the dependency DAG and Modern C++ ownership

Reference alignment does not restore SQLite's source-level cycles.

- Parsing remains pure.
- Planning remains separate from bytecode lowering.
- Record comparison remains below both B-tree and VM.
- Journal and cache remain below Pager.
- B-tree does not depend on VM or syntax types.
- Expected failures remain typed results.
- Owning raw pointers remain forbidden.

When SQLite combines several responsibilities in one structure, Modern SQLite
splits that structure across the accepted modules while preserving the
reference state machine at their seam.

### 3. Audit outcome for completed Nodes 1 through 35

The existing ADRs, implementation seams, golden tests, differential harness,
and local SQLite mappings produce the following disposition.

| Existing area | Alignment level | Disposition before continuing Node 36 |
|---|---|---|
| Bytes, errors, instrumentation | C | Retain. These are ownership and observability infrastructure, not SQLite persistent mechanisms. |
| Binary coding and database header parsing | A | Retain and continue using independent SQLite vectors. Varints, endian values, checksums, locking-page rules, and header fields are already defined from pinned source. |
| SQL values, affinity, collations, functions | B | Retain. Continue differential expansion instead of copying `Mem`, callback, or connection-global layouts. |
| Record codec and index comparison | A | Retain. Encoding, serial types, payload widths, and prefix comparison are shared format contracts below B-tree and VM. |
| VFS and POSIX backend | A | Retain. Lock bytes, process-local lock accounting, short I/O, sync, directory durability, and shared-memory rules are already mapped to `os_unix.c`. |
| Page cache | A at the pin/dirty/LRU seam; C for its hash container | Retain the intrusive pin, clean/dirty, soft-capacity, and writeback model. Keep the private `std::unordered_map` until matched evidence justifies SQLite's specialized hash table. Add only the in-place promotion and allocation-free rekey operations required by Pager and B-tree. |
| Read pager | A | Retain. Snapshot identity, hot-journal rejection, page-count rules, locking-page exclusion, cache invalidation, and read state follow `pager.c`. |
| B-tree page decoder and read cursors | A | Retain. Page geometry, four page kinds, payload formulas, overflow traversal, signed rowid search, index comparison, and the 20-level path are already source-mapped. |
| Storage diagnostics | A for format and ownership rules | Retain. It may use a Modern workspace, but ownership, freelist, pointer-map, reachability, and ordering checks remain SQLite-format checks outside the point-operation hot path. |
| Journal format backend | A | Retain. Header publication, sector cohorts, checksums, savepoint boundaries, streaming playback, crash-tail handling, and DELETE finalization already follow SQLite. |
| Journal transaction membership and savepoint duplicate suppression | A | Replace the current `std::unordered_set` implementation with the shared SQLite-style adaptive PageBitvec. This supersedes ADR-0040's measured-baseline deferral. |
| Writable pager | A | Retain its SQLite state machine and durable ordering. Complete sole-pin promotion, bounded page-number permutation, transaction content history, and their failure tests before B-tree mutation resumes. |
| Lexer through read session | B | Retain the acyclic Modern architecture. The committed differential harness, not SQLite's mutable parser/VDBE structures, is the compatibility gate. |
| Existing custom writable B-tree implementation | A | Replace. ADR-0044 remains the implementation authority. No custom split planner, ownership hash, PMR recovery scheme, or second-search deletion path survives. |

This audit does not claim that every supported SQLite behavior is complete.
It determines whether the existing architecture can compose with the
reference-faithful write path and identifies the current mechanism-level
divergences that must be removed first.

### 4. Share one SQLite-style PageBitvec below Pager and Journal

The adaptive bit-vector currently introduced for Pager content history moves
to a shared storage utility below both Journal and Pager.

It reproduces the Set/Test and recursive-storage mechanisms from
`src/bitvec.c`:

- one 512-byte conceptual node geometry;
- bitmap storage for small domains;
- sparse open-addressed hash storage;
- SQLite's collision-dependent subdivision threshold;
- recursive child ranges;
- one-based page numbers;
- no loss of previously set bits on allocation failure; and
- move-only Modern ownership.

An empty transaction or savepoint scope retains no page-set node because it
has no valid page number to record.

It is then used for:

- transaction main-journal membership, equivalent to `Pager.pInJournal`;
- each savepoint membership set, equivalent to
  `PagerSavepoint.pInSavepoint`;
- savepoint-playback duplicate suppression, equivalent to `pDone`; and
- Pager's transaction-scoped `pHasContent` history.

The nested savepoint stack remains a typed `std::vector`. SQLite also stores a
resizable ordered savepoint array; the vector does not replace a
durability-relevant set algorithm.

### 5. Keep the page-cache hash implementation unless evidence requires parity

SQLite's page-cache hash table is an internal lookup implementation. The
accepted Modern cache already reproduces the compatibility-relevant behavior:

- stable pinned frames;
- multiple read pins and one exclusive pin;
- intrusive unpinned clean recency;
- dirty-page retention;
- soft capacity; and
- pager-selected writeback.

The standard node-based map does not change database bytes, transaction
ordering, or failure recovery. Replacing it solely for source resemblance
would add risk without resolving a composition problem.

The private rekey operation uses node extraction to preserve frame addresses.
It is admitted only with tests proving:

- no frame or byte-view movement;
- no target collision;
- no exposed locking-page sentinel;
- complete validation and journal capture before the first rekey; and
- correct savepoint and full rollback.

A SQLite-style custom page hash remains a future performance change requiring
a matched cache/pager benchmark and its own reviewed decision.

### 6. Finish the lower storage alignment before B-tree mutation

Node 36 pauses structural writer work until this ordered prerequisite slice is
complete:

1. make PageBitvec a shared storage primitive and match `bitvec.c` branching;
2. replace journal transaction, savepoint, and playback sets with PageBitvec;
3. finish Pager read-pin promotion, page-number permutation, and content
   history;
4. run focused journal, pager, cache, rollback, crash, OOM, and sanitizer
   tests;
5. run the complete fast unit and read-compatibility tiers to prove no lower
   contract regression; and
6. review the complete lower-layer diff before resuming ADR-0044's writable
   cursor and `MemPage` work.

No higher DAG node starts during this alignment slice.

### 7. Require seam tests in addition to module tests

Each Level A boundary must have tests that cross the adjacent module seam.

For the current storage stack, the minimum seam evidence is:

- VFS short read and sync behavior through Pager or Journal;
- journal capture membership through Pager sector capture;
- savepoint rollback through Journal playback into Pager cache and database
  targets;
- cache pin promotion without eviction or reread;
- page-number permutation followed by savepoint and full rollback;
- Pager content-history survival across savepoint rollback;
- B-tree read cursors over SQLite-created files; and
- later, SQLite `integrity_check` over Modern-mutated files.

Passing isolated module tests is not sufficient when a persistent transition
crosses modules.

## Consequences

### Positive

- Existing compatible work is retained instead of rewritten indiscriminately.
- Storage mechanisms that determine durability now have one unambiguous
  reference-faithful policy.
- The journal, pager, and B-tree share SQLite's adaptive page-set mechanism.
- The acyclic architecture and Modern C++ ownership model remain intact.
- Integration failures are addressed at their seams before more writer code is
  added.

### Negative

- A completed journal-contract decision is reopened and its allocation tests
  must change.
- Node 36 pauses while lower storage code is hardened.
- Mechanism-level source mapping increases implementation complexity in the
  durability path.

### Deferred

- Replacing the page-cache map with SQLite's specialized hash table.
- Reproducing SQLite VDBE opcode layout or mutable parser actions.
- Source or ABI compatibility.
- Post-parity storage refactoring.

## References

- ADR-0001: Ground-Up C++23 Reimplementation
- ADR-0002: SQLite Compatibility Contract
- ADR-0003: Layered Dependency Architecture
- ADR-0008: SQLite Performance Parity Strategy
- ADR-0021: Page Cache
- ADR-0022: Read-Only Pager
- ADR-0023: B-Tree Page Decoding
- ADR-0024: Read-Only B-Tree Cursors
- ADR-0038: Pinned Read Compatibility Harness
- ADR-0040: Journal Contracts and Durable Ordering
- ADR-0041: SQLite-Compatible DELETE-Mode Rollback Journal
- ADR-0042: Rollback-Mode Writable Pager
- ADR-0044: Reference-Faithful SQLite B-Tree Mutation
- SQLite 3.54.0 `src/bitvec.c`
- SQLite 3.54.0 `src/pager.c`
- SQLite 3.54.0 `src/pcache.c`
- SQLite 3.54.0 `src/pcache1.c`
- SQLite 3.54.0 `src/btree.c`
