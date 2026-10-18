# ADR-0110: SQLite-Aligned Outer Aggregate Ownership

- Status: Accepted
- Date: 2026-10-18
- Module graph node: `implement-advanced-sql`

## Context

ADR-0092 completes semantic result-reference copies and query-bearing
aggregate ORDER registration. Five native-valid subquery records remain
explicitly unsupported:

- a direct aggregate whose arguments reference only an outer query;
- ownership selected by an outer-only FILTER;
- an outer-owned aggregate in discarded EXISTS ordering;
- an outer aggregate result alias consumed through a HAVING child; and
- the same alias consumed through an ORDER BY child.

SQLite does not assign every aggregate function to the query containing its
syntax. After resolving arguments, FILTER, and aggregate-local ordering, it
walks outward to the nearest name context whose source list is referenced.
That context owns the aggregate. A local reference keeps the invocation local
even when other arguments are outer; an outer-only reference moves the
invocation outward. With no source dependency, the aggregate belongs to the
definition's current name context.

Pinned SQLite 3.54.0 implements this in:

- `src/resolve.c:1287-1402`, where aggregate legality is checked, every
  argument/modifier is resolved, and the nearest referenced `NameContext`
  receives `NC_HasAgg`;
- `src/expr.c:7507-7678`, where aggregate analysis uses the recorded nesting
  depth to install each function in its owning `AggInfo`;
- `src/select.c:7098-7118`, where a HAVING term already owned by an outer
  aggregate is kept out of the HAVING-to-WHERE rewrite; and
- `src/resolve.c:1968-2020`, where each SELECT receives its own name context
  and aggregate-query status.

The existing Modern mechanisms are sufficient:

- canonical borrowed `NameScopeView` chains and saved query scratch;
- one statement-global expression/query/source arena;
- one per-core pending aggregate list and phase-ordered finalizer;
- query/source/context captures and inline lexical compiler frames;
- statement-global aggregate invocation IDs and per-query programs;
- canonical aggregate source/representative leaf analysis; and
- existing aggregate result registers, reset, cleanup, and VM state.

No second aggregate evaluator, runtime closure, copied row, bytecode opcode,
or verifier exception is needed.

### Native ownership and schedule audit

An independent audit against the pinned local library uses three input rows
and a non-deterministic counter:

| Shape | Result or callback schedule |
|---|---|
| `SELECT (SELECT sum(o.id)) FROM t o` | one outer aggregate row, value `6` |
| Child aggregate argument using local and outer fields | remains child-local |
| Outer-only argument with a child FROM source | remains outer-owned |
| Outer-only FILTER or local ORDER term | assigns ownership to the outer query |
| Local FILTER or local ORDER term | keeps ownership in the child |
| Pure `sum(tick()) AS x`, child HAVING alias | three calls; original invocation reused |
| `sum((SELECT tick())) AS x`, child HAVING alias | two calls; copied query creates a second outer invocation |
| Child `sum(x)` over outer source-free `count(*) AS x` | child-local and valid |
| Child `sum(x)` over outer source-anchored `count(id) AS x` | nested aggregate misuse |
| Query-bearing alias in both child ORDER and HAVING | three calls in result, ORDER, HAVING order |
| Repeated query-bearing alias references | one fresh invocation per reference |
| Outer-owned query-bearing result with direct ORDER reuse | two calls |
| Equivalent compound output ordering without a local child aggregate program | one call |
| Grouped outer owner | one completed outer invocation per group |
| Ignored EXISTS projection or ordering | outer aggregate work still executes |
| Child `LIMIT 0` or a skipped `coalesce` branch | outer aggregate work still executes |
| Source-free singleton child ORDER with local `sum(tick())` | ordering discarded, zero calls |
| Discarded child ORDER owned by nonaggregate ancestor ORDER/WHERE | ordering discarded, zero calls |
| Aggregate in an outer WHERE or outer GROUP BY child | existing misuse error |
| Outer-owned aggregate in a child GROUP BY | legal when the child's own clause permits it |

The discarded-order distinction is observable at prepare time. A local
aggregate containing a wide scalar query or missing collation is ignored with
the EXISTS ordering, while the same syntax plus an outer source dependency
requires the scalar width or collation and reports its error.

## Decision

### 1. Select one aggregate owner from the canonical name-scope chain

Bind ordinary arguments first, retain the existing early misuse check, then
bind aggregate-local ordering and FILTER in their current error order.

Collect source dependencies from all three components through the existing
query/source traversal. Starting from the expression's actual definition
scope, select the nearest scope owning a referenced source. A local source
therefore wins over a farther outer source. When no source is referenced,
select the definition scope itself.

An aggregate-result context contributes an ownership anchor only when its
referenced invocation is itself source anchored by an ordinary argument,
FILTER, or aggregate-local ordering dependency. A source-free invocation such
as `count(*)` is a value dependency but not an ownership anchor.

`BoundAggregateInvocation` therefore records whether ownership was selected
from at least one source dependency. This is independent from whether the
owner query has a FROM source.

After selection:

- a source-anchored aggregate context participates as a source anchor at its
  owner before selection;
- an aggregate-context dependency owned by a strict ancestor of the selected
  owner remains an ordinary upward dependency;
- a dependency owned by the selected owner is nested aggregate misuse; and
- a dependency from a descendant prevents hoisting past that owner and reports
  nested aggregate misuse.

This matches SQLite's rejection of an aggregate that attempts to move past a
nested aggregate owned by an intermediate query, while admitting a child-local
`sum(x)` over an outer source-free `count(*)` result.

Extend `NameScopeView` with one borrowed aggregate-binding target:

- owning query identity;
- owning global core index; and
- aggregate occurrence phase.

The target contains no pointer to another query's mutable vector. Current and
saved scopes publish stable statement-local owner identities. Definition-
template copies retain the original scope's target. These are compiler-only
values; no published bound object stores them.

Replace the per-active-core pending vector with one binder-owned transactional
occurrence journal. Each entry records:

- immutable aggregate-call syntax ID;
- aggregate-call expression;
- still-owned invocation candidate;
- owner query and global core;
- original site query; and
- owner occurrence phase.

`BindingArenaCheckpoint` records the journal size. Every speculative compound
match, validation-only replay, discarded ordering bind, and ordinary failure
therefore truncates owner-bound occurrences together with expressions,
queries, functions, collations, and active captures. No binding path mutates
an ancestor program or capture list before commit.

The existing provisional invocation lookup searches this one journal by
expression identity. Finalization consumes only entries matching the current
query/core and marks them finalized after assigning the published invocation
ID. Ancestor entries remain staged while a child is bound. This is one
canonical pending-occurrence mechanism, not a second aggregate registry.

The owner target also records the clause's aggregate permission and whether
that query was already aggregate when ORDER BY binding began. These values are
used only by discarded-order replay in section 5; ordinary live binding keeps
the existing clause checks.

Add `AggregatePermission::kOuterOnly` for ORDER BY in a query with local
sources that is not otherwise known to be aggregate. It admits an invocation
only when ownership resolves to an outer query. A local invocation retains
the existing ORDER-only misuse. WHERE, ON, aggregate arguments, and other
prohibited contexts remain prohibited. GROUP BY keeps its bind-then-reject
behavior for a locally owned aggregate while allowing an aggregate owned by
an outer query.

A nonaggregate singleton SELECT with no local sources is a separate SQLite
case. Bind its ORDER BY through the same speculative checkpoint used for
discarded EXISTS ordering. Resolve names, functions, arguments, modifiers,
and binding dependencies, but discard locally owned aggregate work and the
ordering expression. Strict-ancestor-owned work follows section 5 and
survives. Thus `SELECT (SELECT 1 ORDER BY sum(tick())) FROM t` executes no
`tick()`, while invalid names and argument errors remain visible.

### 2. Register every invocation in its owning aggregate program

`BoundAggregateInvocation` records its owning `BoundQueryId`.

Stage each occurrence in the transactional journal with the phase captured
from the owner's clause. The existing finalizer remains the only operation
that:

- orders result, ORDER, and HAVING occurrences;
- deduplicates structurally equivalent invocations;
- assigns statement-global invocation IDs;
- rewrites aggregate-call expressions; and
- enforces the owner's aggregate-term limit.

Before finalizing one owner core, reparent nested queries in its staged
invocation components and record owner-relative binding dependencies. Only
then deduplicate and publish the invocation. Validation-only or failed
bindings never reach this commit point.

The child query publishes no aggregate program for an outer-owned call.
Mixed local/outer invocations remain child-owned and unchanged.

Pure outer aggregate aliases continue to reference the original immutable
target and therefore reuse the original invocation. Query-containing alias
copies use ADR-0092's scoped template binding, receive fresh nested-query
identities, and may create a distinct invocation in the same outer program.

### 3. Publish an explicit aggregate-context dependency

Add one capture alternative naming:

- the query that owns the aggregate result; and
- the aggregate-call expression that identifies the dependency.

This is a zero-data lexical dependency. It is not a source leaf, SQL value,
register, or copied aggregate state.

When an invocation is moved outward:

1. remove argument/modifier captures provisionally attributed to the child;
2. reparent nested queries in the invocation components to the aggregate
   owner, because those expressions execute in the owner's input phase;
3. record the invocation components' binding dependencies relative to the
   owner query; and
4. add one aggregate-context capture to the child.

Context propagation remains bottom-up through the existing query capture
walker. The owner never captures itself. Intermediate queries retain the
aggregate-context dependency so every consumer is emitted inline in the
lexical chain leading to the owner.

Every result-alias dereference records its defining query as provenance,
including aliases local to the active query and query-containing aliases
expanded through template binding. Local provenance does not become a
published capture, but it remains available while classifying hoisted or
discarded aggregate work.

Dependency transfer is owner-relative, not a byte-for-byte copy:

- source captures owned by the selected owner are local and are not captured;
- source captures from a strict ancestor of the owner propagate normally;
- alias/query contexts naming the owner are local and are not captured;
- contexts naming a strict ancestor of the owner propagate normally; and
- contexts naming a descendant between the owner and original aggregate site
  are transparent. Do not publish them on the owner; continue through the
  immutable alias target instead.

Every published source, query-context, and aggregate-context capture therefore
points strictly upward. The root query remains capture free. This prevents a
child-to-owner aggregate dependency and an owner-to-child alias dependency
from forming a cycle.

An alias target containing an outer aggregate publishes ordinary expression
properties with `contains_aggregate` cleared relative to the child. A nested
query expression sets `contains_aggregate` only when its capture identifies
the current consuming query as the owner. This keeps the child ordinary while
allowing the owner's result-list aggregate to establish aggregate-query
status and preserving existing WHERE/GROUP misuse phases.

Separately retain `references_aggregate` on bound expression properties. It is
true for any aggregate call regardless of owner and propagates through normal
expression wrappers and aggregate-context subqueries. It does not establish
child aggregate-query status. ADR-0092's simple direct ORDER alias/ordinal
copy trigger uses `has_subqueries && references_aggregate`, so an outer-owned
query-bearing result still receives its independent direct ORDER invocation.
Compound late registration continues to require a locally owned aggregate
program and therefore does not add a copy to an ordinary child core.

For compound ORDER BY, the complete selected definition is first bound inside
an arena/journal checkpoint. Use the resulting journal entries only to
identify aggregate-call syntax owned by the activated compound arm/core, then
rollback the entire copy. Replay only those local calls through the same
scoped template binder. Ancestor-owned occurrences, scalar siblings, and
their nested queries are discarded. This prevents a mixed-owner definition
from duplicating outer work merely because the arm also has a local aggregate
or grouping program.

If a selected local occurrence contains a nested aggregate-context dependency
owned by another query, return a typed unsupported boundary instead of
replaying its syntax. Correct replay would require an occurrence-substitution
graph that freshens the local call while redirecting nested nonlocal calls to
their existing semantic occurrences. This slice does not add that second
aggregate graph. Ordinary mixed-owner definitions whose aggregate calls are
siblings remain supported and owner-filtered.

Apply the same boundary when the selected local occurrence depends on an
alias defined only inside any provisional copied query that the checkpoint
will remove. Mandatory alias provenance detects that vanished namespace
before rollback. Aliases defined in the surviving arm/definition scope remain
available to canonical replay.

Simple direct ORDER alias/ordinal copies are not filtered this way: they
retain every owner occurrence produced by the selected complete definition,
matching SQLite's independent simple-order invocation behavior.

### 4. Read finalized aggregate values from the owner compiler frame

Lowering resolves an aggregate call through
`BoundAggregateInvocation::owner`.

For a locally owned invocation, keep the existing aggregate-result register
copy. For an outer-owned invocation, walk the existing `inline_parent_` chain
to the matching query frame and copy that frame's finalized result register.
Failure to find the declared lexical owner or a finalized register is an
internal compiler error.

Clause legality guarantees that an outer-owned aggregate value is read only
after the owner has finalized the relevant global or grouped row. Grouped
queries therefore expose the current group's finalized aggregate register.
No aggregate state crosses query frames, and no child steps, resets, or
closes the owner's capability.

The same shared bytecode builder preserves reached-site child evaluation.
Child local sources may still control whether a scalar row is produced, but
they do not multiply outer aggregate steps.

### 5. Rebind surviving discarded-order work after rollback

EXISTS ordering keeps its existing arena checkpoint and removes every
child-local expression, query, descriptor, and aggregate.

If aggregate ownership resolves to a strict ancestor of the outermost query
whose ordering checkpoint is active, do not stage the provisional occurrence
in the journal. Record only:

- the immutable aggregate-call syntax ID;
- the original definition scope when that scope survives rollback;
- the selected surviving owner identity;
- the owner phase; and
- the consumer query that must retain the aggregate-context dependency.

After the checkpoint rollback, replay that aggregate call through ADR-0092's
canonical scoped template binder with value demand enabled. Use the selected
surviving definition scope for name resolution while retaining the separately
selected owner identity, phase, and current checkpoint-query consumer. This
keeps aliases defined by the rollback query or a surviving ancestor available,
creates only the surviving owner occurrence and its nested queries, then
records the consumer's aggregate-context capture. Nested discarded queries
need not survive merely to own replayed work. A local aggregate, or an
aggregate owned by any query created inside the active checkpoint, remains
discarded.

The replay is required for scalar-width and executable-collation parity.
It also makes nested queries inside the hoisted invocation children of the
owner instead of retaining a discarded lexical parent.

Replay is conditional on the surviving owner clause:

- result-owned work is retained;
- HAVING-owned work is retained after the ordinary HAVING aggregate-query
  admission;
- ORDER-owned work is retained only when the owner query was already
  aggregate before ordering;
- GROUP BY ownership reports the existing group-clause aggregate error; and
- work local to WHERE, ON, another aggregate argument, or a nonaggregate
  owner ORDER BY is discarded with the enclosing ordering.

Thus a strict ancestor relationship alone never promotes aggregate work.

An aggregate whose discarded syntax depends on any alias defined only inside
a rolled-back query remains an explicit typed unsupported boundary, whether
the alias is local or outer-facing, field free or source bearing, and whether
its definition is reused or template-expanded. Replaying `sum(x)` in the
owner scope cannot reconstruct the vanished `x` namespace or definition.
Detect this through the mandatory alias provenance before rollback. Current
staged corpus records do not use that shape. Ordinary live outer aggregates
and aliases are not subject to this boundary.

Discarded-order replay also rejects an occurrence containing a nested
aggregate-context dependency. Replaying the enclosing syntax would otherwise
create a second nested invocation, while replaying each occurrence separately
would require the same substitution graph. Direct arguments, scalar nested
queries, FILTER, local ordering, and query-containing aliases without nested
aggregate calls remain supported.

### 6. Preserve clause and error ordering

Keep these rules:

- ordinary arguments bind before aggregate misuse;
- scalar/local-order/FILTER modifier errors retain ADR-0060 order;
- a locally owned aggregate in a nonaggregate ORDER BY remains misuse;
- an outer-owned aggregate in that ORDER BY is admitted;
- a source-free singleton query resolves and discards its locally owned
  ORDER-only aggregate without executing it;
- an aggregate owned by a query whose WHERE or ON contains the child remains
  misuse when the child expression returns to that clause;
- an outer GROUP BY child reports the existing group-clause aggregate error;
- a child GROUP BY may use an aggregate owned by its outer query;
- HAVING transfer never moves a term carrying an outer aggregate context;
- aggregate-only ORDER cannot silently establish the wrong child query as
  aggregate; and
- query width, collation, function, term-limit, and expression-depth errors
  remain typed and deterministic.

### 7. Keep the remaining subquery surface separate

This slice admits aggregate ownership for the current scalar and EXISTS
expression-query surface, including local/outer aliases, VALUES/compound
definitions already accepted as scalar expressions, grouping, joined sources,
and current access paths.

It does not add mutation query destinations, IN, FROM-derived sources, CTEs,
views, coroutines, windows, user-defined aggregate registration, or another
aggregate scheduler. Those remain in ADR-0053 order. Node 43 remains active.

## Verification

Begin with focused failures for the five staged native records. Add binder
specifications for:

- nearest-scope ownership across argument, FILTER, and local ordering;
- source-free definition ownership;
- source-free aggregate-result contexts versus source-anchored aggregate
  contexts;
- mixed local/outer ownership;
- pure versus query-containing alias deduplication;
- result/ORDER/HAVING invocation phase order;
- direct and transitive aggregate-context captures;
- owner selection constrained by aggregate-context dependencies and nested
  aggregate misuse;
- nested-query reparenting;
- owner aggregate status without child aggregate status;
- owner-relative versus any-owner aggregate expression metadata;
- simple versus compound mixed-owner direct ORDER copies and per-owner term
  limits;
- parent WHERE/GROUP errors and legal child GROUP/ORDER cases;
- term-limit ownership; and
- discarded-order value activation and the explicit inner-alias and
  nested-cross-owner replay boundaries.

Execution tests must pin:

- direct global and grouped outer owners;
- child local-source scans without duplicate outer stepping;
- representative and joined/NULL-extended source values;
- pure and query-bearing callback counts;
- repeated alias occurrences;
- ignored EXISTS projection/order, LIMIT zero, and skipped branches;
- result reset/rebind and execution-lifetime query caches;
- source, aggregate-state, and temporary-storage failures; and
- every existing streaming, ordered, materialized, and demand-one destination
  reached by an outer aggregate owner.

Promote the five current unsupported subquery records without changing their
SQL, operations, IDs, or native outcomes. Append direct, mixed, grouped,
modifier, nested-query, clause-error, reset/rebind, and lifecycle controls.
Keep the 512-case file cap and every existing profile/resource limit.

Add complete binder and lowering OOM sweeps, strict allocation evidence,
grammar-aware read-SQL seeds, four 20,000-run fuzz domains, the existing
fixed-work instrumentation contract, and all native compatibility/performance
contracts.

Complete Debug, Release, ASan/UBSan, instrumentation, genuine GCC 16 Debug and
Release, full clang-tidy, pinned formatting, graph, engineering discipline,
shared-check reconciliation, and exact-tree GPT-5.6 Sol review. Publish a
draft PR after the accepted slice commit, require all exact-head hosted gates,
squash merge, verify tree identity, and clean the merged branch.

## Implementation evidence

The binder now owns one checkpointed statement-global aggregate occurrence
journal. Each occurrence records syntax, expression, candidate invocation,
owner query/core, original site, and owner phase. Speculative compound
matching, validation replay, and discarded ordering truncate journal entries
with their expression/query arenas. Owner finalization reparents nested
queries, rebases captures relative to the owner, deduplicates through the
existing phase finalizer, and publishes the invocation only once.

Bound aggregate calls and invocations publish their actual owner. Aggregate
contexts append transactionally and normalize at query finalization, retaining
a source-anchored representative when one exists. Expression metadata
separates owner-local `contains_aggregate` from transitive
`references_aggregate`. Local and outer aliases carry defining-query
provenance, and nested subqueries propagate owner-relative aggregate context
without making an ordinary child query aggregate.

Lowering follows the existing inline compiler-parent chain to the invocation
owner and copies its finalized result register. No logical-plan, physical-plan,
bytecode, VM, sorter, relation, or persistent-format mechanism changes.

Focused red/green coverage includes nearest ownership through arguments,
FILTER, and local ordering; source-free versus source-anchored aggregate
contexts; pure/query aliases; repeated and transitive copies; result/ORDER/
HAVING phases; grouped and joined owners; nested-query parent repair; ignored
EXISTS projection/order; LIMIT zero; skipped branches; source-free singleton
ordering; speculative rollback; mixed-owner simple/compound copies; reset/
rebind; surviving rollback-query aliases; clause errors; term limits; and the
two explicit replay boundaries. The complete focused binder/query-copy/
outer-runtime selection passes 80 tests.

The subquery shard promotes all five prior outer-aggregate boundaries and adds
21 comparisons plus two explicit replay boundaries. It now contains 246
comparisons and two unsupported boundaries under the unchanged 512-case cap,
profile, fixtures, and resource limits. Pinned regeneration and differential
execution pass.

Exhaustive binder and lowering OOM sweeps include direct, alias, discarded,
mixed-owner, and speculative-rollback owners. The strict outer-aggregate
lowering fixture uses 109 allocations in eight identical runs. Fast-local
passes 1,390 batched and 89 isolated tests.

Isolated Debug, Release, ASan/UBSan, and genuine GCC 16 Debug/Release each pass
1,518 tests; instrumentation passes 1,535. Fifty-six native compatibility/
performance contracts pass, and 28 shared checks reconcile 168 configured
instances. Four fresh 20,000-run fuzz domains complete with seed 102. Full
clang-tidy, pinned formatting, graph, engineering discipline, whitespace, and
JSON checks pass. The final GPT-5.6 Sol review identified the surviving-alias
replay gap; its pinned-source regression failed before the root correction,
and the frozen exact tree passes the complete closing matrix.

## Consequences

Positive:

- aggregate ownership matches SQLite's nearest referenced name context;
- aliases and direct syntax share one owner/program/finalizer contract;
- child queries read finalized ancestor aggregate values without copied state;
- ignored EXISTS ordering preserves only aggregate work that SQLite executes;
- grouped and global owners reuse existing registers and lifecycle; and
- no planner, bytecode, VM, sorter, relation, or persistent-format mechanism
  is duplicated.

Negative:

- name scopes gain one compiler-only owner identity and phase;
- the binder owns one statement-global transactional occurrence journal;
- query captures gain an aggregate-context alternative;
- nested queries inside outer invocations require parent reassignment;
- discarded ordering needs one narrow post-rollback replay; and
- discarded/compound replay with a vanished alias namespace or nested
  cross-owner aggregate remains explicitly staged.

## Rejected alternatives

### Treat every nested aggregate as child-local

Rejected because direct outer arguments, FILTER, and local ordering assign the
invocation to an outer query in SQLite and change both cardinality and
callback schedule.

### Capture the finalized aggregate result as an ordinary value

Rejected because aggregate results are phase-owned registers, not source
leaves or eager child inputs. Grouped owners require the current completed
group's register at the reached child site.

### Copy aggregate state into each child query

Rejected because it duplicates invocation state, modifiers, finalization,
reset, errors, and representative behavior and produces the wrong callback
counts.

### Keep the child query marked aggregate

Rejected because outer-owned calls do not create a child aggregate program.
Child local FROM rows and clauses retain their ordinary semantics.

### Preserve the complete discarded ORDER BY arena

Rejected because it would compile and allocate child-local queries and
resources that SQLite discards, reintroducing deferred width/collation errors
and invalid execution caches.

### Add a runtime aggregate closure

Rejected because the existing lexical compiler frames and statement-global
register identities already provide the required owner path. A runtime
closure would add a second execution mechanism and lifetime contract.

## Traceability

Node: `implement-advanced-sql`, prerequisite closure ordinal 7.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0057,
ADR-0060, ADR-0061, ADR-0063, ADR-0068, ADR-0075, ADR-0082, ADR-0083,
ADR-0084, ADR-0090, ADR-0091, ADR-0092, ADR-0102, and ADR-0106.

Relevant implementation:

- `src/binder/bound_select.cpp` — name scopes, aggregate ownership,
  occurrences, captures, and discarded-order replay;
- `include/modern_sqlite/binder/bound_select.hpp` — invocation ownership and
  aggregate-context capture contracts; and
- `src/lowering/plan_lowering.cpp` — lexical owner-frame aggregate result
  access.
