# ADR-0172: Native Non-Reader Execution Admission

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Prerequisites: ADR-0151 and qualified PR #270

## Completed inventory and bounded finding

PR #270 delivers the reviewed live full-rollback corrections as `7e11e2f`,
tree `c9c76e646ef9f4e691a96ed6ac2c984b943d0d1d`. Its exact-tree closing
gates and actual-main CI/production quality pass. This does not close the
broader session, savepoint or resize findings.

The current writable facade always refreshes its catalog through a temporary
coordinator read statement, then admits another statement for VM execution.
This includes an initialized, source-free `SELECT 7`. It also performs that
catalog preflight before pure transaction commands. These reads are not
required by their native execution mechanisms.

Sixteen matched public controls use actual native MSVC Release engines,
4096-byte pages, a 512-page cache, DELETE/FULL, memory temporary storage, no
mmap and normal locking. The pinned oracle independently verifies all 32
final databases, exact IDs/payloads, integrity and absent sidecars.

| Warm first step | Native main-file reads | Modern main-file reads |
|---|---:|---:|
| `SELECT 7` | 0 | 2 |
| `BEGIN` | 0 | 1 |
| `SAVEPOINT s` | 0 | 1 |
| `BEGIN IMMEDIATE` | 1 | 2 |
| Existing table read | 1 | 2 |

This is fixed-work evidence, not a latency or allocation-parity claim.
The table-read extra epoch is a separate subsequent correction. This ADR
first removes only source-free VM admission and transaction-command catalog
preflight, without redesigning data-statement schema retry.

### Native state, owners and cleanup

| Native mechanism | Pinned source |
|---|---|
| Bytecode determines `bIsReader`; ordinary scalar programs leave it false | `vdbeaux.c:878-915` |
| `OP_Transaction` begins actual database access and checks schema | `vdbe.c:4245-4350` |
| `OP_AutoCommit` and `OP_Savepoint` are reader-classified commands, not scalar non-readers | `vdbeaux.c:900-906`; `vdbe.c:3959-4215` |
| First step owns active-VM accounting and automatic reset | `vdbeapi.c:838-973` |
| Halt runs database commit/rollback policy only for `bIsReader` | `vdbeaux.c:3326-3508` |
| Reset/finalize close VM resources without inventing another transaction | `vdbeaux.c:3597-3699` |

A non-reader VM still owns registers, bindings and temporary results, but no
main-database transaction. In contrast, transaction commands execute their
existing explicit commit/rollback/savepoint operations. Zero preflight I/O
does not mean those commands have `bIsReader == 0`, or that their terminal
error policy may be skipped.

Two additional pinned-native controls start the same explicit transaction
with a prior inserted row. A deterministic scalar callback returns
`SQLITE_NOMEM`: source-free invocation retains autocommit false and both
rows, whereas invocation from a table-reading query automatically rolls back
the transaction and retains only the original row. Both terminal files pass
independent exact-content and integrity validation. This is native error-state
evidence; the Modern owning test must inject a real allocation failure.

Three additional native first-allocation controls cover a long-name
SAVEPOINT, duplicate BEGIN and missing RELEASE after prior explicit writes.
All return `SQLITE_NOMEM`, restore autocommit and remove only the uncommitted
row; independent exact-content/integrity validation passes. `OP_Savepoint`
allocates its name/structure at `vdbe.c:4009-4018`; early name allocation is
not exempt from the reader-classified halt policy. The writable facade must
not rely on its removed catalog token to provide that rollback accidentally.

### Existing canonical Modern mechanisms

- Immutable program publication derives `requires_database_snapshot` from
  the declared requirement, write access and persistent cursor descriptors.
  Public input cannot hide those resources by clearing the flag
  (`program.cpp:536-553`).
- `ReadStatement::Step` already acquires a read reference only when this flag
  requires it. Writable preparation creates the same published read program.
- `VmExecutionContext` carries the existing database/generation and temporary
  storage factory. A source-free VM still needs this context; dropping it
  would break configured temporary storage and existing VM validation.
- `WriteStatement` already owns an optional `TransactionStatement`.
  Error/reset/finalize paths can release a VM without rolling back a missing
  token. `FailForOutOfMemory` affects only its own optional active token.
- `CleanupPreparation`, `RefreshCatalog`, `EnsureCurrentProgram` and
  coordinator admission already own retained cleanup, invalidation, automatic
  recompile and pending-completion readiness. Reuse them, not a second
  catalog, snapshot owner, readiness algorithm or SQL-text classifier.

## Decision

### 1. Select catalog preflight by its existing requirement

At fresh first step, reconcile retained preparation cleanup and sticky schema
reset before consulting initialized state. Preserve the existing catalog
refresh path for a database-snapshot program, a known-invalid catalog used by
a VM, or any pending coordinator completion. Preserve its typed errors and
cleanup ownership.

An initialized source-free read program and an ordinary transaction command
do not open an extra catalog read statement. Transaction commands use their
existing coordinator dispatch, including the actual read/write admission
inside `BEGIN IMMEDIATE`. Retried terminal cleanup keeps its current bypass
and original error-reporting rules.

Retain `EnsureCurrentProgram`, schema cookie/generation checks and immutable
recompilation. This slice does not remove existing conservative generation
work or turn a known invalid catalog into a valid one.

Transaction-command NOMEM/FULL/I/O errors, including exceptions before a
coordinator operation has allocated its name or savepoint, retain the native
reader error policy. Route them through the existing canonical full rollback,
not scalar non-reader isolation. Reuse the session's rollback/error-combining
path for both typed command errors and the executing-command allocation
boundary; reset is not a new command execution.

Use the coordinator's existing pending-completion and Pager-state facts.
An already pending rollback keeps its original continuation. A pending
commit that has reached WriterFinished, Reader or Open keeps its durable
cleanup owner and must not be rolled back or replayed. A resource failure in
a not-yet-durable commit attempt instead takes canonical rollback. Do not
repeat rollback after the coordinator is already clean autocommit/Open.
Busy and ordinary SQL-command errors preserve their existing transaction
state. Keep the primary error and surface any rollback failure explicitly;
unfinished cleanup remains retryable through the original owner.
The executing allocation boundary returns the same combined error retained
for continuation/reset, rather than replacing it with unconditional OOM.
If allocating diagnostic context itself fails, use the existing allocation-free
OOM error for both outcomes without changing pending cleanup.

### 2. Do not admit a source-free VM as a database statement

Use the published snapshot requirement and existing write-access fact.
A database reader/writer retains the unchanged coordinator token, rollback
mode and `uses_statement_journal` metadata. A source-free read VM has no such
token and executes with the same borrowed read execution context, including
the configured temporary storage factory.

Pending completion must still encounter the existing coordinator readiness
error; a non-reader must not finish or retire another statement's cleanup.
A source-free VM may execute beside a suspended data reader without ending
or replacing its snapshot. The single active coordinator statement invariant
and Busy outcome for another database statement remain intact. This is not
multi-threaded access, another snapshot, or node-44 admission.

Detach/reset/finalize the same VM resources on every outcome. A successful
non-reader retires its session-local completion bookkeeping without calling
coordinator success or publishing a DML change count. Missing-token write
completion remains an explicit internal error, not success.

An allocation or function error from a non-reader does not run database
rollback policy. A database-reading negative control retains its existing
resource-error rollback. Never erase compiler `uses_statement_journal`,
infer it from savepoint existence, or substitute preparation-probe policy
for an admitted reader/writer.

### 3. Preserve the remaining boundaries

No VM, verifier, bytecode, parser, planner, public API, persistent transition,
coordinator state or container is added or weakened. The read facade and
its reference counting remain unchanged.

The ordinary data statement's two admission epochs, no-op-DDL access
classification, savepoint publication/resize and Python-only tool timeout
remain separate findings. In particular, broader native schema-reprepare
allocation cleanup is not assumed to preserve prior transaction work at
every fault point; its completed source/failure map is required before that
later correction. This slice makes no whole-module or node-completion claim.

## Test-first qualification

First add a warm source-free writable-session test with an armed database
read failure. It must return its bound row and DONE without consuming that
failure; a subsequent database reader must encounter it. Record the genuine
failure against unchanged production before implementation.

Owning coverage also includes deferred/named and immediate transaction
commands, reset/reexecution/finalize and counters, a suspended data reader,
pending cleanup, configured temporary storage, and real non-reader OOM while
prior explicit writes remain intact. The equivalent table-reader OOM remains
a full-rollback negative control. Long-name SAVEPOINT, duplicate BEGIN and
missing RELEASE first-allocation failures must follow native full rollback,
while a resource error after durable COMMIT must retain cleanup-only retry
without changing database writes or journal deletion. Sweep every allocation
prefix of the command baseline, including error conversion, and retain the
owning rollback-cleanup continuation. New allocation-hook diagnostics follow
ADR-0171's narrow source-local `modernize-use-std-print` exception: retain
allocator-independent C stdio, without suppressing analyzer or correctness
checks or changing global policy. Keep the existing lifecycle/Busy,
schema/reprepare, resource and terminal-cleanup tests unchanged.

Run focused green, the complete fast unit tier, selected isolated/native and
sanitizer/model/interoperability gates, changed-source analysis, graph and
engineering checks, independent implementation review and exact-head closing
gates. Re-run the matched controls with their original inputs and independently
verify exact rows/integrity. Retain all original workload, allocation ceilings,
assertions, deadlines, default native stack and compiler/sanitizer policy.
Record each accepted slice and its evidence under ADR-0106 before continuation.
