# ADR-0090: Source-Correlated Scalar and EXISTS Queries

- Status: Accepted
- Date: 2026-10-15

## Context

PR #200 completes automatic covering equality access without completing
`implement-advanced-sql`. The next ADR-0053 subquery consumer can reuse the
accepted nested-query owners, value-register interface, private read regions,
and existing query producers.

The remaining direct-source boundary needs lexical resolution and lazy access
to the current source row, not a missing VM executor. The binder already saves
lexical query scopes, and source occurrence and column IDs are statement
global. ADR-0083 forbids a private callee from reading or borrowing a caller's
cursor.

Eagerly copying outer columns into a private call is not sufficient. Even a
source-field read may allocate, fetch overflow pages, or report corruption or
a size limit. A skipped coalesce/conditional/FULL-merge branch must not read
its field. This package therefore emits correlated producers in their
enclosing lexical bytecode region, preserving on-demand access without
weakening private-call ownership.

Fourteen additional native lazy-source specifications distinguish skipped and
taken oversized fields, including an actual FULL USING merged value after a
runtime length-limit change. Separate aggregate probes establish an important
phase distinction: native representative and sorted-group snapshots retain
outer binding references even from ignored EXISTS projection/order or a
zero-limit child. A global aggregate argument evaluated directly on current
input rows retains its ordinary expression laziness. These audit-only
connection overrides do not modify the frozen compatibility profile.

An independent audit against the pinned SQLite 3.54.0 library checks 70 source
scope, aggregate, set, and callback-frequency cases. It establishes that:

- current columns and current clause-visible aliases precede outer lookup;
  each outer frame is searched before a more distant frame;
- outer lookup uses the same qualified, rowid, ambiguity, USING/NATURAL, and
  FULL merged-value rules as local lookup;
- a qualifier with no matching column can resolve to the same qualifier in an
  outer frame, but not to an unrelated qualifier;
- scalar SELECT and VALUES expressions can capture outer source values;
  LIMIT/OFFSET provide a source-visibility barrier even through another
  nested SELECT;
- direct outer references are admitted in child grouping, HAVING, and
  ordering, subject to the ordinary clause rules;
- correlation is a binding dependency even when EXISTS discards the
  referencing projection or ORDER BY;
- an uncorrelated callback query runs once for three outer rows; a correlated
  query, including one correlated only through discarded EXISTS expressions,
  runs three times;
- parent aggregate arguments use current input rows, whereas a scalar query
  in a completed aggregate result uses its retained representative row;
- aggregate arguments, FILTER, and local ordering all participate in deciding
  whether an invocation belongs to this query or an outer query; and
- parent result aliases are invisible while their SELECT result list is being
  bound, but may be visible to subqueries in later clauses. Their expansion
  also needs occurrence-specific subquery caching: native callback probes
  distinguish the alias copy from the original result expression.

The last rule refines the initial projection-only observation in ADR-0084.
Cross-query result-alias expansion and outer-owned aggregate execution are
separate follow-ups, not assumptions to hide inside direct-source lookup.

## Decision

### 1. Admit source correlation through one lexical resolver

Refactor the existing source-name resolver to consume borrowed source and
joined-table metadata from either the current frame or a saved outer frame.
Do not add another namespace, source-column store, rowid resolver, or
merged-value algorithm.

Keep local column-before-alias precedence. Search outer frames in lexical
order only after the current scope has no match. The first ambiguous frame
reports the existing typed ambiguity error rather than searching farther.
Qualification, explicit rowid spellings, declared rowid shadowing, and merged
column expressions use the canonical source contracts.

A source-invisible LIMIT/OFFSET frame stops outward lookup; it is not merely
skipped. A VALUES query has no local sources but can use its permitted outer
source chain. Parent result-alias visibility is recorded from the existing
clause flags. Encountering such an otherwise visible alias is an explicit
unsupported-feature boundary in this package, not permission to capture its
value eagerly or to skip to a more distant matching column.

Publish a typed lexical parent on each nested bound query. Root remains
inline, parentless, and capture free.

### 2. Separate binding dependencies from executable value dependencies

Each bound query owns one deduplicated capture list. A capture contains:

- the existing normalized `BoundSourceLeafIdentity`; and
- an optional statement-global leaf-expression ID identifying a value
  dependency of its executable consumers.

The identity is the binding dependency. The optional expression is present
only when the query's effective consumers need that value. This is one
capture contract, not parallel source or value owners.

Record direct outer leaves during resolution. When a nested expression is
bound, propagate its free leaves to the calling query, excluding sources
owned by that query. This builds the transitive closure bottom up without a
second query graph or changes to ancestor scratch during speculative binding.

Binding dependencies survive discarded EXISTS ordering, while the canonical
arena checkpoint still removes discarded expressions, nested queries,
functions, and aggregate work. Retain only stable source identities from that
discarded work; never retain an expression ID into a rolled-back arena.
Ordinary speculative rollback restores the current query's capture prefix;
discarded EXISTS ordering is the explicit dependency-preserving exception.

After a query is bound, derive its executable leaves from its actual semantic
consumers. Include ON/WHERE, grouping, HAVING, live aggregate arguments/FILTER/
ordering, needed result values, ordering, and LIMIT/OFFSET. EXISTS does not
consume ordinary projections, but set comparison and aggregate work still
consume their required values. Reuse the existing demand-one UNION proof;
do not invent a second comparison-elision rule.

Use one canonical source-leaf traversal for capture values, dependency
placement, required index fields, and aggregate retained-leaf collection.
Binding-dependency traversal includes inactive captures. Value traversal
includes only executable leaves. Both stop at an aggregate invocation's
separately owned work, whose arguments/FILTER/order have their own consumers.
Reuse the existing leaf normalization and expression topology rather than
maintaining another feature-local walker.

Aggregate row retention uses binding dependencies, not only executable child
leaves. Include the current core's leaves referenced through an ignored EXISTS
value or discarded ORDER, matching the native aggregate-analysis phase.
Exclude incoming ancestor values from that core's own row snapshots. If a
retained identity has no surviving expression ID, publish its canonical
column/rowid leaf through the existing binder after traversal completes;
never grow an arena while a visitor still borrows its span. The aggregate
program remains the sole published owner of its source/representative lists.
Lowering reuses those lists rather than reconstructing an incomplete nested
expression walk.

Row-processing global aggregates also retain query-level ORDER BY binding
leaves before scalar ordering is elided. This includes a direct field, a
skipped coalesce operand, and captures in ignored EXISTS values/order or
zero-limit children.
Pinned native bytecode and malformed-table probes distinguish this earlier
snapshot from evaluating the discarded ORDER BY expression. Standalone,
joined, and compound-core global emission therefore borrow the complete
canonical representative list; they do not reconstruct a result/HAVING-only
subset. Scalar ORDER BY callbacks still do not run.

Preserve the native simple-count exception: one ordinary source, including the
synthetic schema source, no WHERE,
GROUP BY, or HAVING, one direct zero-argument builtin count result, and no
second aggregate, DISTINCT invocation, FILTER, or local ordering. SQLite's
`isSimpleCount()` / `tag-select-0821` path does not capture representatives.
The binder omits ORDER-only representatives for that proven shape, while
execution keeps the existing row-count producer; this adds no count opcode,
alternate executor, or performance claim. All currently constructible
aggregate definitions are builtin registry entries, so the canonical interned
count identity and invocation shape identify this exception.

Borrowing the canonical representative list removes two duplicate compiler
scratch allocations. For the existing `SELECT Payload,min(Name) FROM Items`
lowering fixture, the measured strict allocation count falls from 59 to 57.
Tighten that one exact expectation to 57; do not pad allocations, weaken the
equality check, or increase a ceiling to preserve the old signature. Its SQL,
eight-repeat determinism check, and all other frozen allocation expectations
remain unchanged. This is required canonical ownership cleanup, not a timing
or SQLite speedup claim.

Global aggregate input demand instead uses value-intent traversal of the
invocation arguments, FILTER, and live local ordering, not the binding-only
input snapshot list required by grouped aggregation. An ignored EXISTS capture
in a direct global argument must not force a table read. Grouped aggregates
retain both complete canonical snapshot lists. Required stored-index fields
and automatic-index payloads follow the same execution-phase distinction.
An incoming ancestor value referenced only by an elided global ORDER BY
remains a binding-only capture. Its own query does not snapshot that ancestor;
an enclosing aggregate still retains its own binding leaves normally.

### 3. Emit correlated producers in the enclosing lexical region

Reuse the existing per-query `PlanLowerer`, physical producers, expression
dispatcher, scalar/EXISTS result register, and completion-label interception.
A correlated expression emits that same query body at its reached expression
site. Its normal completion continues the enclosing bytecode; it does not
create a private subroutine or a new runtime call boundary.

A borrowed compiler-frame link identifies the enclosing query's existing
source/retained-row emitters. Resolving an external source leaf delegates only
that leaf's emission to its owning active frame. Do not copy a source registry,
materialize a row at entry, evaluate an alias, or create a runtime closure.
The compiler frames and their borrowed links expire before publication;
the immutable program owns all resulting instructions and descriptors.

An outer field read consequently appears inside the original coalesce,
conditional, merged-value, filter, or row-admission branch. A skipped branch
does not trigger its leaf read; projection fields remain unread when child
emptiness, guard rejection, or a zero limit bypasses projection. Other
semantic phases retain their original evaluation positions.
Read grouped or representative values through the existing ancestor snapshot
mapping, not through a closed or advanced source cursor. Existing `SqlValue`
ownership, cloning, affinity, default, collation, and logical NULL-row rules
remain authoritative. The earlier aggregate snapshots described above are
not moved into a lazy output expression.

The bottom-up free-source closure proves lexical ownership: a query that
needs a source outside its nearest private ancestor makes every intervening
query correlated. All those bodies are emitted in the same nearest
uncorrelated ancestor's region, or main. A private source-free ancestor may
contain an inline query using that ancestor's own current rows. No mutable
descriptor crosses a region and no private callee borrows a caller cursor.
The unchanged verifier remains the final authority; an ownership mismatch is
a compiler error, never permission to relax its analysis.

Prepare each query's statement-global register layout once before creating
the builder. Register its descriptors once, and reuse those resources only
for sequential, nonrecursive occurrences in the same lexical region. Keep
the emission strategy stable and explicitly reset per-emission LIMIT,
ordering, output, completion, and aggregate-expression context. A second
emission must not inherit a prior site's initialized flag, label, or active
core. Reuse the existing frame owner rather than adding another register
allocator or a copied query representation.

Share nested-body initialization, canonical producer emission, and result
completion between inline emission and the existing private routine path.
Source-free queries retain that path and the verified static value ABI.
This scoped extension replaces ADR-0084's proposed eager capture transfer;
it changes neither ADR-0082's call interface nor ADR-0083's capability rule.
No new VM instruction, lazy SQL-value type, coroutine, executor, or producer
algorithm is introduced.

### 4. Keep cache and producer ownership query local

A query with any free binding dependency is correlated even if it needs no
executable value, as in an ignored EXISTS value. Emit its inline body whenever
its expression is reached, without consulting the uncorrelated ready cache.
Source-free queries keep the existing lazy cache for an execution lifetime.
Reset/rebind clears that cache through the unchanged statement lifecycle.

Existing query-local guards and rowid/stored-index access may read their
permitted ancestor values where reached. A subquery depending on a producer's
own current row is not an invariant probe key for that producer. Join predicate readiness
accounts for nested-query binding dependencies; a nested producer's external
sources are already positioned and must not be mistaken for missing local
sources.

Parent source projection must retain fields needed only by an executable
child capture. Aggregate input and representative snapshots include their
query-local binding leaves referenced through child captures, including
inactive child values, while excluding incoming values that do not belong to
that aggregate's source rows.

Use the existing producer, temporary storage, result destinations, lexical
read regions, shared symbols, and statement-wide resource identities.
Every normal nested completion closes that query's resources and preserves
its ancestor's positioned cursors. Error, reset, interruption,
OOM, and temporary I/O failures retain the canonical whole-execution cleanup.
No verifier capability relaxation or VM opcode is required.

### 5. Preserve explicit remaining boundaries

Determine aggregate ownership from all relevant bound source dependencies.
An aggregate with local input references, including local FILTER/order
references, stays local. An invocation requiring an outer owner is rejected
explicitly until that owner/schedule is implemented. Existing aggregate
misuse errors and their clause phase remain authoritative.

This package does not admit outer result-alias expansion, outer-owned
aggregate execution, mutation query destinations, IN, derived FROM sources,
views, CTEs, or coroutines. Native-valid staged cases retain explicit typed
unsupported boundaries. Invalid names and illegal aggregate clauses retain
their normal errors rather than success-shaped substitutes.

Keep the shared SQL expression-height and source limits, including the
43/44 source-free nested SELECT and 998/999 normalized JOIN boundaries.
The pinned direct-source nesting probe reaches 42 levels and rejects 43
because its enclosing source query also contributes to cumulative height.
The separate verified 64-call capacity does not broaden SQL limits.

### 6. Extend native evidence without relaxing bounded contracts

The original version-one read corpus has reached its unchanged 512-case
limit. Promote only its owned correlated-source boundary to comparison,
retaining that record's SQL, operations, and historical ID. Preserve the
other 511 corpus/oracle records as JSON data.

Add a separately versioned subquery corpus/oracle pair using the existing
explicit-path harness and schema, with the same per-file limits and pinned
profile, source hashes, fixtures, and bounded child process. Register it
through the existing differential and oracle-regeneration drivers.

Generalize the existing differential driver's temporary regression fixture
to preserve the supplied corpus/oracle's repository-relative paths. Choose
unsupported and ordinary prepare-error controls by their validated semantic
classification rather than hardcoded primary-corpus IDs. Each shard contains
both controls; missing controls fail explicitly. Keep the existing mismatch
classification assertions and run them for both shards. Do not raise a limit,
truncate old evidence, duplicate a runner, silently skip a shard test, or
change frozen performance inputs.

## Verification

Begin with one failing public-session source-correlation specification.
Then cover direct and transitive values, local/outer shadowing and ambiguity,
qualification and rowid, merged and NULL-extended joins, VALUES, LIMIT scope
barriers, parameters, nested metadata, lazy/zero-limit paths, ignored EXISTS
values/order, sets, groups, parent aggregate arguments and representatives,
and repeated calls across suspension/reset/rebind.

Pin callback frequency independently, including discarded EXISTS references.
Inspect generated lexical resource ownership, inline continuation placement,
and unchanged source-free private-read descriptors, not only final rows.
Exercise multiple emitted occurrences of each nested producer family,
including local alias reuse, runtime top-N/sorter branches, compounds, and
grouped/representative phases. Assert stored/covering/automatic source
selection where it matters so a passing fallback does not stand in for the
intended path.

Add malformed and staged-boundary cases, complete new allocation-failure
sweeps, owned text/blob lifetimes, one-page-cache behavior, source and spill
fault recovery, independent models, and grammar-aware sanitized fuzzing.
Skipped and taken coalesce, conditional, and FULL merged-value paths must
distinguish source-read faults, corruption, value-size limits, and OOM;
zero-limit and empty-source cases must not materialize unused outer fields.
Separately pin the native earlier aggregate-retention errors for those same
references, including ignored EXISTS projection/order, against direct global
aggregate argument laziness.
Record the correctness-complete fixed-work baseline before any later
correlation optimization. Do not increase allocation ceilings or change frozen
performance contracts. Tighten only the strict representative-lowering
expectation described in the decision above.

Review the design with GPT-5.6 Sol before production changes and review the
exact implementation/evidence tree before committing. Complete the existing
Debug, Release, ASan/UBSan, genuine GCC, formatting, unfiltered production
clang-tidy, engineering, graph, native, and matched-contract gates. Merge only
after all nine exact-head hosted checks succeed. This remains one ordered
slice of node 43, not completion of that node or permission to start node 44.

## Implementation evidence

The public-session specification first failed with the existing typed
correlation rejection before production changes. Direct and transitive
captures now execute through the canonical query frames and producers.
Scope, dependency placement, stored/noncovering/automatic field coverage,
lexical region ownership, all producer families at repeated alias sites,
binding-only callback frequency, aggregate input and representative phases,
source-free child caching, reset/rebind, and 42/43 source nesting have focused
module-owned coverage.

The primary corpus contains 507 comparisons and five staged boundaries.
Only `unsupported-correlated-scalar-query` is promoted; the other 511
corpus and oracle records remain identical as JSON data. The bounded
subquery shard adds 101 comparisons and four explicit outer-alias or
outer-aggregate boundaries, including outer-only aggregates in discarded
ordering. Both pairs use the unchanged pinned profile and generator and
pass native regeneration and differential classification regressions.
CASE syntax is not admitted by this slice; conditional evidence uses the
existing lazy `iif` function.

Twenty-five native-derived size-limit cases distinguish lazy field reads
from earlier aggregate snapshots. Actual overflow I/O and malformed-chain
tests cover skipped/taken coalesce, conditional, and FULL-join coalesce
paths. Real FULL USING merged captures additionally have guarded-read
bytecode and merged/qualified/NULL-row execution evidence; the dynamic
connection-length-limit FULL USING probes remain native audit evidence,
not a claimed Modern connection-limit API.
An additional 25-case native table-read and length-limit audit, plus ten
count-shape opcode probes, pin the global ORDER retention, simple-count
exception, and inactive global-input distinction. Five module specifications
were observed failing before that phase correction and passing afterward,
including aggregate-term validation before representative omission. Removing
the duplicate lowering scratch vectors measures 57 rather than 59 allocations
in eight independent runs of the unchanged representative fixture; its exact
assertion is tightened, not replaced with an upper-bound check.
Owned 4,096-byte captured blobs survive one-page-cache traversal and pager
destruction. Child read and aggregate PMA write failures recover through the
existing VM lifecycle. Exhaustive binder, optimizer, lowering, prepare, and
four correlated temporary-consumer OOM sweeps retain their original limits.
Public-session allocation faults distinguish skipped/taken overflow fields
through coalesce and `iif`, verify lock release and the prior-error reset
status, and successfully retry with owned blob results.

The independent vector model exercises 64 deterministic inputs with each
temporary backend, covering correlated ranges, grouped representatives,
transitive aggregate arguments, NULL-extended FULL sources, and rebinding.
Grammar-aware fuzzing retains the four source-free families and adds six
correlated families, scalar/EXISTS destinations, transitive frames, ignored
ordering, and repeated local alias emission.

The instrumentation-only fixed-work specification checks every result row,
value, and terminal state before recording counters. Fresh sessions run the
same 32-row fixture with outer prefixes of one, eight, and 32 rows. The
recorded VM-instruction baseline is:

| Consumer family | 1 outer row | 8 outer rows | 32 outer rows |
|---|---:|---:|---:|
| Transitive scalar and binding-only EXISTS | 205 | 436 | 1,228 |
| Rowid scalar and EXISTS probes | 204 | 442 | 1,258 |
| Sorted grouped scalar and range EXISTS | 417 | 2,751 | 18,186 |

Each run records three source-page reads; registered copy counters are also
published by the test. Reproduce with the instrumentation preset and
`ReadSession.RecordsSourceCorrelatedScalarAndExistsFixedWorkBaseline`.
This is a correctness-complete Modern work baseline, not a throughput or
SQLite speedup claim. Frozen workloads, timing thresholds, profiles, and
harness limits are unchanged. No allocation ceiling is increased; the single
strict representative-lowering expectation is tightened as specified above.

Final-tree gates pass all 1,433 Debug, Release, and ASan/UBSan cases per
configuration, 1,444 instrumentation cases, and 1,430 genuine GCC 16 cases
in each of Debug and Release. One canonical label-selected run passes all
68 existing performance/compatibility contracts. A fresh grammar-aware
ASan/UBSan campaign completes 20,000 inputs with seed 90. The fixed-work
replay preserves the counters above. Unfiltered clang-tidy for both production
libraries, pinned formatting, engineering discipline, native provenance,
and graph validation pass.

## Traceability

Node: `implement-advanced-sql`, ADR-0053 slice 8.

Contracts: ADR-0033, ADR-0034, ADR-0035, ADR-0036, ADR-0053, ADR-0056,
ADR-0057, ADR-0061, ADR-0072, ADR-0075, ADR-0078, ADR-0081, ADR-0082,
ADR-0083, ADR-0084, and ADR-0089.
