# ADR-0104: Conditional Predicate Branch Emission

- Status: Accepted
- Date: 2026-10-17
- Module graph node: `implement-advanced-sql`

## Context

The implemented-engine SQLite reference audit identifies A3: Modern evaluates
logical predicate expressions as complete register values before branching.
Pinned SQLite instead uses dedicated conditional code generation in WHERE,
HAVING, ON, IIF conditions, aggregate FILTER clauses, and mutation
predicates.

Begin after PR #213, main commit
`c7ae1e86cee99ed3f911a5087eb3f32c52ba52ad`, accepted tree
`014603ef28a12441327f2b35841d5e0bfe587043`. All nine first-head hosted checks
passed. The original unchanged sanitizer model passed in 3.28 seconds under
its ten-second deadline. B2 is complete; node 43 and the other four
reference-audit findings remain open.

The exact existing mechanisms are:

- `PlanLowerer::EmitPredicate` emits a complete expression value and then
  jumps on false or NULL.
- `EmitBinaryValue` evaluates both logical operands unless the binder's narrow
  SQLite-compatible truth hint selects one operand.
- `EmitConditional` separately evaluates each IIF condition as a value and
  then performs false/NULL jumps.
- aggregate FILTER has another value-then-jump body in
  `aggregate_modifier_lowering.cpp`.
- every WHERE guard/residual, HAVING expression, admitted ON expression, and
  mutation predicate already reaches `EmitPredicate` at its established
  optimizer-selected phase.
- aliases, likelihood hints, collations, labels, typed conditional jumps,
  lazy coalesce/IIF values, aggregate source selection, and verified
  subquery calls already exist.

Pinned `src/expr.c:6224-6585` has separate `sqlite3ExprIfTrue` and
`sqlite3ExprIfFalse` paths. Logical AND, OR, and NOT branch recursively with
three-valued NULL behavior. A truth test delegates to its operand with the
appropriate NULL policy. If the left logical operand contains a subquery and
the right does not, the right operand is considered first. Ordinary
comparisons can also avoid a subquery operand when the already-evaluated
non-query operand is NULL. These rules do not make ordinary scalar AND/OR
values universally lazy: `src/expr.c:4986-5033` retains value-expression
evaluation, with only its own narrow simplification and subquery optimization.

The frozen 20-case predicate audit has 12 differences. Eleven arise from the
missing conditional branch mechanism. One WHERE case,
`(NULL OR <row error>) AND id<0`, also depends on pinned SQLite selecting an
INTEGER PRIMARY KEY `rowid<?` range. Modern currently publishes a table scan.
This slice must not reorder residuals or invent a range access merely to hide
that independent optimizer difference.

## Decision

### 1. Add one canonical conditional branch emitter

Keep value production in `PlanLowerer::EmitExpression`. Add one private
conditional helper that receives:

- a bound expression;
- a target label;
- whether the target represents true or false; and
- whether NULL also reaches the target.

The helper uses existing labels, registers, `JumpIfInstruction`, and
`EmitExpression`. Extend the existing `JumpCondition` enum with two composite
conditions:

- true or NULL; and
- false or NULL.

ADR-0162 amends the C++ traversal implementation, not this conditional
algorithm: use lazy bounded compiler continuations for expression/label work
instead of recursive typed-result frames. Exact native1000-level HAVING
controls exposed default-stack overflow after preserving the native clause
tree. Operand order, local label binding, NULL policy and the leaf fast path
remain unchanged; no VM state or increased stack/depth limit is introduced.

The VM evaluates each through the existing SQL truth conversion, and the
program verifier admits the two new enum values through the existing
`JumpIfInstruction` validation. This adds no instruction alternative, operand,
register state, evaluator, executor, container, or plugin mechanism.

Use a composite condition whenever the caller sends NULL to the same target as
true or false. Retain a separate NULL jump only for comparison-operand
short-circuiting, where NULL may bypass an unevaluated query operand to either
the caller's target or a local fallthrough label.

For logical AND and OR:

1. apply the existing `SimplifiedLogicalOperand` rule;
2. consider the right operand first only when the left contains a nested query
   and the right does not, matching `exprEvalRhsFirst`;
3. recursively emit the same true/false/NULL control-flow structure as
   `sqlite3ExprIfTrue` and `sqlite3ExprIfFalse`; and
4. bind only local labels owned by that recursive branch.

Logical NOT swaps true and false while retaining the caller's NULL policy.
Alias, likelihood, and collation wrappers delegate to their operand.

Truth tests delegate directly to their operand. The truth-test result itself
is never NULL, so the outer caller's NULL policy is ignored:

| Target | Expression | Operand branch | Operand NULL |
|---|---|---|---|
| true | `x IS TRUE` | true | no branch |
| true | `x IS NOT TRUE` | false | branch |
| true | `x IS FALSE` | false | no branch |
| true | `x IS NOT FALSE` | true | branch |
| false | `x IS TRUE` | false | branch |
| false | `x IS NOT TRUE` | true | no branch |
| false | `x IS FALSE` | true | branch |
| false | `x IS NOT FALSE` | false | no branch |

For ordinary comparisons containing a nested query, reproduce
`exprComputeOperands`:

1. evaluate the right operand first only when the left contains a query, the
   right does not, and the right syntactically may be NULL;
2. in that right-first form, a NULL right operand takes the caller's NULL edge
   without evaluating the left query;
3. otherwise evaluate the left operand first;
4. when the right contains a query and the left syntactically may be NULL,
   a NULL left operand takes the caller's NULL edge without evaluating the
   right query; this includes comparisons whose two operands both contain
   queries; and
5. otherwise evaluate the remaining operand and emit the existing typed
   comparison.

`IS` and `IS NOT` remain non-NULL comparisons and bypass this optimization,
retaining left-to-right operand evaluation.

Add one private allocation-free syntactic may-be-NULL inspection equivalent
to SQLite's conservative `sqlite3ExprCanBeNull` rule for the supported bound
surface:

- strip only unary plus and unary minus;
- non-NULL INTEGER, REAL, TEXT, and BLOB literals cannot be NULL;
- a column or rowid cannot be NULL only when its existing catalog/source
  contract proves NOT NULL and its source cannot be NULL-extended by the
  current join core; and
- every unproven or more complex shape may be NULL.

False positives only retain an unnecessary runtime NULL check. A false
negative would change execution, so the helper defaults to may-be-NULL. It
uses existing source-column, catalog, and join-preservation contracts and
adds no persisted binder metadata.

Use a separate bounded recursive query-containment inspection over the
immutable bound expression graph. `BoundSubqueryExpression` is terminal:
report that it contains a query without descending into the referenced query
body. Recurse through every other ordinary payload and aggregate invocation
argument, FILTER, and ordering expression. Do not substitute capture/source
dependency traversal, add a second ownership graph, or persist compiler state
in bound contracts merely for this lowering decision.

### 2. Reuse the branch emitter in every current predicate surface

`EmitPredicate` becomes the canonical false-or-NULL branch call. Its existing
call sites retain their current labels and phases:

- statement guards and row filters;
- global and grouped HAVING;
- INNER/LEFT/RIGHT/FULL ON conditions at their current readiness source;
- DISTINCT/compound/streaming filters;
- UPDATE and DELETE source predicates; and
- existing aggregate access paths.

IIF conditions use the same false-or-NULL branch instead of first
materializing a logical value.
CASE syntax remains an explicit unsupported boundary and is not admitted by
this slice.

Extend `AggregateModifierServices` with one predicate operation that carries
the existing `AggregateExpressionSource`. The `PlanLowerer` implementation
temporarily selects that source and delegates to the canonical branch helper.
Remove the modifier module's duplicate value-then-jump filter body. Aggregate
arguments and order keys remain value expressions.

### 3. Preserve value semantics and optimizer ownership

Do not change `EmitBinaryValue`, `BinaryInstruction`, or `LogicalBinary`.
Ordinary result, ORDER BY, grouping, function-argument, aggregate-argument,
and stored value expressions retain their current scalar evaluation contract.
The existing narrow truth-hint simplification remains the only general
logical simplification in this slice.

Do not change:

- top-level conjunct splitting;
- statement-guard classification or source order;
- residual order or source-readiness placement;
- rowid/index/automatic-index candidate admission and costs;
- ON condition ownership;
- aggregate representative ownership;
- subquery ownership, caching, or call ABI; or
- callback determinism classification.

Do not widen the existing 64-source bytecode-size bound. A rejected
two-jump prototype generated 2,221 instructions against the unchanged 2,176
limit. The composite `JumpCondition` values are required to retain one typed
branch per predicate outcome and keep the existing linear program-size
contract.

Consequently, constant source-independent errors hoisted as existing guards
remain observable even when another conjunct rejects every row. The known
rowid-range audit case remains an explicit expected difference. A4's joined
ON/access readiness findings also remain open; this slice corrects logical
branching inside the already-selected phase and does not claim to move that
phase.

### 4. Add a dedicated bounded compatibility shard

The primary read corpus is at its 512-case limit. Add a separate version-1
conditional-predicate corpus and pinned oracle using the existing
`read_compatibility.py`, trace runner, oracle regeneration test, and fixture
sources. Do not raise or replace the primary corpus limit.

Cover:

- WHERE and HAVING AND/OR/NOT with true, false, and NULL operands;
- constant and row-dependent error-producing controls;
- nested logical forms and truth tests;
- IIF conditions;
- aggregate FILTER for direct, DISTINCT, ordered, global, and grouped
  invocations;
- INNER and preserving join ON expressions at their existing phase;
- scalar and EXISTS operands, including the pinned right-first rule;
- comparison NULL edges that skip a query operand;
- left-query/right-null reversal, right-query/left-null skipping, both-query
  comparisons whose NULL left side skips the right query, a
  left-query/right-definitely-non-NULL left-first instruction-order control,
  and unchanged `IS`/`IS NOT` left-to-right behavior;
- nested truth tests that exercise both true-target and false-target
  recursion;
- reset/rebind and repeated step behavior;
- one native-valid unsupported prepare boundary;
- one ordinary compare-mode prepare-error control; and
- ordinary SELECT logical values that must remain value expressions.

Use unsupported CASE syntax as the native-valid boundary control; do not add
CASE parsing or binding in this slice.

Use existing core and joined fixtures. Add no fixture solely for this slice.
The committed shard must compare every newly admitted behavior and retain the
known rowid-range/access case as a documented external audit control rather
than an accepted corpus mismatch.

Register:

- `differential.conditional_predicate_compatibility`, using the built trace
  runner and therefore remaining configuration-dependent; and
- `differential.conditional_predicate_oracle_regeneration`, using the exact
  pinned SQLite inputs and therefore joining ADR-0102's
  `configuration-independent` inventory.

Add only the oracle-regeneration test name to
`project/verification-policy.json`. Retain the compatibility test in every
applicable configured tree. The corpus supported matrix records both the
native-valid unsupported boundary and the compare-mode prepare-error control
required by the existing differential harness.

## Verification and delivery

Obtain GPT-5.6 Sol design approval before changing production code.

Develop observable behavior test-first. The first production red must show a
row-dependent dead logical branch executing in a predicate context. Add
separate pre-change reds for IIF and aggregate FILTER before extending their
shared service surface. Preserve an ordinary SELECT-value control that remains
eager and a constant guard control that still reports its error.

Re-run the frozen 20-case predicate audit. The branch slice targets 19 exact
matches and one unchanged, explicitly attributed rowid-range/access
difference, with no new difference. Do not describe that result as complete A3
closure until the remaining access mechanism has its own accepted design.

Cover all current callers of `EmitPredicate`, truth/NULL matrices, query
operand ordering, comparison NULL skipping, callback counts, reset/rebind,
LIMIT zero, allocation failure, both composite `JumpCondition` VM outcomes,
invalid-enum verifier rejection, and conditional control-flow checks. Update
the existing ON test that intentionally preserves the now-corrected logical
branch result, while retaining independent A4 readiness controls. Preserve the
existing 64-source instruction-count ceiling.

Run the complete fast tier and ADR-0102's actual change-aware plan. Retain
required compiler, sanitizer, native, fuzz, format, engineering, and graph
evidence without changing workloads, seeds, allocation ceilings, thresholds,
or deadlines. The original correlated sanitizer model retains its ten-second
deadline.

Obtain GPT-5.6 Sol implementation and exact-tree review. Require all nine
exact-head hosted gates before guarded squash merge and verified named branch
cleanup. This slice does not complete node 43, A4, the rowid-range/access
difference, R2, the verifier finding, or later nodes.

### Focused and integrated evidence

Three focused public tests first failed because a dead predicate branch, IIF
condition, and aggregate FILTER each executed their error-producing operand.
The first two-jump branch implementation corrected those cases and moved the
frozen audit from 8/20 to the intended 19/20, but it produced 2,221
instructions for the unchanged 64-source regression against its 2,176 limit.
That implementation was rejected rather than weakening the bound.

The accepted composite `JumpCondition` implementation retains the existing
instruction variant and restored the original program-size ceiling. Direct VM
tests cover true, false, and NULL for both composite conditions, and the
verifier rejects an unknown condition value.

Implementation review then found that recursive decomposition could recompute
a retained grouped key. The focused nondeterministic HAVING regression
observed four calls instead of the three input-row calls and incorrectly
published a group. The corrected branch entry reuses the existing grouped-key
register through `EmitExpression`; the regression and complete fast tier pass.

The dedicated shard contains 51 cases: 50 comparisons, including the required
prepare-error control, plus one native-valid unsupported CASE boundary. It
matches the exact pinned oracle. The frozen external audit remains 19 matches
and one unchanged rowid-range/access difference, with no new difference.

The aggregate-modifier allocation contract improves from 57 to 56 allocations
because the duplicate FILTER value path is removed. Other exact allocation
counts and the 64-source instruction ceiling remain unchanged.

## References

- ADR-0031: Typed Bytecode Programs
- ADR-0033: Immutable SELECT Binding
- ADR-0036: Physical Read Plan Lowering
- ADR-0053: SQLite-Aligned Advanced SQL Architecture and Ordered Delivery
- ADR-0069: SQLite-Aligned Aggregate Modifier Execution
- ADR-0081: Parameterized Joined Rowid Access
- ADR-0090: Source-Correlated Scalar and EXISTS Queries
- ADR-0096: Computed Joined Rowid Probes
- ADR-0102: Change-Aware Verification
- `docs/sqlite-reference-audit.md`: A3 and A4
