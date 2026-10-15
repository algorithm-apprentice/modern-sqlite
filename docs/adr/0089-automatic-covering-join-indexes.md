# ADR-0089: Automatic Covering Join Indexes

- Status: Accepted
- Date: 2026-10-16
- Module graph node: `implement-advanced-sql`

## Context

PR #199 merges ADR-0088's bounded canonical index-record comparison
prerequisite. Seek and current comparison no longer retain an unaccounted
copy of a covering suffix. The shared record validator, comparator, payload
walker, and complete required-chain error visibility remain authoritative.

The next measured gap is repeated equality access to an unindexed joined
column. On unchanged production, public execution of:

```sql
SELECT count(b.id)
FROM items a JOIN items b ON b.payload=a.payload
WHERE a.id<=32;
```

returns 32 but executes 676,012 VM instructions. Eight probes execute
184,372 instructions; the single-point control executes 20,504. Preparation
is excluded. The original production base is
`4cd23d1d534a7f1a2ceb2ea7205d4c1765b56f4b`; repeating the diagnostic after
the prefix prerequisite at `8923df7b02104a35e0cd9fa7b6c67dfb3909d35c`
produces the same counts. The complete instrumented fast tier is green.

Sixteen profile-validated SQLite 3.54.0 probes use the same frozen
4,096-row fixture. Native selects an automatic covering index for repeated
unindexed equality probes, guards construction with `Once`, and stores
equality columns, required covering columns, and a final rowid. The covering
projection probe stores `[payload, score, flag, rowid]`. Construction
precedes probe-key evaluation and includes NULL keys.

Native genuine RIGHT and FULL preservation does not select this automatic
index. With `a.id<=32` inside ON, the respective
`(count(a.id),count(b.id),count())` results are `(32,4096,4096)` and
`(4096,4096,8160)`. Bloom instructions there perform unmatched-row
bookkeeping, not automatic-source lookup. Native also adds Bloom filtering
to some automatic-index probes; that remains a measured follow-up, not a
prerequisite for removing repeated scans.

The canonical `EphemeralRelation` already owns encoded records, comparison
metadata, statement budgets, and shared-arena roots. Its memory backend is
an ordered `ByteBuffer` array: adverse insertion moves entries quadratically.
Its ordinary rewind begins a one-pass drain, not a reusable lookup lifetime.
Neither that drain nor reset can represent a repeatable automatic source.

## Decision

### 1. Deliver one end-to-end source-access slice

Admit automatic covering equality access to ordinary rowid-table sources in
comma/CROSS, INNER, and LEFT joins, including resolved USING and NATURAL
forms, only when the existing temporary configuration enables file spill.
Memory-mode SQL retains its existing table, rowid, and stored-index choices.
Exclude a core containing right-side preservation from automatic admission.
No new unsupported-SQL result is introduced.

Require at least one usable `=` or `IS` constraint and more than one
estimated prefix execution. Reuse own-ON preference, WHERE preservation
barriers, value-only key classification, earlier-source readiness,
comparison affinity, and effective registered collations. Existing
transparent wrappers and synthesized value-only coalesces keep their
current treatment. Computed/function/subquery keys, range-only keys, schema
tables, and WITHOUT ROWID tables do not gain this path.

Keep rowid-point priority and a selected constrained stored-index path.
Compare the automatic candidate only against the remaining repeated table
scan. This slice adds no syntax, join reordering, persistent index metadata,
partial construction, generic once opcode, hash join, or second executor.
RIGHT/FULL automatic replay and Bloom filtering require later measured
requirements and reviewed contracts.

### 2. Publish an actual physical automatic source

Add a physical automatic-index access kind and node. Its immutable metadata
names the actual core, source, table/root, selected `PhysicalIndexEquality`
terms, and required non-key covering columns. Do not manufacture a catalog
index, `IndexId`, schema entry, statement owner, or runtime cache.

Extract the existing core-owned required-source-value discovery from stored
covering analysis and reuse it for both paths. Reuse its source-value walker
and actual-core ownership rules for ON, WHERE, results, ordering, aggregate
inputs, and representative leaves. Do not scan the shared expression arena
or add another dependency walker.

Joined scopes publish the existing query-owned registered-collation names
even when no active source has a stored index. Automatic admission uses that
same registry; an empty registry is not permission to assume a collation.

Select one usable equality per catalog column through the canonical matcher,
retaining original conjunct positions. Use the chosen effective collation,
ascending order, and NULLS FIRST for each equality field. The record is:

```text
[equality columns..., required non-key columns in catalog order..., rowid]
```

The final rowid also represents an INTEGER PRIMARY KEY alias. Non-key fields
and the final rowid use canonical BINARY ascending comparison metadata.
The complete record is the unique key: `key_field_count == field_count`,
and comparison metadata covers every stored field. The separately retained
probe width is only the number of equality fields. Never truncate insertion
metadata to that probe width or merge distinct covering rows.

Enforce the existing record-field limit before admission. Recompute and
compare the complete physical selection through the same canonical chooser
during validation. The access is forward-only and advertises no delivered
ORDER BY.
Keep the single unchanged record-field ceiling in the shared SQL-value
contract, below both the compiler and the codec. The optimizer must not
include record encoders, decoders, or storage-format contracts merely to
obtain that ceiling.

### 3. Admit and cost only the configured file backend

Optimizer options do not grant file-spill permission by default. Callers
explicitly pass that capability from their execution factory; optimizing
without temporary storage must retain the existing repeated scan.

Add one narrow immutable optimizer input permitting file-backed automatic
relations. It is derived from the session's existing
`TemporaryStorageFactory::file_spill_enabled()` configuration, not a new SQL
or session setting. Pure optimizer code receives no factory, VFS, Pager,
temporary owner, or session type.

Thread this input through SELECT preparation in both read and write
sessions, statement optimization, every nested query, and canonical physical
validation. Retain it with the immutable physical owner so validation cannot
silently recompute a different backend. Callers constructing plans directly
must pass their execution configuration. Sessions retain their existing
file-mode default through that explicit input; tests cover both inputs and
both public sessions.
When permission is false, automatic admission is disabled and validation
rejects any automatic node. Generic relation prefix APIs still support both
existing backends; that does not enable a memory-mode SQL optimization.

The unchanged 32-probe join succeeds with a one-byte memory temporary budget.
Mode-only quadratic costing cannot prove retained capacity and can still
choose an index that deterministically fails `kTooLarge`. Do not replace that
successful scan with an optional-index capacity failure. Budget-aware memory
admission or a different memory construction mechanism requires a separate
measured requirement; neither is introduced here.

Choose joined accesses once in declared source order. Estimate prefix
executions `R` from preceding selected source-output estimates: a rowid point
contributes at most one; a stored index uses its existing estimator; a table
scan uses its canonical estimate. A preceding automatic source reuses the
default equality-prefix estimate. Do not recursively optimize a source
prefix for each later source.
Initialize retained prefix estimates only for an admitted joined scope; do
not allocate this state for unrelated aggregates or disabled automatic access.
The existing exact allocation contracts remain unchanged.

Reuse table statistics, affinity-width estimates, default equality-prefix
cardinality, IS-NULL adjustment, and saturating arithmetic. Extract the
shared equality-prefix estimate rather than copying it.

For source rows `N`, source width `T`, retained record width `W`, matching
prefix rows `M`, and `L = bit_width(max(N,1))`, compare structural work:

```text
repeated scan: R * N * T
source scan:   N * T
file build:    N * L * W
probes:        R * (L + M * W)
automatic:     source scan + file build + probes
```

The file term is valid because lookup-use insertion starts directly in the
shared B-tree arena, not in the ordered array. Costs are deterministic
admission estimates, not SQLite-identical costs or elapsed-time claims.
The existing ordered-memory backend is neither costed as logarithmic nor
selected for automatic SQL sources.

Reject `R<=1`, absent equalities, ties, and non-improving or saturated tied
costs. Do not change frozen benchmark configurations, workload identities,
thresholds, profiles, or baseline evidence.

### 4. Extend the canonical relation, not its representation

Add an immutable lookup-use descriptor purpose and separate lookup states:

```text
writing -> SealForLookup() -> lookup-ready
lookup-ready -> SeekPrefix(key) -> lookup-positioned / lookup-ready
lookup-positioned -> NextPrefix(key) -> lookup-positioned / lookup-ready
lookup-positioned -> ClearPosition() -> lookup-ready
any live state -> Reset() -> writing
any live state -> Close() -> closed
```

Ordinary mutable membership, keep/replace, deletion, rewind, and drain retain
their existing states and behavior. Lookup-purpose sealing is not an alias
for ordinary rewind. After sealing, direct mutation and ordinary drain are
misuse. Clearing an open writing relation leaves it writing; clearing an
ordinary positioned drain ends that drain. Closed and poisoned use reports
the existing typed error.

Logical open remains lazy. For a file-mode lookup relation, the first actual
insertion acquires a root through the existing shared arena before any
ordered-array insertion. Every subsequent insertion uses the same canonical
file backend. Empty sealed relations create no file. Ordinary file-mode
membership relations retain their existing memory-first pressure migration.
Memory mode retains its bounded ordered array and explicit `kTooLarge`.

This narrowly amends ADR-0058's memory-first policy for lookup-purpose
relations only. It prevents quadratic pre-migration construction and avoids
freezing a file-mode ordered array whose retained budget can no longer be
reclaimed by writable-owner pressure callbacks.

Prefix keys borrow `span<const SqlValue>` for one operation. Require a
nonempty prefix no wider than the complete comparison key. Reuse canonical
record/index comparison and the B-tree's lower-bound prefix seek.
Generalize the existing memory binary search for the two existing key forms
and first-equivalent prefix positioning; preserve its unique-complete-key
early return for ordinary membership and insertion. Do not add another
search algorithm, comparator, decoder, or retained probe-key container.

Seek returns the first equal row or lookup-ready. Prefix-next advances only
inside that equal prefix, otherwise clears the position. Factor/reuse
existing advancement and current-record loading. On file access, compare the
prefix before retaining the complete covering payload. ADR-0088 still
validates the complete required overflow chain during comparison.

Every current view is invalidated by seek, advance, clear, reset, close, move
assignment, and destruction. Clear borrowed views before their backing
buffer or page position changes. No pressure callback migrates positioned
storage. Existing retained-byte accounts, bounded file-mode current-record
reservations, one shared relation cache, two-arena file limit, poisoning, and
cleanup remain authoritative. No scan fallback follows a storage failure.

### 5. Borrow through the existing VM source cursor

Add an ephemeral-index read-cursor storage kind with a typed `RelationId`.
It has no persistent root, agrees with the relation's field count, and maps
fields to materialized record positions. Keep complete comparison metadata
in the existing relation descriptor, not a second cursor column container.
Declaring only an ephemeral view does not require a persistent database
snapshot; real construction cursors retain their existing snapshot requirement.

The VM relation slot remains the sole owner. A runtime cursor variant holds
only the borrowed relation ID. Derive and validate a one-to-one borrower map
from immutable descriptors; reuse that map at runtime rather than maintaining
inconsistent ownership metadata. Charge derived analysis storage to existing
verification limits. Open the logical writing relation before its view and
close the view before its owner. Both belong to the same verified read
subroutine owner.

Use the canonical source registry, field mapping, rowid-field emission,
decoded-field cache, and virtual `null_row` flag. Publish the ephemeral view
once as the bound source owner. Construction uses an ordinary real-table
cursor and its existing default/REAL-affinity field descriptor, but does not
publish a second binding for the bound source. Keep only the required typed
construction projection.

Before seeking, advancing, clearing, resetting, closing, or moving the
borrowed relation, invalidate the source cursor's encoded and decoded
record caches. Invalidating them after freeing a retained current record is
too late. Generic column and rowid reads use the existing source reader and
cache; no relation-only expression map or duplicate row owner is added.

`NullRowInstruction` clears the underlying lookup position and publishes the
existing virtual NULL row. Generic field reads return NULL on that path
without accessing relation data. Retain ordinary NULL equality rejection,
IS matching, and LEFT publication.

VM error, halt, reset, finalize, and destruction close source views and
construction cursors before relations, then destroy statement storage and
detach the execution context. No owner or borrowed view survives reset.

### 6. Verify lazy materialization explicitly

Add narrowly typed instructions:

- a lookup-readiness check with separate ready and empty targets;
- relation sealing;
- lookup seek with an explicit retained-key register block; and
- lookup prefix-next with that same explicit key block.

The readiness check falls through only for writing construction. A sealed
empty relation skips probe-key evaluation. After construction, sealing
returns control to the readiness check. No generic once instruction, hidden
key cache, runtime compiler, or opaque built flag is introduced.

Open logical handles after existing zero-LIMIT and statement guards. Do not
scan, encode source rows, or create an arena before the first logical entry
of that source. Use existing typed open/field-read/insert/next/close
instructions to construct every row, including NULL keys. Close the real
construction cursor before publishing sealed readiness.

For lookup relations only, verifier flow can contain writing, lookup-ready,
and their explicit union at a loop merge. The readiness check splits that
union into definitely writing and definitely lookup-ready edges. Insertion
still requires writing; seek/next require sealed readiness. Lookup cursor
positioning remains in the canonical cursor proof, not a writable relation
state. Unrelated capability merges remain exact.

Reject direct drain/reset/mutation of a borrowed sealed owner, close with a
live borrower, duplicate borrowers, mismatched descriptors, wrong cursor
kinds, uninitialized keys, and cross-owner access. Ordinary persistent
rewind/seek/reverse instructions do not silently acquire lookup behavior.
Virtual NULL and EOF edges retain the existing exact cursor proof.

### 7. Extend the joined producer without copying its suffix

Each selected source retains an `E`-register equality block in the existing
joined resource layout. Reuse the explicit-destination key emitter,
canonical affinity application, and original conjunct evaluation order.
Construct once before evaluating those keys, then probe on each logical
source entry. Initialize retained registers before any preserving bypass.

Successful candidates enter the existing ON, match, ready-WHERE, child-source,
and consumer schedule. Prefix exhaustion and rejected NULL keys leave the
source unpositioned. LEFT matches are recorded before WHERE filtering and
use the same one-time virtual NULL publication and shared accepted suffix.

Streaming, ordering/top-N, DISTINCT, compounds, aggregates, private scalar
first-row demand, EXISTS, LIMIT/OFFSET, reset/rebind, and multiple automatic
sources keep their existing consumers and query-owned compilation frames.
Normal and early-stop cleanup follows the same source resource ownership.
No new persistent database transition is introduced.

## Verification

Review this design before production changes. Begin from merged prerequisite
`8923df7b02104a35e0cd9fa7b6c67dfb3909d35c` and the green 1,342-test
instrumented fast baseline. Add a focused public 32-probe test requiring
fewer than 100,000 instructions; observe its genuine unchanged-production
failure before implementation. Keep the one-point no-build control and
frozen stored-index controls unchanged. Record work, not a guessed speedup.

Module-owned tests cover:

- actual file build costs, memory-mode ineligibility, the successful one-byte
  memory scan, one-point and non-improving controls, stored/rowid priority,
  complete-key/probe separation, actual-core ownership, and canonical physical
  validation;
- first-equivalent prefixes, duplicate covering rows, NULL/IS, BINARY/NOCASE,
  mixed storage classes, missing/empty/repeated probes, overflow covering
  records, and both backends;
- direct shared-arena construction from the first file-mode insertion, no
  ordered-array construction cost, empty/lazy behavior, memory limits,
  simultaneous consumers, shared-root/cache safety, and retained-view release;
- malformed descriptors and every owner/borrower/lifecycle branch, including
  the narrowly admitted verifier union and unchanged ordinary merge errors;
- generic source fields/rowid, defaults, REAL affinity, borrowed-cache
  invalidation before relation movement, virtual NULL reads, typed failures,
  poisoning, reset recovery, and allocation/OOM boundaries; and
- all admitted join kinds and consumers, multiple automatic sources, duplicate
  and NULL keys, matched-but-WHERE-rejected LEFT rows, no build for zero LIMIT
  or an unentered source, early stop, private queries, parameters/rebind,
  bounded 64-source compilation, and genuine RIGHT/FULL fallback controls.

Reuse frozen fixtures and append pinned native comparisons without changing
any inherited record or oracle provenance. Extend the independent join
model, existing allocation sweeps, deterministic VFS failures, and
grammar-aware SQL fuzz through their canonical helpers. No alternate fixture,
fallback scan, relaxed error, reduced allocation sweep, or enlarged timeout
may substitute for a passing contract.

Run focused tests and the complete fast tier, then required Debug, Release,
ASan/UBSan, genuine GCC Debug/Release, pinned formatting, unfiltered production
clang-tidy, engineering discipline, graph validation, pinned native gates,
sanitized fuzz, and every unchanged frozen performance contract.

Obtain GPT-5.6 Sol design, implementation, and exact-tree closure review.
Commit only reviewed changes and merge normally after all nine exact-head
hosted gates pass. Clean only the confirmed merged branch, then continue the
remaining prerequisite-ready ADR-0053 slices. This slice does not complete
node 43 or authorize nodes 44-48.

### Recorded source-access evidence

The unchanged 32-probe public test now returns the same 32 rows in 37,365 VM
instructions, below the original exclusive bound of 100,000. Its genuine
676,012-instruction failure, one-point control, one-byte memory scan, and
frozen performance contracts are not replaced or relaxed.

The complete expanded fast gate passes 1,370 unit tests, two independent
model tests, and two fuzz-smoke tests. This includes explicit lookup-purpose
consumer assertions, both write-session temporary configurations,
reset/finalize cleanup, two live automatic sources with one shared cache
page, 64-source compilation, and complete optimizer, lowering, and lookup
allocation-failure sweeps. The existing aggregate allocation count stays 46.
The independent join-model suffix preserves all original seeded cases.

The native corpus appends 36 comparisons to the unchanged 476 inherited
records: 506 comparisons and six unsupported boundaries, exactly within the
existing 512-case limit. The canonical pinned-oracle generator and verifier
preserve inherited records, fixture hashes, profile, source provenance, and
all limits. Grammar-aware smoke coverage exercises every admitted join form,
six equality-key shapes, and six existing consumer forms.

The complete instrumentation tier passes 1,414 tests. Clang Debug, Release,
and ASan/UBSan each pass 1,403; genuine GCC 16 Debug and Release each pass
1,401. A GCC range-loop diagnostic in the new lookup fixture is corrected
with a const-reference binding, preserving every input and assertion. The
corrected fixture passes on every configuration, and the complete 1,374-test
fast/model/fuzz-smoke tier remains green.

A fresh grammar-aware ASan/UBSan campaign completes 20,000 inputs with seed
87, maximum length 4,096, and the unchanged ten-second timeout. All 58 frozen
performance contracts and ten compatibility gates pass with timing isolated
from builds and fuzz. Unfiltered production clang-tidy, pinned formatting,
engineering discipline, and dependency-graph validation pass. No workload,
profile, threshold, timeout, compiler policy, or allocation budget is relaxed.

## Rejected alternatives

- Costing every backend as logarithmic hides ordered-array movement.
- Admitting memory construction without capacity evidence can turn a
  successful scan into a deterministic temporary-budget failure.
- Migrating only at sealing still pays the pre-migration quadratic build.
- Freezing file-mode memory records can strand shared retained-byte budget.
- Partial comparison metadata can merge distinct covering rows.
- Clearing source caches after relation movement leaves dangling views.
- A fake catalog index, private source registry, executor, sorter, or hash
  table duplicates existing ownership and semantic mechanisms.
- Adding Bloom filtering or RIGHT/FULL automatic replay here obscures the
  independently measured source-access requirement.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 automatic transient covering indexes.

Contracts: ADR-0018, ADR-0024, ADR-0033, ADR-0035, ADR-0052, ADR-0053,
ADR-0056, ADR-0058, ADR-0067, ADR-0072, ADR-0073, ADR-0075, ADR-0076,
ADR-0077, ADR-0078, ADR-0079, ADR-0080, ADR-0081, ADR-0082, ADR-0083,
ADR-0084, ADR-0085, ADR-0086, ADR-0087, and ADR-0088.
