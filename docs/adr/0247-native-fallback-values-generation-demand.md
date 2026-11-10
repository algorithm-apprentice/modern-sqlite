# ADR-0247: Native Fallback VALUES Generation Demand

- Status: Accepted
- Date: 2026-11-10
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #320 main `19d25269`, ledger346
- Related contracts: ADR-0106/0111/0159/0160/0192/0222/0237/0245/0246

## Confirmed boundary and completed mechanism inventory

The remaining generation work exposes an earlier request-selection boundary:

```sql
SELECT (VALUES(8),(id+0x10000000000000000)) FROM items;
```

Pinned SQLite prepares this query and returns8 for every outer row. Both
Modern facades instead reject the later literal during binding. Seven
initial paired controls and twenty additional scope controls retain the raw
native/read/write outcomes. The initial findings also reproduce on qualified
main342, before ADR-0246; this is not a retained-argument regression.

The difference is not a general rule that scalar queries may discard every
later expression. Native construction sometimes converts VALUES rows into
early coroutine producers. Otherwise it retains an ordinary UNION ALL chain
with `SF_MultiValue`. Its specialized generation path emits only the first
row when scalar/EXISTS consumption supplies LIMIT1. Names and structural
widths have already been checked across the complete chain.

| Responsibility | Canonical source |
| --- | --- |
| Incoming/current-row gates, coroutine creation and fallback flags | `sqlite3.c:139836-140003` |
| Explicit compound syntax clears `SF_MultiValue` | `sqlite3.c:184710-184723` |
| Complete name resolution and compound-width checks | `sqlite3.c:112316-112578` |
| Names precede retained source-type information | `sqlite3.c:155992-156012`; ADR-0192 |
| Scalar/EXISTS destinations, correlation and LIMIT1 | `sqlite3.c:116711-116851` |
| First-row-only ordinary VALUES generation | `sqlite3.c:152340-152391,152488-152493` |
| Ordinary compound generation still visits both sides | `sqlite3.c:152511-152581` |
| Immutable constructor partition and prepared roots | `NativeValuesConstruction`, `ConstructValuesRow`, ADR-0159/0222 |
| Ordinary rows and value revalidation | `BindValuesQueryCore`, `ResultDefinition::additional_syntax`, `ValidateResultDefinitionsInSyntaxOrder` |
| Binding versus value captures | `FinalizeQueryCaptures`, `BoundSourceCapture` |
| Nested invocation estimates | `PhysicalPlanBuilder::RecordCoreLoops` |
| Shared streaming/ordered row generation | `VisitValuesRows`, `EmitValuesExpression` |

The admitted immutable syntax forbids ORDER BY or LIMIT on a rightmost
VALUES core. A sole ordinary VALUES core inside scalar/EXISTS therefore
receives the existing normalized LIMIT1, without an explicit OFFSET.
Explicit compounds clear the native flag even when they use UNION ALL.
Once any eager constructor exists, subsequent fallback rows do not restore
that whole-query fast path. Multiple eager cohorts remain distinct.
Window, WITH and schema-definition admissions remain outside this slice.

The expanded controls distinguish omitted later literals and nested scalar
widths from required first-row errors, explicit compounds, earlier/restarted
eager producers and constructor-owned incoming values. A later missing name
still fails; it must not lose to an unrequested oversized literal.
An ignored nested eager source still requires its type/collation work.
Unused plain collation wrappers already agree and remain unchanged.

## Decision

### One shared generation predicate over existing owners

Add one allocation-free binder-layer predicate,
`BoundValuesGenerateOnlyFirstRow`, beside the existing query-demand helpers.
It consumes the bound query, existing VALUES schedule and borrowed chunks.
It is true only for the current scalar/EXISTS destinations, one syntax core,
and an entirely consumer-filtered schedule/partition.

Use the same predicate before row binding with the complete private
construction partition, and after publication with the existing compact or
retained bound partition. `core_count` is established before binding; do not
rely on the compound-operator vector while it is still being populated.
An empty published chunk span inherits the existing schedule. An eager
schedule or any eager chunk excludes the specialized path.

This translates the mapped native flag/caller combination without adding a
flag, row-count cache, query owner, syntax walk, expression arena or runtime
state. Keep every bound row and its original expression IDs. This is a
generation-demand decision, not a row-deletion or name-resolution policy.

### Preserve complete binding while selecting actual value requests

In `BindValuesQueryCore`, retain every row-width check, name/function visit,
parameter assignment, constructor cache lookup and source-type occurrence.
For the specialized path, only the first row can request ordinary result
values. EXISTS continues to omit its result values through the current scope.
Use `BindScope::requires_value` and its existing literal/subquery machinery;
do not recognize particular literal spellings or catch and discard errors.

Only generated additional rows belong in
`ResultDefinition::additional_syntax`, which is consumed exclusively by the
existing value-revalidation routines. Thus a later value-demand replay cannot
materialize an omitted row. Retain the complete bound rows and subquery
metadata for binding, type information and actual template/alias ownership.
Do not add another validation list or change the revalidation algorithm.

This reuses ADR-0160's owned-source literal tokens and the existing
binding-only child-query mode. An omitted nested scalar's value-only width
check is deferred; a malformed VALUES row width is still an error.
Missing names, wrong function arity and retained eager source-type failures
remain visible. Constructor-time preparation and its error precedence do not
change because eager partitions cannot enter this path.

### Keep correlation while removing unrequested value dependencies

In `FinalizeQueryCaptures`, consume result values only from generated VALUES
rows. Retain all binding captures, including names used only by omitted rows.
Do not erase a query's correlation merely because its captured value is no
longer loaded.

The existing subquery emitter chooses correlated invocation from the complete
capture collection, not only captures with a value. A nondeterministic first
row with a name-only outer reference in a later row must still execute for
each outer row. Real alias copies retain separate invocations.

Apply the same row selection in `RecordCoreLoops`. Omitted child expressions
retain the existing initial invocation context for binding-only owners.
For specialized EXISTS, the first row's omitted result values likewise
retain that initial context, following the existing SELECT result-elision
rule. Preserve every unaffected compound/eager path and the existing query
planning order; add no alternate cost model or query registry.

### Reuse the canonical row emitter and all its validation

At `VisitValuesRows`, keep full chunk coverage, schedule and row-width
validation over every retained row. Invoke the existing row-generation
callback only for the first row when the shared predicate applies.
Do not return early before validating the remaining metadata.

Streaming and ordered callers therefore share the same selection. Their
existing result/EXISTS projection, LIMIT completion, label ownership and
error cleanup remain authoritative. Do not generate a dummy row, empty
value, second executor or new bytecode instruction.

Required first-row generation remains unchanged. Client VALUES, explicit
compounds, all eager producers and mixed/restarted partitions retain every
current generation request. Original producer records, once ownership,
constant factoring, fixed argument ranges and initializer order are
unchanged. Both read and mutation hosts use the same common query owner.

## Baseline and bounded proof

Ten new matched callback/resource controls run on qualified main346. All
native and Modern rows and both independently counted callback families
agree; neither engine invokes a callback during preparation.
The name-only-tail nondeterministic case executes3 callbacks, and its actual
WHERE/projection alias copies execute6. These counts must remain unchanged.

The currently unrequested later-function case nevertheless emits one Modern
scalar call, retains one value capture and produces28 instructions with13
registers. Its callback executes zero times. This distinguishes generation
from runtime work. Explicit-compound and earlier-eager controls also retain
the later generated call, and must continue doing so.
These are fixed-work/resource baselines, not timing or heap-saving claims.
The shared predicate adds no stored fields or public carrier growth.

After independent design acceptance, record a focused failing preparation
and row-execution test for the original oversized-literal query before any
production change. Then cover scalar/EXISTS, real alias copies, ignored
nested literal/width values, required first-row errors, missing-name priority,
parameter metadata, structural row widths and retained nested source types.
Keep explicit-compound, client, eager and mixed/restarted controls.

Verify binding-only captures separately from generated value captures, and
retain the3/6 nondeterministic callback counts. Check absence of the omitted
scalar instruction without confusing it with zero runtime callbacks.
Exercise common read/write ownership, reset/rebind, owned source lifetime,
typed failure/recovery and the existing complete allocation-prefix owners.
Use a few focused cases in existing owner files. Do not reduce an existing
case population or relax any allocation ceiling, assertion, workload, stack
allowance or deadline to make the new behavior pass.

Run focused green, complete fast units, current owning strict checks and the
actual change-aware closing plan. Preserve all prior raw outcomes and
authenticate genuinely changed results through the pinned oracle. Independent
source/integrated review, normal squash and separate actual-main qualification
remain required before continuing.

## Explicit remaining boundaries

The twenty-case mapping also confirms two separate existing differences:
outer aggregate ownership in ordinary VALUES, and an invalid first-row value
inside a wholly unrequested scalar query. Neither is repaired by pretending
that the first row is an omitted later row or by suppressing name-phase
errors. Retain these observations for their own completed mechanism mapping.

General fixed-factorable scheduling, FIFO initialization, unused original
producer generation, preparation-error arbitration and the retained deferred
callback/constructor gaps remain open. This source does not admit IN query
RHS, complete node43 or begin a later DAG node.
