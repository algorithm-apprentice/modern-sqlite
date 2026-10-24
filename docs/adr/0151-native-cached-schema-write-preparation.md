# ADR-0151: Native Cached-Schema Writable Preparation

- Status: Accepted
- Date: 2026-10-25
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 15

## Completed mechanism inventory

ADR-0150/PR #256 closes repeated read-facade schema admission, not entry 15.
Its short/wide/deep ordinary profiles identify remaining syntax, allocation,
program analysis and binding work. The source trace establishes:

- `SyntaxTree` is move-only; parser publication, session compilation and
  `StatementBinder` transfer that owner rather than cloning a complete AST.
- ADR-0084's inline root and flat statement-global query/core/expression
  arenas remain canonical. ADR-0092 instantiates query-containing result
  references for native lexical/cache identity, not as an accidental IR copy.
- The bound source copy is ADR-0033's explicit ownership boundary; logical
  and physical stages retain the same moved bound owner.
- ADR-0031 deliberately isolates immutable bytecode from public input and
  retained aliases through a canonical deep copy with retained-capacity
  accounting. `ProgramBuilder::Finish` uses that factory. Structural checks
  and one definite-state analysis run at publication; VM initialization uses
  retained verification metrics instead of repeating that analysis.
- Native `vdbeaux.c:25-45,164-211,2640-2760,3745-3804` grows and packages one
  mutable VDBE, transfers its variable list, reuses opcode-tail space and frees
  those owners on deletion. Modern's immutable stage boundary and verifier
  are accepted architectural costs, not another executor or runtime replay.

Do not add a trusted-input bypass, second compiler, arena pool, unchecked
move factory or weaker verifier to erase those documented costs. This slice
changes the confirmed remaining writable preparation state mismatch.

Current `WriteSession::Prepare` always runs the coordinator's read-statement
path. Eight warmed, unstepped prepare/finalize operations of the existing
short parameterized SELECT make 56 VFS calls and zero page reads/writes.
The unchanged pinned native read-write connection makes zero VFS calls for
the corresponding warm prepare.

### Native source, owners and cleanup

| Native transition | Pinned source |
|---|---|
| Initialized connection schema skips loading | `prepare.c:453-503` |
| Resolver provenance conditionally requests a cookie probe | ADR-0150's `build.c`/`resolve.c` map |
| Missing explicit INSERT/UPDATE target columns request validation | `insert.c:1091-1098`; `update.c:505-513` |
| Early CREATE TABLE name/reserved/collision errors request validation | `build.c:1294-1358,1450-1453` |
| Missing ANALYZE table/index target uses the canonical table lookup | `analyze.c:1491-1494`; `build.c:409-454` |
| Probe borrows an existing read/write transaction or owns and closes a temporary read | `prepare.c:512-553` |
| Probe ordinary I/O preserves semantic primary; OOM dominates without executing a VM rollback | `prepare.c:529-535,818-828`; `vdbeaux.c:3745-3804` |
| Schema parsing publishes connection schema before terminal VM completion; failure resets it | `vdbe.c:7280-7365` |
| Schema-sensitive full rollback expires programs and resets loaded schema | `main.c:1516-1548` |
| Named savepoint rollback resets schema when schema-change state is set, then retains that change flag | `vdbe.c:4074-4103` |
| Ordinary anonymous statement rollback does not reset a prior valid schema | `vdbeaux.c:3230-3280` |
| Reset clears loaded/known state and the change flag; successful commit clears only change state | `build.c:651-676` |
| VM resource-error rollback policy is execution policy, not preparation-probe policy | `vdbeaux.c:3335-3508` |

Twenty-five deterministic controls against the unchanged pinned public C API/VFS
establish zero warm read-write prepare I/O; unchanged schema after DML full
and named rollback; forced reload after DDL full rollback; disappearance of
a rolled-back DDL name while borrowing a savepoint snapshot; loaded schema
after successful commit; retention of a prior schema after ordinary statement
ABORT; and preservation of an explicit transaction after preparation-probe
ordinary I/O and `SQLITE_IOERR_NOMEM` (reported as `SQLITE_NOMEM`).
The additional admitted-statement controls distinguish missing DML targets/
explicit target columns, CREATE INDEX target table/column lookup, ANALYZE
target lookup and early CREATE TABLE errors from unmarked duplicate-column
and CREATE INDEX namespace-collision errors.

Modern already has the necessary owners:

- `CoordinatorState::schema_changed_`, set by `IncrementSchemaCookie`, is the
  canonical schema-sensitive rollback/cursor policy fact.
- `PerformFullRollback` covers explicit, implicit and setup/completion
  failures; `RollbackTo` owns named rollback. Anonymous statement rollback
  has its distinct existing path.
- `WriteStatement::Impl` owns an unfinished statement, completion retry,
  candidate schema, change count and compiled execution.
- `WriteSession::State` owns one catalog and generation; first step refreshes
  and automatically recompiles retained SQL through the existing compiler.

The coordinator's `BeginStatement` must not be reused as a preparation probe:
its read OOM/I/O setup policy can perform an automatic full rollback. Native
preparation does not execute that VM failure policy.

## Decision

### 1. Share one narrow preparation read owner

Extract ADR-0150's read admission, cookie probe, catalog refresh and expected
allocation-failure cleanup into the existing private `session_internal.hpp`.
Both facades use that canonical implementation. Keep coordinator-owned
execution admission separate. Share the one-retry preparation orchestration
as well: its only facade-specific work is invoking the two existing typed
compilation results and readiness policy. Do not copy a second retry loop or
introduce another compiled representation.

A preparation read scope borrows the Pager's existing read/write snapshot.
Otherwise it records responsibility for temporary read cleanup before
attempting `BeginRead`, and owns that read until successful explicit finish
or cleanup. It does not create a coordinator statement/writer, change logical
autocommit, create/release a savepoint, or run automatic full rollback.
Its ownership state records only outstanding preparation cleanup, not another
catalog, snapshot generation or transaction policy.

Failed begin/end cleanup remains owned and retryable. Every substantive
prepare and execution catalog refresh reconciles a retained preparation read
before using a catalog or borrowing another snapshot. An OOM cleanup uses the
current best-effort allocation-failure pattern while retaining cleanup
responsibility for the next barrier. Never close a borrowed active read/write
transaction. Empty and malformed SQL retain parsing-before-storage behavior.

The read-facade extraction is a green-baseline refactor, preserving its
existing first-step admission, schema retry, error and cleanup contracts.

### 2. Propagate canonical rollback invalidation, not another generation

Add one allocation-free sticky schema-reset request to the existing
coordinator state. Produce it from schema-sensitive full rollback and named
savepoint rollback, using the existing `schema_changed_` fact. Keep it across
terminal cleanup and logical-state reset until the owning session consumes
it. Do not produce it for ordinary anonymous statement rollback, successful
commit/release, or DML-only rollback.

Expose only a narrow consume operation to the owning session. It conveys no
catalog/AST/session dependency into the transaction layer. No callback,
observer registry, second schema-change flag or generation counter is added.
The existing cursor-fence policy and ordering remain unchanged.

The session consumes the request before catalog use and clears initialized
validity while retaining the old immutable snapshot for existing borrows.
Reload publishes a new generation even if the cookie returned to an old
value. A failed known replacement remains uninitialized.

### 3. Compile warm writable statements against initialized schema

Add private initialized validity distinct from the retained catalog pointer,
as in ADR-0150. Normal successful warm preparation performs no temporary
storage admission. Cold preparation retains initial catalog validation,
through the shared preparation read owner rather than a VM statement.

Extend the existing canonical sticky marking helper at writable
native-equivalent origins; forwarding only the existing SELECT producers is
insufficient. Mark missing mutation targets, explicit INSERT/UPDATE target
columns, CREATE INDEX target table/columns, missing ANALYZE targets, and the
mapped early CREATE TABLE reserved/collision failures. CREATE INDEX namespace
collisions and ordinary duplicate CREATE TABLE columns remain unmarked,
despite overlapping final error codes.

Publish the existing `BindError.requires_schema_check` fact only on ultimate
failure, including sticky suppressed compound candidates. Unmarked function,
wildcard, ordinal and syntax errors do not probe. Probe, close, invalidate, retained-SQL
reparse and cold reload/recompile follow the same one-retry implementation and
primary-error/OOM rules as the read facade. A probe never ends or rolls back
the caller's explicit transaction.

An active statement's valid snapshot is borrowed. Do not replace a known
invalid catalog underneath a suspended statement. Pending coordinator
completion retains its current explicit retry/cleanup barriers; preparation
must not finish, commit or roll back that completion on another statement's
behalf.

### 4. Publish schema at its native execution boundary

After successful VM completion and schema loading, publish the candidate
catalog before attempting terminal statement success, matching native
`OP_ParseSchema` before `sqlite3VdbeHalt`. Use the single current catalog
owner; remove deferred candidate ownership rather than retaining parallel
published and pending schema containers.

Changes remain pending until successful completion. Last-insert-rowid keeps
its existing VM publication point. If schema loading fails, invalidate
initialized state before the existing statement rollback path, as native
`OP_ParseSchema` does. A later full/named rollback consumes the coordinator's
reset request before any warm compilation.

Keep commit/rollback retries, retired statement behavior, automatic reprepare,
parameter bindings, result views, snapshot pins and zombie ownership intact.
This narrowly supersedes ADR-0048's deferred catalog publication, not its
change-counter, transaction or destructor contracts.

## Test-first delivery and bounded scope

First add a public writable-session regression: warm prepare succeeds without
consuming an armed database read failure, and first step encounters that
failure. Record its failure on unchanged production.

Owning tests cover DML versus DDL full/named rollback, same-cookie generation,
ordinary statement ABORT after prior DDL, schema publication across failed
terminal cleanup and rollback, retained cleanup, active snapshots, sticky
resolver retry/negative controls, known-replacement failure, and probe OOM
preserving explicit transaction state. Coordinator tests own reset-request
delivery and sticky cleanup behavior. Reuse current VFS, allocation, cursor,
rollback and session model helpers.
Binder and writable-session tests specifically cover warm missing target and
explicit target-column retry/OOM, early CREATE TABLE error probing, and the
same-code CREATE INDEX namespace-collision negative controls.

Keep complete fast units, original model deadlines, selected compiler/
sanitizer/native/fault/fuzz/quality gates and exact-head review/delivery.
Record the writable VFS-work baseline before implementation; retain existing
matched workloads and ordinary/diagnostic separation for qualification.
No parser grammar, public session API, C API, SQL feature, write-step admission
algorithm or later DAG node is added. Entry 15 closes only after the completed
inventory, focused repair and supported disposition have final evidence and
review.
