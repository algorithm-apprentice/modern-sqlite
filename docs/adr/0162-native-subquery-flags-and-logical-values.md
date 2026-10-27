# ADR-0162: Native Subquery Flags and Logical Value Generation

- Status: Accepted
- Date: 2026-10-27
- Module graph node: `implement-advanced-sql`
- Delivery: two ordered prerequisite commits under ADR-0106

## Context and completed source mapping

Start from PR #266's verified squash `3b6644b`, tree
`ce1585dd2202df38219a0514a129bb33ebbc3b34`. Actual resulting-main native,
GNU/macOS, sanitizer and full-library quality passed. The original Windows
256-action model passed2.60s under its unchanged20s deadline without retry.
Node43 remains active.

Direct research for the remaining literal-generation boundary found an earlier
control-flow prerequisite. Native
`SELECT (SELECT abs(-9223372036854775808)) OR abs(1)` returns1; Modern executes
the unused query and raises integer overflow. Reusing the current recursive
query-containment helper for the missing value branch would introduce another
error: Native treats a COLLATE wrapper differently from an ordinary subtree.

The pinned SQLite3.54.0 source map is:

| Responsibility | Native source and invariant |
| --- | --- |
| Propagated construction properties | `sqliteInt.h:3174-3177`; `expr.c:1015-1054` copies `EP_Propagate` from attached children |
| Direct scalar/EXISTS query property | `expr.c:1079-1090` sets `EP_Subquery` when attaching a SELECT |
| COLLATE boundary | `expr.c:175-199` attaches the operand without copying its propagated properties |
| Function argument properties | `expr.c:858-888,1181-1210` propagates argument-list properties |
| Function ORDER BY/FILTER attachment | `expr.c:1234-1283`; `window.c:1311-1327` do not propagate those later attachments into the function's query property |
| Alias replacement | `resolve.c:68-106` copies the resolved result expression, not a new parent's properties |
| Synthetic singleton-IN operands | `parse.y:1520-1526`; `expr.c:1015-1054` constructs unary plus from the RHS alone, then the comparison from both inputs |
| Logical operand order | `expr.c:2408-2421`, `exprEvalRhsFirst` inspects the two operand properties |
| Logical value emission | `expr.c:4973-5040`, `exprCodeTargetAndOr` |
| Predicate emission | `expr.c:6237-6269,6434-6474` uses the same operand-order rule |
| Comparison branch admission | `expr.c:6303-6319,6503-6519` requires the comparison root's query property before subquery/NULL operand skipping |
| Query call ownership and cleanup | `expr.c:3914-4053`; existing ADR-0090/0157 query owners and verified call regions |

This property is not semantic query containment. A collated query still owns
and binds its SELECT. A directly resolved alias copies its target's query
property, but an already constructed unary/function/arithmetic parent does not
retroactively gain that property when its identifier child resolves to an
alias containing a query.

The executed93-case read matrix retains all earlier observations and separates
these distinctions. For example,
`SELECT 7 WHERE ((SELECT abs(-9223372036854775808)) COLLATE BINARY) OR abs(1)`
raises the native runtime error, whereas Modern incorrectly returns7.
Direct versus wrapped aliases with OFFSET that suppresses output distinguish
the flag from later projection evaluation. Comparison aliases distinguish the
root-property gate from inspecting only the resolved operands.

Fifteen pinned C-API callback controls also record exact order/counts. With
non-deterministic `probe(x)` recording and returning x, the query-containing
forms omit an unnecessary query, query-free OR still calls both functions,
NULL does not skip a required query, COLLATE retains eager value evaluation,
and an alias's new parent does not acquire its child's replacement property.
This is fixed-work execution evidence, not a timing claim.

## Decision

### 1. Publish one native construction property

Add one `has_subquery_flag` boolean to the existing internal and published
expression-properties records. Place it in existing alignment padding and
measure the actual carrier layout: do not grow the ordinary112-byte bound
expression, its80-byte payload, or the72-byte lowering-error carrier.
It is immutable compiler metadata, not a VM field, query identity, source
dependency, semantic-containment answer, or second query registry.

Compute it in the existing per-expression construction annotation/kernel.
Enable that same existing pass when the current inventory encounters a
subquery expression, in addition to its current IN/AND/IS consumers. Add no
new arena or AST traversal. Preserve source-ordered parameter assignment and
the existing multirow VALUES event schedule.

Direct query nodes set the property. Ordinary attached children and function
arguments propagate it. Parentheses are transparent. COLLATE clears it.
Later function ORDER BY/FILTER attachments do not supply it. Constructor
replacement with an integer boolean clears it. Do not enter a child SELECT
to compute its parent's property.

Use the existing expression/query binding context restoration to publish the
current syntax construction property through the canonical `AppendExpression`.
A nested query starts its own construction context; generated joined-column
expressions must not inherit the enclosing query expression's property.
Retain the original metadata for actual copied result aliases and result
references, including freshly instantiated query-containing definitions.
Use one explicit private metadata-origin distinction for those copies, not
an inferred rule that every `BoundAliasReferenceExpression` is a name alias:
joined-value and incoming-VALUES likelihood wrappers are different producers.
Retain the latter's original constructor property, including ignored arguments.

Every synthesized constructor derives its metadata from its own inputs rather
than the enclosing binding context. Singleton IN's generated unary plus uses
the original RHS construction property; its comparison/optional NOT retain
the combined expression property. Generated integer constants have no query
property, and normalized scalar-limit comparisons retain the original limit
operand's property. Use the same explicit property-publication mechanism for
these constructors and copied expressions; add no special runtime IN branch.

Keep the existing `BoundExpressionContainsQuery` semantic helper unchanged.
It remains authoritative for query ownership, result-definition copies and
binding/capture work. It cannot answer the native flag question because its
required semantics deliberately include queries beneath COLLATE and aliases.
No second equivalent semantic walker is authorized.

Change the lowerer's existing operand-order classifier to the published O(1)
construction property. Conditional comparison optimization additionally checks
the comparison root's property, exactly as the native caller does. Keep
conditional labels, NULL policy, IS/IS NOT behavior and optimizer placement.
Do not reorder WHERE conjuncts, guards, ON readiness or aggregate phases.

#### Accepted corrective refinement: clause construction provenance

Independent review accepted this refinement before its production changes.
The second implementation review found HAVING conjunctions rebuilt outside
syntax binding losing their property. Nine executed native/Modern controls
confirm the reported three-term raw-query case, right-associated terms and
transferred-term variants. They also disprove blindly OR-ing resolved children
for every retained HAVING parent: a query introduced through a left-associated
result alias must still raise the native error.

The completed source map is `select.c:7082-7142`, `walker.c:54-99`,
`expr.c:994-1006,1015-1054,1150-1177`, and the alias replacement map above.
Native walks only top-level AND structure in left-to-right order, moves an
eligible leaf into a newly constructed WHERE AND, and replaces that HAVING
occurrence with integer1. It does not flatten/reassociate retained HAVING or
recompute its existing parents' construction properties after alias resolution.

Replace the existing flatten/delete/rebuild transfer with one immutable
translation of that same walk, not an additional walker or arena. Keep the
current eligibility and outer-aggregate restrictions. Reuse unchanged nodes;
replace each moved occurrence with the canonical integer1; clone only changed
HAVING AND/alias paths, retaining their original properties and alias context.
Keep the original grouping and the HAVING predicate even when every moved leaf
has become true. Newly appended WHERE conjunctions derive properties from
their actual children at that post-resolution construction point.
This supersedes only ADR-0060 section8's flatten/rebuild/no-remaining-HAVING
disposition, not its eligibility, order, invocation or leaf-ownership rules.

Audit every current expression publisher and make its syntax-versus-explicit
metadata origin mandatory at the canonical publication API. Shared unary,
binary and comparison constructors require their own query-property input;
callers must choose original syntax, retained expression or newly constructed
children as the native creation phase requires. Generated scalar leaves,
joined values and clause nodes may not silently use an inactive syntax flag.
Keep one publication implementation and the existing properties/annotation,
not parallel binding algorithms or an origin registry.

Reuse the former traversal scratch as one bounded DFS work stack of original
IDs and child results. A measured recursive typed-result prototype overflowed
the default MSVC stack during binding at the original1000-level HAVING boundary;
the same-depth WHERE control completed every compiler phase, and pinned native
SQLite accepted the HAVING input. Explicit frames preserve the same left-first
walk, ordering and O(depth) storage without a second container, increased stack
or reduced input limit.

Record a public grouped-HAVING runtime red before implementation. Test retained
and transferred leaves, both associations, aliases that must retain errors,
all-transferred true HAVING, outer ownership and original maximum depth.
Update directly affected bound-tree expectations rather than suppressing
errors or retaining a second compatibility rewrite. Repeat complete owning
failure sweeps, fast units and native vectors; no stack or ceiling increase
is permitted.

#### Accepted default-stack conditional-emission refinement

Independent design review accepted this corrective emission change before implementation.
After the bounded HAVING rewrite, the exact1000-level probe completes binding,
logical planning and optimization, but overflows the default MSVC stack during
lowering. This is a separate observed failure, not a reason to remove the
retained HAVING tree or change the limit. The current recursive conditional
emitter now receives a native-valid deep tree that the old deletion avoided.

Keep ADR-0104's one conditional algorithm, native operand order, labels,
three-valued NULL policy and representative-row selection. Replace its compiler
call stack with one lazy bounded LIFO of expression/label continuations.
Tail-selected operands and transparent wrappers retarget the current request
without pushing. For a direct two-operand branch, defer the second operand and
process the first; for the local fallthrough form, defer label binding and the
second operand in reverse order. Thus bytecode generation order and label
ownership remain exactly those of the native recursive algorithm.

Ordinary leaf predicates allocate no continuation storage. This is compiler
scratch, not a register container, query cache, VM flag, runtime evaluator,
second emitter or new instruction. Keep all existing error propagation and
allocation/OOM ceilings, and test the original boundary with both transferable
constant terms and retained aggregate terms. Do not rely on tail-call
optimization, a larger executable stack or a lower admitted depth.

### 2. Translate query-containing logical value control flow

After the flag prerequisite is independently accepted and committed, extend
the existing logical value producer with `exprCodeTargetAndOr`'s missing
control flow. Reuse the published property, existing expression homes, typed
labels/jumps, canonical SQL truth conversion, `BinaryInstruction`, query
frames and call/cache state. Add no evaluator, instruction, runtime flag,
register container, query cache or read/write-specific path.

Keep the existing truth-hint selection before this value path. Then:

1. If only the left operand has the native query flag, generate the right
   operand first; otherwise retain left-first generation.
2. Skip the other operand only when that operand has the native query flag
   and the first value determines OR-true or AND-false. If both operands have
   the flag, left-first generation may skip the right query.
3. NULL alone never takes this skip edge.
4. On ordinary fallthrough, evaluate the other operand and use the existing
   logical binary operation.
5. On the skip edge, coerce the already computed value to SQLite's integer
   boolean using the same existing logical operation on that value twice.

Reuse one boolean-coercion emission for this edge and existing selected-operand
value emission. Compile the retained operand regions even when a runtime branch
will skip one; do not suppress required preparation errors or scalar width.
Skipped query calls must neither open their cursors nor mark their result cache
ready. Reset/rebind and statement moves retain the existing query lifecycle.

Query-free values remain eager, including collated queries whose native flag
was intentionally cleared. Do not turn scalar AND/OR into universally lazy
operators or reuse predicate short-circuiting as a value evaluator.
Keep arithmetic/comparison value generation and their separate preparation
boundaries unchanged.

Do not inflate the ordinary arithmetic emitter's recursive frame merely to
carry query-only labels. Keep query-only control-flow scratch at its existing
typed emission boundary; verify native default-stack/depth behavior and both
maximum expression/query cases rather than increasing stack or limits.

#### Accepted query-value depth refinement

Independent design review accepted this corrective continuation change before implementation.
Pinned native execution accepts a scalar query followed by997 parameter OR
terms and rejects998 under the original cumulative1000-level expression limit.
The first direct query-value emitter passes the focused semantic red/green
but overflows the default MSVC stack at that accepted997-term case. A matched
phase probe completes parsing, binding, logical planning and optimization,
then fails in lowering with `0xc00000fd`.

Retain the same `exprCodeTargetAndOr` generation algorithm with one lazy
compiler continuation stack for logical value expressions. Each continuation
holds the original expression/destination, optional local labels and whether
its first operand has completed. Evaluate the first operand, emit its native
determining-value jump, evaluate the second, emit the binary operation and
local skip/coercion/completion labels in exactly the recursive order.
Selected-operand continuations retain their existing boolean coercion.

Use the existing canonical expression selector before expanding a child
inline so grouped representative overrides remain authoritative. Other value
payloads delegate to their existing emitter. This replaces query-logical C++
recursion only; it is compiler control scratch, not a second evaluator,
register container, query cache, runtime state or instruction.
Ordinary arithmetic and query-free logical expressions keep their existing
path. Verify both997 acceptance/998 rejection and runtime reset/rebind under
the original limits and allocation ceilings.

### 3. Preserve the separate remaining gates

This does not admit IN query RHS or fix later literal value materialization,
complete-name-before-value diagnostics, required negated hexadecimal minimum,
or scalar-width checks for a statically discarded logical query. No naive early
unary guard is permitted.

The research also confirms distinct current shallow truth-selection and outer
ON-property differences. Native recursive `sqlite3ExprSimplifiedAndOr` and
`ExprAlwaysTrue/False`'s `EP_OuterON` exclusion require their complete alias,
query-boundary and join-strength-reduction provenance. Keep those as explicit
blocking follow-ups; do not add a blanket ON suppression or duplicate shallow
and recursive simplifiers as a compatibility shim in this batch.

## Ordered TDD, review and delivery

Use two small buildable commits in one related prerequisite PR:

1. Construction metadata and conditional consumers: first record the public
   collated-predicate runtime red, then implement the canonical flag. Cover
   direct/wrapped aliases, comparison roots, function modifiers, normalized
   constants, generated joins, eager VALUES and source-owner/copy lifetimes.
2. Logical value control flow: first record the public query-OR runtime red,
   then implement native generation. Cover AND/OR, both query positions,
   both-query left-first order, NULL, nonboolean coercion, COLLATE, parameters,
   required compile errors and shared INSERT/UPDATE/DELETE consumers.

Each commit requires focused green, the complete fast unit tier, complete owning
allocation/OOM sweeps under the original caps, independent implementation review
and node/ADR ledger evidence before the next commit begins. Use the existing
custom callbacks to match native fixed-work order/counts, not a new production
instrumentation mechanism. Preserve the original maximum-depth,64-source
program-size, native model and ordinary allocation controls.

Extend the existing bounded subquery compatibility shard with genuine pinned
GNU oracle regeneration, retaining historical outcomes, compiler provenance,
canonical LF bytes and the512-case cap. Publish the draft after the first
accepted commit. Review the final integrated tree and run the actual combined
change-aware native/compiler/sanitizer/full-quality/four original fuzz gates.
Changing an earlier commit invalidates dependent evidence until requalified.

Use guarded squash only after exact-head acceptance, prove the resulting tree
and sole parent, and verify actual resulting-main execution. No deadline,
workload, allocation ceiling, assertion, stack, sanitizer or agent protection
is relaxed. Node43/project completion is not claimed.

## Recorded first-slice evidence

Independent design review accepted this contract before production changes.
The public collated-predicate regression first returned a row instead of the
native runtime error and now retains that error. Fifteen focused cases and all
2075 native isolated unit cases pass, including the original depth/ownership
controls, alias/modifier distinctions, generated joined values, early VALUES,
callback order, statement moves/reset and shared mutation statement rollback.
The binder's complete OOM population grows44 to51 under the unchanged1024 cap;
five additional lowering cases retain their existing allocation ceilings.

Independent implementation review found the generated singleton-IN unary
inheriting its enclosing membership's property. Actual native controls and
corrective public/binder reds confirmed the error. The canonical unary
constructor now receives its own construction property explicitly; generated
constants and normalized scalar-limit comparisons use the same own-input rule.
IN/NOT IN NULL, nullable rebind and non-NULL required-query controls pass without
a special runtime membership path.

A second source review exposed HAVING reconstruction losing original
properties. Independent refinement reviews accepted immutable native tree
preservation and bounded compiler continuations before those changes.
The public runtime red and nine native alias/association/transfer controls now
agree. Actual1000-level constant and retained-count HAVING cases pass with the
default MSVC stack after separately diagnosed binding/lowering overflows;
the original limits and failure reports remain intact.

Actual MSVC measurement retains properties16, expression112, payload80, token24
and lowering-error72 bytes. Every one of the eight changed actual C++ TUs
compiles with GNU13.3 `-O3 -DNDEBUG` and all original strict warnings. Both
changed production objects pass current native clang-tidy.

Thirty-six genuine pinned GNU vectors extend the current309-case shard to345,
with343 comparisons and the two unchanged unsupported controls. All309
historical outcomes, the intermediate326/336-case prefixes and native compiler
provenance remain unchanged; canonical
LF bytes, the staged corpus and oracle hash agree. These observations do not
replace implementation review, the ordered second slice or final integrated
exact-head delivery gates.

## Recorded second-slice evidence

The first slice is sealed as accepted commit `1c96e1ab`, tree
`ca8d7d541cba0ebfe96fdda0069d3d00a01a1fbf`, before the second slice begins.
Its exact source review and per-commit ledger remain independent from this
successor's qualification.

The public query-OR regression first raised integer overflow instead of native1
and now passes. Six focused logical-value cases and all2081 native isolated
unit cases pass. Coverage includes both query positions, both-query left-first
order, three-valued NULL behavior, eager query-free/COLLATE controls, retained
name/width/literal prepare errors, fixed-work callbacks, retired source input,
statement moves/reset/rebind and shared mutation statement rollback.
Four additional lowering owner cases retain the original256 allocation cap;
the first slice's complete51-scenario binder population still passes1024.

The direct emitter's actual997-term default-stack failure was isolated to
lowering before its accepted continuation refinement. Pinned native and the
public lifecycle test now agree on997 acceptance,998 rejection and both
determining/non-determining parameter values under the cumulative1000 limit.
The same selector and expression homes remain authoritative; ordinary
arithmetic frames acquire no query-only labels.

Twenty-two genuine pinned GNU vectors extend345 to367 cases, with365
comparisons and both unchanged unsupported controls. Prefix native outcomes and
compiler provenance remain unchanged. Current strict GNU Release obligations,
native changed-object analysis and pinned formatting pass. These per-commit
observations do not replace independent implementation review or the final
exact-head integrated batch gates.
