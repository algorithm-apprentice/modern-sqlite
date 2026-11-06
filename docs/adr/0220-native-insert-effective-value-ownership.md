# ADR-0220: Native INSERT Effective-Value Ownership

- Status: Accepted
- Date: 2026-11-06
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #298 main `d946177a`, ledger248
- Related contracts: ADR-0048/0157/0159/0160, ADR-0199/0200 and ADR-0106/0111

## Confirmed mismatch and completed ownership map

The remaining generation-owner investigation confirms an existing single-row
INSERT defect, independently of constant-function factoring. For an ordinary
table `t(a,b)`:

```sql
INSERT INTO t(a,a,b) VALUES(7,abs(-9223372036854775808),9);
```

Pinned SQLite inserts `(7,9)` without executing the unused expression.
Modern executes that expression and reports integer overflow. The same
distinction applies to earlier overwritten hidden-rowid/INTEGER PRIMARY KEY
inputs and unused query bodies. Ordinary unused wide-integer and scalar-width
requirements also incorrectly reject Modern preparation.

Twelve fresh paired writable-facade cases contain six phase/persisted-row
differences and six controls. Nine paired callback cases contain seven
order/count differences, with ordinary INSERT and UPDATE controls matching.
Fourteen additional prepare-only controls distinguish ignored width/collation
requirements from required eager construction, names, arity, aggregate legality
and compound structure. These are bounded research populations, not35 unique
defects, a behavioral red or a timing improvement.

Both writable engines used separate identical owned fixtures. Unmodified
pinned SQLite read both resulting files. Callback probes used the same
nondeterministic identity function in both engines, with no callbacks during
preparation. Existing stored values agree in those nine controls even when
callback order or count differs.

| Responsibility | Pinned or current source |
| --- | --- |
| Reduce single-row VALUES to its expression list | `sqlite3.c:140172-140183` |
| First ordinary input and last normalized rowid input | `sqlite3.c:140296-140329` |
| Resolve names in every supplied expression | `sqlite3.c:140419-140437` |
| Generate selected ordinary columns in storage order | `sqlite3.c:140577-140661` |
| Generate the effective rowid afterward | `sqlite3.c:140729-140741` |
| Earlier eager VALUES construction and its errors | `sqlite3.c:139859-140003`; ADR-0159 |
| Distinct UPDATE rowid-first generation | `sqlite3.c:161217-161288`; ADR-0157 |
| Normalized targets and current first/last ownership | `LookupMutationField`, `MarkInsertEffectiveness`, `BoundInsertValue` |
| Transitive non-value binding | `BindUpdateRhs`, `BindScope::requires_value`, `binding_only_` |
| Early construction before mutation binding | `InitializeCommon`, `NormalizeValuesConstruction` |
| Existing effective-field selection and emitter | `PrepareUpdateRecord`, `EmitExpression`, `EmitInsert` |
| Effective-only query invocation planning | `PhysicalMutationPlanBuilder::BuildMutationQueries` |

Current INSERT binding marks effectiveness only after binding every value.
Its lowerer then emits every supplied expression in source order, including
ineffective values. The optimizer already records query invocation demand only
for effective INSERT roots. Binding and emission must agree with that owner.

ADR-0048's requirement to execute every duplicate INSERT input, and ADR-0157's
preservation of that premise, are contradicted by these pinned executions.
Their name-binding/parameter and UPDATE contracts remain valid.

## Decision

### Establish existing target ownership before value binding

Keep the existing source-ordered `BoundInsertValue` array and normalized
`BoundMutationField` identities. Resolve the supplied targets into those same
slots, then run `MarkInsertEffectiveness` before binding their RHS expressions.
Use the existing incomplete-slot pattern from UPDATE; no partial owner may
be published on error or allocation failure.

Retain first ownership for an ordinary column and last ownership for all
names resolving to the same hidden-rowid/INTEGER PRIMARY KEY destination.
Real catalog columns still take precedence over hidden-rowid names through
the current resolver. Do not sort the public input array, discard syntax or
parameters, add another target map, or rebind successful expressions.

Rename the existing private `BindUpdateRhs` helper to reflect its shared
mutation role, and use it for INSERT and UPDATE. Bind INSERT expressions in
their original source order. Effective values retain their current required
binding; only ineffective values enter the existing transitive binding-only
mode. A leaf-only `requires_value` change is insufficient for nested queries
and aggregate arguments.

All supplied expressions still receive name, function-arity, aggregate-legality,
parameter and compound-structure checks. Unused scalar-width, ordinary literal
materialization and required comparison/ORDER collation are value demands,
not reasons to evaluate an ineffective input. Effective inputs retain those
requirements.

Preserve `InitializeCommon` and the earlier construction pass. An invalid
first or incoming eager VALUES row still fails even when the enclosing INSERT
input is later ineffective. Do not broaden binding-only mode into a blanket
construction exemption, change parser behavior, or confuse an ineffective
input with an optimizer-pruned required value.

### Share effective-field lookup and preserve native statement ordering

Extract the existing UPDATE effective-field lookup into one private lowerer
helper over the existing supplied-value/assignment spans and normalized fields.
Use it in both INSERT and UPDATE. Keep ADR-0157's accepted bounded
inputs-times-columns lookup behavior without another mapping allocation,
register container, execution mechanism or public contract.

INSERT retains its current register layout, default/NULL initialization,
statement-wide query bindings and ordinary expression emitter. Generate only
effective ordinary fields, in `BoundMutationTarget::columns` order, skipping
the rowid-alias payload slot. Then generate the effective rowid into the
existing rowid register. Unspecified fields retain their existing defaults;
an unspecified rowid retains normal generation. No ineffective expression
may acquire runtime side effects merely because its bound node was retained.

UPDATE keeps its distinct rowid-first generation and immediate integer
coercion, followed by ordinary schema-order fields. Sharing lookup must not
change its access plan, source snapshot, assignment policy or callback order.

Keep existing typed failures, cursor admission, rowid resolution, affinity,
record encoding, index maintenance, statement effects, transaction publication
and cleanup. This changes which already-bound input is generated and its
required ordering, not a persistent mutation algorithm or durable transition.
The same query-frame machinery continues to serve read and mutation hosts.

### Bound the supersession

Supersede only ADR-0048's all-input/source-order INSERT emission premise and
ADR-0157's statements preserving that premise. Every duplicate still binds
names and retains parameter slots. UPDATE's accepted ownership and ordering,
the literal bridge and constructor/default contracts remain unchanged.

This does not implement the broader ordinary reached-once/deferred generation
owner, native eager-alias identity, signed-alias generation or general
diagnostic arbitration. In particular, active DML/collation diagnostic-winner
differences remain distinct from the newly confirmed ineffective-input
admissions. Do not invent a universal first/last-error rule here or claim all
INSERT preparation ordering has been aligned.

No IN query RHS, INSERT SELECT, multirow INSERT, trigger, generated-column,
new timing family or later DAG node is admitted.

## Focused proof and delivery

Begin with one owning lowerer regression combining a discarded failing/value-
invalid input with observable ordinary-field and rowid callback order. Record
the real pre-change preparation/execution failure before production edits.
Then cover ordinary duplicates, normalized rowid aliases, implicit column
lists, query-valued inputs and the existing UPDATE control.

Add focused binder/public-write checks for ignored literals, width and
collation, while retaining names, arity, aggregate and compound errors.
Include the early eager-constructor counterexamples, parameter retention,
default selection, reset/rebind, error retirement and original allocation/OOM
owners. Reuse existing fixtures and helpers; preserve every ceiling,
default-stack limit, workload, assertion and deadline.

Correct the existing tests that require discarded parameters/callbacks or a
discarded failing callback to execute, using the independent native evidence
rather than weakening expectations. Record changed instruction/callback work
and actual allocation behavior; claim no unmeasured speed improvement.

Run the focused green, complete fast tier and actual change-aware closing
plan. Independent design acceptance precedes implementation; bounded source
review and a normal coherent commit/ledger entry precede delivery. Final
exact-source local/hosted gates and expected-OID cleanup remain mandatory.
This closes one confirmed ownership prerequisite, not the general generation
owner, IN, node43 or the project.
