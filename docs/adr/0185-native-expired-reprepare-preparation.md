# ADR-0185: Native Expired-Statement Preparation Scope

- Status: Accepted
- Date: 2026-10-31
- Module graph node: `implement-advanced-sql`
- Prerequisites: qualified main `ff720b6`, ADR-0151 and ADR-0184
- Related contracts: ADR-0102, ADR-0106, ADR-0111 and ADR-0173

## Completed inventory and bounded correction

PR #277 closes native owned-binding transfer on actual main `ff720b6`,
tree `9633121`, ledger 136. Both existing recompile consumers use the same
guarded VM operation; caller binding still clones. Node 43 remains active.

Freshly compiled current public controls and matched pinned native controls
retain six resource-scope differences in twenty cases per engine:

- Four unexpired journaled statements lose prior explicit/named work when
  the temporary execution-catalog coordinator Read allocation fails.
- Two READY-expired statements lose that work when the same extra owner
  runs before replacement preparation. Native preparation OOM preserves it
  for both journaled and journalfree programs.

This decision corrects only the second, preparation-before-execution path.
Ordinary unexpired admission, old guard/halt cleanup and runtime retry remain
a separate coherent mechanism; do not disguise them by changing a temporary
Read token's error classification.

### Native expiration survives connection schema reload

| Responsibility | Pinned SQLite source |
|---|---|
| Schema-sensitive full rollback expires statements and resets schema | `main.c:1516-1548` |
| Schema-sensitive named rollback does the same | `vdbe.c:4097-4102` |
| Expiration is retained by each prepared program | `vdbeaux.c:5268-5290` |
| READY-expired check returns SCHEMA before old VM startup | `vdbeapi.c:835-858` |
| Outer runtime wrapper prepares a replacement | `vdbeapi.c:980-1027` |
| Replacement uses the ordinary bounded preparation orchestration | `prepare.c:862-934` |
| Schema initialization borrows a transaction or owns only its preparation read | `prepare.c:213-445` |
| Actual VM admission/guard and error rollback are distinct | `vdbe.c:4245-4350`; `vdbeaux.c:3335-3488` |

Four additional native controls first reload the connection schema through
another statement's successful prepare. The old target still reprepares.
Its first replacement-allocation OOM has no old PROFILE, retains both prior
rows and the named scope, and does not become an execution rollback merely
because the connection catalog is initialized again.

The current Modern owner inventory is:

- The coordinator already owns one allocation-free sticky schema-reset
  request, produced only by schema-sensitive full/named rollback.
- `WriteSession::State::CleanupPreparation` consumes it and clears
  `catalog_initialized`; later reload publishes the canonical increasing
  `catalog_generation`.
- Old immutable programs retain their original generation, but the session
  currently forgets which generations were explicitly expired by rollback.
  Checking only `!catalog_initialized` would fail after another prepare
  reloads the catalog.
- `Recompile` reparses retained SQL and invokes the ordinary compiler directly.
  The existing `CompilePreparedStatement` already owns preparation admission,
  one preparation-specific schema retry, expected cleanup and provenance.
- `PrepareReadScope` borrows an existing read/write transaction, closes only
  its own read and retains failed cleanup. It publishes no coordinator
  statement, savepoint, execution token or logical transaction.
- ADR-0184 already provides binding retention and atomic ownership publication.

## Decision

### 1. Reuse the canonical preparation orchestration

Route writable `Recompile` through existing `CompilePreparedStatement` after
the retained SQL parses exactly. Keep all existing parameter names,
execution-command and transaction-presence checks, then use the same
`TransferBindingsFrom` publication boundary. Do not copy a retry loop, loader,
catalog container or binding algorithm.

The preparation bound remains one; it is not the native runtime wrapper's
separate bound of fifty. An expected load/compile/metadata failure retains
the old program and bindings. Cleanup failure remains explicit and owned.

### 2. Retain one expiration boundary in the existing generation domain

When consuming the canonical schema-reset request, copy the current
`catalog_generation` into one private expiration boundary in the existing
session State, then clear initialized validity as before. Initialize the
boundary to zero; successfully compiled catalogs already start at generation
one. A program whose generation is at or below that boundary is expired.

This is a retained boundary value, not another incrementing generation,
statement registry, per-VM flag, callback or catalog. Only the canonical
generation producer advances identity; only consumption of the existing
schema-reset request updates the boundary. Ordinary DDL publication without
that reset does not acquire preparation-before-execution policy.

The boundary must survive catalog reload, successful preparation of a
different statement, failed replacement preparation and statement Reset.
Newly compiled programs use the newer canonical generation and do not remain
expired. Repeated invalidation is idempotent until successful reload.
Generation exhaustion keeps its existing explicit error; never wrap or
invent another counter.

### 3. Prepare expired READY data before execution admission

After the existing readiness/preparation cleanup barrier, recognize expired
READY data programs using that boundary and invoke the same `Recompile`
before the temporary execution preflight or actual coordinator admission.
On failure, publish the preparation error without statement/transaction
rollback; no execution capability has been acquired by this phase.

Use the existing allocation-failure preparation cleanup helper at the owning
Step OOM boundary so an owned temporary preparation read is ended or remains
explicitly retryable. Never end a borrowed explicit transaction.

Keep pending completion and active-statement barriers authoritative:
preparation cannot finish another statement's commit/rollback or bypass its
Busy/misuse result. Transaction commands and row-suspended executions do not
enter the new READY data path. Successful replacement then rejoins the
unchanged execution path.

This does not move unexpired data admission, weaken schema validation,
reinterpret resumed SCHEMA errors, change primary/cleanup precedence, or
alter compiler journal/rollback metadata, change counters or last-rowid.
It narrowly completes ADR-0151's separation of preparation policy from VM
resource-error policy for an already-known expired statement.

## Two ordered sources and verification

1. Green-baseline orchestration reuse: route writable replacement compilation
   through the existing canonical preparation helper. Preserve the current
   ordinary reprepare, byte ownership, metadata and failure contracts.
   Record the green baseline and run the complete fast tier before review
   and a normal accepted source commit.
2. Focused runtime red before expiration routing: use the existing session
   OOM/VFS owner to specify retention of prior explicit/named work when
   replacement preparation fails, both before and after another prepare
   reloads the connection catalog. Implement only the boundary and caller
   sequencing above. Cover both journal classes, bindings, schema-sensitive
   rollback, repeated invalidation, retry/Reset/rebind, full rollback cleanup,
   pending completion and ordinary unexpired negative controls.

Retain the twenty-case current/native diagnostic matrix and the independent
native reload controls. After qualification, the two original expired cases
must match without relabeling the four unexpired differences as repaired.
The new reload cases must also match; an initialized-catalog shortcut is
insufficient. These are semantic state/phase controls, not matched heap
ordinals, timings or database-file interoperability claims.

Each source needs accepted design, focused green or genuine red/green,
complete fast units, applicable ownership/OOM/lifecycle controls, strict
quality, independent SourceAccepted, normal commit and append-only ledger
before its successor. Use one ADR-0106 draft after the first accepted prefix.
Close the actual final change-aware compiler/sanitizer/native/model/quality
and bounded campaign union, independent integrated review, normal squash,
fresh actual-main checks and delivered-reference retirement.

No deadline, workload, assertion, allocation ceiling, default stack or
diagnostic condition is reduced. Do not start SQL expansion or a later DAG
node. The larger unexpired admission/old-halt/retry repair stays explicit.
