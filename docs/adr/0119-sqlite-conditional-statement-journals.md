# ADR-0119: SQLite Conditional Statement Journals

- Status: Accepted
- Date: 2026-10-20
- Module graph node: `implement-advanced-sql`
- Audit entry: `storage-transaction`

## Context and source mapping

Modern currently opens an anonymous savepoint for every nonempty write inside
an explicit transaction. SQLite opens one only when compiler effects require
it. Replacing statement rollback policy with transaction rollback policy is
not equivalent: an ordinary single-row constraint failure must preserve prior
explicit-transaction work even without a statement journal.

| SQLite 3.54.0 mechanism | Local pinned source |
|---|---|
| Mark `Parse.isMultiWrite` | `build.c:5470-5500`; `insert.c:1012`; `update.c:609,715-769`; `delete.c:422,532` |
| Mark `Parse.mayAbort` | `build.c:5502-5543`; `vdbeaux.c:475-479,568-572`; constraint emission in `insert.c` |
| Freeze `usesStmtJournal = isMultiWrite && mayAbort` | `vdbeaux.c:2704-2705` |
| Admit an anonymous savepoint | `vdbe.c:4210-4310`; `btree.c:4599-4655` |
| Select statement or full rollback and close the savepoint | `vdbeaux.c:3220-3280,3320-3510` |
| ANALYZE cleanup and index-accumulator effects | `analyze.c:195-245,941-945,1153-1157,1246-1250,1390-1445` |

Native EXPLAIN controls confirm journal-free single-row INSERT, point
UPDATE/DELETE, and non-abortable scans. Abortable scans and schema creation
require journals. ANALYZE requires separate effects: index accumulation marks
ABORT, while statistics-table creation or scoped nested DELETE marks
multi-write. Database-wide ANALYZE with an existing statistics table remains
journal-free, even when it accumulates indexes.

The exact pre-repair write baseline at `8b772e0` is retained as
`conditional-journal-write-before-v2`. No SQL feature is added. Persistent
Pager/journal algorithms, named savepoints, durable commit cleanup, and the
current single-active-statement boundary remain unchanged.

## Decision

### Compiler ownership

Add immutable `ProgramWriteEffects` with `multi_write` and `may_abort`, both
defaulting false. Their conjunction determines `uses_statement_journal` for a
write program. Carry the effects through the existing program builder,
canonical clone, and immutable program. Keep `ProgramRollbackMode` separate.

Lowering owns the effects; the session consumes the resulting boolean without
SQL inspection or a second bytecode classifier.

| Current shape | `multi_write` |
|---|---|
| Supported single-row INSERT/default values | false |
| Empty or rowid-point UPDATE/DELETE | false |
| Scanned UPDATE/DELETE | true |
| Non-no-op CREATE TABLE/CREATE INDEX | true |
| ANALYZE creating `sqlite_stat1` or performing scoped cleanup | true |
| Database-wide ANALYZE with existing `sqlite_stat1` | false |
| SELECT and no-op DDL | false |

Use monotonic ABORT marking at existing emission decisions:

- emitted scalar calls, rowid/UNIQUE checks, and enforced NOT NULL checks;
- UPDATE rowid changes, including the check inside table-update execution;
- non-no-op schema creation and catalog reparse; and
- ANALYZE statistics-table creation or index accumulation.

Conditional built-ins that emit branches rather than a scalar call retain
that distinction. Do not add an expression-tree error classifier.

For UPDATE, the canonical write descriptor enforces NOT NULL only for
effectively assigned columns, matching SQLite. Reuse this effective-assignment
decision for the descriptor and its ABORT effect. Affinity and serialization
remain unchanged; untouched schema constraints are not rechecked.

### Coordinator admission and failure completion

Add `TransactionStatementOptions::uses_statement_journal`, defaulting true for
existing low-level callers. `WriteSession` passes the immutable program's
derived requirement, including after reprepare.

Create an anonymous savepoint iff all four conditions hold:

```text
write access AND non-autocommit AND statement rollback policy
AND uses_statement_journal
```

Always materialize named savepoints and begin/end the managed B-tree statement
epoch as before. Successful journal-free statements retain the transaction's
B-tree session while retiring their token and escaped handles.

Allow the existing statement rollback operation to receive an optional
primary error code. The session supplies it and retains the original error
while combining cleanup context. No-argument rollback remains explicit
cancellation and must not assume that a direct caller made no changes.

Implicit write failure still rolls back the whole implicit transaction.
Explicit write completion follows `sqlite3VdbeHalt()`:

| Cause | Completion |
|---|---|
| Transaction-rollback policy | Full rollback |
| I/O or interruption | Full rollback, even with an anonymous savepoint |
| OOM/FULL with an anonymous savepoint | Roll back and release that savepoint |
| OOM/FULL without one | Full rollback |
| Ordinary ABORT with an anonymous savepoint | Roll back and release it |
| Ordinary ABORT without one, Pager unpoisoned | End epoch/token; retain the outer transaction |
| Poisoned Pager without one | Full rollback |
| Explicit cancellation without one | Full rollback |

Read I/O/OOM/FULL also rolls back an existing outer write transaction, since
the failure can originate in dirty-page pressure. Ordinary read errors and
read-only interruption finish only the read statement.

Close VM resources before coordinator completion. Preserve statement-change
and last-rowid publication, reset/finalize/reprepare behavior, stale-writer
checks, generation invalidation, and existing rollback failure/retry paths.
Never retain a usable-looking outer transaction after failed restoration, or
turn durable commit-cleanup failure into rollback.

SQLite's other-active-reader admission branch, conflict-specific
FAIL/IGNORE/REPLACE/ROLLBACK, triggers, foreign keys, multi-row VALUES, and
INSERT SELECT remain deferred. No duplicate rollback mechanism or destructor
I/O is introduced.

## Verification and acceptance

Reuse the existing fixtures and fault harnesses. Focused regressions cover
compiler effects, cloning, scoped ANALYZE, assigned versus untouched NOT NULL,
coordinator savepoint counts, managed epochs, named savepoints, ordinary
journal-free failures preserving prior work, mid-scan ABORT, and the special
error table above. Exercise cleanup failure, OOM, reset, finalize, reprepare,
changes, last rowid, and stale handles through existing helpers.

Use native controls and coordinator/diagnostic work counts to prove omitted
savepoint and preimage work. Generate one repaired matched write baseline
against immutable v2 and the retained exact pre-repair baseline. Preserve both
durability profiles, SQL, result digests, workload sizes, limits, deadlines,
and guards. Record explicit batch INSERT, point/scan mutations, commit,
rollback, allocations, and subjournal work.

After focused red/green and complete fast units, run the existing closing
plan once on the final production head: Debug, Release, ASan/UBSan,
instrumentation, GCC 16 Debug/Release, native contracts, four fuzz domains,
clang-tidy, formatting, graph, discipline, implementation review, and hosted
checks. Documentation-only updates reuse matching evidence under ADR-0102.

## Consequences

Repeated single-row writes avoid unnecessary savepoint/preimage work.
Immutable programs gain two effect bits, and failed statement completion
receives its actual error class. Existing transaction, Pager, journal, B-tree,
and cleanup owners remain canonical.

## Evidence

Design `1614805` and implementation `5b304be` translate this mechanism.
Focused regressions first reproduced journal-free FULL and setup-OOM
completion failures, then passed with the native admission/error rules.
Review also closed missing catalog/OOM cause propagation before qualification.

The retained exact-core comparison is
`conditional-journal-write-before-v2` versus
`conditional-journal-write-after-awake-v2`; immutable v2 is unchanged.

| Explicit batch INSERT | Prior wall ratio | New wall ratio | Prior allocations | New allocations |
|---|---:|---:|---:|---:|
| engine-default | 5.303962 | 4.927977 | 743,188 | 677,649 |
| matched-durable | 5.834854 | 4.966247 | 1,106,646 | 677,649 |

Both profiles preserve results, final database digests, workloads, durability,
and guards. A sleep-contaminated capture was rejected, not used as evidence.
The remaining matched scan ratios are 2.819651x DELETE and 1.837035x UPDATE.

Fast-local passes 1,443 batched plus 92 isolated cases. Debug, Release,
ASan/UBSan, and GCC 16 Debug/Release each pass 1,577 isolated cases;
instrumentation passes 1,594. Shared checks execute 27 cases covering 161
configured instances; native contracts pass 58 cases. Four 20,000-run
seed-102 fuzz domains, clang-tidy, formatting, graph, and discipline pass.

## References

- ADR-0047: Reference-Faithful Single-Database Transaction Coordination
- ADR-0098: Retired Statement Completion After Rollback
- ADR-0102: Change-Aware Verification
- ADR-0111: SQLite Mechanism Baseline and Implemented-Engine Audit
- ADR-0113: SQLite In-Memory Statement Journal and Spill
