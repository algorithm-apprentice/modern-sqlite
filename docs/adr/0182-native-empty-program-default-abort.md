# ADR-0182: Native Empty-Program Default ABORT

- Status: Accepted
- Date: 2026-10-30
- Module graph node: `implement-advanced-sql`
- Prerequisites: qualified main `8fbe1f2`, ADR-0119 and ADR-0179
- Related contracts: ADR-0048, ADR-0052, ADR-0106, ADR-0111

## Completed inventory and measured finding

PR #275 is normally squashed as `8fbe1f2`, exact tree `462e7ea`.
Actual-main nine-context CI, fresh whole-production quality and identical-tree
actual manual qualification pass. Delivered references are retired, while
unmerged #269 remains preserved. Node 43 is not complete.

The resumed data-admission inventory identifies a smaller prerequisite.
`BuildMutation` marks proven-empty UPDATE/DELETE and no-op CREATE TABLE/INDEX
as `MutationAtomicity::kTransaction`. Lowering carries that choice into
`ProgramRollbackMode`; the coordinator then discards prior explicit work on
a real pre-execution schema guard error.

This conflates eliminating a write with selecting SQLite's `OE_Rollback`.
An empty SQL body can still fail its schema/format/context guards before
executing any SQL instruction. Absence of statement-journal work does not
authorize transaction-wide rollback for an ordinary ABORT-class failure.

An unchanged-production native MSVC diagnostic reuses the existing owning
lowering fixtures and actual optimizer, lowerer, coordinator and VM. It
establishes durable and prior uncommitted records, admits the generated
statement, and fails the real VM attachment generation guard. All cases
execute zero VM instructions and return the actual schema error:

| Generated shape | Explicit/named cases | Current rollback | Prior work |
|---|---:|---|---|
| Empty UPDATE | 2 | Transaction | Lost |
| Empty DELETE | 2 | Transaction | Lost |
| No-op CREATE TABLE | 2 | Transaction | Lost |
| No-op CREATE INDEX | 2 | Transaction | Lost |
| Ordinary point UPDATE control | 2 | Statement | Retained |

This is an owning producer/consumer guard diagnostic, not a claim that the
current facade's two-admission race was executed or that the synthetic
lowering fixtures are a native file-interoperability oracle.

Ten pinned native public memory-database controls prepare the same five
families before prior explicit writes and a schema change. Their first old
halt PROFILE occurs with zero reprepares and autocommit false. The final
step completes after one reprepare with both exact rows retained; named
scope use, final rollback contents and integrity independently pass.
No native/Modern heap ordinal or timing alignment is asserted.

## Completed native state and owner map

| Native mechanism | Pinned SQLite source | Invariant |
|---|---|---|
| VM starts with `OE_Abort` | `vdbeaux.c:2624-2630` | No-work elimination does not select `OE_Rollback`. |
| Admit actual access before schema guard | `vdbe.c:4245-4350` | A guard can fail even when the SQL body has no mutation. |
| SQL Halt may select a conflict action | `vdbe.c:1385-1386` | A later instruction's error action is not the pre-execution default. |
| Default ABORT closes statement scope | `vdbeaux.c:3455-3459` | Retain prior explicit work, including when no anonymous scope was needed. |
| Missing anonymous statement close is harmless | `vdbeaux.c:3275-3280` | Journal-free ordinary failure is not an implicit full rollback. |
| Resource/I/O/interrupt policy | `vdbeaux.c:3335-3390` | Special failures keep their independent full/statement cleanup rules. |
| Old halt precedes bounded reprepare | `vdbeapi.c:980-1027` | Do not erase old cleanup by compiling first. |

Current canonical Modern owners already express the needed policy.
`MutationAtomicity::kStatement` lowers through the existing program metadata;
the coordinator's ordinary, unpoisoned, journal-free statement rollback ends
the managed epoch/token without discarding the parent. `ProgramWriteEffects`
independently controls the actual anonymous journal. Reuse those contracts,
not another error-action enum, phase classifier, rollback driver or token.

## Decision

Publish statement/default-ABORT atomicity for the current proven-empty data
mutations and no-op schema mutations. Align the UPDATE/DELETE physical payload
defaults with the other current mutation defaults. Keep the existing enum and
explicit low-level transaction-rollback option; do not remove or reinterpret
an explicitly requested transaction rollback.

Keep all unrelated metadata and execution decisions unchanged:

- empty access, predicates, source ownership, rowid collection and SQL body;
- immutable schema requirement, transaction access and snapshot requirement;
- zero multi-write/may-abort effects and false `uses_statement_journal` for
  the admitted empty/no-op programs;
- zero anonymous-savepoint work for these programs;
- change/last-rowid publication, schema refresh flags and successful no-op
  lifecycle;
- resource, I/O, interruption, explicit cancellation, poisoned-state and
  genuine rollback-failure behavior.

This narrowly supersedes ADR-0048's transaction-atomic empty/no-op
classification. It does not introduce conflict-specific ROLLBACK/FAIL/
IGNORE/REPLACE, alter native-compatible storage or add SQL syntax.
No-op DDL's independent lock-access/read-only/reader flags remain a separate
known mechanism finding; do not fix them by flipping write access here.

## Two ordered owning TDD slices

1. Empty data mutations. Add a focused actual lowered-program/VM guard
   regression that first reproduces prior-work loss for empty UPDATE and
   DELETE. Then correct their canonical physical publication/defaults.
   Update direct plan/lowering expectations and the relevant ADR-0048 text.
2. No-op schema mutations. Reuse the same owning prior-work guard helper for
   no-op CREATE TABLE and CREATE INDEX, observe the existing failure first,
   then correct their physical publication. Preserve successful no-op flags,
   schema metadata and journal-free behavior.

Tests retain exact row IDs/payloads, ordinary controls, explicit and named
parents, no active token/pending completion after the guard, direct commit
and retry, and independent reopened durable bytes. Assert unchanged false
journal effects and actual zero anonymous-savepoint work. Reuse the existing
fixture, codec and coordinator owners rather than duplicate their algorithms.
Existing resource/I/O/interruption/full-rollback and explicit low-level
transaction-policy controls remain mandatory negative evidence.

Each source requires genuine focused red/green, complete fast units,
applicable allocation/ownership/malformed/failure coverage, independent
implementation acceptance and append-only ledger evidence before its
successor. Publish a draft after the first accepted normal commit and retain
the ordered two-source review boundary under ADR-0106.

Run the combined actual change-aware closing selection and required hosted
compiler, sanitizer, native, quality and bounded campaign gates against the
final exact head. Preserve original workloads, allocation ceilings,
assertions, deadlines, default stacks and permissions. This changes error
scope, not an ordinary engine speed claim or a new performance archive.

Actual data-statement snapshot admission, native SCHEMA cleanup/retry,
bindings/Busy/generation/error priority, no-op classification and preparation
ownership remain separate subsequent work. Do not promote the preparation
read token or begin a later SQL feature/DAG node as part of this prerequisite.
