# ADR-0163: Native Truth Provenance and Outer-Join Strength Reduction

- Status: Accepted
- Date: 2026-10-27
- Module graph node: `implement-advanced-sql`
- Delivery: three ordered related commits under ADR-0106
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

### Delivery refinement after the accepted foundation

The original four-commit design was accepted before production. Its first
foundation is accepted and sealed at `da2fe13`, tree `dadbcea`. Before changing
the second slice's production path, further direct source mapping establishes
that term-flag retirement and effective join kinds are an indivisible compiler
transition, not independently activatable consumers.

The existing repeated-alias regression requires one selected rowid occurrence
and nine deterministic callbacks. A matched pinned native control confirms
three rows, nine callbacks and `SEARCH b USING INTEGER PRIMARY KEY`, without
`LEFT-JOIN`. Retaining a unary ON parent instead gives three rows, fifteen
callbacks and `SCAN b LEFT-JOIN`. The canonical Modern key-eligibility check
correctly rejects an unowned ordinary constraint on a still-preserving source.
Changing ON-term flags first would therefore retire the old key without the
native join reduction that makes its replacement eligible. Do not retain a
clause-derived fake flag, add a second eligibility proof, or weaken that
existing callback/access regression until a later commit.

Combine the originally planned scope/placement and reduction/observation
slices into one coherent solver-to-producer transition. Keep the already
accepted foundation and the later recursive-selector commit separate: the
normal batch has three reviewable commits, not four. The combined transition
retains all original reds, focused/complete-fast/owning gates and one exact
source review before commit. This delivery refinement requires acceptance
before its production changes.

### Default-stack continuation refinement

Exact native probes admit a whole forward ON alias of height999 and reject
height1000; they admit997 OR-proof terms and reject998. The ordinary Windows
stack and all existing construction limits remain acceptance requirements.
The first direct C++ translation passes the proof goldens but overflows while
processing the admitted deep OR proof. Do not solve that by raising the stack,
reducing the limit, flattening the proof, or accepting a missed optimization.

Represent the same root, AND-tail and walker states with one compiler-owned
continuation vector, reusing the existing binder/lowerer frame pattern rather
than introducing a new container. A continuation borrows the deferred child ID,
its native state, and whether both proofs or either proof is required. Tail
steps replace the current expression; only a deferred sibling needs a frame.
Unwinding preserves native short-circuit proof order and the one-time initial
NOT-NULL peel. No alternate AST, dependency collector or stronger proof is
introduced. Retain the canonical source-leaf identity and deliberate function,
IS/truth, ON and query pruning.

The source/capture dependency walker remains the sole ordinary dependency
traversal. Its node-entry callback supplies marked-region activation without
adding another recursive child-edge helper. Keep ordinary arithmetic frames
free of rich truth-diagnostic results: a compact typed error code propagates to
one outlined rich diagnostic boundary. Actual PE/PDB inspection identifies a
failed784-byte arithmetic frame; the compact result restores608 bytes and the
unchanged1000-depth arithmetic/VALUES test passes.

This same-rule continuation refinement requires acceptance before replacing
the proof recursion. Add the exact native accepted/rejected depth regressions
and complete affected allocation/OOM sweeps under the original128/256/1024
ceilings. Retain all original fast, callback, replay, compiler and final gates.

### Native flag-retirement reachability refinement

Independent source review rejects candidate `dcddd8f`: an effective INNER
owner does not mean every descendant's outer-ON flag was retired. The parent
independently reproduces native overflow versus a Modern row for:

```sql
SELECT a.id FROM Items a LEFT JOIN Items b
ON 1 IN (abs(-9223372036854775808) OR 1,2)
WHERE b.id IS NOT NULL LIMIT 1;
```

`sqlite3SetJoinExpr` tags every ordinary expression-list child. Its inverse
`unsetJoinExpr` visits left/right edges and only `TK_FUNCTION` expression
lists, not `TK_IN` lists. Four executed reduced-list/nested-function controls
retain native overflow. Six controls distinguish the visited IN operand,
singleton-IN equality rewrite, function arguments, real alias replacement,
original INNER and preserved LEFT cases. This is a shallow-OR correction
required before the later recursive selector, not a new IN-query admission.

Retain this native reachability in the one existing origin property. Extend
the foundation's optional ordinal to `on_origin`, an optional one-byte
`BoundOnOrigin` value containing a six-bit core-local source ordinal and one
retirement-blocked bit. This is typed compiler metadata, not a new flag
registry, second container or runtime state. Both actual MSVC and strict GNU
prototypes retain origin1/optional2/properties16/expression112/payload80/
token24/lowering-error72. Internal construction rejects ordinals outside64
instead of silently truncating; existing SQL source-count validation remains.

The canonical bind scope starts retirement-reachable at an ON root. Only
genuine list-IN RHS binding blocks retirement for the incoming origin;
ordinary descendants inherit that disposition. A nested function cannot
restore reachability across a skipped list edge. The IN operand and existing
`SingletonEquality` constructor retain the incoming disposition because
native retirement visits their rewritten ordinary edges. Child SELECTs clear
the entire enclosing origin. Real alias/property copies retain the
definition's complete origin, including its absence; do not post-mark a
resolved subtree or propagate the bit from children.

Use one origin interpretation rule in optimizer proof/term consumers and the
truth observer: a reachable origin observes the effective owner role; a
blocked origin observes the original declared owner role. Borrow the latter
directly from the existing active core's declared source vector by ordinal,
beside the effective mask. This remains O(1), reuses the saved/restored core
binding and requires no second role array, per-expression map or lowering
walk. Original INNER origins do not become outer merely because retirement
is blocked. Rename directly dependent internal call sites without a shim.
Keep conservative column nullability unchanged.

This refinement requires design acceptance before production changes, plus
focused metadata and actual overflow reds/greens, all three join kinds,
nested/singleton/function/alias/query controls, source1/63 and child-scope
restoration, original carrier/default-stack bounds, complete affected
allocation/OOM sweeps and exact-source review. Candidate `dcddd8f` was never
committed; its earlier green evidence is preserved as rejected, not reused as
corrective acceptance.

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
| WHERE term construction | `whereexpr.c:60-85,1217-1227,1635-1644`; split AND through native transparent wrappers, observe the stripped term's flags, add outer-owner prerequisites, and retire late inner origin |
| Constant-term bypass | `where.c:6455-6477,7005-7045`, `expr.c:2326-2330`; no local prerequisites, deterministic execution, no outer origin, and no remaining inner origin in a RIGHT/FULL core; the SELECT-fail callback rejects query-containing guards |
| Candidate versus ordinary residual phase | `wherecode.c:1476-1516,2680-2727,2840-2875`; native readiness, match publication, then ordinary constraints in the existing replay body |
| Combined WHERE construction | `expr.c:1158-1175`; append original WHERE and ON/USING in declared order; ON flags prevent false-AND deletion |
| Proof root normalization | `expr.c:218-232,7135-7155`; skip COLLATE/likelihood, not arbitrary unary operators |
| Non-null-row proof | `expr.c:6977-7155`; distinct root and walker states, exact AND tail, both-sided nested AND/OR, ON exclusions and function/IS/truth pruning |
| Reduction and flag retirement | `select.c:412-493,7848-7905`; declaration-order LEFT/RIGHT/FULL transitions; setter visits ordinary lists, retirement visits only function lists, preserving skipped IN-list descendants |
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

The foundation's `on_source_ordinal` establishes a byte-sized declared source
position. Refine it to `on_origin`, one optional byte-sized `BoundOnOrigin`
containing that position and native retirement reachability in the same
properties space. It represents native ON/USING origin, including INNER
origin; absence represents no such flag. The core's at-most64 sources bound
it. It is not a global source ID or a runtime flag.

Measured MSVC and strict GNU prototypes retain properties16, expression112,
payload80, token24 and lowering-error72 bytes; optional origin size is2.
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
Publish that existing per-term outer disposition with its placement so the
producer does not re-infer it from the containing physical join. Constant
flag-free terms may bypass even an empty preserving join; a retained inner
origin in a RIGHT/FULL core may not. Scope validation still observes original
marked parents before WHERE-term COLLATE/likelihood stripping.
Retain the canonical guard predicate's query rejection. Actual paired empty
controls show that an ordinary `abs(INT64_MIN)` alias errors before opening,
but a scalar-query alias `(SELECT abs(INT64_MIN))` does not. The native
determinism walk's SELECT-fail callback, not a generic source-free dependency
test, distinguishes them.

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

Observe raw truth hints through the expression's native ON origin,
retirement reachability, original declared role and the current core's
published effective outer-owner mask. A retained outer origin blocks an
always-true/false hint even if its owner became INNER; ordinary, real-alias
and actually retired inner origins do not.
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

Use three buildable commits in one related prerequisite PR:

1. Provenance foundation: public binder metadata and pre-HAVING identity red,
   canonical origin publication, alias/query/generated-USING controls,
   unchanged layouts and complete fast/owning review.
2. Native join provenance transition: real whole-alias false/forward ON reds,
   actual marked-region checks, term flags and exact native join reduction
   through the existing solver, schedule and producer. Include the native
   join-kind/phase and simple LEFT-ON overflow reds, one effective state and
   truth observer. Preserve whole-parent versus copied-child, the existing
   repeated-alias access/callback contract, matched proof false negatives,
   empty inputs, replay/early-stop and failure controls.
3. Recursive selection: the public nested-OR overflow red, one shared selector,
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
