# ADR-0240: Native Guarded Mutation Generation

- Status: Accepted
- Date: 2026-11-09
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #314 main `6bb1d998`, ledger320
- Related contracts: ADR-0048/0051/0106/0111/0119/0136/0157/0182/0238

## Requirement and observed defect

Top-level UPDATE and DELETE still classify a known false or NULL conjunct as
an empty access path. Lowering emits completion instead of the ordinary
mutation program. This drops both the generated body and preceding applicable
guards, not merely execution of rows that cannot qualify.

Twelve fresh paired runtime controls on the qualified source establish five
native-error/Modern-success differences. For example:

```sql
UPDATE items SET score=score
WHERE abs(-9223372036854775808)>0 AND 0;

DELETE FROM items
WHERE abs(-9223372036854775808)>0 AND NULL;
```

Pinned SQLite executes the preceding guard and reports integer overflow.
Modern reports successful completion. The controls also cover a NULL guard
before the failing expression, point access, unbound-parameter guards,
source-ready expressions, and an unreachable UPDATE assignment. All twelve
use separate owned writable fixtures and retain the original table contents.
The native connection is genuinely read/write; a read-only-open failure is
not used as an expression-error observation.

The earlier supported fourteen-case preparation study has matching preparation
outcome categories. It remains a separate prepare/finalize observation, not
evidence of the new runtime error regression.

Current optimizer/lowering preparation measurements contain twelve cases,
eight samples per domain and 192 stable observations. Their fresh probe uses
the current class definitions and the accepted source317 library. All tracked
owners retire. False/NULL mutation programs omit scalar-call instructions and
cursor descriptors and publish zero write effects; parameter, scan and point
controls retain the corresponding ordinary generation. These are preparation
allocation/program-shape observations, not a timing improvement claim.

## Completed bounded source and canonical-owner inventory

| Responsibility | Canonical source |
| --- | --- |
| False-term branch followed by normal WHERE planning | Pinned `sqlite3.c:176553-176617` |
| UPDATE write intent, one-pass selection and original-row collection | `sqlite3.c:160930-161165` |
| UPDATE writer/body/assignment/constraint generation after WHERE selection | `sqlite3.c:161165-161365` |
| DELETE one-pass selection, rowid RowSet and ordinary deletion body | `sqlite3.c:133610-133855` |
| Function generation marks possible ABORT | `sqlite3.c:87863-87893` |
| Top-level write/multi-write/ABORT ownership | `sqlite3.c:132161-132220` |
| Rowid conflict generation | `sqlite3.c:132264-132280,141475-141540` |
| Prior-error barrier, transaction prologue and deferred drain | `sqlite3.c:126820-126950` |
| Freeze the conjunction of compiler effects | `sqlite3.c:90118` |
| Shared truth, dependency and exact-rowid rules | `ConstantTruth`, `IsStatementGuard`, `RowIdLookupKey` |
| Mutation conjunct/access ownership | `AnalyzePredicate`, `ChooseAccess`, `BuildMutation` |
| Ordinary guard, exact, one-pass and collected emission | Existing `PlanLowerer` mutation producers |
| Constraint/function effect ownership | Mutation descriptors and canonical `PlanLowerer::Append` |
| Immutable journal requirement and session forwarding | `BytecodeProgram::uses_statement_journal`, `WriteSession` |
| Savepoint admission and error/cursor cleanup | `TransactionCoordinator`, ADR-0119/0136/0179/0182 |

The existing read and mutation analyses share the truth/dependency helpers but
publish different contracts. Read `ReadPredicateTerm` retains conjunct position
for WhereLoop construction. Mutation analysis selects its existing exact-rowid
key before compacting guards/residuals, preserving its current allocation
ownership. Do not copy read terms into another mutation container, fabricate
a read query, or introduce a second access solver.

Native UPDATE can emit an ephemeral-open instruction before the false-term
bypass and later replace it with a Noop for one-pass access. Native rowid
DELETE instead uses a RowSet when collection is required. They are not one
identical native container. The accepted Modern architectural translation
already owns a bounded ordered rowid list under ADR-0048/0051/0157. Its clear,
append, rewind and advance instructions, collection-before-mutation order,
limit, reset and cleanup remain canonical. This correction does not introduce
an ephemeral B-tree or change that accepted collection representation.

## Decision

### Preserve ordered guards and the ordinary access choice

Remove the mutation analysis's early empty classification. Retain false and
NULL terms and continue classifying every remaining conjunct in its original
order. Use the existing `IsStatementGuard` rule for pre-open guards and retain
source-ready terms as residuals. A known eventual truth does not permit
hoisting a nondeterministic or row-dependent expression.

Keep the current treatment of known-true terms. Preserve the existing
transparent-expression, conjunction, source-dependency and exact-rowid rules.
Select the existing first eligible rowid lookup when applicable; otherwise
publish the ordinary table scan. Keep key consumption, guard/residual ordering
and current compacted-vector ownership. Do not add a predicate cache or change
read-side WhereLoop planning.

Reuse `BuildMutation`'s current choice between point, one-pass and collected
mutation. Rowid changes, indexes and WHERE-query presence keep their existing
collection decisions. Retained mutation-query bodies keep the canonical
clause/pass-sensitive planning context from ADR-0157.

Affected public physical plans intentionally describe the real guarded access
instead of optimizer-elided empty access. Keep the existing public enum names
and unrelated empty/no-op handling; this is not a representation migration.

### Generate and execute through existing mutation owners

Let the ordinary lowering path allocate its layouts, register its descriptors
and generate its guards, source traversal and mutation body. A false/NULL
guard reaches the existing completion before source-row access or ordinary
row-body execution.

Preceding applicable guards must execute, including their errors. A later
guard or assignment must not execute after an earlier false/NULL guard.
Source-ready calls remain inside the row loop. Do not repair the missing
guards with a second shadow expression pass or a special empty-program emitter.

Preserve effective-assignment ownership, rowid-first coercion, schema-column
order, index maintenance, original-row collection and existing cursor cleanup.
The collection owner remains statement-local; reset clears its logical contents
and canonical halt/error cleanup retires its positioned state and capabilities.
No new VM instruction, verifier exemption, session path or storage mechanism
is needed.

### Retain compiler effects even when the row body is unreachable

Use the existing lowering decisions:

- table-scan mutation retains `multi_write`; proven point access does not;
- generated scalar calls and emitted rowid/UNIQUE checks mark `may_abort`;
- applicable assigned NOT NULL checks use the existing descriptor owner; and
- a potentially changed rowid retains its ordinary ABORT effect.

This narrowly supersedes ADR-0119's classification of optimizer-elided empty
UPDATE/DELETE as non-multi-write and ADR-0182's preserved empty-DML body/effect
boundary, not its default-ABORT or no-op-DDL policy. Runtime zero rows do not erase
the compiler's ordinary access and generated effects. A non-abortable guarded
scan still does not require an anonymous statement journal. Do not mark every
guarded mutation abortable or infer effects again in the session.

Keep statement/default-ABORT atomicity separate from journal admission.
The immutable effect conjunction still reaches the coordinator unchanged.
Explicit journal admission, setup-OOM handling, ordinary ABORT, implicit full
rollback and OOM/FULL cursor-trip policy remain governed by their existing
owners, including when no anonymous savepoint was materialized.

### Keep the remaining generation boundary explicit

This is the top-level UPDATE/DELETE counterpart to ADR-0238, through the
existing shared mutation owner. It adds no SQL syntax or broader writer
access path. Read-query generation, INSERT duplicate/effective ownership,
no-op DDL, the transaction coordinator and persistent encodings are unchanged.

Do not activate non-function deferred evaluation, reusable/fixed request
modes, unused original-producer initialization, synthetic-zero reuse,
signed-token materialization or preparation-error accumulation. The existing
nine deferred callback differences and other signed/preparation differences
remain separate open work. No IN or later DAG node is admitted.

## Focused proof and delivery

After independent design acceptance, add one owning runtime regression for
the preceding-guard error and record its actual current failure. Implement
the smallest mutation-analysis correction and run that regression plus the
complete fast unit tier.

Extend the same owning tests to cover UPDATE and DELETE, false and NULL,
guard order, source-ready and unreachable-body controls, point access and
existing collection. Inspect retained function/constraint generation and the
resulting effect/journal contract. Exercise zero changes, unchanged contents,
explicit prior work, reset and applicable setup/failure cleanup through the
existing fixtures. Do not invent a preparation-error mismatch from the
separate preparation study.

Update only directly dependent empty-plan/body expectations. Preserve
ordinary nonempty/parameter controls, current allocation ceilings, default
stacks, rowid-list bounds and malformed-input checks. Reuse the owning
optimizer/lowering fault sweeps; add a focused case only where this retained
combination is missing. Compare current preparation ownership/program shape
with the recorded baseline and report added preparation cost honestly.

Retain the preparation protocol populations, delivered reached-once/original
VALUES controls and remaining deferred caller baseline. Run the actual
change-selected closing plan, strict compiler/checker owners and required
hosted gates without weakening deadlines or workloads.

Deliver one coherent reviewed node43 slice with normal commit/ADR/ledger
traceability, independent source and final integration review, squash, and
separate actual-main qualification. Historical CI failures remain preserved;
the completed PR314 descendant closure is not their causal explanation.
