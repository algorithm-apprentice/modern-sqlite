# ADR-0179: Native Failure-Atomic Savepoint Admission

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Prerequisites: ADR-0119, ADR-0173, ADR-0174 and qualified main `26a0002`
- Related contracts: ADR-0040, ADR-0041, ADR-0106, ADR-0111

## Completed inventory and bounded finding

PR #274 is normally squashed as `26a0002`, exact tree
`308b7f3e95cb46b38d8403b5eee81d2e5f729dbe`. Actual-main nine-context CI,
fresh production quality and identical-tree actual manual qualification
pass. The physical-extent prerequisite is closed; node 43 is not complete.

The subsequent statement data-admission inventory identifies a narrower
prerequisite before changing the writable facade's two admission epochs.
`JournalTransaction::CreateSavepoint` enters Failed on an unpublished
PageBitvec allocation, savepoint-vector allocation or backend metadata
allocation failure. `CoordinatorState::BeginStatement` then unconditionally
fully rolls back a failed anonymous savepoint. This loses earlier user work
even when the compiler requires a statement journal.

An unmodified-production native MSVC diagnostic includes the existing owning
coordinator OOM helpers, links the ordinary library and establishes prior
explicit-transaction work before arming one real allocation failure. Warm
journaled admission performs three allocations. Failure at the first two
retains the transaction and prior row; failure at the third reports NOMEM,
sets autocommit and loses the prior row. Journal-free admission has two
allocations; each failure correctly performs full rollback.

These are measured Modern ordinals, not an alignment with native heap
ordinals. Existing owning OOM checks accept either autocommit or a retained
transaction for journaled setup, so their broad invariant misses this loss.

The pinned native public control fails statement-savepoint bitvector
allocation before its later schema guard. It reports `SQLITE_NOMEM`,
autocommit false, both prior live rows and no automatic reprepare. Its
later schema mismatch is not reached at this failing stage. The separately
observed schema-cleanup allocation failure remains a subsequent finding.

## Completed native owner and cleanup map

| Mechanism | Pinned SQLite 3.54.0 source | Required invariant |
|---|---|---|
| Actual write admission and statement identity | `vdbe.c:4245-4318` | Compiler `usesStmtJournal` controls the anonymous scope before the schema guard. |
| B-tree statement entry | `btree.c:4617-4635` | Reuse the current write transaction and delegate savepoint metadata admission. |
| Savepoint array and membership allocation | `pager.c:6995-7048` | Retain capacity if necessary, but publish each new savepoint only after its membership exists. NOMEM does not poison prior state. |
| Beyond-current rollback/release | `pager.c:7083-7128` | An unpublished anonymous index does not create a new playback obligation. Existing savepoints remain valid. |
| Resource error policy | `vdbeaux.c:3335-3390` | NOMEM/FULL with compiler `usesStmtJournal` selects statement cleanup, not automatic loss of the outer transaction merely because setup failed. |
| Statement-close owner | `vdbeaux.c:3230-3280,3465-3488` | A real cleanup failure retains its existing full-rollback/error-precedence policy; do not treat it as harmless admission failure. |

This correction does not change native allocation shape, persistent bytes,
journal publication, playback or cursor algorithms.

## Existing canonical Modern owners

`JournalTransaction` owns the PageBitvec membership and semantic savepoint
vector. Its local optional membership and existing provisional push/pop
already provide the necessary unpublished-resource lifetime. The rollback
backend separately owns offsets in one existing vector; its
`CreateSavepoint` performs no filesystem operation and vector-allocation
failure publishes no boundary.

`Pager::CreateSavepoint` delegates to this journal owner after the existing
writer/pin/readiness checks. `EnsureJournalTransaction` publishes its owner
only after Begin succeeds. `write_failure_code` observes the journal's
Failed/Error state; no extra Pager error latch is needed.

The coordinator preallocates its token/capability before lower admission.
Its existing `FailStatementSetup` already selects resource rollback from
the compiler's statement-journal requirement. The anonymous savepoint is
created before managed writer-epoch/active-token publication. Reuse these
owners and policy rather than add another pending token, metadata vector,
rollback driver, allocator hook in production or error classifier.

## Decision

### 1. Keep unpublished savepoint allocation failure atomic

For CreateSavepoint only, an allocation failure before successful metadata
publication returns the original typed OOM and leaves the transaction
Active. Preserve every prior savepoint, page-membership set, next identifier
and dirty/synchronized/database-modified fact. Retained vector capacity is
allowed; consuming an identifier or clearing an earlier failure is not.

The backend's CreateSavepoint hook has the same strong OOM postcondition:
no new semantic boundary is published on either a returned OOM or a
pre-publication `std::bad_alloc`. Catch that allocation exception at this
hook boundary, retire the canonical provisional entry and return typed OOM.
Keep the existing rollback backend and provisional JournalTransaction
push/pop, destroying the unadmitted PageBitvec through its canonical owner.

This narrowly amends ADR-0040's conservative failure treatment of
unpublished savepoint metadata allocation. It does not relax capture,
membership growth after append, playback, database mutation, I/O, corruption
or already-failed transaction behavior. Those failures keep the existing
Failed/Error state and cleanup requirements. Non-OOM backend failures
retain their current handling.

### 2. Preserve compiler-selected setup rollback scope

Once lower admission is reviewed and qualified, do not unconditionally
fully roll back the anonymous savepoint's atomic OOM failure. Return it to
the existing setup-error policy with the unchanged compiler
`uses_statement_journal` fact. A journaled explicit statement leaves no
active token or pending completion, keeps prior work and remains usable
for direct commit, explicit rollback or a successful retry.

Journal-free/resource, I/O and other already admitted failure policies stay
unchanged. Named SQL SAVEPOINT command resource handling retains
ADR-0172's reader-classified full rollback. Real statement rollback failure
is not an unpublished metadata allocation failure.

This clarifies ADR-0119's admission boundary: lack of a successfully
published anonymous savepoint does not erase the compiler's journal
requirement. Do not copy the journal-free full-rollback rule into this
pre-execution failure.

## Two ordered test-first slices

1. Journal metadata atomicity. Add a focused owning allocation-failure
   expectation that fails on the current Failed state, then preserve Active
   state without changing the existing containers. Sweep every actual
   allocation prefix for empty/nonempty membership and nested savepoints;
   include typed backend OOM, unchanged next IDs, all ordering flags,
   membership, retry, direct commit/rollback and real backend metadata.
   Keep append/membership-growth/playback/I/O failure negative controls
   unchanged. Update directly related contract text.
2. Coordinator caller. Add a focused real OOM regression with prior
   explicit writes and a journaled statement. It must first reproduce
   autocommit/prior-row loss, then verify every actual setup allocation
   prefix preserves exact prior rows, transaction usability, no active
   token and no pending cleanup. Cover direct commit and retry/new writes,
   existing named scopes and journal-free full-rollback negative controls.
   Reuse the existing allocator/fixture harness and measure ordinals
   locally; do not require one standard-library allocation count.

Each slice requires focused red/green, complete fast units, owning
failure/malformed/lifetime coverage, independent implementation acceptance
and append-only ledger evidence before its successor. Preserve original
allocation ceilings, workloads, deadlines, default stacks and all compiler
or sanitizer checks. Run affected Native, sanitizer and interoperability
qualification; final exact-head hosted/integrated gates and actual-main
proof remain required before normal squash delivery.

This is one bounded node-43 prerequisite, not a feature or a whole
transaction audit. Actual data-statement snapshot admission, pre-execution
schema cleanup/retry, independent reader/read-only/no-op-DDL flags and
preparation allocation remain separate. Do not promote a preparation token,
flip no-op DDL to ordinary read-only policy or begin the later SQL feature
slice as part of this correction.
