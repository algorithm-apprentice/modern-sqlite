# ADR-0120: SQLite WhereLoop and Path Solving

- Status: Accepted
- Date: 2026-10-19
- Module graph node: `implement-advanced-sql`
- Audit ledger: ADR-0111, entry 9 (`execution-optimizer`)

## Decision

Translate the pinned SQLite planner for the access capabilities already
implemented by Modern SQLite. Replace byte-width work scores, greedy
source-order access selection, and repeated prefix estimation with SQLite's
LogEst costs, prerequisite-bearing WhereLoops, dominance pruning, and bounded
N-best WherePaths. Do not design another join-order algorithm.

### Native source map

The reference is the local SQLite 3.54.0 snapshot in `project/manifest.json`.

| Responsibility | Pinned source |
|---|---|
| LogEst conversion/addition and logarithmic seek estimate | `util.c:2055-2130`; `where.c:700-702` |
| Schema-width and default prefix estimates | `build.c:2305-2328,4631-4673` |
| Load STAT1 before applying missing-index defaults | `analyze.c:1942-2008` |
| Term prerequisites, likelihood, ON ownership | `whereexpr.c:75-84,1154-1240`; `where.c:832-853` |
| Compatible column/affinity/collation term scanning | `where.c:351-521` |
| Access-term consumption and outer residual readiness | `wherecode.c:419-438,1494-1516,2678-2733,2854-2872` |
| Comparison-specific probe affinity, independently for each bound | `expr.c:344-359`; `wherecode.c:976-1008` |
| RIGHT replay and execution-level null prefix | `wherecode.c:2893-2935` |
| Loop ownership, transfer, pruning, subset adjustment | `whereInt.h:129-174`; `where.c:2522-2612,2652-2935` |
| Range and residual output estimates | `where.c:1911-1922,2225-2247,3032-3129` |
| Prefix enumeration, table/index/automatic costs | `where.c:3223-3619,4007-4305` |
| CROSS/outer join prerequisites | `where.c:4941-5042` |
| Order properties and sort cost | `where.c:5150-5488,5539-5599` |
| Star-query adjustment and path bounds | `where.c:5666-5798,5846-5905` |
| N-best expansion, replacement, final publication | `whereInt.h:213-222`; `where.c:5910-6270` |
| Interstage search protection and single-source shortcut | `where.c:6314-6452,7089-7120` |

Native `WhereInfo` owns one candidate list and one selected level sequence
per query core. `WhereLoopBuilder` borrows the clause/catalog; its template
is unpublished scratch. `whereLoopInsert()` replaces dominated loops before
path solving. Solver scratch is released after selected levels are published.
An allocation failure abandons the unpublished plan, not the catalog or
bound query.

### Translation boundaries

- Keep the existing immutable bound/logical/physical owners, source IDs,
  affinity/collation matcher, canonical source-dependency visitor, typed
  access payloads, and single lowering producer.
- Use integer LogEst arithmetic and the native constants. Row widths enter
  the native ratio as LogEst widths, not raw-byte scan multipliers.
  Preserve separate setup, run, and output estimates; an automatic index is
  a setup-bearing loop, not an independently admitted greedy choice.
  Deliberately replace the pre-C-API `AccessPathCost` linear fields with
  signed `setup_log_est`, `run_log_est`, and `output_log_est` fields. Zero
  setup means no setup; output may represent a fraction of one row. Update
  all consumers and assertions directly: no legacy work-score projection,
  compatibility shim, or selection through approximate linear costs remains.
- Enumerate alternative equality prefixes and their optional range bounds.
  Candidate prerequisites come from key dependencies and native join
  barriers, never from comparing source ID numbers. Keep original predicate
  occurrence and ON-owner identities when commuting a comparison.
  Keep key-side prerequisites (`prereqRight`), whole-term readiness
  (`prereqAll`), and ON ownership separate; outer-ON adds its owner to
  readiness and its left prefix to key prerequisites.
- Preserve native dominance/subset rules, automatic-index preference,
  rowid/unique shortcut, residual selectivity, range floor, and path
  replacement/tie order. Keep the native 20,000-plus-1,000-per-source search
  budget and 1/5/12/18 path widths, including star-query scan adjustment.
  Search-budget exhaustion remains a bounded-search outcome and is visible
  in plan explanation; allocation or invariant failures remain errors.
- Treat captured outer sources as already available. Retain the existing
  query-owner/capture contracts and the native bounded outer-loop estimate;
  never copy expressions or create another subquery executor.
- Publish the selected source sequence once, alongside the existing
  per-core physical plan. This is the acyclic equivalent of `WhereInfo.a[]`.
  The declared join tree retains ON/null-extension ownership, not execution
  order. Lowering reorders its existing borrowed source descriptors once and
  consumes the published sequence without choosing accesses again.
  Each descriptor retains its declared `BoundTableSource` and ON owner.
  Execution depth is used only for nesting and replay-prefix traversal;
  schema, cursor fields, primary-key identity, and join metadata use the
  stable source reference, never `table_sources[depth]`. Validate that the
  selected sequence is an exact permutation of the core's source IDs.
- Assign WHERE/ON readiness using that sequence. Preserve outer-ON floors,
  RIGHT/FULL replay boundaries, original occurrence ordering within a phase,
  and exact consumed-rowid identity. A reordered source may use an equality
  from another INNER ON owner; that occurrence must not execute twice.
  Consumption remains limited to exact rowid ON terms. Stored/automatic
  constraints and WHERE terms retain ADR-0107's residual rechecks. Native
  outer readiness governs whether a residual runs before or after match
  publication and which constraints execute during unmatched replay.
- Reuse existing ordering/grouping/min-max contracts. Cost only order
  properties that the canonical lowering can actually deliver. This does
  not introduce partial/block sorting, ordered DISTINCT, or a second sorter.
  Mandatory materialization remains mandatory.
- Existing access capability limits remain explicit: no IN/OR-union,
  skip-scan, expression/partial-index access, WITHOUT ROWID key search,
  virtual/derived sources, or new SQL syntax. Automatic-index backend
  permission and RIGHT/FULL replay admission remain unchanged.
- Build each core's candidate/solution state once. Internal physical
  publication validation borrows that state rather than rerunning the
  planner. Do not retain scratch in the immutable physical owner.

This supersedes the work-score/source-order policies in ADR-0035, ADR-0052,
ADR-0073/0077, ADR-0085-0089, and ADR-0107 only where specified above.
Their ownership, key-domain, error, preservation, and lowering contracts
otherwise remain in force. New enumerators use unprefixed names.

## Delivery and evidence

Use one ordered two-commit implementation batch under ADR-0106:

1. Translate LogEst primitives, default/STAT1 costs, and single-source
   shortcut/selection.
2. Translate candidate prerequisites/pruning and bounded path solving, then
   consume the selected sequence in canonical lowering.

Each commit has a focused red/green or green-refactor baseline, the complete
fast tier, and GPT-5.6 Sol review before the next begins. Run integrated
change-aware closing gates once on the final production head. An earlier
slice change invalidates its dependent suffix.

Reuse existing optimizer/session fixtures, OOM sweeps, native contracts,
models, and fuzz inputs. Add focused cases for small/selective-first INNER
and comma joins, forward parameterized access, non-greedy alternatives,
CROSS and LEFT/RIGHT/FULL barriers, repeated ON ownership, star/path bounds,
and the native tiny-table/rowid/range cost decisions. Preserve unordered-row
semantics and explicitly compare ordered results, errors, captures,
null-extension, reset, and allocation cleanup.

Before implementation, twelve pinned-native EXPLAIN controls are retained
in session evidence. They include `Tiny` before `Items` for an INNER/comma
join, declared order for CROSS and outer controls, `a,c,b` across the legal
right side of CROSS, and a tiny single-source rowid shortcut. The unchanged
index workload baseline at `ce7217b` is retained in
`build/evidence/where-path-index-before-v1`; its matched guards pass.
Closing evidence compares the same inputs and existing joined fixed-work
controls, without replacing historical baselines or relaxing budgets.

Ledger 9 remains open until both slices and their integrated gates close.
No later repair or SQL feature begins during this batch.

The first slice's tiny-table joined-scan regression failed before production
changes. Source-verified follow-ups also failed for advanced/aggregate
shortcut precedence and missing-index STAT1 defaults. The corrected slice
passes all 78 optimizer cases and the complete 1,447-batched-plus-92-isolated
fast-local tier.

Admitting native unique shortcuts reuses canonical index descriptors, one
shared access-register workspace, traversal primitives, and cursor cleanup
inside the existing streaming source producer. Existing DISTINCT and
compound consumers use that producer; focused coverage includes covering and
noncovering hits, misses, NULL keys, residual rejection, LIMIT, materialized
and ordered compounds, and aggregate siblings. No second source executor is
introduced. Candidate-dependent likelihood, residual adjustments, pruning,
and path selection still belong to the second slice.

GPT-5.6 Sol approved the first implementation slice and its directly coupled
lowering corrections with no significant unresolved issue before commit.

## Native key and term-consumption closure

Status: Accepted amendment for the second slice after GPT-5.6 Sol review.

Native `where.c:3300-3327,4115` admits a supported key expression when its
dependencies exclude the accessed source and its outer-join/affinity rules
permit the constraint. It does not impose Modern's value/arithmetic-only
join-key classes or reject a function merely because it is volatile.
Translate that rule through the existing bound-expression emitter; do not
add path-dependent custom key-shape barriers to the N-best solver.

Together, translate `wherecode.c:419-438` consumption for exact rowid and
stored/automatic constraints, including WHERE constraints. A selected term
is coded only when its complete prerequisites are ready, its access occurs
no later than the term's evaluation phase, and outer-join rules permit it.
Track original conjunct occurrences and ON ownership, not expression IDs
alone. Transitive constraints that cannot discharge the original term keep
that residual. Publish the resulting predicate schedule once; no key-result
cache or second evaluator is introduced.

This supersedes the retained key-shape and blanket
stored/automatic/WHERE recheck limits above and in ADR-0081/0085-0087/0107.
It does not add SQL syntax or unimplemented access kinds. Native EXPLAIN and
focused callback-count controls precede implementation.

The pinned native library executes the three volatile rowid-ON, rowid-WHERE,
and stored-index-ON controls with three key calls each; the unchanged Modern
baseline executes nine. Native coalesce and correlated query-value keys also
use the existing access mechanism. Evidence remains in session artifacts.
Publish joined `source_order` and bounded-search status through the existing
`PhysicalCoreAccessPlan`, not a parallel query-plan container.

## Construction and nested-context closure

Cache the normalized column references and both operand prerequisite masks
once per original term. Visit original terms first, followed by the commuted
virtual terms in reverse original order (`whereexpr.c:1225-1290,1926-1928`).
Automatic admission and finalization use direct clause passes, as
`termCanDriveIndex()` and `constructAutomaticIndex()` do
(`where.c:901-925,1029-1156`), not a column-by-term-by-clause search.
Preserve the shared top-column bit and the native all-wide-columns covering
tail when any column at or above 63 is needed.

Translate automatic partial construction through the existing build cursor,
bound-expression emitter, and false-or-NULL continuation. Eligible original
terms are deterministic, contain no subquery, and reference only the build
table; LEFT/RIGHT ownership restrictions are unchanged
(`expr.c:2543-2623,2716-2832`; `where.c:1034-1046,1203-1220`).
They remain residuals unless separately consumed by an exact access key.
Construction temporarily borrows the existing source-to-cursor binding;
the covering and construction descriptors already share field positions.
No second evaluator, expression copy, or construction executor is added.

Carry signed native `nQueryLoop` between existing query owners. A source
loop's compilation context adds its selected output estimate to the saved
ambient context, and ending that loop restores the ambient context
(`where.c:6926,7207,7505,7902`; `sqliteInt.h:3926`). Keep the solver's cap of
48, not an unsigned or globally clamped ambient estimate. LIMIT/OFFSET,
source-free false-WHERE bypass terms, aggregate HAVING/results, and VALUES
use the saved context. Source-dependent WHERE/ON, ordinary projections and
ordering, grouping keys, and aggregate input compiled inside a source loop
use its augmented context. Sorted-group aggregate inputs are compiled after
that loop and use the saved context
(`where.c:7003-7046`; `select.c:8411-8473,8650-8719,8780-8834,9003-9035`).
Reuse the canonical bound dependency traversal to find directly referenced
query owners. Keep invocation estimates in transient builder state, not a
second retained query-plan owner. Binding-only query owners need no
execution-context estimate.

Access-key evaluation follows `wherecode.c:970-1008,2053-2110,2155-2177`:
equalities run in physical index-prefix order, followed by the resolved
traversal-start bound. Evaluate the resolved end bound only after positioning
succeeds. Extend the existing shared key emitter with explicit start/end
phases and use the same `IndexTraversal` in ordinary, streaming, joined, and
aggregate consumers. Do not sort key callbacks by conjunct occurrence or
introduce a key-result cache. Retain the converted prefix registers when
building the end key. A NULL end key retires its positioned cursor through
the existing `NullRow(false)` transition before joining seek-miss cleanup;
the bytecode verifier's cursor-state rules remain unchanged.

Before propagating a solved non-shortcut input, apply the native 30-point
`WHERE_WANT_DISTINCT` output reduction (`where.c:7124-7130`), including the
single-DISTINCT-aggregate input request. Preserve shortcut bypass and the
existing destination/value-admission contracts. This is an invocation-estimate
adjustment, not ordered DISTINCT or a DISTINCT-to-GROUP-BY implementation.

The second slice's focused native source-order, volatile-key frequency,
mixed-affinity probes, original/commuted key order (including captures),
partial construction, wide-column tail, and nested-loop-context regressions
were observed red and then green. Existing unordered expectations changed
only after executing the same fixture against the pinned library. Additional
controls cover CROSS right-side reordering with differently shaped sources,
star schedules, search-budget exhaustion without lost residuals, and
automatic-construction OOM cleanup.

The completed fast-local selection passes 1,461 batched and 92 isolated
cases. Optimizer simple and stored-index allocation contracts remain unchanged;
aggregate planning falls from 59 to 35 allocations. Native loop templates
transfer the basic selected index payload once, and validation borrows its
stable equality/predicate buffers instead of allocating another copy.
The construction/nested-context design completion was approved by
GPT-5.6 Sol before those production changes.

Implementation review identified eager stored-index end keys and missing
DISTINCT invocation reduction. Both were independently confirmed against the
pinned source and executable controls, then corrected through the accepted
shared mechanisms. Covering/noncovering, streaming, joined, global aggregate,
MIN/MAX, NULL extension, ascending/descending, and seek-miss controls now
cover phased keys. DISTINCT controls include source-free negative estimates,
aggregate inputs, and shortcut bypass. Removing the sorted-key scratch
tightens the four affected lowering allocation counts from
34/40/47/59 to 33/39/46/57; no allocation ceiling is raised.

GPT-5.6 Sol approved the corrected second implementation slice with no
significant unresolved issue. Integrated exact-head qualification and matched
closing evidence remain required before ledger 9 closes.
