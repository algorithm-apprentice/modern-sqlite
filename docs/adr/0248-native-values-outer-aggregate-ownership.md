# ADR-0248: Native VALUES Outer Aggregate Ownership

- Status: Accepted
- Date: 2026-11-10
- Module graph node: `implement-advanced-sql`
- Prerequisite: qualified PR #321 main `f015a1cf`, ledger350
- Related contracts: ADR-0106/0110/0111/0159/0192/0247

## Confirmed boundary and canonical inventory

Pinned SQLite returns one row containing8 for:

```sql
SELECT (VALUES(8),(sum(id))) FROM items;
```

Both current Modern facades reject the aggregate during VALUES binding.
The later row need not generate a value, but its name-resolution visit still
assigns the aggregate to the outer SELECT. That owner, not the VALUES child,
performs the aggregate work and determines the outer result cardinality.

Thirty-three paired native/read/write observations retain twenty native-valid
preparation gaps, nine error boundaries and four exact protocol controls.
Sixteen gaps concern outer-owned VALUES aggregates in existing result-owner
contexts. Singleton local aggregation, omitted local occurrences and an
omitted outer-WHERE occurrence expose separate boundaries. The observations
predate this implementation and do not establish a regression in ADR-0247.

| Responsibility | Canonical source |
| --- | --- |
| Argument/modifier names and nearest aggregate owner | `sqlite3.c:111700-111839` |
| Result name context and aggregate-query status | `sqlite3.c:112423-112467` |
| Source references excluding subquery-local sources | `sqlite3.c:120020-120136` |
| Owner-relative aggregate analysis and deduplication | `sqlite3.c:120304-120498` |
| Ordinary VALUES row generation, without a local aggregate program | `sqlite3.c:152358-152391,152488-152493` |
| Aggregate SELECT registration and parser-owned cleanup | `sqlite3.c:157915-158015,156639-156644` |
| Required aggregate value without an installed owner slot | `sqlite3.c:118230-118241` |
| Borrowed lexical owner and clause metadata | `NameScopeView`, ADR-0110 |
| Canonical argument/modifier binder and owner selection | `BeginAggregateBinding`, `FinishAggregateCall` |
| Transactional occurrence ownership and cleanup | `PendingAggregateInvocation`, `BindingArenaCheckpoint` |
| Shared owner program publication | `FinalizeAggregateProgram`, `FinishSelectQueryCore` |
| Existing outer-only admission policy | `AggregatePermission::kOuterOnly` |
| Owner-relative aggregate results | `QueryFrame`, `EmitAggregateResult` |

`BindValuesQueryCore` currently leaves each row's `BindScope::aggregates` at
the default prohibited permission. Consequently, the shared aggregate binder
rejects the occurrence before its existing owner-selection mechanism runs.
The SELECT path already supports the required lexical owner, pending journal,
aggregate-context captures, query reparenting, alias copies and final registers.
There is no need for another aggregate implementation.

Fourteen additional native callback controls establish the execution boundary.
First-row, omitted-later-row, EXISTS and wholly unrequested result consumers
still perform three aggregate-argument callbacks over the three-row input.
Grouped input performs three callbacks and publishes two rows; empty input
performs none and publishes one aggregate row. Eager and restarted prefixes,
FILTER-only anchors and aggregate-local ORDER anchors retain the same owner.
A direct ORDER alias deduplicates to three callbacks. Query-bearing aggregate
arguments copied by ORDER or HAVING produce six. No callback runs in prepare.

The separate omitted-WHERE control performs no callback and publishes three
ordinary rows. It must not be used to justify hoisting every named aggregate
into an owner program. This slice retains the existing shared clause boundary
rather than changing late-clause generation or aggregate-query establishment.

## Decision

### Admit an outer owner through the existing VALUES row scope

Set the ordinary VALUES row binding scope to the existing outer-only aggregate
permission. Apply it uniformly to every retained row, including singleton
VALUES and rows that ADR-0247 will omit from value generation.

Reuse `BeginAggregateBinding` and `FinishAggregateCall` unchanged. Ordinary
arguments, aggregate-local ordering and FILTER retain their canonical binding
order. Source dependencies and source-anchored aggregate aliases select the
nearest lexical owner through `NameScopeView`. A local owner remains rejected
by the existing outer-only check.

This is a bounded admission of an already implemented mechanism, not a new
aggregate evaluator or a second local aggregate representation. Do not add a
field, owner table, container, bytecode instruction, syntax rewrite or special
case for a particular aggregate function or SQL spelling.

Constructor-time scopes and cached early roots remain unchanged. An aggregate
that reaches ordinary VALUES name binding uses the existing lexical scopes;
an early constructor is not retrospectively assigned an outer namespace.
Keep every VALUES row, structural width, source-type occurrence, parameter and
rightmost-result metadata. ADR-0247's generation predicate remains unchanged.

### Preserve ownership independently of child value generation

The canonical pending journal stages the invocation in its selected outer
query/core and occurrence phase. The existing finalizer alone deduplicates
and publishes the invocation in the owner's `BoundAggregateProgram`.
The VALUES child still has no local aggregate program.

Preserve the aggregate-context capture even when the child does not emit the
aggregate result. Existing parent binding establishes result-clause aggregate
status, and existing owner emission performs the input work. Query-bearing
arguments retain canonical reparenting and alias-copy identities. Do not
replace binding dependencies with only generated-value dependencies.

An actually requested aggregate result resolves through `QueryFrame` and
copies the finalized owner register through `EmitAggregateResult`. The child
must not step, reset or finalize the owner's aggregate capability.
Global, grouped, nested, joined and empty-input owners use the same path.

Keep the existing typed clause checks on return from `BindSubquery`.
Do not loosen WHERE, ON, GROUP BY, aggregate-argument or mutation-target
restrictions to make a local VALUES test pass. Keep the existing arena/journal
checkpoint and RAII unwinding for every typed or allocation failure.

## Focused proof and delivery

After independent design acceptance, add one observable failing test for the
omitted-later-row example before changing production. Expand the owning proof
to first/single/later rows, EXISTS, an unrequested result, empty/grouped input,
eager/restarted prefixes, explicit compounds, nested owners and FILTER/ORDER
source anchors. Reuse the current fixture, callback functions and runtime
helpers rather than introducing a second SQL harness.

Check parent program ownership, upward-only aggregate captures, query-argument
reparenting and unchanged VALUES partitions in the existing binder tests.
Retain required name, arity, argument-width, collation, nested-aggregate and
clause-error controls. Verify real alias callback counts against the recorded
native baseline, including the three-versus-six distinction.

Exercise both read and write facades through their common query owner.
Retain reset/rebind, statement failure and recovery coverage. Add a focused
scenario to each existing complete binder/lowerer allocation-prefix owner,
without changing ceilings, workers, assertions, workloads or deadlines.

No persistent format, VM capability, public carrier size or ordinary
nonaggregate execution mechanism changes. The unsupported-before baseline
records preparation rejection; native callbacks provide an execution schedule,
not a Modern speed or heap-saving claim. Preserve ordinary SELECT aggregate
and previously delivered VALUES generation controls.

Run focused green, the complete fast unit tier, current owning strict checks
and the actual change-aware closing plan. Preserve original failures and exact
source-input identities. Independent source/integrated review, normal squash
and separate actual-main qualification remain required.

## Explicit remaining boundaries

Local VALUES aggregation, including native-valid singleton local aggregates,
remains outside this admission. A row containing both outer-owned and local
aggregate occurrences retains the local-owner boundary. Omitted local
occurrences and late outer-clause occurrences require their own completed
name/aggregate-registration/generation mapping.

The wholly unrequested first-value finding is not VALUES-specific: twenty-three
additional paired controls reproduce the same early value-demand difference
for direct literals and ordinary scalar SELECT. It must not be patched by
changing only the first VALUES row. General logical value-demand/error
arbitration, fixed-factorable/FIFO initialization, unused original producers,
the retained deferred callback gaps and IN query RHS remain open.
This ADR does not complete node43 or begin a later DAG node.
