# ADR-0091: Lexical Outer Expression Aliases

- Status: Accepted
- Date: 2026-10-16

## Context

PR #201 completes ADR-0090 source correlation, not `implement-advanced-sql`.
The next subquery prerequisite is the result-alias lookup already identified
by ADR-0084 and ADR-0090.

The binder has one immutable expression arena, one canonical source resolver,
one clause-aware lexical scope chain, and explicit alias references. A result
definition is bound before its alias becomes visible. Its bound source leaves
therefore already identify the definition's original sources, including
USING/NATURAL merged values. Reusing an immutable expression without a nested
query preserves those identities even if a consuming child shadows a source
name.

An independent 83-case audit of the pinned SQLite 3.54.0 adapter establishes:

- local sources precede local aliases, and each permitted outer frame is
  searched before a more distant frame;
- the first duplicate result alias wins;
- parent result aliases are invisible inside their result list, but visible
  from the parent's later clauses;
- qualified names do not resolve result aliases;
- LIMIT/OFFSET stop outward source and alias lookup;
- a constant or source-free function alias can correlate a child without
  supplying any source field;
- an alias reference reevaluates its defining expression at its reached site;
  it is not a captured result register; and
- a definition containing a nested query needs an independent semantic query
  instance and cache for each expression copy.

The last requirement is a separate ownership prerequisite, not permission to
weaken the inline compiler's lexical-parent check. This ADR admits the pure
expression subset first and leaves query-containing definitions explicit.

Native counter probes distinguish correlation from access-path placement:

| Query shape over three outer rows | Native callback calls |
|---|---:|
| Constant alias in scalar WHERE child | 3 |
| Constant alias in scalar ORDER BY child | 3 |
| Constant alias through two scalar children | 3 |
| Constant alias in a scalar predicate over a 3-by-3 CROSS join | 3 |
| Constant alias in a scalar rowid lookup key | 1 |
| Constant alias only in ignored EXISTS projection, child LIMIT 1 | 3 |
| Constant alias only in ignored EXISTS ordering, child LIMIT 1 | 3 |
| Source-free child with no alias dependency, child LIMIT 1 | 1 |
| Volatile alias in scalar WHERE, plus original result evaluation | 6 |
| Volatile alias behind skipped coalesce, plus result evaluation | 3 |
| Query-containing alias copied into local or nested WHERE | 2 |
| Same query-containing definition used as direct result ORDER BY | 1 |

The last two rows are follow-up evidence, not executable admission here.
Scalar queries also preserve the existing rule that child column collation
is not inherited by the containing scalar expression.

The audit includes controls for SQLite's `existsToJoin()` optimization.
An eligible bare WHERE EXISTS with one ordinary inner source can become a
join; ignored projection/order then no longer determines its loop placement.
Adding child LIMIT 1 prevents that rewrite and isolates query correlation.
The same distinction occurs with direct source references, not only aliases.
Do not infer a missing alias dependency from the rewritten query's callback
count, fake a source dependency, or add a feature-local EXISTS executor to
imitate it. Callback specifications for this slice use scalar consumers,
source-free EXISTS, or the explicit LIMIT control.

## Decision

### 1. Admit one bounded lexical expression subset

Resolve an otherwise visible outer result alias when its bound definition
contains neither an aggregate owned by the defining query nor a nested
query. Admit ordinary source values, rowid, merged joined values, constants,
parameters, scalar functions, coalesce/conditional expressions, comparisons,
arithmetic, and COLLATE through the existing bound expression types.

Keep the exact local-source/local-alias/outer-frame precedence. Use the
existing alias list and result syntax; do not add a namespace, source-column
store, alias value map, or second resolver. Preserve first-match duplicate
aliases, qualification, ambiguity, double-quoted-string behavior, and the
existing source-invisible scope barrier.

Track whether binding a result definition introduced nested query owners
using the existing statement query arena. Result definitions bind once before
later-clause alias use; syntax query identities do not alias another result
definition. Store this fact with the existing alias entry, not in a second
expression ownership graph or a global expression-property traversal.
Check the alias target's scalarity before the staged boundary, matching
native `sqlite3ExprVectorSize()`: a direct scalar-query target with multiple
output columns reports the existing typed width error even in an ignored
value context. Otherwise, query-containing definitions retain a typed
unsupported-feature error even if their nested query is source free or
occurs in a skipped expression.

Use one typed aggregate permission in the canonical binding scope:
prohibited, allowed, or GROUP BY's bind-then-reject state. Preserve that
permission in the defining frame alongside its existing visibility flags.
GROUP BY, WHERE, and ON select aggregate misuse for an outer aggregate alias;
HAVING and ORDER BY may reach the explicit outer-ownership boundary. GROUP
BY's ordinary local aggregate binding and subsequent rejection remain
unchanged. Do not copy its temporary aggregate-binding allowance into an
outer-alias permission or silently bind a parent aggregate in the child.

### 2. Reuse immutable definitions, not saved result values

An admitted alias appends the existing alias-reference expression pointing
at its already bound definition. Preserve affinity, explicit collation,
truth metadata, and normal expression dispatch. Annotate an outer alias
reference with its typed defining query identity.

The original target is immutable and contains no query body requiring a
different lexical parent or cache. A shadowing child source consequently
cannot rebind its names. No syntax replay, IR cloning, copied source frame,
new runtime value, or eager alias evaluation is needed.

The target executes at the reached alias site. Existing coalesce,
conditional, FULL merged-value, filter, and row-admission branches remain
lazy. Source leaves delegate to the canonical ancestor emitter, including
retained aggregate rows and logical NULL rows. Do not fetch every possible
source value at entry.

Keep the existing cumulative SQL resolution-height accounting and compiler
limits. Reusing an already bound pure expression does not reset that budget
or rebind it under a different query owner. Query-containing semantic copies
require their own documented limit and ownership audit before admission.
Existing local aliases and direct result-column ORDER BY reuse are unchanged.

### 3. Generalize the one capture contract with a zero-data context identity

Keep exactly one deduplicated capture list on each query. Its entries are
typed alternatives:

- the existing normalized source capture, including its optional executable
  leaf-expression ID; or
- a query-context capture naming the defining query of an outer alias.

A query-context capture is a binding dependency, not a rowid, source column,
SQL value, register, runtime environment, or copied alias. It has no value
transfer. It is required even for a constant alias and for an alias mentioned
only by an ignored EXISTS value or ordering.

Extend the canonical dependency traversal to visit this identity at an
annotated alias reference and through nested-query captures. Existing source
leaf/owner adapters select only real source captures. Reuse the same
expression topology and transitive free-dependency closure; do not add a
parallel context vector, correlation boolean, alias walker, or fake source.

An alias dependency is free when its defining query is not the current query.
Propagate it bottom up through each intervening query. The defining query
does not capture itself, and root remains parentless and capture free.
Deduplicate context identities independently of real source identities.

Source captures retain ADR-0090's binding/value distinction. Source demand,
stored/covering/automatic index coverage, aggregate ownership, aggregate
snapshots, and predicate readiness continue to use actual source leaves.
A field-free context identity must not imply dependence on all joined
sources or make an otherwise available rowid probe key self-dependent.

An ignored EXISTS result definition must not itself demand its source fields.
Share the binder's existing result-value demand decision with required-field
discovery for stored and automatic indexes. This decision retains the same
demand-one set-comparison proof; predicates, live alias uses, aggregate work,
and set comparisons still demand their actual values. Do not compensate for
discarded projections by changing capture identity or disabling index access.

The existing arena checkpoint handles context identities just as stable
source identities: normal rollback restores the capture prefix; discarded
EXISTS ordering preserves binding dependencies without retaining a rolled-back
expression or query ID. A retained context must name the already existing
defining ancestor, not a discarded child query.

### 4. Retain canonical producer, cache, and capability ownership

A nonempty capture list still means correlated execution. The existing inline
query path reevaluates a context-dependent body whenever its expression is
reached, even when there is no source field to transfer.

The existing physical producer determines where an expression is reached.
A field-free context is not a substitute for source dependence: ordinary
predicate placement and source-independent lookup admission remain intact.
Query bodies with no free dependency retain execution-lifetime private-call
caching.

Keep exact lexical-parent checks, transitive region ownership, source
borrowing, private-call verification, completion handling, resource cleanup,
reset/rebind, and interruption behavior. No bytecode instruction, verifier
exception, alternate query producer, new sorter, temporary relation, or VM
executor is introduced.

### 5. Keep the next ownership slice explicit

Do not admit an outer alias whose definition contains a nested query by
sharing its original bound query identity. Native `(SELECT tick()) AS x`
uses two independent source-free caches when copied into WHERE, but one
when reused as the direct result ORDER BY key. A semantic copy also needs
the actual consuming lexical parent while retaining the definition's original
name scope.

Implement that requirement next, consistently for local and outer alias
copies, under a separate reviewed ADR. Preserve the current local alias path
in this slice; do not make a partial cache repair or change aggregate
invocation ownership as a side effect.

That semantic-instance slice also owns activation of nested scalar-width
checks when an initially discarded definition becomes executable. This slice
checks the alias target's immediately known scalarity but never activates a
query-containing definition, including a scalar function containing a wider
child query. Such definitions remain a staged surface, not a claim of full
prepare-error compatibility. Do not add a second value-demand traversal just
to partially validate an unadmitted semantic copy.

Outer aggregate ownership, mutation query destinations, IN, derived FROM
sources, CTEs, views, and coroutines remain subsequent ADR-0053 work. This
bounded admission does not complete result-alias expansion or node 43.

## Verification

Review this design with GPT-5.6 Sol before production changes. Begin
implementation with one public-session test observed failing at the current
outer-alias boundary.

Add module-owned specifications for lexical precedence, duplicate aliases,
original source identity under child shadowing, qualified-name rejection,
result-list invisibility, LIMIT/OFFSET barriers, direct/transitive context
closure, source-free ancestors, real source plus context captures, and root
capture freedom. Cover ordinary, joined, NULL-extended, merged, grouped, and
representative source values without a second retained-row mechanism.

Pin callback counts from the controlled native shapes above. Distinguish a
constant alias from an unrelated source-free child; preserve skipped volatile
alias branches; check join placement and source-independent rowid lookup.
Inspect inline/private region ownership and real field demand rather than
accepting a scan fallback as index evidence.

Keep query-containing and outer-aggregate definitions as explicit boundaries.
Cover immediately known invalid alias-target width, including a target
initially bound in a discarded value context. Distinguish this lookup error
from deeper scalar-width activation on the deferred query-copy surface.
Aggregate misuse must retain its correct phase, including a parent GROUP BY
consumer. Preserve existing 43/44 source-free SELECT, 42/43 source correlation,
and 998/999 normalized JOIN limits.

Add ownership, reset/rebind, one-page-cache, source-failure recovery, complete
allocation-failure sweeps, independent models, grammar-aware fuzz input, and
pinned native comparisons for the admitted subset. Preserve historical
corpus/oracle records except the explicitly promoted boundary; use the
existing bounded subquery shard rather than increasing a file limit.

Record a result-checked instrumentation baseline before optimization. Keep
frozen performance inputs, profiles, limits, exact allocation expectations,
and ceilings unchanged.

Complete the existing Debug, Release, ASan/UBSan, genuine GCC, formatting,
production clang-tidy, engineering, dependency graph, native provenance, fuzz,
and matched-contract gates. Obtain exact-tree GPT-5.6 Sol implementation
review before commit. Publish a `full-quality` PR and merge only after all
nine required exact-head hosted checks succeed. Verify the squash tree and
clean only confirmed merged branches, then continue the next ordered slice.

## Implementation evidence

Five focused specifications were observed failing before the pure-alias
implementation: public source-bearing evaluation, field-free callback
frequency, transitive context closure, defining-clause aggregate errors, and
immediate alias-root scalarity. All pass after implementation. The complete
fast unit tier passes 1,399 cases without changing an existing exact allocation
expectation or failure-sweep ceiling.

Module-owned coverage checks lexical precedence and barriers, immutable source
identity under shadowing, direct and transitive source/context captures,
source-free private ancestors, discarded EXISTS dependencies, predicate and
rowid-key placement, reached-site volatile evaluation, retained aggregate
values, and the unchanged 43/44 alias depth boundary. A direct-source control
reproduced the unnecessary EXISTS projection demand; the shared result-value
contract fixes both forms without weakening the covering-index assertion.
Destination and compound tests retain live set-comparison demand.

Ownership and failure cases include one-page-cache overflow values surviving
pager destruction, skipped and taken source I/O/corruption, recovery after an
actual alias field failure, aggregate PMA write failure, complete prepare and
execution allocation sweeps, and reset/rebinding. An independent vector model
checks 64 bindings on each temporary backend. Four grammar-derived alias
families and a raw SQL seed extend the existing SQL fuzz target.

The canonical native subquery shard now has 190 comparisons and eight staged
boundaries. Promotion of the earlier pure outer-alias boundary and 93 appended
records preserve the other 104 historical corpus/oracle records as JSON data.
The primary 512-record pair, fixtures, semantic profile, generator, per-file
limits, and performance contracts are unchanged. The two pairs together
contain 697 comparisons and 13 staged boundaries. New native cases include
lexical shadowing, joined and NULL-extended values, grouping, private ancestors,
set consumers, ignored versus live comparison collations, lazy overflow
recovery, rebinding, and alias depth. Query-containing definitions and legal
outer aggregate ownership remain native-valid explicit boundaries.

### Fixed-work baseline

The result-checked instrumentation query projects either `1 AS x` or
`o.id+1 AS x` from `model_rows o`, filters `o.id<=?1`, consumes `x` through two
scalar SELECTs in WHERE, and orders by `o.id`. It records execution only after
preparation and binding. An independent rerun reproduces all 18 counters:

| Outer rows | Constant-alias VM instructions | Source-alias VM instructions | Pages read, either shape | Bytes copied, either shape |
|---|---:|---:|---:|---:|
| 1 | 198 | 202 | 3 | 0 |
| 8 | 394 | 426 | 3 | 0 |
| 32 | 1066 | 1194 | 3 | 0 |

This is a deterministic pre-optimization work baseline, not a timing or
SQLite speedup claim. It does not change an archived workload, threshold,
allocation budget, or matched benchmark setting.

### Validation closure

Complete Debug, Release, and ASan/UBSan configurations each pass 1,449 tests.
Instrumentation passes 1,461; genuine GCC 16 Debug and Release each pass
1,446. The new binder capture assertions use bounds- and type-checked variant
access; compiler diagnostics remain errors. Native extraction and clean
regeneration pass for both configured corpus/oracle pairs.

The rebuilt grammar-aware ASan/UBSan SQL target completes a fresh 20,000-input
campaign with seed 91, including the new raw alias seed. All 68 existing
performance/compatibility contracts pass in one serial isolated invocation
after builds and fuzzing finish. Pinned clang-format 23.1.2, production-target
clang-tidy, engineering discipline, whitespace, and the acyclic 48-node graph
checks pass. No archived timing, workload, profile, allocation expectation, or
failure ceiling changes.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 8.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0072,
ADR-0075, ADR-0078, ADR-0082, ADR-0083, ADR-0084, and ADR-0090.

Pinned native mechanisms: `resolveAlias()`, `lookupName()` and its name-context
reference accounting, `resolveSelectStep()`, `sqlite3CodeSubselect()`,
`sqlite3ExprVectorSize()`, and the `existsToJoin()` audit control.
