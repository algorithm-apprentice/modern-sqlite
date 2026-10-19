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
| Term prerequisites, likelihood, ON ownership | `whereexpr.c:75-84,1154-1240`; `where.c:832-853` |
| Compatible column/affinity/collation term scanning | `where.c:351-521` |
| Access-term consumption and outer residual readiness | `wherecode.c:419-438,1494-1516,2678-2733,2854-2872` |
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

1. Translate native costs, estimates, and single-source shortcut/selection.
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
