# ADR-0163: Native Truth Provenance and Outer-Join Strength Reduction

- Status: Accepted
- Date: 2026-10-27
- Module graph node: `implement-advanced-sql`
- Delivery: four ordered related commits under ADR-0106
- Prerequisite: verified PR #267 squash `ca19502`

## Context and completed investigation

PR #267 closes ADR-0162, not node43. Its actual resulting main, tree
`02541d9732dae6e49087a0f59bca1d56d64ec095`, passes native2116/all2081 units,
five2129-case POSIX/sanitizer selections, both55 native contracts and actual
full-library quality. Original Windows model14.01s/20 and tooling3.67s/10 pass
without retry. Required literal/name/unary materialization, recursive truth
and scalar-width gates still precede IN query RHS.

The next value-generation boundary needs native static expression selection.
Executed valid-literal controls expose two existing errors independently of
oversized literals:

```sql
SELECT abs(-9223372036854775808) OR (0 OR 1);
SELECT u.id FROM u LEFT JOIN u AS r
ON abs(-9223372036854775808) OR 1;
```

The first returns native1 but Modern raises overflow. The second raises native
overflow but Modern returns a row. A blanket rule disabling simplification in
ON is wrong: the same LEFT with `WHERE r.id IS NOT NULL`, or with a direct
result alias replacing1, returns a native row. A unary-plus alias does not
acquire that truth flag. Thirty-five paired provenance controls separate
these cases, captured signed aliases, wrapper barriers and HAVING timing.

Native reduction must precede HAVING-to-WHERE transfer. An eligible
`HAVING r.id IS NOT NULL` does not authorize the earlier reduction; the same
predicate in original WHERE does. Current binding preserves HAVING's native
shape but publishes only its post-transfer WHERE root.

Twenty-one genuine native EXPLAIN baselines also disprove a semantic
"null-rejecting expression" replacement. In the pinned snapshot:

- `r.id IS NOT NULL AND 1` reduces a LEFT; `1 AND r.id IS NOT NULL` does not.
- FULL with `u.id IS NOT NULL AND r.id IS NOT NULL` becomes LEFT, while swapping
  those operands becomes RIGHT. Comparisons on both sides can become INNER.
- A function-containing proof and `IS NOT NULL` inside OR can miss reduction.
- A later INNER ON may reduce an earlier LEFT, but that INNER ON is excluded
  while proving reduction of a RIGHT boundary.

Whole alias replacement also affects term ownership and scope, not just truth.
Native `SELECT 0 AS no ... LEFT JOIN ... ON no` returns no rows, whereas raw
`ON 0` preserves the left row. A whole forward-source alias in ON is permitted
when its replacement has no ON flag, while a pre-existing unary parent retains
its flag and rejects that reference. Current Modern's clause-wide inference
misses these distinctions. Keep the actual flag and the SQL occurrence owner
as separate facts.

These are native algorithm observations, not permission to improve its proof.
The exact top-level versus walker state and AND-tail handling must be retained.

### Pinned source map

Reference: SQLite3.54.0, Git
`cb547ab3e931c7766e24834af5ef6c4578863e3d`.

| Responsibility | Native mechanism |
| --- | --- |
| Expression always-true/false | `sqliteInt.h:3186-3187`; require `EP_IsTrue/False` without `EP_OuterON` |
| Raw ON tagging | `select.c:412-455,653-660`; tag every ordinary expression node with join ownership, without entering child SELECTs |
| Generated USING terms | `select.c:594-650`; tag each generated equality, not its column children or new AND parent |
| Namespace and alias phases | `select.c:6552-6565`; expansion/join processing precedes resolution |
| Real result alias replacement | `resolve.c:68-106`; copy the definition's flags, not the replaced identifier's ON flags |
| Flag-driven ON scope check | `select.c:7480-7601`; activate an owner at a marked node and check its whole subtree, including captured outer sources |
| Combined WHERE construction | `expr.c:1158-1175`; append original WHERE and ON/USING in declared order; ON flags prevent false-AND deletion |
| Proof root normalization | `expr.c:218-232,7135-7155`; skip COLLATE/likelihood, not arbitrary unary operators |
| Non-null-row proof | `expr.c:6977-7155`; distinct root and walker states, exact AND tail, both-sided nested AND/OR, ON exclusions and function/IS/truth pruning |
| Reduction and flag retirement | `select.c:457-493,7848-7905`; declaration-order LEFT/RIGHT/FULL transitions and per-owner ON retirement |
| Later HAVING transfer | `select.c:8553-8561`; after the earlier strength-reduction phase |
| Recursive logical selection | `expr.c:2389-2403,4973-5040,6237-6269,6434-6474`; right then left, no child-tree mutation |

### Canonical Modern inventory

Reuse `ExpressionProperties`, `BoundExpressionProperties`, mandatory publication
origin, one binding scope/restoration, one immutable source/expression/query
owner, real alias-property copies and existing generated-expression builders.
Do not propagate ON origin from children: unlike `EP_Subquery`, it is not an
ordinary propagated construction flag.

`FinishSelectQueryCore` already has the original bound WHERE immediately before
`TransferHavingToWhere`. Keep one immutable ID there, not a copied predicate.
Query-containing result copies still use the one canonical binder and scoped
query cache; pure aliases reuse their immutable definitions.

`CollectLoopTerms`, `PrepareLoopTerms`, `WhereLoopBuilder`, `WhereLoopPlan`,
joined term scheduling and physical publication are the sole optimizer chain.
They currently read declared join kinds. The existing physical join nodes and
per-core access metadata are the sole output contracts. Preserve source IDs,
predicate occurrence/ON owner, native costs/search budgets, access consumption
and the one positioned source producer.

The lowerer already retains `active_streaming_core_` in `ReadEmissionBinding`.
Borrow its per-core physical metadata; do not introduce another emission
context, origin registry, cursor map, evaluator or query-state cache.
Replace the existing shallow `SimplifiedLogicalOperand`, not its consumers
with separate recursive implementations.

## Decision

### 1. Publish bounded native ON origin and the earlier WHERE identity

Add `on_source_ordinal`, an optional byte-sized declared source position, in the
existing properties padding. It represents native ON/USING origin, including
INNER origin; absence represents no such flag. The core's at-most64 sources
bound it. It is not a global source ID or a runtime flag.

Measured MSVC and strict GNU prototypes retain properties16, expression112,
payload80, token24 and lowering-error72 bytes; optional ordinal size is2.
Preserve these layouts and ordinary stacks in actual implementation.

Set origin through the existing binding scope and canonical publication API.
Raw ON syntax, including constructor replacements, receives its current owner.
Child SELECT binding starts without the parent's ON origin. Real result aliases
and semantic copies retain the definition's properties; a pre-existing parent
does not acquire replacement-child flags. Preserve generated joined-value and
incoming-VALUES distinctions already made by ADR-0162.

Generated USING equality roots receive their owner; their fresh AND parents
and generated column children retain their native untagged disposition.
Every existing generated constructor and copied publisher must choose its own
origin explicitly. Do not mark an entire resolved ON tree after binding.

Publish `pre_having_where` on the existing `BoundSelectCore`, captured immediately
before HAVING transfer. It is an optional ID into the same immutable arena,
not a second tree, predicate collection or runtime WHERE. Ordinary filtering
continues to consume the existing post-transfer `where`.
Keep query/core lifetime, checkpoints, copies and validation consistent.

Native ON scope validation observes marked expression regions, not the original
clause's entire resolved dependency set. Reuse the canonical source/capture
visitor once a marked region activates its owner; do not introduce a second
source-dependency algorithm. Preserve the current source-count, qualifier and
query-parent contracts. This correction does not claim to close the separate
general name/value diagnostic-phase boundary.

### 2. Translate the native proof and one effective join state

Implement `sqlite3ExprImpliesNonNullRow` as its own native compiler predicate,
not a reuse of generic source dependency or constant truth. Those have different
semantics. Its root state and expression-walker state must remain distinct:

1. Normalize only the native transparent root shapes.
2. Handle the one initial NOT-NULL root before the AND loop.
3. Recurse on an AND's left, then continue on its right without re-entering the
   initial NOT-NULL case. Do not flatten this into an OR of independently
   tested conjuncts.
4. In walker state, exclude outer ON and, for RIGHT inquiry, inner ON; prune
   functions, IS/null/truth tests and child queries as native does.
5. Match only the requested source's canonical column/rowid leaves. Both sides
   of nested AND/OR must independently prove the property; nonempty list-IN
   walks only its operand. Preserve native false negatives.

The input is the original `pre_having_where` plus ON/USING roots in native
append order. Model the left-associated combined AND as a borrowed view over
these existing roots; do not allocate another AST or use the flattened access
term list for the proof.

Keep one bounded effective-kind array on the existing transient `WhereLoopPlan`.
Initialize it from declared kinds and perform declaration-order native
reductions before term prerequisites, candidates, order properties or path
solving. A source is queried for its own LEFT role and/or its position left of
a remaining RIGHT boundary, matching native `JT_LEFT`/`JT_LTORJ`.

Preserve the exact transitions: FULL-to-RIGHT, LEFT-to-INNER, later
FULL-to-LEFT and RIGHT-to-INNER. Subsequent proofs observe the updated state.
Derive per-node outer/inner ON interpretation from its origin owner and that
state, including alias replacements. Do not compute a second reduction in
lowering or validate by rerunning the solver.

Use the same effective state for every optimizer join barrier, right floor,
key eligibility, automatic partial construction, residual phase/consumption,
source order and physical join kind. Retain declared descriptors for binding
namespace and original source identity. No compatibility fallback silently
reuses an old declared-kind test.

Access terms retain their SQL conjunct occurrence and original ON owner for
identity/validation, but native ON readiness, key floors and constraint ownership
come from the term expression's actual origin flag. Do not recover a lost alias
flag from clause provenance. Existing placements must support a flag-free term
attached to a preserving join running pre-open or at a later ready source.
Only effective outer-ON terms run before that source's match publication;
flag-free terms use the canonical ordinary residual phase, including NULL and
RIGHT/FULL replay. Keep one placement list, not a second WHERE/alias collection.

Publish the effective outer-owner bitmask on existing `PhysicalCoreAccessPlan`,
beside its source sequence. Physical join nodes consume the same canonical
state. The mask is compiler metadata, not VM state; it provides O(1) flag
interpretation without a second container or span-search heuristic.
Keep existing conservative column-value handling; do not introduce new
CanBeNull-dependent code-elision optimizations in this correction.

The existing source producer consumes effective join kinds for preservation,
match-register/relation allocation, replay and readiness. It never re-proves
reduction. Existing schema/USING output provenance, cursor ownership, NULL-row
semantics and typed cleanup remain unchanged.

### 3. Use one native truth observer and recursive selector

Observe raw truth hints through the expression's native ON origin and the
current core's published outer-owner mask. A preserved outer origin blocks an
always-true/false hint; ordinary, real-alias and reduced-inner origins do not.
Use the existing saved/restored core binding, including child/aggregate/coroutine
consumers. No blanket ON suppression or global "has outer join" switch.

Replace `SimplifiedLogicalOperand` with the native recursive right-then-left
rule. It may return a selected descendant, but must not rewrite a retained
parent or cache a different expression tree. Keep one implementation for
conditional, ordinary logical-value and query-logical-value consumers.
Representative-register selection remains authoritative before expansion.

Retain the native boolean coercion, query-flag operand order, NULL skip policy,
required retained operand regions, query lifetime/cache and ordinary arithmetic
frames. Query-free logical values do not become universally lazy.
Use bounded compiler recursion only if the original default-stack depth tests
prove it safe; otherwise the accepted same-rule continuation refinement must
precede a change. Do not increase stack or lower admitted depth.

## Ordered development and verification

Use four small buildable commits in one related prerequisite PR:

1. Provenance foundation: public binder metadata and pre-HAVING identity red,
   canonical origin publication, alias/query/generated-USING controls,
   unchanged layouts and complete fast/owning review.
2. Flag-driven scope/placement: real whole-alias false/forward ON reds, actual
   marked-region checks and term flags through the existing schedule/producer.
   Preserve whole-parent versus copied-child and replay/early-stop controls.
3. Native reduction and observation: real native join-kind/phase red plus the
   simple LEFT-ON overflow red, exact proof/state/publication through solver
   and producer, matched plan/false-negative and failure controls.
4. Recursive selection: the public nested-OR overflow red, one shared selector,
   callbacks, aliases, preserved/reduced ON, query scopes and maximum depth.

Each requires Accepted design, focused real red/green, complete isolated native
fast units, complete affected OOM/allocation sweeps, independent implementation
review and sealed ADR-0106 evidence before its successor. Preserve1024/256 and
all other current ceilings, assertions and timeouts.

Cover LEFT/RIGHT/FULL combinations; WHERE versus ON versus late HAVING;
source-free/captured/aggregate aliases; generated USING; false/null/NOT/IS/OR
proofs and deliberate misses; table/rowid/covering/noncovering/automatic accesses;
declared CROSS barriers; ordering, grouping, DISTINCT/sets and query values;
empty inputs, move/reset/rebind and shared mutation-query hosts.
No new source, SQL syntax, storage format, VM opcode or executor is admitted.

Retain21 native plan baselines and all35 paired provenance observations as
immutable prefixes. Add real unit/golden controls for the AND-tail NOT-NULL
asymmetry and pre-HAVING phase; never "correct" them with a broader proof.
Use existing fixtures and bounded fixed-work diagnostics, not expanded timing
campaigns or historical benchmark rewrites.

Extend only the relevant existing bounded compatibility shards, preserving old
native outcomes/compiler provenance and their512 caps. Run the combined actual
change-aware native/compiler/sanitizer/full-quality/four original fuzz gates at
the exact final head, reconcile actual names/work and publish durable evidence
before final acceptance and guarded squash. Verify actual resulting main.

## Explicit remaining boundaries

This closes native truth/ON interpretation needed by later value-generation
work. It does not claim ordinary literal/name/unary materialization, statically
discarded scalar width, IN query RHS, FROM/CTE/view/CASE/CAST, WAL or later nodes.
No eager value attempt followed by an error-shaped token fallback, second value
parser/cache, error queue or compatibility switch is authorized.
Node43/project remain incomplete.
