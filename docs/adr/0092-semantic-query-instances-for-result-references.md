# ADR-0092: Semantic Query Instances for Result References

- Status: Accepted
- Date: 2026-10-16

## Context

PR #202 completes ADR-0091's pure outer expression aliases, not node 43.
Sharing an immutable expression containing a nested query would also share
that query's lexical parent, private resources, and execution-lifetime cache.
That is not SQLite's result-reference semantics, including for local aliases
that the existing binder already accepts.

The canonical mechanisms available for this slice are:

- one source-preserving immutable syntax arena and statement binder;
- one result-definition syntax vector, alias list, and per-core source
  metadata;
- the existing query-binding cache, flat query/core/expression arenas, and
  speculative binding checkpoint;
- clause-specific aggregate permissions and one aggregate invocation
  finalizer, equivalence test, and retained-leaf analysis;
- one typed source/context capture contract; and
- the unchanged physical producers, inline lexical frames, verified private
  read calls, result destinations, and execution lifecycle.

An independent native audit uses the pinned SQLite 3.54.0 adapter and a
non-deterministic counter function over three source rows. It distinguishes:

| Consumer | Counter calls |
|---|---:|
| `(SELECT tick()) AS x`, local WHERE alias | 2 |
| Same definition, outer WHERE alias | 2 |
| Two WHERE copies plus original projection | 3 |
| Direct result ORDER BY alias or ordinal | 1 |
| ORDER BY expression or scalar child using the alias | 2 |
| GROUP BY alias | 2 |
| Single GROUP BY ordinal, including explicit COLLATE | 1 |
| Repeated GROUP BY ordinal `1,1` | 2 |
| Local HAVING copy of `sum((SELECT tick()))` | 2 |
| Direct ORDER BY alias or ordinal for that aggregate | 2 |
| Compound ORDER BY of that aggregate's result column | 2 |
| Discarded EXISTS ordering of that aggregate | 1 |
| `sum(a)+(SELECT tick())`, direct result ORDER BY | 1 |

The aggregate ORDER BY counts are not a request to evaluate an elided scalar
sort key. Native aggregate analysis registers separate aggregate work when a
copied invocation contains a query. Ordinary equivalent aggregate inputs still
deduplicate. A scalar sibling outside the aggregate remains unevaluated when
the ordering key is elided.

The audit also establishes original source identities under child shadowing,
double-quoted-string and boolean resolution in the definition's original
scope, lazy copies, private ancestors, joined and retained values, compounds,
VALUES, scalar-width activation, and collation phases. A definition with 43
nested SELECTs can be copied into a separately valid 43-level consumer; the
corresponding 44-level definition or consumer is rejected.

## Decision

### 1. Instantiate validated definitions through the canonical binder

Admit local and outer references to result definitions containing scalar or
EXISTS queries. Each semantic expression copy obtains fresh query owners,
source occurrences, captures, private resources, and caches through the
existing binder. Keep the bound expression arena, query arena, and source
identity domain statement global.

Retain one canonical result-definition metadata vector. Extend its existing
syntax entry with the query-containing fact currently held in `AliasEntry`
and whether the original definition was fully value checked. Alias entries
continue to select that metadata by their existing result index; do not add
a parallel expression-property traversal, template arena, or definition map.
Wildcard definitions retain their existing absence of expression syntax.

Pure expressions continue to reuse their immutable targets. Query-containing
copies bind their existing immutable syntax; there is no reparsing, SQL text
rewrite, AST duplication, bound-IR clone, copied ownership graph, runtime
closure, or alternate evaluator.

Direct result-column reuse remains distinct from expression copying.
Ordinary ORDER BY aliases and ordinals retain the original value/cache
identity. A named GROUP BY alias is an expression copy. Each query-containing
GROUP BY ordinal also instantiates its own key expression, while preserving
the existing `result_column` metadata. The grouped output reuses the first
matching ordinal key through the canonical grouped-key register map; it does
not execute the original projection's query. Consequently, `GROUP BY 1`
calls the counter once, whereas `GROUP BY 1,1` calls it twice, and both
project the first key's value. A single counter call does not imply shared
query identity. Keep existing result-column matching and explicit COLLATE
wrappers; do not change the lowering reuse rule or add a second key cache.

### 2. Separate borrowed name scope from the actual lexical parent

Make the existing saved query namespace available as one borrowed name-scope
view. Current and outer lookup use this same view and the existing source
resolver, alias list, precedence, qualification, ambiguity, rowid, joined
values, and source-visibility barriers.

The view borrows the saved frame's source states, table sources, aliases,
result definitions, and outer link. It owns no namespace or source data.
Saved compiler scratch remains the sole owner. Moving its vectors into a
saved query frame preserves the borrowed element storage; no view survives
restoration, binding failure, or statement publication.

An instantiation selects the definition's original name scope, with its own
result aliases invisible as they were during result binding. Its inherited
outer frames retain their original clause visibility. Do not expose the
consuming child's shadowing sources or newly visible sibling aliases.

The active query remains the actual consuming semantic owner. A nested
query's `parent` is that owner, not the selected name scope. At entry into a
new query, its own sources and clauses use the ordinary binder; only its
outer name chain starts at the original definition scope.

Source references resolved through the borrowed definition scope record
captures against the actual active owner. The normal bottom-up closure and
outer-alias context identity propagate them through intervening queries.
Keep exact lexical-parent verification and private cursor ownership.

### 3. Scope the single syntax-to-query cache by semantic instantiation

An already bound syntax query may be reused only within its binding instance.
An expression copy must not consume the definition's cached query identity.

Use scoped suffixes of the existing query-binding vector for instantiations.
Each suffix has the same syntax-query indexing, starts empty, and is retired
when its binding scope returns. Preserve the prior prefix and restore its
active base on both success and typed failure. Check size arithmetic before
growing the vector.

This is one canonical binding cache with nested lifetimes, not a second query
registry or name namespace. Ordinary statements retain the existing prefix
and allocation path. Sequential copies may reuse scratch capacity but never
share their published query identities or execution caches.

Never retain a cache-element reference across binding that can grow this
vector. Reacquire by index. Likewise, reacquire core/query metadata after
nested arena growth instead of retaining references to relocated owner
objects.

### 4. Reuse a validated template's height admission

The definition's syntax and nested query structure have already passed
ordinary resolution-height and structural validation. Instantiation does not
charge those same syntax roots again against the consuming expression's
cumulative resolution height.

Keep the original definition and consumer admission checks, parser bounds,
per-query source limits, typed ID overflow checks, and verified call limit.
Only canonical binding of an already validated result template bypasses the
duplicate resolution-height charge. Do not reset a surrounding height budget
or exempt newly parsed syntax.

Value requirements are not part of that exemption: fresh binding must still
perform the scalar-width, collation, function, aggregate-legality, and other
checks required by its actual consumer.

### 5. Register copied aggregate work through the existing finalizer

An alias copy with an aggregate owned by its defining query remains illegal
in WHERE, ON, and GROUP BY. Legal outer-owned aggregate references remain
explicitly unsupported until the subsequent ownership/scheduling slice.
Local HAVING and ORDER BY copies retain the current owning query and clause.

Fresh query identities distinguish copied aggregate arguments, FILTER, and
local ordering. Feed their invocations into the existing phase-ordered
finalizer and semantic equivalence test. Do not create another aggregate
registry, state container, or invocation deduplication rule.

A direct ORDER BY result-column reference still reuses the projected value,
but also binds the copied aggregate definition when it contains queries.
Register its aggregate work without evaluating otherwise elided scalar
siblings. Simple and grouped SELECTs do this before their normal finalization.

For compound ordering, first use the existing complete result-column
resolution. Then instantiate the selected aggregate definitions in each
owning SELECT core. Extend its existing aggregate program through the same
invocation-finalization operation. Retain a phase boundary in that canonical
program so late ordering invocations are inserted before HAVING, not appended
after it. Preserve published invocation identities, normal equivalence
deduplication, and the relative order of existing invocations. An equivalent
HAVING invocation newly required by ordering moves to the ordering partition
rather than creating another state.

For example, with three input rows, the result, ordering copy, and HAVING
invocation in this query consume counter values one, two, and three:

```sql
SELECT sum((SELECT tick())) AS x
FROM outer_t
HAVING sum((SELECT tick())) = 9
UNION ALL SELECT 0
ORDER BY x;
```

The HAVING sum is nine, not six. This narrowly supersedes ADR-0057's compound
ordering noninterference rule for query-containing result definitions.
Output matching and nonaggregate-to-aggregate rejection remain unchanged.

Rerun canonical retained-leaf analysis for affected cores as replacement, not
append. Borrow the actual core's sources, result definitions, and WHERE;
map live compound ordering to that core's own result columns. Compute both
replacement leaf vectors before installing them, so failed analysis never
publishes a partial replacement and previously retained identities are not
duplicated. Normal first analysis uses the same operation. Reacquire arena
owners after binding. Do not copy a core, reconstruct a second aggregate
program, or apply the left core's sources to another core.

Apply this extension only to live compound ordering after the existing
EXISTS-order checkpoint has returned. Discarded ordering cannot mutate a
previously published core program or retain rolled-back query IDs.

### 6. Activate reused values through canonical speculative binding

An initially discarded definition can become a value consumer through an
alias, grouping key, or actual EXISTS set comparison. Deferred nested
scalar widths and executable comparison collations must then be checked at
prepare, even behind an unreachable runtime branch.

An expression copy, including each query-containing ordinal grouping key,
receives these checks during fresh binding. A consumer that reuses an
original expression validates its syntax in a fresh scoped binding instance,
using the same original name view, then rolls that temporary work back
through the existing arena checkpoint. Preserve the original
expression/query identity and runtime producer. Do not substitute replay
validation for a grouping key's required semantic instance.

Use this one template-binding mechanism for activation; do not add another
expression walker. Do not treat template validation as permanent aggregate
work or retain temporary captures, sources, expressions, functions, or
query IDs. An already fully value-checked definition needs no replay.

Gate validation by the actual consumer, whole-query binding-only state, and
the existing shared result-value/set-comparison demand. Ignored EXISTS values
and ordering remain inactive; aggregate inputs and live set comparisons keep
their established demand. Preserve the immediate named-alias scalarity error
before template binding and the distinct direct-result reuse rules.

### 7. Preserve execution, errors, and remaining scope

Use the same source/context capture alternatives, field-demand discovery,
physical query views, inline compiler frames, private call descriptors,
sorters, temporary relations, aggregate snapshots, and statement cleanup.
No VM or verifier relaxation, opcode, coroutine, or executor is introduced.

Each source-free semantic query instance has its own lazy execution-lifetime
cache. A correlated instance is emitted at its reached site through its
actual lexical parent. Reset, rebinding, interruption, failure, and ownership
retain the canonical lifecycle.

Outer aggregate ownership, mutation query destinations, IN, FROM-derived
queries, CTEs, views, and coroutine consumers remain later ADR-0053 work.
This slice does not complete node 43.

## Ordered delivery

Keep this decision reviewable through three ordered pull requests:

1. Unify canonical result-definition metadata and borrowed current/outer
   name-scope views as a behavior-preserving binder refactor. Preserve the
   current query-alias admission boundaries and start from a green unit tier.
2. Instantiate local and outer query-containing expression copies, including
   ordinal grouping keys, with original names, actual parents, scoped query
   bindings, validated height admission, and consumer value activation.
   Existing direct aggregate ORDER BY registration remains the next slice.
3. Add direct and compound aggregate ordering registration through the
   canonical phase partition and replacement retained-leaf analysis.

Each pull request has its own review and validation gates. Neither the
foundation refactor nor expression-copy delivery closes this decision;
integrated query-instance coverage is complete only after the final slice.
Node 43 remains active throughout.

### Foundation integration after prerequisite corrections

The first delivery was reviewed at tree
`be92d19215b114856c80747645dd45d9c00974f4` and published as PR #203,
head `9b6ad86bf035b8a48c6bbb74af2d82021c10cfd2`. Its original local
compiler/sanitizer/native checks passed, but the unchanged correlated model
exceeded the hosted ten-second deadline.

ADR-0096/PR #205 separately removed the computed-rowid scan amplification
and verified the original hosted model in 6.60 seconds on its first head.
ADR-0095/PR #204 then passed that same unchanged model in 6.68 seconds.
The resulting main is
`f1ed8f52355687ecc0493215c34569cb02f0444a`.

Normally merge that main into the existing foundation branch without
rewriting its published history. Preserve the reviewed binder and
characterization implementations, reconcile ADR ordering, and require
combined-tree review/local validation and all new-head hosted checks.
This refresh admits no expression-copy or aggregate-ordering behavior and
does not complete this ADR.

### Semantic expression-copy delivery

The second delivery instantiates query-containing local and outer result
references through the canonical binder. It uses scoped suffixes of the one
query-binding cache, the definition's borrowed name scope, the consuming query
as actual parent, and validation-only replay for deferred result demand.

Each query-containing GROUP BY ordinal owns a fresh semantic instance while
retaining `result_column` for first-key output reuse. Named aliases remain
ordinary expression copies. VALUES definitions retain every row's syntax in
the same canonical result-definition metadata so ignored EXISTS payloads stay
inactive and live set comparisons validate every row.

The pinned subquery shard promotes the three prior query-definition
boundaries and appends 16 query-copy cases. It now contains 213 comparisons
and five explicit unsupported boundaries without changing any inherited SQL,
operations, or oracle outcome outside those promotions.

Direct and compound aggregate ORDER registration, phase partitioning, and
replacement retained-leaf analysis are delivered by the final ordered slice
below.

### Aggregate ordering delivery

The third delivery keeps direct ORDER BY aliases and ordinals mapped to the
original result value while binding the complete query-containing aggregate
definition once more in the ordering phase. Fresh nested-query identities
therefore create independent aggregate work, while equivalent ordinary
aggregate calls continue to deduplicate and discarded scalar siblings remain
unevaluated.

`BoundAggregateProgram` retains the first HAVING-only invocation index.
Canonical finalization inserts new ordering invocations at that boundary and
promotes an equivalent HAVING invocation to the ordering partition instead of
creating another state. Runtime consumers still iterate the one canonical
invocation vector and require no planner, lowering, bytecode, or VM change.

Compound ORDER BY first completes ordinary output-column resolution. For live
ordering, each SELECT arm then rebinds its selected definition with that arm's
original sources active throughout binding, finalization, and analysis.
Retained-leaf analysis maps ordering back to the owning core's result column,
computes both replacement vectors before publishing either, and never applies
the left core's source identities to another arm. The existing EXISTS
checkpoint leaves discarded compound ordering unchanged.

Seven deterministic direct, compound, phase, reset, and ignored-EXISTS cases
extend the pinned subquery shard to 220 comparisons and five explicit
unsupported boundaries. Callback tests separately pin the native independent
invocation counts and result/ORDER/HAVING execution order.

This completes ADR-0092's three ordered deliveries. ADR-0110 subsequently
delivers outer aggregate ownership. Node 43 remains active.

## Verification

Obtain GPT-5.6 Sol design approval before production changes. Start with
observed public-session and callback reds for independent query copies.
Add focused reds for original-name scope, local aggregate copies, direct
and compound aggregate ordering, result/ORDER/HAVING phase partitioning,
retained-leaf replacement, independent repeated ordinal keys with
first-key projection reuse, and deferred width activation.

Module tests must inspect distinct query IDs and actual parents, transitive
source/context captures, private region isolation, result reuse, per-core
aggregate ownership, and retained/index field demand. Pin the native callback
and 43/44 admission distinctions above, including the accepted combined
43-by-43 template/consumer shape. Preserve 42/43 direct-source and 998/999
normalized JOIN controls.

Cover ignored and live scalar widths/collations, source-free and private
owners, original names under shadowing, full/merged/NULL-extended and retained
sources, compound/VALUES definitions, skipped branches, reset/rebinding, and
owned overflow results. Exercise actual source and temporary failures and
complete allocation-failure sweeps without raising existing ceilings.

Extend the canonical native shard by promoting only owned query-alias
boundaries, retaining every other historical corpus/oracle record and all
profile/file limits. Add independent model cases, grammar-aware inputs,
sanitized fuzzing, and a result-checked pre-optimization fixed-work baseline.
Frozen performance inputs and budgets remain unchanged.

Complete Debug, Release, ASan/UBSan, instrumentation, genuine GCC Debug and
Release, pinned formatting, production clang-tidy, engineering discipline,
graph, native provenance, and the existing isolated performance contracts.
Obtain exact-tree GPT-5.6 Sol implementation review. Publish a `full-quality`
PR, require all nine exact-head hosted successes, use ordinary squash merge,
verify tree/parent/trailer, and clean only the confirmed merged branch.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 8.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0072,
ADR-0075, ADR-0078, ADR-0082, ADR-0083, ADR-0084, ADR-0090, and ADR-0091.

Pinned mechanisms: `resolveAlias()`, `lookupName()`,
`sqlite3ResolveOrderGroupBy()`, `resolveCompoundOrderBy()`,
`sqlite3ExprCompare()`, aggregate analysis, and `sqlite3CodeSubselect()`.
